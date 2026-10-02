# Informe de optimización y rendimiento — plataformas STM32Cube y ESP-IDF

Revisión completa y endurecimiento de los ports `platform/stm32cube` y
`platform/esp-idf`, más el motor de detección y autoajuste del núcleo, alineado
con los cuatro objetivos técnicos solicitados (RAM, rendimiento, detección por
microprocesador y autoajuste). Todas las cifras y afirmaciones de este documento
proceden del código de este repositorio y de las suites que se citan; no hay
mediciones extrapoladas.

---

## 1. Método

1. **Auditoría** del código real de ambos ports y del núcleo de arquitectura,
   con hallazgos numerados (`STM-nn`, `IDF-nn`).
2. **Corrección** de cada hallazgo en el código fuente.
3. **Verificación en host** con las suites del repositorio (compilan el código
   *real* de los ports contra el núcleo *real*, con shims mínimos del SDK).
4. **Sincronización** de la documentación afectada (`README` de plataforma,
   `DOCS/hal.md`).

Sin heap y sin cambio de determinismo: todas las decisiones de autoajuste se
toman **una sola vez** (arranque/montaje) y no se reajustan en runtime (P1/P12).

---

## 2. Resumen ejecutivo

| Bloque | Hallazgos corregidos | Efecto principal |
|---|---|---|
| STM32Cube | STM-01…13 | RAM de pila, exactitud de dirección, E/S por lotes, timeouts reales |
| ESP-IDF | IDF-01,03,05,06,07,09,10 | carrera VFS, RMW eliminado en el caso común, ciclos entre núcleos, ranuras de error |
| Núcleo (Fase 3) | detección DSP/Helium + núcleos y motor determinista | autoajuste reproducible en el arranque |

**Evidencia de regresión (0 fallos):**

| Suite | Resultado |
|---|---|
| Principal del proyecto (`make test`) | **1232 checks · 0 fallos** |
| STM32Cube host · 2 B sin ECC (F0/F1) | 258 checks · 0 fallos |
| STM32Cube host · 4 B sin ECC (F4/F7) | 260 checks · 0 fallos |
| STM32Cube host · 8 B con ECC (L4/G4/G0) | 264 checks · 0 fallos |
| STM32Cube host · 16 B con ECC (H5/H7/U5) | 280 checks · 0 fallos |
| ESP-IDF host v5.5 / v6.1 (funcional) | 77 checks · 0 fallos (cada perfil) |
| ESP-IDF host v5.5 / v6.1 (contrato de versión) | 14 y 17 checks · 0 fallos |

---

## 3. STM32Cube

### 3.1 Corrección (bugs)

| ID | Descripción | Corrección |
|---|---|---|
| **STM-03** | El direccionamiento SPI NOR se decidía con el umbral `>16 MiB ? 3 : 2` bytes, de modo que **todo** dispositivo de 64 KiB–16 MiB (lo habitual) se direccionaba con 2 bytes y la dirección se **truncaba**. | Umbral correcto `≤64 KiB ? 2 : 3` bytes; en `mfs_l2_8bit.c` se calcula **después** de la autodetección JEDEC (antes se hacía con `total_size = 0`) y se rechaza `MFS_ENOTSUP` por encima de 16 MiB en vez de truncar. |
| **(nuevo)** | Puntero colgante: el HAL conservaba el puntero de la tabla de sectores, y en `prepare` esa tabla era una variable de **pila**. | La tabla vive en respaldo estático (`s_if_map_backing`) y el HAL recibe siempre un puntero persistente. |
| **STM-05** | Fase de dirección QUADSPI de 4 bytes incorrecta (`QSPI_ADDRESS_4_LINES` en lugar de 32 bits). | `AddressMode = QSPI_ADDRESS_1_LINE` + `AddressSize = QSPI_ADDRESS_32_BITS`. |
| **STM-06** | Timeout OSPI por **contador de intentos**, no por tiempo real. | Espera con `mfs_port_time_us()` (50 ms programación, 500 ms borrado). |
| **STM-04 (bug de revisión)** | El lote de 16 B programaba **una sola** quadword e ignoraba `len`; y `block_buf[64]` se desbordaba con `chunk_size` hasta 4 KiB. | El bloque recorre **todas** las quadwords dentro de un único ciclo unlock/lock; el buffer se vuelca al llenarse (coste de pila constante). |
| **STM-13 (bug de revisión)** | `mfs_port_time_us()` dependía de `SysTick`; el shim de host no lo exponía (fallo de compilación). | Shim ampliado con `SysTick`/`SystemCoreClock`; resolución real en µs en destino. |

