# HAL: detección, HWV, viabilidad y bus

Spec: §5, §6, §14.2, §20.2–§20.4, §25.
Implementación: `src/core/mfs_hal.c`; contrato de puerto
[`include/matrixfs/mfs_port.h`](../include/matrixfs/mfs_port.h).

El núcleo **no habla nunca con el controlador de memoria** (§2 P4): todo el
acceso pasa por el driver L2 (`mfs_l2_driver`) y por la cascada de detección
delegada al BSP. Esta capa resuelve tres cosas: *qué medio hay*, *qué modos son
viables* y *a qué velocidad se puede leer el bus* 

## Cascada de detección (§5.1)

`mfs_hal_detect(mf_t *fs, const mfs_config *cfg, mfs_hwv_t *out)` ejecuta las
fases en este orden estricto y con esta degradación:

| Fase | Fuente | Efecto |
|---|---|---|
| 1–3 | `cfg->geom` (estática) | si está presente, es la geometría autoritativa: **no** se sondea el medio |
| 1–3 | `g_hal->jedec_id()` (`0x9F`) | rellena `hw_version = (id[1] << 8) \| id[2]`; `id[0] == 0xFF` ⇒ chip ausente |
| 1–3 | `g_hal->sfdp_parse()` (`0x5A`) | completa la geometría; marca `have_geom` |
| 4–7 | defaults §25 | rellena huecos de la geometría (tabla siguiente) |
| 8 | `g_hal->assets()` | `flags2`/`flags3` (PUF, ADC de rail, supercap, TrustZone, aceleradores) |
| 9 | `g_hal->bad_block_probe()` | prueba activa limitada (bloque defectuoso), sólo si el BSP la ofrece |
| — | `cfg->drv_t0 && cfg->geom_t0` | HMT: `t0_size = geom_t0->size`, `t0_write_ns = t_prog_max_us · 1000` |

Sin geometría estática ni driver no hay nada que detectar ⇒ `MFS_EINVAL`.

Los cuatro ganchos del BSP se registran con `mfs_set_hal_ops()` y son opcionales:
si el integrador no los aporta, la detección se apoya en `cfg->geom`.

### Defaults de geometría (§25)

| Campo | Valor por defecto | Motivo |
|---|---|---|
| `type` | `MFS_MEDIA_NOR_SPI` | perfil de referencia |
| `erase_unit` | `4096` (mínimo forzado 1024) | sector NOR mínimo |
| `page_size` | `256` | página de programación |
| `program_granularity` | `1` | NOR programable byte a byte |
| `t_prog_max_us` / `t_erase_max_us` / `t_read_max_us` | `700` / `45000` / `100` | presupuestos de datasheet (los `T_max` se respetan, §2 P5) |

Un valor de `erase_unit` distinto de cero y menor que 1024 se eleva a 4096: un
bloque menor no permite alojar el sector 0 (SB A + HWV) y rompería el layout.

## HWV — vector de hardware (§5.2)

`mfs_hwv_t` es la **foto inmutable** del hardware que el montaje hace cumplir.
Vive en flash (@512, 64 B LE) y en RAM sólo se guarda un *handle* de 8 B
(`mfs_hwv_handle_t { hwv_flash_addr, hwv_crc }`, MFS-HWV-001).

| Orden | Campo | Tipo | Nota |
|---|---|---|---|
| 1 | `magic[4]` | `'M','H','W','V'` | |
| 2 | `hw_version` | u16 | del JEDEC |
| 3 | `arch_class` | u8 | 1 = 16 bit, 2 = 32 bit; **0 ⇒ `MFS_EARCH`** |
| 4 | `mode_forced` | u8 | `0xFF` = auto (§6.2) |
| 5 | `ram_total` | u32 | RAM física declarada |
| 6 | `bus_speed_hz` | u32 | `min(MCU, flash)`, sólo medida por lectura |
| 7–10 | `t_prog_max_us, t_erase_max_us, t_read_max_us, t_suspend_max_us` | u32 ×4 | |
| 11–12 | `program_granularity, erase_unit` | u32 ×2 | |
| 13 | `base_reserved_off` | u32 | tras SB A/B + HWV: inicio de la zona de tokens |
| 14 | `oob_bytes` | u16 | |
| 15–18 | `flags0..flags3` | u8 ×4 | capacidades (tabla siguiente) |
| 19–20 | `t0_size, t0_write_ns` | u32 ×2 | tiering HMT (§11.11) |
| 21 | `profile_id` | u32 | perfil firmado (`mfstool sign-profile`) |
| 22–23 | `media_type, media_size` | u32 ×2 | extensión: medio T1 real |
| 24 | `crc` | u32 | CRC-32C del cuerpo |

Capacidades declaradas (§5.2):

