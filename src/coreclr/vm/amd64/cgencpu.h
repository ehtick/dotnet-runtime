// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// CGENCPU.H -
//
// Various helper routines for generating AMD64 assembly code.
//
// DO NOT INCLUDE THIS FILE DIRECTLY - ALWAYS USE CGENSYS.H INSTEAD
//



#ifndef TARGET_AMD64
#error Should only include "AMD64\cgencpu.h" for AMD64 builds
#endif

#ifndef __cgencpu_h__
#define __cgencpu_h__

#include "xmmintrin.h"

// Given a return address retrieved during stackwalk,
// this is the offset by which it should be decremented to lend somewhere in a call instruction.
#define STACKWALK_CONTROLPC_ADJUST_OFFSET 1

// preferred alignment for data
#define DATA_ALIGNMENT 8

class MethodDesc;
class FramedMethodFrame;
class Module;
struct VASigCookie;
class ComCallMethodDesc;


#define COMMETHOD_PREPAD                        16   // # extra bytes to allocate in addition to sizeof(ComCallMethodDesc)
#define COMMETHOD_CALL_PRESTUB_SIZE             6    // 32-bit indirect relative call
#define COMMETHOD_CALL_PRESTUB_ADDRESS_OFFSET   -10  // the offset of the call target address inside the prestub

#define STACK_ALIGN_SIZE                        16

#define JUMP_ALLOCATE_SIZE                      12   // # bytes to allocate for a 64-bit jump instruction
#define BACK_TO_BACK_JUMP_ALLOCATE_SIZE         12   // # bytes to allocate for a back to back 64-bit jump instruction
#define SIZEOF_LOAD_AND_JUMP_THUNK              22   // # bytes to mov r10, X; jmp Z
#define SIZEOF_LOAD2_AND_JUMP_THUNK             32   // # bytes to mov r10, X; mov r11, Y; jmp Z

#define HAS_PINVOKE_IMPORT_PRECODE              1
#define HAS_FIXUP_PRECODE                       1

// ThisPtrRetBufPrecode one is necessary for closed delegates over static methods with return buffer
#define HAS_THISPTR_RETBUF_PRECODE              1

#define CODE_SIZE_ALIGN                         16   // must alloc code blocks on 8-byte boundaries; for perf reasons we use 16 byte boundaries
#define CACHE_LINE_SIZE                         64   // Current AMD64 processors have 64-byte cache lines as per AMD64 optimization manual
#define LOG2SLOT                                LOG2_PTRSIZE


#ifdef UNIX_AMD64_ABI
#define ENREGISTERED_RETURNTYPE_INTEGER_MAXSIZE 16   // bytes
#define ENREGISTERED_PARAMTYPE_MAXSIZE          16   // bytes
#define ENREGISTERED_RETURNTYPE_MAXSIZE         16   // bytes
#define CALLDESCR_ARGREGS                       1    // CallDescrWorker has ArgumentRegister parameter
#define CALLDESCR_FPARGREGS                     1    // CallDescrWorker has FloatArgumentRegisters parameter
#else
#define ENREGISTERED_RETURNTYPE_INTEGER_MAXSIZE 8    // bytes
#define ENREGISTERED_PARAMTYPE_MAXSIZE          8    // bytes
#define ENREGISTERED_RETURNTYPE_MAXSIZE         8    // bytes
#define COM_STUBS_SEPARATE_FP_LOCATIONS
#define CALLDESCR_REGTYPEMAP                    1
#endif

#define INSTRFMT_K64SMALL
#define INSTRFMT_K64

#ifndef TARGET_UNIX
#define USE_REDIRECT_FOR_GCSTRESS
#endif // TARGET_UNIX

//
// REX prefix byte
//
#define REX_PREFIX_BASE         0x40        // 0100xxxx
#define REX_OPERAND_SIZE_64BIT  0x08        // xxxx1xxx
#define REX_MODRM_REG_EXT       0x04        // xxxxx1xx     // use for 'middle' 3 bit field of mod/r/m
#define REX_SIB_INDEX_EXT       0x02        // xxxxxx10
#define REX_MODRM_RM_EXT        0x01        // XXXXXXX1     // use for low 3 bit field of mod/r/m
#define REX_SIB_BASE_EXT        0x01        // XXXXXXX1
#define REX_OPCODE_REG_EXT      0x01        // XXXXXXX1

