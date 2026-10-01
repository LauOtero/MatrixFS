# Integración embebida: MCU de 32 bits, 8 bits y ecosistemas

Spec: §20 (contrato de puerto), §21 (API), §22 (layout), §5–§6 (detección y
viabilidad), MFS-ARCH-010 rev. 3.

Este documento describe cómo se integra MatrixFS Ultra en
microcontroladores y en los ecosistemas de desarrollo más usados. El núcleo no
tiene dependencias de plataforma: **todo el acoplamiento pasa por el contrato de
puerto (§20.2) y el driver L2 (§20.3)**, que aporta la integración.

## Modelo de integración

```
   Aplicación (Arduino / ESP-IDF / MicroPython / firmware propio)
        │
        │  API pública  include/matrixfs/matrixfs.h  +  src/mfs_internal.h (mf_t)
        ▼
   matrixfs_embedded  (platform/embedded/)   ── adapta callbacks del SDK a L2
        │            · mfs_embedded_flash_t   (read/prog/erase + geometría)
        │            · mfs_embedded_mount/format/setup
        ▼
   Núcleo MatrixFS (src/core, crypto, ftl, tier, sec, xio)
        │
        │  mfs_port_crit_enter/exit · cycles · time_us · wfi   (los aporta la integración)
        ▼
   Región de flash reservada (partición esp_partition, offset de flash, XIP…)
```

Toda integración debe aportar:

1. **Primitivas de puerto obligatorias** (§20.2/§20.3): `mfs_port_crit_enter`,
   `mfs_port_crit_exit`, `mfs_port_cycles`, `mfs_port_time_us`, `mfs_port_wfi`.
   Deben compilarse **una sola vez** por binario.
2. **Tres callbacks de flash** (`read` / `prog` / `erase`) sobre una región
   plana, con la semántica NOR de MatrixFS (`prog` sólo aclara bits 1→0 y
   retorna tras verificar el estado: barrera WOB).

La instancia del núcleo (`mf_t`) es **opaca** en la API pública; para declararla
hay que incluir `src/mfs_internal.h` (añadir `src/` al *include path*), igual
que hace [`platform/common/mfs_vfs.c`](../platform/common/mfs_vfs.c). El núcleo
es de **instancia única** (estado global de pools y L2P): una sola instancia
montada a la vez.

## Capa común `platform/embedded/`

| Símbolo | Descripción |
|---|---|
| `mfs_embedded_flash_t` | Región de flash: callbacks del SDK, geometría (`base_addr`, `size`, `erase_unit`, `page_size`, `t_*_max_us`, `no_erase`) y estado rellenado por `setup`. |
| `mfs_embedded_opts` | Presupuesto RAM, `arch_class`, modo forzado, clave, suite, permisos, `format_if_needed`. |
| `mfs_embedded_opts_default()` | Rellena opciones seguras (32 KB RAM, `arch_class=2`, negociar suite). |
| `mfs_embedded_setup()` | Construye `drv` + `geom` + `mfs_config` a partir de la región. Idempotente. |
| `mfs_embedded_mount()` | `mf_init`; si `format_if_needed` y el volumen no es válido, formatea y reintenta. |
| `mfs_embedded_format()` | Borra SB/HWV/zonas y crea la raíz. |

Ejemplo canónico (equivalente al usado en los envoltorios):

```c
#include "mfs_embedded.h"
#include "mfs_internal.h"   /* mf_t */

static mfs_embedded_flash_t flash;   /* estado persistente del montaje */
static mf_t fs;

mfs_embedded_opts o;
mfs_embedded_opts_default(&o);
o.ram_total = 32u * 1024u;
o.format_if_needed = true;

flash.read  = mi_flash_read;   /* mfs_st (*)(void*, uint32_t, void*, uint32_t)   */
flash.prog  = mi_flash_prog;   /* retorna tras verificar estado (WOB)           */
flash.erase = mi_flash_erase;  /* borra el bloque que contiene addr             */
flash.ctx   = &mi_dispositivo;
flash.base_addr = 0x00100000u; /* dirección absoluta en el mapa del MCU        */
flash.size      = 0x00100000u; /* 1 MiB                                        */
flash.erase_unit = 4096u;
flash.page_size  = 256u;

if (mfs_embedded_mount(&fs, &flash, &o) != MFS_OK) { /* error tipificado */ }
```

