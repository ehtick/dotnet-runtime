// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
//                                    Early Value Propagation
//
// This phase performs an SSA-based value propagation optimization that currently only applies to array
// lengths and explicit null checks. An SSA-based backwards tracking of local variables
// is performed at each point of interest, e.g., an array length reference site, a method table reference site, or
// an indirection.
// The tracking continues until an interesting value is encountered. The value is then used to rewrite
// the source site or the value.
//
///////////////////////////////////////////////////////////////////////////////////////

#include "jitpch.h"

bool Compiler::optDoEarlyPropForFunc()
{
    // TODO-MDArray: bool propMDArrayLen = (optMethodFlags & OMF_HAS_MDNEWARRAY) && (optMethodFlags &
    // OMF_HAS_MDARRAYREF);
    bool propArrayLen  = (optMethodFlags & OMF_HAS_ARRAYREF) != 0;
    bool propNullCheck = (optMethodFlags & OMF_HAS_NULLCHECK) != 0;
    return propArrayLen || propNullCheck;
}

//------------------------------------------------------------------------------------------
// optEarlyProp: The entry point of the early value propagation.
//
// Returns:
//    suitable phase status
//
// Notes:
//    This phase performs an SSA-based value propagation, including null check folding and
//    constant folding for GT_BOUNDS_CHECK nodes.
//
//    Null check folding tries to find GT_INDIR(obj + const) that GT_NULLCHECK(obj) can be folded into
//    and removed. Currently, the algorithm only matches GT_INDIR and GT_NULLCHECK in the same basic block.
//
//    TODO: support GT_MDARR_LENGTH, GT_MDARRAY_LOWER_BOUND
//
PhaseStatus Compiler::optEarlyProp()
{
    if (!optDoEarlyPropForFunc())
    {
        // We perhaps should verify the OMF are set properly
        //
        JITDUMP("no arrays or null checks in the method\n");
        return PhaseStatus::MODIFIED_NOTHING;
    }

    assert(fgSsaPassesCompleted == 1);
    unsigned numChanges = 0;

    for (BasicBlock* const block : Blocks())
    {
        compCurBB = block;

        CompAllocator                 allocator(getAllocator(CMK_EarlyProp));
        LocalNumberToNullCheckTreeMap nullCheckMap(allocator);

        for (Statement* stmt = block->firstStmt(); stmt != nullptr;)
        {
            // Preserve the next link before the propagation and morph.
            Statement* next = stmt->GetNextStmt();

            if (stmt->GetRootNode() == nullptr || ((stmt->GetRootNode()->gtFlags & GTF_ALL_EFFECT) == 0))
            {
                stmt = next;
                continue;
            }

            compCurStmt = stmt;

            // Walk the stmt tree in linear order to rewrite any array length reference with a
            // constant array length.
            bool isRewritten = false;
            for (GenTree* tree = stmt->GetTreeList(); tree != nullptr; tree = tree->gtNext)
            {
                GenTree* rewrittenTree = optEarlyPropRewriteTree(tree, &nullCheckMap);
                if (rewrittenTree != nullptr)
                {
                    gtUpdateSideEffects(stmt, rewrittenTree);
                    isRewritten = true;
                    tree        = rewrittenTree;
                }
            }

            // Update the evaluation order and the statement info if the stmt has been rewritten.
            if (isRewritten)
            {
                // Make sure the transformation happens in debug, check, and release build.
                gtSetStmtInfo(stmt);
                fgSetStmtSeq(stmt);
                numChanges++;
            }

            stmt = next;
        }
    }

    JITDUMP("\nOptimized %u trees\n", numChanges);
    return numChanges > 0 ? PhaseStatus::MODIFIED_EVERYTHING : PhaseStatus::MODIFIED_NOTHING;
}

