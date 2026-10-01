# Integración con RTOS y plataformas de silicio

Guía de adaptación de MatrixFS Ultra a sistemas operativos de tiempo real y a
los SDK de los principales fabricantes de semiconductores.

Spec: §20.2–§20.3 (contrato de puerto), §13 (RT), MFS-HW-001 (fallback
software). Implementación: [`include/matrixfs/mfs_port_rtos.h`](../include/matrixfs/mfs_port_rtos.h),
[`src/core/mfs_port_rtos.c`](../src/core/mfs_port_rtos.c) (núcleo),
[`platform/rtos/`](../platform/rtos/) (plantilla de portado).

## 1. Qué es el puerto RTOS

MatrixFS no habla directamente con el hardware: consume **cinco primitivas**
(el contrato §20.2, `include/matrixfs/mfs_port.h`):

| Primitiva | Semántica | Latencia objetivo |
|---|---|---|
| `mfs_port_crit_enter/exit` | sección crítica anidable | ≤ 1 µs (§20.3) |
| `mfs_port_cycles` | contador de ciclos libre-corriente | — |
| `mfs_port_time_us` | tiempo monotónico en µs | — |
| `mfs_port_wfi` | idle de bajo consumo durante cuantos ESP (§13.2) | — |

El puerto RTOS (`mfs_port_rtos.*`) mapea esas primitivas a las llamadas nativas
del RTOS en tiempo de compilación. De este modo, el núcleo y el FTL quedan
**deterministas y ajenos al RTOS** (MFS-ARCH-010), y MatrixFS funciona con o
sin scheduler.

## 2. RTOS cubiertos

| RTOS | Detección | Adaptador | Estado |
|---|---|---|---|
| **FreeRTOS** (Amazon FreeRTOS, SMP) | `configUSE_PREEMPTION`, `__has_include(<FreeRTOS.h>)` o `-DMFS_RTOS_FREERTOS=1` | nativo en el núcleo (`taskENTER/EXIT_CRITICAL`, tick) | **Implementado** |
| **Zephyr RTOS** | `__ZEPHYR__` o `__has_include(<zephyr/kernel.h>)` | nativo en el núcleo (`irq_lock/unlock`, `k_cycle_get_32`) | **Implementado** |
| **Eclipse ThreadX** (Azure RTOS) | `-DMFS_RTOS_THREADX=1` | nativo en el núcleo (`tx_interrupt_control`, `tx_time_get`) | **Implementado** |
| **Mbed OS** | `__MBED__` | detección + plantilla (`core_util_critical_section_*`) | Detección + registro |
| **Apache NuttX** | `__NUTTX__` | detección + plantilla (`enter/leave_critical_section`) | Detección + registro |
| **RIOT OS** | `RIOT_VERSION` | detección + plantilla (`irq_disable/restore`) | Detección + registro |
| **Apache Mynewt** | `__MYNEWT__` | detección + plantilla (`OS_ENTER/EXIT_CRITICAL`) | Detección + registro |
| **RT-Thread** | `RT_VERSION` | detección + plantilla (`rt_hw_interrupt_disable/enable`) | Detección + registro |
| **PX5 RTOS** | `-DMFS_RTOS_PX5=1` | detección + plantilla (API del BSP) | Detección + registro |

> Los tres RTOS con más implantación industrial traen adaptador nativo
> compilado en el núcleo. El resto se **detectan** y se resuelven con la
> plantilla `platform/rtos/mfs_rtos_port_template.c`, porque sus builds no se
> pueden verificar en este entorno (véase §6, estado honesto).

## 3. Integración

### 3.1 Uso básico

```c
#include "matrixfs/mfs_port_rtos.h"

void bsp_arranque(void) {
  mfs_port_rtos_init();    /* activa el adaptador nativo detectado */
  mf_init(&fs, &cfg);      /* el núcleo ya usa el puerto */
}
```

`mfs_port_rtos_init()` es idempotente. Si no se detecta RTOS, activa el
adaptador **bare-metal** determinista (contador propio, sección crítica no-op).

### 3.2 Aportar un RTOS no cableado

```c
#include "matrixfs/mfs_port_rtos.h"

/* Rellena con las llamadas nativas de tu RTOS (ver plantilla). */
static const mfs_rtos_ops mi_ops = {
    mi_crit_enter, mi_crit_exit, mi_cycles, mi_time_us,
    mi_wfi,        mi_yield,     MFS_RTOS_CUSTOM, "Mi RTOS",
    (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD)};

mfs_port_rtos_register(&mi_ops);   /* sustituye el adaptador activo */
```

`mfs_port_rtos_register()` devuelve `MFS_EINVAL` si faltan las primitivas
obligatorias (`crit_enter`, `crit_exit`, `cycles`, `time_us`).

### 3.3 Sustituir el contrato global §20.2

Compila el target con **`-DMFS_PORT_RTOS_GLUE=1`** para que el módulo defina los
símbolos globales `mfs_port_crit_enter/exit`, `mfs_port_cycles`,
`mfs_port_time_us` y `mfs_port_wfi`.

> `MFS_PORT_RTOS_GLUE` y `MFS_PORT_ARCH_GLUE` (envoltorio del puerto de
> arquitectura para 8 bits, `src/core/mfs_port_arch.c`) son **excluyentes**: el
> build falla a propósito (`MFS-PORT-001`) si se definen ambos.

### 3.4 Flags de capacidad (`caps`)

Solo declara lo que puedes ejecutar (MFS-HW-001):

