/*
 * Copyright (c) 2025-2026 TSN Lab, Inc.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as published by the Free
 * Software Foundation. A proprietary license is also available on request - see README.md.
 */

// Minimal FreeRTOSConfig.h for the QEMU `virt` RV32 target this platform runs on - not a
// general-purpose template. Trimmed from FreeRTOS-Kernel's own
// examples/template_configuration/FreeRTOSConfig.h down to what this single-core, no-MPU,
// no-TrustZone, no-SMP target actually needs.

#pragma once

// QEMU's `-machine virt` CLINT-driven timer isn't tied to a real crystal; this only sets the
// tick/timer-compare math, not real wall-clock behavior. tt_get_ns() (src/hal_freertos.c) reads
// the same CLINT mtime counter directly instead of relying on the RTOS tick for its nanosecond
// clock - hal_freertos.c's own CLINT_TIMEBASE_HZ must be kept equal to this value.
#define configCPU_CLOCK_HZ 10000000UL
#define configTICK_RATE_HZ 1000

#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE 0

#define configMAX_PRIORITIES 5
#define configMINIMAL_STACK_SIZE 256
#define configISR_STACK_SIZE_WORDS 512
#define configMAX_TASK_NAME_LEN 16
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1

#define configUSE_MUTEXES 1
// lwIP's contrib/ports/freertos/sys_arch.c implements sys_mutex_t with recursive semaphores.
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_QUEUE_SETS 0
#define configQUEUE_REGISTRY_SIZE 0

#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_TASK_STACK_DEPTH configMINIMAL_STACK_SIZE
#define configTIMER_QUEUE_LENGTH 10

#define configUSE_EVENT_GROUPS 1
#define configUSE_STREAM_BUFFERS 0
#define configUSE_CO_ROUTINES 0

#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
// Let the kernel provide vApplicationGetIdleTaskMemory()/vApplicationGetTimerTaskMemory() itself
// (valid since this port doesn't use the MPU wrappers) instead of us implementing them.
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
// lwIP's own heap (see lwipopts.h's MEM_SIZE) is separate from this; this is only for FreeRTOS
// kernel objects (tasks/queues/etc.) and TickLE's own dynamic allocation (_tt_malloc/_tt_free).
#define configTOTAL_HEAP_SIZE (128 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP 0

#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0

#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_TRACE_FACILITY 0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configRECORD_STACK_HIGH_ADDRESS 1

#define configASSERT(x)           \
    if ((x) == 0) {               \
        taskDISABLE_INTERRUPTS(); \
        for (;;)                  \
            ;                     \
    }

// QEMU's `virt` machine places its CLINT (timer) at the standard SiFive-derived base 0x02000000:
// mtime at +0xBFF8, mtimecmp[hart] at +0x4000 + hart*8. port.c derives both timer registers and
// vPortSetupTimerInterrupt() entirely from these two - no other timer setup code is needed.
#define configMTIME_BASE_ADDRESS 0x0200BFF8UL
#define configMTIMECMP_BASE_ADDRESS 0x02004000UL

#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_eTaskGetState 0
#define INCLUDE_xTaskGetHandle 0
#define INCLUDE_xTaskResumeFromISR 1
