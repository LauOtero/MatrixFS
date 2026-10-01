/* mfs_rtos_port_template.c — Plantilla de portado RTOS para MatrixFS
 *
 * Copia este fichero a tu BSP, renómbralo (p. ej. mfs_rtos_port_miRTOS.c) y
 * completa las primitivas del contrato §20.2 con las llamadas nativas de tu
 * RTOS. Compílalo junto al núcleo definiendo MFS_PORT_RTOS_GLUE=1 (y NO
 * definiendo MFS_PORT_ARCH_GLUE).
 *
 * Objetivo (§20.2/§20.3): sección crítica con latencia ≤ 1 µs y anidables,
 * contador de ciclos libre-corriente, tiempo monotónico en µs e idle de bajo
 * consumo. MFS-HW-001: declara en `caps` SOLO lo que puedas ejecutar.
 *
 * Este fichero NO forma parte del build por defecto (vive en platform/rtos/,
 * fuera de los globs de compilación): es material de partida.
 */

#include "matrixfs/mfs_port_rtos.h"

/* ------------------------------------------------------------------ */
/* 1. Estado para secciones críticas anidables                         */
/* ------------------------------------------------------------------ */
#define MI_RTOS_MAX_NEST 8u
static uint32_t g_nest_depth = 0;
static uint32_t g_nest_save[MI_RTOS_MAX_NEST];

/* ------------------------------------------------------------------ */
/* 2. Primitivas obligatorias (§20.2) — RELLENAR con tu RTOS           */
/* ------------------------------------------------------------------ */

static void mi_rtos_crit_enter(void) {
  /* TODO: guarda el estado de interrupciones y deshabilítalas.
   *   Mbed OS : core_util_critical_section_enter();
   *   NuttX   : irqstate_t f = enter_critical_section();
   *   RIOT    : unsigned s = irq_disable();
   *   Mynewt  : os_sr_t sr; OS_ENTER_CRITICAL(sr);
   *   RT-Thread: rt_base_t l = rt_hw_interrupt_disable();
   *   PX5     : px5_critical_section_enter();  (API del BSP)
   */
  uint32_t saved = 0; /* TODO: valor nativo a restaurar */
  if (g_nest_depth < MI_RTOS_MAX_NEST)
    g_nest_save[g_nest_depth] = saved;
  g_nest_depth++;
}

static void mi_rtos_crit_exit(void) {
  if (g_nest_depth == 0u)
    return;
  g_nest_depth--;
  /* TODO: restaura el estado guardado (LIFO):
   *   Mbed OS : core_util_critical_section_exit();
   *   NuttX   : leave_critical_section(g_nest_save[...]);
   *   RIOT    : irq_restore(g_nest_save[...]);
   *   Mynewt  : OS_EXIT_CRITICAL(sr);
   *   RT-Thread: rt_hw_interrupt_enable(g_nest_save[...]);
   */
  (void)g_nest_save[g_nest_depth < MI_RTOS_MAX_NEST ? g_nest_depth
                                                    : MI_RTOS_MAX_NEST - 1u];
}

static uint32_t mi_rtos_cycles(void) {
  /* TODO: contador de ciclos libre-corriente (DWT_CYCCNT, SysTick extendido,
   * timer de 32 bits sin parar). Si no existe, usa el tick del scheduler. */
  return 0u;
}

static uint32_t mi_rtos_time_us(void) {
  /* TODO: tiempo monotónico en µs. Ideal: k_cycle/hw cycles convertidos a µs.
   * Si solo hay tick: ticks * 1000000 / TICKS_POR_SEGUNDO. */
  return 0u;
}

static void mi_rtos_wfi(void) {
  /* TODO: idle de bajo consumo o cedida. p. ej. __WFI() o el equivalente
   * cooperativo del RTOS. */
}

/* ------------------------------------------------------------------ */
/* 3. Primitiva opcional: cedida cooperativa                           */
/* ------------------------------------------------------------------ */
static void mi_rtos_yield(void) {
  /* TODO: p. ej. osThreadYield() / sched_yield() / rt_thread_yield(). */
}

/* ------------------------------------------------------------------ */
/* 4. Descriptor del adaptador                                         */
/* ------------------------------------------------------------------ */
static const mfs_rtos_ops mi_rtos_ops = {
    mi_rtos_crit_enter, mi_rtos_crit_exit,
    mi_rtos_cycles,     mi_rtos_time_us,
    mi_rtos_wfi,        mi_rtos_yield,
    MFS_RTOS_CUSTOM, /* o el id que corresponda si ya está cableado */
    "Mi RTOS",          (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD)};

/* ------------------------------------------------------------------ */
/* 5. Registro: llamar UNA vez antes de mf_init()                      */
/* ------------------------------------------------------------------ */
mfs_st mi_rtos_port_setup(void) { return mfs_port_rtos_register(&mi_rtos_ops); }
