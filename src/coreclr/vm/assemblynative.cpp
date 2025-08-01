// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*============================================================
**
** Header:  AssemblyNative.cpp
**
** Purpose: Implements AssemblyNative (loader domain) architecture
**
**


**
===========================================================*/

#include "common.h"

#include <stdlib.h>
#include "assemblynative.hpp"
#include "dllimport.h"
#include "field.h"
#include "eeconfig.h"
#include "interoputil.h"
#include "frames.h"
#include "typeparse.h"
#include "encee.h"
#include "threadsuspend.h"
#include <caparser.h>

#include "appdomainnative.hpp"
#include "../binder/inc/bindertracing.h"
#include "../binder/inc/defaultassemblybinder.h"

extern "C" void QCALLTYPE AssemblyNative_InternalLoad(NativeAssemblyNameParts* pAssemblyNameParts,
                                                      QCall::ObjectHandleOnStack requestingAssembly,
                                                      QCall::StackCrawlMarkHandle stackMark,
                                                      BOOL fThrowOnFileNotFound,
                                                      QCall::ObjectHandleOnStack assemblyLoadContext,
                                                      QCall::ObjectHandleOnStack retAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    DomainAssembly * pParentAssembly = NULL;
    Assembly * pRefAssembly = NULL;
    AssemblyBinder *pBinder = NULL;

    {
        GCX_COOP();

        if (assemblyLoadContext.Get() != NULL)
        {
            INT_PTR nativeAssemblyBinder = ((ASSEMBLYLOADCONTEXTREF)assemblyLoadContext.Get())->GetNativeAssemblyBinder();
            pBinder = reinterpret_cast<AssemblyBinder*>(nativeAssemblyBinder);
        }

        // Compute parent assembly
        if (requestingAssembly.Get() != NULL)
        {
            pRefAssembly = ((ASSEMBLYREF)requestingAssembly.Get())->GetAssembly();
        }
        else if (pBinder == NULL)
        {
            pRefAssembly = SystemDomain::GetCallersAssembly(stackMark);
        }
    }

    AssemblySpec spec;

    if (pAssemblyNameParts->_pName == NULL)
        COMPlusThrow(kArgumentException, W("Format_StringZeroLength"));

    StackSString ssName;
    ssName.SetAndConvertToUTF8(pAssemblyNameParts->_pName);

    AssemblyMetaDataInternal asmInfo;

    asmInfo.usMajorVersion = pAssemblyNameParts->_major;
    asmInfo.usMinorVersion = pAssemblyNameParts->_minor;
    asmInfo.usBuildNumber = pAssemblyNameParts->_build;
    asmInfo.usRevisionNumber = pAssemblyNameParts->_revision;

    SmallStackSString ssLocale;
    if (pAssemblyNameParts->_pCultureName != NULL)
        ssLocale.SetAndConvertToUTF8(pAssemblyNameParts->_pCultureName);
    asmInfo.szLocale = (pAssemblyNameParts->_pCultureName != NULL) ? ssLocale.GetUTF8() : NULL;

    // Initialize spec
    spec.Init(ssName.GetUTF8(), &asmInfo,
        pAssemblyNameParts->_pPublicKeyOrToken, pAssemblyNameParts->_cbPublicKeyOrToken, pAssemblyNameParts->_flags);

    if (pRefAssembly != NULL)
        spec.SetParentAssembly(pRefAssembly);

    // Have we been passed the reference to the binder against which this load should be triggered?
    // If so, then use it to set the fallback load context binder.
    if (pBinder != NULL)
    {
        spec.SetFallbackBinderForRequestingAssembly(pBinder);
        spec.SetPreferFallbackBinder();
    }
    else if (pRefAssembly != NULL)
    {
        // If the requesting assembly has Fallback LoadContext binder available,
        // then set it up in the AssemblySpec.
        PEAssembly *pRefAssemblyManifestFile = pRefAssembly->GetPEAssembly();
        spec.SetFallbackBinderForRequestingAssembly(pRefAssemblyManifestFile->GetFallbackBinder());
    }

    Assembly *pAssembly = spec.LoadAssembly(FILE_LOADED, fThrowOnFileNotFound);

    if (pAssembly != NULL)
    {
        GCX_COOP();
        retAssembly.Set(pAssembly->GetExposedObject());
    }

    END_QCALL;
}

/* static */
Assembly* AssemblyNative::LoadFromPEImage(AssemblyBinder* pBinder, PEImage *pImage, bool excludeAppPaths)
{
    CONTRACT(Assembly*)
    {
        STANDARD_VM_CHECK;
        PRECONDITION(CheckPointer(pBinder));
        PRECONDITION(pImage != NULL);
        POSTCONDITION(CheckPointer(RETVAL));
    }
    CONTRACT_END;

    Assembly *pLoadedAssembly = NULL;
    ReleaseHolder<BINDER_SPACE::Assembly> pAssembly;

    // Set the caller's assembly to be CoreLib
    Assembly *pCallersAssembly = SystemDomain::System()->SystemAssembly();

    // Initialize the AssemblySpec
    AssemblySpec spec;
    spec.InitializeSpec(TokenFromRid(1, mdtAssembly), pImage->GetMDImport(), pCallersAssembly);
    spec.SetBinder(pBinder);

    BinderTracing::AssemblyBindOperation bindOperation(&spec, pImage->GetPath());

    HRESULT hr = S_OK;
    PTR_AppDomain pCurDomain = GetAppDomain();
    hr = pBinder->BindUsingPEImage(pImage, excludeAppPaths, &pAssembly);

    if (hr != S_OK)
    {
        StackSString name;
        spec.GetDisplayName(0, name);
        if (hr == COR_E_FILELOAD)
        {
            // Give a more specific message for the case when we found the assembly with the same name already loaded.
            // Show the assembly name, since we know the error is about the assembly name.
            StackSString errorString;
            errorString.LoadResource(CCompRC::Error, IDS_HOST_ASSEMBLY_RESOLVER_ASSEMBLY_ALREADY_LOADED_IN_CONTEXT);
            COMPlusThrow(kFileLoadException, IDS_EE_FILELOAD_ERROR_GENERIC, name, errorString);
        }
        else
        {
            // Propagate the actual HResult to the FileLoadException
            // Use the path if this load request was for a file path, display name otherwise
            EEFileLoadException::Throw(pImage->GetPath().IsEmpty() ? name : pImage->GetPath(), hr);
        }
    }

    PEAssemblyHolder pPEAssembly(PEAssembly::Open(pAssembly->GetPEImage(), pAssembly));
    bindOperation.SetResult(pPEAssembly.GetValue());

    RETURN pCurDomain->LoadAssembly(&spec, pPEAssembly, FILE_LOADED);
}

