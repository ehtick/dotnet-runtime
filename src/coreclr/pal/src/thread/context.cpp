// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*++

Module Name:

    context.c

Abstract:

    Implementation of GetThreadContext/SetThreadContext/DebugBreak.
    There are a lot of architecture specifics here.

--*/

#include "pal/dbgmsg.h"
SET_DEFAULT_DEBUG_CHANNEL(THREAD); // some headers have code with asserts, so do this first

#include "pal/palinternal.h"
#include "pal/context.h"
#include "pal/debug.h"
#include "pal/thread.hpp"
#include "pal/utils.h"
#include "pal/virtual.h"

#if HAVE_SYS_PTRACE_H
#include <sys/ptrace.h>
#endif
#include <errno.h>
#include <unistd.h>
#if defined(HOST_APPLE) && !defined(HOST_OSX)
#include <libkern/OSCacheControl.h>
#endif

extern PGET_GCMARKER_EXCEPTION_CODE g_getGcMarkerExceptionCode;

#define CONTEXT_AREA_MASK 0xffff
#ifdef HOST_X86
#define CONTEXT_ALL_FLOATING (CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS)
#else
#define CONTEXT_ALL_FLOATING CONTEXT_FLOATING_POINT
#endif

#if !HAVE_MACH_EXCEPTIONS

#ifndef __GLIBC__
typedef int __ptrace_request;
#endif

#if HAVE_MACHINE_REG_H
#include <machine/reg.h>
#endif  // HAVE_MACHINE_REG_H
#if HAVE_MACHINE_NPX_H
#include <machine/npx.h>
#endif  // HAVE_MACHINE_NPX_H

#if HAVE_PT_REGS
#include <asm/ptrace.h>
#endif  // HAVE_PT_REGS

#endif // !HAVE_MACH_EXCEPTIONS

#ifdef HOST_AMD64
#ifdef __HAIKU__
#define ASSIGN_CONTROL_REGS \
        ASSIGN_REG(Rbp)     \
        ASSIGN_REG(Rip)     \
        ASSIGN_REG(EFlags)  \
        ASSIGN_REG(Rsp)     \

#else // __HAIKU__
#define ASSIGN_CONTROL_REGS \
        ASSIGN_REG(Rbp)     \
        ASSIGN_REG(Rip)     \
        ASSIGN_REG(SegCs)   \
        ASSIGN_REG(EFlags)  \
        ASSIGN_REG(Rsp)     \

#endif // __HAIKU__
#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(Rdi)     \
    ASSIGN_REG(Rsi)     \
    ASSIGN_REG(Rbx)     \
    ASSIGN_REG(Rdx)     \
    ASSIGN_REG(Rcx)     \
    ASSIGN_REG(Rax)     \
    ASSIGN_REG(R8)     \
    ASSIGN_REG(R9)     \
    ASSIGN_REG(R10)     \
    ASSIGN_REG(R11)     \
    ASSIGN_REG(R12)     \
    ASSIGN_REG(R13)     \
    ASSIGN_REG(R14)     \
    ASSIGN_REG(R15)     \

#elif defined(HOST_X86)
#define ASSIGN_CONTROL_REGS \
    ASSIGN_REG(Ebp)     \
    ASSIGN_REG(Eip)     \
    ASSIGN_REG(SegCs)   \
    ASSIGN_REG(EFlags)  \
    ASSIGN_REG(Esp)     \
    ASSIGN_REG(SegSs)   \

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(Edi)     \
    ASSIGN_REG(Esi)     \
    ASSIGN_REG(Ebx)     \
    ASSIGN_REG(Edx)     \
    ASSIGN_REG(Ecx)     \
    ASSIGN_REG(Eax)     \

#elif defined(HOST_ARM)
#define ASSIGN_CONTROL_REGS \
    ASSIGN_REG(Sp)     \
    ASSIGN_REG(Lr)     \
    ASSIGN_REG(Pc)   \
    ASSIGN_REG(Cpsr)  \

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(R0)     \
    ASSIGN_REG(R1)     \
    ASSIGN_REG(R2)     \
    ASSIGN_REG(R3)     \
    ASSIGN_REG(R4)     \
    ASSIGN_REG(R5)     \
    ASSIGN_REG(R6)     \
    ASSIGN_REG(R7)     \
    ASSIGN_REG(R8)     \
    ASSIGN_REG(R9)     \
    ASSIGN_REG(R10)     \
    ASSIGN_REG(R11)     \
    ASSIGN_REG(R12)
#elif defined(HOST_ARM64)
#define ASSIGN_CONTROL_REGS \
    ASSIGN_REG(Cpsr)    \
    ASSIGN_REG(Fp)      \
    ASSIGN_REG(Sp)      \
    ASSIGN_REG(Lr)      \
    ASSIGN_REG(Pc)

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(X0)      \
    ASSIGN_REG(X1)      \
    ASSIGN_REG(X2)      \
    ASSIGN_REG(X3)      \
    ASSIGN_REG(X4)      \
    ASSIGN_REG(X5)      \
    ASSIGN_REG(X6)      \
    ASSIGN_REG(X7)      \
    ASSIGN_REG(X8)      \
    ASSIGN_REG(X9)      \
    ASSIGN_REG(X10)     \
    ASSIGN_REG(X11)     \
    ASSIGN_REG(X12)     \
    ASSIGN_REG(X13)     \
    ASSIGN_REG(X14)     \
    ASSIGN_REG(X15)     \
    ASSIGN_REG(X16)     \
    ASSIGN_REG(X17)     \
    ASSIGN_REG(X18)     \
    ASSIGN_REG(X19)     \
    ASSIGN_REG(X20)     \
    ASSIGN_REG(X21)     \
    ASSIGN_REG(X22)     \
    ASSIGN_REG(X23)     \
    ASSIGN_REG(X24)     \
    ASSIGN_REG(X25)     \
    ASSIGN_REG(X26)     \
    ASSIGN_REG(X27)     \
    ASSIGN_REG(X28)

#elif defined(HOST_LOONGARCH64)
#define ASSIGN_CONTROL_REGS  \
    ASSIGN_REG(Fp)      \
    ASSIGN_REG(Sp)      \
    ASSIGN_REG(Ra)      \
    ASSIGN_REG(Pc)

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(R0)     \
    ASSIGN_REG(A0)     \
    ASSIGN_REG(A1)     \
    ASSIGN_REG(A2)     \
    ASSIGN_REG(A3)     \
    ASSIGN_REG(A4)     \
    ASSIGN_REG(A5)     \
    ASSIGN_REG(A6)     \
    ASSIGN_REG(A7)     \
    ASSIGN_REG(T0)     \
    ASSIGN_REG(T1)     \
    ASSIGN_REG(T2)     \
    ASSIGN_REG(T3)     \
    ASSIGN_REG(T4)     \
    ASSIGN_REG(T5)     \
    ASSIGN_REG(T6)     \
    ASSIGN_REG(T7)     \
    ASSIGN_REG(T8)     \
    ASSIGN_REG(S0)     \
    ASSIGN_REG(S1)     \
    ASSIGN_REG(S2)     \
    ASSIGN_REG(S3)     \
    ASSIGN_REG(S4)     \
    ASSIGN_REG(S5)     \
    ASSIGN_REG(S6)     \
    ASSIGN_REG(S7)     \
    ASSIGN_REG(S8)     \
    ASSIGN_REG(X0)

#elif defined(HOST_RISCV64)

// https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/2d865a2964fe06bfc569ab00c74e152b582ed764/riscv-cc.adoc

#define ASSIGN_CONTROL_REGS  \
    ASSIGN_REG(Ra)      \
    ASSIGN_REG(Sp)      \
    ASSIGN_REG(Fp)      \
    ASSIGN_REG(Pc)

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(Gp)     \
    ASSIGN_REG(Tp)     \
    ASSIGN_REG(T0)     \
    ASSIGN_REG(T1)     \
    ASSIGN_REG(T2)     \
    ASSIGN_REG(S1)     \
    ASSIGN_REG(A0)     \
    ASSIGN_REG(A1)     \
    ASSIGN_REG(A2)     \
    ASSIGN_REG(A3)     \
    ASSIGN_REG(A4)     \
    ASSIGN_REG(A5)     \
    ASSIGN_REG(A6)     \
    ASSIGN_REG(A7)     \
    ASSIGN_REG(S2)     \
    ASSIGN_REG(S3)     \
    ASSIGN_REG(S4)     \
    ASSIGN_REG(S5)     \
    ASSIGN_REG(S6)     \
    ASSIGN_REG(S7)     \
    ASSIGN_REG(S8)     \
    ASSIGN_REG(S9)     \
    ASSIGN_REG(S10)    \
    ASSIGN_REG(S11)    \
    ASSIGN_REG(T3)     \
    ASSIGN_REG(T4)     \
    ASSIGN_REG(T5)     \
    ASSIGN_REG(T6)

#elif defined(HOST_S390X)
#define ASSIGN_CONTROL_REGS \
    ASSIGN_REG(PSWMask) \
    ASSIGN_REG(PSWAddr) \
    ASSIGN_REG(R15)     \

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(R0)      \
    ASSIGN_REG(R1)      \
    ASSIGN_REG(R2)      \
    ASSIGN_REG(R3)      \
    ASSIGN_REG(R4)      \
    ASSIGN_REG(R5)      \
    ASSIGN_REG(R5)      \
    ASSIGN_REG(R6)      \
    ASSIGN_REG(R7)      \
    ASSIGN_REG(R8)      \
    ASSIGN_REG(R9)      \
    ASSIGN_REG(R10)     \
    ASSIGN_REG(R11)     \
    ASSIGN_REG(R12)     \
    ASSIGN_REG(R13)     \
    ASSIGN_REG(R14)

#elif defined(HOST_POWERPC64)
#define ASSIGN_CONTROL_REGS \
    ASSIGN_REG(Nip) \
    ASSIGN_REG(Msr) \
    ASSIGN_REG(Ctr) \
    ASSIGN_REG(Link) \
    ASSIGN_REG(Xer) \
    ASSIGN_REG(Ccr) \
    ASSIGN_REG(R31) \