| Vector | Bits |
|---|---|
| `flags0` | `SUSPEND_E 0x01` · `SUSPEND_P 0x02` · `MULTI_PLANE 0x04` · `ECC_ON_DIE 0x08` |
| `flags1` | `BYTE_ADDR 0x01` · `MANAGED 0x02` · `PPP 0x04` · `SLC 0x08` · `ASYM 0x10` · `SE 0x20` |
| `flags2` | `PUF 0x01` · `ADC_RAIL 0x02` · `SUPERCAP 0x04` · `TRUSTZONE 0x08` |
| `flags3` | `CRYPTO_HW 0x01` · `ASCON_HW 0x02` · `B3_HW 0x04` · `CQE 0x08` · `ZNS 0x10` · `DMA_CRC 0x20` |

Dos normalizaciones que hace la detección:

- **Suspend**: si el driver no aporta `suspend`, se **limpia** `SUSPEND_E` —
  declarar una capacidad que no se puede ejecutar es peor que no declararla
  (los subsistemas de FTL 2 la usarían y fallarían en tiempo de ejecución).
- **Serie de utilidades**: `mfs_hwv_serialize/deserialize` (64 B) y
  `mfs_hwv_validate` (magic + CRC + `arch_class`, §24.2 paso 2). Toda lectura
  on-flash usa los accesores LE `mfs_ld16/ld32/ld64` (§20.4).

## Modo y viabilidad (§6.1)

`mfs_select_mode(hwv, cfg)` aplica primero el manifiesto y después el reparto de
presupuesto RAM (MFS-VIA-002):

```
firmware 50 %   pila 15 %   periféricos 8 %   margen 10 %   ⇒   disponible 17 %
```

Cada término puede fijarse en **valor absoluto** con los campos
`ram_firmware_min`, `ram_stack_min`, `ram_peripherals`, `ram_margin`; a cero se
derivan porcentualmente de `ram_total`. El margen nunca baja del 10 %.

- Si `fw + pila + perif + margen > ram_total` ⇒ `MFS_MODE_UNSUPPORTED`
  (equivalente semántico a `MFS_ENOTVIABLE`).
- Se elige el **mayor** modo cuyo requisito de RAM quepa en el disponible
  (descendente de Extended a Ultra-Nano).

| Modo | RAM | Chunk |
|---|---|---|
| `MFS_MODE_ULTRA_NANO` | 720 B | 128 B |
| `MFS_MODE_NANO` | 1536 B | 256 B |
| `MFS_MODE_COMPACT` | 3584 B | 512 B |
| `MFS_MODE_BALANCED` | 11520 B | 4096 B |
| `MFS_MODE_EXTENDED` | 21504 B | 4096 B |

El modo fija el tamaño de página del layout: **la imagen de un volumen no es
intercambiable entre modos**.

`mode_forced` (manifiesto) tiene prioridad sobre el cálculo: es la vía para
fijar el layout de un producto y garantizar que dos equipos con la misma imagen
eligen chunk idéntico.

## Negociación de suite (§10.6)

`mfs_negotiate_suite(hwv, cfg, mode)` decide la suite criptográfica **antes** de
montar, de modo que el layout (tamaño de tag) queda fijado por la configuración:

1. Ultra-Nano ⇒ `MFS_SUITE_NONE` (no hay presupuesto para AEAD).
2. Sin clave (`cfg->key == NULL`) ⇒ `MFS_SUITE_NONE`.
3. Preferencia explícita (`suite_preferred != 0xFF`): S1 sin
   `CRYPTO_HW`/`B3_HW` y S2 sin `ASCON_HW`/`B3_HW` **degradan a S3**.
4. Automático: `CRYPTO_HW && B3_HW` ⇒ S1; `ASCON_HW || B3_HW` ⇒ S2; si no, S3
   (implementación software determinista).

## Medición de bus de sólo lectura (§14.2)

`mfs_bus_measure_hz(drv, ctx)` — MFS-BUS-001: una lectura de calentamiento y
ocho lecturas de 64 B desde la dirección 0, cronometradas con
`mfs_port_cycles()`. Devuelve una tasa relativa (`512·10⁶ / (ciclos·8)`), o `0`
si cualquier lectura falla o el contador no avanza.

**No hay ninguna operación de escritura ni de borrado**: medir el bus nunca
puede alterar el medio. Con `cfg->bus_speed_hz != 0` se respeta el valor
aportado y no se mide.

## EDP (§12.3)

`mfs_edp_capable(fs)` es la puerta que habilita el *Emergency Data Path*:
verdadero sólo si el HWV declara `ADC_RAIL` o `SUPERCAP` (o si el hook de
prueba `g_mfs_edp_inject` está activo). Sin medida de rail ni supercap no hay
forma determinista de saber cuánta energía queda, y EDP no debe armarse.

## Puertas y errores

| Situación | Resultado |
|---|---|
| Sin geometría estática y sin driver | `MFS_EINVAL` |
| `arch_class == 0` | `MFS_EARCH` (al validar el HWV, §24.2 paso 2) |
| Presupuesto de viabilidad insuficiente | `MFS_MODE_UNSUPPORTED` |
| Driver sin `suspend` | se limpia `SUSPEND_E` (no es error) |
| Lectura de bus fallida o contador parado | `mfs_bus_measure_hz` ⇒ `0` |