//----------------------------------------------------------------
// optEarlyPropRewriteValue: Rewrite a tree to the actual value.
//
// Arguments:
//    tree           - The input tree node to be rewritten.
//    nullCheckMap   - Map of the local numbers to the latest NULLCHECKs on those locals in the current basic block.
//
// Return Value:
//    Return a new tree if the original tree was successfully rewritten.
//    The containing tree links are updated.
//
GenTree* Compiler::optEarlyPropRewriteTree(GenTree* tree, LocalNumberToNullCheckTreeMap* nullCheckMap)
{
    GenTree*    objectRefPtr = nullptr;
    optPropKind propKind     = optPropKind::OPK_INVALID;
    bool        folded       = false;

    if (tree->OperIsIndirOrArrMetaData())
    {
        // optFoldNullCheck takes care of updating statement info if a null check is removed.
        folded = optFoldNullCheck(tree, nullCheckMap);
    }
    else
    {
        return nullptr;
    }

    if (tree->OperIs(GT_ARR_LENGTH))
    {
        objectRefPtr = tree->AsOp()->gtOp1;
        propKind     = optPropKind::OPK_ARRAYLEN;
    }
    else
    {
        return folded ? tree : nullptr;
    }

    if (!objectRefPtr->OperIsScalarLocal() || !lvaInSsa(objectRefPtr->AsLclVarCommon()->GetLclNum()))
    {
        return folded ? tree : nullptr;
    }

    unsigned lclNum    = objectRefPtr->AsLclVarCommon()->GetLclNum();
    unsigned ssaNum    = objectRefPtr->AsLclVarCommon()->GetSsaNum();
    GenTree* actualVal = optPropGetValue(lclNum, ssaNum, propKind);

    if (actualVal != nullptr)
    {
        assert(propKind == optPropKind::OPK_ARRAYLEN);
        assert(actualVal->IsCnsIntOrI() && !actualVal->IsIconHandle());
        assert(actualVal->GetNodeSize() == TREE_NODE_SZ_SMALL);

        ssize_t actualConstVal = actualVal->AsIntCon()->IconValue();

        if (propKind == optPropKind::OPK_ARRAYLEN)
        {
            if ((actualConstVal < 0) || (actualConstVal > CORINFO_Array_MaxLength))
            {
                // Don't propagate array lengths that are beyond the maximum value of a GT_ARR_LENGTH or negative.
                // node. CORINFO_HELP_NEWARR_1_PTR helper call allows to take a long integer as the
                // array length argument, but the type of GT_ARR_LENGTH is always INT32.
                return nullptr;
            }

            // When replacing GT_ARR_LENGTH nodes with constants we can end up with GT_BOUNDS_CHECK
            // nodes that have constant operands and thus can be trivially proved to be useless. It's
            // better to remove these range checks here, otherwise they'll pass through assertion prop
            // (creating useless (c1 < c2)-like assertions) and reach RangeCheck where they are finally
            // removed. Common patterns like new int[] { x, y, z } benefit from this.

            if ((tree->gtNext != nullptr) && tree->gtNext->OperIs(GT_BOUNDS_CHECK))
            {
                GenTreeBoundsChk* check = tree->gtNext->AsBoundsChk();

                if ((check->GetArrayLength() == tree) && check->GetIndex()->IsCnsIntOrI())
                {
                    ssize_t checkConstVal = check->GetIndex()->AsIntCon()->IconValue();
                    if ((checkConstVal >= 0) && (checkConstVal < actualConstVal))
                    {
                        GenTree* comma = check->gtGetParent(nullptr);

                        // We should never see cases other than these in the IR,
                        // as the check node does not produce a value.
                        assert(((comma != nullptr) && comma->OperIs(GT_COMMA) &&
                                (comma->gtGetOp1() == check || comma->TypeIs(TYP_VOID))) ||
                               (check == compCurStmt->GetRootNode()));

                        // Still, we guard here so that release builds do not try to optimize trees we don't understand.
                        if (((comma != nullptr) && comma->OperIs(GT_COMMA) && (comma->gtGetOp1() == check)) ||
                            (check == compCurStmt->GetRootNode()))
                        {
                            // Both `tree` and `check` have been removed from the statement.
                            // 'tree' was replaced with 'nop' or side effect list under 'comma'.
                            // optRemoveRangeCheck returns this modified tree.
                            return optRemoveRangeCheck(check, comma, compCurStmt);
                        }
                    }
                }
            }
        }

        JITDUMP("optEarlyProp Rewriting " FMT_BB "\n", compCurBB->bbNum);
        DISPSTMT(compCurStmt);
        JITDUMP("\n");
    }

    return folded ? tree : nullptr;
}