#define ASSIGN_INTEGER_REGS \
    ASSIGN_REG(R0)      \
    ASSIGN_REG(R1)      \
    ASSIGN_REG(R2)      \
    ASSIGN_REG(R3)      \
    ASSIGN_REG(R4)      \
    ASSIGN_REG(R5)      \
    ASSIGN_REG(R5)      \
    ASSIGN_REG(R6)      \
    ASSIGN_REG(R7)      \
    ASSIGN_REG(R8)      \
    ASSIGN_REG(R9)      \
    ASSIGN_REG(R10)     \
    ASSIGN_REG(R11)     \
    ASSIGN_REG(R12)     \
    ASSIGN_REG(R13)     \
    ASSIGN_REG(R14)     \
    ASSIGN_REG(R15)     \
    ASSIGN_REG(R16)     \
    ASSIGN_REG(R17)     \
    ASSIGN_REG(R18)     \
    ASSIGN_REG(R19)     \
    ASSIGN_REG(R20)     \
    ASSIGN_REG(R21)     \
    ASSIGN_REG(R22)     \
    ASSIGN_REG(R23)     \
    ASSIGN_REG(R24)     \
    ASSIGN_REG(R25)     \
    ASSIGN_REG(R26)     \
    ASSIGN_REG(R27)     \
    ASSIGN_REG(R28)     \
    ASSIGN_REG(R29)     \
    ASSIGN_REG(R30)

#elif defined(HOST_WASM)
#define ASSIGN_CONTROL_REGS  \
    ASSERT("WASM does not have registers");
#define ASSIGN_INTEGER_REGS  \
    ASSERT("WASM does not have registers");
#else
#error "Don't know how to assign registers on this architecture"
#endif

#define ASSIGN_ALL_REGS     \
        ASSIGN_CONTROL_REGS \
        ASSIGN_INTEGER_REGS \

#if defined(XSTATE_SUPPORTED) || defined(HOST_AMD64) && defined(HAVE_MACH_EXCEPTIONS)
bool Xstate_IsAvx512Supported()
{
#if defined(HAVE_MACH_EXCEPTIONS)
    // MacOS has specialized behavior where it reports AVX512 support but doesnt
    // actually enable AVX512 until the first instruction is executed and does so
    // on a per thread basis. It does this by catching the faulting instruction and
    // checking for the EVEX encoding. The kmov instructions, despite being part
    // of the AVX512 instruction set are VEX encoded and dont trigger the enablement
    //
    // See https://github.com/apple/darwin-xnu/blob/main/osfmk/i386/fpu.c#L174

    // TODO-AVX512: Enabling this for OSX requires ensuring threads explicitly trigger
    // the AVX-512 enablement so that arbitrary usage doesn't cause downstream problems

    return false;
#else
    static int Xstate_Avx512Supported = -1;

    if (Xstate_Avx512Supported == -1)
    {
        int cpuidInfo[4];

        const int CPUID_EAX = 0;
        const int CPUID_EBX = 1;
        const int CPUID_ECX = 2;
        const int CPUID_EDX = 3;

#ifdef _DEBUG
        // We should only be calling this function if we know the extended feature exists
        __cpuid(cpuidInfo, 0x00000000);
        _ASSERTE(static_cast<uint32_t>(cpuidInfo[CPUID_EAX]) >= 0x0D);
#endif // _DEBUG

        __cpuidex(cpuidInfo, 0x0000000D, 0x00000000);

        if ((cpuidInfo[CPUID_EAX] & XSTATE_MASK_AVX512) == XSTATE_MASK_AVX512)
        {
            // Knight's Landing and Knight's Mill shipped without all 5 of the "baseline"
            // AVX-512 ISAs that are required by x86-64-v4. Specifically they do not include
            // BW, DQ, or VL. RyuJIT currently requires all 5 ISAs to be present so we will
            // only enable Avx512 context save/restore when all exist. This requires us to
            // query which ISAs are actually supported to ensure they're all present.

            __cpuidex(cpuidInfo, 0x00000007, 0x00000000);

            const int requiredAvx512Flags = (1 << 16) |   // AVX512F
                                            (1 << 17) |   // AVX512DQ
                                            (1 << 28) |   // AVX512CD
                                            (1 << 30) |   // AVX512BW
                                            (1 << 31);    // AVX512VL

            if ((cpuidInfo[CPUID_EBX] & requiredAvx512Flags) == requiredAvx512Flags)
            {
                Xstate_Avx512Supported = 1;
            }
        }

        if (Xstate_Avx512Supported == -1)
        {
            Xstate_Avx512Supported = 0;
        }
    }

    return Xstate_Avx512Supported == 1;
#endif
}

bool Xstate_IsApxSupported()
{
#if defined(HAVE_MACH_EXCEPTIONS)
    // TODO-xarch-apx: I assume OSX will never support APX
    return false;
#else
    static int Xstate_ApxSupported = -1;

    if (Xstate_ApxSupported == -1)
    {
        int cpuidInfo[4];

        const int CPUID_EAX = 0;
        const int CPUID_EBX = 1;
        const int CPUID_ECX = 2;
        const int CPUID_EDX = 3;

#ifdef _DEBUG
        // We should only be calling this function if we know the extended feature exists
        __cpuid(cpuidInfo, 0x00000000);
        _ASSERTE(static_cast<uint32_t>(cpuidInfo[CPUID_EAX]) >= 0x0D);
#endif // _DEBUG

        __cpuidex(cpuidInfo, 0x0000000D, 0x00000000);

        if ((cpuidInfo[CPUID_EAX] & XSTATE_MASK_APX) == XSTATE_MASK_APX)
        {
            // Knight's Landing and Knight's Mill shipped without all 5 of the "baseline"
            // AVX-512 ISAs that are required by x86-64-v4. Specifically they do not include
            // BW, DQ, or VL. RyuJIT currently requires all 5 ISAs to be present so we will
            // only enable Avx512 context save/restore when all exist. This requires us to
            // query which ISAs are actually supported to ensure they're all present.

            __cpuidex(cpuidInfo, 0x00000007, 0x00000001);

            const int requiredApxFlags = (1 << 21);

            if ((cpuidInfo[CPUID_EDX] & requiredApxFlags) == requiredApxFlags)
            {
                Xstate_ApxSupported = 1;
            }
        }

        if (Xstate_ApxSupported == -1)
        {
            Xstate_ApxSupported = 0;
        }
    }

    return Xstate_ApxSupported == 1;
#endif
}
#endif // XSTATE_SUPPORTED || defined(HOST_AMD64) && defined(HAVE_MACH_EXCEPTIONS)

#if !HAVE_MACH_EXCEPTIONS

#if defined(XSTATE_SUPPORTED) && defined(HOST_AMD64)
Xstate_ExtendedFeature Xstate_ExtendedFeatures[Xstate_ExtendedFeatures_Count];
#endif // XSTATE_SUPPORTED && HOST_AMD64

/*++
Function:
  CONTEXT_GetRegisters

Abstract
  retrieve the machine registers value of the indicated process.

Parameter
  processId: process ID
  lpContext: context structure in which the machine registers value will be returned.
Return
 returns TRUE if it succeeds, FALSE otherwise
--*/
BOOL CONTEXT_GetRegisters(DWORD processId, LPCONTEXT lpContext)
{
#if HAVE_BSD_REGS_T
    int regFd = -1;
#endif  // HAVE_BSD_REGS_T
    BOOL bRet = FALSE;

    if (processId == GetCurrentProcessId())
    {
        CONTEXT_CaptureContext(lpContext);
    }
    else
    {
        ucontext_t registers;
#if HAVE_PT_REGS
        struct pt_regs ptrace_registers;
        if (ptrace((__ptrace_request)PTRACE_GETREGS, processId, (caddr_t) &ptrace_registers, 0) == -1)
#elif HAVE_BSD_REGS_T
        struct reg ptrace_registers;
        if (PAL_PTRACE(PT_GETREGS, processId, &ptrace_registers, 0) == -1)
#endif
        {
            ASSERT("Failed ptrace(PT_GETREGS, processId:%d) errno:%d (%s)\n",
                   processId, errno, strerror(errno));
        }

#if HAVE_PT_REGS
#define ASSIGN_REG(reg) MCREG_##reg(registers.uc_mcontext) = PTREG_##reg(ptrace_registers);
#elif HAVE_BSD_REGS_T
#define ASSIGN_REG(reg) MCREG_##reg(registers.uc_mcontext) = BSDREG_##reg(ptrace_registers);
#else
#define ASSIGN_REG(reg)
	ASSERT("Don't know how to get the context of another process on this platform!");
	return bRet;
#endif
        ASSIGN_ALL_REGS
#undef ASSIGN_REG

        CONTEXTFromNativeContext(&registers, lpContext, lpContext->ContextFlags);
    }

    bRet = TRUE;
#if HAVE_BSD_REGS_T
    if (regFd != -1)
    {
        close(regFd);
    }
#endif  // HAVE_BSD_REGS_T
    return bRet;
}

/*++
Function:
  GetThreadContext

See MSDN doc.
--*/
BOOL
CONTEXT_GetThreadContext(
         DWORD dwProcessId,
         pthread_t self,
         LPCONTEXT lpContext)
{
    BOOL ret = FALSE;

    if (lpContext == NULL)
    {
        ERROR("Invalid lpContext parameter value\n");
        SetLastError(ERROR_NOACCESS);
        goto EXIT;
    }

    /* How to consider the case when self is different from the current
       thread of its owner process. Machine registers values could be retrieved
       by a ptrace(pid, ...) call or from the "/proc/%pid/reg" file content.
       Unfortunately, these two methods only depend on process ID, not on
       thread ID. */

    if (dwProcessId == GetCurrentProcessId())
    {
        if (self != pthread_self())
        {
            DWORD flags;
            // There aren't any APIs for this. We can potentially get the
            // context of another thread by using per-thread signals, but
            // on FreeBSD signal handlers that are called as a result
            // of signals raised via pthread_kill don't get a valid
            // sigcontext or ucontext_t. But we need this to return TRUE
            // to avoid an assertion in the CLR in code that manages to
            // cope reasonably well without a valid thread context.
            // Given that, we'll zero out our structure and return TRUE.
            ERROR("GetThreadContext on a thread other than the current "
                  "thread is returning TRUE\n");
            flags = lpContext->ContextFlags;
            memset(lpContext, 0, sizeof(*lpContext));
            lpContext->ContextFlags = flags;
            ret = TRUE;
            goto EXIT;
        }

    }

    if (lpContext->ContextFlags &
        (CONTEXT_CONTROL | CONTEXT_INTEGER) & CONTEXT_AREA_MASK)
    {
        if (CONTEXT_GetRegisters(dwProcessId, lpContext) == FALSE)
        {
            SetLastError(ERROR_INTERNAL_ERROR);
            goto EXIT;
        }
    }

    ret = TRUE;

EXIT:
    return ret;
}

