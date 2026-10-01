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