### 3.2 RAM y rendimiento

| ID | Cambio | Efecto |
|---|---|---|
| **STM-01** | `iflash_state_t` ya **no** incrusta el mapa `sectors[2048]` y `prepare` ya **no** reserva `expanded[2048]` en pila; guarda el rango `[win_first, win_count)` sobre el mapa ordenado. | El estado pasa de ~16 KB a ~40 B; la pila de `prepare` deja de tener una ráfaga de 16 KB. Queda un respaldo estático de 16 KB sólo para el caso de tabla por familia (si el integrador aporta su mapa, no se usa). |
| **STM-02** | Índice de sector por **búsqueda binaria** (antes lineal). | O(log n) en vez de O(n) por borrado, con tablas no uniformes (F4: 12 sectores; L0: 1536 páginas). |
| **STM-04** | Lote `unlock → program×N → lock` por bloque contiguo. | Un ciclo de desbloqueo por lote (hasta 4 quadwords de 16 B / 32 unidades de 2 B) en lugar de uno por unidad. |
| **STM-10** | Sección crítica *bare-metal* anidable con pila LIFO (`MAX_NEST = 8`) y restauración de `PRIMASK`. | Sin corrupción del estado de interrupciones con anidamiento. |
| **STM-11** | *Acknowledge polling* de EEPROM I²C cede CPU con `mfs_port_wfi()`. | No gira en vacío durante la escritura. |
| **STM-13** | `mfs_port_time_us()` con resolución sub-ms (`DWT->CYCCNT` o `SysTick->VAL`). | Presupuestos temporales precisos en lugar de granularidad de 1 ms. |

Un efecto colateral verificado: el port programa **siempre** con el controlador
desbloqueado (el lote lo garantiza). El modelo de la suite cuenta cualquier
programación con `LOCK=1`; el test exige ahora **0** (antes documentaba el
defecto como hallazgo).

---

## 4. ESP-IDF

| ID | Hallazgo | Corrección |
|---|---|---|
| **IDF-01** | Los límites de RAM del núcleo no eran sobrescribibles. | `MFS_MAX_FILES_OPEN`, `MFS_WAL_WINDOW_MAX` y `MFS_SCRATCH_MAX` quedan tras `#ifndef` y se exponen por `Kconfig` + `-D` desde `CMakeLists.txt`. |
| **IDF-03** | El *read-modify-write* se aplicaba **siempre** en la programación. | *Fast-path*: si el tramo está alineado a la granularidad, se escribe directo, sin lectura extra. |
| **IDF-05** | **Carrera**: el descriptor se validaba **fuera** del mutex; otra tarea podía cerrarlo y reutilizar la ranura (se operaba sobre el fichero equivocado). | La validación de descriptores y de directorios se hace **dentro** del mutex en `close/read/write/lseek/fstat/readdir/closedir`. |
| **IDF-07** | El help de `Kconfig` afirmaba que el backend trocea por `page_size`; no lo hace. | Documentado como **declarativo** (`esp_partition_write` resuelve los cruces de página). |
| **IDF-09** | `mfs_port_cycles()` leía el `CCOUNT` **por núcleo** en chips duales (escalas incomparables). | En multi-núcleo se deriva una base común de `esp_timer_get_time()` × frecuencia de CPU; en un núcleo se usa el contador de hardware. |
| **IDF-10** | Las ranuras de error por tarea **nunca** se liberaban al morir una tarea. | Desalojo **LRU** determinista de la ranura usada menos recientemente. |
| **IDF-06** | Deriva entre documentación y API del componente. | Corregida en la cabecera del VFS. |

---

## 5. Núcleo — detección por microprocesador y autoajuste determinista (Fase 3)

### 5.1 Detección de capacidades

`mfs_arch_info_t` expone ahora, además de los aceleradores ya existentes:

- `hwaccel` con los bits nuevos **`MFS_HWACCEL_DSP`** (`__ARM_FEATURE_DSP` /
  `__ARM_FEATURE_SIMD32`: Cortex-M4/M7/M33) y **`MFS_HWACCEL_MVE`**
  (`__ARM_FEATURE_MVE`: Helium de Cortex-M55/M85; también marca `SIMD`).
- `cores`: número de núcleos, tomado de `MFS_ARCH_CORES` (override del
  integrador) o de macros de plataforma (`CONFIG_MP_MAX_NUM_CPUS`,
  `CONFIG_NUM_CORES`, `configNUM_CORES`, `portNUM_PROCESSORS`); por defecto 1.