extern "C" void QCALLTYPE AssemblyNative_LoadFromPath(INT_PTR ptrNativeAssemblyBinder, LPCWSTR pwzILPath, LPCWSTR pwzNIPath, QCall::ObjectHandleOnStack retLoadedAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    PTR_AppDomain pCurDomain = GetAppDomain();

    // Get the binder context in which the assembly will be loaded.
    AssemblyBinder *pBinder = reinterpret_cast<AssemblyBinder*>(ptrNativeAssemblyBinder);
    _ASSERTE(pBinder != NULL);

    // Form the PEImage for the ILAssembly. In case of an exception, the holder will ensure
    // the release of the image.
    PEImageHolder pILImage;

    if (pwzILPath != NULL)
    {
        pILImage = PEImage::OpenImage(pwzILPath);

        // Need to verify that this is a valid CLR assembly.
        if (!pILImage->CheckILFormat())
            THROW_BAD_FORMAT(BFA_BAD_IL, pILImage.GetValue());

        LoaderAllocator* pLoaderAllocator = pBinder->GetLoaderAllocator();
        if (pLoaderAllocator && pLoaderAllocator->IsCollectible() && !pILImage->IsILOnly())
        {
            // Loading IJW assemblies into a collectible AssemblyLoadContext is not allowed
            THROW_BAD_FORMAT(BFA_IJW_IN_COLLECTIBLE_ALC, pILImage.GetValue());
        }
    }

    Assembly *pLoadedAssembly = AssemblyNative::LoadFromPEImage(pBinder, pILImage);

    {
        GCX_COOP();
        retLoadedAssembly.Set(pLoadedAssembly->GetExposedObject());
    }

    LOG((LF_CLASSLOADER,
            LL_INFO100,
            "\tLoaded assembly from a file\n"));

    END_QCALL;
}

/*static */
extern "C" void QCALLTYPE AssemblyNative_LoadFromStream(INT_PTR ptrNativeAssemblyBinder, INT_PTR ptrAssemblyArray,
                                              INT32 cbAssemblyArrayLength, INT_PTR ptrSymbolArray, INT32 cbSymbolArrayLength,
                                              QCall::ObjectHandleOnStack retLoadedAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    // Ensure that the invariants are in place
    _ASSERTE(ptrNativeAssemblyBinder != (INT_PTR)NULL);
    _ASSERTE((ptrAssemblyArray != (INT_PTR)NULL) && (cbAssemblyArrayLength > 0));
    _ASSERTE((ptrSymbolArray == (INT_PTR)NULL) || (cbSymbolArrayLength > 0));

    PEImageHolder pILImage(PEImage::CreateFromByteArray((BYTE*)ptrAssemblyArray, (COUNT_T)cbAssemblyArrayLength));

    // Need to verify that this is a valid CLR assembly.
    if (!pILImage->CheckILFormat())
        ThrowHR(COR_E_BADIMAGEFORMAT, BFA_BAD_IL);

    // Get the binder context in which the assembly will be loaded
    AssemblyBinder *pBinder = reinterpret_cast<AssemblyBinder*>(ptrNativeAssemblyBinder);

    LoaderAllocator* pLoaderAllocator = pBinder->GetLoaderAllocator();
    if (pLoaderAllocator && pLoaderAllocator->IsCollectible() && !pILImage->IsILOnly())
    {
        // Loading IJW assemblies into a collectible AssemblyLoadContext is not allowed
        ThrowHR(COR_E_BADIMAGEFORMAT, BFA_IJW_IN_COLLECTIBLE_ALC);
    }

    // Pass the stream based assembly as IL in an attempt to bind and load it
    Assembly* pLoadedAssembly = AssemblyNative::LoadFromPEImage(pBinder, pILImage);
    {
        GCX_COOP();
        retLoadedAssembly.Set(pLoadedAssembly->GetExposedObject());
    }

    LOG((LF_CLASSLOADER,
            LL_INFO100,
            "\tLoaded assembly from a file\n"));

    // In order to assign the PDB image (if present),
    // the resulting assembly's image needs to be exactly the one
    // we created above. We need pointer comparison instead of pe image equivalence
    // to avoid mixed binaries/PDB pairs of other images.
    // This applies to both Desktop CLR and CoreCLR, with or without fusion.
    BOOL fIsSameAssembly = (pLoadedAssembly->GetPEAssembly()->GetPEImage() == pILImage);

    // Setting the PDB info is only applicable for our original assembly.
    // This applies to both Desktop CLR and CoreCLR, with or without fusion.
    if (fIsSameAssembly)
    {
#ifdef DEBUGGING_SUPPORTED
        // If we were given symbols, save a copy of them.
        if (ptrSymbolArray != 0)
        {
            PBYTE pSymbolArray = reinterpret_cast<PBYTE>(ptrSymbolArray);
            pLoadedAssembly->GetModule()->SetSymbolBytes(pSymbolArray, (DWORD)cbSymbolArrayLength);
        }
#endif // DEBUGGING_SUPPORTED
    }

    END_QCALL;
}

#ifdef TARGET_WINDOWS
/*static */
extern "C" void QCALLTYPE AssemblyNative_LoadFromInMemoryModule(INT_PTR ptrNativeAssemblyBinder, INT_PTR hModule, QCall::ObjectHandleOnStack retLoadedAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    // Ensure that the invariants are in place
    _ASSERTE(ptrNativeAssemblyBinder != NULL);
    _ASSERTE(hModule != NULL);

    PEImageHolder pILImage(PEImage::CreateFromHMODULE((HMODULE)hModule));

    // Need to verify that this is a valid CLR assembly.
    if (!pILImage->HasCorHeader())
        ThrowHR(COR_E_BADIMAGEFORMAT, BFA_BAD_IL);

    // Get the binder context in which the assembly will be loaded
    AssemblyBinder *pBinder = reinterpret_cast<AssemblyBinder*>(ptrNativeAssemblyBinder);

    // Pass the in memory module as IL in an attempt to bind and load it
    Assembly* pLoadedAssembly = AssemblyNative::LoadFromPEImage(pBinder, pILImage);
    {
        GCX_COOP();
        retLoadedAssembly.Set(pLoadedAssembly->GetExposedObject());
    }

    LOG((LF_CLASSLOADER,
            LL_INFO100,
            "\tLoaded assembly from pre-loaded native module\n"));

    END_QCALL;
}
#endif