//-------------------------------------------------------------------------------------------
// optPropGetValue: Given an SSA object ref pointer, get the value needed based on valueKind.
//
// Arguments:
//    lclNum         - The local var number of the ref pointer.
//    ssaNum         - The SSA var number of the ref pointer.
//    valueKind      - The kind of value of interest.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValue(unsigned lclNum, unsigned ssaNum, optPropKind valueKind)
{
    return optPropGetValueRec(lclNum, ssaNum, valueKind, 0);
}

//-----------------------------------------------------------------------------------
// optPropGetValueRec: Given an SSA object ref pointer, get the value needed based on valueKind
//                     within a recursion bound.
//
// Arguments:
//    lclNum         - The local var number of the array pointer.
//    ssaNum         - The SSA var number of the array pointer.
//    valueKind      - The kind of value of interest.
//    walkDepth      - Current recursive walking depth.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValueRec(unsigned lclNum, unsigned ssaNum, optPropKind valueKind, int walkDepth)
{
    if (ssaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return nullptr;
    }

    GenTree* value = nullptr;

    // Bound the recursion with a hard limit.
    if (walkDepth > optEarlyPropRecurBound)
    {
        return nullptr;
    }

    // Track along the use-def chain to get the array length
    LclSsaVarDsc*        ssaVarDsc   = lvaTable[lclNum].GetPerSsaData(ssaNum);
    GenTreeLclVarCommon* ssaDefStore = ssaVarDsc->GetDefNode();

    // Incoming parameters or live-in variables don't have actual definition tree node for
    // their FIRST_SSA_NUM. Definitions induced by calls do not record the store node. See
    // SsaBuilder::RenameDef.
    if (ssaDefStore != nullptr)
    {
        assert(ssaDefStore->OperIsLocalStore());

        GenTree* defValue = ssaDefStore->Data();

        // Recursively track the value for "entire" stores.
        if (ssaDefStore->OperIs(GT_STORE_LCL_VAR) && (ssaDefStore->GetLclNum() == lclNum) &&
            defValue->OperIs(GT_LCL_VAR))
        {
            unsigned defValueLclNum = defValue->AsLclVar()->GetLclNum();
            unsigned defValueSsaNum = defValue->AsLclVar()->GetSsaNum();

            value = optPropGetValueRec(defValueLclNum, defValueSsaNum, valueKind, walkDepth + 1);
        }
        else
        {
            if (valueKind == optPropKind::OPK_ARRAYLEN)
            {
                value = getArrayLengthFromAllocation(defValue DEBUGARG(ssaVarDsc->GetBlock()));
                if (value != nullptr)
                {
                    if (!value->IsCnsIntOrI())
                    {
                        // Leave out non-constant-sized array
                        value = nullptr;
                    }
                }
            }
        }
    }

    return value;
}

//----------------------------------------------------------------
// optFoldNullChecks: Try to find a GT_NULLCHECK node that can be folded into the indirection node mark it for removal
// if possible.
//
// Arguments:
//    tree           - The input indirection tree.
//    nullCheckMap   - Map of the local numbers to the latest NULLCHECKs on those locals in the current basic block
//
// Returns:
//    true if a null check was folded
//
// Notes:
//    If a GT_NULLCHECK node is post-dominated by an indirection node on the same local and the trees between
//    the GT_NULLCHECK and the indirection don't have unsafe side effects, the GT_NULLCHECK can be removed.
//    The indir will cause a NullReferenceException if and only if GT_NULLCHECK will cause the same
//    NullReferenceException.

