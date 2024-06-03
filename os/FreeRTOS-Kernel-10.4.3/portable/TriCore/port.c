/*
 * FreeRTOS Kernel V10.3.1
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */

/**
 * \file port.c
 * \brief File defining the FreeRTOS portable functions towards TriCore CPUs
 *
 * \copyright Copyright (c) 2020 Infineon Technologies AG. All rights reserved.
 *
 *                               IMPORTANT NOTICE
 *
 * Use of this file is subject to the terms of use agreed between (i) you or
 * the company in which ordinary course of business you are acting and (ii)
 * Infineon Technologies AG or its licensees. If and as long as no such
 * terms of use are agreed, use of this file is subject to following:
 *
 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or
 * organization obtaining a copy of the software and accompanying
 * documentation covered by this license (the "Software") to use, reproduce,
 * display, distribute, execute, and transmit the Software, and to prepare
 * derivative works of the Software, and to permit third-parties to whom the
 * Software is furnished to do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer, must
 * be included in all copies of the Software, in whole or in part, and all
 * derivative works of the Software, unless such copies or derivative works are
 * solely in the form of machine-executable object code generated by a source
 * language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Standard includes. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "list.h"

#include "Ifx_Types.h"
#include "IfxStm.h"
#if configCHECK_FOR_STACK_OVERFLOW > 0
    #error "Stack checking cannot be used with this port, as, unlike most ports, the pxTopOfStack member of the TCB is consumed CSA.  CSA starvation, loosely equivalent to stack overflow, will result in a trap exception."
    /* The stack pointer is accessible using portCSA_TO_ADDRESS( portCSA_TO_ADDRESS( pxCurrentTCB->pxTopOfStack )[ 0 ] )[ 2 ]; */
#endif /* configCHECK_FOR_STACK_OVERFLOW */

/*-----------------------------------------------------------*/

/* System register Definitions. */
#define portSYSTEM_PROGRAM_STATUS_WORD                    ( 0x000008FFUL ) /* Supervisor Mode, MPU Register Set 0 and Call Depth Counting disabled. */
#define portINITIAL_PRIVILEGED_PROGRAM_STATUS_WORD        ( 0x000014FFUL ) /* IO Level 1, MPU Register Set 1 and Call Depth Counting disabled. */
#define portINITIAL_UNPRIVILEGED_PROGRAM_STATUS_WORD      ( 0x000010FFUL ) /* IO Level 0, MPU Register Set 1 and Call Depth Counting disabled. */
#define portINITIAL_PCXI_UPPER_CONTEXT_WORD               ( 0x00300000UL ) /* The lower 20 bits identify the CSA address. */
#define portINITIAL_SYSCON                                ( 0x00000000UL ) /* MPU Disable. */

/* CSA manipulation macros. */
#define portCSA_FCX_MASK                                  ( 0x000FFFFFUL )

/* OS Interrupt and Trap mechanisms. */
#define portRESTORE_PSW_MASK                              ( ~( 0x000000FFUL ) )

/* Each CSA contains 16 words of data. */
#define portNUM_WORDS_IN_CSA                              ( 16 )

/*-----------------------------------------------------------*/

/* This reference is required by the save/restore context macros. */
extern volatile  TaskHandle_t *pxCurrentTCBs[ configNUM_CORES ];
#define pxCurrentTCB    ((unsigned long *)pxCurrentTCBs[__mfcr(TRICORE_CPU_CORE_ID)])

/*-----------------------------------------------------------*/

