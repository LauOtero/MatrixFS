/* mfs_port_rtos.c — MatrixFS Ultra «ATLAS» v1.0 — puerto RTOS genérico
 *
 * Forma parte del NÚCLEO. Selecciona, por detección en tiempo de compilación,
 * el adaptador nativo del RTOS huésped y expone las primitivas del contrato
 * §20.2 (sección crítica, ciclos, tiempo, WFI) con nombres estables.
 *
 * RTOS con adaptador cableado en el núcleo (los más usados): FreeRTOS,
 * Zephyr y Eclipse ThreadX. El resto (Mbed OS, NuttX, RIOT, Mynewt,
 * RT-Thread, PX5) se detectan y el integrador aporta su adaptador con
 * mfs_port_rtos_register() usando la plantilla platform/rtos/.
 *
 * Reglas de codificación (MFS-ARCH-010 rev.3): C puro, sin heap, estado
 * estático y determinista.
 */

#include "matrixfs/mfs_port_rtos.h"

#include <string.h>

/* Los dos envoltorios (§20.2) que definen los símbolos globales mfs_port_* son
 * mutuamente excluyentes: el de arquitectura (src/core/mfs_port_arch.c) y el
 * del RTOS (este). Definir ambos produciría símbolos duplicados. */
#if defined(MFS_PORT_RTOS_GLUE) && defined(MFS_PORT_ARCH_GLUE)
#error "MFS-PORT-001: MFS_PORT_RTOS_GLUE y MFS_PORT_ARCH_GLUE son excluyentes"
#endif

/* =====================================================================
 * Detección del RTOS huésped (tiempo de compilación)
 * ===================================================================== */

/* Sonda de cabecera portátil: `__has_include` no existe en todo compilador. */
#if defined(__has_include)
#define MFS_HAS_INCLUDE(x) __has_include(x)
#else
#define MFS_HAS_INCLUDE(x) 0
#endif

/* El integrador puede forzar cualquiera de estos guiones con
 * -DMFS_RTOS_<NOMBRE>=1 si el RTOS no define una macro reconocible. */
#if defined(__ZEPHYR__) || defined(MFS_RTOS_ZEPHYR) ||                         \
    MFS_HAS_INCLUDE(<zephyr / kernel.h>)
#define MFS_RTOS_HAVE_ZEPHYR 1
#else
#define MFS_RTOS_HAVE_ZEPHYR 0
#endif

#if defined(MFS_RTOS_FREERTOS) || defined(INC_FREERTOS_H) ||                   \
    defined(configUSE_PREEMPTION) || MFS_HAS_INCLUDE(<FreeRTOS.h>)
#define MFS_RTOS_HAVE_FREERTOS 1
#else
#define MFS_RTOS_HAVE_FREERTOS 0
#endif

#if defined(MFS_RTOS_THREADX) || defined(TX_INCLUDE_USER_DEFINE_FILE)
#define MFS_RTOS_HAVE_THREADX 1
#else
#define MFS_RTOS_HAVE_THREADX 0
#endif

#if defined(__MBED__) || defined(MBED_VERSION_MAJOR) || defined(MFS_RTOS_MBED)
#define MFS_RTOS_HAVE_MBED 1
#else
#define MFS_RTOS_HAVE_MBED 0
#endif

#if defined(__NUTTX__) || defined(MFS_RTOS_NUTTX)
#define MFS_RTOS_HAVE_NUTTX 1
#else
#define MFS_RTOS_HAVE_NUTTX 0
#endif

#if defined(RIOT_VERSION) || defined(MFS_RTOS_RIOT)
#define MFS_RTOS_HAVE_RIOT 1
#else
#define MFS_RTOS_HAVE_RIOT 0
#endif

#if defined(__MYNEWT__) || defined(MFS_RTOS_MYNEWT)
#define MFS_RTOS_HAVE_MYNEWT 1
#else
#define MFS_RTOS_HAVE_MYNEWT 0
#endif

#if defined(RT_VERSION) || defined(__RTTHREAD__) || defined(MFS_RTOS_RTTHREAD)
#define MFS_RTOS_HAVE_RTTHREAD 1
#else
#define MFS_RTOS_HAVE_RTTHREAD 0
#endif

#if defined(MFS_RTOS_PX5) || defined(PX5_RTOS)
#define MFS_RTOS_HAVE_PX5 1
#else
#define MFS_RTOS_HAVE_PX5 0
#endif

mfs_rtos_id_t mfs_rtos_detect(void) {
#if MFS_RTOS_HAVE_ZEPHYR
  return MFS_RTOS_ZEPHYR;
#elif MFS_RTOS_HAVE_FREERTOS
  return MFS_RTOS_FREERTOS;
#elif MFS_RTOS_HAVE_THREADX
  return MFS_RTOS_THREADX;
#elif MFS_RTOS_HAVE_MBED
  return MFS_RTOS_MBED;
#elif MFS_RTOS_HAVE_NUTTX
  return MFS_RTOS_NUTTX;
#elif MFS_RTOS_HAVE_RIOT
  return MFS_RTOS_RIOT;
#elif MFS_RTOS_HAVE_MYNEWT
  return MFS_RTOS_MYNEWT;
#elif MFS_RTOS_HAVE_RTTHREAD
  return MFS_RTOS_RTTHREAD;
#elif MFS_RTOS_HAVE_PX5
  return MFS_RTOS_PX5;
#else
  return MFS_RTOS_NONE;
#endif
}