extern "C" void QCALLTYPE AssemblyNative_GetLocation(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    {
        retString.Set(pAssembly->GetPEAssembly()->GetPath());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetTypeCore(QCall::AssemblyHandle pAssembly,
    LPCSTR szTypeName,
    LPCSTR * rgszNestedTypeNames,
    int32_t cNestedTypeNamesLength,
    QCall::ObjectHandleOnStack retType)
{
    CONTRACTL
    {
        QCALL_CHECK;
        PRECONDITION(CheckPointer(szTypeName));
    }
    CONTRACTL_END;

    BEGIN_QCALL;

    TypeHandle th = TypeHandle();
    Module* pManifestModule = pAssembly->GetModule();
    ClassLoader* pClassLoader = pAssembly->GetLoader();

    NameHandle typeName(pManifestModule, mdtBaseType);
    CQuickBytes qbszNamespace;

    for (int32_t i = -1; i < cNestedTypeNamesLength; i++)
    {
        LPCUTF8 szFullyQualifiedName = (i == -1) ? szTypeName : rgszNestedTypeNames[i];

        LPCUTF8 szNameSpace = "";
        LPCUTF8 szName = "";

        if ((szName = ns::FindSep(szFullyQualifiedName)) != NULL)
        {
            SIZE_T d = szName - szFullyQualifiedName;
            szNameSpace = qbszNamespace.SetString(szFullyQualifiedName, d);
            szName++;
        }
        else
        {
            szName = szFullyQualifiedName;
        }

        typeName.SetName(szNameSpace, szName);

        // typeName.m_pBucket gets set here if the type is found
        // it will be used in the next iteration to look up the nested type
        th = pClassLoader->LoadTypeHandleThrowing(&typeName, CLASS_LOADED);

        // If we didn't find a type, don't bother looking for its nested type
        if (th.IsNull())
            break;

        if (th.GetAssembly() != pAssembly)
        {
            // For forwarded type, use the found assembly class loader for potential nested types search
            // The nested type has to be in the same module as the nesting type, so it doesn't make
            // sense to follow the same chain of type forwarders again for the nested type
            pClassLoader = th.GetAssembly()->GetLoader();
        }
    }

    if (!th.IsNull())
    {
        GCX_COOP();
        retType.Set(th.GetManagedClassObject());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetTypeCoreIgnoreCase(QCall::AssemblyHandle assemblyHandle,
    LPCWSTR wszTypeName,
    LPCWSTR* rgwszNestedTypeNames,
    int32_t cNestedTypeNamesLength,
    QCall::ObjectHandleOnStack retType)
{
    CONTRACTL
    {
        QCALL_CHECK;
        PRECONDITION(CheckPointer(wszTypeName));
    }
    CONTRACTL_END;

    BEGIN_QCALL;

    Assembly* pAssembly = assemblyHandle;

    TypeHandle th = TypeHandle();
    Module* pManifestModule = pAssembly->GetModule();
    ClassLoader* pClassLoader = pAssembly->GetLoader();

    NameHandle typeName(pManifestModule, mdtBaseType);
    CQuickBytes qbszNamespace;

    // Set up the name handle
    typeName.SetCaseInsensitive();

    for (int32_t i = -1; i < cNestedTypeNamesLength; i++)
    {
        // each extra name represents one more level of nesting
        StackSString name((i == -1) ? wszTypeName : rgwszNestedTypeNames[i]);

        // The type name is expected to be lower-cased by the caller for case-insensitive lookups
        name.LowerCase();

        LPCUTF8 szFullyQualifiedName = name.GetUTF8();

        LPCUTF8 szNameSpace = "";
        LPCUTF8 szName = "";

        if ((szName = ns::FindSep(szFullyQualifiedName)) != NULL)
        {
            SIZE_T d = szName - szFullyQualifiedName;
            szNameSpace = qbszNamespace.SetString(szFullyQualifiedName, d);
            szName++;
        }
        else
        {
            szName = szFullyQualifiedName;
        }

        typeName.SetName(szNameSpace, szName);

        // typeName.m_pBucket gets set here if the type is found
        // it will be used in the next iteration to look up the nested type
        th = pClassLoader->LoadTypeHandleThrowing(&typeName, CLASS_LOADED);

        // If we didn't find a type, don't bother looking for its nested type
        if (th.IsNull())
            break;

        if (th.GetAssembly() != pAssembly)
        {
            // For forwarded type, use the found assembly class loader for potential nested types search
            // The nested type has to be in the same module as the nesting type, so it doesn't make
            // sense to follow the same chain of type forwarders again for the nested type
            pClassLoader = th.GetAssembly()->GetLoader();
        }
    }

    if (!th.IsNull())
    {
         GCX_COOP();
         retType.Set(th.GetManagedClassObject());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetForwardedType(QCall::AssemblyHandle pAssembly, mdToken mdtExternalType, QCall::ObjectHandleOnStack retType)
{
    CONTRACTL
    {
        QCALL_CHECK;
    }
    CONTRACTL_END;

    BEGIN_QCALL;

    LPCSTR pszNameSpace;
    LPCSTR pszClassName;
    mdToken mdImpl;

    Module *pManifestModule = pAssembly->GetModule();
    IfFailThrow(pManifestModule->GetMDImport()->GetExportedTypeProps(mdtExternalType, &pszNameSpace, &pszClassName, &mdImpl, NULL, NULL));
    if (TypeFromToken(mdImpl) == mdtAssemblyRef)
    {
        NameHandle typeName(pszNameSpace, pszClassName);
        typeName.SetTypeToken(pManifestModule, mdtExternalType);
        TypeHandle typeHnd = pAssembly->GetLoader()->LoadTypeHandleThrowIfFailed(&typeName);
        {
            GCX_COOP();
            retType.Set(typeHnd.GetManagedClassObject());
        }
    }

    END_QCALL;
}

FCIMPL1(FC_BOOL_RET, AssemblyNative::GetIsDynamic, Assembly* pAssembly)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pAssembly));
    }
    CONTRACTL_END;

    FC_RETURN_BOOL(pAssembly->GetPEAssembly()->IsReflectionEmit());
}
FCIMPLEND

extern "C" void QCALLTYPE AssemblyNative_GetVersion(QCall::AssemblyHandle pAssembly, INT32* pMajorVersion, INT32* pMinorVersion, INT32*pBuildNumber, INT32* pRevisionNumber)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    UINT16 major=0xffff, minor=0xffff, build=0xffff, revision=0xffff;

    pAssembly->GetPEAssembly()->GetVersion(&major, &minor, &build, &revision);

    *pMajorVersion = major;
    *pMinorVersion = minor;
    *pBuildNumber = build;
    *pRevisionNumber = revision;

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetPublicKey(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retPublicKey)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    DWORD cbPublicKey = 0;
    const void *pbPublicKey = pAssembly->GetPEAssembly()->GetPublicKey(&cbPublicKey);
    retPublicKey.SetByteArray((BYTE *)pbPublicKey, cbPublicKey);

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetSimpleName(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retSimpleName)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;
    retSimpleName.Set(pAssembly->GetSimpleName());
    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetLocale(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    LPCUTF8 pLocale = pAssembly->GetPEAssembly()->GetLocale();
    if(pLocale)
    {
        retString.Set(pLocale);
    }

    END_QCALL;
}

extern "C" BOOL QCALLTYPE AssemblyNative_GetCodeBase(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BOOL ret = TRUE;

    BEGIN_QCALL;

    StackSString codebase;

    {
        ret = pAssembly->GetPEAssembly()->GetCodeBase(codebase);
    }

    retString.Set(codebase);
    END_QCALL;

    return ret;
}

extern "C" INT32 QCALLTYPE AssemblyNative_GetHashAlgorithm(QCall::AssemblyHandle pAssembly)
{
    QCALL_CONTRACT;

    INT32 retVal=0;
    BEGIN_QCALL;
    retVal = pAssembly->GetPEAssembly()->GetHashAlgId();
    END_QCALL;
    return retVal;
}

extern "C" INT32 QCALLTYPE AssemblyNative_GetFlags(QCall::AssemblyHandle pAssembly)
{
    QCALL_CONTRACT;

    INT32 retVal=0;
    BEGIN_QCALL;
    retVal = pAssembly->GetPEAssembly()->GetFlags();
    END_QCALL;
    return retVal;
}

extern "C" BYTE * QCALLTYPE AssemblyNative_GetResource(QCall::AssemblyHandle pAssembly, LPCWSTR wszName, DWORD * length)
{
    QCALL_CONTRACT;

    PBYTE       pbInMemoryResource  = NULL;

    BEGIN_QCALL;

    if (wszName == NULL)
        COMPlusThrow(kArgumentNullException, W("ArgumentNull_String"));

    // Get the name in UTF8
    StackSString name;
    name.SetAndConvertToUTF8(wszName);

    LPCUTF8 pNameUTF8 = name.GetUTF8();

    if (*pNameUTF8 == '\0')
        COMPlusThrow(kArgumentException, W("Format_StringZeroLength"));

    pAssembly->GetPEAssembly()->GetResource(pNameUTF8, length,
                           &pbInMemoryResource, NULL, NULL,
                           NULL, pAssembly);

    END_QCALL;

    // Can return null if resource file is zero-length
    return pbInMemoryResource;
}

extern "C" INT32 QCALLTYPE AssemblyNative_GetManifestResourceInfo(QCall::AssemblyHandle pAssembly, LPCWSTR wszName, QCall::ObjectHandleOnStack retAssembly, QCall::StringHandleOnStack retFileName)
{
    QCALL_CONTRACT;

    INT32 rv = -1;

    BEGIN_QCALL;

    if (wszName == NULL)
        COMPlusThrow(kArgumentNullException, W("ArgumentNull_String"));

    // Get the name in UTF8
    StackSString name;
    name.SetAndConvertToUTF8(wszName);
    LPCUTF8 pNameUTF8 = name.GetUTF8();

    if (*pNameUTF8 == '\0')
        COMPlusThrow(kArgumentException, W("Format_StringZeroLength"));

    Assembly * pReferencedAssembly = NULL;
    LPCSTR pFileName = NULL;
    DWORD dwLocation = 0;

    if (pAssembly->GetPEAssembly()->GetResource(pNameUTF8, NULL, NULL, &pReferencedAssembly, &pFileName,
                              &dwLocation, pAssembly))
    {
        if (pFileName)
            retFileName.Set(pFileName);

        GCX_COOP();

        if (pReferencedAssembly)
            retAssembly.Set(pReferencedAssembly->GetExposedObject());

        rv = dwLocation;
    }

    END_QCALL;

    return rv;
}

extern "C" void QCALLTYPE AssemblyNative_GetModules(QCall::AssemblyHandle pAssembly, BOOL fLoadIfNotFound, BOOL fGetResourceModules, QCall::ObjectHandleOnStack retModules)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    HENUMInternalHolder phEnum(pAssembly->GetMDImport());
    phEnum.EnumInit(mdtFile, mdTokenNil);

    InlineSArray<Module *, 8> modules;

    modules.Append(pAssembly->GetModule());

    mdFile mdFile;
    while (pAssembly->GetMDImport()->EnumNext(&phEnum, &mdFile))
    {
        if (fLoadIfNotFound)
        {
            Module* pModule = pAssembly->GetModule()->LoadModule(mdFile);
            modules.Append(pModule);
        }
    }

    {
        GCX_COOP();

        PTRARRAYREF orModules = NULL;

        GCPROTECT_BEGIN(orModules);

        // Return the modules
        orModules = (PTRARRAYREF)AllocateObjectArray(modules.GetCount(), CoreLibBinder::GetClass(CLASS__MODULE));

        for(COUNT_T i = 0; i < modules.GetCount(); i++)
        {
            Module * pModule = modules[i];

            OBJECTREF o = pModule->GetExposedObject();
            orModules->SetAt(i, o);
        }

        retModules.Set(orModules);

        GCPROTECT_END();
    }

    END_QCALL;
}

extern "C" BOOL QCALLTYPE AssemblyNative_GetIsCollectible(QCall::AssemblyHandle pAssembly)
{
    QCALL_CONTRACT;

    BOOL retVal = FALSE;

    BEGIN_QCALL;

    retVal = pAssembly->IsCollectible();

    END_QCALL;

    return retVal;
}

extern volatile uint32_t g_cAssemblies;

extern "C" uint32_t QCALLTYPE AssemblyNative_GetAssemblyCount()
{
    QCALL_CONTRACT_NO_GC_TRANSITION;

    return g_cAssemblies;
}

extern "C" void QCALLTYPE AssemblyNative_GetModule(QCall::AssemblyHandle pAssembly, LPCWSTR wszFileName, QCall::ObjectHandleOnStack retModule)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    Module * pModule = NULL;

    CQuickBytes qbLC;

    if (wszFileName == NULL)
        COMPlusThrow(kArgumentNullException, W("ArgumentNull_FileName"));
    if (wszFileName[0] == W('\0'))
        COMPlusThrow(kArgumentException, W("Argument_EmptyFileName"));


    MAKE_UTF8PTR_FROMWIDE(szModuleName, wszFileName);


    LPCUTF8 pModuleName = NULL;

    if SUCCEEDED(pAssembly->GetModule()->GetScopeName(&pModuleName))
    {
        if (::SString::_stricmp(pModuleName, szModuleName) == 0)
            pModule = pAssembly->GetModule();
    }


    if (pModule != NULL)
    {
        GCX_COOP();
        retModule.Set(pModule->GetExposedObject());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetExportedTypes(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retTypes)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    InlineSArray<TypeHandle, 20> types;
    IMDInternalImport *pImport = pAssembly->GetMDImport();

    {
        HENUMTypeDefInternalHolder phTDEnum(pImport);
        phTDEnum.EnumTypeDefInit();

        mdTypeDef mdTD;
        while(pImport->EnumNext(&phTDEnum, &mdTD))
        {
            DWORD dwFlags;
            IfFailThrow(pImport->GetTypeDefProps(
                mdTD,
                &dwFlags,
                NULL));

            // nested type
            mdTypeDef mdEncloser = mdTD;
            while (SUCCEEDED(pImport->GetNestedClassProps(mdEncloser, &mdEncloser)) &&
                   IsTdNestedPublic(dwFlags))
            {
                IfFailThrow(pImport->GetTypeDefProps(
                    mdEncloser,
                    &dwFlags,
                    NULL));
            }

            if (IsTdPublic(dwFlags))
            {
                TypeHandle typeHnd = ClassLoader::LoadTypeDefThrowing(pAssembly->GetModule(), mdTD,
                                                                      ClassLoader::ThrowIfNotFound,
                                                                      ClassLoader::PermitUninstDefOrRef);
                types.Append(typeHnd);
            }
        }
    }

    {
        HENUMInternalHolder phCTEnum(pImport);
        phCTEnum.EnumInit(mdtExportedType, mdTokenNil);

        // Now get the ExportedTypes that don't have TD's in the manifest file
        mdExportedType mdCT;
        while(pImport->EnumNext(&phCTEnum, &mdCT))
        {
            mdToken mdImpl;
            LPCSTR pszNameSpace;
            LPCSTR pszClassName;
            DWORD dwFlags;

            IfFailThrow(pImport->GetExportedTypeProps(
                mdCT,
                &pszNameSpace,
                &pszClassName,
                &mdImpl,
                NULL,           //binding
                &dwFlags));

            // nested type
            while ((TypeFromToken(mdImpl) == mdtExportedType) &&
                   (mdImpl != mdExportedTypeNil) &&
                   IsTdNestedPublic(dwFlags))
            {
                IfFailThrow(pImport->GetExportedTypeProps(
                    mdImpl,
                    NULL,       //namespace
                    NULL,       //name
                    &mdImpl,
                    NULL,       //binding
                    &dwFlags));
            }

            if ((TypeFromToken(mdImpl) == mdtFile) &&
                (mdImpl != mdFileNil) &&
                IsTdPublic(dwFlags))
            {
                NameHandle typeName(pszNameSpace, pszClassName);
                typeName.SetTypeToken(pAssembly->GetModule(), mdCT);
                TypeHandle typeHnd = pAssembly->GetLoader()->LoadTypeHandleThrowIfFailed(&typeName);

                types.Append(typeHnd);
            }
        }
    }

    {
        GCX_COOP();

        PTRARRAYREF orTypes = NULL;

        GCPROTECT_BEGIN(orTypes);

        // Return the types
        orTypes = (PTRARRAYREF)AllocateObjectArray(types.GetCount(), CoreLibBinder::GetClass(CLASS__TYPE));

        for(COUNT_T i = 0; i < types.GetCount(); i++)
        {
            TypeHandle typeHnd = types[i];

            OBJECTREF o = typeHnd.GetManagedClassObject();
            orTypes->SetAt(i, o);
        }

        retTypes.Set(orTypes);

        GCPROTECT_END();
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetForwardedTypes(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retTypes)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    InlineSArray<TypeHandle, 8> types;
    IMDInternalImport *pImport = pAssembly->GetMDImport();

    // enumerate the ExportedTypes table
    {
        HENUMInternalHolder phCTEnum(pImport);
        phCTEnum.EnumInit(mdtExportedType, mdTokenNil);

        // Now get the ExportedTypes that don't have TD's in the manifest file
        mdExportedType mdCT;
        while(pImport->EnumNext(&phCTEnum, &mdCT))
        {
            mdToken mdImpl;
            LPCSTR pszNameSpace;
            LPCSTR pszClassName;
            DWORD dwFlags;

            IfFailThrow(pImport->GetExportedTypeProps(mdCT,
                &pszNameSpace,
                &pszClassName,
                &mdImpl,
                NULL, //binding
                &dwFlags));

            if ((TypeFromToken(mdImpl) == mdtAssemblyRef) && (mdImpl != mdAssemblyRefNil))
            {
                NameHandle typeName(pszNameSpace, pszClassName);
                typeName.SetTypeToken(pAssembly->GetModule(), mdCT);
                TypeHandle typeHnd = pAssembly->GetLoader()->LoadTypeHandleThrowIfFailed(&typeName);

                types.Append(typeHnd);
            }
        }
    }

    // Populate retTypes
    {
        GCX_COOP();

        PTRARRAYREF orTypes = NULL;

        GCPROTECT_BEGIN(orTypes);

        // Return the types
        orTypes = (PTRARRAYREF)AllocateObjectArray(types.GetCount(), CoreLibBinder::GetClass(CLASS__TYPE));

        for(COUNT_T i = 0; i < types.GetCount(); i++)
        {
            TypeHandle typeHnd = types[i];

            OBJECTREF o = typeHnd.GetManagedClassObject();
            orTypes->SetAt(i, o);
        }

        retTypes.Set(orTypes);

        GCPROTECT_END();
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetManifestResourceNames(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retResourceNames)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    IMDInternalImport *pImport = pAssembly->GetMDImport();

    HENUMInternalHolder phEnum(pImport);
    phEnum.EnumInit(mdtManifestResource, mdTokenNil);

    DWORD dwCount = pImport->EnumGetCount(&phEnum);

    GCX_COOP();

    PTRARRAYREF ItemArray = (PTRARRAYREF) AllocateObjectArray(dwCount, g_pStringClass);

    GCPROTECT_BEGIN(ItemArray);

    for(DWORD i = 0;  i < dwCount; i++)
    {
        mdManifestResource mdResource;
        pImport->EnumNext(&phEnum, &mdResource);

        LPCSTR pszName = NULL;
        IfFailThrow(pImport->GetManifestResourceProps(
            mdResource,
            &pszName,   // name
            NULL,       // linkref
            NULL,       // offset
            NULL));     //flags

        OBJECTREF o = (OBJECTREF) StringObject::NewString(pszName);
        ItemArray->SetAt(i, o);
    }

    retResourceNames.Set(ItemArray);
    GCPROTECT_END();

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetReferencedAssemblies(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retReferencedAssemblies)
{
    BEGIN_QCALL;

    IMDInternalImport *pImport = pAssembly->GetMDImport();

    HENUMInternalHolder phEnum(pImport);
    phEnum.EnumInit(mdtAssemblyRef, mdTokenNil);

    DWORD dwCount = pImport->EnumGetCount(&phEnum);

    MethodTable* pAsmNameClass = CoreLibBinder::GetClass(CLASS__ASSEMBLY_NAME);

    GCX_COOP();

    struct {
        PTRARRAYREF ItemArray;
        ASSEMBLYNAMEREF pObj;
    } gc;
    gc.ItemArray = NULL;
    gc.pObj = NULL;

    GCPROTECT_BEGIN(gc);

    gc.ItemArray = (PTRARRAYREF) AllocateObjectArray(dwCount, pAsmNameClass);

    for(DWORD i = 0; i < dwCount; i++)
    {
        mdAssemblyRef mdAssemblyRef;
        pImport->EnumNext(&phEnum, &mdAssemblyRef);

        AssemblySpec spec;
        spec.InitializeSpec(mdAssemblyRef, pImport);

        gc.pObj = (ASSEMBLYNAMEREF) AllocateObject(pAsmNameClass);
        spec.AssemblyNameInit(&gc.pObj);

        gc.ItemArray->SetAt(i, (OBJECTREF) gc.pObj);
    }

    retReferencedAssemblies.Set(gc.ItemArray);
    GCPROTECT_END();

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetEntryPoint(QCall::AssemblyHandle pAssembly, QCall::ObjectHandleOnStack retMethod)
{
    QCALL_CONTRACT;

    MethodDesc* pMeth = NULL;

    BEGIN_QCALL;

    pMeth = pAssembly->GetEntryPoint();
    if (pMeth != NULL)
    {
        GCX_COOP();
        retMethod.Set(pMeth->AllocateStubMethodInfo());
    }

    END_QCALL;
}

//---------------------------------------------------------------------------------------
//
// Release QCALL for System.SafePEFileHandle
//
//

extern "C" void QCALLTYPE AssemblyNative_GetFullName(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    StackSString name;
    pAssembly->GetPEAssembly()->GetDisplayName(name);
    retString.Set(name);

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetExecutingAssembly(QCall::StackCrawlMarkHandle stackMark, QCall::ObjectHandleOnStack retAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    Assembly* pAssembly = SystemDomain::GetCallersAssembly(stackMark);
    if(pAssembly)
    {
        GCX_COOP();
        retAssembly.Set(pAssembly->GetExposedObject());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetEntryAssembly(QCall::ObjectHandleOnStack retAssembly)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    Assembly * pAssembly = GetAppDomain()->GetRootAssembly();
    if (pAssembly)
    {
        GCX_COOP();
        retAssembly.Set(pAssembly->GetExposedObject());
    }

    END_QCALL;
}

extern "C" void QCALLTYPE AssemblyNative_GetImageRuntimeVersion(QCall::AssemblyHandle pAssembly, QCall::StringHandleOnStack retString)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    // Retrieve the PEAssembly from the assembly.
    PEAssembly* pPEAssembly = pAssembly->GetPEAssembly();
    _ASSERTE(pPEAssembly!=NULL);

    LPCSTR pszVersion = NULL;
    IfFailThrow(pPEAssembly->GetMDImport()->GetVersionString(&pszVersion));

    SString version(SString::Utf8, pszVersion);

    // Allocate a managed string that contains the version and return it.
    retString.Set(version);

    END_QCALL;
}

/*static*/

extern "C" INT_PTR QCALLTYPE AssemblyNative_InitializeAssemblyLoadContext(INT_PTR ptrAssemblyLoadContext, BOOL fRepresentsTPALoadContext, BOOL fIsCollectible)
{
    QCALL_CONTRACT;

    INT_PTR ptrNativeAssemblyBinder = 0;

    BEGIN_QCALL;

    // We do not need to take a lock since this method is invoked from the ctor of AssemblyLoadContext managed type and
    // only one thread is ever executing a ctor for a given instance.
    //

    // Initialize the assembly binder instance in the VM
    PTR_AppDomain pCurDomain = AppDomain::GetCurrentDomain();
    DefaultAssemblyBinder *pDefaultBinder = pCurDomain->GetDefaultBinder();
    if (!fRepresentsTPALoadContext)
    {
        // Initialize a custom assembly binder
        CustomAssemblyBinder *pCustomBinder = NULL;

        AssemblyLoaderAllocator* loaderAllocator = NULL;
        OBJECTHANDLE loaderAllocatorHandle = NULL;

        if (fIsCollectible)
        {
            // Create a new AssemblyLoaderAllocator for an AssemblyLoadContext
            loaderAllocator = new AssemblyLoaderAllocator();

            GCX_COOP();
            LOADERALLOCATORREF pManagedLoaderAllocator = NULL;
            GCPROTECT_BEGIN(pManagedLoaderAllocator);
            {
                GCX_PREEMP();
                // Some of the initialization functions are not virtual. Call through the derived class
                // to prevent calling the base class version.
                loaderAllocator->Init();
                loaderAllocator->InitVirtualCallStubManager();

                // Setup the managed proxy now, but do not actually transfer ownership to it.
                // Once everything is setup and nothing can fail anymore, the ownership will be
                // atomically transferred by call to LoaderAllocator::ActivateManagedTracking().
                loaderAllocator->SetupManagedTracking(&pManagedLoaderAllocator);
            }

            // Create a strong handle to the LoaderAllocator
            loaderAllocatorHandle = pCurDomain->CreateHandle(pManagedLoaderAllocator);

            GCPROTECT_END();

            loaderAllocator->ActivateManagedTracking();
        }

        IfFailThrow(CustomAssemblyBinder::SetupContext(pDefaultBinder, loaderAllocator, loaderAllocatorHandle, ptrAssemblyLoadContext, &pCustomBinder));
        ptrNativeAssemblyBinder = reinterpret_cast<INT_PTR>(pCustomBinder);
    }
    else
    {
        // We are initializing the managed instance of Assembly Load Context that would represent the TPA binder.
        // First, confirm we do not have an existing managed ALC attached to the TPA binder.
        _ASSERTE(pDefaultBinder->GetAssemblyLoadContext() == (INT_PTR)NULL);

        // Attach the managed TPA binding context with the native one.
        pDefaultBinder->SetAssemblyLoadContext(ptrAssemblyLoadContext);
        ptrNativeAssemblyBinder = reinterpret_cast<INT_PTR>(pDefaultBinder);
    }

    END_QCALL;

    return ptrNativeAssemblyBinder;
}

/*static*/
extern "C" void QCALLTYPE AssemblyNative_PrepareForAssemblyLoadContextRelease(INT_PTR ptrNativeAssemblyBinder, INT_PTR ptrManagedStrongAssemblyLoadContext)
{
    QCALL_CONTRACT;

    BOOL fDestroyed = FALSE;

    BEGIN_QCALL;


    {
        GCX_COOP();
        reinterpret_cast<CustomAssemblyBinder *>(ptrNativeAssemblyBinder)->PrepareForLoadContextRelease(ptrManagedStrongAssemblyLoadContext);
    }

    END_QCALL;
}

/*static*/
extern "C" INT_PTR QCALLTYPE AssemblyNative_GetLoadContextForAssembly(QCall::AssemblyHandle pAssembly)
{
    QCALL_CONTRACT;

    INT_PTR ptrAssemblyLoadContext = 0;

    BEGIN_QCALL;

    _ASSERTE(pAssembly != NULL);

    AssemblyBinder* pAssemblyBinder = pAssembly->GetPEAssembly()->GetAssemblyBinder();

    if (!pAssemblyBinder->IsDefault())
    {
        // Fetch the managed binder reference from the native binder instance
        ptrAssemblyLoadContext = pAssemblyBinder->GetAssemblyLoadContext();
        _ASSERTE(ptrAssemblyLoadContext != (INT_PTR)NULL);
    }

    END_QCALL;

    return ptrAssemblyLoadContext;
}

// static
extern "C" BOOL QCALLTYPE AssemblyNative_InternalTryGetRawMetadata(
    QCall::AssemblyHandle assembly,
    UINT8 **blobRef,
    INT32 *lengthRef)
{
    QCALL_CONTRACT;

    PTR_CVOID metadata = nullptr;

    BEGIN_QCALL;

    _ASSERTE(assembly != nullptr);
    _ASSERTE(blobRef != nullptr);
    _ASSERTE(lengthRef != nullptr);

    static_assert_no_msg(sizeof(*lengthRef) == sizeof(COUNT_T));
    metadata = assembly->GetPEAssembly()->GetLoadedMetadata(reinterpret_cast<COUNT_T *>(lengthRef));
    *blobRef = reinterpret_cast<UINT8 *>(const_cast<PTR_VOID>(metadata));
    _ASSERTE(*lengthRef >= 0);

    END_QCALL;

    return metadata != nullptr;
}

// static
FCIMPL0(FC_BOOL_RET, AssemblyNative::IsTracingEnabled)
{
    FCALL_CONTRACT;

    FC_RETURN_BOOL(BinderTracing::IsEnabled());
}
FCIMPLEND

// static
extern "C" void QCALLTYPE AssemblyNative_TraceResolvingHandlerInvoked(LPCWSTR assemblyName, LPCWSTR handlerName, LPCWSTR alcName, LPCWSTR resultAssemblyName, LPCWSTR resultAssemblyPath)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    FireEtwAssemblyLoadContextResolvingHandlerInvoked(GetClrInstanceId(), assemblyName, handlerName, alcName, resultAssemblyName, resultAssemblyPath);

    END_QCALL;
}

// static
extern "C" void QCALLTYPE AssemblyNative_TraceAssemblyResolveHandlerInvoked(LPCWSTR assemblyName, LPCWSTR handlerName, LPCWSTR resultAssemblyName, LPCWSTR resultAssemblyPath)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    FireEtwAppDomainAssemblyResolveHandlerInvoked(GetClrInstanceId(), assemblyName, handlerName, resultAssemblyName, resultAssemblyPath);

    END_QCALL;
}

// static
extern "C" void QCALLTYPE AssemblyNative_TraceAssemblyLoadFromResolveHandlerInvoked(LPCWSTR assemblyName, bool isTrackedAssembly, LPCWSTR requestingAssemblyPath, LPCWSTR requestedAssemblyPath)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    FireEtwAssemblyLoadFromResolveHandlerInvoked(GetClrInstanceId(), assemblyName, isTrackedAssembly, requestingAssemblyPath, requestedAssemblyPath);

    END_QCALL;
}

// static
extern "C" void QCALLTYPE AssemblyNative_TraceSatelliteSubdirectoryPathProbed(LPCWSTR filePath, HRESULT hr)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    BinderTracing::PathProbed(filePath, BinderTracing::PathSource::SatelliteSubdirectory, hr);

    END_QCALL;
}

// static
extern "C" void QCALLTYPE AssemblyNative_ApplyUpdate(
    QCall::AssemblyHandle assembly,
    UINT8* metadataDelta,
    INT32 metadataDeltaLength,
    UINT8* ilDelta,
    INT32 ilDeltaLength,
    UINT8* pdbDelta,
    INT32 pdbDeltaLength)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    _ASSERTE(assembly != nullptr);
    _ASSERTE(metadataDelta != nullptr);
    _ASSERTE(metadataDeltaLength > 0);
    _ASSERTE(ilDelta != nullptr);
    _ASSERTE(ilDeltaLength > 0);

#ifdef FEATURE_METADATA_UPDATER
    GCX_COOP();
    {
        if (CORDebuggerAttached())
        {
            COMPlusThrow(kNotSupportedException, W("NotSupported_DebuggerAttached"));
        }
        Module* pModule = assembly->GetModule();
        if (!pModule->IsEditAndContinueEnabled())
        {
            COMPlusThrow(kInvalidOperationException, W("InvalidOperation_AssemblyNotEditable"));
        }
        HRESULT hr = ((EditAndContinueModule*)pModule)->ApplyEditAndContinue(metadataDeltaLength, metadataDelta, ilDeltaLength, ilDelta);
        if (FAILED(hr))
        {
            COMPlusThrow(kInvalidOperationException, W("InvalidOperation_EditFailed"));
        }
        g_metadataUpdatesApplied = true;
    }
#else
    COMPlusThrow(kNotImplementedException);
#endif

    END_QCALL;
}

// static
extern "C" BOOL QCALLTYPE AssemblyNative_IsApplyUpdateSupported()
{
    QCALL_CONTRACT;

    BOOL result = false;

    BEGIN_QCALL;

#ifdef FEATURE_METADATA_UPDATER
    result = CORDebuggerAttached() || g_pConfig->ForceEnc() || g_pConfig->DebugAssembliesModifiable();
#endif

    END_QCALL;

    return result;
}

namespace
{
    LPCSTR TypeMapAssemblyTargetAttributeName = "System.Runtime.InteropServices.TypeMapAssemblyTargetAttribute`1";
    LPCSTR TypeMapAttributeName = "System.Runtime.InteropServices.TypeMapAttribute`1";
    LPCSTR TypeMapAssociationAttributeName = "System.Runtime.InteropServices.TypeMapAssociationAttribute`1";

    bool IsTypeSpecForTypeMapGroup(
        MethodTable* groupTypeMT,
        Assembly* pAssembly,
        mdToken typeSpec)
    {
        STANDARD_VM_CONTRACT;
        _ASSERTE(groupTypeMT != NULL);
        _ASSERTE(pAssembly != NULL);
        _ASSERTE(TypeFromToken(typeSpec) == mdtTypeSpec);

        IMDInternalImport* pImport = pAssembly->GetMDImport();

        PCCOR_SIGNATURE sig;
        ULONG sigLen;
        IfFailThrow(pImport->GetTypeSpecFromToken(typeSpec, &sig, &sigLen));

        SigPointer sigPointer{ sig, sigLen };

        SigTypeContext context{};
        TypeHandle typeMapAttribute = sigPointer.GetTypeHandleNT(pAssembly->GetModule(), &context);
        if (typeMapAttribute.IsNull()
            || !typeMapAttribute.HasInstantiation())    // All TypeMap attributes are generic.
        {
            return false;
        }

        Instantiation genericParams = typeMapAttribute.GetInstantiation();
        if (genericParams.GetNumArgs() != 1) // All TypeMap attributes have a single generic parameter.
            return false;

        return genericParams[0] == groupTypeMT;
    }

    template<typename ATTR_PROCESSOR>
    void ProcessTypeMapAttribute(
        LPCSTR attributeName,
        ATTR_PROCESSOR& processor,
        MethodTable* groupTypeMT,
        Assembly* pAssembly)
    {
        STANDARD_VM_CONTRACT;
        _ASSERTE(attributeName != NULL);
        _ASSERTE(groupTypeMT != NULL);
        _ASSERTE(pAssembly != NULL);

        HRESULT hr;
        IMDInternalImport* pImport = pAssembly->GetMDImport();

        // Find all the CustomAttributes with the supplied name
        MDEnumHolder hEnum(pImport);
        hr = pImport->EnumCustomAttributeByNameInit(
            TokenFromRid(1, mdtAssembly),
            attributeName,
            &hEnum);
        IfFailThrow(hr);

        // Enumerate all instances of the CustomAttribute we asked about.
        // Since the TypeMap attributes are generic, we need to narrow the
        // search to only those that are instantiated over the "GroupType"
        // that is supplied by the caller.
        mdTypeSpec lastMatchingTypeSpec = mdTypeSpecNil;
        mdCustomAttribute tkAttribute;
        while (pImport->EnumNext(&hEnum, &tkAttribute))
        {
            mdToken tokenMember;
            IfFailThrow(pImport->GetCustomAttributeProps(tkAttribute, &tokenMember));

            mdToken tokenType;
            IfFailThrow(pImport->GetParentToken(tokenMember, &tokenType));

            // Ensure the parent token is a TypeSpec.
            // This can occur if the attribute is redefined externally.
            if (TypeFromToken(tokenType) != mdtTypeSpec)
                continue;

            // Determine if this TypeSpec contains the "GroupType" we are looking for.
            // There is no requirement in ECMA-335 that the same TypeSpec be used
            // for the same generic instantiation. It is true for Roslyn assemblies so we
            // will do a check as an optimization, but we must fall back and re-check
            // the TypeSpec contents to be sure it doesn't match.
            if (tokenType != lastMatchingTypeSpec)
            {
                if (!IsTypeSpecForTypeMapGroup(groupTypeMT, pAssembly, tokenType))
                    continue;

                lastMatchingTypeSpec = (mdTypeSpec)tokenType;
            }

            // We've determined the attribute is the instantiation we want, now process the attribute contents.
            void const* blob;
            ULONG blobLen;
            IfFailThrow(pImport->GetCustomAttributeAsBlob(tkAttribute, &blob, &blobLen));

            // Pass the blob data off to the processor.
            if (!processor.Process(blob, blobLen))
            {
                // The processor has indicated processing should stop.
                break;
            }
        }
    }

    class AssemblyPtrCollectionTraits : public DefaultSHashTraits<Assembly*>
    {
    public:
        typedef Assembly* key_t;
        static const key_t GetKey(Assembly* e) { LIMITED_METHOD_CONTRACT; return e; }
        static count_t Hash(key_t key) { LIMITED_METHOD_CONTRACT; return (count_t)(size_t)key; }
        static BOOL Equals(key_t lhs, key_t rhs) { LIMITED_METHOD_CONTRACT; return (lhs == rhs); }
    };

    using AssemblyPtrCollection = SHash<AssemblyPtrCollectionTraits>;

    // Used for TypeMapAssemblyTargetAttribute`1 attribute.
    class AssemblyTargetProcessor final
    {
        AssemblyPtrCollection _toProcess;
        AssemblyPtrCollection _processed;

    public:
        AssemblyTargetProcessor(Assembly* first)
        {
            _toProcess.Add(first);
        }

        BOOL Process(void const* blob, ULONG blobLen)
        {
            CustomAttributeParser cap(blob, blobLen);
            IfFailThrow(cap.ValidateProlog());

            LPCUTF8 assemblyName;
            ULONG assemblyNameLen;
            IfFailThrow(cap.GetNonNullString(&assemblyName, &assemblyNameLen));

            // Load the assembly
            SString assemblyNameString{ SString::Utf8, assemblyName, assemblyNameLen };

            AssemblySpec spec;
            spec.Init(assemblyNameString);

            Assembly* pAssembly = spec.LoadAssembly(FILE_LOADED);

            // Only add the assembly if it is unknown.
            if (_toProcess.Lookup(pAssembly) == NULL
                && _processed.Lookup(pAssembly) == NULL)
            {
                _toProcess.Add(pAssembly);
            }

            return TRUE;
        }

        bool IsEmpty() const
        {
            return _toProcess.GetCount() == 0;
        }

        Assembly* GetNext()
        {
            AssemblyPtrCollection::Iterator first = _toProcess.Begin();
            Assembly* tmp = *first;
            _toProcess.Remove(first);
            _ASSERTE(_toProcess.Lookup(tmp) == NULL);
            _processed.Add(tmp);
            _ASSERTE(_processed.Lookup(tmp) != NULL);
            return tmp;
        }
    };

    // Used for TypeMapAttribute`1 and TypeMapAssociationAttribute`1 attributes.
    class MappingsProcessor final
    {
        BOOL (*_callback)(CallbackContext* context, ProcessAttributesCallbackArg* arg);
        CallbackContext* _context;

    public:
        MappingsProcessor(
            BOOL (*callback)(CallbackContext* context, ProcessAttributesCallbackArg* arg),
            CallbackContext* context)
            : _callback{ callback }
            , _context{ context }
        {
            _ASSERTE(_callback != NULL);
        }

        BOOL Process(void const* blob, ULONG blobLen)
        {
            CustomAttributeParser cap(blob, blobLen);
            IfFailThrow(cap.ValidateProlog());

            // Observe that one of the constructors for TypeMapAttribute`1
            // takes three (3) arguments, but we only ever look at two (2).
            // This is because the third argument isn't needed by the
            // mapping logic and is only used by the Trimmer.

            LPCUTF8 str1;
            ULONG strLen1;
            IfFailThrow(cap.GetNonNullString(&str1, &strLen1));

            LPCUTF8 str2;
            ULONG strLen2;
            IfFailThrow(cap.GetNonNullString(&str2, &strLen2));

            ProcessAttributesCallbackArg arg;
            arg.Utf8String1 = str1;
            arg.Utf8String2 = str2;
            arg.StringLen1 = strLen1;
            arg.StringLen2 = strLen2;

            return _callback(_context, &arg);
        }
    };
}

extern "C" void QCALLTYPE TypeMapLazyDictionary_ProcessAttributes(
    QCall::AssemblyHandle pAssembly,
    QCall::TypeHandle pGroupType,
    BOOL (*newExternalTypeEntry)(CallbackContext* context, ProcessAttributesCallbackArg* arg),
    BOOL (*newProxyTypeEntry)(CallbackContext* context, ProcessAttributesCallbackArg* arg),
    CallbackContext* context)
{
    QCALL_CONTRACT;
    _ASSERTE(pAssembly != NULL);
    _ASSERTE(!pGroupType.AsTypeHandle().IsNull());
    _ASSERTE(newExternalTypeEntry != NULL || newProxyTypeEntry != NULL);
    _ASSERTE(context != NULL);

    BEGIN_QCALL;

    TypeHandle groupTypeTH = pGroupType.AsTypeHandle();
    _ASSERTE(!groupTypeTH.IsTypeDesc());
    MethodTable* groupTypeMT = groupTypeTH.AsMethodTable();

    AssemblyTargetProcessor assemblies{ pAssembly };
    while (!assemblies.IsEmpty())
    {
        Assembly* currAssembly = assemblies.GetNext();

        // Set the current assembly in the context.
        {
            GCX_COOP();
            context->_currAssembly = currAssembly->GetExposedObject();
        }

        ProcessTypeMapAttribute(
            TypeMapAssemblyTargetAttributeName,
            assemblies,
            groupTypeMT,
            currAssembly);

        if (newExternalTypeEntry != NULL)
        {
            MappingsProcessor onExternalType{ newExternalTypeEntry, context };
            ProcessTypeMapAttribute(
                TypeMapAttributeName,
                onExternalType,
                groupTypeMT,
                currAssembly);
        }

        if (newProxyTypeEntry != NULL)
        {
            MappingsProcessor onProxyType{ newProxyTypeEntry, context };
            ProcessTypeMapAttribute(
                TypeMapAssociationAttributeName,
                onProxyType,
                groupTypeMT,
                currAssembly);
        }
    }

    END_QCALL;
}
