# Compatibilidad con ESP-IDF y STM32Cube — análisis profundo y plan de implementación

Documento de trabajo de ingeniería. Cubre la **auditoría del port ESP-IDF**
existente, la **auditoría de la capa compartida** que ambos ports necesitan y
el **diseño completo del port STM32Cube**, con hallazgos verificados sobre el
código del repositorio (referencias `fichero:línea`), criterios de aceptación y
el plan de verificación.

Spec relacionada: §20.2/§20.3 (contrato de puerto), §20.4 (serialización),
§21 (API), §22 (layout on-flash), §5.1–§5.2 (detección y HWV), MFS-CAP-001
(perfiles de medio), MFS-HW-001 (fallback software).

---

## 0. Resumen ejecutivo

| # | Hallazgo | Severidad | Estado |
|---|---|---|---|
| **E1** | El componente ESP-IDF **no puede enlazar**: `CMakeLists.txt` omite `src/core/mfs_arch.c`, que define dos símbolos que el núcleo invoca en cada montaje y en cada CRC. | **Bloqueante** | Corregido |
| **E2** | `REQUIRES` mal usado: las dependencias de IDF son privadas (la cabecera pública no expone tipos de IDF). | Media | Corregido |
| **E3** | Las fuentes se referencian como `../../src/...`. **Corrección al análisis inicial:** IDF **no** emite error ni aviso por fuentes fuera del directorio del componente (verificado en `component.cmake` de v5.3 y master). El riesgo real es otro: el componente deja de funcionar si se reubica o se instala desde el registro. | Media | Corregido y reencuadrado |
| **E4** | Sin serialización: el núcleo **no es reentrante** y una app IDF es multitarea por defecto. | Alta | Corregido |
| **E5** | Sin integración POSIX VFS: no hay `fopen`/`stat`/`opendir` sobre el volumen. | Alta | Implementado |
| **E6** | Particiones con cifrado exigen alineación de 16 B; el RMW alinea a 4 B. **Corrección al análisis inicial: es LATENTE, no un fallo vivo** — medido, las 96 programaciones de una carga representativa son múltiplos de 16 B en dirección y longitud, de modo que la ventana a 4 B ya sale alineada a 16 B. Se corrige igualmente por robustez futura. La capa compartida **no podía ni expresar** granularidad > 1, y eso sí se ha resuelto. | Media (latente) | Corregido |
| **E11** | **`esp_vfs_t` es una unión de dos juegos de punteros y el flag decide cuál se usa.** Asignar los callbacks `*_p` con `ESP_VFS_FLAG_DEFAULT` (lo que hacía la primera versión de `matrixfs_esp_vfs.c`) hace que IDF llame por el juego sin contexto, con firma distinta ⇒ comportamiento indefinido. Detectado al verificar la API. | **Alta** | Corregido |
| **E7** | `mfs_port_cycles` (`esp_cpu_get_cycle_count`) es **core-local** en partes duales y se detiene en light sleep. | Baja | Documentado |
| **E8** | `portENTER_CRITICAL` es de contexto de tarea: el componente no es ISR-safe y no lo declaraba. | Media | Documentado |
| **E9** | Kconfig incompleto y sin validación de capacidad mínima real (reserva de 3 sectores + tope de zonas). | Media | Corregido |
| **E10** | Sin ejemplo, sin `partitions.csv`, sin CI ni QEMU: estado «no verificado» estructural. | Alta | Implementado |
| **S1** | `mfs_embedded_flash_t` no tiene `program_granularity`: `mfs_embedded_setup()` lo fija a `1u`. | **Bloqueante para STM32** | Corregido |
| **S2** | `mfs_embedded` asume `erase_unit` uniforme; STM32 tiene sectores no uniformes. | Alta | Resuelto por convenio + helper |
| **S3** | `mfs_l2_8bit.c:l2_prog` **no trocea en la frontera de página** del NOR ⇒ corrupción silenciosa en escrituras > `page_size`. | **Crítica** | Corregido |
| **S4** | `mfs_l2_8bit_create()` exige callbacks SPI incondicionalmente ⇒ las rutas **I²C** (FRAM/EEPROM) son inalcanzables. | Alta | Corregido |
| **S5** | `mfs_l2_8bit_get_geom()`: ternario redundante, `program_granularity` siempre 1, `zone_size` puede quedar 0 en FRAM. | Media | Corregido |
| **S6** | `(uint16_t)` en las transferencias de bus ⇒ truncamiento silencioso por encima de 64 KiB. | Media | Corregido |
| **T1** | No existe ningún port de STM32Cube. | — | Implementado |
| **T2** | Flash interna STM32: unidad de programación de 8/16 B + ECC ⇒ RMW obligatorio y **no se puede reprogramar** una quad-word ya programada. | Alta (diseño) | Resuelto por algoritmo |
| **T3** | STM32Cube no tiene tabla de particiones: la región debe reservarse en el linker. | Alta (diseño) | Resuelto con `ASSERT` |
| **R1** | El consumo de RAM del núcleo **no está documentado en ninguna parte** y es de **~198 KB de `.bss` + 17,2 KB de `mf_t`** en el build por defecto: no cabe en la mayoría de STM32. | **Alta** | Medido y documentado |

**Conclusión de la auditoría.** El port ESP-IDF tenía un fallo de enlace que lo
hacía inutilizable pese a que su documentación lo describía como «pendiente de
compilar»; no era un problema de verificación, era un defecto de construcción.
El port STM32Cube no existía y su mayor dificultad no es el HAL sino la
**semántica de la flash interna** (ECC + unidades de 8/16 B + sectores no
uniformes + *read-while-write*), que el modelo de medio de MatrixFS no podía
expresar. Ambas cosas se han resuelto y se verifican en host con *shims* de SDK.

---

## 1. Metodología y evidencia

El análisis se hizo en tres pasadas:

1. **Lectura del contrato real, no de la documentación.** Se reconstruyó qué
   necesita un port leyendo el núcleo (`src/core/*.c`), no los README. Los
   README describen intenciones; el contrato lo fijan las llamadas reales:
   - `src/core/mfs_hal.c:62` → `mfs_arch_detect()`
   - `src/core/mfs_util.c:143` → `mfs_arch_crc32c_hw_available()`
   - `src/core/mfs_zone.c:191-218` → `drv->read/prog/erase` y su **uso dentro de
     sección crítica** (`mfs_read` envuelve la lectura en
     `mfs_port_crit_enter/exit`; `mfs_write` **no**).
   - `src/core/mfs_fs.c:120`, `mfs_zone.c:249`, `mfs_zone.c:487`,
     `mfs_wal.c:175` → **tamaños y alineaciones reales de las programaciones**.
2. **Reconstrucción del layout on-flash** desde `src/mfs_internal.h:14-36`:
   ```
   offset 0                 SB A (@0)  + HWV (@512)
   offset 1·erase_unit      SB B
   offset 2·erase_unit      anillo de tokens T1 (8 slots × 32 B)
   offset 3·erase_unit      zona 0 … zona N   (zone_size = erase_unit)
   ```
   Consecuencia directa: **`erase_unit` debe ser uniforme** en toda la región,
   porque dimensiona simultáneamente SB B, el anillo y cada zona.
3. **Verificación por construcción.** Se comprobó cada hallazgo con búsquedas
   dirigidas (p. ej. `grep mfs_port_arch_init` demuestra que el núcleo **nunca**
   llama al puerto de arquitectura, lo que hace correcta su omisión del build de
   IDF). Los hallazgos E1, S1, S3, S4 y S5 se confirmaron leyendo el código
   exacto que los produce.

Los hechos de API de terceros (ESP-IDF v5.x, HAL de STM32) se contrastaron con
documentación oficial; las afirmaciones que no se han podido confirmar contra
silicio o SDK real se marcan explícitamente en la §8.

---

## 2. El contrato que debe cumplir cualquier port

Resumen operativo (lo que un port nuevo tiene que aportar, sin adorno):

| Obligación | Dónde se exige | Consecuencia de incumplir |
|---|---|---|
| 5 primitivas `mfs_port_*` | `include/matrixfs/mfs_port.h:17-21` | Error de enlace |
| `read` / `prog` / `erase` con semántica NOR | `include/matrixfs/mfs_port.h:25-28` | Corrupción |
| `prog` retorna **tras** verificar estado (barrera WOB) | `mfs_port.h:26`, `mfs_zone.c:205` | Pérdida de datos en corte de energía |
| Geometría coherente con el medio | `mfs_port.h:43-58` | Zonas mal dimensionadas |
| Una sola definición de `mfs_port_*` por imagen | `mfs_port_rtos.c:23-25` | Error de enlace (MFS-PORT-001) |
| **Sin heap** en el núcleo | §18, `DOCS/known-limitations.md` | Fallo en MCU sin `malloc` |