#define X86_REGISTER_MASK       0x7

#define X86RegFromAMD64Reg(extended_reg) \
            ((X86Reg)(((int)extended_reg) & X86_REGISTER_MASK))

#define FLOAT_REGISTER_SIZE 16 // each register in FloatArgumentRegisters is 16 bytes.

// Why is the return value ARG_SLOT? On 64-bit systems, that is 64-bits
// and much bigger than necessary for R4, requiring explicit downcasts.
inline
ARG_SLOT FPSpillToR4(void* pSpillSlot)
{
    LIMITED_METHOD_CONTRACT;
    return *(DWORD*)pSpillSlot;
}

inline
ARG_SLOT FPSpillToR8(void* pSpillSlot)
{
    LIMITED_METHOD_CONTRACT;
    return *(SIZE_T*)pSpillSlot;
}

inline
void     R4ToFPSpill(void* pSpillSlot, DWORD  srcFloatAsDWORD)
{
    LIMITED_METHOD_CONTRACT;
    *(SIZE_T*)pSpillSlot = (SIZE_T)srcFloatAsDWORD;
    *((SIZE_T*)pSpillSlot + 1) = 0;
}

inline
void     R8ToFPSpill(void* pSpillSlot, SIZE_T  srcDoubleAsSIZE_T)
{
    LIMITED_METHOD_CONTRACT;
    *(SIZE_T*)pSpillSlot = srcDoubleAsSIZE_T;
    *((SIZE_T*)pSpillSlot + 1) = 0;
}


#define GetEEFuncEntryPoint(pfn) GFN_TADDR(pfn)


//**********************************************************************
// Parameter size
//**********************************************************************

inline unsigned StackElemSize(unsigned parmSize, bool isValueType = false /* unused */, bool isFloatHfa = false /* unused */)
{
    const unsigned stackSlotSize = 8;
    return ALIGN_UP(parmSize, stackSlotSize);
}

//**********************************************************************
// Frames
//**********************************************************************
//--------------------------------------------------------------------
// This represents some of the TransitionFrame fields that are
// stored at negative offsets.
//--------------------------------------------------------------------
struct REGDISPLAY;

//--------------------------------------------------------------------
// This represents the arguments that are stored in volatile registers.
// This should not overlap the CalleeSavedRegisters since those are already
// saved separately and it would be wasteful to save the same register twice.
// If we do use a non-volatile register as an argument, then the ArgIterator
// will probably have to communicate this back to the PromoteCallerStack
// routine to avoid a double promotion.
//--------------------------------------------------------------------
#ifdef UNIX_AMD64_ABI

#define ENUM_ARGUMENT_REGISTERS() \
    ARGUMENT_REGISTER(RDI) \
    ARGUMENT_REGISTER(RSI) \
    ARGUMENT_REGISTER(RDX) \
    ARGUMENT_REGISTER(RCX) \
    ARGUMENT_REGISTER(R8) \
    ARGUMENT_REGISTER(R9)

#define NUM_ARGUMENT_REGISTERS 6

// The order of registers in this macro is hardcoded in assembly code
// at number of places
#define ENUM_CALLEE_SAVED_REGISTERS() \
    CALLEE_SAVED_REGISTER(R12) \
    CALLEE_SAVED_REGISTER(R13) \
    CALLEE_SAVED_REGISTER(R14) \
    CALLEE_SAVED_REGISTER(R15) \
    CALLEE_SAVED_REGISTER(Rbx) \
    CALLEE_SAVED_REGISTER(Rbp)

#define NUM_CALLEE_SAVED_REGISTERS 6

// No floating point callee saved registers on Unix AMD64
#define ENUM_FP_CALLEE_SAVED_REGISTERS()

#else // UNIX_AMD64_ABI

#define ENUM_ARGUMENT_REGISTERS() \
    ARGUMENT_REGISTER(RCX) \
    ARGUMENT_REGISTER(RDX) \
    ARGUMENT_REGISTER(R8) \
    ARGUMENT_REGISTER(R9)

#define NUM_ARGUMENT_REGISTERS 4

