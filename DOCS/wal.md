# WAL+, tokens y checkpoint

Spec: §8.4, §9 (§9.1–§9.6), §11.11, §15, §24.1, §24.2, §25.
Implementación: `src/core/mfs_wal.c`.

Toda modificación de metadatos es **transaccional** y se hace visible sólo con
el token de commit. El contrato completo son cuatro invariantes:

| Regla | Semántica |
|---|---|
| **MFS-TX-001** | Ninguna modificación es visible antes del token T1. |
| **MFS-TX-002** | Orden de escritura: `WALENT`(s) → … → T1 `{txid, crc, epoch}`. |
| **MFS-TX-003** | Tras un corte, el montaje sólo acepta entradas con token vigente. |
| **MFS-TX-004** | La ventana WAL en RAM está acotada (`W = 256`): al desbordar se fuerza *checkpoint*; si éste falla ⇒ `MFS_EBACKPRESSURE`. |

## Registro WALENT (§9.1)

Payload de `WALHDR = 12` B más los datos, con relleno `0x5A` hasta el tamaño de
página:

| Offset | Tamaño | Campo |
|---|---|---|
| 0 | 4 | `txid` (LE) |
| 4 | 4 | `lba` lógico |
| 8 | 2 | `len` de datos |
| 10 | 1 | `kind` (`MFS_RT_*`) |
| 11 | 1 | `sp_depth` (savepoint activo) |
| 12 | `len` | datos |
| 12+`len` | resto | relleno `0x5A` |

Se escribe con `mfs_rec_write(..., MFS_RT_WALENT, gen = epoch & 0xF, ...)`, es
decir, **a través de E2G**, de modo que el registro lleva su propia cabecera con
CRC-32C y (si hay suite) su AEAD. El `kind` viaja dentro del WALENT porque la
entrada es genérica: sirve para `INODE`, `DIRENT`, `TXMARK`, `EPOCH`, etc.

### `mfs_wal_append`

1. Sin montaje ⇒ `MFS_ENOTMOUNTED`.
2. `len + 12 > mfs_page_bytes(fs)` ⇒ `MFS_EINVAL` (el registro no puede cruzar
   la página).
3. Contabilidad GLD (§9.4): si `debt_gld >= d_max`, se intenta GC antes de
   escribir para no sellar el volumen por deuda.
4. Si la ventana está llena (`wal_count >= 256`) ⇒ `mfs_checkpoint_write`; si
   éste falla ⇒ `MFS_EBACKPRESSURE` (nunca se pierde la entrada aceptada).
5. Apertura perezosa de la zona WAL: si no hay zona abierta ⇒
   `mfs_zone_alloc_open`. Ante fallo se reintenta con `mfs_gld_maybe_gc` y luego
   con `mfs_gc_force`; agotadas las vías ⇒ `MFS_ENOSPC`.
6. Serializa y escribe; si la zona se sella a media escritura (`MFS_ENOSPC`), se
   abre otra zona y se **reintenta** una vez.
7. Registra en la ventana BMT en RAM
   (`wal[(wal_head + wal_count) % 256]`), con
   `committed = tx_open ? 0 : 1`, y devuelve el `ppage` asignado.

## Anillo de tokens (§9.2)

El sector de tokens ocupa `mfs_tok_off(hwv) = 2 · erase_unit` y aloja un anillo
de **8 slots** de 32 B. La generación se identifica por un contador monótono
persistido (`seq`), no por un contador termométrico en el propio byte: es
semánticamente equivalente y, sobre todo, **NOR-correcto** (véase
[`known-limitations.md`](known-limitations.md)).

### Token T1 (32 B)

| Offset | Tamaño | Campo | Nota |
|---|---|---|---|
| 0–1 | 2 | magic `'T','O'` (`0x544F`) | |
| 2 | 1 | `ver = 1` | |
| 3 | 1 | `flags = 0` | |
| 4 | 4 | `txid` | |
| 8 | 4 | `n` | nº de entradas WAL del `txid` |
| 12 | 4 | `body_crc` | verificación del cuerpo |
| 16 | 4 | `epoch` | época (§15) |
| 20 | 4 | `seq` | contador monótono |
| 24 | 4 | `merkle` | 4 primeros B de la raíz Merkle |
| 28 | 4 | `crc` | CRC-32C de los 28 B previos |

### Token T0 (16 B, HMT §11.11)

`magic(2) | ver(1) | flags(1) | txid(4) | crc(4) | seq(4)`, escrito
**byte-directo** (sin borrado) en el tier T0 de `t0_size` bytes.