StackType_t *pxPortInitialiseStack( portSTACK_TYPE * pxTopOfStack, pdTASK_CODE pxCode, void *pvParameters )
{
    unsigned long *pulUpperCSA = NULL;
    unsigned long *pulLowerCSA = NULL;

    /* 16 Address Registers (4 Address registers are global), 16 Data
    Registers, and 3 System Registers.

    There are 3 registers that track the CSAs.
        FCX points to the head of globally free set of CSAs.
        PCX for the task needs to point to Lower->Upper->NULL arrangement.
        LCX points to the last free CSA so that corrective action can be taken.

    Need two CSAs to store the context of a task.
        The upper context contains D8-D15, A10-A15, PSW and PCXI->NULL.
        The lower context contains D0-D7, A2-A7, A11 and PCXI->UpperContext.
        The pxCurrentTCB->pxTopOfStack points to the Lower Context RSLCX matching the initial BISR.
        The Lower Context points to the Upper Context ready for the return from the interrupt handler.

    The Real stack pointer for the task is stored in the A10 which is restored
    with the upper context. */

    /* Have to disable interrupts here because the CSAs are going to be
       manipulated. */
    portENTER_CRITICAL();
    {
        /* DSync to ensure that buffering is not a problem. */
        TriCore__dsync();

        /* Consume two free CSAs. */
        pulLowerCSA = portCSA_TO_ADDRESS( TriCore__mfcr( TRICORE_CPU_FCX ) );
        if( NULL != pulLowerCSA )
        {
            /* The Lower Links to the Upper. */
            pulUpperCSA = portCSA_TO_ADDRESS( pulLowerCSA[ 0 ] );
        }

        /* Check that we have successfully reserved two CSAs. */
        if( ( NULL != pulLowerCSA ) && ( NULL != pulUpperCSA ) )
        {
            /* Remove the two consumed CSAs from the free CSA list. */
            TriCore__disable();
            TriCore__dsync();
            TriCore__mtcr( TRICORE_CPU_FCX, pulUpperCSA[ 0 ] );
            TriCore__isync();
            TriCore__enable();
        }
        else
        {
            /* Simply trigger a context list depletion trap. */
            TriCore__svlcx();
        }
    }
    portEXIT_CRITICAL();

    /* Clear the upper CSA. */
    memset( pulUpperCSA, 0, portNUM_WORDS_IN_CSA * sizeof( unsigned long ) );

    /* Upper Context. */
    pulUpperCSA[ 2 ] = ( unsigned long  )pxTopOfStack;        /* A10;    Stack Return aka Stack Pointer */
    pulUpperCSA[ 1 ] = portSYSTEM_PROGRAM_STATUS_WORD;        /* PSW    */

    /* Clear the lower CSA. */
    memset( pulLowerCSA, 0, portNUM_WORDS_IN_CSA * sizeof( unsigned long ) );

    /* Lower Context. */
    pulLowerCSA[ 8 ] = ( unsigned long ) pvParameters;        /* A4;    Address Type Parameter Register    */
    pulLowerCSA[ 1 ] = ( unsigned long ) pxCode;            /* A11;    Return Address aka RA */

    /* PCXI pointing to the Upper context. */
    pulLowerCSA[ 0 ] = ( portINITIAL_PCXI_UPPER_CONTEXT_WORD | ( unsigned long ) portADDRESS_TO_CSA( pulUpperCSA ) );

    /* Save the link to the CSA in the top of stack. */
    pxTopOfStack = (unsigned long * ) portADDRESS_TO_CSA( pulLowerCSA );

    /* DSync to ensure that buffering is not a problem. */
    TriCore__dsync();

    return pxTopOfStack;
}
/*-----------------------------------------------------------*/

#define ISR_PRIORITY_STM        40                              /* Priority for interrupt ISR                       */
#define TIMER_INT_TIME          1                             /* Time between interrupts in ms                    */
//#define STM                     &MODULE_STM0                    /* STM0 is used in this example                     */
static volatile Ifx_STM *const STM[7] = {&MODULE_STM0, &MODULE_STM1, &MODULE_STM2,&MODULE_STM3,&MODULE_STM4,NULL,&MODULE_STM5};
IfxStm_CompareConfig g_STMConf[7];                                 /* STM configuration structure                      */
IfxSrc_Tos stm_tos[7] = {IfxSrc_Tos_cpu0,IfxSrc_Tos_cpu1,IfxSrc_Tos_cpu2,IfxSrc_Tos_cpu3,IfxSrc_Tos_cpu4,IfxSrc_Tos_dma,IfxSrc_Tos_cpu5};
Ifx_TickTime g_ticksFor1ms;

IFX_INTERRUPT(isrSTM, 0, ISR_PRIORITY_STM);
IFX_INTERRUPT(isrSTM1, 1, ISR_PRIORITY_STM);
IFX_INTERRUPT(isrSTM2, 2, ISR_PRIORITY_STM);
IFX_INTERRUPT(isrSTM3, 3, ISR_PRIORITY_STM);
IFX_INTERRUPT(isrSTM4, 4, ISR_PRIORITY_STM);
IFX_INTERRUPT(isrSTM5, 5, ISR_PRIORITY_STM);
void isrSTM(void)
{
    //__disable();
    //portDISABLE_INTERRUPTS();
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
    //portENABLE_INTERRUPTS();
    //__enable();
}
void isrSTM1(void)
{
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
}
void isrSTM2(void)
{
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
}
void isrSTM3(void)
{
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
}
void isrSTM4(void)
{
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
}
void isrSTM5(void)
{
    IfxStm_increaseCompare(STM[portGET_CORE_ID()], g_STMConf[portGET_CORE_ID()].comparator, g_ticksFor1ms);
    vPortSystemTickHandler();
}