La decisión es de **compilación** (macros del compilador), no de sondeo en
runtime en MCU, lo que mantiene el determinismo.

### 5.2 Motor de autoajuste determinista

`mfs_arch_adapt_config(info, cfg)` es ahora un motor **idempotente y puro**
respecto de `info`, conectado en `mfs_embedded_bind()` (arranque/montaje, una
sola vez):

1. resuelve `arch_class` sólo si vale `MFS_ARCH_AUTO`;
2. rellena `ram_total` sólo si es 0 y el objetivo es de 8 bits;
3. deriva `bus_speed_hz` sólo si es 0 y el objetivo es de 8 bits.

Regla invariante: **nunca** sobreescribe un valor declarado explícitamente y
**no** hay reajustes posteriores en runtime. Idéntica entrada ⇒ idéntica
configuración.

---

## 6. Cumplimiento de los objetivos solicitados

| Objetivo | Cómo se aborda |
|---|---|
| **1. Minimizar RAM** | Eliminación de la ráfaga de 16 KB en la pila del backend de flash interna y del mapa embebido en el estado (STM-01); límites de RAM del núcleo configurables en ESP-IDF (IDF-01). El núcleo sigue **sin heap**. |
| **2. Rendimiento ultra alto** | Lote unlock/program/lock (STM-04), *fast-path* sin RMW (IDF-03), búsqueda binaria de sector (STM-02), timeouts reales sin busy-wait (STM-06/STM-11/STM-13). |
| **3. Detección por microprocesador** | CRC-32C/AES/SHA/CLMUL/SIMD (ya existente) + **DSP**, **MVE/Helium** y **núcleos** (Fase 3). |
| **4. Autoajuste/autoconfiguración** | Motor determinista en el arranque (Fase 3), sin reajuste en runtime. |

---

## 7. Cómo reproducir

```powershell
# Suite principal del proyecto
mingw32-make test

# Ports en host (compilan el código REAL contra el núcleo REAL)
pwsh -File platform/esp-idf/test/run_esp_idf_host_tests.ps1
# STM32Cube: 4 variantes (2/4/8/16 B ± ECC) — ver Makefile, objetivo platform-test
```

---

## 8. Límites de este informe

- No hay SDK de ST ni ESP-IDF instalados: la verificación es **en host** con
  shims que reproducen las restricciones que importan (unidad de programación,
  ECC, sectores no uniformes, alineación de cifrado, semántica `old & new`). No
  se emulan tiempos de silicio ni concurrencia real entre tareas.
- Las cifras de RAM del **núcleo** (apartado 2 del `README` de STM32Cube) no
  cambian con este trabajo: las optimizaciones de RAM afectan al **port**, no al
  `.bss` del núcleo.
- DSP/Helium y núcleos se **detectan y exponen** para diagnóstico y
  planificación; la única ruta acelerada explotada hoy es CRC-32C (idéntica bit
  a bit a la tabla software).

---

## 9. Validación con SDKs reales (ESP-IDF v5.5 y STM32CubeF4)

Se instalaron los SDK **reales** (dentro de `.tools/`, sin tocar el sistema) y se
compiló el código del repositorio contra ellos. Esta pasada encontró defectos que
las suites de *host* (con shims) **no podían ver**, porque los shims declaraban
símbolos en el sitio equivocado o no existían en el SDK real.

### 9.1 Entorno instalado

| SDK | Versión | Toolchain |
|---|---|---|
| ESP-IDF | v5.5 (clon + `install.ps1`, `IDF_TOOLS_PATH` local) | `xtensa-esp32-elf-gcc` 14.2.0 (esp32) |
| STM32CubeF4 | v1.28.0 (HAL `stm32f4xx_hal_driver` + CMSIS device/core) | `arm-none-eabi-gcc` 15.2.1 (xpack) |
| STM32CubeU5 | v1.9.0 (HAL `stm32u5xx_hal_driver` + CMSIS device/core) | `arm-none-eabi-gcc` 15.2.1 (Cortex-M33, `-mfpu=fpv5-sp-d16`) |

### 9.2 Defectos encontrados y corregidos — ESP-IDF