const char *mfs_rtos_name(mfs_rtos_id_t id) {
  switch (id) {
  case MFS_RTOS_ZEPHYR:
    return "Zephyr RTOS";
  case MFS_RTOS_FREERTOS:
    return "FreeRTOS";
  case MFS_RTOS_THREADX:
    return "Eclipse ThreadX";
  case MFS_RTOS_MBED:
    return "Mbed OS";
  case MFS_RTOS_NUTTX:
    return "Apache NuttX";
  case MFS_RTOS_RIOT:
    return "RIOT OS";
  case MFS_RTOS_MYNEWT:
    return "Apache Mynewt";
  case MFS_RTOS_RTTHREAD:
    return "RT-Thread";
  case MFS_RTOS_PX5:
    return "PX5 RTOS";
  case MFS_RTOS_CUSTOM:
    return "Custom (integrador)";
  case MFS_RTOS_NONE:
  default:
    return "Bare-metal";
  }
}

/* =====================================================================
 * Adaptador bare-metal (fallback determinista, sin scheduler)
 * ===================================================================== */
static uint32_t g_bare_counter = 0;

static void bare_crit_enter(void) {}
static void bare_crit_exit(void) {}
static uint32_t bare_cycles(void) { return g_bare_counter++; }
static uint32_t bare_time_us(void) { return g_bare_counter; }
static void bare_wfi(void) {
#if defined(__GNUC__) || defined(__clang__)
  __asm__ volatile("nop");
#endif
}

static const mfs_rtos_ops bare_ops = {
    bare_crit_enter, bare_crit_exit, bare_cycles,
    bare_time_us,    bare_wfi,       NULL,
    MFS_RTOS_NONE,   "Bare-metal",   MFS_RTOS_CAP_NESTING};

/* =====================================================================
 * Adaptador FreeRTOS (tarea + tick)
 * ===================================================================== */
#if MFS_RTOS_HAVE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"

/* taskENTER_CRITICAL/taskEXIT_CRITICAL son anidables (portable). */
static void fr_crit_enter(void) { taskENTER_CRITICAL(); }
static void fr_crit_exit(void) { taskEXIT_CRITICAL(); }

/* Sin contador de ciclos portable: se usa el tick del scheduler. */
static uint32_t fr_cycles(void) { return (uint32_t)xTaskGetTickCount(); }

static uint32_t fr_time_us(void) {
  return (uint32_t)(((uint64_t)xTaskGetTickCount() * 1000000ull) /
                    (uint64_t)configTICK_RATE_HZ);
}

static void fr_wfi(void) { taskYIELD(); }
static void fr_yield(void) { taskYIELD(); }

static const mfs_rtos_ops freertos_ops = {
    fr_crit_enter,
    fr_crit_exit,
    fr_cycles,
    fr_time_us,
    fr_wfi,
    fr_yield,
    MFS_RTOS_FREERTOS,
    "FreeRTOS",
    (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD | MFS_RTOS_CAP_SMP)};
#endif /* MFS_RTOS_HAVE_FREERTOS */

/* =====================================================================
 * Adaptador Zephyr RTOS (irq_lock + contador de ciclos del SoC)
 * ===================================================================== */
#if MFS_RTOS_HAVE_ZEPHYR
#include <zephyr/kernel.h>

#define MFS_ZP_MAX_NEST 8u
static uint32_t g_zp_keys[MFS_ZP_MAX_NEST];
static uint32_t g_zp_depth = 0;

static void zp_crit_enter(void) {
  unsigned int key = irq_lock();
  if (g_zp_depth < MFS_ZP_MAX_NEST)
    g_zp_keys[g_zp_depth] = (uint32_t)key;
  g_zp_depth++;
}

static void zp_crit_exit(void) {
  if (g_zp_depth == 0u)
    return;
  g_zp_depth--;
  irq_unlock(
      (unsigned int)
          g_zp_keys[g_zp_depth < MFS_ZP_MAX_NEST ? g_zp_depth
                                                 : MFS_ZP_MAX_NEST - 1u]);
}

static uint32_t zp_cycles(void) { return (uint32_t)k_cycle_get_32(); }
static uint32_t zp_time_us(void) {
  return k_cyc_to_us_floor32(k_cycle_get_32());
}
static void zp_wfi(void) { k_yield(); }
static void zp_yield(void) { k_yield(); }

static const mfs_rtos_ops zephyr_ops = {
    zp_crit_enter,
    zp_crit_exit,
    zp_cycles,
    zp_time_us,
    zp_wfi,
    zp_yield,
    MFS_RTOS_ZEPHYR,
    "Zephyr RTOS",
    (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD |
              MFS_RTOS_CAP_TICKLESS | MFS_RTOS_CAP_SMP)};
#endif /* MFS_RTOS_HAVE_ZEPHYR */