/*++
Function:
  SetThreadContext

See MSDN doc.
--*/
BOOL
CONTEXT_SetThreadContext(
           DWORD dwProcessId,
           pthread_t self,
           CONST CONTEXT *lpContext)
{
    BOOL ret = FALSE;

#if HAVE_PT_REGS
    struct pt_regs ptrace_registers;
#elif HAVE_BSD_REGS_T
    struct reg ptrace_registers;
#endif

    if (lpContext == NULL)
    {
        ERROR("Invalid lpContext parameter value\n");
        SetLastError(ERROR_NOACCESS);
        goto EXIT;
    }

    /* How to consider the case when self is different from the current
       thread of its owner process. Machine registers values could be retrieved
       by a ptrace(pid, ...) call or from the "/proc/%pid/reg" file content.
       Unfortunately, these two methods only depend on process ID, not on
       thread ID. */

    if (dwProcessId == GetCurrentProcessId())
    {
        // Need to implement SetThreadContext(current thread) for the IX architecture; look at common_signal_handler.
        _ASSERT(FALSE);

        ASSERT("SetThreadContext should be called for cross-process only.\n");
        SetLastError(ERROR_INVALID_PARAMETER);
        goto EXIT;
    }

    if (lpContext->ContextFlags  &
        (CONTEXT_CONTROL | CONTEXT_INTEGER) & CONTEXT_AREA_MASK)
    {
#if HAVE_PT_REGS
        if (ptrace((__ptrace_request)PTRACE_GETREGS, dwProcessId, (caddr_t)&ptrace_registers, 0) == -1)
#elif HAVE_BSD_REGS_T
        if (PAL_PTRACE(PT_GETREGS, dwProcessId, &ptrace_registers, 0) == -1)
#endif
        {
            ASSERT("Failed ptrace(PT_GETREGS, processId:%d) errno:%d (%s)\n",
                   dwProcessId, errno, strerror(errno));
             SetLastError(ERROR_INTERNAL_ERROR);
             goto EXIT;
        }

#if HAVE_PT_REGS
#define ASSIGN_REG(reg) PTREG_##reg(ptrace_registers) = lpContext->reg;
#elif HAVE_BSD_REGS_T
#define ASSIGN_REG(reg) BSDREG_##reg(ptrace_registers) = lpContext->reg;
#else
#define ASSIGN_REG(reg)
	ASSERT("Don't know how to set the context of another process on this platform!");
	return FALSE;
#endif
        if (lpContext->ContextFlags & CONTEXT_CONTROL & CONTEXT_AREA_MASK)
        {
            ASSIGN_CONTROL_REGS
        }
        if (lpContext->ContextFlags & CONTEXT_INTEGER & CONTEXT_AREA_MASK)
        {
            ASSIGN_INTEGER_REGS
        }
#undef ASSIGN_REG

#if HAVE_PT_REGS
        if (ptrace((__ptrace_request)PTRACE_SETREGS, dwProcessId, (caddr_t)&ptrace_registers, 0) == -1)
#elif HAVE_BSD_REGS_T
        if (PAL_PTRACE(PT_SETREGS, dwProcessId, &ptrace_registers, 0) == -1)
#endif
        {
            ASSERT("Failed ptrace(PT_SETREGS, processId:%d) errno:%d (%s)\n",
                   dwProcessId, errno, strerror(errno));
            SetLastError(ERROR_INTERNAL_ERROR);
            goto EXIT;
        }
    }

    ret = TRUE;
   EXIT:
     return ret;
}

#endif // !HAVE_MACH_EXCEPTIONS

/*++
Function :
    CONTEXTToNativeContext

    Converts a CONTEXT record to a native context.

Parameters :
    CONST CONTEXT *lpContext : CONTEXT to convert
    native_context_t *native : native context to fill in

Return value :
    None

--*/
void CONTEXTToNativeContext(CONST CONTEXT *lpContext, native_context_t *native)
{
#define ASSIGN_REG(reg) MCREG_##reg(native->uc_mcontext) = lpContext->reg;
    if ((lpContext->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        ASSIGN_CONTROL_REGS
    }

    if ((lpContext->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
    {
        ASSIGN_INTEGER_REGS
    }
#undef ASSIGN_REG

#if !HAVE_FPREGS_WITH_CW
#if (HAVE_GREGSET_T || HAVE___GREGSET_T) && !defined(HOST_S390X) && !defined(HOST_LOONGARCH64) && !defined(HOST_RISCV64) && !defined(HOST_POWERPC64)
#if HAVE_GREGSET_T
    if (native->uc_mcontext.fpregs == nullptr)
#elif HAVE___GREGSET_T
    if (native->uc_mcontext.__fpregs == nullptr)
#endif // HAVE_GREGSET_T
    {
        // If the pointer to the floating point state in the native context
        // is not valid, we can't copy floating point registers regardless of
        // whether CONTEXT_FLOATING_POINT is set in the CONTEXT's flags.
        return;
    }
#endif // (HAVE_GREGSET_T || HAVE___GREGSET_T) && !HOST_S390X && !HOST_LOONGARCH64 && !HOST_RISCV64 && !HOST_POWERPC64
#endif // !HAVE_FPREGS_WITH_CW

#if defined(HOST_ARM64) && !defined(__APPLE__) && !defined(TARGET_FREEBSD)
    sve_context* sve = nullptr;
    fpsimd_context* fp = nullptr;
    if (((lpContext->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT) ||
        ((lpContext->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE))
    {
        GetNativeSigSimdContext(native, &fp, &sve);
    }
#endif // HOST_ARM64 && !__APPLE__ && !TARGET_FREEBSD

    if ((lpContext->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT)
    {
#ifdef HOST_AMD64
        FPREG_ControlWord(native) = lpContext->FltSave.ControlWord;
        FPREG_StatusWord(native) = lpContext->FltSave.StatusWord;
#if HAVE_FPREGS_WITH_CW
        FPREG_TagWord1(native) = lpContext->FltSave.TagWord >> 8;
        FPREG_TagWord2(native) = lpContext->FltSave.TagWord & 0xff;
#else
        FPREG_TagWord(native) = lpContext->FltSave.TagWord;
#endif
        FPREG_ErrorOffset(native) = lpContext->FltSave.ErrorOffset;
        FPREG_ErrorSelector(native) = lpContext->FltSave.ErrorSelector;
        FPREG_DataOffset(native) = lpContext->FltSave.DataOffset;
        FPREG_DataSelector(native) = lpContext->FltSave.DataSelector;
        FPREG_MxCsr(native) = lpContext->FltSave.MxCsr;
        FPREG_MxCsr_Mask(native) = lpContext->FltSave.MxCsr_Mask;

        for (int i = 0; i < 8; i++)
        {
            FPREG_St(native, i) = lpContext->FltSave.FloatRegisters[i];
        }

        for (int i = 0; i < 16; i++)
        {
            FPREG_Xmm(native, i) = lpContext->FltSave.XmmRegisters[i];
        }
#elif defined(HOST_ARM64)
#ifdef __APPLE__
        _STRUCT_ARM_NEON_STATE64* fp = GetNativeSigSimdContext(native);
        fp->__fpsr = lpContext->Fpsr;
        fp->__fpcr = lpContext->Fpcr;
        for (int i = 0; i < 32; i++)
        {
            *(NEON128*) &fp->__v[i] = lpContext->V[i];
        }
#elif defined(TARGET_FREEBSD)
        struct fpregs* fp = GetNativeSigSimdContext(native);
        if (fp)
        {
            fp->fp_sr = lpContext->Fpsr;
            fp->fp_cr = lpContext->Fpcr;
            for (int i = 0; i < 32; i++)
            {
                *(NEON128*) &fp->fp_q[i] = lpContext->V[i];
            }
        }
#else // __APPLE__
        if (fp)
        {
            fp->fpsr = lpContext->Fpsr;
            fp->fpcr = lpContext->Fpcr;
            for (int i = 0; i < 32; i++)
            {
                *(NEON128*) &fp->vregs[i] = lpContext->V[i];
            }
        }
#endif // __APPLE__
#elif defined(HOST_ARM)
        VfpSigFrame* fp = GetNativeSigSimdContext(native);
        if (fp)
        {
            fp->Fpscr = lpContext->Fpscr;
            for (int i = 0; i < 32; i++)
            {
                fp->D[i] = lpContext->D[i];
            }
        }
#elif defined(HOST_S390X)
        fpregset_t *fp = &native->uc_mcontext.fpregs;
        static_assert_no_msg(sizeof(fp->fprs) == sizeof(lpContext->Fpr));
        memcpy(fp->fprs, lpContext->Fpr, sizeof(lpContext->Fpr));
#elif defined(HOST_LOONGARCH64)
        struct sctx_info* info = (struct sctx_info*) native->uc_mcontext.__extcontext;
        if (FPU_CTX_MAGIC == info->magic)
        {
            struct fpu_context* fpr = (struct fpu_context*)(info + 1);
            fpr->fcsr = lpContext->Fcsr;
            fpr->fcc  = lpContext->Fcc;
            memcpy(fpr->regs, lpContext->F, sizeof(fpr->regs));
        }
        else if (LSX_CTX_MAGIC == info->magic)
        {
            struct lsx_context* fpr = (struct lsx_context*)(info + 1);
            fpr->fcsr = lpContext->Fcsr;
            fpr->fcc  = lpContext->Fcc;
            memcpy(fpr->regs, lpContext->F, sizeof(fpr->regs));
        }
        else if (LASX_CTX_MAGIC == info->magic)
        {
            struct lasx_context* fpr = (struct lasx_context*)(info + 1);
            fpr->fcsr = lpContext->Fcsr;
            fpr->fcc  = lpContext->Fcc;
            memcpy(fpr->regs, lpContext->F, sizeof(fpr->regs));
        }
        else
        {
            _ASSERTE(LBT_CTX_MAGIC == info->magic);
        }
#elif defined(HOST_RISCV64)
        native->uc_mcontext.__fpregs.__d.__fcsr = lpContext->Fcsr;
        for (int i = 0; i < 32; i++)
        {
            native->uc_mcontext.__fpregs.__d.__f[i] = lpContext->F[i];
        }
#endif
    }

    // TODO: Enable for all Unix systems
#if defined(XSTATE_SUPPORTED)
    if ((lpContext->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE)
    {
#if defined(HOST_AMD64)
        if (FPREG_HasYmmRegisters(native))
        {
            _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX) == XSTATE_MASK_AVX);

            uint32_t size;
            void *dest;

            dest = FPREG_Xstate_Ymmh(native, &size);
            _ASSERT(size == (sizeof(M128A) * 16));
            memcpy_s(dest, sizeof(M128A) * 16, &lpContext->Ymm0H, sizeof(M128A) * 16);

            if (FPREG_HasAvx512Registers(native))
            {
                _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX512) == XSTATE_MASK_AVX512);

                dest = FPREG_Xstate_Opmask(native, &size);
                _ASSERT(size == (sizeof(DWORD64) * 8));
                memcpy_s(dest, sizeof(DWORD64) * 8, &lpContext->KMask0, sizeof(DWORD64) * 8);

                dest = FPREG_Xstate_ZmmHi256(native, &size);
                _ASSERT(size == (sizeof(M256) * 16));
                memcpy_s(dest, sizeof(M256) * 16, &lpContext->Zmm0H, sizeof(M256) * 16);

                dest = FPREG_Xstate_Hi16Zmm(native, &size);
                _ASSERT(size == (sizeof(M512) * 16));
                memcpy_s(dest, sizeof(M512) * 16, &lpContext->Zmm16, sizeof(M512) * 16);

#ifndef TARGET_OSX
                // TODO-xarch-apx: I suppose OSX will not support APX.
                if (FPREG_HasApxRegisters(native))
                {
                    _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_APX) == XSTATE_MASK_APX);

                    dest = FPREG_Xstate_Egpr(native, &size);
                    _ASSERT(size == (sizeof(DWORD64) * 16));
                    memcpy_s(dest, sizeof(DWORD64) * 16, &lpContext->R16, sizeof(DWORD64) * 16);
                }
#endif //  !TARGET_OSX
            }
        }
#elif defined(HOST_ARM64)
        if (sve && sve->head.size >= SVE_SIG_CONTEXT_SIZE(sve_vq_from_vl(sve->vl)))
        {
            //TODO-SVE: This only handles vector lengths of 128bits.
            if (CONTEXT_GetSveLengthFromOS() == 16)
            {
                _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_ARM64_SVE) == XSTATE_MASK_ARM64_SVE);

                uint16_t vq = sve_vq_from_vl(lpContext->Vl);

                // Vector length should not have changed.
                _ASSERTE(lpContext->Vl == sve->vl);

                //Note: Size of ffr register is SVE_SIG_FFR_SIZE(vq) bytes.
                *(WORD*) (((uint8_t*)sve) + SVE_SIG_FFR_OFFSET(vq)) = lpContext->Ffr;

                //TODO-SVE: Copy SVE registers once they are >128bits
                //Note: Size of a Z register is SVE_SIG_ZREGS_SIZE(vq) bytes.

                for (int i = 0; i < 16; i++)
                {
                    //Note: Size of a P register is SVE_SIG_PREGS_SIZE(vq) bytes.
                    *(WORD*) (((uint8_t*)sve) + SVE_SIG_PREG_OFFSET(vq, i)) = lpContext->P[i];
                }
            }
        }
#endif //HOST_AMD64
    }
