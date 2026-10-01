# Puerto RTOS — MatrixFS

Capa de adaptación del contrato de puerto §20.2 a sistemas operativos de
tiempo real. El núcleo ya trae adaptadores nativos para los RTOS más usados y
un mecanismo de registro para el resto.

## Adaptadores cableados en el núcleo

| RTOS | Detección | Sección crítica | Tiempo |
|---|---|---|---|
| **FreeRTOS** | `configUSE_PREEMPTION` / `__has_include(<FreeRTOS.h>)` / `-DMFS_RTOS_FREERTOS=1` | `taskENTER/EXIT_CRITICAL` | tick × rate |
| **Zephyr RTOS** | `__ZEPHYR__` / `__has_include(<zephyr/kernel.h>)` | `irq_lock/unlock` (anidable) | `k_cycle_get_32` |
| **Eclipse ThreadX** | `-DMFS_RTOS_THREADX=1` | `tx_interrupt_control` (anidable) | `tx_time_get` |

## RTOS detectados que requieren adaptador del integrador

Mbed OS, NuttX, RIOT, Mynewt, RT-Thread y PX5 se detectan por macro
(`__MBED__`, `__NUTTX__`, `RIOT_VERSION`, `__MYNEWT__`, `RT_VERSION`,
`-DMFS_RTOS_PX5=1`) y se resuelven aportando un `mfs_rtos_ops` propio.

## Uso

```c
#include "matrixfs/mfs_port_rtos.h"

mfs_port_rtos_init();          /* activa el adaptador nativo detectado */
/* ... o para un RTOS no cableado: */
mfs_port_rtos_register(&mi_rtos_ops);
```

Compila con `-DMFS_PORT_RTOS_GLUE=1` para que este módulo defina los símbolos
globales `mfs_port_crit_enter/exit`, `mfs_port_cycles`, `mfs_port_time_us` y
`mfs_port_wfi` del contrato §20.2. Es **excluyente** con `MFS_PORT_ARCH_GLUE`
(el build falla a propósito si se definen ambos).

`mfs_rtos_port_template.c` es la plantilla de partida (no se compila por
defecto). Guía completa en [`DOCS/rtos-integration.md`](../../DOCS/rtos-integration.md).