### 2.1 Lo que el núcleo realmente pide al driver

Medido sobre el código, no sobre la spec:

| Llamada | Dirección | Longitud | Alineación |
|---|---|---|---|
| `HWV` | `MFS_HWV_OFFSET` = 512 | **64 B** | 512 |
| `SB` | 0 y `erase_unit` | **256 B** | `erase_unit` |
| Cabecera de zona | `3·EU + k·EU` | **64 B** | `erase_unit` |
| Registro de datos | página dentro de zona | `chunk_size` (64 B … 4096 B) | página |
| Token T1 | `2·EU + slot·32` | **32 B** | 32 |
| `erase` | `k·erase_unit` | — | **siempre múltiplo de `erase_unit`** |

Tres conclusiones que gobiernan todo el diseño de los backends:

- **(a)** Existen programaciones de **64 B y 32 B**. Un backend con unidad de
  escritura de 16 B debe hacer RMW para la de 32 B si cae a mitad de quad-word,
  y **no puede** hacerlo si esa quad-word ya está programada (caso ECC).
- **(b)** Existen programaciones de hasta `chunk_size` (4096 B). Un NOR SPI
  **debe** trocearlas en la frontera de página (típ. 256 B) — de ahí S3.
- **(c)** **Todas** las direcciones de `erase` son múltiplos de `erase_unit`.
  Esto permite definir `erase_unit` como un múltiplo del sector físico del
  dispositivo y borrar el tramo completo: es la clave para resolver la no
  uniformidad de sectores de STM32 sin tocar el núcleo (§5.2.3).

### 2.2 Contexto de llamada (detalle que casi nadie documenta)

- `mfs_read` se ejecuta **dentro** de `mfs_port_crit_enter/exit`
  (`mfs_zone.c:195-197`): la lectura debe ser corta y no bloquear.
- `mfs_write` **no** está en sección crítica (`mfs_zone.c:201-211`): puede
  tardar (polling de WIP, 700 µs–45 ms).
- Por tanto `mfs_port_crit_enter` **no** puede deshabilitar interrupciones
  durante milisegundos, y `mfs_port_wfi` debe ceder o dormir, nunca girar en
  vacío (en ESP-IDF es `taskYIELD()`; es correcto que sea así).

### 2.3 R1 — El presupuesto de RAM del núcleo (medido, y no documentado)

Este no es un hallazgo de código sino de **expectativas**, y es el que más
condiciona la compatibilidad con STM32. La documentación del repositorio declara
presupuestos de RAM por modo de 720 B a 21,5 KB
(`include/matrixfs/mfs_types.h:185-193`) y `mfs_embedded_opts_default()` propone
`ram_total = 32768` «que cubre Balanced sin sobre-declarar»
(`platform/embedded/mfs_embedded.c`). Medido sobre el código:

| Configuración | `.bss` del núcleo + capa embebida | `sizeof(mf_t)` |
|---|---|---|
| Por defecto (32 bits, `MFS_L2P_SLOTS=4096`, `MFS_ZONE_MAX=128`) | **202 496 B** | **17 240 B** |
| `-DMFS_L2P_SLOTS=1024` | 165 440 B | 17 240 B |
| `-DMFS_ALLOW_8BIT_TARGET=1` (perfil mínimo) | **87 168 B** | **1 840 B** |

Reparto por unidad en la configuración por defecto (`.bss`, medido con `size`):

```
mfs_zone.c    45 088 B   (mapa L2P 32 KB + tabla de zonas)
mfs_compact.c 33 856 B
mfs_ftl2.c    33 568 B
mfs_fs.c      33 184 B
mfs_xio.c     24 640 B
mfs_pq.c      12 480 B
mfs_hmt.c     12 288 B
mfs_wal.c      4 096 B
mfs_blake3.c   2 048 B
resto          ~2 000 B
```

**Por qué ocurre.** El núcleo declara **~20 buffers estáticos de
`MFS_SCRATCH_MAX`** (4096 B cada uno en 32 bits) repartidos por `mfs_ftl2.c`,
`mfs_compact.c`, `mfs_xio.c`, `mfs_hmt.c`, `mfs_fs.c`, `mfs_wal.c` y
`mfs_zone.c`. Al ser `static` de ámbito de función, **no se solapan**: cada uno
ocupa su propio hueco en `.bss`. A eso se suman el mapa L2P
(`l2p_ent_t g_l2p[4096]` = 32 KB, `src/core/mfs_zone.c:68`) y la tabla de zonas
dentro de `mf_t`.

**Dos consecuencias que la documentación no anticipa:**

1. **`ram_total` no reserva memoria.** Solo alimenta a `mfs_select_mode()`
   (`src/core/mfs_hal.c:171-196`). El consumo real se fija en **compilación** con
   `MFS_L2P_SLOTS`, `MFS_ZONE_MAX`, `MFS_SCRATCH_MAX`, `MFS_MAX_FILES_OPEN` y
   `MFS_WAL_WINDOW_MAX`. Un usuario que siga el ejemplo, declare 32 KB y elija un
   MCU de 64 KB descubrirá el problema en el enlace, no antes.
2. **La pila de la tarea también es grande.** Varias rutas declaran
   `uint8_t buf[MFS_SCRATCH_MAX]` **en la pila** (no `static`):
   `mfs_fs.c:312,1973,2222,2428` y `crypto/mfs_suites.c:638,701`
   (`MFS_SCRATCH_MAX + 16`). Son ≥ 4 KB por marco y pueden anidarse. Con el
   `configMINIMAL_STACK_SIZE` por defecto de CubeMX (128 palabras ≈ 512 B) el
   desbordamiento es inmediato: para un port de FreeRTOS esto es un *hard fault*
   garantizado, y era invisible.

**Corrección aplicada.** No se puede reducir el consumo sin cambiar el diseño del
núcleo, pero sí se puede hacer **predecible y configurable**:

- `MFS_ZONE_MAX` pasa a ser sobreescribible por build
  (`include/matrixfs/mfs_types.h`, antes era un `#define` fijo). Reduce `mf_t` de
  17 240 a 14 680 B con `MFS_ZONE_MAX=64`.
- El port de STM32 expone `MFS_L2P_SLOTS` y `MFS_ZONE_MAX` como opciones de
  primer nivel (`matrixfs_stm32.mk`, `CMakeLists.txt`) y **documenta la tabla
  medida** en su README, con la regla explícita de qué familias caben.
- El port de ESP-IDF los expone por `Kconfig`
  (`MATRIXFS_L2P_SLOTS`, `MATRIXFS_ZONE_MAX`) y su `Kconfig` advierte de que
  `MATRIXFS_RAM_BUDGET` **no reserva memoria**.
- Se documenta el requisito de pila (≥ 16 KB en 32 bits, ≥ 8 KB en el perfil
  mínimo) en ambos README y en `DOCS/known-limitations.md`.

> Nota de proceso: este hallazgo no salió de leer la documentación sino de
> **medir** (`size` sobre los objetos del núcleo y `sizeof` sobre `mf_t`). Un
> proyecto que declara presupuestos de RAM por modo debería verificar el consumo
> real en CI con `size`, igual que verifica la suite de 1232 comprobaciones.

---

## 3. Auditoría del componente ESP-IDF (`platform/esp-idf/`)
### 3.1 E1 — El componente no enlaza (bloqueante)

**Evidencia.** `platform/esp-idf/CMakeLists.txt:55-68` lista explícitamente las
fuentes del núcleo y **no incluye `src/core/mfs_arch.c`**:

```
54	        # Núcleo: core / crypto / ftl / tier / sec / xio.
55	        "../../src/core/mfs_compact.c"
56	        "../../src/core/mfs_fs.c"
57	        "../../src/core/mfs_hal.c"      ← llama mfs_arch_detect()
58	        "../../src/core/mfs_profile.c"
59	        "../../src/core/mfs_util.c"     ← llama mfs_arch_crc32c_hw_available()
60	        "../../src/core/mfs_wal.c"
61	        "../../src/core/mfs_zone.c"
```

Y las dos llamadas son **incondicionales**:

- `src/core/mfs_hal.c:62` → `(void)mfs_arch_detect(&ai);`
  (alcanzable desde `mf_init` vía `mfs_fs.c:784` y `:885`)