// The order of registers in this macro is hardcoded in assembly code
// at number of places
#define ENUM_CALLEE_SAVED_REGISTERS() \
    CALLEE_SAVED_REGISTER(Rdi) \
    CALLEE_SAVED_REGISTER(Rsi) \
    CALLEE_SAVED_REGISTER(Rbx) \
    CALLEE_SAVED_REGISTER(Rbp) \
    CALLEE_SAVED_REGISTER(R12) \
    CALLEE_SAVED_REGISTER(R13) \
    CALLEE_SAVED_REGISTER(R14) \
    CALLEE_SAVED_REGISTER(R15)

#define NUM_CALLEE_SAVED_REGISTERS 8

#define ENUM_FP_CALLEE_SAVED_REGISTERS() \
    CALLEE_SAVED_REGISTER(Xmm6) \
    CALLEE_SAVED_REGISTER(Xmm7) \
    CALLEE_SAVED_REGISTER(Xmm8) \
    CALLEE_SAVED_REGISTER(Xmm9) \
    CALLEE_SAVED_REGISTER(Xmm10) \
    CALLEE_SAVED_REGISTER(Xmm11) \
    CALLEE_SAVED_REGISTER(Xmm12) \
    CALLEE_SAVED_REGISTER(Xmm13) \
    CALLEE_SAVED_REGISTER(Xmm14) \
    CALLEE_SAVED_REGISTER(Xmm15)

#endif // UNIX_AMD64_ABI

typedef DPTR(struct ArgumentRegisters) PTR_ArgumentRegisters;
struct ArgumentRegisters {
    #define ARGUMENT_REGISTER(regname) INT_PTR regname;
    ENUM_ARGUMENT_REGISTERS();
    #undef ARGUMENT_REGISTER
};

typedef DPTR(struct CalleeSavedRegisters) PTR_CalleeSavedRegisters;
struct CalleeSavedRegisters {
    #define CALLEE_SAVED_REGISTER(regname) INT_PTR regname;
    ENUM_CALLEE_SAVED_REGISTERS();
    #undef CALLEE_SAVED_REGISTER
};

struct CalleeSavedRegistersPointers {
    #define CALLEE_SAVED_REGISTER(regname) PTR_TADDR p##regname;
    ENUM_CALLEE_SAVED_REGISTERS();
    #undef CALLEE_SAVED_REGISTER
};

#define SCRATCH_REGISTER_X86REG kRAX

#ifdef UNIX_AMD64_ABI
#define THIS_REG RDI
#define THIS_kREG kRDI

#define ARGUMENT_kREG1 kRDI
#define ARGUMENT_kREG2 kRSI
#else
#define THIS_REG RCX
#define THIS_kREG kRCX

#define ARGUMENT_kREG1 kRCX
#define ARGUMENT_kREG2 kRDX
#endif

#ifdef UNIX_AMD64_ABI

#define NUM_FLOAT_ARGUMENT_REGISTERS 8

typedef DPTR(struct FloatArgumentRegisters) PTR_FloatArgumentRegisters;
struct FloatArgumentRegisters {
     M128A d[NUM_FLOAT_ARGUMENT_REGISTERS];   // xmm0-xmm7
};
#else
// Windows x64 calling convention uses 4 registers for floating point data
#define NUM_FLOAT_ARGUMENT_REGISTERS 4
#endif


void UpdateRegDisplayFromCalleeSavedRegisters(REGDISPLAY * pRD, CalleeSavedRegisters * pRegs);


// Sufficient context for Try/Catch restoration.
struct EHContext {
    // Not used
};

#define ARGUMENTREGISTERS_SIZE sizeof(ArgumentRegisters)


#include "stublinkeramd64.h"

//**********************************************************************
// Profiling
//**********************************************************************

#ifdef PROFILING_SUPPORTED

#define PROFILE_PLATFORM_SPECIFIC_DATA_BUFFER_SIZE 16