/* Function to initialize the STM */
void initSTM(void)
{
    /* Initialize time constant */
    /* Variable to store the number of ticks to wait    */
    g_ticksFor1ms = IfxStm_getTicksFromMilliseconds(STM[portGET_CORE_ID()], TIMER_INT_TIME);
    IfxStm_enableOcdsSuspend (STM[portGET_CORE_ID()]);
    IfxStm_initCompareConfig(&g_STMConf[portGET_CORE_ID()]);           /* Initialize the configuration structure with default values   */

    g_STMConf[portGET_CORE_ID()].triggerPriority = ISR_PRIORITY_STM;   /* Set the priority of the interrupt                            */
    g_STMConf[portGET_CORE_ID()].typeOfService = stm_tos[portGET_CORE_ID()];      /* Set the service provider for the interrupts                  */
    g_STMConf[portGET_CORE_ID()].ticks = g_ticksFor1ms;              /* Set the number of ticks after which the timer triggers an
                                                     * interrupt for the first time                                 */
    IfxStm_initCompare(STM[portGET_CORE_ID()], &g_STMConf[portGET_CORE_ID()]);            /* Initialize the STM with the user configuration               */
}
BaseType_t xPortStartScheduler( void )
{
    unsigned long ulMFCR = 0UL;
    unsigned long *pulCSA = NULL;

    initSTM();
    /* Interrupts at or below configMAX_SYSCALL_INTERRUPT_PRIORITY are disable
    when this function is called. */
    TriCore__disable();
    {
        /* Load the initial SYSCON. */
        TriCore__mtcr( TRICORE_CPU_SYSCON, portINITIAL_SYSCON );
        TriCore__isync();

        /* ENDINIT has already been applied in the 'cstart.c' code. */

        /* Clear the PSW.CDC to enable the use of an RFE without it generating an
        exception because this code is not genuinely in an exception. */
        ulMFCR = TriCore__mfcr( TRICORE_CPU_PSW );
        ulMFCR &= portRESTORE_PSW_MASK;
        TriCore__dsync();
        TriCore__mtcr( TRICORE_CPU_PSW, ulMFCR );
        TriCore__isync();

        /* Finally, perform the equivalent of a portRESTORE_CONTEXT() */
        pulCSA = portCSA_TO_ADDRESS( ( *pxCurrentTCB ) );
        (void) portCSA_TO_ADDRESS( pulCSA[0] );
        TriCore__dsync();
        TriCore__mtcr(TRICORE_CPU_PCXI, *pxCurrentTCB );
        TriCore__isync();
        TriCore__nop();
        TriCore__rslcx();
        TriCore__nop();
    }
    TriCore__enable();

    /* Return from function, which would return to the first task selected to execute. */
    return 0;
}
/*-----------------------------------------------------------*/

TRICORE_CINLINE void prvYield(void)
{
    unsigned long *pxUpperCSA = NULL;
    unsigned long xUpperCSA = 0UL;
    /* Save the context of a task.
       The upper context is automatically saved when entering a trap or interrupt.
       Need to save the lower context as well and copy the PCXI CSA ID into
       pxCurrentTCB->pxTopOfStack. Only Lower Context CSA IDs may be saved to the
       TCB of a task.

       Call vTaskSwitchContext to select the next task, note that this changes the
       value of pxCurrentTCB so that it needs to be reloaded.

       Call vPortSetMPURegisterSetOne to change the MPU mapping for the task
       that has just been switched in.

       Load the context of the task.
       Need to restore the lower context by loading the CSA from
       pxCurrentTCB->pxTopOfStack into PCXI (effectively changing the call stack).
       In the Interrupt handler post-amble, RSLCX will restore the lower context
       of the task. RFE will restore the upper context of the task, jump to the
       return address and restore the previous state of interrupts being
       enabled/disabled.
    */

    TriCore__disable();
    {
        TriCore__dsync();
        xUpperCSA = TriCore__mfcr( TRICORE_CPU_PCXI );
        pxUpperCSA = portCSA_TO_ADDRESS( xUpperCSA );
        *pxCurrentTCB = pxUpperCSA[ 0 ];
        vTaskSwitchContext();
        pxUpperCSA[ 0 ] = *pxCurrentTCB;
        TriCore__isync();
    }
    TriCore__enable();
}

