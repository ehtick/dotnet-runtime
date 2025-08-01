// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// FRAMES.H


//
// These C++ classes expose activation frames to the rest of the EE.
// Activation frames are actually created by JIT-generated or stub-generated
// code on the machine stack. Thus, the layout of the Frame classes and
// the JIT/Stub code generators are tightly interwined.
//
// IMPORTANT: Since frames are not actually constructed by C++,
// don't try to define constructor/destructor functions. They won't get
// called.
//
// IMPORTANT: Not all methods have full-fledged activation frames (in
// particular, the JIT may create frameless methods.) This is one reason
// why Frame doesn't expose a public "Next()" method: such a method would
// skip frameless method calls. You must instead use one of the
// StackWalk methods.
//
//
// The following is the hierarchy of frames:
//
//    Frame                     - the root class. There are no actual instances
//    |                           of Frames.
//    |
//    +- FaultingExceptionFrame - this frame was placed on a method which faulted
//    |                           to save additional state information
//    |
#ifdef FEATURE_HIJACK
//    |
//    +-HijackFrame             - if a method's return address is hijacked, we
//    |                           construct one of these to allow crawling back
//    |                           to where the return should have gone.
//    |
//    +-ResumableFrame          - this abstract frame provides the context necessary to
//    | |                         allow garbage collection during handling of
//    | |                         a resumable exception (e.g. during edit-and-continue,
//    | |                         or under GCStress4).
//    | |
//    | +-RedirectedThreadFrame - this frame is used for redirecting threads during suspension
//    |
#endif // FEATURE_HIJACK
//    |
//    |
//    |
//    +-InlinedCallFrame        - if a call to unmanaged code is hoisted into
//    |                           a JIT'ted caller, the calling method keeps
//    |                           this frame linked throughout its activation.
//    |
//    +-TransitionFrame         - this abstract frame represents a transition from
//    | |                         one or more nested frameless method calls
//    | |                         to either a EE runtime helper function or
//    | |                         a framed method.
//    | |
//    | +-FramedMethodFrame     - this abstract frame represents a call to a method
//    |   |                       that generates a full-fledged frame.
//    |   |
#ifdef FEATURE_COMINTEROP
//    |   |
//    |   +-CLRToCOMMethodFrame  - represents a CLR to COM call using the generic worker
//    |   |
#endif //FEATURE_COMINTEROP
//    |   |
//    |   +-PInvokeCalliFrame   - protects arguments when a call to GetILStubForCalli is made
//    |   |                       to get or create IL stub for an unmanaged CALLI
//    |   |
//    |   +-PrestubMethodFrame  - represents a call to a prestub
//    |   |
//    |   +-StubDispatchFrame   - represents a call into the virtual call stub manager
//    |   |
//    |   +-CallCountingHelperFrame - represents a call into the call counting helper when the
//    |   |                           call count threshold is reached
//    |   |
//    |   +-ExternalMethodFrame  - represents a call from an ExternalMethodThunk
//    |
#ifdef FEATURE_COMINTEROP
//    +-UnmanagedToManagedFrame - this frame represents a transition from
//    | |                         unmanaged code back to managed code. It's
//    | |                         main functions are to stop CLR exception
//    | |                         propagation and to expose unmanaged parameters.
//    | |
//    | +-ComMethodFrame        - this frame represents a transition from
//    |   |                       com to CLR
//    |   |
//    |   +-ComPrestubMethodFrame - prestub frame for calls from COM to CLR
//    |
#endif //FEATURE_COMINTEROP
#if defined(TARGET_X86) && !defined(UNIX_X86_ABI)
//    +-TailCallFrame           - padding for tailcalls
//    |
#endif
//    +-ProtectValueClassFrame
//    |
//    +-DebuggerClassInitMarkFrame - marker frame to indicate that "class init" code is running
//    |
//    +-DebuggerExitFrame - marker frame to indicate control flow has left the runtime
//    |
//    +-DebuggerU2MCatchHandlerFrame - marker frame to indicate that native code is going to catch and
//    |                                swallow a managed exception
//    |
#ifdef DEBUGGING_SUPPORTED
//    +-FuncEvalFrame         - frame for debugger function evaluation
#endif // DEBUGGING_SUPPORTED
//    |
//    |
//    +-ExceptionFilterFrame - this frame wraps call to exception filter
//
//------------------------------------------------------------------------
#if 0
//------------------------------------------------------------------------

This is the list of Interop stubs & transition helpers with information
regarding what (if any) Frame they used and where they were set up:

P/Invoke:
 JIT inlined: The code to call the method is inlined into the caller by the JIT.
    InlinedCallFrame is erected by the JITted code.
 Requires marshaling: The stub does not erect any frames explicitly but contains
    an unmanaged CALLI which turns it into the JIT inlined case.

Delegate over a native function pointer:
 The same as P/Invoke but the raw JIT inlined case is not present (the call always
 goes through an IL stub).

Calli:
 The same as P/Invoke.
 PInvokeCalliFrame is erected in stub generated by GenerateGetStubForPInvokeCalli
 before calling to GetILStubForCalli which generates the IL stub. This happens only
 the first time a call via the corresponding VASigCookie is made.

ClrToCom:
 Late-bound or eventing: The stub is generated by GenerateGenericComplusWorker
    (x86) or exists statically as GenericCLRToCOMCallStub[RetBuffArg] (64-bit),
    and it erects a CLRToCOMMethodFrame frame.
 Early-bound: The stub does not erect any frames explicitly but contains an
    unmanaged CALLI which turns it into the JIT inlined case.

ComToClr:
  Normal stub:
 Interpreted: The stub is generated by ComCall::CreateGenericComCallStub
    (in ComToClrCall.cpp) and it erects a ComMethodFrame frame.
Prestub:
 The prestub is ComCallPreStub (in ComCallableWrapper.cpp) and it erects
    a ComPrestubMethodFrame frame.

Reverse P/Invoke (used for C++ exports & fixups as well as delegates
obtained from function pointers):
  Normal stub:
 The stub exists statically as UMThunkStub and calls to IL stub.
Prestub:
 The prestub exists statically as TheUMEntryPrestub.

//------------------------------------------------------------------------
#endif // 0
//------------------------------------------------------------------------

#include "FrameTypes.h"

//------------------------------------------------------------------------

#ifndef __frames_h__
#define __frames_h__
#if defined(_MSC_VER) && defined(TARGET_X86) && !defined(FPO_ON)
#pragma optimize("y", on)   // Small critical routines, don't put in EBP frame
#define FPO_ON 1
#define FRAMES_TURNED_FPO_ON 1
#endif

#include "util.hpp"
#include "vars.hpp"
#include "regdisp.h"
#include "object.h"
#include <stddef.h>
#include "siginfo.hpp"
#include "method.hpp"
#include "stackwalk.h"
#include "stubmgr.h"
#include "threads.h"
#include "callingconvention.h"

// Forward references
class Frame;
class FramedMethodFrame;
typedef DPTR(class FramedMethodFrame) PTR_FramedMethodFrame;
struct HijackArgs;
struct ResolveCacheElem;
#if defined(DACCESS_COMPILE)
class DacDbiInterfaceImpl;
#endif // DACCESS_COMPILE
#ifdef FEATURE_COMINTEROP
class ComMethodFrame;
class ComCallMethodDesc;
#endif // FEATURE_COMINTEROP

// Note: the value (-1) is used to generate the largest possible pointer value: this keeps frame addresses
// increasing upward. Because we want to ensure that we don't accidentally change this, we have a C_ASSERT
// in stackwalk.cpp. Since it requires constant values as args, we need to define FRAME_TOP in two steps.
// First we define FRAME_TOP_VALUE which we'll use when we do the compile-time check, then we'll define
// FRAME_TOP in terms of FRAME_TOP_VALUE. Defining FRAME_TOP as a PTR_Frame means we don't have to type cast
// whenever we compare it to a PTR_Frame value (the usual use of the value).
#define FRAME_TOP_VALUE  ~0     // we want to say -1 here, but gcc has trouble with the signed value
#define FRAME_TOP (PTR_Frame(FRAME_TOP_VALUE))
#define GCFRAME_TOP (PTR_GCFrame(FRAME_TOP_VALUE))


enum class FrameIdentifier : TADDR
{
    None = 0,
#define FRAME_TYPE_NAME(frameType) frameType,
#include "FrameTypes.h"
    CountPlusOne
};

// TransitionFrame only apis
class TransitionFrame;
TADDR Frame_GetTransitionBlock(TransitionFrame* frame);
BOOL Frame_SuppressParamTypeArg(TransitionFrame* frame);

//------------------------------------------------------------------------
// Frame defines methods common to all frame types. There are no actual
// instances of root frames.
//------------------------------------------------------------------------

class Frame
{
    friend class CheckAsmOffsets;
#ifdef DACCESS_COMPILE
    friend void Thread::EnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif

public:
    FrameIdentifier GetFrameIdentifier() { LIMITED_METHOD_DAC_CONTRACT; return _frameIdentifier; }

    enum ETransitionType
    {
        TT_NONE,
        TT_M2U, // we can safely cast to a FramedMethodFrame
        TT_U2M, // we can safely cast to a UnmanagedToManagedFrame
        TT_AppDomain, // transitioniting between AppDomains.
        TT_InternalCall, // calling into the CLR (ecall/fcall).
    };

    enum Interception
    {
        INTERCEPTION_NONE,
        INTERCEPTION_CLASS_INIT,
        INTERCEPTION_EXCEPTION,
        INTERCEPTION_CONTEXT,
        INTERCEPTION_SECURITY,
        INTERCEPTION_PRESTUB,
        INTERCEPTION_OTHER,

        INTERCEPTION_COUNT
    };