/* =====================================================================
 * Adaptador Eclipse ThreadX (tx_interrupt_control + tick)
 * ===================================================================== */
#if MFS_RTOS_HAVE_THREADX
#include "tx_api.h"

#define MFS_TX_MAX_NEST 8u
static ULONG g_tx_saved[MFS_TX_MAX_NEST];
static uint32_t g_tx_depth = 0;

static void tx_crit_enter(void) {
  ULONG saved = tx_interrupt_control(TX_INT_DISABLE);
  if (g_tx_depth < MFS_TX_MAX_NEST)
    g_tx_saved[g_tx_depth] = saved;
  g_tx_depth++;
}

static void tx_crit_exit(void) {
  if (g_tx_depth == 0u)
    return;
  g_tx_depth--;
  tx_interrupt_control(
      g_tx_saved[g_tx_depth < MFS_TX_MAX_NEST ? g_tx_depth
                                              : MFS_TX_MAX_NEST - 1u]);
}

static uint32_t tx_cycles(void) { return (uint32_t)tx_time_get(); }
static uint32_t tx_time_us(void) {
  return (uint32_t)(((uint64_t)tx_time_get() * 1000000ull) /
                    (uint64_t)TX_TIMER_TICKS_PER_SECOND);
}
static void tx_wfi(void) { tx_thread_relinquish(); }
static void tx_yield(void) { tx_thread_relinquish(); }

static const mfs_rtos_ops threadx_ops = {
    tx_crit_enter,
    tx_crit_exit,
    tx_cycles,
    tx_time_us,
    tx_wfi,
    tx_yield,
    MFS_RTOS_THREADX,
    "Eclipse ThreadX",
    (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD)};
#endif /* MFS_RTOS_HAVE_THREADX */

/* =====================================================================
 * Selección y registro
 * ===================================================================== */
static const mfs_rtos_ops *g_rtos_ops = NULL;

static const mfs_rtos_ops *builtin_ops(mfs_rtos_id_t id) {
  switch (id) {
#if MFS_RTOS_HAVE_FREERTOS
  case MFS_RTOS_FREERTOS:
    return &freertos_ops;
#endif
#if MFS_RTOS_HAVE_ZEPHYR
  case MFS_RTOS_ZEPHYR:
    return &zephyr_ops;
#endif
#if MFS_RTOS_HAVE_THREADX
  case MFS_RTOS_THREADX:
    return &threadx_ops;
#endif
  default:
    return &bare_ops;
  }
}

mfs_st mfs_port_rtos_init(void) {
  g_rtos_ops = builtin_ops(mfs_rtos_detect());
  return MFS_OK;
}

mfs_st mfs_port_rtos_register(const mfs_rtos_ops *ops) {
  if (!ops || !ops->crit_enter || !ops->crit_exit || !ops->cycles ||
      !ops->time_us)
    return MFS_EINVAL;
  g_rtos_ops = ops;
  return MFS_OK;
}

const mfs_rtos_ops *mfs_port_rtos_get_ops(void) { return g_rtos_ops; }

/* =====================================================================
 * Despacho de bajo nivel (contrato §20.2)
 * ===================================================================== */
void mfs_port_rtos_crit_enter(void) {
  if (g_rtos_ops && g_rtos_ops->crit_enter)
    g_rtos_ops->crit_enter();
}

void mfs_port_rtos_crit_exit(void) {
  if (g_rtos_ops && g_rtos_ops->crit_exit)
    g_rtos_ops->crit_exit();
}

uint32_t mfs_port_rtos_cycles(void) {
  return (g_rtos_ops && g_rtos_ops->cycles) ? g_rtos_ops->cycles() : 0u;
}

uint32_t mfs_port_rtos_time_us(void) {
  return (g_rtos_ops && g_rtos_ops->time_us) ? g_rtos_ops->time_us() : 0u;
}

void mfs_port_rtos_wfi(void) {
  if (g_rtos_ops && g_rtos_ops->wfi)
    g_rtos_ops->wfi();
}

void mfs_port_rtos_yield(void) {
  if (g_rtos_ops && g_rtos_ops->yield)
    g_rtos_ops->yield();
}

/* =====================================================================
 * Envoltorio opcional del contrato §20.2 (build de target con RTOS)
 * ---------------------------------------------------------------------
 * Definir MFS_PORT_RTOS_GLUE=1 en el build de target que use un RTOS y NO
 * definir MFS_PORT_ARCH_GLUE (son excluyentes). En host/tests las primitivas
 * las aporta sim/mfs_port_host.c y este bloque queda desactivado.
 * ===================================================================== */
#if defined(MFS_PORT_RTOS_GLUE)
void mfs_port_crit_enter(void) { mfs_port_rtos_crit_enter(); }
void mfs_port_crit_exit(void) { mfs_port_rtos_crit_exit(); }
uint32_t mfs_port_cycles(void) { return mfs_port_rtos_cycles(); }
uint32_t mfs_port_time_us(void) { return mfs_port_rtos_time_us(); }
void mfs_port_wfi(void) { mfs_port_rtos_wfi(); }
#endif /* MFS_PORT_RTOS_GLUE */
