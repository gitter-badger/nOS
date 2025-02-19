/*
 * Copyright (c) 2014-2015 Jim Tremblay
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#define NOS_PRIVATE
#include "nOS.h"

#ifdef __cplusplus
extern "C" {
#endif

void PendSV_Handler(void) __attribute__( ( naked ) );

#ifdef NOS_CONFIG_ISR_STACK_SIZE
 static nOS_Stack _isrStack[NOS_CONFIG_ISR_STACK_SIZE] __attribute__ ( ( section(".noinit") ) );
#endif

void nOS_InitSpecific(void)
{
#ifdef NOS_CONFIG_ISR_STACK_SIZE
 #if (NOS_CONFIG_DEBUG > 0)
    size_t i;

    for (i = 0; i < NOS_CONFIG_ISR_STACK_SIZE; i++) {
        _isrStack[i] = 0xFFFFFFFFUL;
    }
 #endif

    /* Copy MSP to PSP */
    _SetPSP(_GetMSP());
    /* Set MSP to local ISR stack */
    _SetMSP((uint32_t)&_isrStack[NOS_CONFIG_ISR_STACK_SIZE] & 0xFFFFFFF8UL);
    /* Set current stack to PSP and privileged mode */
    _SetCONTROL(_GetCONTROL() | 0x00000002UL);
#endif
    /* Set PendSV exception to lowest priority */
    *(volatile uint32_t *)0xE000ED20UL |= 0x00FF0000UL;
}

void nOS_InitContext(nOS_Thread *thread, nOS_Stack *stack, size_t ssize, nOS_ThreadEntry entry, void *arg)
{
    nOS_Stack *tos = (nOS_Stack*)((uint32_t)(stack + ssize) & 0xFFFFFFF8UL);
#if (NOS_CONFIG_DEBUG > 0)
    size_t i;

    for (i = 0; i < ssize; i++) {
        stack[i] = 0xFFFFFFFFUL;
    }
#endif

    *(--tos) = 0x01000000UL;    /* xPSR */
    *(--tos) = (nOS_Stack)entry;/* PC */
    *(--tos) = 0x00000000UL;    /* LR */
#if (NOS_CONFIG_DEBUG > 0)
    *(--tos) = 0x12121212UL;    /* R12 */
    *(--tos) = 0x03030303UL;    /* R3 */
    *(--tos) = 0x02020202UL;    /* R2 */
    *(--tos) = 0x01010101UL;    /* R1 */
#else
        tos -= 4;               /* R12, R3, R2 and R1 */
#endif
    *(--tos) = (nOS_Stack)arg;  /* R0 */
    *(--tos) = 0xFFFFFFFDUL;    /* EXC_RETURN (Thread mode, Thread use PSP) */
#if (NOS_CONFIG_DEBUG > 0)
    *(--tos) = 0x11111111UL;    /* R11 */
    *(--tos) = 0x10101010UL;    /* R10 */
    *(--tos) = 0x09090909UL;    /* R9 */
    *(--tos) = 0x08080808UL;    /* R8 */
    *(--tos) = 0x07070707UL;    /* R7 */
    *(--tos) = 0x06060606UL;    /* R6 */
    *(--tos) = 0x05050505UL;    /* R5 */
    *(--tos) = 0x04040404UL;    /* R4 */
#else
        tos -= 8;               /* R11, R10, R9, R8, R7, R6, R5 and R4 */
#endif

    thread->stackPtr = tos;
}

void nOS_SwitchContext(void)
{
    nOS_StatusReg   sr = _GetBASEPRI();

    /* Request context switch */
    *(volatile uint32_t *)0xE000ED04UL = 0x10000000UL;

    /* Leave critical section */
    _SetBASEPRI(0);
    _DSB();
    _ISB();

    _NOP();

    /* Enter critical section */
    _SetBASEPRI(sr);
    _DSB();
    _ISB();
}

void nOS_EnterIsr (void)
{
    nOS_StatusReg   sr;

#if (NOS_CONFIG_SAFE > 0)
    if (nOS_running)
#endif
    {
        nOS_EnterCritical(sr);
        nOS_isrNestingCounter++;
        nOS_LeaveCritical(sr);
    }
}

void nOS_LeaveIsr (void)
{
    nOS_StatusReg   sr;

#if (NOS_CONFIG_SAFE > 0)
    if (nOS_running)
#endif
    {
        nOS_EnterCritical(sr);
        nOS_isrNestingCounter--;
#if (NOS_CONFIG_SCHED_PREEMPTIVE_ENABLE > 0)
        if (nOS_isrNestingCounter == 0) {
 #if (NOS_CONFIG_SCHED_LOCK_ENABLE > 0)
            if (nOS_lockNestingCounter == 0)
 #endif
            {
                nOS_highPrioThread = nOS_FindHighPrioThread();
                if (nOS_runningThread != nOS_highPrioThread) {
                    *(volatile uint32_t *)0xE000ED04UL = 0x10000000UL;
                }
            }
        }
#endif
        nOS_LeaveCritical(sr);
    }
}

void PendSV_Handler(void)
{
    __asm volatile (
        /* Save PSP before doing anything, PendSV_Handler already running on MSP */
        "MRS        R0,         PSP                 \n"
        "ISB                                        \n"

        /* Get the location of nOS_runningThread */
        "LDR        R3,         runningThread       \n"
        "LDR        R2,         [R3]                \n"

#if defined(__VFP_FP__) && !defined(__SOFTFP__)
        /* Is thread using FPU? Yes, push high VFP registers to stack */
        "TST        LR,         #0x10               \n"
        "IT         EQ                              \n"
        "VSTMDBEQ   R0!,        {S16-S31}           \n"
#endif

        /* Push remaining registers on thread stack */
        "STMDB      R0!,        {R4-R11, LR}        \n"

        /* Save PSP to nOS_Thread object of current running thread */
        "STR        R0,         [R2]                \n"

        /* Get the location of nOS_highPrioThread */
        "LDR        R1,         highPrioThread      \n"
        "LDR        R2,         [R1]                \n"

        /* Copy nOS_highPrioThread to nOS_runningThread */
        "STR        R2,         [R3]                \n"

        /* Restore PSP from nOS_Thread object of high prio thread */
        "LDR        R0,         [R2]                \n"

        /* Pop registers from thread stack */
        "LDMIA      R0!,        {R4-R11, LR}        \n"

#if defined(__VFP_FP__) && !defined(__SOFTFP__)
        /* Is thread using FPU? Yes, pop high VFP registers from stack */
        "TST        LR,         #0x10               \n"
        "IT         EQ                              \n"
        "VLDMIAEQ   R0!,        {S16-S31}           \n"
#endif

        /* Restore PSP to high prio thread stack */
        "MSR        PSP,        R0                  \n"
        "ISB                                        \n"

        /* Return */
        "BX         LR                              \n"

        ".align 2                                   \n"
        "runningThread: .word nOS_runningThread     \n"
        "highPrioThread: .word nOS_highPrioThread   \n"
    );
}

#ifdef __cplusplus
}
#endif