TRICORE_NOINLINE void vPortSystemTickHandler( void )
{
    unsigned long ulSavedInterruptMask;
    long lYieldRequired;

    /* Reload the Compare Match register for X ticks into the future.

       If critical section or interrupt nesting budgets are exceeded, then
       it is possible that the calculated next compare match value is in the
       past.  If this occurs (unlikely), it is possible that the resulting
       time slippage will exceed a single tick period.  Any adverse effect of
       this is time bounded by the fact that only the first n bits of the 56 bit
       STM timer are being used for a compare match, so another compare match
       will occur after an overflow in just those n bits (not the entire 56 bits).
       As an example, if the peripheral clock is 75MHz, and the tick rate is 1KHz,
       a missed tick could result in the next tick interrupt occurring within a
       time that is 1.7 times the desired period.  The fact that this is greater
       than a single tick period is an effect of using a timer that cannot be
       automatically reset, in hardware, by the occurrence of a tick interrupt.
       Changing the tick source to a timer that has an automatic reset on compare
       match (such as a GPTA timer) will reduce the maximum possible additional
       period to exactly 1 times the desired period.
    */

    /* Kernel API calls require Critical Sections. */
    ulSavedInterruptMask = portSET_INTERRUPT_MASK_FROM_ISR();
    {
        /* Increment the Tick. */
        lYieldRequired = xTaskIncrementTick();
    }
    
    portCLEAR_INTERRUPT_MASK_FROM_ISR( ulSavedInterruptMask );

    if( lYieldRequired != pdFALSE )
    {
        prvYield();
    }
}

/*-----------------------------------------------------------*/

/*
 * When a task is deleted, it is yielded permanently until the IDLE task
 * has an opportunity to reclaim the memory that that task was using.
 * Typically, the memory used by a task is the TCB and Stack but in the
 * TriCore this includes the CSAs that were consumed as part of the Call
 * Stack. These CSAs can only be returned to the Globally Free Pool when
 * they are not part of the current Call Stack, hence, delaying the
 * reclamation until the IDLE task is freeing the task's other resources.
 * This function uses the head of the linked list of CSAs (from when the
 * task yielded for the last time) and finds the tail (the very bottom of
 * the call stack) and inserts this list at the head of the Free list,
 * attaching the existing Free List to the tail of the reclaimed call stack.
 *
 * NOTE: the IDLE task needs processing time to complete this function
 * and in heavily loaded systems, the Free CSAs may be consumed faster
 * than they can be freed assuming that tasks are being spawned and
 * deleted frequently.
 */
void vPortReclaimCSA( unsigned long *pxTCB )
{
    unsigned long pxHeadCSA, pxTailCSA, pxFreeCSA;
    unsigned long *pulNextCSA;

    /* A pointer to the first CSA in the list of CSAs consumed by the task is
    stored in the first element of the tasks TCB structure (where the stack
    pointer would be on a traditional stack based architecture). */
    pxHeadCSA = ( *pxTCB ) & portCSA_FCX_MASK;

    /* Mask off everything in the CSA link field other than the address.  If
    the address is NULL, then the CSA is not linking anywhere and there is
    nothing to do. */
    pxTailCSA = pxHeadCSA;

    /* Convert the link value to contain just a raw address and store this
    in a local variable. */
    pulNextCSA = portCSA_TO_ADDRESS( pxTailCSA );

    /* Iterate over the CSAs that were consumed as part of the task.  The
    first field in the CSA is the pointer to then next CSA.  Mask off
    everything in the pointer to the next CSA, other than the link address.
    If this is NULL, then the CSA currently being pointed to is the last in
    the chain. */

    while( 0UL != ( pulNextCSA[ 0 ] & portCSA_FCX_MASK ) )
    {
        /* Clear all bits of the pointer to the next in the chain, other
        than the address bits themselves. */
        pulNextCSA[ 0 ] = pulNextCSA[ 0 ] & portCSA_FCX_MASK;

        /* Move the pointer to point to the next CSA in the list. */
        pxTailCSA = pulNextCSA[ 0 ];

        /* Update the local pointer to the CSA. */
        pulNextCSA = portCSA_TO_ADDRESS( pxTailCSA );
    }

    TriCore__disable();
    {
        /* Look up the current free CSA head. */
        TriCore__dsync();
        pxFreeCSA = TriCore__mfcr( TRICORE_CPU_FCX );

        /* Join the current Free onto the Tail of what is being reclaimed. */
        portCSA_TO_ADDRESS( pxTailCSA )[ 0 ] = pxFreeCSA;

        /* Move the head of the reclaimed into the Free. */
        TriCore__dsync();
        TriCore__mtcr( TRICORE_CPU_FCX, pxHeadCSA );
        TriCore__isync();
    }
    TriCore__enable();
}
/*-----------------------------------------------------------*/