bool Compiler::optFoldNullCheck(GenTree* tree, LocalNumberToNullCheckTreeMap* nullCheckMap)
{
    GenTree*   nullCheckTree   = optFindNullCheckToFold(tree, nullCheckMap);
    GenTree*   nullCheckParent = nullptr;
    Statement* nullCheckStmt   = nullptr;
    bool       folded          = false;
    if ((nullCheckTree != nullptr) && optIsNullCheckFoldingLegal(tree, nullCheckTree, &nullCheckParent, &nullCheckStmt))
    {
        // Make sure the transformation happens in debug, check, and release build.
        JITDUMP("optEarlyProp Marking a null check for removal\n");
        DISPTREE(nullCheckTree);
        JITDUMP("\n");

        // Remove the null check
        nullCheckTree->gtFlags &= ~(GTF_EXCEPT | GTF_DONT_CSE);

        // Set this flag to prevent reordering
        nullCheckTree->SetHasOrderingSideEffect();
        nullCheckTree->gtFlags |= GTF_IND_NONFAULTING;

        // The current indir is no longer non-faulting.
        tree->gtFlags &= ~GTF_IND_NONFAULTING;

        if (nullCheckParent != nullptr)
        {
            nullCheckParent->gtFlags &= ~GTF_DONT_CSE;
        }

        nullCheckMap->Remove(nullCheckTree->gtGetOp1()->AsLclVarCommon()->GetLclNum());

        // Re-morph the statement.
        Statement* curStmt = compCurStmt;
        fgMorphBlockStmt(compCurBB, nullCheckStmt DEBUGARG("optFoldNullCheck"), /* allowFGChange */ false);
        optRecordSsaUses(nullCheckStmt->GetRootNode(), compCurBB);
        compCurStmt = curStmt;

        folded = true;
    }

    if (tree->OperIs(GT_NULLCHECK) && (tree->gtGetOp1()->OperIs(GT_LCL_VAR)))
    {
        nullCheckMap->Set(tree->gtGetOp1()->AsLclVarCommon()->GetLclNum(), tree,
                          LocalNumberToNullCheckTreeMap::SetKind::Overwrite);
    }

    return folded;
}

//----------------------------------------------------------------
// optFindNullCheckToFold: Try to find a GT_NULLCHECK node that can be folded into the indirection node.
//
// Arguments:
//    tree           - The input indirection tree.
//    nullCheckMap   - Map of the local numbers to the latest NULLCHECKs on those locals in the current basic block
//
// Notes:
//    Check for cases where
//    1. One of the following trees
//
//       nullcheck(x)
//       or
//       x = comma(nullcheck(y), add(y, const1))
//
//       is post-dominated in the same basic block by one of the following trees
//
//       indir(x)
//       or
//       indir(add(x, const2))
//
//       (indir is any node for which OperIsIndirOrArrMetaData() is true.)
//
//     2.  const1 + const2 if sufficiently small.