- `src/core/mfs_util.c:139-146`:
  ```c
  uint32_t mfs_crc32c(const uint8_t *buf, uint32_t len, uint32_t seed) {
    if (mfs_arch_crc32c_hw_available())      /* ← definido solo en mfs_arch.c */
      return mfs_crc32c_hw(buf, len, seed);
    return crc32c_sw(buf, len, seed);
  }
  ```

Definiciones en `src/core/mfs_arch.c:102` y `:211`. Resultado: **`undefined
reference to mfs_arch_detect` y `mfs_arch_crc32c_hw_available`** en el enlace de
cualquier aplicación. No es un problema de «no probado»: es un fallo
determinista.

**Por qué pasó desapercibido, y por qué es específico de este port.** El resto
de integraciones del repositorio usan *globs* que capturan `src/core/*.c`
completo:

```make
# Makefile:34, platform/linux/Makefile:61
CORE_SRC := $(wildcard src/core/*.c) ...
```
```cmake
# CMakeLists.txt:21
file(GLOB CORE_SRC ${CMAKE_SOURCE_DIR}/src/core/*.c ...)
```

`platform/esp-idf/CMakeLists.txt` es **el único** build con lista escrita a mano,
y es justo el que se dejó un fichero. `platform/micropython/micropython.mk:43`
excluye `mfs_port_arch.c` a propósito — pero eso es correcto, porque el núcleo
nunca lo llama (verificado: no existe ninguna referencia a
`mfs_port_arch_init/get_ops/set_arch` fuera de su propia unidad y su cabecera).
Lo mismo para `mfs_port_rtos.c`.

**Corrección aplicada.** Se añade la unidad que falta y se sustituye la lista a
mano por la misma fuente de verdad que el resto del repositorio, para que el
defecto no pueda repetirse: `MATRIXFS_CORE_SRC` derivado de
`file(GLOB ...)` + exclusión explícita y justificada de `mfs_port_arch.c`
(porque el componente aporta su propio puerto).

> Lección de proceso: una lista de fuentes escrita a mano **debe** verificarse
> con un enlace real. El nuevo test de host (§7) enlaza el componente contra el
> núcleo completo y falla con el mismo error que fallaría `idf.py build`, de modo
> que esta clase de defecto deja de ser invisible.

### 3.2 E2/E3 — Registro del componente y rutas de fuentes

**E2.** `CMakeLists.txt:73-75` declaraba `REQUIRES esp_partition esp_timer`. La
cabecera pública `include/matrixfs_esp.h:34-35` incluye **solo** cabeceras de
MatrixFS, no de IDF. Con `REQUIRES`, todos los consumidores heredan los
*include dirs* de IDF. Lo correcto es `PRIV_REQUIRES`. Además, la verificación de
la API confirmó qué componentes son **comunes** (siempre en el build, no hace
falta declararlos): `freertos`, `esp_hw_support` (que aporta `esp_cpu.h`), `log`
y `newlib`/`esp_libc`. Solo hay que declarar los que no lo son:
`esp_partition`, `esp_flash`, `esp_timer` y `vfs`.

**E3 — corrección al análisis inicial.** Al revisar el código de `idf.py` se
comprueba que **ESP-IDF NO emite error ni aviso** por listar fuentes fuera del
directorio del componente: `__component_add_sources` resuelve cada ruta con
`get_filename_component(... ABSOLUTE BASE_DIR "${COMPONENT_DIR}")` y la pasa a
`add_library`, sin ninguna comprobación de ubicación (verificado en
`tools/cmake/component.cmake` de v5.3 y de master). La afirmación «IDF avisa»
que figuraba en el análisis inicial **no está respaldada** y se retira.

El problema real es distinto y sí justifica el diseño aplicado:

- **Los *include dirs* sí deben existir.** IDF aborta con
  `FATAL_ERROR "Include directory '...' is not a directory."` si una ruta de
  `INCLUDE_DIRS` no existe. Por eso el `CMakeLists.txt` **valida primero** que el
  árbol del repositorio esté donde espera y falla con un mensaje accionable, en
  lugar de dejar que IDF falle con una ruta opaca.
- **La reubicación rompe el componente.** Si `platform/esp-idf/` se copia a
  `components/matrixfs/` sin el resto del árbol —o se instala desde el registro
  en `managed_components/`— las rutas `../../src` dejan de resolver.

**Corrección aplicada.** El núcleo se compila como **librería estática propia**
enlazada al componente (`target_link_libraries(${COMPONENT_LIB} INTERFACE
matrixfs_core)`), que es uno de los enfoques que la propia documentación del
build system admite para incorporar código externo, y el `CMakeLists.txt` falla
con diagnóstico claro si falta el árbol.

### 3.3 E6 — Modelo de medio: alineación, cifrado y granularidad

`matrixfs_esp.c:129-178` implementa *read-modify-write* alineado a **4 bytes**.
El comentario reconoce el riesgo («en particiones cifradas el bloque es de
16 B») pero el código no lo cubre. Dos problemas distintos:

1. **Alineación insuficiente** con `CONFIG_FLASH_ENCRYPTION_ENABLED`: escribir
   con longitud/offset no múltiplos del bloque de cifrado devuelve
   `ESP_ERR_INVALID_ARG` o produce datos mal cifrados.
2. **La capa compartida no puede expresarlo.** `mfs_embedded_flash_t`
   (`platform/embedded/mfs_embedded.h:24-48`) **no tiene** campo de granularidad
   de programación, y `mfs_embedded_setup()` la fija a 1:
   ```c
   platform/embedded/mfs_embedded.c:92
   flash->geom.program_granularity = 1u; /* NOR programable byte a byte */
   ```
   Aunque el HAL de ESP-IDF o el de STM32 necesiten 16 B, **no hay forma de
   declararlo**. Es el hallazgo **S1**, y es común a ambos ports.

**Corrección aplicada.** (i) `mfs_embedded_flash_t` gana `program_granularity`
y `mfs_embedded_setup()` lo propaga; (ii) el backend ESP-IDF lo calcula del
entorno real (`esp_flash_encryption_enabled()` → 16, si no 4) y el RMW se alinea
a esa granularidad.

**Corrección al análisis inicial (importante).** La afirmación «hay un fallo de
alineación de 16 B» resultó ser **falsa en la práctica**, y se comprobó
midiendo, no razonando. Los requisitos reales, verificados en la cabecera y en
`esp_flash_api.c` de v5.3, son:

| Partición | Requisito de `esp_partition_write` | Error si se viola |
|---|---|---|
| Sin cifrar | **ninguno** (byte a byte); el dato debe estar borrado antes (NOR 1→0) | — |
| Con cifrado | `offset` y `length` **múltiplos de 16 B** | `offset%16≠0` → `ESP_ERR_INVALID_ARG`; `length%16≠0` → `ESP_ERR_INVALID_SIZE` |

Instrumentando el driver L2 del núcleo con un contador (véase el test de host)
sobre una carga representativa: **96 programaciones, y las 96 con dirección y
longitud múltiplos de 16 B** (`min(addr % 16) = 0`, `min(addr % 32) = 0`). La
razón es que el núcleo escribe páginas de `chunk_size` (256…4096 B, múltiplos de
16) en direcciones alineadas a página, más el HWV (512, 64 B) y las cabeceras de
zona (en múltiplos de `erase_unit` ≥ 1024). Con `g = 4` la ventana
`[off & ~3, align_up(off+len, 4))` coincide con `[off, off+len)` y también es
múltiplo de 16.

Por tanto **E6 es un riesgo latente, no un defecto vivo**: se materializaría si
el núcleo llegase a programar 1…15 B en una dirección no múltiplo de 16. La
corrección se mantiene porque (a) cuesta dos líneas, (b) protege frente a
cambios futuros del núcleo y (c) el modelo de host incluye un **control
negativo** que sí discrimina: con cifrado activo, `esp_partition_write` rechaza
`offset=516, len=4` y `offset=512, len=12`.

> Lección de método: una afirmación de «fallo» basada en leer el código y no en
> medirlo puede ser falsa. El valor del banco de pruebas en host fue justamente
> permitir comprobar la hipótesis en lugar de sostenerla.

### 3.3bis Hechos verificados de la API de ESP-IDF (v5.3, con deltas v6.x)

Contrastados contra cabeceras y fuentes oficiales. Los que cambiaron el diseño:

| Hecho | Consecuencia en el port |
|---|---|
| `esp_partition_erase_range` valida la alineación contra **`SPI_FLASH_SEC_SIZE` (0x1000)**, no contra `partition->erase_size` | `esp_flash_erase()` normaliza a 4096 y **rechaza** una unidad lógica que no sea múltiplo de 4096 (antes podía fallar con `MFS_EIO` en silencio) |
| En **v5.3**, `esp_partition_register_external()` **no inicializa `erase_size`** (queda 0 por `calloc`) | El componente trata `erase_size == 0` como 4096. Es la trampa que habría dado `erase_unit = 0`. En v5.5 ya se fija a 4096 |
| `esp_partition_write` **no** exige alineación en particiones sin cifrar | El RMW a 4 B es conservador, no obligatorio; se mantiene porque es inocuo en NOR |
| `ESP_PARTITION_SUBTYPE_ANY` es válido para «cualquier subtipo» | El componente localiza la partición correctamente |
| Para un subtipo propio, lo documentado es el property CMake **`EXTRA_PARTITION_SUBTYPES`**; el subtipo numérico en el CSV funciona pero no está documentado | El README lo recomienda ahora |
| `esp_cpu_get_cycle_count()` es `uint32_t`, **core-local**, envuelve a 2³² y devuelve 0 si no está soportado | Confirma E7: no sirve para medir intervalos **entre núcleos** |
| `esp_timer_get_time()` es `int64_t` µs, correcto con DFS y light sleep, reinicia en deep sleep | Confirma la elección de `mfs_port_time_us` |
| `portENTER_CRITICAL` **funciona** desde ISR en ESP32 por defecto (es la misma función), pero **aborta** con `CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE`; lo previsto son `_ISR`/`_SAFE` | Convierte E8 de «no es ISR-safe, punto» a un matiz que se documenta con precisión |
| `esp_partition_register_partition()` **no existe**; el nombre correcto es `esp_partition_register_external()` | El README ya usaba el nombre correcto |
| `idf.py qemu` es soporte oficial; en v5.3 sólo `esp32` y `esp32c3`; en v6.x se añade `esp32s3` | Limita la receta de CI documentada |
| `esp_vfs_t` es una **unión** de dos juegos de callbacks y el flag decide cuál; en v6.x los punteros sin `_p` están deprecados | **Origen del fallo E11**; la solución (`ESP_VFS_FLAG_CONTEXT_PTR` + `*_p`) es además la no deprecada en v6.x |

### 3.4 E4 — Concurrencia

El núcleo mantiene estado global (mapa L2P, pools CFX, ventana WAL, ventana de
inodos) y **no es reentrante** — lo dice `DOCS/known-limitations.md:72-77`. Una
aplicación ESP-IDF es multitarea desde el primer `xTaskCreate`, y el componente
no tomaba ninguna precaución: dos tareas llamando a `mf_write` corrompen el
volumen. `matrixfs_esp_last_error()` (`matrixfs_esp.c:338-340`) devuelve un
buffer estático compartido, y su propia cabecera admite que «no es thread-safe».

**Corrección aplicada.** Mutex recursivo de FreeRTOS creado de forma perezosa y
serialización de toda la superficie pública, activable por Kconfig
(`CONFIG_MATRIXFS_THREAD_SAFE`, por defecto `y`), más un par
`matrixfs_esp_lock()`/`matrixfs_esp_unlock()` para que la aplicación agrupe
operaciones compuestas (p. ej. `stat` + `open`). El buffer de error pasa a
`__thread`-equivalente por tarea (buffer por *task handle* acotado) o, si el
mutex está activo, se documenta como válido solo dentro del bloqueo.

### 3.5 E5 — Sin VFS POSIX

El componente solo ofrece `matrixfs_esp_mount/format/mount_default/last_error`.
No hay forma de usar `fopen`, `stat`, `opendir` ni de montar el volumen en un
punto de montaje, que es lo que un usuario de ESP-IDF espera (SPIFFS y FAT lo
hacen con `esp_vfs_register`). **Implementado**: `matrixfs_esp_vfs.c` con
`esp_vfs_t` y traducción de errores del núcleo a `errno` (`MFS_ENOENT`→`ENOENT`,
`MFS_EEXISTS`→`EEXIST`, `MFS_ENOSPC`→`ENOSPC`, `MFS_EACCES`→`EACCES`,
`MFS_EROFS`→`EROFS`, `MFS_ENOTSUP`→`ENOSYS`, resto→`EIO`).

### 3.6 E7/E8 — Ciclos, tiempo y contexto de ISR

- `mfs_port_cycles` = `esp_cpu_get_cycle_count()` (`matrixfs_esp.c:86`). En
  partes **duales** (ESP32, ESP32-S3) el contador es **por núcleo**: dos tareas
  en núcleos distintos observan escalas de tiempo distintas. `mfs_port_time_us`
  usa `esp_timer_get_time()`, que sí es global, así que la única consecuencia es
  que `cycles` no sirve para medir intervalos entre núcleos. Se documenta y se
  ofrece el contador de `esp_timer` como alternativa por Kconfig.
- `mfs_port_crit_enter` = `portENTER_CRITICAL` (`:82`). **Matiz verificado:** en
  ESP32 esta macro y `portENTER_CRITICAL_ISR` son la **misma** función, de modo
  que llamarla desde una ISR *funciona* con la configuración por defecto; pero
  **aborta** si se habilita `CONFIG_FREERTOS_CHECK_PORT_CRITICAL_COMPLIANCE`, y
  no es el uso previsto (lo correcto en ISR es `portENTER_CRITICAL_ISR`, y
  `portENTER_CRITICAL_SAFE` si el contexto es mixto). El núcleo usa el puerto en
  contexto de **tarea** (alrededor de las lecturas), así que la elección es
  correcta; se documenta la distinción en lugar de afirmar sin matices que «no
  es ISR-safe».

### 3.7 E9/E10 — Kconfig, capacidad y verificabilidad

- **Capacidad.** `MFS_ESP_MIN_SECTORS 4` (`matrixfs_esp.c:56`) exige 3 sectores
  reservados + 1 zona. Correcto, pero el README no dice que por encima de
  `MFS_ZONE_MAX (128)` zonas × `erase_unit` el espacio **no se aprovecha**
  (`DOCS/known-limitations.md`, y `include/matrixfs/mfs_types.h:218`). El
  componente ahora **valida y reporta** la capacidad utilizable real al montar,
  y ofrece `MATRIXFS_ZONE_MAX` como ajuste para particiones grandes.
- **Kconfig** carecía de: tamaño de página, presupuestos `t_*`, thread-safety,
  VFS, modo forzado por defecto y opciones de flash externa. Añadidos.
- **E10.** No había ejemplo, ni `partitions.csv`, ni `sdkconfig.defaults`, ni
  forma de compilar en CI. Añadidos, más un *shim* de host que permite
  **ejecutar** la integración sin ESP-IDF instalado.

---

## 4. Auditoría de la capa compartida (`platform/embedded/`, `platform/common/`)

Esta capa es la que comparten ESP-IDF, Arduino, PlatformIO y MicroPython, así
que sus límites **limitan a todos los ports a la vez**. Los hallazgos S3, S4, S5
y S6 son defectos reales, no limitaciones de diseño.

### 4.1 S1 — `program_granularity` no existe (bloqueante para STM32)

`platform/embedded/mfs_embedded.h:24-48` define la región de flash con
`read/prog/erase`, `size`, `erase_unit`, `page_size`, `t_*`, `no_erase`.
**No hay granularidad de programación**, y `mfs_embedded.c:92` la fuerza a 1.

Esto hace **inexpresable** la flash interna de STM32 (8 B en L4/G4/H5/U4,
16 B en H7/U5) y las particiones cifradas de ESP-IDF (16 B). El campo
`mfs_media_geom.program_granularity` **sí existe** (`mfs_port.h:48`) y se
serializa al HWV (`mfs_util.c:164`), pero nunca se usa para trocear
programaciones: es informativo. La corrección es propagar el valor declarado y
que **cada backend** implemente el RMW correspondiente, que es donde está la
información real del dispositivo.

### 4.2 S2 — Uniformidad de `erase_unit`

`mfs_embedded` (y el núcleo) asumen un único `erase_unit` para toda la región
— ver §1 y `src/mfs_internal.h:27-36`. STM32 tiene familias con sectores de
tamaños distintos dentro del mismo *die* (F4: 16 KB ×4 + 64 KB ×1 + 128 KB ×3;
F1: 1 KB/2 KB/4 KB…; H7: 128 KB uniformes). Como **todas** las direcciones de
`erase` son múltiplos de `erase_unit` (§2.1c), la solución no requiere tocar el
núcleo:

> **Convenio.** El integrador elige una **ventana uniforme**: un tramo contiguo
> de `n` sectores del dispositivo **de igual tamaño `s`**, y declara
> `erase_unit = s` (o `k·s`). El backend implementa `erase(addr)` borrando
> **todos** los sectores del dispositivo que cubren `[addr, addr + erase_unit)`.

Se añade el helper `platform/common/mfs_sectors.{h,c}` con
`mfs_sector_uniform_window()` para calcular esa ventana a partir de una tabla de
sectores y **test unitario en host**, porque la aritmética es justo donde este
tipo de código falla en silencio.

Restricción dura detectada: `mfs_hal.c:104-107` fuerza `erase_unit >= 1024`
(`if (out->erase_unit < 1024u) out->erase_unit = 4096u;`). En familias con
sectores de 128 B (STM32L0) o 1 KB (STM32F1), declarar el tamaño real **no es
posible si es < 1024**; la ventana uniforme debe agregar sectores hasta 4 KB
(32 × 128 B o 4 × 1 KB), que es exactamente lo que hace el convenio anterior.

### 4.3 S3 — `l2_prog` no trocea en la frontera de página (crítico)

`platform/common/mfs_l2_8bit.c:114-159` emite **una sola** orden *Page Program*
para todo `len`:

```c
st = d->cfg.spi_transfer(d->cfg.ctx, hdr, NULL, n);
if (st == MFS_OK)
  st = d->cfg.spi_transfer(d->cfg.ctx, (const uint8_t *)src, NULL,
                           (uint16_t)len);      /* ← sin trocear por página */
```

En un NOR SPI, *Page Program* **no puede cruzar** una frontera de página: el
dispositivo **envuelve** dentro de la página, de modo que escribir 4096 B desde
un offset con `offset % 256 != 0` corrompe el bloque **sin devolver error**.
Y el núcleo pide exactamente eso: `src/core/mfs_zone.c:487`
`mfs_write(fs, addr, buf, pb)` con `pb = chunk_size` hasta 4096 B. Además:

- `l2_nor_wren()` se envía **una vez** (línea 123) cuando el troceado exige un
  `WREN` **por página**;
- `(uint16_t)len` trunca en silencio por encima de 64 KiB (S6), en `l2_read`
  (línea 108) y en `l2_prog` (línea 148).

Corrección: bucle que trocea por `page_size`, con `WREN` + *Page Program* +
espera de WIP por cada trozo, y uso de `uint32_t`/tamaño real en el callback de
bus (`mfs_l2_8bit.h:37-38` declara `uint16_t len`, que además hay que ensanchar
o documentar como cota).

### 4.4 S4 — Las rutas I²C son inalcanzables

`mfs_l2_8bit.h:23-25` declara `MFS_L2_8BIT_I2C_FRAM` y
`MFS_L2_8BIT_I2C_EEPROM`, `mfs_l2_8bit_cfg_t` tiene `i2c_init/deinit/write/read`
(líneas 44-47), pero:

- `mfs_l2_8bit_create()` (`:247-248`) **exige** `spi_cs_low`, `spi_cs_high` y
  `spi_transfer` sin mirar `cfg->type` ⇒ una configuración I²C **siempre**
  devuelve `MFS_EINVAL`;
- `l2_read` (`:87-100`) y `l2_prog` (`:129-140`) no tienen rama para los tipos
  I²C: caen en `default: return MFS_ENOTSUP;`
- `mfs_l2_8bit_probe_spi` (`:314`) vuelve a exigir SPI.

Es decir: la mitad «I²C» de la tabla de medios MCU está declarada pero **no
implementada**, lo que bloquea directamente uno de los medios pedidos para STM32
(FRAM/EEPROM I²C). Corregido: validación por tipo, despacho I²C real
(dirección + *word address* de 2 B, *page write* con troceo por página de
escritura del dispositivo) y `probe` opcional.

### 4.5 S5 — Geometría del driver MCU

`mfs_l2_8bit_get_geom()` (`:192-240`):

```c
geom->program_granularity =
    (drv->cfg.type == MFS_L2_8BIT_SPI_FRAM ||
     drv->cfg.type == MFS_L2_8BIT_I2C_FRAM) ? 1u : 1u;   /* ← ternario inútil */
...
geom->zone_size = drv->cfg.erase_size;   /* 0 en FRAM/EEPROM */
```

- el ternario devuelve `1u` en ambas ramas (redundante);
- `MFS_L2_8BIT_INTERNAL_FLASH` se mapea a `MFS_MEDIA_NOR_SPI` con
  `program_granularity = 1`, **incorrecto** para STM32 (8/16 B);
- `zone_size = 0` para FRAM/EEPROM, y `0` significa «derivar» en
  `mfs_port.h:57`, pero `erase_unit` queda en 4096 por el `?:` de la línea 227,
  con lo que el layout reserva 3 × 4096 B de una FRAM byte-direccionable sin
  necesidad.

Corregido: `pgm_gran` explícito en `mfs_l2_8bit_cfg_t`, `zone_size` derivado de
`erase_unit` cuando no hay borrado, y `flags0 |= MFS_HWV0_ECC_ON_DIE` para flash
interna con ECC.

> Nota sobre ECC: declarar `MFS_HWV0_ECC_ON_DIE` **cambia el comportamiento del
> núcleo** — `src/ftl/mfs_ftl2.c:175-176` selecciona `MFS_ECC_LDPC` (la clase
> más tolerante) cuando el bit está puesto. Es exactamente lo que se quiere para
> flash interna con ECC por hardware, pero conviene saber que la consecuencia no
> es cosmética.

### 4.6 S6 — Truncamiento a 16 bits

Los callbacks de bus y las transferencias usan `uint16_t len`
(`mfs_l2_8bit.h:37-38`, y los casts de `mfs_l2_8bit.c:108,148`). Cualquier
transferencia > 65535 B se trunca **sin diagnóstico**. Corregido ensanchando a
`uint32_t` en la interfaz y troceando internamente.

---

## 5. Diseño del port STM32Cube (`platform/stm32cube/`)

No existía nada. El diseño parte de una decisión: **no reimplementar nada que ya
exista** y apoyarse en las tres capas que el repositorio ya tiene.

### 5.1 Arquitectura y reparto

```
        Aplicación STM32 (CubeMX / CubeIDE)
                    │
        matrixfs_stm32.h   ← API pública del port
                    │
   ┌────────────────┼──────────────────────────────┐
   │                │                              │
 mfs_embedded   mfs_l2_8bit (MCU)          mfs_l2_managed
 (región plana)  (dispositivo SPI/I²C)      (medio gestionado)
   │                │                              │
 iflash  ospi     spi_nor fram eeprom            sdmmc
 (HAL_FLASH) (HAL_OSPI/QSPI) (HAL_SPI/I2C)     (HAL_SD/MMC)
```

| Medio | Capa usada | Motivo |
|---|---|---|
| Flash interna | `mfs_embedded` | Región plana, geometría declarada por familia |
| NOR externa OSPI/QSPI | `mfs_embedded` | Región plana, sectores uniformes de 4 KB |
| NOR externa SPI | `mfs_l2_8bit` | Ya implementa comandos JEDEC, WIP, RDID |
| FRAM / EEPROM SPI-I²C | `mfs_l2_8bit` | Ya implementa FRAM/EEPROM, **tras corregir S4** |
| SD / eMMC | `mfs_l2_managed` | El dispositivo trae FTL; no duplicarlo (§3.2) |

API pública (una sola puerta, descriptor estático, instancia única — igual que
ESP-IDF y por la misma razón: el núcleo no es reentrante):

```c
typedef enum {
  MFS_STM32_MEDIA_IFLASH = 0,   /* flash interna vía HAL_FLASH          */
  MFS_STM32_MEDIA_OSPI_NOR,     /* NOR externa OSPI/QUADSPI             */
  MFS_STM32_MEDIA_SPI_NOR,      /* NOR externa SPI (mfs_l2_8bit)        */
  MFS_STM32_MEDIA_FRAM,         /* FRAM SPI o I²C                       */
  MFS_STM32_MEDIA_EEPROM,       /* EEPROM SPI o I²C                     */
  MFS_STM32_MEDIA_SDMMC         /* SD/eMMC (mfs_l2_managed)             */
} mfs_stm32_media_t;

mfs_st matrixfs_stm32_mount(mf_t *fs, const mfs_stm32_cfg *cfg);
mfs_st matrixfs_stm32_format(mf_t *fs, const mfs_stm32_cfg *cfg);
mfs_st matrixfs_stm32_mount_default(mf_t *fs);   /* usa mfs_stm32_conf.h */
const char *matrixfs_stm32_last_error(void);
void matrixfs_stm32_lock(void);                  /* operación compuesta */
void matrixfs_stm32_unlock(void);
/* Diagnóstico */
const mfs_hwv_t *matrixfs_stm32_hwv(void);
uint32_t matrixfs_stm32_capacity_bytes(void);
```