| # | Defecto | Corrección |
|---|---|---|
| **IDF-A1** | `file(GLOB … CONFIGURE_DEPENDS)` es **error en modo script**; ESP-IDF parsea el CMakeLists del componente así (`component_get_requirements.cmake`), de modo que **el build abortaba**: *"CONFIGURE_DEPENDS is invalid for script and find package modes"*. | Quitado `CONFIGURE_DEPENDS` del glob. |
| **IDF-A2** | `PRIV_REQUIRES esp_flash` referencia un componente **inexistente** en IDF v5.5 (la API `esp_flash_*` vive en `spi_flash`). | `spi_flash` (+ `bootloader_support`, ver IDF-A3). |
| **IDF-A3** | `esp_flash_encryption_enabled()` se usaba incluyendo `esp_flash.h`, pero en IDF real se declara en **`esp_flash_encrypt.h`** (componente `bootloader_support`): *implicit declaration*. El shim de host lo declaraba en `esp_flash.h`, ocultando el fallo. | `#include "esp_flash_encrypt.h"`. |
| **IDF-A4** | El callback estático `esp_flash_read()` **colisiona** con la función pública de IDF `esp_err_t esp_flash_read(esp_flash_t*, …)` (conflicting types). | Renombrados a `esp_mfs_read` / `esp_mfs_prog` / `esp_mfs_erase`. |
| **IDF-A5** (núcleo) | `crc32c_x86_supported()` se compilaba en ARM, donde no se usa → `-Wunused-function`. | Definición guardada con `#if MFS_HWCRC_X86`. |

**Resultado:** el componente y el núcleo compilan para `esp32` con el toolchain
real (4/4 ficheros de componente+`main`, **15/15** del núcleo), y la suite de
host sigue en 77/77 en v5.5 y v6.1.

### 9.3 Defectos encontrados y corregidos — STM32Cube

| # | Defecto | Corrección |
|---|---|---|
| **STM-A1** | `mfs_stm32_hal.h` usa `mfs_sector_t` **sin incluir** `mfs_sectors.h`; en host lo aportaba el shim. En destino, los TU que incluyen esa cabecera primero (`mfs_stm32_hal.c`, `mfs_stm32_port.c`) **no compilaban**. | `#include "mfs_sectors.h"`. |
| **STM-A2** | El borrado usaba `FLASH_BANK_2` sin guarda: **no compila** en dispositivos F4 de **un solo banco** (F401/F405/F407/F411/**F446**). | Guardado con `#if defined(FLASH_BANK_2)`; si no, `FLASH_BANK_1`. |
| **STM-A3** | `mfs_stm32_hal_flash_single_bank()` devolvía `false` (RWW) para **todo** F4/F7, cuando la mayoría son de un solo banco. | Devuelve `false` **solo** si el dispositivo define `FLASH_BANK_2`. |
| **STM-A4** | El `.mk` (vía CubeMX/Makefile) **no permitía excluir backends**, así que un dispositivo sin QUADSPI (p. ej. F407) no compilaba `mfs_stm32_ospi.c`. | Añadidas `MATRIXFS_NO_OSPI` / `MATRIXFS_NO_IFLASH` al `.mk` (paridad con el CMakeLists). Documentado en el README. |
| **STM-A5** | `mfs_stm32_hal.c` usaba `FLASH_TYPEPROGRAM_HALFWORD/WORD/DOUBLEWORD` sin guarda; **STM32U5 solo define `QUADWORD`** → no compilaba. | Casos del `switch` guardados con `#if defined(FLASH_TYPEPROGRAM_*)`. |
| **STM-A6** | El borrado clasificaba **H5/U5 como "sectores"**, pero U5 usa **PÁGINAS de 8 KB** → `'FLASH_TYPEERASE_SECTORS' undeclared` / sin miembro `Sector`. | H5/U5 sacados a una rama propia que elige PAGINAS/SECTORES **por la macro que define el HAL** (no por una lista a mantener). |

**Resultado:** `STM32F446` (con QUADSPI) → **28/28** ficheros; `STM32F407`
(sin QUADSPI, con `MATRIXFS_NO_OSPI`) → **27/27**; **`STM32U575` (Cortex-M33,
OCTOSPI) → 28/28**, con el toolchain real `arm-none-eabi-gcc` y el HAL/CMSIS de
STM32CubeU5 v1.9.0. En todas las variantes la suite de host de STM32 sigue en
258/260/264/280 (0 fallos) y la suite principal en 1232 (0 fallos).

### 9.4 Límite de esta pasada

El `idf.py build` completo (ninja) quedó **bloqueado en este entorno** en un paso
posterior a la configuración (ninja ocioso, 0 objetos), probablemente por una
restricción del *sandbox* sobre un subproceso. La validación de compilación se
hizo con el **mismo toolchain y las mismas flags reales** que IDF genera
(`compile_commands.json`), ejecutando las órdenes de compilación de forma
directa; no se llegó a **enlazar** el `.elf`. En STM32 sí se compiló cada objeto
con el HAL/CMSIS reales (compilación, no enlace con script de arranque).