    void GcScanRoots(promote_func *fn, ScanContext* sc);
    unsigned GetFrameAttribs();
#ifndef DACCESS_COMPILE
    void ExceptionUnwind();
#endif
    BOOL NeedsUpdateRegDisplay();
    BOOL IsTransitionToNativeFrame();
    MethodDesc *GetFunction();
    Assembly *GetAssembly();
    PTR_BYTE GetIP();
    TADDR GetReturnAddressPtr();
    PCODE GetReturnAddress();
    void UpdateRegDisplay(const PREGDISPLAY, bool updateFloats = false);
    int GetFrameType();
    ETransitionType GetTransitionType();
    Interception GetInterception();
    void GetUnmanagedCallSite(TADDR* ip, TADDR* returnIP, TADDR* returnSP);
    BOOL TraceFrame(Thread *thread, BOOL fromPatch, TraceDestination *trace, REGDISPLAY *regs);
#ifdef DACCESS_COMPILE
    void EnumMemoryRegions(CLRDataEnumMemoryFlags flags);
#endif // DACCESS_COMPILE
#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
    BOOL Protects(OBJECTREF *ppObjectRef);
#endif // defined(_DEBUG) && !defined(DACCESS_COMPILE)

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc) {
        LIMITED_METHOD_CONTRACT;
        // Nothing to protect
    }

    // Should only be called on Frames that derive from TransitionFrame
    TADDR GetTransitionBlock_Impl()
    {
        _ASSERTE(!"Unexpected");
        return (TADDR)0;
    }

    // Should only be called on Frames that derive from TransitionFrame
    BOOL SuppressParamTypeArg_Impl()
    {
        _ASSERTE(!"Unexpected");
        return FALSE;
    }

    //------------------------------------------------------------------------
    // Special characteristics of a frame
    //------------------------------------------------------------------------
    enum FrameAttribs {
        FRAME_ATTR_NONE = 0,
        FRAME_ATTR_EXCEPTION = 1,           // This frame caused an exception
        FRAME_ATTR_FAULTED = 4,             // Exception caused by Win32 fault
        FRAME_ATTR_RESUMABLE = 8,           // We may resume from this frame
    };
    unsigned GetFrameAttribs_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return FRAME_ATTR_NONE;
    }

    //------------------------------------------------------------------------
    // Performs cleanup on an exception unwind
    //------------------------------------------------------------------------
#ifndef DACCESS_COMPILE
    void ExceptionUnwind_Impl()
    {
        // Nothing to do here.
        LIMITED_METHOD_CONTRACT;
    }
#endif

    // Should be overridden to return TRUE if the frame contains register
    // state of the caller.
    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return FALSE;
    }

    //------------------------------------------------------------------------
    // Is this a frame used on transition to native code from jitted code?
    //------------------------------------------------------------------------
    BOOL IsTransitionToNativeFrame_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return FALSE;
    }

    MethodDesc *GetFunction_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return NULL;
    }

    Assembly *GetAssembly_Impl()
    {
        WRAPPER_NO_CONTRACT;
        MethodDesc *pMethod = GetFunction();
        if (pMethod != NULL)
            return pMethod->GetModule()->GetAssembly();
        else
            return NULL;
    }

    // indicate the current X86 IP address within the current method
    // return 0 if the information is not available
    PTR_BYTE GetIP_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return NULL;
    }

    // DACCESS: GetReturnAddressPtr should return the
    // target address of the return address in the frame.
    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return 0;
    }

    // ASAN doesn't like us messing with the return address.
    DISABLE_ASAN PCODE GetReturnAddress_Impl()
    {
        WRAPPER_NO_CONTRACT;
        TADDR ptr = GetReturnAddressPtr();
        return (ptr != 0) ? *PTR_PCODE(ptr) : 0;
    }

#ifndef DACCESS_COMPILE
    // ASAN doesn't like us messing with the return address.
    void DISABLE_ASAN SetReturnAddress(TADDR val)
    {
        WRAPPER_NO_CONTRACT;
        TADDR ptr = GetReturnAddressPtr();
        _ASSERTE(ptr != (TADDR)NULL);
        *(TADDR*)ptr = val;
    }
#endif // #ifndef DACCESS_COMPILE

    static bool HasValidFrameIdentifier(Frame * pFrame);
    void Init(FrameIdentifier frameIdentifier);

    // Callers, note that the REGDISPLAY parameter is actually in/out. While
    // UpdateRegDisplay is generally used to fill out the REGDISPLAY parameter, some
    // overrides (e.g., code:ResumableFrame::UpdateRegDisplay) will actually READ what
    // you pass in. So be sure to pass in a valid or zeroed out REGDISPLAY.
    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats)
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return;
    }

    //------------------------------------------------------------------------
    // Debugger support
    //------------------------------------------------------------------------

public:
    // Get the type of transition.
    // M-->U, U-->M
    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_NONE;
    }

    enum
    {
        TYPE_INTERNAL,
        TYPE_ENTRY,
        TYPE_EXIT,
        TYPE_INTERCEPTION,
        TYPE_CALL,
        TYPE_FUNC_EVAL,

        TYPE_COUNT
    };

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_INTERNAL;
    };

    // When stepping into a method, various other methods may be called.
    // These are refererred to as interceptors. They are all invoked
    // with frames of various types. GetInterception() indicates whether
    // the frame was set up for execution of such interceptors

    Interception GetInterception_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_NONE;
    }

    // Return information about an unmanaged call the frame
    // will make.
    // ip - the unmanaged routine which will be called
    // returnIP - the address in the stub which the unmanaged routine
    //            will return to.
    // returnSP - the location returnIP is pushed onto the stack
    //            during the call.
    //
    void GetUnmanagedCallSite_Impl(TADDR* ip,
                                   TADDR* returnIP,
                                   TADDR* returnSP)
    {
        LIMITED_METHOD_CONTRACT;
        if (ip)
            *ip = 0;

        if (returnIP)
            *returnIP = 0;

        if (returnSP)
            *returnSP = 0;
    }

    // Return where the frame will execute next - the result is filled
    // into the given "trace" structure.  The frame is responsible for
    // detecting where it is in its execution lifetime.
    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch,
                            TraceDestination *trace, REGDISPLAY *regs)
    {
        LIMITED_METHOD_CONTRACT;
        LOG((LF_CORDB, LL_INFO10000,
             "Default TraceFrame always returns false.\n"));
        return FALSE;
    }

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions_Impl(CLRDataEnumMemoryFlags flags)
    {
        WRAPPER_NO_CONTRACT;

        // Many frames store a MethodDesc pointer in m_Datum
        // so pick that up automatically.
        MethodDesc* func = GetFunction();
        if (func)
        {
            func->EnumMemoryRegions(flags);
        }
    }
#endif

    //---------------------------------------------------------------
    // Expose key offsets and values for stub generation.
    //---------------------------------------------------------------
    static BYTE GetOffsetOfNextLink()
    {
        WRAPPER_NO_CONTRACT;
        size_t ofs = offsetof(class Frame, m_Next);
        _ASSERTE(FitsInI1(ofs));
        return (BYTE)ofs;
    }

#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
    BOOL Protects_Impl(OBJECTREF *ppObjectRef)
    {
        LIMITED_METHOD_CONTRACT;
        return FALSE;
    }
#endif

#ifndef DACCESS_COMPILE
    // Link and Unlink this frame
    VOID Push();
    VOID Pop();
    VOID Push(Thread *pThread);
    VOID Pop(Thread *pThread);
#endif // DACCESS_COMPILE

#ifdef _DEBUG_IMPL
    void Log();
    static BOOL ShouldLogTransitions() { WRAPPER_NO_CONTRACT; return LoggingOn(LF_STUBS, LL_INFO1000000); }
    static void __stdcall LogTransition(Frame* frame);
    void LogFrame(int LF, int LL);       // General purpose logging.
    void LogFrameChain(int LF, int LL);  // Log the whole chain.
#endif

    static LPCSTR GetFrameTypeName(FrameIdentifier frameIdentifier);

private:
    FrameIdentifier _frameIdentifier;

protected:
    // Pointer to the next frame up the stack.
    PTR_Frame m_Next;        // offset +4

public:
    PTR_Frame PtrNextFrame() { return m_Next; }

private:
    // Because JIT-method activations cannot be expressed as Frames,
    // everyone must use the StackCrawler to walk the frame chain
    // reliably. We'll expose the Next method only to the StackCrawler
    // to prevent mistakes.
    /*<TODO>@NICE: Restrict "friendship" again to the StackWalker method;
      not done because of circular dependency with threads.h</TODO>
    */
    //        friend Frame* Thread::StackWalkFrames(PSTACKWALKFRAMESCALLBACK pCallback, VOID *pData);
    friend class Thread;
    friend void CrawlFrame::GotoNextFrame();
    friend class StackFrameIterator;
    friend class TailCallFrame;
    friend class AppDomain;
    friend VOID RealCOMPlusThrow(OBJECTREF);
#ifdef _DEBUG
    friend LONG WINAPI CLRVectoredExceptionHandlerShim(PEXCEPTION_POINTERS pExceptionInfo);
#endif
#ifdef HOST_64BIT
    friend Thread * JIT_InitPInvokeFrame(InlinedCallFrame *pFrame);
#endif
#ifdef FEATURE_EH_FUNCLETS
    friend struct ExInfo;
#endif
#if defined(DACCESS_COMPILE)
    friend class DacDbiInterfaceImpl;
#endif // DACCESS_COMPILE

    PTR_Frame  Next()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_Next;
    }

protected:
#ifndef DACCESS_COMPILE
    // Frame is considered an abstract class: this protected constructor
    // causes any attempt to instantiate one to fail at compile-time.
    Frame(FrameIdentifier frameIdentifier)
    : _frameIdentifier(frameIdentifier), m_Next(dac_cast<PTR_Frame>(nullptr))
    {
        LIMITED_METHOD_CONTRACT;
    }

#endif // DACCESS_COMPILE

#ifndef DACCESS_COMPILE
#if !defined(TARGET_X86) || defined(TARGET_UNIX)
    static void UpdateFloatingPointRegisters(const PREGDISPLAY pRD);
#endif // !TARGET_X86 || TARGET_UNIX
#endif // DACCESS_COMPILE

#if defined(TARGET_UNIX) && !defined(DACCESS_COMPILE)
    ~Frame() { PopIfChained(); }

    void PopIfChained();
#endif // TARGET_UNIX && !DACCESS_COMPILE

    friend struct ::cdac_data<Frame>;
};

template<>
struct cdac_data<Frame>
{
    static constexpr size_t Next = offsetof(Frame, m_Next);
};