typedef struct _PROFILE_PLATFORM_SPECIFIC_DATA
{
    FunctionID  functionId;
    void       *rbp;
    void       *probeRsp;
    void       *ip;
    void       *profiledRsp;
    UINT64      rax;
    LPVOID      hiddenArg;
    UINT64      flt0;   // floats stored as doubles
    UINT64      flt1;
    UINT64      flt2;
    UINT64      flt3;
#if defined(UNIX_AMD64_ABI)
    UINT64      flt4;
    UINT64      flt5;
    UINT64      flt6;
    UINT64      flt7;
    UINT64      rdi;
    UINT64      rsi;
    UINT64      rdx;
    UINT64      rcx;
    UINT64      r8;
    UINT64      r9;
#endif
    UINT32      flags;
#if defined(UNIX_AMD64_ABI)
    // A buffer to copy structs in to so they are sequential for GetFunctionEnter3Info.
    UINT64      buffer[PROFILE_PLATFORM_SPECIFIC_DATA_BUFFER_SIZE];
#endif
} PROFILE_PLATFORM_SPECIFIC_DATA, *PPROFILE_PLATFORM_SPECIFIC_DATA;

#endif  // PROFILING_SUPPORTED

//**********************************************************************
// Exception handling
//**********************************************************************

inline PCODE GetIP(const CONTEXT * context)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;

    return PCODE(context->Rip);
}

inline void SetIP(CONTEXT* context, PCODE rip)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;

    context->Rip = (DWORD64) rip;
}

inline TADDR GetSP(const CONTEXT * context)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;

    return (TADDR)context->Rsp;
}
inline void SetSP(CONTEXT *context, TADDR rsp)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        SUPPORTS_DAC;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;

    context->Rsp = rsp;
}

#if !defined(DACCESS_COMPILE)
inline DWORD64 GetSSP(const CONTEXT * context)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;
#ifdef TARGET_WINDOWS
    XSAVE_CET_U_FORMAT* pCET = (XSAVE_CET_U_FORMAT*)LocateXStateFeature(const_cast<PCONTEXT>(context), XSTATE_CET_U, NULL);
    if ((pCET != NULL) && (pCET->Ia32CetUMsr != 0))
    {
        return pCET->Ia32Pl3SspMsr;
    }
#else
    // TODO: implement when we enable Intel CET on Unix
#endif
    return 0;
}

inline void SetSSP(CONTEXT *context, DWORD64 ssp)
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;

        PRECONDITION(CheckPointer(context));
    }
    CONTRACTL_END;

#ifdef TARGET_WINDOWS
    XSAVE_CET_U_FORMAT* pCET = (XSAVE_CET_U_FORMAT*)LocateXStateFeature(context, XSTATE_CET_U, NULL);
    if (pCET != NULL)
    {
        pCET->Ia32Pl3SspMsr = ssp;
        pCET->Ia32CetUMsr = 1;
    }
#else
    // TODO: implement when we enable Intel CET on Unix
#endif
}
#endif // !DACCESS_COMPILE

inline void SetFP(CONTEXT *context, TADDR rbp)
{
    LIMITED_METHOD_DAC_CONTRACT;

    context->Rbp = (DWORD64)rbp;
}

inline TADDR GetFP(const CONTEXT * context)
{
    LIMITED_METHOD_CONTRACT;

    return (TADDR)(context->Rbp);
}

inline void SetFirstArgReg(CONTEXT *context, TADDR value)
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef UNIX_AMD64_ABI
    context->Rdi = (DWORD64)value;
#else
    context->Rcx = (DWORD64)value;
#endif
}

inline TADDR GetFirstArgReg(CONTEXT *context)
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef UNIX_AMD64_ABI
    return (TADDR)(context->Rdi);
#else
    return (TADDR)(context->Rcx);
#endif
}

inline void SetSecondArgReg(CONTEXT *context, TADDR value)
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef UNIX_AMD64_ABI
    context->Rsi = (DWORD64)value;
#else
    context->Rdx = (DWORD64)value;
#endif
}

inline TADDR GetSecondArgReg(CONTEXT *context)
{
    LIMITED_METHOD_DAC_CONTRACT;
#ifdef UNIX_AMD64_ABI
    return (TADDR)(context->Rsi);
#else
    return (TADDR)(context->Rdx);
#endif
}

extern "C" TADDR GetCurrentSP();

// Emits:
//  mov r10, pv1
//  mov rax, pTarget
//  jmp rax
void EncodeLoadAndJumpThunk (LPBYTE pBuffer, LPVOID pv, LPVOID pTarget);