GenTree* Compiler::optFindNullCheckToFold(GenTree* tree, LocalNumberToNullCheckTreeMap* nullCheckMap)
{
    assert(tree->OperIsIndirOrArrMetaData());

    GenTree* addr = tree->GetIndirOrArrMetaDataAddr()->gtEffectiveVal();

    ssize_t offsetValue = 0;

    if (addr->OperIs(GT_ADD) && addr->gtGetOp2()->IsCnsIntOrI())
    {
        offsetValue += addr->gtGetOp2()->AsIntConCommon()->IconValue();
        addr = addr->gtGetOp1();
    }

    if (!addr->OperIs(GT_LCL_VAR))
    {
        return nullptr;
    }

    GenTreeLclVarCommon* const lclVarNode = addr->AsLclVarCommon();
    const unsigned             ssaNum     = lclVarNode->GetSsaNum();

    if (ssaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return nullptr;
    }

    const unsigned lclNum        = lclVarNode->GetLclNum();
    GenTree*       nullCheckTree = nullptr;

    // Check if we saw a nullcheck on this local in this basic block
    // This corresponds to nullcheck(x) tree in the header comment.
    if (nullCheckMap->Lookup(lclNum, &nullCheckTree))
    {
        GenTree* nullCheckAddr = nullCheckTree->AsIndir()->Addr();
        if (!nullCheckAddr->OperIs(GT_LCL_VAR) || (nullCheckAddr->AsLclVarCommon()->GetSsaNum() != ssaNum))
        {
            nullCheckTree = nullptr;
        }
    }

    if (nullCheckTree == nullptr)
    {
        // Check if we have x = comma(nullcheck(y), add(y, const1)) pattern.

        // Find the definition of the indirected local ('x' in the pattern above).
        LclSsaVarDsc* defLoc = lvaTable[lclNum].GetPerSsaData(ssaNum);

        if (compCurBB != defLoc->GetBlock())
        {
            return nullptr;
        }

        GenTreeLclVarCommon* defNode = defLoc->GetDefNode();
        if ((defNode == nullptr) || !defNode->OperIs(GT_STORE_LCL_VAR) || (defNode->GetLclNum() != lclNum))
        {
            return nullptr;
        }

        GenTree* defValue = defNode->Data();
        if (!defValue->OperIs(GT_COMMA))
        {
            return nullptr;
        }

        GenTree* commaOp1EffectiveValue = defValue->gtGetOp1()->gtEffectiveVal();

        if (!commaOp1EffectiveValue->OperIs(GT_NULLCHECK))
        {
            return nullptr;
        }

        GenTree* nullCheckAddress = commaOp1EffectiveValue->gtGetOp1();

        if (!nullCheckAddress->OperIs(GT_LCL_VAR) || (defValue->gtGetOp2()->OperGet() != GT_ADD))
        {
            return nullptr;
        }

        // We found a candidate for 'y' in the pattern above.

        GenTree* additionNode = defValue->gtGetOp2();
        GenTree* additionOp1  = additionNode->gtGetOp1();
        GenTree* additionOp2  = additionNode->gtGetOp2();
        if (additionOp1->OperIs(GT_LCL_VAR) &&
            (additionOp1->AsLclVarCommon()->GetLclNum() == nullCheckAddress->AsLclVarCommon()->GetLclNum()) &&
            (additionOp2->IsCnsIntOrI()))
        {
            offsetValue += additionOp2->AsIntConCommon()->IconValue();
            nullCheckTree = commaOp1EffectiveValue;
        }
    }

    if (fgIsBigOffset(offsetValue))
    {
        return nullptr;
    }
    else
    {
        return nullCheckTree;
    }
}

//----------------------------------------------------------------
// optIsNullCheckFoldingLegal: Check the nodes between the GT_NULLCHECK node and the indirection to determine
//                             if null check folding is legal.
//
// Arguments:
//    tree                - The input indirection tree.
//    nullCheckTree       - The GT_NULLCHECK tree that is a candidate for removal.
//    nullCheckParent     - The parent of the GT_NULLCHECK tree that is a candidate for removal (out-parameter).
//    nullCheckStatement  - The statement of the GT_NULLCHECK tree that is a candidate for removal (out-parameter).