//-----------------------------------------------------------------------------
// This frame provides a context for a code location at which
// execution was interrupted and may be resumed,
// such as asynchronous suspension or handling of an exception.
//
// It is necessary to create this frame if garbage
// collection may happen during the interruption.
// The FRAME_ATTR_RESUMABLE flag tells
// the GC that the preceding frame needs to be treated
// like the top of stack (with the important implication that
// caller-save-registers will be potential roots).
//-----------------------------------------------------------------------------
#ifdef FEATURE_HIJACK
//-----------------------------------------------------------------------------

typedef DPTR(class ResumableFrame) PTR_ResumableFrame;

class ResumableFrame : public Frame
{
public:
#ifndef DACCESS_COMPILE
    ResumableFrame(T_CONTEXT* regs) : Frame(FrameIdentifier::ResumableFrame) {
        LIMITED_METHOD_CONTRACT;
        m_Regs = regs;
    }

    ResumableFrame(FrameIdentifier frameIdentifier, T_CONTEXT* regs) : Frame(frameIdentifier) {
        LIMITED_METHOD_CONTRACT;
        m_Regs = regs;
    }
#endif

    TADDR GetReturnAddressPtr_Impl();

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats);

    unsigned GetFrameAttribs_Impl() {
        LIMITED_METHOD_DAC_CONTRACT;
        return FRAME_ATTR_RESUMABLE;    // Treat the next frame as the top frame.
    }

    T_CONTEXT *GetContext() {
        LIMITED_METHOD_DAC_CONTRACT;
        return (m_Regs);
    }

#ifdef DACCESS_COMPILE
    void EnumMemoryRegions_Impl(CLRDataEnumMemoryFlags flags)
    {
        WRAPPER_NO_CONTRACT;
        Frame::EnumMemoryRegions_Impl(flags);
        m_Regs.EnumMem();
    }
#endif

    void GcScanRoots_Impl(promote_func* fn, ScanContext* sc)
    {
        WRAPPER_NO_CONTRACT;

        // The captured context may be provided by OS or by our own
        // capture routine. The context may not necessary be on the
        // stack or could be outside of reported stack range.
        // To be sure that the registers in the context are reported
        // in conservative root reporting, just report them here.
#if defined(FEATURE_CONSERVATIVE_GC) && !defined(DACCESS_COMPILE)
        if (sc->promotion && g_pConfig->GetGCConservative())
        {

#ifdef TARGET_AMD64
            Object** firstIntReg = (Object**)&this->GetContext()->Rax;
            Object** lastIntReg  = (Object**)&this->GetContext()->R15;
#elif defined(TARGET_X86)
            Object** firstIntReg = (Object**)&this->GetContext()->Edi;
            Object** lastIntReg  = (Object**)&this->GetContext()->Ebp;
#elif defined(TARGET_ARM)
            Object** firstIntReg = (Object**)&this->GetContext()->R0;
            Object** lastIntReg  = (Object**)&this->GetContext()->R12;
#elif defined(TARGET_ARM64)
            Object** firstIntReg = (Object**)&this->GetContext()->X0;
            Object** lastIntReg  = (Object**)&this->GetContext()->X28;
#elif defined(TARGET_LOONGARCH64)
            Object** firstIntReg = (Object**)&this->GetContext()->A0;
            Object** lastIntReg  = (Object**)&this->GetContext()->S8;
#elif defined(TARGET_RISCV64)
            Object** firstIntReg = (Object**)&this->GetContext()->Gp;
            Object** lastIntReg  = (Object**)&this->GetContext()->T6;
#elif defined(TARGET_WASM)
            Object** firstIntReg = nullptr;
            Object** lastIntReg  = nullptr;
#else
            _ASSERTE(!"nyi for platform");
#endif
            for (Object** ppObj = firstIntReg; ppObj <= lastIntReg; ppObj++)
            {
                fn(ppObj, sc, GC_CALL_INTERIOR | GC_CALL_PINNED);
            }
        }
#endif
    }

protected:
    PTR_CONTEXT m_Regs;

    friend struct cdac_data<ResumableFrame>;
};

template<>
struct cdac_data<ResumableFrame>
{
    static constexpr size_t TargetContextPtr = offsetof(ResumableFrame, m_Regs);
};


//-----------------------------------------------------------------------------
// RedirectedThreadFrame
//-----------------------------------------------------------------------------

class RedirectedThreadFrame : public ResumableFrame
{
public:
#ifndef DACCESS_COMPILE
    RedirectedThreadFrame(T_CONTEXT *regs) : ResumableFrame(FrameIdentifier::RedirectedThreadFrame, regs) {
        LIMITED_METHOD_CONTRACT;
    }

    void ExceptionUnwind_Impl();
#endif
};

typedef DPTR(RedirectedThreadFrame) PTR_RedirectedThreadFrame;

inline BOOL ISREDIRECTEDTHREAD(Thread * thread)
{
    WRAPPER_NO_CONTRACT;
    return (thread->GetFrame() != FRAME_TOP &&
            thread->GetFrame()->GetFrameIdentifier() ==
            FrameIdentifier::RedirectedThreadFrame);
}

inline T_CONTEXT * GETREDIRECTEDCONTEXT(Thread * thread)
{
    WRAPPER_NO_CONTRACT;
    _ASSERTE(ISREDIRECTEDTHREAD(thread));
    return dac_cast<PTR_RedirectedThreadFrame>(thread->GetFrame())->GetContext();
}

//------------------------------------------------------------------------
#else // FEATURE_HIJACK
//------------------------------------------------------------------------

inline BOOL ISREDIRECTEDTHREAD(Thread * thread) { LIMITED_METHOD_CONTRACT; return FALSE; }
inline CONTEXT * GETREDIRECTEDCONTEXT(Thread * thread) { LIMITED_METHOD_CONTRACT; return (CONTEXT*) NULL; }

//------------------------------------------------------------------------
#endif // FEATURE_HIJACK
//------------------------------------------------------------------------
// This frame represents a transition from one or more nested frameless
// method calls to either a EE runtime helper function or a framed method.
// Because most stackwalks from the EE start with a full-fledged frame,
// anything but the most trivial call into the EE has to push this
// frame in order to prevent the frameless methods inbetween from
// getting lost.
//------------------------------------------------------------------------

typedef DPTR(class TransitionFrame) PTR_TransitionFrame;

class TransitionFrame : public Frame
{
#ifndef DACCESS_COMPILE
protected:
    TransitionFrame(FrameIdentifier frameIdentifier) : Frame(frameIdentifier) {
        LIMITED_METHOD_CONTRACT;
    }
#endif

public:

    TADDR GetTransitionBlock();
    BOOL SuppressParamTypeArg();

    // DACCESS: GetReturnAddressPtr should return the
    // target address of the return address in the frame.
    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetTransitionBlock() + TransitionBlock::GetOffsetOfReturnAddress();
    }

    //---------------------------------------------------------------
    // Get the "this" object.
    //---------------------------------------------------------------
    OBJECTREF GetThis()
    {
        WRAPPER_NO_CONTRACT;
        Object* obj = PTR_Object(*PTR_TADDR(GetAddrOfThis()));
        return ObjectToOBJECTREF(obj);
    }

    PTR_OBJECTREF GetThisPtr()
    {
        WRAPPER_NO_CONTRACT;
        return PTR_OBJECTREF(GetAddrOfThis());
    }

    //---------------------------------------------------------------
    // Get the extra info for shared generic code.
    //---------------------------------------------------------------
    PTR_VOID GetParamTypeArg();

    //---------------------------------------------------------------
    // Gets value indicating whether the generic parameter type
    // argument should be suppressed.
    //---------------------------------------------------------------
    BOOL SuppressParamTypeArg_Impl()
    {
        return FALSE;
    }

protected:  // we don't want people using this directly
    //---------------------------------------------------------------
    // Get the address of the "this" object. WARNING!!! Whether or not "this"
    // is gc-protected is depends on the frame type!!!
    //---------------------------------------------------------------
    TADDR GetAddrOfThis();

public:
    //---------------------------------------------------------------
    // For vararg calls, return cookie.
    //---------------------------------------------------------------
    VASigCookie *GetVASigCookie();

    CalleeSavedRegisters *GetCalleeSavedRegisters()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return dac_cast<PTR_CalleeSavedRegisters>(
            GetTransitionBlock() + TransitionBlock::GetOffsetOfCalleeSavedRegisters());
    }

    ArgumentRegisters *GetArgumentRegisters()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return dac_cast<PTR_ArgumentRegisters>(
            GetTransitionBlock() + TransitionBlock::GetOffsetOfArgumentRegisters());
    }

    TADDR GetSP()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return GetTransitionBlock() + sizeof(TransitionBlock);
    }

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);
#ifdef TARGET_X86
    void UpdateRegDisplayHelper(const PREGDISPLAY, UINT cbStackPop);
#endif

#if defined (_DEBUG) && !defined (DACCESS_COMPILE)
    BOOL Protects_Impl(OBJECTREF *ppORef);
#endif //defined (_DEBUG) && defined (DACCESS_COMPILE)

    // For use by classes deriving from FramedMethodFrame.
    void PromoteCallerStack(promote_func* fn, ScanContext* sc);

    void PromoteCallerStackHelper(promote_func* fn, ScanContext* sc,
        MethodDesc * pMD, MetaSig *pmsig);

    void PromoteCallerStackUsingGCRefMap(promote_func* fn, ScanContext* sc, PTR_BYTE pGCRefMap);

#ifdef TARGET_X86
    UINT CbStackPopUsingGCRefMap(PTR_BYTE pGCRefMap);
#endif
};

//-----------------------------------------------------------------------
// TransitionFrames for exceptions
//-----------------------------------------------------------------------

// The define USE_FEF controls how this class is used.  Look for occurrences
//  of USE_FEF.
typedef DPTR(class FaultingExceptionFrame) PTR_FaultingExceptionFrame;