void vPortEndScheduler( void )
{
  /* Nothing to do. Unlikely to want to end. */
}
/*-----------------------------------------------------------*/

TRICORE_NOINLINE void vTrapYield( int iTrapIdentification )
{
    switch( iTrapIdentification )
    {
        case portSYSCALL_TASK_YIELD:
            prvYield();
            break;

        default:
            /* Unimplemented trap called. */
            configASSERT( ( ( volatile void * ) NULL ) );
            break;
  }
}
/*-----------------------------------------------------------*/

TRICORE_NOINLINE void vPortSystemTaskHandler( void )
{
    prvYield();
}

TRICORE_NOINLINE void vPortYield( void )
{
	TriCore__svlcx( );
	vPortSystemTaskHandler();
	TriCore__rslcx( );
}

/*-----------------------------------------------------------*/

unsigned long uxPortSetInterruptMaskFromISR( void )
{
    unsigned long uxReturn = 0UL;

    TriCore__disable();
    uxReturn = TriCore__mfcr( TRICORE_CPU_ICR );
    TriCore__mtcr( TRICORE_CPU_ICR, ( ( uxReturn & ~portCCPN_MASK ) | configMAX_SYSCALL_INTERRUPT_PRIORITY ) );
    TriCore__isync();
    TriCore__enable();

    /* Return just the interrupt mask bits. */
    return ( uxReturn & portCCPN_MASK );
}
/*-----------------------------------------------------------*/

__attribute__((__noreturn__)) void vPortLoopForever(void)
{
	while(1);
}

__attribute__((__noreturn__)) void vApplicationMallocFailedHook( void )
{
    vPortLoopForever();
}

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )
/* configUSE_STATIC_ALLOCATION is set to 1, so the application must provide an
 * implementation of vApplicationGetIdleTaskMemory() to provide the memory that is
 * used by the Idle task. */
void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    uint32_t * pulIdleTaskStackSize )
{
/* If the buffers to be provided to the Idle task are declared inside this
 * function then they must be declared static - otherwise they will be allocated on
 * the stack and so not exists after this function exits. */
    static StaticTask_t xIdleTaskTCBs[ configNUM_CORES ];
    static StackType_t uxIdleTaskStacks[ configNUM_CORES ][ configMINIMAL_STACK_SIZE ];

    #define xIdleTaskTCB    xIdleTaskTCBs[__mfcr(TRICORE_CPU_CORE_ID)]
    #define uxIdleTaskStack uxIdleTaskStacks[__mfcr(TRICORE_CPU_CORE_ID)]

    /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
     * state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
#endif // configSUPPORT_STATIC_ALLOCATION == 1
/*-----------------------------------------------------------*/
StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];
#if ( configSUPPORT_STATIC_ALLOCATION == 1 )
/* configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
 * application must provide an implementation of vApplicationGetTimerTaskMemory()
 * to provide the memory that is used by the Timer service task. */
void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
/* If the buffers to be provided to the Timer task are declared inside this
 * function then they must be declared static - otherwise they will be allocated on
 * the stack and so not exists after this function exits. */
    static StaticTask_t xTimerTaskTCBs[ configNUM_CORES ];
    #define xTimerTaskTCB    xTimerTaskTCBs[__mfcr(TRICORE_CPU_CORE_ID)]

    /* Pass out a pointer to the StaticTask_t structure in which the Timer
     * task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
#endif // configSUPPORT_STATIC_ALLOCATION == 1