### 5.2 Flash interna (`mfs_stm32_iflash.c`) — el caso difícil

#### 5.2.1 Unidades y ECC

La flash interna de STM32 **no** es un NOR byte-programable:

| Familia | Programa mínimo | Borrado | ECC | RWW |
|---|---|---|---|---|
| F0, F1 | 2 B (halfword) / 1 B | página 1–2 KB (no uniforme) | no | no |
| F3 | 2 B | página 2 KB | no | no |
| F4 | 4 B (word) | **no uniforme**: 16 KB ×4, 64 KB ×1, 128 KB ×3 | no | no |
| F7 | 4 B | no uniforme (32/128/256 KB) | no | sí (dual bank) |
| L0 | 4 B | **128 B** | no | no |
| L1 | 4 B | 256 B | no | no |
| L4, L5, G0, G4, U5 | **8 B** (doubleword) | 2 KB (L4/G0/G4), 8 KB (U5) | **sí** | L5/U5 sí |
| H5, H7 | **16 B** (quadword) | 128 KB, uniforme en H7 | **sí** | sí (dual bank) |
| WB, WL | 8 B | 4 KB / 2 KB | parcial | no |

Dos consecuencias que dominan el diseño:

1. **RMW obligatorio.** El núcleo pide programaciones de **32 B** (token T1,
   `mfs_wal.c:175`) y **64 B** (HWV `mfs_fs.c:120`, cabecera de zona
   `mfs_zone.c:249`). Con unidad de 16 B, 32 B y 64 B son múltiplos exactos
   **si la dirección está alineada**; el token en `2·EU + slot·32` lo está. Los
   registros de datos (chunk 64…4096 B) también. Aun así el backend **no puede
   asumir** alineación: implementa RMW para cualquier `(addr, len)`.
2. **Con ECC no se puede reprogramar.** En L4/H5/H7/U5, programar dos veces la
   misma double/quad-word —aunque sea con el mismo valor— produce `PROGERR`, y
   dejar una quad-word a medio programar produce **error de ECC al leerla**
   (`ECCC`/`ECCC2` en H7, `DBECCERR`/`SNECCERR` en U5). El RMW ciego del port
   ESP-IDF (leer ventana, reescribirla entera) **no es válido aquí**.

**Algoritmo de programación (ECC-safe).** Para cada unidad `u` (8 o 16 B) que
cubra `[addr, addr+len)`:

```
leer u completa (HAL, la lectura devuelve el dato ya corregido por ECC)
si u es todo 0xFF:                       → fusionar y programar (camino normal)
si no:
    si (u_actual & valor_nuevo) == valor_nuevo  → ya está: no programar (no-op)
    si no                                       → MFS_EIO  (requiere erase)
```

La segunda rama es la clave: como NOR solo permite 1→0, si el dato ya presente
**contiene** el nuevo (AND bit a bit igual al nuevo), la operación es un no-op
seguro; si no, **es imposible sin borrar el sector** y se devuelve error
tipificado en lugar de corromper. Este caso no debería ocurrir con el diseño
log-structured del núcleo (que borra la zona antes de programar), pero el
backend lo **detecta y lo reporta** en vez de confiar en ello — y el test de
host lo provoca a propósito.

#### 5.2.2 Borrado por tramo

`erase(addr)` borra **todos** los sectores del dispositivo que cubren
`[addr, addr + erase_unit)`. Como el núcleo solo borra en múltiplos de
`erase_unit` (§2.1c), esto es correcto por construcción y **elimina la necesidad
de que los sectores sean uniformes** desde el punto de vista del núcleo: la
uniformidad se consigue **eligiendo la ventana** (§5.2.3). El backend usa
`HAL_FLASHEx_Erase` con el número de sector/página obtenido de una tabla por
familia, y soporta doble banco (`FLASH_BANK_1/2`).

#### 5.2.3 Selección de la ventana y reserva en el linker

Dos problemas de STM32Cube que ESP-IDF no tiene:

1. **No hay tabla de particiones.** La región del volumen se declara en
   `mfs_stm32_conf.h` (`MFS_STM32_IFLASH_ADDR`, `MFS_STM32_IFLASH_SIZE`) **y** se
   reserva en el *linker script* con un `ASSERT` que falla el build si el código
   o los datos invaden la región:
   ```ld
   _mfs_region_start = 0x08040000;
   _mfs_region_end   = 0x08080000;
   ASSERT(_mfs_region_start >= _edata, "MatrixFS: la region pisa datos")
   ASSERT(_mfs_region_end  <= ORIGIN(FLASH) + LENGTH(FLASH), "...")
   ```
2. **Los sectores son no uniformes.** `mfs_stm32_iflash_uniform_window()` recorre
   la tabla de sectores de la familia y devuelve el **mayor tramo contiguo de
   sectores iguales** que empiece en o después de la dirección pedida. Si el
   usuario reserva una región que cruza un cambio de tamaño, el port **no
   adivina**: informa del tramo válido y del descartado.

Criterio de aceptación: la capacidad declarada nunca puede exceder
`MFS_ZONE_MAX × erase_unit` (128 zonas), y el port lo reporta.

#### 5.2.4 Avís sobre *read-while-write*

En familias **single-bank** (F0, F1, F3, L0, L1, G0, G4, WB, WL), programar o
borrar la flash **detiene la búsqueda de instrucciones** durante toda la
operación: un borrado de 45 ms se convierte en 45 ms sin ejecutar código. Un
sistema de archivos que hace GC y borrados periódicos **rompe los plazos** de
cualquier tarea de tiempo real. Esto no se puede «arreglar» por software; el
diseño lo resuelve así:

- el port **declara la penalización** y la publica por diagnóstico;
- la recomendación por defecto es **NOR externa OSPI** o **SD/eMMC**, que no
  bloquean la CPU;
- si se usa flash interna en single-bank, se documenta que el volumen debe
  montarse con `t_erase_max_us` realista y que el GC puede causar *jitter* de
  decenas de milisegundos.

### 5.3 NOR externa OSPI/QUADSPI (`mfs_stm32_ospi.c`)

Medio ideal para MatrixFS: sectores uniformes de 4 KB, sin ECC, sin bloquear la
CPU, lectura en sitio.

- **Modos.** Lectura en **indirecto** para el camino de datos del núcleo
  (evita la coherencia de caché) y **memory-mapped** disponible para lectura
  directa; programación y borrado **siempre** en indirecto (la región mapeada es
  de solo lectura).
- **Troceado por página.** Igual que S3: *Page Program* de 256 B, con `WREN` y
  espera de WIP por página. Este backend **no** reutiliza `mfs_l2_8bit`, así que
  implementa el troceado correctamente desde el principio — y el test lo
  verifica con len > página y offset no alineado.
- **Caché Cortex-M7.** En H7 (y L4+/U5) tras programar hay que
  **invalidar la D-caché** del rango si se va a leer por la ventana mapeada, o
  leer en indirecto. El port configura la región mapeada como *non-cacheable* o
  *write-through* por MPU cuando está disponible y documenta la elección.
- **Autodetección.** JEDEC ID (`0x9F`) → capacidad y geometría; opcionalmente
  SFDP para `erase_unit`/`page_size` reales. Si no hay respuesta, error
  tipificado (nunca geometría inventada).
- **4 bytes de dirección.** `EN4B (0xB7)`/`EX4B (0xE9)` para dispositivos
  > 16 MiB, ya previstos en `mfs_l2_8bit.h:120-121`.

### 5.4 SD/eMMC (`mfs_stm32_sd.c`)

Se apoya en `mfs_l2_managed`, que **ya** hace el RMW alineado a sector, el
`bounce buffer` y el TRIM. El port solo aporta los 2-3 callbacks de sector sobre
`HAL_SD_ReadBlocks`/`HAL_SD_WriteBlocks` (bloque de 512 B, timeout acotado) y el
`flush` (`HAL_SD_GetCardState`). `trim_sectors` → `HAL_SD_Erase` cuando la
tarjeta lo soporte; si no, se deja `NULL` y el adaptador no lo anuncia.
La caché de datos del Cortex-M7 hay que mantenerla coherente con el DMA de SDMMC
(el HAL lo contempla con `SCB_InvalidateDCache`/`CleanDCache` en las variantes
`_DMA`); se documenta como requisito del BSP.

