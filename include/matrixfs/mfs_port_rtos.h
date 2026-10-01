/* mfs_port_rtos.h — MatrixFS «ATLAS» v1.0 — puerto RTOS genérico
 *
 * Capa de adaptación del núcleo que mapea el contrato de puerto §20.2
 * (sección crítica, ciclos, tiempo, WFI) a las primitivas nativas de un
 * sistema operativo de tiempo real (RTOS). El adaptador se selecciona por
 * detección en tiempo de compilación (macros del RTOS huésped) y puede
 * sustituirse por el integrador mediante mfs_port_rtos_register() cuando el
 * RTOS no trae adaptador cableado o el BSP aporta su propio contador.
 *
 * Reglas (MFS-HW-001): el adaptador SOLO declara capacidades que puede
 * ejecutar; la ausencia de un bit implica el mismo resultado por la ruta
 * genérica.
 *
 * Uso típico en el BSP:
 *     mfs_port_rtos_init();         // selecciona el adaptador nativo
 *     mf_init(&fs, &cfg);           // el núcleo ya usa el puerto
 *
 * Para aportar un puerto propio (RTOS no cableado):
 *     static const mfs_rtos_ops mi_ops = { ... };
 *     mfs_port_rtos_register(&mi_ops);
 */
#ifndef MATRIXFS_MFS_PORT_RTOS_H
#define MATRIXFS_MFS_PORT_RTOS_H

#include "mfs_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Identificadores de RTOS/scheduler ==== */
typedef enum {
  MFS_RTOS_NONE = 0,     /* bare-metal, sin scheduler (fallback genérico) */
  MFS_RTOS_ZEPHYR = 1,   /* Zephyr RTOS                                    */
  MFS_RTOS_FREERTOS = 2, /* FreeRTOS / Amazon FreeRTOS / SMP            */
  MFS_RTOS_THREADX = 3,  /* Eclipse ThreadX (Azure RTOS)                 */
  MFS_RTOS_MBED = 4,     /* Mbed OS                                       */
  MFS_RTOS_NUTTX = 5,    /* Apache NuttX                                  */
  MFS_RTOS_RIOT = 6,     /* RIOT OS                                       */
  MFS_RTOS_MYNEWT = 7,   /* Apache Mynewt                                 */
  MFS_RTOS_RTTHREAD = 8, /* RT-Thread                                   */
  MFS_RTOS_PX5 = 9,      /* PX5 RTOS                                      */
  MFS_RTOS_CUSTOM = 0xFF /* aportado por el integrador                  */
} mfs_rtos_id_t;

/* ==== Capacidades declaradas por el adaptador (MFS-HW-001) ==== */
#define MFS_RTOS_CAP_SMP 0x01u      /* sección crítica válida en SMP       */
#define MFS_RTOS_CAP_TICKLESS 0x02u /* tiempo desde contador libre, no tick*/
#define MFS_RTOS_CAP_YIELD 0x04u    /* aporta yield cooperativo            */
#define MFS_RTOS_CAP_NESTING 0x08u  /* crit_enter/exit anidables           */

/* ==== Operaciones del puerto RTOS ==== */
typedef struct mfs_rtos_ops {
  /* Sección crítica (§20.2): latencia objetivo ≤ 1 µs. */
  void (*crit_enter)(void);
  void (*crit_exit)(void);
  /* Contador de ciclos libre-corriente y tiempo monotónico en µs. */
  uint32_t (*cycles)(void);
  uint32_t (*time_us)(void);
  /* Idle de bajo consumo durante cuantos ESP (§13.2). */
  void (*wfi)(void);
  /* Cedida cooperativa opcional (NULL ⇒ no-op). */
  void (*yield)(void);
  uint8_t id;       /* mfs_rtos_id_t                              */
  const char *name; /* nombre legible (diagnóstico)           */
  uint8_t caps;     /* MFS_RTOS_CAP_*                             */
} mfs_rtos_ops;

/* ==== API pública ==== */

/* Detección en tiempo de compilación del RTOS huésped activo. */
mfs_rtos_id_t mfs_rtos_detect(void);

/* Nombre legible de un identificador (nunca NULL). */
const char *mfs_rtos_name(mfs_rtos_id_t id);

/* Inicializa el puerto: activa el adaptador nativo detectado (o el genérico
 * bare-metal si no se detecta RTOS). Idempotente. Llamar antes de mf_init(). */
mfs_st mfs_port_rtos_init(void);

/* El integrador sustituye el adaptador activo (RTOS no cableado o BSP propio).
 * Devuelve MFS_EINVAL si ops es NULL o le faltan las primitivas obligatorias.
 */
mfs_st mfs_port_rtos_register(const mfs_rtos_ops *ops);

/* Adaptador activo (NULL si no se ha inicializado/registrado). */
const mfs_rtos_ops *mfs_port_rtos_get_ops(void);

/* Despacho de bajo nivel: usan el adaptador activo; no-op/0 si no hay ninguno.
 * Son la superficie que consume el núcleo a través del contrato §20.2. */
void mfs_port_rtos_crit_enter(void);
void mfs_port_rtos_crit_exit(void);
uint32_t mfs_port_rtos_cycles(void);
uint32_t mfs_port_rtos_time_us(void);
void mfs_port_rtos_wfi(void);
void mfs_port_rtos_yield(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_MFS_PORT_RTOS_H */