// Get Rel32 destination, emit jumpStub if necessary
INT32 rel32UsingJumpStub(INT32 UNALIGNED * pRel32, PCODE target, MethodDesc *pMethod,
    LoaderAllocator *pLoaderAllocator = NULL, bool throwOnOutOfMemoryWithinRange = true);

// Get Rel32 destination, emit jumpStub if necessary into a preallocated location
INT32 rel32UsingPreallocatedJumpStub(INT32 UNALIGNED * pRel32, PCODE target, PCODE jumpStubAddr, PCODE jumpStubAddrRW, bool emitJump);

void emitCOMStubCall (ComCallMethodDesc *pCOMMethodRX, ComCallMethodDesc *pCOMMethodRW, PCODE target);

void emitJump(LPBYTE pBufferRX, LPBYTE pBufferRW, LPVOID target);

BOOL isJumpRel32(PCODE pCode);
PCODE decodeJump32(PCODE pCode);

BOOL isJumpRel64(PCODE pCode);
PCODE decodeJump64(PCODE pCode);

//
// On IA64 back to back jumps should be separated by a nop bundle to get
// the best performance from the hardware's branch prediction logic.
// For all other platforms back to back jumps don't require anything special
// That is why we have these two wrapper functions that call emitJump and decodeJump
//
inline void emitBackToBackJump(LPBYTE pBufferRX, LPBYTE pBufferRW, LPVOID target)
{
    WRAPPER_NO_CONTRACT;

    emitJump(pBufferRX, pBufferRW, target);
}

inline PCODE decodeBackToBackJump(PCODE pCode)
{
    WRAPPER_NO_CONTRACT;
    SUPPORTS_DAC;
    if (isJumpRel32(pCode))
        return decodeJump32(pCode);
    else
    if (isJumpRel64(pCode))
        return decodeJump64(pCode);
    else
        return (PCODE)0;
}

extern "C" void setFPReturn(int fpSize, INT64 retVal);
extern "C" void getFPReturn(int fpSize, INT64 *retval);

struct HijackArgs
{
#ifdef UNIX_AMD64_ABI
    union
    {
        struct
        {
            ULONG64 Rax;
            ULONG64 Rdx;
        };
        ULONG64 ReturnValue[2];
    };
#else // UNIX_AMD64_ABI
    union
    {
        ULONG64 Rax;
        ULONG64 ReturnValue[1];
    };
#endif // UNIX_AMD64_ABI
    union
    {
        ULONG64 Rcx;
        ULONG64 AsyncRet;
    };
    CalleeSavedRegisters Regs;
#ifdef TARGET_WINDOWS
    ULONG64 Rsp;
#endif
    union
    {
        ULONG64 Rip;
        size_t ReturnAddress;
    };
};

#ifndef DACCESS_COMPILE

DWORD GetOffsetAtEndOfFunction(ULONGLONG           uImageBase,
                               PT_RUNTIME_FUNCTION   pFunctionEntry,
                               int                 offsetNum = 1);

#endif // DACCESS_COMPILE

// ClrFlushInstructionCache is used when we want to call FlushInstructionCache
// for a specific architecture in the common code, but not for other architectures.
// We call ClrFlushInstructionCache whenever we create or modify code in the heap.
// Currently ClrFlushInstructionCache has no effect on AMD64
//

inline BOOL ClrFlushInstructionCache(LPCVOID pCodeAddr, size_t sizeOfCode, bool hasCodeExecutedBefore = false)
{
    if (hasCodeExecutedBefore)
    {
        FlushInstructionCache(GetCurrentProcess(), pCodeAddr, sizeOfCode);
    }
    else
    {
        MemoryBarrier();
    }
    return TRUE;
}

//
// JIT HELPER ALIASING FOR PORTABILITY.
//
// Create alias for optimized implementations of helpers provided on this platform
//
#define JIT_GetDynamicGCStaticBase           JIT_GetDynamicGCStaticBase_SingleAppDomain
#define JIT_GetDynamicNonGCStaticBase        JIT_GetDynamicNonGCStaticBase_SingleAppDomain

#endif // __cgencpu_h__