### 5.5 FRAM / EEPROM SPI e I²C (`mfs_stm32_fram.c`)

Reutiliza `mfs_l2_8bit` **después de corregir S4**. Aporta los callbacks de bus
sobre `HAL_SPI_TransmitReceive`/`HAL_I2C_Mem_Read`/`HAL_I2C_Mem_Write` con
CS por software. Fruto de usar la capa existente:

- FRAM: `no_erase`, `program_granularity = 1`, sin desgaste efectivo;
- EEPROM: **page write** con `t_prog_max_us` alto (5 ms típicos) → el troceado
  por página de escritura del dispositivo es obligatorio, no opcional.

### 5.6 Región, layout y geometría declarada

| Medio | `erase_unit` | `page_size` | `program_granularity` | `flags` |
|---|---|---|---|---|
| Flash interna (F4, ventana 128 KB) | 131072 | 16 | 4 | — |
| Flash interna (L4, ventana 2 KB) | 2048 → **agregada a 4096** | 8 | 8 | `ECC_ON_DIE` |
| Flash interna (H7) | 131072 | 16 | 16 | `ECC_ON_DIE` |
| OSPI/QSPI NOR | 4096 | 256 | 1 | `SUSPEND_E|SUSPEND_P` |
| SPI NOR | 4096 | 256 | 1 | `SUSPEND_E|SUSPEND_P` |
| FRAM | 4096 (virtual) | 1 | 1 | `BYTE_ADDR` |
| EEPROM | 4096 (virtual) | 64 | 1 | `BYTE_ADDR` |
| SD/eMMC | `alloc_unit` del perfil | 512 | 1 | `MANAGED|ECC_ON_DIE` |

> Nótese la fila L4: `erase_unit` real de 2 KB es **< 1024×2** pero el core
> fuerza ≥ 1024; 2048 pasa el filtro, pero el layout reserva 3×2048 = 6 KB de un
> sector de 2 KB, lo cual es correcto. Lo que **no** se puede es usar 128 B (L0)
> sin agregar a 4 KB.

### 5.7 Puerto §20.2 (`mfs_stm32_port.c`)

Un solo fichero, tres modos detectados en compilación:

| Modo | Detección | Crítica | Ciclos | Tiempo | WFI |
|---|---|---|---|---|---|
| FreeRTOS (CubeMX) | `configUSE_PREEMPTION` / `USE_FREERTOS` | `taskENTER/EXIT_CRITICAL` | `DWT->CYCCNT` | `xTaskGetTickCount()·1e6/configTICK_RATE_HZ` | `taskYIELD()` |
| CMSIS-RTOS v2 | `CMSIS_V2` / `osKernelGetTickCount` | `osKernelLock/Unlock` | DWT o `osKernelGetSysTimerCount` | `osKernelGetTickCount()·1e6/osKernelGetTickFreq()` | `osDelay(0)` |
| Bare-metal | por defecto | `__disable_irq/__enable_irq` con guarda de anidamiento | DWT/SysTick | `HAL_GetTick()·1000` | `__WFI()` |

Detalles que importan:

- **DWT** no existe en Cortex-M0/M0+ (STM32F0, L0, G0, C0). El puerto lo detecta
  (`__CORTEX_M >= 3`) y cae a SysTick/`HAL_GetTick()`; el `CYCCNT` se habilita
  una vez (`DEMCR.TRCENA`, `DWT->CTRL |= CYCCNTENA`).
- `HAL_GetTick()` **envuelve a los 49,7 días** con tick de 1 ms; se documenta
  porque `mfs_port_time_us` es un contador de 32 bits que ya envuelve cada
  ~71,6 min.
- La sección crítica con `__disable_irq` **no es anidable** por sí sola; se
  implementa con profundidad y guardado del estado `PRIMASK` (mismo patrón que
  `src/core/mfs_port_rtos.c:208-227` usa para Zephyr).
- Objetivo de latencia ≤ 1 µs (§20.3): `__disable_irq`/`PRIMASK` cumple en
  Cortex-M.

### 5.8 Integración con CubeMX / CubeIDE / CubeCLT

El punto crítico es **sobrevivir a la regeneración del `.ioc`**: CubeMX
reescribe `Core/Src`, `Core/Inc`, `.cproject` y el `Makefile` generado. Por eso
el port se entrega como **código fuera del árbol generado** y se integra por una
de estas dos vías, ambas estables frente a la regeneración:

1. **CubeIDE (Makefile generado).** El `Makefile` de CubeIDE permite
   `include` de fragmentos en `Core/Src/subdir.mk`. Se entrega
   `matrixfs_stm32.mk` que añade las fuentes, los *include dirs* y los `-D`
   necesarios. Se documenta **añadir una sola línea** al `Makefile` raíz
   (`-include ../MatrixFS/platform/stm32cube/matrixfs_stm32.mk`) que CubeMX no
   toca porque solo regenera los `subdir.mk` de `Core/`.
2. **CubeCLT / CMake (recomendado para CI).** `add_subdirectory()` del port y
   enlace de `matrixfs_stm32` como librería estática, con
   `target_link_libraries(app PRIVATE matrixfs_stm32)`.

Se entrega además:

- `mfs_stm32_conf_template.h` → copiar a `Core/Inc/mfs_stm32_conf.h` (zona de
  usuario que CubeMX **no** regenera si se marca como tal; se documenta);
- `linker/matrixfs_region.ld` con los `ASSERT` de no solapamiento;
- un ejemplo mínimo `examples/` con el fragmento de `main.c` a insertar tras
  `MX_*_Init()`;
- una tabla de los `HAL_*_MspInit` y periféricos que hay que habilitar en CubeMX
  (OSPI/QSPI o SPI + GPIO, I²C, SDMMC, y `DWT` si se quiere `cycles` real).

### 5.9 Matriz de verificación por familia

No se puede probar en silicio aquí. La estrategia es **probar la lógica contra
un modelo fiel de la flash**, que es donde están los errores:

| Comportamiento a probar | Cómo |
|---|---|
| RMW de 8 B con ECC | Shim que rechaza reprogramar una doubleword y devuelve error de ECC si queda a medias |
| Escritura de 32 B no alineada | Igual, forzando `addr % 8 != 0` |
| `erase(addr)` sobre sectores no uniformes | Tabla F4 real (16/64/128 KB) y verificación de que borra el tramo completo |
| Ventana uniforme | Test unitario de `mfs_sector_uniform_window()` con tablas F1/F4/L0/H7 |
| Troceado por página del NOR | Shim que **envuelve** dentro de la página (como el silicio) y comprueba el dato |
| Bloqueo por RWW | Se documenta; no simulable |

---

## 6. Plan de implementación

| Fase | Contenido | Criterio de aceptación |
|---|---|---|
| **F0** | Capa compartida: `program_granularity` (S1), `mfs_sectors` (S2), S3–S6 en `mfs_l2_8bit` | La suite existente sigue en verde; nuevos tests unitarios de sectores y page-split |
| **F1** | ESP-IDF: enlace (E1), registro (E2/E3), Kconfig (E9), thread-safety (E4) | El componente compila y enlaza contra el núcleo vía shim; monta, escribe y lee |
| **F2** | ESP-IDF: VFS POSIX (E5) | `fopen`/`fwrite`/`stat`/`opendir` funcionan sobre el volumen montado |
| **F3** | STM32Cube: puerto §20.2 + API pública + región/ventana | Compila contra shims de HAL; monta sobre flash simulada |
| **F4** | STM32Cube: flash interna (ECC, RMW, borrado por tramo) | Pasa los tests de §5.9, incluido el rechazo tipificado |
| **F5** | STM32Cube: OSPI/QSPI NOR | Pasa page-split y JEDEC; lectura indirecta correcta |
| **F6** | STM32Cube: SPI NOR, FRAM, EEPROM (SPI e I²C), SD/eMMC | Cada medio monta, escribe y relee en el modelo |
| **F7** | Integración CubeIDE/CubeCLT, linker, ejemplo, conf | `matrixfs_stm32.mk` y `CMakeLists.txt` completos; ejemplo compilable |
| **F8** | Documentación y estado honesto | `platform/stm32cube/README.md`, actualización de `DOCS/embedded-integration.md` y `known-limitations.md` |
| **F9** | CI | Job opcional de `idf.py build` (imagen Espressif) y de host-shims en cada push |

---

## 7. Verificación sin los SDK