## Arquitecturas 8/16/32/64 bits (integradas en el núcleo)

El puerto (§20.2) y el modelo de arquitectura son parte del **núcleo**, común a
todas las clases:

| Fichero | Contenido |
|---|---|
| `src/core/mfs_port_arch.{h,c}` | Puerto por arquitectura (crítica, ciclos, tiempo, WFI) — **AVR, 8051, STM8, PIC16/18, Z80** y *fallback* C genérico (16/32/64 bits). Autodetectado por macros del compilador. |
| `src/core/mfs_arch.c` | Clasificación de arquitectura (8/16/32/64-bit o `MFS_ARCH_AUTO`) y **detección/autoconfiguración de aceleración por hardware**. Tipos en `include/matrixfs/mfs_types.h` (`mfs_arch_info_t`, `MFS_HWACCEL_*`); API en `src/mfs_internal.h`. |
| `platform/common/mfs_l2_8bit.{h,c}` | Drivers L2 de dispositivo para MCU: **NOR SPI, FRAM SPI/I²C, EEPROM SPI/I²C, flash interna y SD-SPI**, con barrera WOB y timeouts acotados. Autodetección JEDEC (`0x9F`). |

**Clases soportadas** (`arch_class`): `0`=8-bit, `1`=16-bit, `2`=32-bit, `3`=64-bit
y `MFS_ARCH_AUTO` (0xFF) = autodetectar. La clase gobierna la familia de modos
(8-bit Ultra/Nano/Compact vs clásica Ultra-Nano…Extended) y el reparto de RAM.

**Autoconfiguración** (`mfs_arch.c`): clasifica la arquitectura, detecta
capacidades (`MFS_HWACCEL_CRC32C/AES/SHA256/CLMUL/SIMD/RNG/ATOMICS`) y ajusta la
clase, el presupuesto de RAM (8-bit) y la velocidad de bus. La **única ruta
acelerada efectiva hoy es CRC-32C** (x86 SSE4.2 / ARMv8 CRC32, resultado idéntico
a la tabla); el resto se reporta para diagnóstico y no se declara en el HWV,
porque el HWV sólo anuncia lo que el núcleo puede ejecutar (MFS-HW-001).

**Modos 8-bit** (familia independiente, elegidos automáticamente si
`arch_class == 0`):

| Modo | RAM | Chunk | Ficheros | Snapshots |
|---|---|---|---|---|
| `MFS_MODE_8BIT_ULTRA` | 512 B | 64 B | 1 | 0 |
| `MFS_MODE_8BIT_NANO` | 1 KB | 128 B | 2 | 1 |
| `MFS_MODE_8BIT_COMPACT` | 2 KB | 256 B | 4 | 2 |

En 8-bit no hay AEAD (`MFS_SUITE_NONE`): la integridad la cubre **CRC-32C**. Para
no gastar RAM, el build 8-bit usa una tabla de *nibble* (16 × 4 B en `.rodata`)
en lugar de la tabla completa de 256 entradas (1 KiB en `.bss`), con resultado
idéntico bit a bit (`CRC32C("123456789") == 0xE3069283`).

### Build 8-bit y mínima RAM

Definir `MFS_ALLOW_8BIT_TARGET=1` (⇒ `MFS_IS_8BIT_TARGET`) hace que el núcleo
dimensione pools y *scratch* al mínimo (§18.2, MFS-RES-001):

| Recurso | 16/32-bit | 8-bit |
|---|---|---|
| `mf_t` (pools estáticos) | ≈ 17 KB | ≈ 1.8 KB |
| Ventana de inodos | 48 | 4 |
| Zonas en RAM | 128 | 16 |
| Ventana WAL | 256 | 16 |
| *Scratch* de página (`MFS_SCRATCH_MAX`) | 4096 B | 256 B |

Compilación de la capa 8-bit como librería:

```bash
make mcu                          # libmatrixfs_mcu.a (drivers L2 de MCU)
cmake -DMATRIXFS_BUILD_MCU=ON ..  # equivalente
```

Los subsistemas pesados y opcionales (PQ/LMS, SDP, ZRP, dedup, CDC) no se
anuncian en los modos 8-bit; un build 8-bit puede excluir sus unidades de
traducción (p. ej. `src/sec/mfs_pq.c`, sin dependencias desde el núcleo).