class FaultingExceptionFrame : public Frame
{
    friend class CheckAsmOffsets;

#ifndef FEATURE_EH_FUNCLETS
#ifdef TARGET_X86
    DWORD                   m_Esp;
    CalleeSavedRegisters    m_regs;
    TADDR                   m_ReturnAddress;
#else  // TARGET_X86
    #error "Unsupported architecture"
#endif // TARGET_X86
#else // FEATURE_EH_FUNCLETS
    BOOL                    m_fFilterExecuted;  // Flag for FirstCallToHandler
    TADDR                   m_ReturnAddress;
    T_CONTEXT               m_ctx;
#endif // !FEATURE_EH_FUNCLETS

#ifdef TARGET_AMD64
    TADDR                   m_SSP;
#endif

public:
#ifndef DACCESS_COMPILE
    FaultingExceptionFrame() : Frame(FrameIdentifier::FaultingExceptionFrame) {
        LIMITED_METHOD_CONTRACT;
    }
#endif

    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_HOST_MEMBER_TADDR(FaultingExceptionFrame, this, m_ReturnAddress);
    }

    void Init(T_CONTEXT *pContext);
    void InitAndLink(T_CONTEXT *pContext);

    Interception GetInterception_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_EXCEPTION;
    }

    unsigned GetFrameAttribs_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
#ifdef FEATURE_EH_FUNCLETS
        return FRAME_ATTR_EXCEPTION | (!!(m_ctx.ContextFlags & CONTEXT_EXCEPTION_ACTIVE) ? FRAME_ATTR_FAULTED : 0);
#else
        return FRAME_ATTR_EXCEPTION | FRAME_ATTR_FAULTED;
#endif
    }

#ifndef FEATURE_EH_FUNCLETS
    CalleeSavedRegisters *GetCalleeSavedRegisters()
    {
#ifdef TARGET_X86
        LIMITED_METHOD_DAC_CONTRACT;
        return &m_regs;
#else
        PORTABILITY_ASSERT("GetCalleeSavedRegisters");
#endif // TARGET_X86
    }
#endif // FEATURE_EH_FUNCLETS

#ifdef FEATURE_EH_FUNCLETS
    T_CONTEXT *GetExceptionContext ()
    {
        LIMITED_METHOD_CONTRACT;
        return &m_ctx;
    }

    BOOL * GetFilterExecutedFlag()
    {
        LIMITED_METHOD_CONTRACT;
        return &m_fFilterExecuted;
    }
#endif // FEATURE_EH_FUNCLETS

#ifdef TARGET_AMD64
    void SetSSP(TADDR value)
    {
        m_SSP = value;
    }
#endif

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);

    friend struct ::cdac_data<FaultingExceptionFrame>;
};

template<>
struct cdac_data<FaultingExceptionFrame>
{
#ifdef FEATURE_EH_FUNCLETS
    static constexpr size_t TargetContext = offsetof(FaultingExceptionFrame, m_ctx);
#endif // FEATURE_EH_FUNCLETS
};

typedef DPTR(class SoftwareExceptionFrame) PTR_SoftwareExceptionFrame;

class SoftwareExceptionFrame : public Frame
{
    TADDR                           m_ReturnAddress;
#if !defined(TARGET_X86) || defined(FEATURE_EH_FUNCLETS)
    T_KNONVOLATILE_CONTEXT_POINTERS m_ContextPointers;
#endif
    // This T_CONTEXT field needs to be the last field in the class because it is a
    // different size between Linux (pal.h) and the Windows cross-DAC (winnt.h).
    T_CONTEXT                       m_Context;

public:
#ifndef DACCESS_COMPILE
    SoftwareExceptionFrame() : Frame(FrameIdentifier::SoftwareExceptionFrame) {
        LIMITED_METHOD_CONTRACT;
    }

#ifdef TARGET_X86
    void UpdateContextFromTransitionBlock(TransitionBlock *pTransitionBlock);
#endif
#endif

    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_HOST_MEMBER_TADDR(SoftwareExceptionFrame, this, m_ReturnAddress);
    }

#ifndef DACCESS_COMPILE
    void Init();
    void InitAndLink(Thread *pThread);
#endif

    Interception GetInterception_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_EXCEPTION;
    }

    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_InternalCall;
    }

    unsigned GetFrameAttribs_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return FRAME_ATTR_EXCEPTION;
    }

    T_CONTEXT* GetContext()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return &m_Context;
    }

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);

    friend struct ::cdac_data<SoftwareExceptionFrame>;
};
template<>
struct cdac_data<SoftwareExceptionFrame>
{
    static constexpr size_t TargetContext = offsetof(SoftwareExceptionFrame, m_Context);
    static constexpr size_t ReturnAddress = offsetof(SoftwareExceptionFrame, m_ReturnAddress);
};

//-----------------------------------------------------------------------
// Frame for debugger function evaluation
//
// This frame holds a ptr to a DebuggerEval object which contains a copy
// of the thread's context at the time it was hijacked for the func
// eval.
//
// UpdateRegDisplay updates all registers inthe REGDISPLAY, not just
// the callee saved registers, because we can hijack for a func eval
// at any point in a thread's execution.
//
//-----------------------------------------------------------------------

#ifdef DEBUGGING_SUPPORTED
class DebuggerEval;
typedef DPTR(class DebuggerEval) PTR_DebuggerEval;

class FuncEvalFrame : public Frame
{
    TADDR           m_ReturnAddress;
    PTR_DebuggerEval m_pDebuggerEval;

    BOOL            m_showFrame;

public:
#ifndef DACCESS_COMPILE
    FuncEvalFrame(DebuggerEval *pDebuggerEval, TADDR returnAddress, BOOL showFrame) : Frame(FrameIdentifier::FuncEvalFrame)
    {
        LIMITED_METHOD_CONTRACT;
        m_pDebuggerEval = pDebuggerEval;
        m_ReturnAddress = returnAddress;
        m_showFrame = showFrame;
    }
#endif

    BOOL IsTransitionToNativeFrame_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return FALSE;
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_FUNC_EVAL;
    }

    unsigned GetFrameAttribs_Impl();

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);

    DebuggerEval * GetDebuggerEval();

    TADDR GetReturnAddressPtr_Impl();

    /*
     * ShowFrame
     *
     * Returns if this frame should be returned as part of a stack trace to a debugger or not.
     *
     */
    BOOL ShowFrame()
    {
        LIMITED_METHOD_CONTRACT;

        return m_showFrame;
    }

    friend struct cdac_data<FuncEvalFrame>;
};

template<>
struct cdac_data<FuncEvalFrame>
{
    static constexpr size_t DebuggerEvalPtr = offsetof(FuncEvalFrame, m_pDebuggerEval);
};

typedef DPTR(FuncEvalFrame) PTR_FuncEvalFrame;
#endif // DEBUGGING_SUPPORTED

class FramedMethodFrame : public TransitionFrame
{
    TADDR m_pTransitionBlock;

protected:
    PTR_MethodDesc m_pMD;

public:
#ifndef DACCESS_COMPILE
    FramedMethodFrame(FrameIdentifier frameIdentifier, TransitionBlock * pTransitionBlock, MethodDesc * pMD)
        : TransitionFrame(frameIdentifier), m_pTransitionBlock(dac_cast<TADDR>(pTransitionBlock)), m_pMD(pMD)
    {
        LIMITED_METHOD_CONTRACT;
    }
#endif // DACCESS_COMPILE

    TADDR GetTransitionBlock_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pTransitionBlock;
    }

    MethodDesc *GetFunction_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pMD;
    }

#ifndef DACCESS_COMPILE
    void SetFunction(MethodDesc *pMD)
    {
        CONTRACTL
        {
            NOTHROW;
            GC_NOTRIGGER;
            MODE_COOPERATIVE; // Frame MethodDesc should be always updated in cooperative mode to avoid racing with GC stackwalk
        }
        CONTRACTL_END;

        m_pMD = pMD;
    }
#endif

    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_M2U; // we can safely cast to a FramedMethodFrame
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_CALL;
    }

#ifdef COM_STUBS_SEPARATE_FP_LOCATIONS
    static int GetFPArgOffset(int iArg)
    {
#ifdef TARGET_AMD64
        // Floating point spill area is between return value and transition block for frames that need it
        // (see code:CLRToCOMMethodFrame)
        return -(4 * 0x10 /* floating point args */ + 0x8 /* alignment pad */ + TransitionBlock::GetNegSpaceSize()) + (iArg * 0x10);
#endif
    }
#endif

    //
    // GetReturnObjectPtr and GetReturnValuePtr are only valid on frames
    // that allocate
    //
    PTR_PTR_Object GetReturnObjectPtr()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_PTR_Object(GetReturnValuePtr());
    }

    // Get return value address
    PTR_VOID GetReturnValuePtr()
    {
        LIMITED_METHOD_DAC_CONTRACT;
#ifdef COM_STUBS_SEPARATE_FP_LOCATIONS
        TADDR p = GetTransitionBlock() + GetFPArgOffset(0);
#else
        TADDR p = GetTransitionBlock() - TransitionBlock::GetNegSpaceSize();
#endif
        // Return value is right before the transition block (or floating point spill area on AMD64) for frames that need it
        // (see code:CLRToCOMMethodFrame)
#ifdef ENREGISTERED_RETURNTYPE_MAXSIZE
        p -= ENREGISTERED_RETURNTYPE_MAXSIZE;
#else
        p -= sizeof(ARG_SLOT);
#endif
        return dac_cast<PTR_VOID>(p);
    }

    friend struct cdac_data<FramedMethodFrame>;
};

template<>
struct cdac_data<FramedMethodFrame>
{
    static constexpr size_t TransitionBlockPtr = offsetof(FramedMethodFrame, m_pTransitionBlock);
};

#ifdef FEATURE_COMINTEROP

//-----------------------------------------------------------------------
// Transition frame from unmanaged to managed
//-----------------------------------------------------------------------

class UnmanagedToManagedFrame : public Frame
{
    friend class CheckAsmOffsets;

protected:
#ifndef DACCESS_COMPILE
    UnmanagedToManagedFrame(FrameIdentifier frameIdentifier) : Frame(frameIdentifier)
    {
        LIMITED_METHOD_CONTRACT;
    }
#endif // DACCESS_COMPILE

public:

    // DACCESS: GetReturnAddressPtr should return the
    // target address of the return address in the frame.
    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_HOST_MEMBER_TADDR(UnmanagedToManagedFrame, this,
                                     m_ReturnAddress);
    }

    PCODE GetReturnAddress_Impl();

    // Retrieves pointer to the lowest-addressed argument on
    // the stack. Depending on the calling convention, this
    // may or may not be the first argument.
    TADDR GetPointerToArguments()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return dac_cast<TADDR>(this) + GetOffsetOfArgs();
    }

    // Exposes an offset for stub generation.
    static BYTE GetOffsetOfArgs()
    {
        LIMITED_METHOD_DAC_CONTRACT;
#if defined(TARGET_ARM) || defined(TARGET_ARM64) || defined(TARGET_RISCV64)
        size_t ofs = offsetof(UnmanagedToManagedFrame, m_argumentRegisters);
#else
        size_t ofs = sizeof(UnmanagedToManagedFrame);
#endif
        _ASSERTE(FitsInI1(ofs));
        return (BYTE)ofs;
    }

    // depends on the sub frames to return approp. type here
    TADDR GetDatum()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pvDatum;
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_ENTRY;
    }

    //------------------------------------------------------------------------
    // For the debugger.
    //------------------------------------------------------------------------
    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_U2M;
    }

    //------------------------------------------------------------------------
    // Performs cleanup on an exception unwind
    //------------------------------------------------------------------------
#ifndef DACCESS_COMPILE
    void ExceptionUnwind_Impl();
#endif

protected:
    TADDR           m_pvDatum;        // type depends on the sub class

#if defined(TARGET_X86)
    CalleeSavedRegisters  m_calleeSavedRegisters;
    TADDR           m_ReturnAddress;
#elif defined(TARGET_ARM)
    TADDR           m_R11; // R11 chain
    TADDR           m_ReturnAddress;
    ArgumentRegisters m_argumentRegisters;
#elif defined (TARGET_ARM64)
    TADDR           m_fp;
    TADDR           m_ReturnAddress;
    TADDR           m_x8; // ret buff arg
    ArgumentRegisters m_argumentRegisters;
#elif defined (TARGET_LOONGARCH64) || defined (TARGET_RISCV64)
    TADDR           m_fp;
    TADDR           m_ReturnAddress;
    ArgumentRegisters m_argumentRegisters;
#else
    TADDR           m_ReturnAddress;  // return address into unmanaged code
#endif
};

//------------------------------------------------------------------------
// This frame represents a transition from COM to CLR
//------------------------------------------------------------------------

class ComMethodFrame : public UnmanagedToManagedFrame
{
public:
#ifndef DACCESS_COMPILE
    ComMethodFrame(FrameIdentifier frameIdentifier = FrameIdentifier::ComMethodFrame) : UnmanagedToManagedFrame(FrameIdentifier::ComMethodFrame)
    {
        LIMITED_METHOD_CONTRACT;
    }
#endif // DACCESS_COMPILE

#ifdef TARGET_X86
    // Return the # of stack bytes pushed by the unmanaged caller.
    UINT GetNumCallerStackBytes();
#endif

    PTR_ComCallMethodDesc GetComCallMethodDesc()
    {
        LIMITED_METHOD_CONTRACT;
        return dac_cast<PTR_ComCallMethodDesc>(m_pvDatum);
    }

#ifndef DACCESS_COMPILE
    static void DoSecondPassHandlerCleanup(Frame * pCurFrame);
#endif // !DACCESS_COMPILE
};

typedef DPTR(class ComMethodFrame) PTR_ComMethodFrame;

//------------------------------------------------------------------------
// This represents a generic call from CLR to COM
//------------------------------------------------------------------------

typedef DPTR(class CLRToCOMMethodFrame) PTR_CLRToCOMMethodFrame;

class CLRToCOMMethodFrame : public FramedMethodFrame
{
public:
    CLRToCOMMethodFrame(TransitionBlock * pTransitionBlock, MethodDesc * pMethodDesc);

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc);

    BOOL IsTransitionToNativeFrame_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return TRUE;
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_EXIT;
    }

    void GetUnmanagedCallSite_Impl(TADDR* ip,
                              TADDR* returnIP,
                              TADDR* returnSP);

    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch,
                    TraceDestination *trace, REGDISPLAY *regs);
};

#endif // FEATURE_COMINTEROP

//------------------------------------------------------------------------
// This represents a call from a helper to GetILStubForCalli
//------------------------------------------------------------------------

typedef DPTR(class PInvokeCalliFrame) PTR_PInvokeCalliFrame;

class PInvokeCalliFrame : public FramedMethodFrame
{
    PTR_VASigCookie m_pVASigCookie;
    PCODE m_pUnmanagedTarget;

public:
    PInvokeCalliFrame(TransitionBlock * pTransitionBlock, VASigCookie * pVASigCookie, PCODE pUnmanagedTarget);

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc)
    {
        WRAPPER_NO_CONTRACT;
        FramedMethodFrame::GcScanRoots_Impl(fn, sc);
        PromoteCallerStack(fn, sc);
    }

    void PromoteCallerStack(promote_func* fn, ScanContext* sc);

    // not a method
    MethodDesc *GetFunction_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return NULL;
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_INTERCEPTION;
    }

    PCODE GetPInvokeCalliTarget()
    {
        LIMITED_METHOD_CONTRACT;
        return m_pUnmanagedTarget;
    }

    PTR_VASigCookie GetVASigCookie()
    {
        LIMITED_METHOD_CONTRACT;
        return m_pVASigCookie;
    }

#ifdef TARGET_X86
    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);
#endif // TARGET_X86

    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch,
                    TraceDestination *trace, REGDISPLAY *regs)
    {
        WRAPPER_NO_CONTRACT;

        trace->InitForUnmanaged(GetPInvokeCalliTarget());
        return TRUE;
    }
};

// Some context-related forwards.
#ifdef FEATURE_HIJACK
//------------------------------------------------------------------------
// This frame represents a hijacked return.  If we crawl back through it,
// it gets us back to where the return should have gone (and eventually will
// go).
//------------------------------------------------------------------------

typedef DPTR(class HijackFrame) PTR_HijackFrame;

class HijackFrame : public Frame
{
public:
    // DACCESS: GetReturnAddressPtr should return the
    // target address of the return address in the frame.
    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_HOST_MEMBER_TADDR(HijackFrame, this,
                                     m_ReturnAddress);
    }

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);

#ifdef TARGET_X86
    // On x86 we need to specialcase return values
    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc);
#else
    // On non-x86 platforms HijackFrame is just a more compact form of a resumable
    // frame with main difference that OnHijackTripThread captures just the registers
    // that can possibly contain GC roots.
    // The regular reporting of a top frame will report everything that is live
    // after the call as specified in GC info, thus we do not need to worry about
    // return values.
    unsigned GetFrameAttribs_Impl() {
        LIMITED_METHOD_DAC_CONTRACT;
        return FRAME_ATTR_RESUMABLE;    // Treat the next frame as the top frame.
    }
#endif

    // HijackFrames are created by trip functions. See OnHijackTripThread()
    // They are real C++ objects on the stack.
    // So, it's a public function -- but that doesn't mean you should make some.
    HijackFrame(LPVOID returnAddress, Thread *thread, HijackArgs *args);

protected:

    TADDR               m_ReturnAddress;
    PTR_Thread          m_Thread;
    DPTR(HijackArgs)    m_Args;

    friend struct ::cdac_data<HijackFrame>;
};

template<>
struct cdac_data<HijackFrame>
{
    static constexpr size_t ReturnAddress = offsetof(HijackFrame, m_ReturnAddress);
    static constexpr size_t HijackArgsPtr = offsetof(HijackFrame, m_Args);
};

#endif // FEATURE_HIJACK

//------------------------------------------------------------------------
// This represents a call to a method prestub. Because the prestub
// can do gc and throw exceptions while building the replacement
// stub, we need this frame to keep things straight.
//------------------------------------------------------------------------

typedef DPTR(class PrestubMethodFrame) PTR_PrestubMethodFrame;

class PrestubMethodFrame : public FramedMethodFrame
{
public:
    PrestubMethodFrame(TransitionBlock * pTransitionBlock, MethodDesc * pMD);

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc)
    {
        WRAPPER_NO_CONTRACT;
        FramedMethodFrame::GcScanRoots_Impl(fn, sc);
        PromoteCallerStack(fn, sc);
    }

    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch,
                    TraceDestination *trace, REGDISPLAY *regs);

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_INTERCEPTION;
    }

    // Our base class is an M2U TransitionType; but we're not. So override and set us back to None.
    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_NONE;
    }

    Interception GetInterception_Impl();
};

//------------------------------------------------------------------------
// This represents a call into the virtual call stub manager
// Because the stub manager can do gc and throw exceptions while
// building the resolve and dispatch stubs and needs to communicate
// if we need to setup for a methodDesc call or do a direct call
// we need this frame to keep things straight.
//------------------------------------------------------------------------

class StubDispatchFrame : public FramedMethodFrame
{
    // Representative MethodTable * and slot. They are used to
    // compute the MethodDesc* lazily
    PTR_MethodTable m_pRepresentativeMT;
    UINT32          m_representativeSlot;

    // Indirection cell and containing module. Used to compute pGCRefMap lazily.
    PTR_Module      m_pZapModule;
    TADDR           m_pIndirection;

    // Cached pointer to native ref data.
    PTR_BYTE        m_pGCRefMap;

public:
    StubDispatchFrame(TransitionBlock * pTransitionBlock);

    MethodDesc* GetFunction_Impl();

    // Returns this frame GC ref map if it has one
    PTR_BYTE GetGCRefMap();

#ifdef TARGET_X86
    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats = false);
    PCODE GetReturnAddress_Impl();
#endif // TARGET_X86

    PCODE GetUnadjustedReturnAddress()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return FramedMethodFrame::GetReturnAddress_Impl();
    }

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc);

#ifndef DACCESS_COMPILE
    void SetRepresentativeSlot(MethodTable * pMT, UINT32 representativeSlot)
    {
        LIMITED_METHOD_CONTRACT;

        m_pRepresentativeMT = pMT;
        m_representativeSlot = representativeSlot;
    }

    void SetCallSite(Module * pZapModule, TADDR pIndirection)
    {
        LIMITED_METHOD_CONTRACT;

        m_pZapModule = pZapModule;
        m_pIndirection = pIndirection;
    }

    void SetForNullReferenceException()
    {
        LIMITED_METHOD_CONTRACT;

        // Nothing to do. Everything is initialized in Init.
    }
