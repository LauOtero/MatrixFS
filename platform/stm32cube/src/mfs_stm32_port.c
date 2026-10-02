/* mfs_stm32_port.c — contrato de puerto §20.2 para STM32Cube.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Aporta las CINCO primitivas obligatorias del puerto de MatrixFS
 * (include/matrixfs/mfs_port.h):
 *
 *   mfs_port_crit_enter / mfs_port_crit_exit   seccion critica anidable, <= 1
 * us mfs_port_cycles                            contador de ciclos
 * libre-corriente mfs_port_time_us                           tiempo monotono en
 * microsegundos mfs_port_wfi                               idle de bajo consumo
 *
 * IMPORTANTE: este fichero define simbolos GLOBALES. En un build de STM32 NO
 * deben compilarse tambien src/core/mfs_port_arch.c ni src/core/mfs_port_rtos.c
 * con sus envoltorios activos (MFS_PORT_ARCH_GLUE / MFS_PORT_RTOS_GLUE): serian
 * definiciones duplicadas. El .mk y el CMakeLists del port ya los excluyen.
 *
 * Deteccion del contexto de ejecucion, en este orden:
 *   1) FreeRTOS (CubeMX con FreeRTOS, CMSIS_V1 o CMSIS_V2)
 *   2) CMSIS-RTOS v2 sin FreeRTOS
 *   3) Bare-metal (super-bucle)
 *
 * CONTEXTO DE LLAMADA: el nucleo invoca mfs_port_crit_enter/exit alrededor de
 * las LECTURAS (src/core/mfs_zone.c) y mfs_port_wfi durante esperas largas. Por
 * eso:
 *   · la seccion critica debe ser CORTA (no engloba programaciones ni
 * borrados); · mfs_port_wfi debe CEDER o DORMIR, nunca girar en vacio. Ninguna
 * de estas primitivas es ISR-safe: no llamar a la API del nucleo desde una ISR
 * a traves de este puerto.
 */
#include "mfs_stm32_hal.h"

#include "matrixfs/mfs_port.h"

/* =====================================================================
 * 1. Deteccion del contexto de ejecucion
 * ===================================================================== */
#if defined(configUSE_PREEMPTION) || defined(MFS_STM32_USE_FREERTOS) ||        \
    defined(USE_FREERTOS) || defined(CMSIS_V1)
#define MFS_STM32_HAVE_FREERTOS 1
#else
#define MFS_STM32_HAVE_FREERTOS 0
#endif

/* CMSIS-RTOS v2 se usa solo si CubeMX lo selecciono explicitamente. */
#if !MFS_STM32_HAVE_FREERTOS &&                                                \
    (defined(CMSIS_V2) || defined(MFS_STM32_USE_CMSIS2))
#define MFS_STM32_HAVE_CMSIS2 1
#else
#define MFS_STM32_HAVE_CMSIS2 0
#endif

#if MFS_STM32_HAVE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#elif MFS_STM32_HAVE_CMSIS2
#include "cmsis_os2.h"
#endif

/* =====================================================================
 * 2. Contador de ciclos (DWT) con fallback
 *
 * DWT->CYCCNT existe en Cortex-M3/M4/M7/M33 y NO en Cortex-M0/M0+ (STM32F0,
 * L0, G0, C0). Se detecta con __CORTEX_M, que define el propio CMSIS.
 * ===================================================================== */
#ifndef __CORTEX_M
#define __CORTEX_M 3 /* conservador si el CMSIS no lo define */
#endif

#if (__CORTEX_M >= 3)
#define MFS_STM32_HAVE_DWT 1
#else
#define MFS_STM32_HAVE_DWT 0
#endif

#if MFS_STM32_HAVE_DWT
/* Habilita el contador de ciclos una sola vez (TRCENA + CYCCNTENA). */
static void mfs_stm32_dwt_init(void) {
  static volatile bool inited = false;
  if (inited)
    return;
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  inited = true;
}
#else
/* Sin DWT (Cortex-M0/M0+: STM32F0/L0/G0/C0) no hay nada que habilitar:
 * `mfs_port_cycles` toma entonces la rama que usa HAL_GetTick(), de modo que
 * esta funcion no se llama. El atributo evita -Wunused-function. */
__attribute__((unused)) static void mfs_stm32_dwt_init(void) {}
#endif

/* =====================================================================
 * 3. Primitivas obligatorias
 * ===================================================================== */

#if MFS_STM32_HAVE_FREERTOS

/* --- FreeRTOS --- */
void mfs_port_crit_enter(void) { taskENTER_CRITICAL(); }
void mfs_port_crit_exit(void) { taskEXIT_CRITICAL(); }