#endif //XSTATE_SUPPORTED

}

#if defined(HOST_64BIT) && defined(HOST_ARM64) && !defined(TARGET_FREEBSD) && !defined(__APPLE__)
/*++
Function :
    _GetNativeSigSimdContext

    Finds the FP and SVE context from the reserved data section of a native context.

Parameters :
    uint8_t *data : native context reserved data.
    uint32_t size : size of the reserved data.
    fpsimd_context **fp_ptr : returns a pointer to the FP context.
    sve_context **sve_ptr : returns a pointer to the SVE context.

Return value :
    None.

--*/
void _GetNativeSigSimdContext(uint8_t *data, uint32_t size, fpsimd_context **fp_ptr, sve_context **sve_ptr)
{
    size_t position = 0;
    fpsimd_context *fp = nullptr;
    sve_context *sve = nullptr;
    extra_context *extra = nullptr;
    bool done = false;

    while (!done)
    {
        _aarch64_ctx *ctx = reinterpret_cast<_aarch64_ctx *>(&data[position]);

        _ASSERTE(position + ctx->size <= size);

        switch (ctx->magic)
        {
            case FPSIMD_MAGIC:
                _ASSERTE(fp == nullptr);
                _ASSERTE(ctx->size >= sizeof(fpsimd_context));
                fp = reinterpret_cast<fpsimd_context *>(&data[position]);
                break;

            case SVE_MAGIC:
                _ASSERTE(sve == nullptr);
                _ASSERTE(ctx->size >= sizeof(sve_context));
                sve = reinterpret_cast<sve_context *>(&data[position]);
                break;

            case EXTRA_MAGIC:
            {
                // Points to an additional section of reserved data.
                _ASSERTE(extra == nullptr);
                _ASSERTE(ctx->size >= sizeof(extra_context));
                fpsimd_context *fpOrig = fp;
                sve_context *sveOrig = sve;

                extra = reinterpret_cast<extra_context *>(&data[position]);
                _GetNativeSigSimdContext((uint8_t*)extra->datap, extra->size, &fp, &sve);

                // There should only be one block of each type.
                _ASSERTE(fpOrig == nullptr || fp == fpOrig);
                _ASSERTE(sveOrig == nullptr || sve == sveOrig);
                break;
            }

            case 0:
                _ASSERTE(ctx->size == 0);
                done = true;
                break;

            default:
                // Any other section.
                _ASSERTE(ctx->size != 0);
                break;
        }

        position += ctx->size;
    }

    if (fp)
    {
        *fp_ptr = fp;
    }
    if (sve)
    {
        // If this ever fires then we have an SVE context but no FP context. Given that V and Z
        // registers overlap, then when propagating this data to other structures, the SVE
        // context should be used to fill the FP data.
        _ASSERTE(fp != nullptr);

        *sve_ptr = sve;
    }
}
#endif // HOST_64BIT && HOST_ARM64 && !TARGET_FREEBSD && !__APPLE__