bool Compiler::optIsNullCheckFoldingLegal(GenTree*    tree,
                                          GenTree*    nullCheckTree,
                                          GenTree**   nullCheckParent,
                                          Statement** nullCheckStmt)
{
    // Check all nodes between the GT_NULLCHECK and the indirection to see
    // if any nodes have unsafe side effects.
    bool           isInsideTry        = compCurBB->hasTryIndex();
    bool           canRemoveNullCheck = true;
    const unsigned maxNodesWalked     = 50;
    unsigned       nodesWalked        = 0;

    // First walk the nodes in the statement containing the GT_NULLCHECK in forward execution order
    // until we get to the indirection or process the statement root.
    GenTree* previousTree = nullCheckTree;
    GenTree* currentTree  = nullCheckTree->gtNext;
    assert(fgNodeThreading == NodeThreading::AllTrees);
    while (canRemoveNullCheck && (currentTree != tree) && (currentTree != nullptr))
    {
        if ((*nullCheckParent == nullptr) && currentTree->TryGetUse(nullCheckTree))
        {
            *nullCheckParent = currentTree;
        }
        const bool checkExceptionSummary = false;
        if ((nodesWalked++ > maxNodesWalked) ||
            !optCanMoveNullCheckPastTree(currentTree, isInsideTry, checkExceptionSummary))
        {
            canRemoveNullCheck = false;
        }
        else
        {
            previousTree = currentTree;
            currentTree  = currentTree->gtNext;
        }
    }

    if (currentTree == tree)
    {
        // The GT_NULLCHECK and the indirection are in the same statements.
        *nullCheckStmt = compCurStmt;
    }
    else
    {
        // The GT_NULLCHECK and the indirection are in different statements.
        // Walk the nodes in the statement containing the indirection
        // in reverse execution order starting with the indirection's
        // predecessor.
        GenTree* nullCheckStatementRoot = previousTree;
        currentTree                     = tree->gtPrev;
        while (canRemoveNullCheck && (currentTree != nullptr))
        {
            const bool checkExceptionSummary = false;
            if ((nodesWalked++ > maxNodesWalked) ||
                !optCanMoveNullCheckPastTree(currentTree, isInsideTry, checkExceptionSummary))
            {
                canRemoveNullCheck = false;
            }
            else
            {
                currentTree = currentTree->gtPrev;
            }
        }

        // Finally, walk the statement list in reverse execution order
        // until we get to the statement containing the null check.
        // We only check the side effects at the root of each statement.
        Statement* curStmt = compCurStmt->GetPrevStmt();
        currentTree        = curStmt->GetRootNode();
        while (canRemoveNullCheck && (currentTree != nullCheckStatementRoot))
        {
            const bool checkExceptionSummary = true;
            if ((nodesWalked++ > maxNodesWalked) ||
                !optCanMoveNullCheckPastTree(currentTree, isInsideTry, checkExceptionSummary))
            {
                canRemoveNullCheck = false;
            }
            else
            {
                curStmt     = curStmt->GetPrevStmt();
                currentTree = curStmt->GetRootNode();
            }
        }
        *nullCheckStmt = curStmt;
    }

    if (canRemoveNullCheck && (*nullCheckParent == nullptr))
    {
        *nullCheckParent = nullCheckTree->gtGetParent(nullptr);
    }

    return canRemoveNullCheck;
}

//----------------------------------------------------------------
// optCanMoveNullCheckPastTree: Check if a nullcheck node that is before `tree`
//                              in execution order may be folded into an indirection node that
//                              is after `tree` is execution order.
//
// Arguments:
//    tree                  - The tree to check.
//    isInsideTry           - True if tree is inside try, false otherwise.
//    checkSideEffectSummary -If true, check side effect summary flags only,
//                            otherwise check the side effects of the operation itself.
//
// Return Value:
//    True if nullcheck may be folded into a node that is after tree in execution order,
//    false otherwise.

bool Compiler::optCanMoveNullCheckPastTree(GenTree* tree, bool isInsideTry, bool checkSideEffectSummary)
{
    bool result = true;

    if ((tree->gtFlags & GTF_CALL) != 0)
    {
        result = !checkSideEffectSummary && !tree->OperRequiresCallFlag(this);
    }

    if (result && (tree->gtFlags & GTF_EXCEPT) != 0)
    {
        result = !checkSideEffectSummary && !tree->OperMayThrow(this);
    }

    if (result && ((tree->gtFlags & GTF_ASG) != 0))
    {
        if (tree->OperIsStore())
        {
            if (checkSideEffectSummary && ((tree->Data()->gtFlags & GTF_ASG) != 0))
            {
                result = false;
            }
            else if (isInsideTry)
            {
                // Inside try we allow only stores to locals not live in handlers.
                result = tree->OperIs(GT_STORE_LCL_VAR) && !lvaTable[tree->AsLclVar()->GetLclNum()].lvLiveInOutOfHndlr;
            }
            else
            {
                // We disallow stores to global memory.
                result = tree->OperIsLocalStore() && !lvaGetDesc(tree->AsLclVarCommon())->IsAddressExposed();

                // TODO-ASG-Cleanup: delete this zero-diff quirk. Some setup args for by-ref args do not have GLOB_REF.
                if ((tree->gtFlags & GTF_GLOB_REF) == 0)
                {
                    result = true;
                }
            }
        }
        else if (checkSideEffectSummary)
        {
            result = !isInsideTry && ((tree->gtFlags & GTF_GLOB_REF) == 0);
        }
        else
        {
            result = !isInsideTry && (!tree->OperRequiresAsgFlag() || ((tree->gtFlags & GTF_GLOB_REF) == 0));
        }
    }

    return result;
}