uint32_t mfs_port_cycles(void) {
#if MFS_STM32_HAVE_DWT
  mfs_stm32_dwt_init();
  return (uint32_t)DWT->CYCCNT;
#else
  /* Sin DWT (Cortex-M0/M0+): se usa el tick del scheduler. */
  return (uint32_t)xTaskGetTickCount();
#endif
}

uint32_t mfs_port_time_us(void) {
  return (uint32_t)(((uint64_t)xTaskGetTickCount() * 1000000ull) /
                    (uint64_t)configTICK_RATE_HZ);
}

void mfs_port_wfi(void) { taskYIELD(); }

#elif MFS_STM32_HAVE_CMSIS2

/* --- CMSIS-RTOS v2 ---
 * osKernelLock/Unlock son anidables por contador propio del kernel. */
void mfs_port_crit_enter(void) { (void)osKernelLock(); }
void mfs_port_crit_exit(void) { (void)osKernelUnlock(); }

uint32_t mfs_port_cycles(void) {
#if MFS_STM32_HAVE_DWT
  mfs_stm32_dwt_init();
  return (uint32_t)DWT->CYCCNT;
#else
  return osKernelGetSysTimerCount();
#endif
}

uint32_t mfs_port_time_us(void) {
  uint32_t f = osKernelGetTickFreq();
  if (f == 0u)
    return 0u;
  return (uint32_t)(((uint64_t)osKernelGetTickCount() * 1000000ull) /
                    (uint64_t)f);
}

void mfs_port_wfi(void) { (void)osDelay(0u); }

#else

/* --- Bare-metal (super-bucle) ---
 * Sin RTOS no hay scheduler: la seccion critica debe deshabilitar
 * interrupciones y ser ANIDABLE (el nucleo puede anidar crit_enter/exit),
 * guardando el estado de PRIMASK en una pila LIFO, como hace el adaptador de
 * Zephyr del nucleo.
 *
 * STM-10: MAX_NEST = 8 es suficiente para cualquier anidamiento realista del
 * nucleo (normalmente 1-2). Si se desborda, se satura en la ultima entrada. */
#define MFS_STM32_CRIT_MAX_NEST 8u
static uint32_t g_crit_saved[MFS_STM32_CRIT_MAX_NEST];
static uint32_t g_crit_depth = 0u;

void mfs_port_crit_enter(void) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (g_crit_depth < MFS_STM32_CRIT_MAX_NEST)
    g_crit_saved[g_crit_depth] = primask;
  g_crit_depth++;
}

void mfs_port_crit_exit(void) {
  if (g_crit_depth == 0u)
    return;
  g_crit_depth--;
  uint32_t idx = g_crit_depth < MFS_STM32_CRIT_MAX_NEST
                     ? g_crit_depth
                     : MFS_STM32_CRIT_MAX_NEST - 1u;
  uint32_t saved = g_crit_saved[idx];
  if (saved == 0u)
    __enable_irq();
  /* Si ya estabamos en seccion critica (saved == 1), se deja PRIMASK=1. */
}

uint32_t mfs_port_cycles(void) {
#if MFS_STM32_HAVE_DWT
  mfs_stm32_dwt_init();
  return (uint32_t)DWT->CYCCNT;
#else
  /* Cortex-M0/M0+: el SysTick de 24 bits es el unico contador garantizado.
   * Devuelve una base de tiempo monotona en microsegundos para no mentir sobre
   * "ciclos". */
  return (uint32_t)HAL_GetTick() * 1000u;
#endif
}

uint32_t mfs_port_time_us(void) {
  /* STM-13: resolucion real en microsegundos.
   * - Con DWT (Cortex-M3/M4/M7/M33): usa DWT->CYCCNT / (CPU_Hz/1e6).
   * - Sin DWT (Cortex-M0/M0+): usa SysTick->VAL directamente para sub-ms.
   * HAL_GetTick() * 1000 solo tiene resolucion de 1 ms. */
#if MFS_STM32_HAVE_DWT
  mfs_stm32_dwt_init();
  /* SystemCoreClock se actualiza por HAL_Init() / SystemClock_Config(). */
  return (uint32_t)((uint64_t)DWT->CYCCNT * 1000000ull /
                    (uint64_t)SystemCoreClock);
#else
  /* SysTick es de 24 bits descendente; LOAD = reload value.
   * us = (LOAD - VAL) * 1e6 / LOAD. LOAD = SystemCoreClock / 1000 (1 ms). */
  uint32_t load = SysTick->LOAD;
  if (load != 0u) {
    uint32_t val = SysTick->VAL;
    return (uint32_t)HAL_GetTick() * 1000u +
           (uint32_t)((load - val) * 1000ull / load);
  }
  return (uint32_t)HAL_GetTick() * 1000u; /* fallback */
#endif
}

void mfs_port_wfi(void) { __WFI(); }

#endif /* contexto */