#endif

    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch,
                    TraceDestination *trace, REGDISPLAY *regs);

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return TYPE_CALL;
    }

    Interception GetInterception_Impl();

    BOOL SuppressParamTypeArg_Impl()
    {
        //
        // Shared default interface methods (i.e. virtual interface methods with an implementation) require
        // an instantiation argument. But if we're in the stub dispatch frame, we haven't actually resolved
        // the method yet (we could end up in class's override of this method, for example).
        //
        // So we need to pretent that unresolved default interface methods are like any other interface
        // methods and don't have an instantiation argument.
        //
        // See code:getMethodSigInternal
        //
        assert(GetFunction()->GetMethodTable()->IsInterface());
        return TRUE;
    }

private:
    friend class VirtualCallStubManager;
};

typedef DPTR(class StubDispatchFrame) PTR_StubDispatchFrame;

typedef DPTR(class CallCountingHelperFrame) PTR_CallCountingHelperFrame;

class CallCountingHelperFrame : public FramedMethodFrame
{
public:
    CallCountingHelperFrame(TransitionBlock *pTransitionBlock, MethodDesc *pMD);

    void GcScanRoots_Impl(promote_func *fn, ScanContext *sc); // override
    BOOL TraceFrame_Impl(Thread *thread, BOOL fromPatch, TraceDestination *trace, REGDISPLAY *regs); // override

    int GetFrameType_Impl() // override
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_CALL;
    }

    Interception GetInterception_Impl() // override
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_NONE;
    }
};

//------------------------------------------------------------------------
// This represents a call from an ExternalMethodThunk or a VirtualImportThunk
// Because the resolving of the target address can do gc and/or
//  throw exceptions we need this frame to report the gc references.
//------------------------------------------------------------------------

class ExternalMethodFrame : public FramedMethodFrame
{
    // Indirection and containing module. Used to compute pGCRefMap lazily.
    PTR_Module      m_pZapModule;
    TADDR           m_pIndirection;

    // Cached pointer to native ref data.
    PTR_BYTE        m_pGCRefMap;

public:
    ExternalMethodFrame(TransitionBlock * pTransitionBlock);

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc);

    // Returns this frame GC ref map if it has one
    PTR_BYTE GetGCRefMap();

#ifndef DACCESS_COMPILE
    void SetCallSite(Module * pZapModule, TADDR pIndirection)
    {
        LIMITED_METHOD_CONTRACT;

        m_pZapModule = pZapModule;
        m_pIndirection = pIndirection;
    }
#endif

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return TYPE_CALL;
    }

    Interception GetInterception_Impl();

#ifdef TARGET_X86
    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats = false);
#endif
};

typedef DPTR(class ExternalMethodFrame) PTR_ExternalMethodFrame;

class DynamicHelperFrame : public FramedMethodFrame
{
    int m_dynamicHelperFrameFlags;

public:
    DynamicHelperFrame(TransitionBlock * pTransitionBlock, int dynamicHelperFrameFlags);

    void GcScanRoots_Impl(promote_func *fn, ScanContext* sc);

#ifdef TARGET_X86
    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats = false);
#endif

    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_InternalCall;
    }
};

typedef DPTR(class DynamicHelperFrame) PTR_DynamicHelperFrame;

#ifdef FEATURE_COMINTEROP

//------------------------------------------------------------------------
// This represents a com to CLR call method prestub.
// we need to catch exceptions etc. so this frame is not the same
// as the prestub method frame
// Note that in rare IJW cases, the immediate caller could be a managed method
// which pinvoke-inlined a call to a COM interface, which happenned to be
// implemented by a managed function via COM-interop.
//------------------------------------------------------------------------

typedef DPTR(class ComPrestubMethodFrame) PTR_ComPrestubMethodFrame;

class ComPrestubMethodFrame : public ComMethodFrame
{
    friend class CheckAsmOffsets;

public:
    // Set the vptr and GSCookie
    VOID Init();

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_INTERCEPTION;
    }

    // ComPrestubMethodFrame should return the same interception type as
    // code:PrestubMethodFrame.GetInterception.
    Interception GetInterception_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_PRESTUB;
    }

    // Our base class is an M2U TransitionType; but we're not. So override and set us back to None.
    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_NONE;
    }

    void ExceptionUnwind_Impl()
    {
    }
};

#endif // FEATURE_COMINTEROP

//------------------------------------------------------------------------
// This frame protects object references for the EE's convenience.
// This frame type actually is created from C++.
// There is a chain of GCFrames on a Thread, separate from the
// explicit frames derived from the Frame class.
//------------------------------------------------------------------------
class GCFrame
{
public:

#ifndef DACCESS_COMPILE
    //--------------------------------------------------------------------
    // This constructor pushes a new GCFrame on the GC frame chain.
    //--------------------------------------------------------------------
    GCFrame(OBJECTREF *pObjRefs, UINT numObjRefs, BOOL maybeInterior)
        : GCFrame(GetThread(), pObjRefs, numObjRefs, maybeInterior)
    {
        WRAPPER_NO_CONTRACT;
    }

    GCFrame(Thread *pThread, OBJECTREF *pObjRefs, UINT numObjRefs, BOOL maybeInterior);
    ~GCFrame();

    // Push and pop this frame from the thread's stack.
    void Push(Thread* pThread);
    void Pop();
    // Remove this frame from any position in the thread's stack
    void Remove();

#endif // DACCESS_COMPILE

    void GcScanRoots(promote_func *fn, ScanContext* sc);

#ifdef _DEBUG
    BOOL Protects(OBJECTREF *ppORef)
    {
        LIMITED_METHOD_CONTRACT;
        for (UINT i = 0; i < m_numObjRefs; i++) {
            if (ppORef == m_pObjRefs + i) {
                return TRUE;
            }
        }
        return FALSE;
    }
#endif

    PTR_GCFrame PtrNextFrame()
    {
        WRAPPER_NO_CONTRACT;
        return m_Next;
    }

private:
    PTR_GCFrame   m_Next;
    PTR_Thread    m_pCurThread;
    PTR_OBJECTREF m_pObjRefs;
    UINT          m_numObjRefs;
    BOOL          m_MaybeInterior;
};

//-----------------------------------------------------------------------------

struct ValueClassInfo;
typedef DPTR(struct ValueClassInfo) PTR_ValueClassInfo;

struct ValueClassInfo
{
    PTR_ValueClassInfo  pNext;
    PTR_MethodTable     pMT;
    PTR_VOID            pData;

    ValueClassInfo(PTR_VOID aData, PTR_MethodTable aMT, PTR_ValueClassInfo aNext)
        : pNext(aNext), pMT(aMT), pData(aData)
    {
    }
};

//-----------------------------------------------------------------------------
// ProtectValueClassFrame
//-----------------------------------------------------------------------------

typedef DPTR(class ProtectValueClassFrame) PTR_ProtectValueClassFrame;

class ProtectValueClassFrame : public Frame
{
public:
#ifndef DACCESS_COMPILE
    ProtectValueClassFrame()
        : Frame(FrameIdentifier::ProtectValueClassFrame), m_pVCInfo(NULL)
    {
        WRAPPER_NO_CONTRACT;
        Frame::Push();
    }

    ProtectValueClassFrame(Thread *pThread, ValueClassInfo *vcInfo)
        : Frame(FrameIdentifier::ProtectValueClassFrame), m_pVCInfo(vcInfo)
    {
        WRAPPER_NO_CONTRACT;
        Frame::Push(pThread);
    }
#endif

    void GcScanRoots_Impl(promote_func *fn, ScanContext *sc);

    ValueClassInfo ** GetValueClassInfoList()
    {
        LIMITED_METHOD_CONTRACT;
        return &m_pVCInfo;
    }

private:

    ValueClassInfo *m_pVCInfo;
};


#ifdef _DEBUG
BOOL IsProtectedByGCFrame(OBJECTREF *ppObjectRef);
#endif


//------------------------------------------------------------------------
// DebuggerClassInitMarkFrame is a small frame whose only purpose in
// life is to mark for the debugger that "class initialization code" is
// being run. It does nothing useful except return good values from
// GetFrameType and GetInterception.
//------------------------------------------------------------------------

typedef DPTR(class DebuggerClassInitMarkFrame) PTR_DebuggerClassInitMarkFrame;

class DebuggerClassInitMarkFrame : public Frame
{
public:

#ifndef DACCESS_COMPILE
    DebuggerClassInitMarkFrame() : Frame(FrameIdentifier::DebuggerClassInitMarkFrame)
    {
        WRAPPER_NO_CONTRACT;
        Push();
    };
#endif

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_INTERCEPTION;
    }

    Interception GetInterception_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return INTERCEPTION_CLASS_INIT;
    }
};

//------------------------------------------------------------------------
// DebuggerExitFrame is a small frame whose only purpose in
// life is to mark for the debugger that there is an exit transition on
// the stack.
//------------------------------------------------------------------------

typedef DPTR(class DebuggerExitFrame) PTR_DebuggerExitFrame;

class DebuggerExitFrame : public Frame
{
public:
#ifndef DACCESS_COMPILE
    DebuggerExitFrame() : Frame(FrameIdentifier::DebuggerExitFrame)
    {
        WRAPPER_NO_CONTRACT;
        Push();
    }
#endif

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_EXIT;
    }

    // Return information about an unmanaged call the frame
    // will make.
    // ip - the unmanaged routine which will be called
    // returnIP - the address in the stub which the unmanaged routine
    //            will return to.
    // returnSP - the location returnIP is pushed onto the stack
    //            during the call.
    //
    void GetUnmanagedCallSite_Impl(TADDR* ip,
                                      TADDR* returnIP,
                                      TADDR* returnSP)
    {
        LIMITED_METHOD_CONTRACT;
        if (ip)
            *ip = 0;

        if (returnIP)
            *returnIP = 0;

        if (returnSP)
            *returnSP = 0;
    }
};