/*++
Function :
    CONTEXTFromNativeContext

    Converts a native context to a CONTEXT record.

Parameters :
    const native_context_t *native : native context to convert
    LPCONTEXT lpContext : CONTEXT to fill in
    ULONG contextFlags : flags that determine which registers are valid in
                         native and which ones to set in lpContext

Return value :
    None

--*/
void CONTEXTFromNativeContext(const native_context_t *native, LPCONTEXT lpContext,
                              ULONG contextFlags)
{
    lpContext->ContextFlags = contextFlags;

#define ASSIGN_REG(reg) lpContext->reg = MCREG_##reg(native->uc_mcontext);
    if ((contextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL)
    {
        ASSIGN_CONTROL_REGS
#if defined(HOST_ARM)
        // WinContext assumes that the least bit of Pc is always 1 (denoting thumb)
        // although the pc value retrieved from native context might not have set the least bit.
        // This becomes especially problematic if the context is on the JIT_WRITEBARRIER.
        lpContext->Pc |= 0x1;
#endif
    }

    if ((contextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
    {
        ASSIGN_INTEGER_REGS
    }
#undef ASSIGN_REG

#if !HAVE_FPREGS_WITH_CW
#if (HAVE_GREGSET_T || HAVE___GREGSET_T) && !defined(HOST_S390X) && !defined(HOST_LOONGARCH64) && !defined(HOST_RISCV64) && !defined(HOST_POWERPC64)
#if HAVE_GREGSET_T
    if (native->uc_mcontext.fpregs == nullptr)
#elif HAVE___GREGSET_T
    if (native->uc_mcontext.__fpregs == nullptr)
#endif // HAVE_GREGSET_T
    {
        // Reset the CONTEXT_FLOATING_POINT bit(s) and the CONTEXT_XSTATE bit(s) so it's
        // clear that the floating point and extended state data in the CONTEXT is not
        // valid. Since these flags are defined as the architecture bit(s) OR'd with one
        // or more other bits, we first get the bits that are unique to each by resetting
        // the architecture bits. We determine what those are by inverting the union of
        // CONTEXT_CONTROL and CONTEXT_INTEGER, both of which should also have the
        // architecture bit(s) set.
        const ULONG floatingPointFlags = CONTEXT_FLOATING_POINT & ~(CONTEXT_CONTROL & CONTEXT_INTEGER);
        const ULONG xstateFlags = CONTEXT_XSTATE & ~(CONTEXT_CONTROL & CONTEXT_INTEGER);

        lpContext->ContextFlags &= ~(floatingPointFlags | xstateFlags);

        // Bail out regardless of whether the caller wanted CONTEXT_FLOATING_POINT or CONTEXT_XSTATE
        return;
    }
#endif // (HAVE_GREGSET_T || HAVE___GREGSET_T) && !HOST_S390X && !HOST_LOONGARCH64 && !HOST_RISCV64 && !HOST_POWERPC64 && !HOST_POWERPC64
#endif // !HAVE_FPREGS_WITH_CW

#if defined(HOST_ARM64) && !defined(__APPLE__) && !defined(TARGET_FREEBSD)
    const fpsimd_context* fp = nullptr;
    const sve_context* sve = nullptr;
    if (((lpContext->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT) ||
        ((lpContext->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE))
    {
        GetConstNativeSigSimdContext(native, &fp, &sve);
    }
#endif // HOST_ARM64 && !__APPLE__ && !TARGET_FREEBSD

    if ((contextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT)
    {
#ifdef HOST_AMD64
        lpContext->FltSave.ControlWord = FPREG_ControlWord(native);
        lpContext->FltSave.StatusWord = FPREG_StatusWord(native);
#if HAVE_FPREGS_WITH_CW
        lpContext->FltSave.TagWord = ((DWORD)FPREG_TagWord1(native) << 8) | FPREG_TagWord2(native);
#else
        lpContext->FltSave.TagWord = FPREG_TagWord(native);
#endif
        lpContext->FltSave.ErrorOffset = FPREG_ErrorOffset(native);
        lpContext->FltSave.ErrorSelector = FPREG_ErrorSelector(native);
        lpContext->FltSave.DataOffset = FPREG_DataOffset(native);
        lpContext->FltSave.DataSelector = FPREG_DataSelector(native);
        lpContext->FltSave.MxCsr = FPREG_MxCsr(native);
        lpContext->FltSave.MxCsr_Mask = FPREG_MxCsr_Mask(native);

        for (int i = 0; i < 8; i++)
        {
            lpContext->FltSave.FloatRegisters[i] = FPREG_St(native, i);
        }

        for (int i = 0; i < 16; i++)
        {
            lpContext->FltSave.XmmRegisters[i] = FPREG_Xmm(native, i);
        }
#elif defined(HOST_ARM64)
#ifdef __APPLE__
        const _STRUCT_ARM_NEON_STATE64* fp = GetConstNativeSigSimdContext(native);
        lpContext->Fpsr = fp->__fpsr;
        lpContext->Fpcr = fp->__fpcr;
        for (int i = 0; i < 32; i++)
        {
            lpContext->V[i] = *(NEON128*) &fp->__v[i];
        }
#elif defined(TARGET_FREEBSD)
        const struct fpregs* fp = GetConstNativeSigSimdContext(native);
        if (fp)
        {
            lpContext->Fpsr = fp->fp_sr;
            lpContext->Fpcr = fp->fp_cr;
            for (int i = 0; i < 32; i++)
            {
                lpContext->V[i] = *(NEON128*) &fp->fp_q[i];
            }
        }
#else // __APPLE__
        if (fp)
        {
            lpContext->Fpsr = fp->fpsr;
            lpContext->Fpcr = fp->fpcr;
            for (int i = 0; i < 32; i++)
            {
                lpContext->V[i] = *(NEON128*) &fp->vregs[i];
            }
        }
#endif // __APPLE__
#elif defined(HOST_ARM)
        const VfpSigFrame* fp = GetConstNativeSigSimdContext(native);
        if (fp)
        {
            lpContext->Fpscr = fp->Fpscr;
            for (int i = 0; i < 32; i++)
            {
                lpContext->D[i] = fp->D[i];
            }
        }
        else
        {
            // Floating point state is not valid
            // Mark the context correctly
            lpContext->ContextFlags &= ~(ULONG)CONTEXT_FLOATING_POINT;
        }
#elif defined(HOST_S390X)
        const fpregset_t *fp = &native->uc_mcontext.fpregs;
        static_assert_no_msg(sizeof(fp->fprs) == sizeof(lpContext->Fpr));
        memcpy(lpContext->Fpr, fp->fprs, sizeof(lpContext->Fpr));
#elif defined(HOST_LOONGARCH64)
        struct sctx_info* info = (struct sctx_info*) native->uc_mcontext.__extcontext;
        if (FPU_CTX_MAGIC == info->magic)
        {
            struct fpu_context* fpr = (struct fpu_context*)(info + 1);
            lpContext->Fcsr = fpr->fcsr;
            lpContext->Fcc  = fpr->fcc;
            memcpy(lpContext->F, fpr->regs, sizeof(fpr->regs));
        }
        else if (LSX_CTX_MAGIC == info->magic)
        {
            struct lsx_context* fpr = (struct lsx_context*)(info + 1);
            lpContext->Fcsr = fpr->fcsr;
            lpContext->Fcc  = fpr->fcc;
            memcpy(lpContext->F, fpr->regs, sizeof(fpr->regs));
            lpContext->ContextFlags |= CONTEXT_LSX;
        }
        else if (LASX_CTX_MAGIC == info->magic)
        {
            struct lasx_context* fpr = (struct lasx_context*)(info + 1);
            lpContext->Fcsr = fpr->fcsr;
            lpContext->Fcc  = fpr->fcc;
            memcpy(lpContext->F, fpr->regs, sizeof(fpr->regs));
            lpContext->ContextFlags |= CONTEXT_LASX;
        }
        else
        {
            _ASSERTE(LBT_CTX_MAGIC == info->magic);
        }

#elif defined(HOST_RISCV64)
        lpContext->Fcsr = native->uc_mcontext.__fpregs.__d.__fcsr;
        for (int i = 0; i < 32; i++)
        {
            lpContext->F[i] = native->uc_mcontext.__fpregs.__d.__f[i];
        }
#endif
    }

#if defined(HOST_AMD64) || defined(HOST_ARM64)
    if ((contextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE)
    {
    // TODO: Enable for all Unix systems
#if defined(XSTATE_SUPPORTED)
#if defined(HOST_AMD64)
        if (FPREG_HasYmmRegisters(native))
        {
            uint32_t size;
            void *src;

            src = FPREG_Xstate_Ymmh(native, &size);
            _ASSERT(size == (sizeof(M128A) * 16));
            memcpy_s(&lpContext->Ymm0H, sizeof(M128A) * 16, src, sizeof(M128A) * 16);

            lpContext->XStateFeaturesMask |= XSTATE_MASK_AVX;

            if (FPREG_HasAvx512Registers(native))
            {
                src = FPREG_Xstate_Opmask(native, &size);
                _ASSERT(size == (sizeof(DWORD64) * 8));
                memcpy_s(&lpContext->KMask0, sizeof(DWORD64) * 8, src, sizeof(DWORD64) * 8);

                src = FPREG_Xstate_ZmmHi256(native, &size);
                _ASSERT(size == (sizeof(M256) * 16));
                memcpy_s(&lpContext->Zmm0H, sizeof(M256) * 16, src, sizeof(M256) * 16);

                src = FPREG_Xstate_Hi16Zmm(native, &size);
                _ASSERT(size == (sizeof(M512) * 16));
                memcpy_s(&lpContext->Zmm16, sizeof(M512) * 16, src, sizeof(M512) * 16);

                lpContext->XStateFeaturesMask |= XSTATE_MASK_AVX512;
            }
#if !defined(TARGET_OSX)
            if (FPREG_HasApxRegisters(native))
            {
                src = FPREG_Xstate_Egpr(native, &size);
                _ASSERT(size == (sizeof(DWORD64) * 16));
                memcpy_s(&lpContext->R16, sizeof(DWORD64) * 16, src, sizeof(DWORD64) * 16);

                lpContext->XStateFeaturesMask |= XSTATE_MASK_APX;
            }
#endif // TARGET_OSX
        }
#elif defined(HOST_ARM64)
        if (sve && sve->head.size >= SVE_SIG_CONTEXT_SIZE(sve_vq_from_vl(sve->vl)))
        {
            //TODO-SVE: This only handles vector lengths of 128bits.
            if (CONTEXT_GetSveLengthFromOS() == 16)
            {
                _ASSERTE((sve->vl > 0) && (sve->vl % 16 == 0));
                lpContext->Vl  = sve->vl;

                uint16_t vq = sve_vq_from_vl(sve->vl);

                lpContext->XStateFeaturesMask |= XSTATE_MASK_ARM64_SVE;

                //Note: Size of ffr register is SVE_SIG_FFR_SIZE(vq) bytes.
                lpContext->Ffr = *(WORD*) (((uint8_t*)sve) + SVE_SIG_FFR_OFFSET(vq));

                //TODO-SVE: Copy SVE registers once they are >128bits
                //Note: Size of a Z register is SVE_SIG_ZREGS_SIZE(vq) bytes.

                for (int i = 0; i < 16; i++)
                {
                    //Note: Size of a P register is SVE_SIG_PREGS_SIZE(vq) bytes.
                    lpContext->P[i] = *(WORD*) (((uint8_t*)sve) + SVE_SIG_PREG_OFFSET(vq, i));
                }
            }
        }
#endif // HOST_AMD64
        else
#endif // XSTATE_SUPPORTED
        {
            // Reset the CONTEXT_XSTATE bit(s) so it's clear that the extended state data in
            // the CONTEXT is not valid.
            const ULONG xstateFlags = CONTEXT_XSTATE & ~(CONTEXT_CONTROL & CONTEXT_INTEGER);
            lpContext->ContextFlags &= ~xstateFlags;
        }
    }
#endif // HOST_AMD64 || HOST_ARM64
}

#if !HAVE_MACH_EXCEPTIONS

/*++
Function :
    GetNativeContextPC

    Returns the program counter from the native context.

Parameters :
    const native_context_t *native : native context

Return value :
    The program counter from the native context.

--*/
LPVOID GetNativeContextPC(const native_context_t *context)
{
#ifdef HOST_AMD64
    return (LPVOID)MCREG_Rip(context->uc_mcontext);
#elif defined(HOST_X86)
    return (LPVOID) MCREG_Eip(context->uc_mcontext);
#elif defined(HOST_S390X)
    return (LPVOID) MCREG_PSWAddr(context->uc_mcontext);
#elif defined(HOST_POWERPC64)
    return (LPVOID) MCREG_Nip(context->uc_mcontext);
#else
    return (LPVOID) MCREG_Pc(context->uc_mcontext);
#endif
}

/*++
Function :
    GetNativeContextSP

    Returns the stack pointer from the native context.

Parameters :
    const native_context_t *native : native context

Return value :
    The stack pointer from the native context.

--*/
LPVOID GetNativeContextSP(const native_context_t *context)
{
#ifdef HOST_AMD64
    return (LPVOID)MCREG_Rsp(context->uc_mcontext);
#elif defined(HOST_X86)
    return (LPVOID) MCREG_Esp(context->uc_mcontext);
#elif defined(HOST_S390X)
    return (LPVOID) MCREG_R15(context->uc_mcontext);
#elif defined(HOST_POWERPC64)
    return (LPVOID) MCREG_R31(context->uc_mcontext);
#else
    return (LPVOID) MCREG_Sp(context->uc_mcontext);
#endif
}


/*++
Function :
    CONTEXTGetExceptionCodeForSignal

    Translates signal and context information to a Win32 exception code.

Parameters :
    const siginfo_t *siginfo : signal information from a signal handler
    const native_context_t *context : context information

Return value :
    The Win32 exception code that corresponds to the signal and context
    information.

--*/
#ifdef ILL_ILLOPC
// If si_code values are available for all signals, use those.
DWORD CONTEXTGetExceptionCodeForSignal(const siginfo_t *siginfo,
                                       const native_context_t *context)
{
    // IMPORTANT NOTE: This function must not call any signal unsafe functions
    // since it is called from signal handlers.
    // That includes ASSERT and TRACE macros.

    switch (siginfo->si_signo)
    {
        case SIGILL:
            switch (siginfo->si_code)
            {
                case ILL_ILLOPC:    // Illegal opcode
                case ILL_ILLOPN:    // Illegal operand
                case ILL_ILLADR:    // Illegal addressing mode
                case ILL_ILLTRP:    // Illegal trap
                case ILL_COPROC:    // Co-processor error
                    return EXCEPTION_ILLEGAL_INSTRUCTION;
                case ILL_PRVOPC:    // Privileged opcode
                case ILL_PRVREG:    // Privileged register
                    return EXCEPTION_PRIV_INSTRUCTION;
                case ILL_BADSTK:    // Internal stack error
                    return EXCEPTION_STACK_OVERFLOW;
                default:
                    break;
            }
            break;
        case SIGFPE:
            switch (siginfo->si_code)
            {
                case FPE_INTDIV:
                    return EXCEPTION_INT_DIVIDE_BY_ZERO;
                case FPE_INTOVF:
                    return EXCEPTION_INT_OVERFLOW;
                case FPE_FLTDIV:
                    return EXCEPTION_FLT_DIVIDE_BY_ZERO;
                case FPE_FLTOVF:
                    return EXCEPTION_FLT_OVERFLOW;
                case FPE_FLTUND:
                    return EXCEPTION_FLT_UNDERFLOW;
                case FPE_FLTRES:
                    return EXCEPTION_FLT_INEXACT_RESULT;
                case FPE_FLTINV:
                    return EXCEPTION_FLT_INVALID_OPERATION;
                case FPE_FLTSUB:
                    return EXCEPTION_FLT_INVALID_OPERATION;
                default:
                    break;
            }
            break;
        case SIGSEGV:
            switch (siginfo->si_code)
            {
                case SI_USER:       // User-generated signal, sometimes sent
                                    // for SIGSEGV under normal circumstances
                case SEGV_MAPERR:   // Address not mapped to object
                case SEGV_ACCERR:   // Invalid permissions for mapped object
                    return EXCEPTION_ACCESS_VIOLATION;

#ifdef SI_KERNEL
                case SI_KERNEL:
                {
                    // Identify privileged instructions that are not identified as such by the system
                    if (g_getGcMarkerExceptionCode != nullptr)
                    {
                        DWORD exceptionCode = g_getGcMarkerExceptionCode(GetNativeContextPC(context));
                        if (exceptionCode != 0)
                        {
                            return exceptionCode;
                        }
                    }
                    return EXCEPTION_ACCESS_VIOLATION;
                }
#endif
                default:
                    break;
            }
            break;
        case SIGBUS:
            switch (siginfo->si_code)
            {
                case BUS_ADRALN:    // Invalid address alignment
                    return EXCEPTION_DATATYPE_MISALIGNMENT;
                case BUS_ADRERR:    // Non-existent physical address
                    return EXCEPTION_ACCESS_VIOLATION;
                case BUS_OBJERR:    // Object-specific hardware error
                default:
                    break;
            }
            break;
        case SIGTRAP:
            switch (siginfo->si_code)
            {
#ifdef SI_KERNEL
                case SI_KERNEL:
#endif
                case SI_USER:
                case TRAP_BRKPT:    // Process breakpoint
                    return EXCEPTION_BREAKPOINT;
                case TRAP_TRACE:    // Process trace trap
                    return EXCEPTION_SINGLE_STEP;
                default:
                    // Got unknown SIGTRAP signal with code siginfo->si_code;
                    return EXCEPTION_ILLEGAL_INSTRUCTION;
            }
        default:
            break;
    }

    // Got unknown signal number siginfo->si_signo with code siginfo->si_code;
    return EXCEPTION_ILLEGAL_INSTRUCTION;
}
#else   // ILL_ILLOPC
DWORD CONTEXTGetExceptionCodeForSignal(const siginfo_t *siginfo,
                                       const native_context_t *context)
{
    // IMPORTANT NOTE: This function must not call any signal unsafe functions
    // since it is called from signal handlers.
    // That includes ASSERT and TRACE macros.

    int trap;

    if (siginfo->si_signo == SIGFPE)
    {
        // Floating point exceptions are mapped by their si_code.
        switch (siginfo->si_code)
        {
            case FPE_INTDIV :
                return EXCEPTION_INT_DIVIDE_BY_ZERO;
            case FPE_INTOVF :
                return EXCEPTION_INT_OVERFLOW;
            case FPE_FLTDIV :
                return EXCEPTION_FLT_DIVIDE_BY_ZERO;
            case FPE_FLTOVF :
                return EXCEPTION_FLT_OVERFLOW;
            case FPE_FLTUND :
                return EXCEPTION_FLT_UNDERFLOW;
            case FPE_FLTRES :
                return EXCEPTION_FLT_INEXACT_RESULT;
            case FPE_FLTINV :
                return EXCEPTION_FLT_INVALID_OPERATION;
            case FPE_FLTSUB :/* subscript out of range */
                return EXCEPTION_FLT_INVALID_OPERATION;
            default:
                // Got unknown signal code siginfo->si_code;
                return 0;
        }
    }

    trap = context->uc_mcontext.mc_trapno;
    switch (trap)
    {
        case T_PRIVINFLT : /* privileged instruction */
            return EXCEPTION_PRIV_INSTRUCTION;
        case T_BPTFLT :    /* breakpoint instruction */
            return EXCEPTION_BREAKPOINT;
        case T_ARITHTRAP : /* arithmetic trap */
            return 0;      /* let the caller pick an exception code */
#ifdef T_ASTFLT
        case T_ASTFLT :    /* system forced exception : ^C, ^\. SIGINT signal
                              handler shouldn't be calling this function, since
                              it doesn't need an exception code */
            // Trap code T_ASTFLT received, shouldn't get here;
            return 0;
#endif  // T_ASTFLT
        case T_PROTFLT :   /* protection fault */
            return EXCEPTION_ACCESS_VIOLATION;
        case T_TRCTRAP :   /* debug exception (sic) */
            return EXCEPTION_SINGLE_STEP;
        case T_PAGEFLT :   /* page fault */
            return EXCEPTION_ACCESS_VIOLATION;
        case T_ALIGNFLT :  /* alignment fault */
            return EXCEPTION_DATATYPE_MISALIGNMENT;
        case T_DIVIDE :
            return EXCEPTION_INT_DIVIDE_BY_ZERO;
        case T_NMI :       /* non-maskable trap */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        case T_OFLOW :
            return EXCEPTION_INT_OVERFLOW;
        case T_BOUND :     /* bound instruction fault */
            return EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        case T_DNA :       /* device not available fault */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        case T_DOUBLEFLT : /* double fault */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        case T_FPOPFLT :   /* fp coprocessor operand fetch fault */
            return EXCEPTION_FLT_INVALID_OPERATION;
        case T_TSSFLT :    /* invalid tss fault */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        case T_SEGNPFLT :  /* segment not present fault */
            return EXCEPTION_ACCESS_VIOLATION;
        case T_STKFLT :    /* stack fault */
            return EXCEPTION_STACK_OVERFLOW;
        case T_MCHK :      /* machine check trap */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        case T_RESERVED :  /* reserved (unknown) */
            return EXCEPTION_ILLEGAL_INSTRUCTION;
        default:
            // Got unknown trap code trap;
            break;
    }
    return EXCEPTION_ILLEGAL_INSTRUCTION;
}
#endif  // ILL_ILLOPC

#else // !HAVE_MACH_EXCEPTIONS

#include <mach/message.h>
#include <mach/thread_act.h>
#include "../exception/machexception.h"

/*++
Function:
  CONTEXT_GetThreadContextFromPort

  Helper for GetThreadContext that uses a mach_port
--*/
kern_return_t
CONTEXT_GetThreadContextFromPort(
    mach_port_t Port,
    LPCONTEXT lpContext)
{
    // Extract the CONTEXT from the Mach thread.

    kern_return_t MachRet = KERN_SUCCESS;
    mach_msg_type_number_t StateCount;
    thread_state_flavor_t StateFlavor;

#if defined(HOST_AMD64)
    if (lpContext->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS) & CONTEXT_AREA_MASK)
    {
        x86_thread_state64_t State;
        StateFlavor = x86_THREAD_STATE64;
#elif defined(HOST_ARM64)
    if (lpContext->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER) & CONTEXT_AREA_MASK)
    {
        arm_thread_state64_t State;
        StateFlavor = ARM_THREAD_STATE64;
#else
#error Unexpected architecture.
#endif
        StateCount = sizeof(State) / sizeof(natural_t);
        MachRet = thread_get_state(Port, StateFlavor, (thread_state_t)&State, &StateCount);
        if (MachRet != KERN_SUCCESS)
        {
            ASSERT("thread_get_state(THREAD_STATE) failed: %d\n", MachRet);
            goto exit;
        }

        CONTEXT_GetThreadContextFromThreadState(StateFlavor, (thread_state_t)&State, lpContext);
    }

    if (lpContext->ContextFlags & CONTEXT_ALL_FLOATING & CONTEXT_AREA_MASK)
    {
#if defined(HOST_AMD64)
        // The thread_get_state for floating point state can fail for some flavors when the processor is not
        // in the right mode at the time we are taking the state. So we will try to get the AVX state first and
        // if it fails, get the FLOAT state and if that fails, take AVX512 state. Both AVX and AVX512 states
        // are supersets of the FLOAT state.
        // Check a few fields to make sure the assumption is correct.
        static_assert_no_msg(sizeof(x86_avx_state64_t) > sizeof(x86_float_state64_t));
        static_assert_no_msg(sizeof(x86_avx512_state64_t) > sizeof(x86_avx_state64_t));
        static_assert_no_msg(offsetof(x86_avx_state64_t, __fpu_fcw) == offsetof(x86_float_state64_t, __fpu_fcw));
        static_assert_no_msg(offsetof(x86_avx_state64_t, __fpu_xmm0) == offsetof(x86_float_state64_t, __fpu_xmm0));
        static_assert_no_msg(offsetof(x86_avx512_state64_t, __fpu_fcw) == offsetof(x86_float_state64_t, __fpu_fcw));
        static_assert_no_msg(offsetof(x86_avx512_state64_t, __fpu_xmm0) == offsetof(x86_float_state64_t, __fpu_xmm0));

        x86_avx512_state64_t State;

        StateFlavor = x86_AVX512_STATE64;
        StateCount = sizeof(x86_avx512_state64_t) / sizeof(natural_t);
        MachRet = thread_get_state(Port, StateFlavor, (thread_state_t)&State, &StateCount);

        if (MachRet != KERN_SUCCESS)
        {
            // The AVX512 state is not available, try to get the AVX state.
            lpContext->XStateFeaturesMask &= ~XSTATE_MASK_AVX512;

            StateFlavor = x86_AVX_STATE64;
            StateCount = sizeof(x86_avx_state64_t) / sizeof(natural_t);
            MachRet = thread_get_state(Port, StateFlavor, (thread_state_t)&State, &StateCount);

            if (MachRet != KERN_SUCCESS)
            {
                // Neither the AVX512 nor the AVX state is not available, try to get at least the FLOAT state.
                lpContext->XStateFeaturesMask &= ~XSTATE_MASK_AVX;
                lpContext->ContextFlags &= ~(CONTEXT_XSTATE & CONTEXT_AREA_MASK);

                StateFlavor = x86_FLOAT_STATE64;
                StateCount = sizeof(x86_float_state64_t) / sizeof(natural_t);
                MachRet = thread_get_state(Port, StateFlavor, (thread_state_t)&State, &StateCount);

                if (MachRet != KERN_SUCCESS)
                {
                    // We were unable to get any floating point state. This case was observed on OSX with AVX512 capable processors.
                    lpContext->ContextFlags &= ~((CONTEXT_XSTATE | CONTEXT_ALL_FLOATING) & CONTEXT_AREA_MASK);
                }
            }
        }
#elif defined(HOST_ARM64)
        arm_neon_state64_t State;

        StateFlavor = ARM_NEON_STATE64;
        StateCount = sizeof(arm_neon_state64_t) / sizeof(natural_t);
        MachRet = thread_get_state(Port, StateFlavor, (thread_state_t)&State, &StateCount);
        if (MachRet != KERN_SUCCESS)
        {
            // We were unable to get any floating point state.
            lpContext->ContextFlags &= ~((CONTEXT_ALL_FLOATING) & CONTEXT_AREA_MASK);
        }
#else
#error Unexpected architecture.
#endif

        CONTEXT_GetThreadContextFromThreadState(StateFlavor, (thread_state_t)&State, lpContext);
    }

exit:
    return MachRet;
}

/*++
Function:
  CONTEXT_GetThreadContextFromThreadState

--*/
void
CONTEXT_GetThreadContextFromThreadState(
    thread_state_flavor_t threadStateFlavor,
    thread_state_t threadState,
    LPCONTEXT lpContext)
{
    switch (threadStateFlavor)
    {
#if defined (HOST_AMD64)
        case x86_THREAD_STATE64:
            if (lpContext->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS) & CONTEXT_AREA_MASK)
            {
                x86_thread_state64_t *pState = (x86_thread_state64_t *)threadState;

                lpContext->Rax = pState->__rax;
                lpContext->Rbx = pState->__rbx;
                lpContext->Rcx = pState->__rcx;
                lpContext->Rdx = pState->__rdx;
                lpContext->Rdi = pState->__rdi;
                lpContext->Rsi = pState->__rsi;
                lpContext->Rbp = pState->__rbp;
                lpContext->Rsp = pState->__rsp;
                lpContext->R8 = pState->__r8;
                lpContext->R9 = pState->__r9;
                lpContext->R10 = pState->__r10;
                lpContext->R11 = pState->__r11;
                lpContext->R12 = pState->__r12;
                lpContext->R13 = pState->__r13;
                lpContext->R14 = pState->__r14;
                lpContext->R15 = pState->__r15;
                lpContext->EFlags = pState->__rflags;
                lpContext->Rip = pState->__rip;
                lpContext->SegCs = pState->__cs;
                // RtlRestoreContext uses the actual ss instead of this one
                // to build the iret frame so just set it zero.
                lpContext->SegSs = 0;
                lpContext->SegDs = 0;
                lpContext->SegEs = 0;
                lpContext->SegFs = pState->__fs;
                lpContext->SegGs = pState->__gs;
            }
            break;

        case x86_AVX512_STATE64:
        {
            if (lpContext->ContextFlags & CONTEXT_XSTATE & CONTEXT_AREA_MASK)
            {
                if (Xstate_IsAvx512Supported())
                {
                    x86_avx512_state64_t *pState = (x86_avx512_state64_t *)threadState;

                    memcpy(&lpContext->KMask0, &pState->__fpu_k0, sizeof(_STRUCT_OPMASK_REG) * 8);
                    memcpy(&lpContext->Zmm0H, &pState->__fpu_zmmh0, sizeof(_STRUCT_YMM_REG) * 16);
                    memcpy(&lpContext->Zmm16, &pState->__fpu_zmm16, sizeof(_STRUCT_ZMM_REG) * 16);

                    lpContext->XStateFeaturesMask |= XSTATE_MASK_AVX512;
                }
            }

            // Intentional fall-through, the AVX512 states are supersets of the AVX state
            FALLTHROUGH;
        }

        case x86_AVX_STATE64:
        {
            if (lpContext->ContextFlags & CONTEXT_XSTATE & CONTEXT_AREA_MASK)
            {
                x86_avx_state64_t *pState = (x86_avx_state64_t *)threadState;
                memcpy(&lpContext->Ymm0H, &pState->__fpu_ymmh0, sizeof(_STRUCT_XMM_REG) * 16);
                lpContext->XStateFeaturesMask |= XSTATE_MASK_AVX;
            }

            // Intentional fall-through, the AVX states are supersets of the FLOAT state
            FALLTHROUGH;
        }

        case x86_FLOAT_STATE64:
        {
            if (lpContext->ContextFlags & CONTEXT_FLOATING_POINT & CONTEXT_AREA_MASK)
            {
                x86_float_state64_t *pState = (x86_float_state64_t *)threadState;

                lpContext->FltSave.ControlWord = *(DWORD*)&pState->__fpu_fcw;
                lpContext->FltSave.StatusWord = *(DWORD*)&pState->__fpu_fsw;
                lpContext->FltSave.TagWord = pState->__fpu_ftw;
                lpContext->FltSave.ErrorOffset = pState->__fpu_ip;
                lpContext->FltSave.ErrorSelector = pState->__fpu_cs;
                lpContext->FltSave.DataOffset = pState->__fpu_dp;
                lpContext->FltSave.DataSelector = pState->__fpu_ds;
                lpContext->FltSave.MxCsr = pState->__fpu_mxcsr;
                lpContext->FltSave.MxCsr_Mask = pState->__fpu_mxcsrmask; // note: we don't save the mask for x86

                // Windows stores the floating point registers in a packed layout (each 10-byte register end to end
                // for a total of 80 bytes). But Mach returns each register in an 16-bit structure (presumably for
                // alignment purposes). So we can't just memcpy the registers over in a single block, we need to copy
                // them individually.
                for (int i = 0; i < 8; i++)
                    memcpy(&lpContext->FltSave.FloatRegisters[i], (&pState->__fpu_stmm0)[i].__mmst_reg, 10);

                // AMD64's FLOATING_POINT includes the xmm registers.
                memcpy(&lpContext->Xmm0, &pState->__fpu_xmm0, 16 * 16);

                if (threadStateFlavor == x86_FLOAT_STATE64)
                {
                     // There was just a floating point state, so make sure the CONTEXT_XSTATE is not set
                     lpContext->ContextFlags &= ~(CONTEXT_XSTATE & CONTEXT_AREA_MASK);
                }
            }
            break;
        }

        case x86_THREAD_STATE:
        {
            x86_thread_state_t *pState = (x86_thread_state_t *)threadState;
            CONTEXT_GetThreadContextFromThreadState((thread_state_flavor_t)pState->tsh.flavor, (thread_state_t)&pState->uts, lpContext);
        }
        break;

        case x86_FLOAT_STATE:
        {
            x86_float_state_t *pState = (x86_float_state_t *)threadState;
            CONTEXT_GetThreadContextFromThreadState((thread_state_flavor_t)pState->fsh.flavor, (thread_state_t)&pState->ufs, lpContext);
        }
        break;
        case x86_AVX_STATE:
        {
            x86_avx_state_t *pState = (x86_avx_state_t *)threadState;
            CONTEXT_GetThreadContextFromThreadState((thread_state_flavor_t)pState->ash.flavor, (thread_state_t)&pState->ufs, lpContext);
        }
        break;
        case x86_AVX512_STATE:
        {
            x86_avx512_state_t *pState = (x86_avx512_state_t *)threadState;
            CONTEXT_GetThreadContextFromThreadState((thread_state_flavor_t)pState->ash.flavor, (thread_state_t)&pState->ufs, lpContext);
        }
        break;
#elif defined(HOST_ARM64)
        case ARM_THREAD_STATE64:
            if (lpContext->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER) & CONTEXT_AREA_MASK)
            {
                arm_thread_state64_t *pState = (arm_thread_state64_t*)threadState;
                memcpy(&lpContext->X0, &pState->__x[0], 29 * 8);
                lpContext->Cpsr = pState->__cpsr;
                lpContext->Fp = arm_thread_state64_get_fp(*pState);
                lpContext->Sp = arm_thread_state64_get_sp(*pState);
                lpContext->Lr = (uint64_t)arm_thread_state64_get_lr_fptr(*pState);
                lpContext->Pc = (uint64_t)arm_thread_state64_get_pc_fptr(*pState);
            }
            break;
        case ARM_NEON_STATE64:
            if (lpContext->ContextFlags & CONTEXT_FLOATING_POINT & CONTEXT_AREA_MASK)
            {
                arm_neon_state64_t *pState = (arm_neon_state64_t*)threadState;
                memcpy(&lpContext->V[0], &pState->__v, 32 * 16);
                lpContext->Fpsr = pState->__fpsr;
                lpContext->Fpcr = pState->__fpcr;
            }
            break;
#else
#error Unexpected architecture.
#endif

        default:
            ASSERT("Invalid thread state flavor %d\n", threadStateFlavor);
            break;
    }
}

/*++
Function:
  GetThreadContext

See MSDN doc.
--*/
BOOL
CONTEXT_GetThreadContext(
         DWORD dwProcessId,
         pthread_t self,
         LPCONTEXT lpContext)
{
    BOOL ret = FALSE;

    if (lpContext == NULL)
    {
        ERROR("Invalid lpContext parameter value\n");
        SetLastError(ERROR_NOACCESS);
        goto EXIT;
    }

    if (GetCurrentProcessId() == dwProcessId)
    {
        if (self != pthread_self())
        {
            // the target thread is in the current process, but isn't
            // the current one: extract the CONTEXT from the Mach thread.
            mach_port_t mptPort;
            mptPort = pthread_mach_thread_np(self);

            ret = (CONTEXT_GetThreadContextFromPort(mptPort, lpContext) == KERN_SUCCESS);
        }
        else
        {
            CONTEXT_CaptureContext(lpContext);
            ret = TRUE;
        }
    }
    else
    {
        ASSERT("Cross-process GetThreadContext() is not supported on this platform\n");
        SetLastError(ERROR_NOACCESS);
    }

EXIT:
    return ret;
}

/*++
Function:
  SetThreadContextOnPort

  Helper for CONTEXT_SetThreadContext
--*/
kern_return_t
CONTEXT_SetThreadContextOnPort(
           mach_port_t Port,
           IN CONST CONTEXT *lpContext)
{
    kern_return_t MachRet = KERN_SUCCESS;
    mach_msg_type_number_t StateCount;
    thread_state_flavor_t StateFlavor;

    if (lpContext->ContextFlags & CONTEXT_ALL_FLOATING & CONTEXT_AREA_MASK)
    {
#ifdef HOST_AMD64
#ifdef XSTATE_SUPPORTED
        // We're relying on the fact that the initial portion of
        // x86_avx_state64_t is identical to x86_float_state64_t
        // and x86_avx512_state64_t to _x86_avx_state64_t.
        // Check a few fields to make sure the assumption is correct.
        static_assert_no_msg(sizeof(x86_avx_state64_t) > sizeof(x86_float_state64_t));
        static_assert_no_msg(sizeof(x86_avx512_state64_t) > sizeof(x86_avx_state64_t));
        static_assert_no_msg(offsetof(x86_avx_state64_t, __fpu_fcw) == offsetof(x86_float_state64_t, __fpu_fcw));
        static_assert_no_msg(offsetof(x86_avx_state64_t, __fpu_xmm0) == offsetof(x86_float_state64_t, __fpu_xmm0));
        static_assert_no_msg(offsetof(x86_avx512_state64_t, __fpu_fcw) == offsetof(x86_float_state64_t, __fpu_fcw));
        static_assert_no_msg(offsetof(x86_avx512_state64_t, __fpu_xmm0) == offsetof(x86_float_state64_t, __fpu_xmm0));

        x86_avx512_state64_t State;
        if (lpContext->ContextFlags & CONTEXT_XSTATE & CONTEXT_AREA_MASK)
        {
            if ((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX512) == XSTATE_MASK_AVX512)
            {
                StateFlavor = x86_AVX512_STATE64;
                StateCount = sizeof(x86_avx512_state64_t) / sizeof(natural_t);
            }
            else
            {
                _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX) == XSTATE_MASK_AVX);
                StateFlavor = x86_AVX_STATE64;
                StateCount = sizeof(x86_avx_state64_t) / sizeof(natural_t);
            }
        }
        else
        {
            StateFlavor = x86_FLOAT_STATE64;
            StateCount = sizeof(x86_float_state64_t) / sizeof(natural_t);
        }
#else
        x86_float_state64_t State;
        StateFlavor = x86_FLOAT_STATE64;
        StateCount = sizeof(State) / sizeof(natural_t);
#endif
#elif defined(HOST_ARM64)
        arm_neon_state64_t State;
        StateFlavor = ARM_NEON_STATE64;
        StateCount = sizeof(State) / sizeof(natural_t);
#else
#error Unexpected architecture.
#endif

        if (lpContext->ContextFlags & CONTEXT_FLOATING_POINT & CONTEXT_AREA_MASK)
        {
#ifdef HOST_AMD64
            *(DWORD*)&State.__fpu_fcw = lpContext->FltSave.ControlWord;
            *(DWORD*)&State.__fpu_fsw = lpContext->FltSave.StatusWord;
            State.__fpu_ftw = lpContext->FltSave.TagWord;
            State.__fpu_ip = lpContext->FltSave.ErrorOffset;
            State.__fpu_cs = lpContext->FltSave.ErrorSelector;
            State.__fpu_dp = lpContext->FltSave.DataOffset;
            State.__fpu_ds = lpContext->FltSave.DataSelector;
            State.__fpu_mxcsr = lpContext->FltSave.MxCsr;
            State.__fpu_mxcsrmask = lpContext->FltSave.MxCsr_Mask; // note: we don't save the mask for x86

            // Windows stores the floating point registers in a packed layout (each 10-byte register end to
            // end for a total of 80 bytes). But Mach returns each register in an 16-bit structure (presumably
            // for alignment purposes). So we can't just memcpy the registers over in a single block, we need
            // to copy them individually.
            for (int i = 0; i < 8; i++)
                memcpy((&State.__fpu_stmm0)[i].__mmst_reg, &lpContext->FltSave.FloatRegisters[i], 10);

            memcpy(&State.__fpu_xmm0, &lpContext->Xmm0, 16 * 16);
#elif defined(HOST_ARM64)
            memcpy(&State.__v, &lpContext->V[0], 32 * 16);
            State.__fpsr = lpContext->Fpsr;
            State.__fpcr = lpContext->Fpcr;
#else
#error Unexpected architecture.
#endif
        }

#if defined(HOST_AMD64) && defined(XSTATE_SUPPORTED)
        if (lpContext->ContextFlags & CONTEXT_XSTATE & CONTEXT_AREA_MASK)
        {
            if ((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX512) == XSTATE_MASK_AVX512)
            {
                memcpy(&State.__fpu_k0, &lpContext->KMask0, sizeof(_STRUCT_OPMASK_REG) * 8);
                memcpy(&State.__fpu_zmmh0, &lpContext->Zmm0H, sizeof(_STRUCT_YMM_REG) * 16);
                memcpy(&State.__fpu_zmm16, &lpContext->Zmm16, sizeof(_STRUCT_ZMM_REG) * 16);
            }

            _ASSERT((lpContext->XStateFeaturesMask & XSTATE_MASK_AVX) == XSTATE_MASK_AVX);
            memcpy(&State.__fpu_ymmh0, &lpContext->Ymm0H, sizeof(_STRUCT_XMM_REG) * 16);
        }
#endif

        do
        {
            MachRet = thread_set_state(Port,
                                       StateFlavor,
                                       (thread_state_t)&State,
                                       StateCount);
        }
        while (MachRet == KERN_ABORTED);

        if (MachRet != KERN_SUCCESS)
        {
            ASSERT("thread_set_state(FLOAT_STATE) failed: %d\n", MachRet);
            goto EXIT;
        }
    }

    if (lpContext->ContextFlags & (CONTEXT_CONTROL|CONTEXT_INTEGER) & CONTEXT_AREA_MASK)
    {
#ifdef HOST_AMD64
        x86_thread_state64_t State;
        StateFlavor = x86_THREAD_STATE64;

        State.__rax = lpContext->Rax;
        State.__rbx = lpContext->Rbx;
        State.__rcx = lpContext->Rcx;
        State.__rdx = lpContext->Rdx;
        State.__rdi = lpContext->Rdi;
        State.__rsi = lpContext->Rsi;
        State.__rbp = lpContext->Rbp;
        State.__rsp = lpContext->Rsp;
        State.__r8 = lpContext->R8;
        State.__r9 = lpContext->R9;
        State.__r10 = lpContext->R10;
        State.__r11 = lpContext->R11;
        State.__r12 = lpContext->R12;
        State.__r13 = lpContext->R13;
        State.__r14 = lpContext->R14;
        State.__r15 = lpContext->R15;
        State.__rflags = lpContext->EFlags;
        State.__rip = lpContext->Rip;
        State.__cs = lpContext->SegCs;
        State.__fs = lpContext->SegFs;
        State.__gs = lpContext->SegGs;
#elif defined(HOST_ARM64)
        arm_thread_state64_t State;
        StateFlavor = ARM_THREAD_STATE64;

        memcpy(&State.__x[0], &lpContext->X0, 29 * 8);
        State.__cpsr = lpContext->Cpsr;
        arm_thread_state64_set_fp(State, lpContext->Fp);
        arm_thread_state64_set_sp(State, lpContext->Sp);
        arm_thread_state64_set_lr_fptr(State, lpContext->Lr);
        arm_thread_state64_set_pc_fptr(State, lpContext->Pc);
#else
#error Unexpected architecture.
#endif

        StateCount = sizeof(State) / sizeof(natural_t);

        do
        {
            MachRet = thread_set_state(Port,
                                       StateFlavor,
                                       (thread_state_t)&State,
                                       StateCount);
        }
        while (MachRet == KERN_ABORTED);

        if (MachRet != KERN_SUCCESS)
        {
            ASSERT("thread_set_state(THREAD_STATE) failed: %d\n", MachRet);
            goto EXIT;
        }
    }

EXIT:
    return MachRet;
}

/*++
Function:
  SetThreadContext

See MSDN doc.
--*/
BOOL
CONTEXT_SetThreadContext(
           DWORD dwProcessId,
           pthread_t self,
           CONST CONTEXT *lpContext)
{
    BOOL ret = FALSE;

    if (lpContext == NULL)
    {
        ERROR("Invalid lpContext parameter value\n");
        SetLastError(ERROR_NOACCESS);
        goto EXIT;
    }

    if (dwProcessId != GetCurrentProcessId())
    {
        // GetThreadContext() of a thread in another process
        ASSERT("Cross-process GetThreadContext() is not supported\n");
        SetLastError(ERROR_NOACCESS);
        goto EXIT;
    }

    if (self != pthread_self())
    {
        // hThread is in the current process, but isn't the current
        // thread.  Extract the CONTEXT from the Mach thread.

        mach_port_t mptPort;

        mptPort = pthread_mach_thread_np(self);

        ret = (CONTEXT_SetThreadContextOnPort(mptPort, lpContext) == KERN_SUCCESS);
    }
    else
    {
        MachSetThreadContext(const_cast<CONTEXT *>(lpContext));
        ASSERT("MachSetThreadContext should never return\n");
    }

EXIT:
    return ret;
}

#endif // !HAVE_MACH_EXCEPTIONS

/*++
Function:
  DBG_FlushInstructionCache: processor-specific portion of
  FlushInstructionCache

See MSDN doc.
--*/
BOOL
DBG_FlushInstructionCache(
                          IN LPCVOID lpBaseAddress,
                          IN SIZE_T dwSize)
{
#if defined(__linux__) && defined(HOST_ARM)
    // On Linux/arm (at least on 3.10) we found that there is a problem with __do_cache_op (arch/arm/kernel/traps.c)
    // implementing cacheflush syscall. cacheflush flushes only the first page in range [lpBaseAddress, lpBaseAddress + dwSize)
    // and leaves other pages in undefined state which causes random tests failures (often due to SIGSEGV) with no particular pattern.
    //
    // As a workaround, we call __builtin___clear_cache on each page separately.

    const SIZE_T pageSize = GetVirtualPageSize();
    UINT_PTR begin = (UINT_PTR)lpBaseAddress;
    const UINT_PTR end = begin + dwSize;

    while (begin < end)
    {
        UINT_PTR endOrNextPageBegin = ALIGN_UP(begin + 1, pageSize);
        if (endOrNextPageBegin > end)
            endOrNextPageBegin = end;

        __builtin___clear_cache((char *)begin, (char *)endOrNextPageBegin);
        begin = endOrNextPageBegin;
    }
#elif defined(HOST_RISCV64)
    // __clear_cache() expanded from __builtin___clear_cache() is not implemented
    // on Linux/RISCV64, at least in Clang 14, and we have to make syscall directly.
    //
    // TODO-RISCV64: use __builtin___clear_cache() in future. See https://github.com/llvm/llvm-project/issues/63551

#ifndef __NR_riscv_flush_icache
    #define __NR_riscv_flush_icache 259
#endif

    syscall(__NR_riscv_flush_icache, (char *)lpBaseAddress, (char *)((INT_PTR)lpBaseAddress + dwSize), 0 /* all harts */);
#elif defined(HOST_WASM)
    // do nothing, no instruction cache to flush
#elif defined(HOST_APPLE) && !defined(HOST_OSX)
    sys_icache_invalidate((void *)lpBaseAddress, dwSize);
#else
    __builtin___clear_cache((char *)lpBaseAddress, (char *)((INT_PTR)lpBaseAddress + dwSize));
#endif
    return TRUE;
}

#ifdef HOST_AMD64
CONTEXT& CONTEXT::operator=(const CONTEXT& ctx)
{
    size_t copySize;
    if (ctx.ContextFlags & CONTEXT_XSTATE & CONTEXT_AREA_MASK)
    {
        if ((ctx.XStateFeaturesMask & XSTATE_MASK_AVX512) == XSTATE_MASK_AVX512)
        {
            copySize = offsetof(CONTEXT, R16);
        }
        else
        {
            copySize = offsetof(CONTEXT, KMask0);
        }

        if ((ctx.XStateFeaturesMask & XSTATE_MASK_APX) == XSTATE_MASK_APX)
        {
            // Copy APX EGPRs separately.
            memcpy(&(this->R16), &(ctx.R16), sizeof(DWORD64) * 16);
        }
    }
    else
    {
        copySize = offsetof(CONTEXT, XStateFeaturesMask);
    }

    memcpy(this, &ctx, copySize);

    return *this;
}
#endif // HOST_AMD64
