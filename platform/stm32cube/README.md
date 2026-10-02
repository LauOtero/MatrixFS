# MatrixFS «ATLAS» — port STM32Cube

Integración de **MatrixFS** (sistema de archivos embebido en C11, sin heap) con
**STM32Cube** (HAL de ST) sobre los medios de almacenamiento típicos de un STM32.

El port **no reimplementa** el sistema de archivos: compila el núcleo (`src/**`)
y la capa embebida compartida (`platform/embedded/mfs_embedded.c`), y aporta
únicamente:

1. las **primitivas obligatorias del puerto** §20.2 (`mfs_port_*`), en
   `src/mfs_stm32_port.c`;
2. los **callbacks de medio** sobre el HAL, uno por tecnología;
3. la **API de montaje/formateo** y la validación de la región reservada.

> **Instancia única.** El núcleo usa tablas estáticas (sin heap) y el port aloja
> el descriptor del medio en estado estático: **solo puede haber un volumen
> MatrixFS montado a la vez**.

> **El núcleo no es reentrante.** Ver §Concurrencia antes de usarlo desde varias
> tareas de FreeRTOS.

---

## Índice

- [1. Medios soportados](#1-medios-soportados)
- [2. Presupuesto de RAM y capacidad (leer antes de elegir MCU)](#2-presupuesto-de-ram-y-capacidad-leer-antes-de-elegir-mcu)
- [3. Integración paso a paso](#3-integración-paso-a-paso)
- [4. Reservar la región en la flash interna](#4-reservar-la-región-en-la-flash-interna)
- [5. Geometría: sectores no uniformes y ventana uniforme](#5-geometría-sectores-no-uniformes-y-ventana-uniforme)
- [6. Flash interna: ECC, unidades de 8/16 B y bloques](#6-flash-interna-ecc-unidades-de-816-b-y-bloqueos)
- [7. Concurrencia](#7-concurrencia)
- [8. Puerto §20.2: FreeRTOS, CMSIS-RTOS v2 y bare-metal](#8-puerto-202-freertos-cmsis-rtos-v2-y-bare-metal)
- [9. API pública](#9-api-pública)
- [10. Estado de verificación](#10-estado-de-verificación)
- [11. Solución de problemas](#11-solución-de-problemas)

---

## 1. Medios soportados

| Medio | Fichero | Capa usada | Notas |
|---|---|---|---|
| **NOR externa OSPI/QUADSPI** | `mfs_stm32_ospi.c` | `mfs_embedded` | **Recomendado.** Sectores uniformes de 4 KB, sin ECC, no bloquea la CPU |
| **Flash interna** | `mfs_stm32_iflash.c` | `mfs_embedded` | Unidad de 4/8/16 B + ECC; sectores no uniformes; ver §6 |
| **NOR externa SPI** | `mfs_stm32_mem.c` | `mfs_l2_8bit` | Comandos JEDEC, WIP, autodetección de capacidad |
| **FRAM SPI/I²C** | `mfs_stm32_mem.c` | `mfs_l2_8bit` | Sin borrado ni desgaste efectivo |
| **EEPROM SPI/I²C** | `mfs_stm32_mem.c` | `mfs_l2_8bit` | *Page write* + acknowledge polling |
| **SD / eMMC** | `mfs_stm32_sd.c` | `mfs_l2_managed` | El dispositivo trae su FTL: el núcleo no lo duplica |

**Recomendación por defecto:** usa **NOR externa OSPI/QSPI** o **SD/eMMC**. La
flash interna funciona, pero tiene tres costes reales (RAM, ECC y bloqueo de la
CPU al borrar) que se detallan en §2 y §6.

---

## 2. Presupuesto de RAM y capacidad (leer antes de elegir MCU)

Esta es la parte que decide si MatrixFS cabe en tu STM32. **Las cifras están
medidas sobre el código de este repositorio**, con `arm-none-eabi`/`gcc` y `size`
(sección `.bss` del núcleo + capa embebida, sin la aplicación):

| Configuración | `.bss` núcleo | `sizeof(mf_t)` | Pila de tarea | **Total RAM** |
|---|---|---|---|---|
| Por defecto (32 bits) | **202 496 B** (≈198 KB) | **17 240 B** | ≥ 16 KB | **≈ 232 KB** |
| `-DMFS_L2P_SLOTS=1024` | 165 440 B (≈162 KB) | 17 240 B | ≥ 16 KB | ≈ 196 KB |
| `-DMFS_L2P_SLOTS=1024` sin `src/sec/mfs_pq.c` | ≈153 KB | 17 240 B | ≥ 16 KB | ≈ 186 KB |
| `-DMFS_ALLOW_8BIT_TARGET=1` (perfil mínimo) | **87 168 B** (≈85 KB) | **1 840 B** | ≥ 8 KB | **≈ 95 KB** |

De dónde sale el consumo (medido, `.bss` por unidad, configuración por defecto):

| Unidad | `.bss` | Qué lo ocupa |
|---|---|---|
| `mfs_zone.c` | 45 KB | mapa L2P (32 KB con `MFS_L2P_SLOTS=4096`) + tabla de zonas |
| `mfs_compact.c` | 34 KB | pools CFX y buffers de *scratch* |
| `mfs_ftl2.c` | 34 KB | *scratch* de WOM/SLEC/EBA/ZRP |
| `mfs_fs.c` | 33 KB | ventana de inodos y *scratch* |
| `mfs_xio.c` | 25 KB | pool SDP y buffers |
| `mfs_pq.c` | 12 KB | LMS |
| `mfs_hmt.c` | 12 KB | tiering T0/T1 |
| resto | ≈7 KB | WAL, Blake3, utilidades |

**Consecuencias prácticas:**

- **Ningún STM32 con menos de ~96 KB de RAM puede montar MatrixFS**, ni siquiera
  con el perfil mínimo; con la configuración por defecto hacen falta ≈230 KB.
- **La pila de la tarea que llama al núcleo debe ser grande.** El núcleo declara
  buffers de `MFS_SCRATCH_MAX` (4096 B en 32 bits) **en la pila** en varias rutas
  (`mfs_fs.c`, `mfs_suites.c`), y pueden anidarse. Con
  `configMINIMAL_STACK_SIZE` (128 palabras = 512 B por defecto en CubeMX) el
  desbordamiento es **inmediato**. Define, por ejemplo:
  ```c
  #define MFS_TASK_STACK_WORDS 4096   /* 16 KB */
  osThreadNew(tarea_fs, NULL, &(osThreadAttr_t){ .stack_size = MFS_TASK_STACK_WORDS * 4 });
  ```
- **`MFS_STM32_RAM_BUDGET` no reserva memoria.** Es lo que el port *declara* al
  planificador de viabilidad para elegir el modo operativo. El consumo real se
  fija en **compilación** con `MFS_L2P_SLOTS`, `MFS_ZONE_MAX` y
  `MFS_SCRATCH_MAX`. Declarar 32 KB no hace que MatrixFS ocupe 32 KB.
- **El port ya no añade su propia ráfaga de pila.** El backend de la flash
  interna (`mfs_stm32_iflash.c`) guardaba un mapa de sectores expandido de 16 KB
  **en la pila** de `prepare`; ahora guarda solo el rango de índices sobre el
  mapa ordenado. El estado del backend pasó de ~16 KB a ~40 B. (La pila mínima
  de 16 KB de la tabla anterior la siguen fijando los *scratch* del **núcleo**,
  no el port.)

### Capacidad útil

El núcleo direcciona como máximo `MFS_ZONE_MAX` zonas de `erase_unit` bytes tras
los 3 sectores reservados (SB A, SB B y anillo de tokens):

```
capacidad_util ≈ MFS_ZONE_MAX × erase_unit
```

| `erase_unit` | `MFS_ZONE_MAX=128` | zonas del medio |
|---|---|---|
| 4 KB (NOR SPI) | 512 KB | 128 |
| 2 KB (L4/G0/G4) | 256 KB | 128 |
| 128 KB (F4/H7) | 16 MB | 128 |

Una partición o región **mayor no se aprovecha**. El port lo detecta y lo
reporta: `matrixfs_stm32_capacity_wasteful()` devuelve `true` y
`matrixfs_stm32_capacity_bytes()` da la cifra real. Para aprovechar más espacio,
sube `MFS_ZONE_MAX` (y asume la RAM extra: ≈96 B por zona dentro de `mf_t`).

También hay un tope por el mapa L2P: `MFS_L2P_SLOTS` entradas de 8 B cubren
`MFS_L2P_SLOTS × chunk_size` bytes de páginas lógicas. Con 4096 entradas y chunk
de 4096 B son ~16 MB, holgado para cualquier caso; al agotarse se devuelve
`MFS_ETABLEFULL` (traducido a `ENOSPC`).

---

## 3. Integración paso a paso

### 3.1 Lo que hay que habilitar en CubeMX

| Medio | Periférico | Notas |
|---|---|---|
| OSPI/QUADSPI | `OCTOSPI1` / `QUADSPI` | Configurar el modo de acceso; el port usa el **modo indirecto** para leer y escribir |
| SPI NOR / FRAM / EEPROM SPI | `SPI1..n` | NSS por hardware o CS por software (ver §3.4) |
| FRAM/EEPROM I²C | `I2C1..n` | El port usa `HAL_I2C_Mem_Read/Write` (repeated-START) |
| SD/eMMC | `SDMMC1`/`SDMMC2` | Debe estar **inicializado** antes de montar |
| Flash interna | *(nada)* | Solo requiere reservar la región en el linker (§4) |
| Ciclos reales | `DWT` | Automático en Cortex-M3/M4/M7/M33; en M0/M0+ (F0/L0/G0/C0) se cae a `HAL_GetTick()` |

### 3.2 Añadir las fuentes

**Opción A — CMake / STM32CubeCLT (recomendada, estable frente a la regeneración del `.ioc`):**

```cmake
add_subdirectory(third_party/MatrixFS/platform/stm32cube)
target_link_libraries(mi_firmware PRIVATE matrixfs_stm32)

# Ajuste de RAM/capacidad (por defecto 4096 y 128)
set(MFS_L2P_SLOTS 1024 CACHE STRING "")
set(MFS_ZONE_MAX   64  CACHE STRING "")
```

**Opción B — toolchain «Makefile» de CubeMX:**

```sh
make -f Makefile -f /ruta/MatrixFS/platform/stm32cube/matrixfs_stm32.mk
```

o añade al final del Makefile del proyecto:

```make
include /ruta/MatrixFS/platform/stm32cube/matrixfs_stm32.mk
```

> CubeMX **regenera** el Makefile del proyecto, de modo que la línea `include` se
> pierde en la siguiente generación. La forma estable es el doble `-f` o un
> wrapper propio.

**Opción C — STM32CubeIDE:** añade la carpeta `platform/` del repositorio como
*carpeta enlazada* del proyecto (New → Folder → Advanced → Link to alternate
location) y **excluye del build** los directorios que no apliquen. Añade los
*include paths* y los `-D` que lista `matrixfs_stm32.mk`. Esta vía es la única
que CubeIDE conserva al regenerar código, porque vive en `.cproject` y no en el
Makefile generado.

**Qué NO debe compilarse:** el puerto aporta `mfs_port_*`, así que
`src/core/mfs_port_arch.c` y `src/core/mfs_port_rtos.c` **quedan fuera del build**
(serían definiciones duplicadas). El `.mk` y el `CMakeLists.txt` ya los excluyen
mediante *glob + filter-out*, de modo que un fichero nuevo del núcleo entra solo
y no hay listas escritas a mano que se puedan quedar obsoletas.

> **Define el macro de FAMILIA además del de dispositivo.** CubeMX solo añade el
> macro del **dispositivo** (`-DSTM32F407xx`, `-DSTM32H743xx`…). El puerto, para
> elegir la cabecera HAL (`stm32f4xx_hal.h`, …), necesita además el macro de
> **familia** (`-DSTM32F4xx`, `-DSTM32H7xx`…). Añádelo tú:
>
> ```make
> C_DEFS += -DSTM32F4xx      # misma familia que tu dispositivo CubeMX
> ```
>
> Sin él la compilación para con
> `error: "MatrixFS: familia STM32 no reconocida…"`. Es la única bandera extra
> que el `.mk` no puede deducir por ti.

**Backends opcionales.** Cada backend usa el periférico de su medio; si tu
dispositivo **no** lo tiene, hay que excluir el fichero o la compilación falla.
Por ejemplo, un **F407 no tiene QUADSPI**, así que `src/mfs_stm32_ospi.c` no
compila. Tanto el `CMakeLists.txt` como el `.mk` aceptan las mismas banderas:

| Bandera | Excluye | Cuándo |
|---|---|---|
| `MATRIXFS_NO_OSPI=1` | `mfs_stm32_ospi.c` | Dispositivo sin OCTOSPI/QUADSPI (p. ej. F407, F1, L0) |
| `MATRIXFS_NO_IFLASH=1` | `mfs_stm32_iflash.c` | No se usa la flash interna como medio |

```cmake
set(MATRIXFS_NO_OSPI 1 CACHE STRING "")   # CMake
```
```make
make ... MATRIXFS_NO_OSPI=1               # Makefile de CubeMX
```

### 3.3 Configurar el port

Copia `include/mfs_stm32_conf_template.h` a `Core/Inc/mfs_stm32_conf.h` y ajusta:

```c
#define MFS_STM32_MEDIA            MFS_STM32_MEDIA_OSPI_NOR
#define MFS_STM32_RAM_BUDGET       65536u
#define MFS_STM32_FORMAT_IF_NEEDED 1
/* handles: MFS_STM32_OSPI_HANDLE (&hospi1), MFS_STM32_SPI_HANDLE (&hspi1)... */
```

El fichero incluye `main.h`, de forma que los handles que declara CubeMX son
visibles. Define `-DMFS_STM32_USE_CONF=1` (ya lo hace el `.mk`) para habilitar
`matrixfs_stm32_mount_default()`.

### 3.4 Chip select del SPI (FRAM/EEPROM/NOR SPI)

El CS es específico de la placa, así que se declara como **símbolo débil**. Si tu
periférico no usa NSS por hardware, define en tu BSP:

```c
void mfs_stm32_spi_cs_low(void *ctx)  { HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET); }
void mfs_stm32_spi_cs_high(void *ctx) { HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);   }
```

### 3.5 Primer uso

```c
#include "main.h"
#include "matrixfs/matrixfs.h"
#include "mfs_internal.h"      /* mf_t */
#include "matrixfs_stm32.h"

static mf_t g_fs;   /* GLOBAL: son ~17 KB, no lo pongas en la pila de una tarea */

void app_main(void) {
  mfs_st st = matrixfs_stm32_mount_default(&g_fs);
  if (st != MFS_OK) {
    printf("matrixfs: %s\r\n", matrixfs_stm32_last_error());
    return;
  }
  /* ... mf_open / mf_write / mf_read ... */
  mf_sync(&g_fs);
  matrixfs_stm32_deinit(&g_fs);
}
```

---

## 4. Reservar la región en la flash interna

STM32Cube **no tiene tabla de particiones** (a diferencia de ESP-IDF): la región
del volumen hay que reservarla en el **linker script**. `linker/matrixfs_region.ld`
contiene los `ASSERT` que hacen **fallar el enlace** si la región se solapa con
código, datos o `.bss`:

```ld
INCLUDE platform/stm32cube/linker/matrixfs_region.ld
```

Ejemplo para un **STM32F407 de 1 MB** usando los tres últimos sectores de 128 KB:

| Elemento | Valor |
|---|---|
| Región | `0x080A0000`, 384 KB |
| `erase_unit` resultante | 128 KB |
| Zonas disponibles | 3 |
| Capacidad útil | `min(128, 3) × 128 KB = 384 KB` |

> El ejemplo es deliberadamente pequeño: **3 zonas es poco** para el GC del
> núcleo. Para flash interna conviene reservar muchos sectores del mismo tamaño
> (p. ej. los 8 sectores de 128 KB de un banco del H7 = 1 MB, o las 256 páginas
> de 2 KB de un L4). Cuantas más zonas, mejor se comporta la reclamación.

---

## 5. Geometría: sectores no uniformes y ventana uniforme

El núcleo exige **un único `erase_unit`** para toda la región, porque con él
dimensiona a la vez SB B, el anillo de tokens y cada zona
(`src/mfs_internal.h`). Pero muchas familias tienen sectores de tamaños
**distintos** dentro del mismo dispositivo:

| Familia | Sectores |
|---|---|
| F4 (1 MB) | 4×16 KB + 1×64 KB + 3×128 KB, por banco |
| F7 | 4×32 KB + 1×128 KB + 3×256 KB, por banco |
| F1 densidad media | 128 × 1 KB |
| L0 | 128 B por página |
| H7 | 16 × 128 KB (uniforme) |
| L4/G0/G4 | 2 KB por página |

El port lo resuelve **sin tocar el núcleo**:

1. `mfs_stm32_flash_map.c` expande la tabla de la familia (o la que aporte el
   integrador en `cfg.sectors`, que es la vía siempre correcta);
2. `mfs_stm32_flash_uniform_window()` busca el mayor tramo contiguo de sectores
   **de igual tamaño** dentro de la región reservada, y agrupa sectores pequeños
   hasta el mínimo de 1024 B que impone el núcleo (L0: 32 × 128 B = 4 KB);
3. el `erase(addr)` del backend borra **todos** los sectores que cubren
   `[addr, addr + erase_unit)`. Como el núcleo solo borra en múltiplos de
   `erase_unit`, la propiedad se mantiene.

Si la región reservada cruza un cambio de tamaño de sector, **la parte no
uniforme se descarta** y el port lo dice: no lo oculta.

`mfs_stm32_flash_map_validate()` comprueba además que la tabla cubre
`[base, base+size)` de forma contigua y ordenada. Una tabla mal escrita se
detecta al montar en vez de provocar el borrado del sector equivocado.

---

## 6. Flash interna: ECC, unidades de 8/16 B y bloqueos

La flash interna **no** se comporta como un NOR SPI. Tres diferencias que el
backend resuelve explícitamente:

### 6.1 Unidad de programación mayor que 1 byte

| Familia | Unidad | ECC |
|---|---|---|
| F0, F1, F3 | 2 B | no |
| F4, F7 | 4 B | no |
| L4, L5, G0, G4, WB, WL | 8 B | **sí** |
| H5, H7, U5 | 16 B | **sí** |

El núcleo pide escribir 32 B (token T1) y 64 B (HWV y cabecera de zona), que son
múltiplos exactos solo si la dirección está alineada. El backend **no supone
alineación**: hace *read-modify-write* sobre la unidad que contiene el tramo.

Las unidades contiguas que hay que programar se **agrupan** y se escriben con
**un solo ciclo `unlock → program×N → lock`** (`mfs_stm32_hal_flash_program_block`),
en vez de desbloquear/bloquear por unidad. El bloque se acumula en un buffer de
tamaño fijo (64 B) que se vuelca al llenarse, de modo que el coste en pila es
**constante** aunque el núcleo pida un `chunk` de hasta 4 KiB. Programar siempre
con el controlador desbloqueado es obligatorio: con `LOCK=1` la escritura se
pierde en silencio; la suite lo verifica (contador de programaciones bloqueadas
= 0).

### 6.2 ECC: no se puede reprogramar una unidad

En las familias con ECC, programar dos veces la misma *double/quad word* produce
`PROGERR`, y dejar una unidad a medio programar produce **error de ECC al
leerla** (`ECCC`/`ECCC2` en H7, `DBECCERR`/`SNECCERR` en U5). Un RMW ciego (leer
la ventana y reescribirla entera) **corrompería** el medio.

Algoritmo del backend, por unidad:

```
leer la unidad completa (el dato llega ya corregido por la ECC del dispositivo)
si el contenido pedido ya está contenido en el actual  → no-op (NO se programa)
si la unidad está toda a 0xFF                          → fusionar y programar
en otro caso                                            → MFS_EIO
```

El tercer caso no debería ocurrir con el diseño *log-structured* del núcleo (que
borra la zona antes de programar), pero el backend **lo detecta y lo reporta**
en lugar de confiar en ello. Declarar la ECC
(`flash->flags0_extra |= MFS_HWV0_ECC_ON_DIE`) tiene un efecto real: el núcleo
selecciona la clase ECC más tolerante (`mfs_eba_select` en `src/ftl/mfs_ftl2.c`).

### 6.3 *Read-while-write*: la CPU se detiene

En dispositivos de **un solo banco** programar o borrar la flash **detiene la
búsqueda de instrucciones** durante toda la operación. Un borrado de un sector
grande puede ser de **cientos de milisegundos** (el F4 declara hasta ~1 s para un
sector de 128 KB). Consecuencias:

- el *garbage collector* del núcleo provoca **jitter de decenas o cientos de
  milisegundos** en el resto del sistema;
- no es un defecto del port: es una propiedad del silicio y ninguna capa de
  software puede evitarla;
- si tu aplicación tiene plazos estrictos, usa **NOR externa OSPI/QSPI** o
  **SD/eMMC**, que no bloquean la CPU.

`mfs_stm32_hal_flash_single_bank()` expone esta condición para que la aplicación
pueda decidir (p. ej. posponer operaciones pesadas).

> **Ojo con la familia F4/F7:** NO son siempre de doble banco. Solo hay RWW en
> los dispositivos de **dos bancos** (F42x/F43x en F4; F76x/F77x en F7). El resto
> —F401/F405/F407/F411/**F446**/F72x/F73x…— es de **un solo banco** y la CPU se
> detiene igual que en un F0/F1. El port decide esto por el macro del
> dispositivo: `mfs_stm32_hal_flash_single_bank()` devuelve `false` **solo** si
> el dispositivo define `FLASH_BANK_2`. (Antes lo daba por `false` en todo F4/F7,
> lo que describía RWW en dispositivos que no lo tienen.)

### 6.4 Tiempos

Los presupuestos `t_erase_max_us` por defecto son **conservadores** (1 s) porque
alimentan el modelo de energía/EDP, no un *timeout* de sondeo (el HAL bloquea
hasta terminar). Ajusta `cfg.t_erase_max_us` / `t_prog_max_us` con el dato del
datasheet de tu familia para que las estimaciones sean realistas.

---

## 7. Concurrencia

El núcleo mantiene estado global (mapa L2P, pools, ventana WAL y de inodos) y
**no es reentrante**. Dos tareas llamando a `mf_write` a la vez **corrompen el
volumen**.

El port ofrece dos mecanismos:

```c
/* 1. Registra TU mutex (FreeRTOS/CMSIS-RTOS): el port lo usa en montaje/formateo. */
static void mi_lock(void)   { osMutexAcquire(g_fs_mutex, osWaitForever); }
static void mi_unlock(void) { osMutexRelease(g_fs_mutex); }
matrixfs_stm32_set_lock_hooks(mi_lock, mi_unlock);

/* 2. Agrupa operaciones compuestas sobre el volumen. */
matrixfs_stm32_lock();
mf_stat(&g_fs, "/a", &st);
mf_open(&g_fs, "/a", MFS_O_RDONLY, &f);
matrixfs_stm32_unlock();
```

Sin *hooks* registrados, `matrixfs_stm32_lock()/unlock()` son no-op y la
exclusión mutua es responsabilidad de la aplicación. **Regla práctica:** si el
volumen se usa desde una sola tarea, no hace falta nada; si se usa desde varias,
registra los hooks y envuelve **todo** acceso directo al núcleo.

---

## 8. Puerto §20.2: FreeRTOS, CMSIS-RTOS v2 y bare-metal

`src/mfs_stm32_port.c` selecciona el adaptador en tiempo de compilación:

| Contexto | Detección | Sección crítica | Ciclos | Tiempo |
|---|---|---|---|---|
| **FreeRTOS** | `configUSE_PREEMPTION` / `USE_FREERTOS` / `CMSIS_V1` | `taskENTER/EXIT_CRITICAL` | `DWT->CYCCNT` | tick × `configTICK_RATE_HZ` |
| **CMSIS-RTOS v2** | `CMSIS_V2` | `osKernelLock/Unlock` | `DWT->CYCCNT` | `osKernelGetTickCount/Freq` |
| **Bare-metal** | por defecto | `PRIMASK` con anidamiento LIFO | `DWT->CYCCNT` | `HAL_GetTick() × 1000` |

Detalles que importan:

- **DWT** no existe en Cortex-M0/M0+ (STM32F0, L0, G0, C0). El puerto lo detecta
  con `__CORTEX_M` y cae al tick (`HAL_GetTick()`), que envuelve a los ~49,7 días.
- La sección crítica **no es ISR-safe**: `mfs_read` la usa en contexto de tarea.
  No llames a la API del núcleo desde una ISR.
- `mfs_port_wfi` **cede** (`taskYIELD`/`osDelay(0)`) o **duerme** (`__WFI`), nunca
  gira en vacío: se invoca durante esperas de *write-in-progress*.
- El contrato §20.3 pide latencia de sección crítica ≤ 1 µs; `PRIMASK` y
  `taskENTER_CRITICAL` la cumplen en Cortex-M.

---

## 9. API pública

```c
mfs_st matrixfs_stm32_mount(mf_t *fs, const mfs_stm32_cfg *cfg);
mfs_st matrixfs_stm32_format(mf_t *fs, const mfs_stm32_cfg *cfg);
mfs_st matrixfs_stm32_mount_default(mf_t *fs);      /* usa mfs_stm32_conf.h */
mfs_st matrixfs_stm32_deinit(mf_t *fs);

const char *matrixfs_stm32_last_error(void);
uint32_t    matrixfs_stm32_capacity_bytes(void);
bool        matrixfs_stm32_capacity_wasteful(void);
const mfs_hwv_t *matrixfs_stm32_hwv(void);
const char *matrixfs_stm32_media_name(mfs_stm32_media_t m);

void matrixfs_stm32_set_lock_hooks(mfs_stm32_lock_fn lock, mfs_stm32_lock_fn unlock);
void matrixfs_stm32_lock(void);
void matrixfs_stm32_unlock(void);
```

Los handles del HAL se pasan como `void *` a propósito: la cabecera pública no
arrastra las cabeceras de la HAL, que cambian por familia y por versión de
CubeMX. Los tipos esperados son `OSPI_HandleTypeDef *`, `QSPI_HandleTypeDef *`,
`SPI_HandleTypeDef *`, `I2C_HandleTypeDef *` y `SD_HandleTypeDef *`.
Un handle `NULL` devuelve `MFS_EINVAL`.

---

## 10. Estado de verificación

**Verificado en host** (sin silicio, contra un modelo fiel de la flash):
compilación y ejecución del port completo con el núcleo real; formateo, montaje,
escritura y relectura sobre flash interna simulada con **sectores no uniformes**;
el RMW por unidad con **4, 8 y 16 bytes** y con **ECC activada y desactivada**;
el rechazo tipificado cuando habría que reprogramar una unidad ya escrita; la
expansión y validación del mapa de sectores; y la búsqueda de ventana uniforme.
Ver `test/` y el apartado «Verificación sin los SDK» del informe
[`../compatibility-analysis.md`](../compatibility-analysis.md).

**NO verificado (requiere toolchain de ST y hardware):**

- Compilación contra el HAL real de cada familia. Las firmas de
  `HAL_FLASH_Program`, los campos de `FLASH_EraseInitTypeDef` y la selección de
  banco **varían** entre familias y versiones de CubeMX: por eso todo se aísla en
  `src/mfs_stm32_hal.c` y `src/mfs_stm32_hal.h`. Si tu versión del HAL difiere,
  es el único fichero que hay que ajustar.
- Las tablas de sectores de `mfs_stm32_flash_map.c` reproducen la documentación
  de ST, pero **deben contrastarse con el Reference Manual de tu variante**. La
  vía siempre correcta es aportar tu propia tabla en `cfg.sectors`; el
  `ASSERT` del linker y `mfs_stm32_flash_map_validate()` convierten un error de
  tabla en un fallo detectable.
- Tiempos reales de borrado, comportamiento real de la ECC y penalización RWW.
- Coherencia de caché del Cortex-M7 con el DMA del SDMMC en H7.
- Las rutas OSPI/QUADSPI, SPI, I²C y SD no se han compilado contra el HAL real ni
  ejecutado sobre hardware; su lógica de troceado y de barrera WOB sí está
  revisada y comparte el driver ya verificado en host
  (`platform/common/mfs_l2_8bit.c`).

**Limitaciones conocidas del port** (no del núcleo):

- Instancia única y no reentrante (§7).
- El tope de capacidad es `MFS_ZONE_MAX × erase_unit` (§2).
- La región reservada debe ser un tramo de sectores del **mismo tamaño**; el
  resto se descarta y se reporta (§5).

---

## 11. Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| El enlace falla por `.bss`/RAM | El núcleo consume ~198 KB de `.bss` + 17 KB de `mf_t` | Baja `MFS_L2P_SLOTS` y `MFS_ZONE_MAX`, usa `MFS_ALLOW_8BIT_TARGET`, o elige un MCU con más RAM (§2) |
| *Hard fault* o corrupción al escribir | Pila de tarea demasiado pequeña | ≥ 16 KB para el perfil de 32 bits (§2) |
| `matrixfs_stm32_mount` devuelve `MFS_EINVAL` | La región no valida o el mapa de sectores no cubre la flash | Revisa `linker/matrixfs_region.ld` y `cfg.sectors` (§4, §5) |
| `MFS_ENOTSUP` al preparar el medio | Familia sin tabla de sectores, o `erase_unit` sin comando estándar | Aporta `cfg.sectors`; usa 4/32/64 KB (§5) |
| `MFS_EIO` en una escritura sobre flash interna | Se intentó reprogramar una unidad ya escrita (ECC) | El núcleo debe borrar la zona antes de programar; con 3 zonas el GC no tiene margen (§4, §6.2) |
| El sistema pierde plazos temporalmente | Borrado de flash interna en familia de un solo banco (RWW) | Usa OSPI/QSPI o SD/eMMC (§6.3) |
| `MFS_ENOTVIABLE` al montar | `MFS_STM32_RAM_BUDGET` demasiado bajo para el modo mínimo | Súbelo, sabiendo que **no** reserva memoria (§2) |
| Escrituras I²C que se pierden | Falta el *acknowledge polling* de la EEPROM | Ya lo implementa el port; comprueba que `cfg.bus = MFS_STM32_BUS_I2C` y que el tipo es EEPROM, no FRAM |

---

## Ficheros del port

| Fichero | Contenido |
|---|---|
| `include/matrixfs_stm32.h` | API pública y `mfs_stm32_cfg` |
| `include/mfs_stm32_conf_template.h` | Plantilla de configuración compilada |
| `include/mfs_stm32_flash_map.h` | Tipos y API del mapa de sectores |
| `src/matrixfs_stm32.c` | API, despacho por medio, capacidad y diagnóstico |
| `src/mfs_stm32_backend.h` | Contrato interno port ⇄ backends |
| `src/mfs_stm32_backend.c` | Cierre del backend activo |
| `src/mfs_stm32_port.c` | Primitivas §20.2 (FreeRTOS / CMSIS-RTOS v2 / bare-metal) |
| `src/mfs_stm32_hal.h` / `.c` | **Único** punto de dependencia del HAL y de la familia |
| `src/mfs_stm32_flash_map.c` | Tablas por familia, validación y ventana uniforme |
| `src/mfs_stm32_iflash.c` | Flash interna: RMW con ECC y borrado por tramo |
| `src/mfs_stm32_ospi.c` | NOR externa OSPI/QUADSPI |
| `src/mfs_stm32_mem.c` | NOR SPI, FRAM y EEPROM (SPI/I²C) |
| `src/mfs_stm32_sd.c` | SD/eMMC sobre `mfs_l2_managed` |
| `linker/matrixfs_region.ld` | Reserva de la región con `ASSERT` de no solapamiento |
| `matrixfs_stm32.mk` | Integración con el toolchain Makefile de CubeMX |
| `CMakeLists.txt` | Integración con CMake / STM32CubeCLT / CI |
| `examples/main_matrixfs_example.c` | Fragmento de uso para el `main.c` de CubeMX |
| `test/` | Shims del HAL y tests de host |