//---------------------------------------------------------------------------------------
//
// DebuggerU2MCatchHandlerFrame is a small frame whose only purpose in life is to mark for the debugger
// that there is catch handler inside the runtime which may catch and swallow managed exceptions.  The
// debugger needs this frame to send a CatchHandlerFound (CHF) notification.  Without this frame, the
// debugger doesn't know where a managed exception is caught.
//
// Notes:
//    Currently this frame is only used in code:DispatchInfo.InvokeMember, which is an U2M transition.
//

typedef DPTR(class DebuggerU2MCatchHandlerFrame) PTR_DebuggerU2MCatchHandlerFrame;

class DebuggerU2MCatchHandlerFrame : public Frame
{
public:
#ifndef DACCESS_COMPILE
    DebuggerU2MCatchHandlerFrame(bool catchesAllExceptions) : Frame(FrameIdentifier::DebuggerU2MCatchHandlerFrame),
                                                              m_catchesAllExceptions(catchesAllExceptions)
    {
        WRAPPER_NO_CONTRACT;
        Frame::Push();
    }

    DebuggerU2MCatchHandlerFrame(Thread * pThread, bool catchesAllExceptions) : Frame(FrameIdentifier::DebuggerU2MCatchHandlerFrame),
                                                                                m_catchesAllExceptions(catchesAllExceptions)
    {
        WRAPPER_NO_CONTRACT;
        Frame::Push(pThread);
    }
#endif

    ETransitionType GetTransitionType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TT_U2M;
    }

    bool CatchesAllExceptions()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_catchesAllExceptions;
    }

private:
    // The catch handled marked by the DebuggerU2MCatchHandlerFrame catches all exceptions.
    bool m_catchesAllExceptions;
};

// Frame for the Reverse PInvoke (i.e. UnmanagedCallersOnlyAttribute).
struct ReversePInvokeFrame
{
    Thread* currentThread;
    MethodDesc* pMD;
#if defined(TARGET_X86) && defined(TARGET_WINDOWS)
#ifndef FEATURE_EH_FUNCLETS
    FrameHandlerExRecord record;
#else
    EXCEPTION_REGISTRATION_RECORD m_ExReg;
#endif
#endif
};

//------------------------------------------------------------------------
// This frame is pushed by any JIT'ted method that contains one or more
// inlined PInvoke calls. Note that the JIT'ted method keeps it pushed
// the whole time to amortize the pushing cost across the entire method.
//------------------------------------------------------------------------

typedef DPTR(class InlinedCallFrame) PTR_InlinedCallFrame;

class InlinedCallFrame : public Frame
{
public:

#ifndef DACCESS_COMPILE
#ifdef FEATURE_INTERPRETER
    InlinedCallFrame() : Frame(FrameIdentifier::InlinedCallFrame)
    {
        WRAPPER_NO_CONTRACT;
        m_Datum = NULL;
        m_pCallSiteSP = NULL;
        m_pCallerReturnAddress = 0;
        m_pCalleeSavedFP = 0;
        m_pThread = NULL;
    }
#endif // FEATURE_INTERPRETER
#endif // DACCESS_COMPILE

    MethodDesc *GetFunction_Impl()
    {
        WRAPPER_NO_CONTRACT;
        if (FrameHasActiveCall(this) && HasFunction())
            // Mask off marker bits
            return PTR_MethodDesc((dac_cast<TADDR>(m_Datum) & ~(sizeof(TADDR) - 1)));
        else
            return NULL;
    }

    BOOL HasFunction()
    {
        WRAPPER_NO_CONTRACT;

#ifdef HOST_64BIT
        // See code:GenericPInvokeCalliHelper
        return ((m_Datum != NULL) && !(dac_cast<TADDR>(m_Datum) & 0x1));
#else // HOST_64BIT
        return ((dac_cast<TADDR>(m_Datum) & ~0xffff) != 0);
#endif // HOST_64BIT
    }

    // Retrieves the return address into the code that called out
    // to managed code
    TADDR GetReturnAddressPtr_Impl()
    {
        WRAPPER_NO_CONTRACT;

        if (FrameHasActiveCall(this))
            return PTR_HOST_MEMBER_TADDR(InlinedCallFrame, this,
                                            m_pCallerReturnAddress);
        else
            return 0;
    }

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        WRAPPER_NO_CONTRACT;
        return FrameHasActiveCall(this);
    }

    // Given a methodDesc representing an ILStub for a pinvoke call,
    // this method will return the MethodDesc for the actual interop
    // method if the current InlinedCallFrame is inactive.
    PTR_MethodDesc GetActualInteropMethodDesc()
    {
        // The VM instructs the JIT to publish the secret stub arg at the end
        // of the InlinedCallFrame structure when it exists.
        TADDR addr = dac_cast<TADDR>(this) + sizeof(InlinedCallFrame);
        return PTR_MethodDesc(*PTR_TADDR(addr));
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY, bool updateFloats = false);

    // m_Datum contains MethodDesc ptr or
    // - on 64 bit host: CALLI target address (if lowest bit is set)
    // - on windows x86 host: argument stack size (if value is <64k)
    // When m_Datum contains MethodDesc ptr, then on other than windows x86 host
    // - bit 1 set indicates invoking new exception handling helpers
    // - bit 2 indicates CallCatchFunclet or CallFinallyFunclet
    // See code:HasFunction.
    PTR_PInvokeMethodDesc   m_Datum;

    // X86: ESP after pushing the outgoing arguments, and just before calling
    // out to unmanaged code.
    // Other platforms: the field stays set throughout the declaring method.
    PTR_VOID             m_pCallSiteSP;

    // EIP where the unmanaged call will return to. This will be a pointer into
    // the code of the managed frame which has the InlinedCallFrame
    // This is set to NULL in the method prolog. It gets set just before the
    // call to the target and reset back to NULL after the stop-for-GC check
    // following the call.
    TADDR                m_pCallerReturnAddress;

    // This is used only for EBP. Hence, a stackwalk will miss the other
    // callee-saved registers for the method with the InlinedCallFrame.
    // To prevent GC-holes, we do not keep any GC references in callee-saved
    // registers across an PInvoke call.
    TADDR                m_pCalleeSavedFP;

    // This field is used to cache the current thread object where this frame is
    // executing. This is especially helpful on Unix platforms for the PInvoke assembly
    // stubs, since there is no easy way to inline an implementation of GetThread.
    PTR_VOID             m_pThread;

#ifdef TARGET_ARM
    // Store the value of SP after prolog to ensure we can unwind functions that use
    // stackalloc. In these functions, the m_pCallSiteSP can already be augmented by
    // the stackalloc size, which is variable.
    TADDR               m_pSPAfterProlog;
#endif // TARGET_ARM

public:
    //---------------------------------------------------------------
    // Expose key offsets and values for stub generation.
    //---------------------------------------------------------------

    static void GetEEInfo(CORINFO_EE_INFO::InlinedCallFrameInfo * pEEInfo);

    // Is the specified frame an InlinedCallFrame which has an active call
    // inside it right now?
    static BOOL FrameHasActiveCall(Frame *pFrame)
    {
        WRAPPER_NO_CONTRACT;
        SUPPORTS_DAC;
        return pFrame &&
            pFrame != FRAME_TOP &&
            FrameIdentifier::InlinedCallFrame == pFrame->GetFrameIdentifier() &&
            dac_cast<TADDR>(dac_cast<PTR_InlinedCallFrame>(pFrame)->m_pCallerReturnAddress) != 0;
    }

    // Marks the frame as inactive.
    void Reset()
    {
        m_pCallerReturnAddress = 0;
    }

    int GetFrameType_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return TYPE_EXIT;
    }

    BOOL IsTransitionToNativeFrame_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return TRUE;
    }

    PTR_VOID GetCallSiteSP()
    {
        LIMITED_METHOD_CONTRACT;
        return m_pCallSiteSP;
    }

    TADDR GetCalleeSavedFP()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return m_pCalleeSavedFP;
    }

    // Set the vptr and GSCookie
    VOID Init();
};

// TODO [DAVBR]: For the full fix for VsWhidbey 450273, this
// may be uncommented once isLegalManagedCodeCaller works properly
// with non-return address inputs, and with non-DEBUG builds
//bool isLegalManagedCodeCaller(TADDR retAddr);
bool isRetAddr(TADDR retAddr, TADDR* whereCalled);

#if defined(TARGET_X86) && !defined(UNIX_X86_ABI)
//------------------------------------------------------------------------
// This frame is used as padding for virtual stub dispatch tailcalls.
// When A calls B via virtual stub dispatch, the stub dispatch stub resolves
// the target code for B and jumps to it. If A wants to do a tail call,
// it does not get a chance to unwind its frame since the virtual stub dispatch
// stub is not set up to return the address of the target code (rather
// than just jumping to it).
// To do a tail call, A calls JIT_TailCall, which unwinds A's frame
// and sets up a TailCallFrame. It then calls the stub dispatch stub
// which disassembles the caller (JIT_TailCall, in this case) to get some information,
// resolves the target code for B, and then jumps to B.
// If B also does a virtual stub dispatch tail call, then we reuse the
// existing TailCallFrame instead of setting up a second one.
//
// We could eliminate TailCallFrame if we factor the VSD stub to return
// the target code address. This is currently not a very important scenario
// as tail calls on interface calls are uncommon.
//------------------------------------------------------------------------

typedef DPTR(class TailCallFrame) PTR_TailCallFrame;

class TailCallFrame : public Frame
{
    TADDR           m_CallerAddress;    // the address the tailcall was initiated from
    CalleeSavedRegisters    m_regs;     // callee saved registers - the stack walk assumes that all non-JIT frames have them
    TADDR           m_ReturnAddress;    // the return address of the tailcall

public:
    static TailCallFrame* FindTailCallFrame(Frame* pFrame)
    {
        LIMITED_METHOD_CONTRACT;
        // loop through the frame chain
        while (pFrame->GetFrameIdentifier() != FrameIdentifier::TailCallFrame)
            pFrame = pFrame->m_Next;
        return dac_cast<PTR_TailCallFrame>(pFrame);
    }

    TADDR GetCallerAddress()
    {
        LIMITED_METHOD_CONTRACT;
        return m_CallerAddress;
    }