El problema de fondo del estado actual («no compilado con los SDK») es que sin
verificación, E1 y S3 son invisibles. La solución que se implementa es
**compilar el código de integración real contra shims mínimos de SDK y
ejecutarlo en host**:

```
platform/esp-idf/test/shims/esp_partition.h   ← esp_partition_* sobre RAM
platform/esp-idf/test/shims/FreeRTOS.h        ← taskENTER_CRITICAL, mutex no-op
platform/esp-idf/test/shims/esp_timer.h, esp_log.h, esp_cpu.h, sdkconfig.h
platform/stm32cube/test/shims/stm32_hal.h     ← HAL_FLASH/OSPI/SPI/I2C/SD
```

Características que hacen útil el shim (si no, no prueba nada):

- el shim de `esp_partition` **respeta** las reglas reales: rechaza escrituras
  fuera de rango, y en modo «cifrado» **rechaza** longitudes no múltiplas de 16;
- el shim de flash STM32 **respeta** NOR (1→0), ECC (prohíbe reprogramar la
  unidad), unidades de 8/16 B y sectores no uniformes;
- el shim de NOR SPI **envuelve** dentro de la página, como el silicio, para que
  S3 se manifieste si se reintroduce;
- las suites se integran en `make test` / CTest como objetivos opcionales, de
  modo que un cambio en la capa compartida que rompa un port **falla la suite**.

Esto no sustituye la validación en silicio (que sigue pendiente y así se
declara), pero convierte «no verificado» en «verificado en host, pendiente en
silicio» — que es una afirmación mucho más útil.

---

## 8. Alcance y estado honesto

**Verificado en host (ejecutado, no solo compilado):**

| Suite | Resultado |
|---|---|
| Núcleo (`make test`) | **1232 checks / 0 fallos** (sin regresiones) |
| Núcleo con `-Werror` (`make strict`) | Compila limpio |
| ESP-IDF (`platform/esp-idf/test/`) | **77 checks / 0 fallos** — formateo, montaje, escritura con offset no alineado, partición **cifrada** (16 B), VFS POSIX completo, y recursividad del mutex |
| STM32Cube, unidad 2 B sin ECC (F0/F1) | **90 checks / 0 fallos** |
| STM32Cube, unidad 4 B sin ECC (F4/F7) | **90 checks / 0 fallos** |
| STM32Cube, unidad 8 B **con ECC** (L4/G4/G0) | **95 checks / 0 fallos** |
| STM32Cube, unidad 16 B **con ECC** (H5/H7/U5) | **103 checks / 0 fallos** |

Todo ello se ejecuta con `make platform-test`. Los *shims* no son complacientes:
el de `esp_partition` modela NOR (`old & new`), exige múltiplos de 16 B con
cifrado y rechaza escrituras fuera de rango; el de la flash STM32 modela la
unidad de programación, la ECC (prohíbe reprogramar una unidad) y sectores **no
uniformes** (16/64/128 KB).

**Defectos que este banco de pruebas encontró (y que la inspección no vio):**

| Defecto | Cómo se manifestó |
|---|---|
| `src/core/mfs_arch.c` fuera del build de ESP-IDF | Error de enlace al compilar el componente — **E1** |
| Desbordamiento de buffer de 16 B en el backend de flash interna (`uint64_t` no puede llevar una quadword) | `-Werror=array-bounds` al compilar el modelo de host |
| Lectura de la flash por puntero directo a `0x0800xxxx` | Fallo de acceso al montar en host: el backend no era verificable sin silicio |
| `ESP_VFS_FLAG_DEFAULT` con callbacks `*_p` en la capa VFS | Detectado al verificar la forma real de `esp_vfs_t` (unión de dos juegos de punteros) — **E11** |
| El test de cifrado **no** discriminaba 4 B de 16 B | Se comprobó parcheando una copia temporal: la suite pasaba igual ⇒ la afirmación inicial de E6 era falsa y se corrigió |

**No verificado (requiere SDK y hardware):**

- `idf.py build` con ESP-IDF real y ejecución en ESP32 o QEMU. El *shim* no
  reproduce el planificador de IDF, la tabla de particiones real ni el motor de
  cifrado de flash. (QEMU: soporte oficial, pero en v5.3 solo `esp32` y
  `esp32c3`; en v6.x se añade `esp32s3`.)
- Las firmas exactas de `esp_vfs_t` en cada versión menor de IDF 5.x.
- Compilación contra el HAL real de cada familia STM32: las firmas de
  `HAL_FLASH_Program`, los campos de `FLASH_EraseInitTypeDef` y la selección de
  banco varían. Todo se aísla en `src/mfs_stm32_hal.{h,c}` para que el ajuste sea
  de un solo fichero.
- La programación de **16 B (quadword)** en H5/H7/U5: al no poder verificar la
  secuencia de registros de esas familias, el port define un **gancho débil** que
  por defecto devuelve `MFS_ENOTSUP`. Es decir, **rechaza** programar de 16 en 16
  bytes en silicio en vez de truncar 8 bytes en silencio. El integrador de esas
  familias debe implementarlo; el banco de host lo implementa y prueba.
- Las tablas de sectores de `mfs_stm32_flash_map.c` reproducen la documentación
  de ST y **deben contrastarse con el Reference Manual** de la variante; la vía
  siempre correcta es aportar la tabla en `cfg.sectors`, y la validación más el
  `ASSERT` del linker convierten un error de tabla en un fallo detectable.
- Tiempos reales de borrado, comportamiento real de la ECC, penalización RWW y
  coherencia de caché del Cortex-M7 con el DMA de SDMMC.

**Riesgos residuales conocidos:**

- La ventana uniforme puede **descartar** parte de una región reservada si se
  define cruzando un cambio de tamaño de sector. El port lo reporta
  (`dropped_bytes`, `capacity_wasteful()`), no lo oculta.
- El tope de `MFS_ZONE_MAX = 128` limita el volumen útil a
  `128 × erase_unit`: 512 KB con sectores de 4 KB, 16 MB con sectores de 128 KB.
- La flash interna en familias *single-bank* introduce *jitter* de decenas o
  cientos de milisegundos en el GC. Es una propiedad del silicio.
- Bibliografía de la verificación de API: cabeceras y fuentes de ESP-IDF v5.0,
  v5.3, v5.5 y master, y documentación oficial de Espressif (build system,
  particiones, VFS, esp_timer, QEMU). Los puntos que no se pudieron confirmar se
  marcaron como no verificados en lugar de asumirse.

---

## 9. Anexo — Trazabilidad de los hallazgos

| Hallazgo | Evidencia en el repositorio |
|---|---|
| E1 | `platform/esp-idf/CMakeLists.txt:55-68`; `src/core/mfs_hal.c:62`; `src/core/mfs_util.c:143`; defs `src/core/mfs_arch.c:102,211` |
| E2 | `platform/esp-idf/CMakeLists.txt:73-75`; `platform/esp-idf/include/matrixfs_esp.h:34-35` |
| E3 | `platform/esp-idf/CMakeLists.txt:40-68`; `platform/esp-idf/README.md:40-64,94-100` |
| E4 | `DOCS/known-limitations.md:72-77`; `platform/esp-idf/matrixfs_esp.c:338-340` |
| E5 | `platform/esp-idf/include/matrixfs_esp.h:53-75` |
| E6 | `platform/esp-idf/matrixfs_esp.c:129-142`; `platform/embedded/mfs_embedded.c:92` |
| E7 | `platform/esp-idf/matrixfs_esp.c:86,91` |
| E8 | `platform/esp-idf/matrixfs_esp.c:82-83` |
| E9 | `platform/esp-idf/matrixfs_esp.c:56,215-221`; `include/matrixfs/mfs_types.h:218` |
| S1 | `platform/embedded/mfs_embedded.h:24-48`; `platform/embedded/mfs_embedded.c:92` |
| S2 | `src/mfs_internal.h:27-36`; `src/core/mfs_hal.c:104-107` |
| S3 | `platform/common/mfs_l2_8bit.c:114-159`; `src/core/mfs_zone.c:487` |
| S4 | `platform/common/mfs_l2_8bit.h:23-25,44-47`; `mfs_l2_8bit.c:247-248,98-99,138-139` |
| S5 | `platform/common/mfs_l2_8bit.c:192-240` |
| S6 | `platform/common/mfs_l2_8bit.h:37-38`; `mfs_l2_8bit.c:108,148` |
| T2 | `src/core/mfs_fs.c:120`; `src/core/mfs_zone.c:249,487`; `src/core/mfs_wal.c:175` |
| T3 | No hay tabla de particiones en STM32Cube; requiere linker + `ASSERT` |