## Ecosistemas de 32 bits

### Arduino — `platform/arduino/`

Librería C++ (`library.properties` + `library.json`) con la clase `MatrixFS`,
que resuelve la región de flash (partición `esp_partition` en ESP32, offset de
flash en ESP8266, XIP en RP2040), aporta las primitivas de puerto y delega en el
núcleo. Ejemplo en `examples/Basic/Basic.ino`.

```cpp
#include <MatrixFS.h>
MatrixFS fs("matrixfs");                 // etiqueta de partición (ESP32)
void setup() {
  Serial.begin(115200);
  if (!fs.begin(/*formatIfNeeded=*/true)) Serial.println(fs.lastErrorString());
  fs.writeFile("/hola.txt", "hola", 4);
}
```

`begin()` de una segunda instancia devuelve `MFS_EBUSY` (instancia única del
núcleo). Detalles y particiones de ejemplo en
[`platform/arduino/README.md`](../platform/arduino/README.md).

### ESP-IDF — `platform/esp-idf/`

Componente externo (`CMakeLists.txt` + `idf_component.yml` + `Kconfig`).
Traduce `esp_partition_read/write/erase_range` a los callbacks del núcleo
(escritura alineada con RMW) y aporta las primitivas de puerto con
`portMUX`/`esp_timer`.

API: `matrixfs_esp_mount()`, `matrixfs_esp_format()`,
`matrixfs_esp_mount_default()` (usa `Kconfig`), `matrixfs_esp_last_error()`.
Opciones de `Kconfig`: etiqueta de partición, presupuesto RAM, formateo si hace
falta y modo forzado. Detalles en
[`platform/esp-idf/README.md`](../platform/esp-idf/README.md).

### PlatformIO — `platform/platformio/`

Proyecto de ejemplo (`platformio.ini`) que consume la librería de Arduino y
reserva la región con una tabla de particiones propia
(`board_build.partitions`). **No se usa `board_build.filesystem = littlefs`**:
MatrixFS no es LittleFS; aporta su propio *layout* on-flash y su propia
partición. Detalles en
[`platform/platformio/README.md`](../platform/platformio/README.md).

### MicroPython — `platform/micropython/`

*Usermod* (`micropython.mk` + `mpconfigport_fragment.h`) que expone el módulo
`matrixfs` con dos clases:

```python
import matrixfs
fs = matrixfs.MatrixFS("matrixfs", 0x100000)   # etiqueta, tamaño
fs.mount(ram=32768, format=True)
f = fs.open("/hola.txt", "w")
f.write(b"hola"); f.close()
print(fs.listdir("/"))
```

El módulo acepta un objeto *bdev* (protocolo `readblocks`/`writeblocks`/`ioctl`)
o una partición por etiqueta (best-effort). Traduce los errores del núcleo a
`OSError` (`ENOENT`, `EEXIST`, `EINVAL`, `ENOSPC`, `EIO`). No implementa el
protocolo VFS de MicroPython: convive con LittleFS, no lo sustituye en
`os.mount()`. Detalles en
[`platform/micropython/README.md`](../platform/micropython/README.md).

## Estado de verificación

| Componente | Estado |
|---|---|
| Capa `platform/embedded` | ✅ **Verificada en host**: formato + montaje + E/S (`/boot.log`, 23 B) + persistencia tras remontaje sobre `sim/vflash.c`. |
| Arquitectura, 64-bit y aceleración HW (`mfs_arch.c`) | ✅ **Verificados en host**: clase detectada `64-bit`, `crc_hw=1` (SSE4.2) y CRC acelerado idéntico a la tabla y al KAT. |
| Modos 8-bit, detección y comparaciones de familia | ✅ **Verificados en host** por la suite (§27): 1 147 checks / 0 fallos. |
| `mf_t` en build 8-bit | ✅ Medido en host: ≈ 1 840 B ≤ 2 KB. |
| Puerto (`src/core/mfs_port_arch.c`) y drivers MCU (`platform/common/mfs_l2_8bit.c`) | ✅ Compilan sin avisos en host (rama genérica). ⏳ Sin compilar con `avr-gcc`/`sdcc`/`xc8` reales. |
| Arduino / ESP-IDF / PlatformIO / MicroPython | ⏳ Estructura de build y código revisados por inspección estática; **sin compilar con los SDK de terceros** en este repositorio. |