    TADDR GetReturnAddressPtr_Impl()
    {
        LIMITED_METHOD_DAC_CONTRACT;
        return PTR_HOST_MEMBER_TADDR(TailCallFrame, this,
                                        m_ReturnAddress);
    }

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        return TRUE;
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats = false);

    friend struct cdac_data<TailCallFrame>;
};

template<>
struct cdac_data<TailCallFrame>
{
    static constexpr size_t CalleeSavedRegisters = offsetof(TailCallFrame, m_regs);
    static constexpr size_t ReturnAddress = offsetof(TailCallFrame, m_ReturnAddress);
};

#endif // TARGET_X86 && !UNIX_X86_ABI

//------------------------------------------------------------------------
// ExceptionFilterFrame is a small frame whose only purpose in
// life is to set SHADOW_SP_FILTER_DONE during unwind from exception filter.
//------------------------------------------------------------------------

typedef DPTR(class ExceptionFilterFrame) PTR_ExceptionFilterFrame;

class ExceptionFilterFrame : public Frame
{
    size_t* m_pShadowSP;

public:
#ifndef DACCESS_COMPILE
    ExceptionFilterFrame(size_t* pShadowSP) : Frame(FrameIdentifier::ExceptionFilterFrame)
    {
        WRAPPER_NO_CONTRACT;
        m_pShadowSP = pShadowSP;
        Push();
    }

    void Pop()
    {
        // Nothing to do here.
        WRAPPER_NO_CONTRACT;
        SetFilterDone();
        Frame::Pop();
    }

    void SetFilterDone()
    {
        LIMITED_METHOD_CONTRACT;

        // Mark the filter as having completed
        if (m_pShadowSP)
        {
            // Make sure that CallJitEHFilterHelper marked us as being in the filter.
            _ASSERTE(*m_pShadowSP & ICodeManager::SHADOW_SP_IN_FILTER);
            *m_pShadowSP |= ICodeManager::SHADOW_SP_FILTER_DONE;
        }
    }
#endif
};

#ifdef FEATURE_INTERPRETER
struct InterpMethodContextFrame;
typedef DPTR(struct InterpMethodContextFrame) PTR_InterpMethodContextFrame;

typedef DPTR(class InterpreterFrame) PTR_InterpreterFrame;

class InterpreterFrame : public FramedMethodFrame
{
    static void DummyFuncletCaller() {}
public:

    // This is a special value representing a caller of the first interpreter frame
    // in a block of interpreter frames belonging to a single InterpreterFrame.
    static TADDR DummyCallerIP;

#ifndef DACCESS_COMPILE
    InterpreterFrame(TransitionBlock* pTransitionBlock, InterpMethodContextFrame* pContextFrame)
        : FramedMethodFrame(FrameIdentifier::InterpreterFrame, pTransitionBlock, NULL),
        m_pTopInterpMethodContextFrame(pContextFrame),
        m_isFaulting(false)
#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
        , m_SSP(0)
#endif
    {
        WRAPPER_NO_CONTRACT;
        Push();
    }

    void SetTopInterpMethodContextFrame(InterpMethodContextFrame* pTopInterpMethodContextFrame)
    {
        m_pTopInterpMethodContextFrame = pTopInterpMethodContextFrame;
    }

#endif // DACCESS_COMPILE

    BOOL NeedsUpdateRegDisplay_Impl()
    {
        LIMITED_METHOD_CONTRACT;
        return GetTransitionBlock() != 0;
    }

    PCODE GetReturnAddressPtr_Impl()
    {
        WRAPPER_NO_CONTRACT;
        if (GetTransitionBlock() == 0)
            return 0;

        return FramedMethodFrame::GetReturnAddressPtr_Impl();
    }

    void UpdateRegDisplay_Impl(const PREGDISPLAY pRD, bool updateFloats = false);
#ifndef DACCESS_COMPILE
    void ExceptionUnwind_Impl();
#endif

    PTR_InterpMethodContextFrame GetTopInterpMethodContextFrame();

    void SetContextToInterpMethodContextFrame(T_CONTEXT * pContext);

#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
    void SetInterpExecMethodSSP(TADDR ssp)
    {
        LIMITED_METHOD_CONTRACT;
        m_SSP = ssp;
    }

    TADDR GetInterpExecMethodSSP()
    {
        LIMITED_METHOD_CONTRACT;
        return m_SSP;
    }
#endif // HOST_AMD64 && HOST_WINDOWS

    void SetIsFaulting(bool isFaulting)
    {
        LIMITED_METHOD_CONTRACT;
        m_isFaulting = isFaulting;
    }

private:
    // The last known topmost interpreter frame in the InterpExecMethod belonging to
    // this InterpreterFrame.
    PTR_InterpMethodContextFrame m_pTopInterpMethodContextFrame;
    // Set to true to indicate that the topmost interpreted frame has thrown an exception
    bool m_isFaulting;
#if defined(HOST_AMD64) && defined(HOST_WINDOWS)
    // Saved SSP of the InterpExecMethod for resuming after catch into interpreter frames.
    TADDR m_SSP;
#endif // HOST_AMD64 && HOST_WINDOWS
};

#endif // FEATURE_INTERPRETER

//------------------------------------------------------------------------
// These macros GC-protect OBJECTREF pointers on the EE's behalf.
// In between these macros, the GC can move but not discard the protected
// objects. If the GC moves an object, it will update the guarded OBJECTREF's.
// Typical usage:
//
//   OBJECTREF or = <some valid objectref>;
//   GCPROTECT_BEGIN(or);
//
//   ...<do work that can trigger GC>...
//
//   GCPROTECT_END();
//
//
// These macros can also protect multiple OBJECTREF's if they're packaged
// into a structure:
//
//   struct xx {
//      OBJECTREF o1;
//      OBJECTREF o2;
//   } gc;
//
//   GCPROTECT_BEGIN(gc);
//   ....
//   GCPROTECT_END();
//
//
// Notes:
//
//   - GCPROTECT_BEGININTERIOR() can be used in place of GCPROTECT_BEGIN()
//     to handle the case where one or more of the OBJECTREFs is potentially
//     an interior pointer.  This is a rare situation, because boxing would
//     normally prevent us from encountering it.  Be aware that the OBJECTREFs
//     we protect are not validated in this situation.
//
//   - GCPROTECT_ARRAY_BEGIN() can be used when an array of object references
//     is allocated on the stack.  The pointer to the first element is passed
//     along with the number of elements in the array.
//
//   - The argument to GCPROTECT_BEGIN should be an lvalue because it
//     uses "sizeof" to count the OBJECTREF's.
//
//   - GCPROTECT_BEGIN spiritually violates our normal convention of not passing
//     non-const reference arguments. Unfortunately, this is necessary in
//     order for the sizeof thing to work.
//
//   - GCPROTECT_BEGIN does _not_ zero out the OBJECTREF's. You must have
//     valid OBJECTREF's when you invoke this macro.
//
//   - GCPROTECT_BEGIN begins a new C nesting block. Besides allowing
//     GCPROTECT_BEGIN's to nest, it also has the advantage of causing
//     a compiler error if you forget to code a maching GCPROTECT_END.
//
//   - If you are GCPROTECTing something, it means you are expecting a GC to occur.
//     So we assert that GC is not forbidden.
//------------------------------------------------------------------------

#ifndef DACCESS_COMPILE

#define GCPROTECT_BEGIN(ObjRefStruct)                           do {    \
                GCFrame __gcframe(                                      \
                        (OBJECTREF*)&(ObjRefStruct),                    \
                        sizeof(ObjRefStruct)/sizeof(OBJECTREF),         \
                        FALSE);                                         \
                {

#define GCPROTECT_BEGIN_THREAD(pThread, ObjRefStruct)           do {    \
                GCFrame __gcframe(                                      \
                        pThread,                                        \
                        (OBJECTREF*)&(ObjRefStruct),                    \
                        sizeof(ObjRefStruct)/sizeof(OBJECTREF),         \
                        FALSE);                                         \
                {

#define GCPROTECT_ARRAY_BEGIN(ObjRefArray,cnt) do {                     \
                GCFrame __gcframe(                                      \
                        (OBJECTREF*)&(ObjRefArray),                     \
                        cnt * sizeof(ObjRefArray) / sizeof(OBJECTREF),  \
                        FALSE);                                         \
                {

#define GCPROTECT_BEGININTERIOR(ObjRefStruct)           do {            \
                /* work around Wsizeof-pointer-div warning as we */     \
                /* mean to capture pointer or object size */            \
                UINT subjectSize = sizeof(ObjRefStruct);                \
                GCFrame __gcframe(                                      \
                        (OBJECTREF*)&(ObjRefStruct),                    \
                        subjectSize/sizeof(OBJECTREF),                  \
                        TRUE);                                          \
                {

#define GCPROTECT_BEGININTERIOR_ARRAY(ObjRefArray,cnt) do {             \
                GCFrame __gcframe(                                      \
                        (OBJECTREF*)&(ObjRefArray),                     \
                        cnt,                                            \
                        TRUE);                                          \
                {


#define GCPROTECT_END()                                                 \
                }                                                       \
                } while(0)


#else // #ifndef DACCESS_COMPILE

#define GCPROTECT_BEGIN(ObjRefStruct)
#define GCPROTECT_BEGIN_THREAD(pThread, ObjRefStruct)
#define GCPROTECT_ARRAY_BEGIN(ObjRefArray,cnt)
#define GCPROTECT_BEGININTERIOR(ObjRefStruct)
#define GCPROTECT_END()

#endif // #ifndef DACCESS_COMPILE


#define ASSERT_ADDRESS_IN_STACK(address) _ASSERTE (Thread::IsAddressInCurrentStack (address));

class GCRefMapBuilder;

void ComputeCallRefMap(MethodDesc* pMD,
                       GCRefMapBuilder * pBuilder,
                       bool isDispatchCell);

bool CheckGCRefMapEqual(PTR_BYTE pGCRefMap, MethodDesc* pMD, bool isDispatchCell);

//------------------------------------------------------------------------

#if defined(FRAMES_TURNED_FPO_ON)
#pragma optimize("", on)    // Go back to command line default optimizations
#undef FRAMES_TURNED_FPO_ON
#undef FPO_ON
#endif

#include "crossloaderallocatorhash.inl"

#endif  //__frames_h__