| Bit | Significado |
|---|---|
| `MFS_RTOS_CAP_SMP` | la sección crítica es válida en sistemas multi-núcleo |
| `MFS_RTOS_CAP_TICKLESS` | el tiempo proviene de un contador libre, no del tick |
| `MFS_RTOS_CAP_YIELD` | el adaptador aporta `yield()` cooperativo |
| `MFS_RTOS_CAP_NESTING` | `crit_enter/exit` son anidables |

## 4. Plataformas de semiconductores

MatrixFS **no reimplementa** los controladores de silicio: consume su SDK. El
reparto es limpio y explícito:

| Fabricante | SDK / HAL | Qué aporta a MatrixFS |
|---|---|---|
| **Silicon Labs** | Gecko SDK (emlib), `sl_*` driver, `ezradio` | Driver de flash interna/externa (región plana) → capa [`mfs_embedded`](embedded-integration.md) o [`mfs_l2_8bit`](../platform/common/mfs_l2_8bit.c); RTOS Micrium/FreeRTOS → puerto RTOS |
| **Texas Instruments (TI)** | SimpleLink SDK (DriverLib), MSPM0/MSP430/CC13xx-CC26xx | Flash del MCU + FRAM (MSP430FR) → `mfs_embedded`; RTOS TI-RTOS/FreeRTOS → puerto RTOS |
| **Infineon** | ModusToolbox (PSoC/CYW), `cyhal_*` | Flash/SMIF → `mfs_embedded`; RTOS FreeRTOS/ThreadX → puerto RTOS |
| **Renesas** | FSP (RA/RX), `R_FLASH_*` | Flash de datos/código y OSPI → `mfs_embedded` / `mfs_l2_8bit`; RTOS FreeRTOS/ThreadX → puerto RTOS |

En los cuatro casos la secuencia de integración es la misma:

1. **Puerto de almacenamiento**: rellena `mfs_embedded_flash_t`
   (read/prog/erase + geometría) sobre el driver de flash del SDK, o
   `mfs_managed_dev_t` si el medio es gestionado (SD/eMMC/UFS/NVMe/SATA).
   Ver [`storage-integration.md`](storage-integration.md).
2. **Puerto RTOS**: `mfs_port_rtos_init()` (o `mfs_port_rtos_register()`).
3. **Detección y autoconfiguración**: `mf_init(&fs, &cfg)` con
   `cfg.arch_class = MFS_ARCH_AUTO` (MFS-ARCH-010 rev.3: 8/16/32/64 bits y
   aceleración HW).
4. **Arranque**: `mf_mount()`; `mf_format()` en el primer uso.

### Ejemplo de flags por plataforma

```sh
# Silicon Labs + FreeRTOS (GCC, Gecko SDK)
gcc -std=c11 -Os -DMFS_PORT_RTOS_GLUE=1 -DMFS_RTOS_FREERTOS=1 \
    -Iinclude -Isrc -c src/core/mfs_port_rtos.c

# TI MSP430FR (FRAM, sin RTOS) — sin MFS_PORT_RTOS_GLUE, puerto bare-metal
# Infineon PSoC + ThreadX
gcc -std=c11 -Os -DMFS_PORT_RTOS_GLUE=1 -DMFS_RTOS_THREADX=1 ...
# Renesas RA + FreeRTOS (FSP)
cmake -DCMAKE_TOOLCHAIN_FILE=arm-gcc.cmake -DMATRIXFS_BUILD_MCU=ON ..
```

## 5. Plantilla de portado

`platform/rtos/mfs_rtos_port_template.c` es un esqueleto listo para copiar al
BSP: define el estado de anidamiento, las cinco primitivas obligatorias, la
cedida opcional, el descriptor `mfs_rtos_ops` y la función de registro, con la
llamada nativa de cada RTOS indicada en los `TODO`. No se compila por defecto
(vive fuera de los globs de compilación).

## 6. Validación y estado honesto

- **Host (verificado):** `tests/test_rtos.c` comprueba la detección
  (bare-metal en host), los nombres de cada RTOS, la activación e idempotencia
  de `mfs_port_rtos_init()`, el despacho a través del adaptador activo, la
  validación de `mfs_port_rtos_register()` (rechazo de ops incompletos) y la
  sustitución por un adaptador del integrador. También se verifica que el
  envoltorio `MFS_PORT_RTOS_GLUE` compila y que la exclusión con
  `MFS_PORT_ARCH_GLUE` dispara `MFS-PORT-001`.
- **Target (no verificado aquí):** los adaptadores nativos de FreeRTOS, Zephyr
  y ThreadX solo se compilan en el build real de cada RTOS; este entorno no
  dispone de sus toolchains/SDK. El código sigue las API estables de cada uno.

| Elemento | Estado |
|---|---|
| Puerto RTOS genérico + detección + registro + contrato §20.2 | **Implementado y probado** (host) |
| Adaptadores nativos FreeRTOS / Zephyr / ThreadX | **Implementados**; **no compilados aquí** (sin toolchain del RTOS) |
| Detección Mbed / NuttX / RIOT / Mynewt / RT-Thread / PX5 | **Implementada** |
| Adaptadores Mbed / NuttX / RIOT / Mynewt / RT-Thread / PX5 | **Plantilla** + registro por el integrador |
| Integración con SDKs de silicio (SiLabs/TI/Infineon/Renesas) | **Guía + puntos de enganche**; depende del SoC concreto |

Ver también [`embedded-integration.md`](embedded-integration.md) (Arduino,
ESP-IDF, PlatformIO, MicroPython) y [`hal.md`](hal.md) (detección y HWV).