## Diagnóstico

```c
const mfs_hwv_t *h = mf_last_hwv();   /* último HWV detectado */
mfs_mode_t m = mf_last_selected();    /* último modo elegido */
```

`mfstool verify-hwv <imagen>` valida el HWV de un volumen ya formateado
(magic + CRC-32C + arquitectura) sin montarlo; véase
[`tooling.md`](tooling.md).

## Soporte de MCU de 8 bits (MFS-ARCH-010 rev. 2)

La revisión 2 de MFS-ARCH-010 **admite** arquitecturas de 8 bits como clase
válida (`arch_class == 0`), además de 16 bits (1) y 32 bits (2). Sólo se rechaza
una clase desconocida (`> 2`) con `MFS_EARCH`. Un volumen formateado en un MCU
de 8 bits es por tanto montable en cualquier otra plataforma (portabilidad).

**Selección de modo** (`mfs_select_mode`): si `arch_class == 0`, el selector
prefiere la familia 8-bit antes que los modos clásicos:

| Modo 8-bit | RAM | Chunk | Pila propia | Ficheros | Snapshots |
|---|---|---|---|---|---|
| `MFS_MODE_8BIT_ULTRA` | 512 B | 64 B | 32 B | 1 | 0 |
| `MFS_MODE_8BIT_NANO` | 1 KB | 128 B | 48 B | 2 | 1 |
| `MFS_MODE_8BIT_COMPACT` | 2 KB | 256 B | 64 B | 4 | 2 |

Los modos 8-bit forman una **familia independiente**: su numeración en
`mfs_mode_t` no es mayor que `Extended`, por lo que nunca deben compararse
directamente. Para ello `mfs_types.h` expone `mfs_mode_is_8bit()`,
`mfs_mode_is_classic()` y `mfs_mode_classic_ge()`, que es lo que usa el núcleo.

**Suites**: en 8-bit (y Ultra-Nano) la negociación devuelve `MFS_SUITE_NONE`; la
integridad la cubre **CRC-32C**. Para no consumir RAM, el build 8-bit usa una
tabla de *nibble* (16 × 4 B, en `.rodata`) en lugar de la tabla completa de
256 entradas (1 KiB en `.bss`), con resultado idéntico bit a bit
(KAT: `CRC32C("123456789") == 0xE3069283`).

### Capa de plataforma (`platform/8bit/`)

| Fichero | Contenido |
|---|---|
| `mfs_port_8bit.h/.c` | Primitivas de puerto (crítica, ciclos, tiempo, WFI) por arquitectura: **AVR, 8051, STM8, PIC16/18, Z80** y fallback C genérico. La arquitectura se autodetecta por macros del compilador. |
| `mfs_l2_8bit.h/.c` | Drivers L2 para **NOR SPI, FRAM SPI/I2C, EEPROM SPI/I2C, flash interna MCU y SD en modo SPI**, con barrera WOB y timeouts acotados. Autodetección JEDEC (`0x9F`). |
| `mfs_detect_8bit.h/.c` | **Autodetección de capacidades** del MCU (RAM, flash, EEPROM, CRC/​RNG HW, periféricos) y **autoadaptación** de `mfs_config` (arch_class, modo, suite, velocidad de bus). Toda capacidad no confirmada se declara ausente (fallback software). |

El puerto 8-bit registra las primitivas obligatorias del contrato de puerto
(§20.2) cuando se compila con `MFS_8BIT_PORT_GLUE=1` (build de target); en host
las aporta `sim/mfs_port_host.c`.

### Build de 8 bits y mínima RAM

La capa 8-bit se compila con `MATRIXFS_BUILD_8BIT=ON` (CMake) o `make 8bit`.
Para cross-compilar el núcleo a un MCU de 8 bits hay que definir
`MFS_ALLOW_8BIT_TARGET=1` (`MFS_IS_8BIT_TARGET`); a partir de ahí el core
dimensiona sus pools y buffers **al mínimo** (§18.2, MFS-RES-001), de modo que
`sizeof(mf_t)` baja de ~17 KB (16/32-bit) a < 2 KB:

| Recurso | 16/32-bit | 8-bit |
|---|---|---|
| `mf_t` (pools estáticos) | ~17 KB | < 2 KB |
| Ventana de inodos | 48 | 4 |
| Zonas en RAM | 128 | 16 |
| Ventana WAL | 256 | 16 |
| Scratch de página (`MFS_SCRATCH_MAX`) | 4096 B | 256 B |

Los subsistemas pesados y opcionales (PQ/LMS, SDP, ZRP, dedup, CDC) no se
anuncian en los modos 8-bit; un build 8-bit puede excluir sus unidades de
traducción (p. ej. `src/sec/mfs_pq.c`, sin dependencias desde el núcleo).