### `mfs_token_commit`

1. Compone T1, cuenta `n` (entradas válidas de la ventana con ese `txid`) y
   calcula el CRC del token.
2. **MFS-HMT-001**: si hay T0, se escribe primero el token T0; si falla, se
   **aborta sin escribir T1** — el token de FRAM es el que tiene la última
   palabra (es el único medio que no necesita borrado).
3. Slot destino `seq % 8`. Se borra el sector **antes** de reprogramar si el
   slot es el 0 de la vuelta, o si el slot no está en blanco (`0xFF`): NOR no
   permite subir bits de 0 a 1.
4. Escribe los 32 B y marca `committed = 1` en todas las entradas de la ventana
   con ese `txid`; cierra la transacción (`tx_open = 0`, `sp_depth = 0`).

### Lectura del token vigente

`mfs_tok_latest` recorre los 8 slots, descarta todo lo que no tenga magic `'TO'`
o cuyo CRC-32C no cuadre, y devuelve el de **mayor `seq`**. Es la única fuente
de verdad al montar: un token corrupto por un corte a media escritura se ignora
y se usa el anterior, que sí está íntegro.

## Replay y corte de energía

`mfs_wal_replay(fs, window)` no reconstruye metadatos — de eso se encarga el
escaneo E2G de `mfs_recover_metadata()` — sino que **clasifica el arranque**:

- Hay token ⇒ hubo corte en vuelo: se incrementa `power_events` y se registra
  `MFS_EV_POWER_LOSS` en el HCT con el `seq` observado.
- No hay token ⇒ volumen virgen recién formateado: `MFS_EV_MOUNT_OK`.
- **Anti-rollback** (§15, MFS-SEC-003): si `epoch_token > fs->epoch`, se adopta
  la del token. Una imagen restaurada de un respaldo antiguo no puede hacer
  retroceder la época.

## Checkpoint (§9.5)

`mfs_checkpoint_write(fs)` condensa la ventana WAL en un compromiso Merkle
BLAKE3 y poda lo ya comprometido:

1. `wal_count == 0` ⇒ no hay nada que consolidar (`MFS_OK`).
2. Hojas: hasta 64 hashes `mfs_b3_256(NULL, 0, &wal[j], sizeof(wal[j]))` sobre
   la entrada **en RAM** (no se relee el medio).
3. Reducción por pares hasta una raíz; los niveles impares se promueven.
4. `fs->merkle_root = raíz[0..7]` (truncada a 8 B, lo que cabe en el token).
5. Emite un token sintético con `txid = 0xCECE0000 ^ fs->seq` para marcar la
   frontera consolidada, y restaura `txid_cur`.
6. Compacta la ventana conservando sólo entradas **no comprometidas**
   (`valid && committed != 1`); si queda alguna, la transacción sigue abierta.

`mfs_checkpoint_load(fs)` es el camino inverso al montar: recupera
`merkle_root`, eleva la `epoch` y **restaura la continuidad del contador**
`tok_seq_a` desde el token, para que el siguiente commit no reutilice un `seq`
ya grabado.

## Estructuras en RAM

| Estructura | Campos | Uso |
|---|---|---|
| `mfs_wrec_t` | `txid, ppage, committed, valid` | ventana BMT (`wal[256]`) |
| `mfs_wal_ent_t` | `lba, zone_off, len, kind, committed` | descriptores de entrada |

`committed` distingue *pendiente* (`0`) de *comprometida* (`1`); una transacción
abierta (`tx_open`) hace que las entradas nuevas entren con valor 0 y sólo se
consideren válidas tras el T1.

## Errores

| Código | Causa |
|---|---|
| `MFS_ENOTMOUNTED` | append sin montaje |
| `MFS_EINVAL` | payload mayor que la página |
| `MFS_EBACKPRESSURE` | ventana llena y checkpoint fallido (MFS-TX-004) |
| `MFS_ENOSPC` | no hay zona WAL disponible tras GC y reasignación |
| propagados | los del medio (`mfs_rec_write`, `mfs_erase`, `mfs_write`, `mfs_t0_write`) |

## Savepoints (§9.6)

`sp_marks[5]` con `MFS_SAVEPOINT_DEPTH = 4` niveles: el rollback restaura el
**estado de metadatos** (tamaño y extents inline) y las páginas ya programadas
quedan como basura reclamable por GC. Alcance exacto en
[`known-limitations.md`](known-limitations.md).
