# Pipeline CCD: SRB, LZ4, Gear-CDC, FSST y CFX

Spec: §10.1–§10.4, §18.2, §25.
Implementación: `src/core/mfs_compact.c`.
Reglas: **MFS-CMP-001** (ganancia mínima), **MFS-DDP-001** (dedup convergente).

El *Compression & Dedup Compactor* (CCD) actúa **por página**, antes de E2G: la
página comprimida se escribe como payload de un registro `DATA`, de modo que el
descompresor sólo necesita la cabecera E2G (`dictid`) para revertir el proceso.

```
pl → [SRB] → LZ4 (opcional) → hash BLAKE3 → CFX (dedup) → mfs_rec_write → E2G
                                                              ↓ (miss)
                                                         zona + E2G
```

## Umbral de ganancia (MFS-CMP-001)

`CMP_GAIN_MIN_PCT = 12`: un bloque comprimido sólo se acepta si ahorra **al
menos el 12 %**, y además debe caber con su cabecera E2G en la página
(`c + hs <= page_bytes`). Comprimir por debajo de ese umbral cuesta CPU y no
ahorra flash, así que el códec devuelve `0` y se escribe el original.

## LZ4 (§10.1)

Compresor LZ4 simplificado y determinista, **ventana hacia atrás de 256 B**
(offset de 1 B + un byte alto fijo):

| Elemento | Formato |
|---|---|
| Token | `ctrl = (lits << 4) \| (matchlen − 4)`, 1 B |
| Literales | `lits` bytes (máx. 15 por token) |
| Match | `off_lo` (1 B) + `off_hi = 0` (1 B) |

- `mfs_lz4_compress(in, ilen, out, ospace)` → bytes emitidos, o `0` si no cabe,
  si `ospace < 4` o si no alcanza el 12 %.
- `mfs_lz4_decompress(in, ilen, out, ospace)` → bytes escritos, o `0` si el
  flujo es inconsistente (`off == 0`, `op < off` o desbordamiento del destino).
- La copia de match es **solapada byte a byte**, como exige LZ4 (`op - off`
  puede caer dentro del propio resultado).

El descompresor devuelve `0` ante cualquier inconsistencia y el llamante lo
convierte en `MFS_ECORRUPT` — el CRC-32C de E2G ya lo habría detectado antes.

## Gear-CDC (§10.2)

`gear_table[256]` se genera con un LCG determinista de semilla `0x47454152`
("GEAR") y se inicializa una sola vez (`mfs_gear_hash_init`, idempotente), de
modo que la tabla es **idéntica entre compilaciones y plataformas**.

```c
*state = (*state << 1) + gear_table[b];
corte   = ((*state & mask) == mask);      /* máscara 0xFFF ⇒ media 4 KiB */
```

`mfs_gear_boundary(state, byte, mask)` es el *rolling hash* de 1 byte que marca
fronteras de contenido para chunking CDC. La rutina está implementada y
exportada para el particionado por contenido; el pipeline de página descrito
arriba trabaja sobre la página completa, de modo que hoy el corte CDC no se
invoca en `mfs_data_write` (véase
[`known-limitations.md`](known-limitations.md)).

## FSST-lite (§10.3)

Codificación de **bigramas frecuentes** con diccionario de hasta
`FSST_SYMS = 64` símbolos:

| Parte | Formato |
|---|---|
| Cabecera | `npairs` (1 B) ‖ `pad = 0` (1 B) ‖ `pairs[64]` como `(hi, lo)` big-endian |
| Símbolo | `0x40 + p` sustituye al bigrama `p` (2 B → 1 B) |
| Literal | `0x00` + byte (escape) |

Selección determinista del diccionario: se cuentan los 65536 bigramas posibles
en buckets de 16 bits, se toman los que superan `cnt[b] > 2` y se emiten como
`pairs[n] = b * 8` (sin ordenación por frecuencia, para que el diccionario no
dependa del *timing*). El contador satura en 60000.

`mfs_fsst_encode/decode` validan tamaño de cabecera (`2 + 2·npairs`), `npairs ≤
64` y aplican el mismo umbral del 12 %. Como en el caso de Gear, están
disponibles como primitiva pero el pipeline de página no activa hoy el bit
`fsst` de `dictid`.

## SRB (§10.1, §18.2)

El *Staging Ring Buffer* es la **única** ventana de trabajo de la ruta de datos
(sin heap, MFS-RES-001):

- `mfs_srb_attach(fs)` fija `fs->srb` al overlay `ovl[0]` si el manifiesto lo
  aporta; si no, a un búfer estático de `MFS_CHUNK_EXTENDED = 4096` B. El
  tamaño efectivo se acota además por `cfg->ovl_sizes[0]` y se publica en
  `fs->srb_size`.
- Los búferes `work[]`/`tmp[]` de compresión y descompresión son asimismo
  estáticos de 4096 B: la ruta de datos no asigna memoria dinámica en ningún
  caso.

## CFX: índice de deduplicación (§10.4)

Tabla abierta estática de 256 entradas `{ u64 h; u32 ppage; u8 used }`.

| Operación | Comportamiento |
|---|---|
| `mfs_cfx_lookup(h64, &ppage)` | índice inicial `(h64 ^ (h64 >> 32)) & 0xFF`, sondeo lineal de hasta 8 pasos; `0` = hallado, `-1` = no está |
| `mfs_cfx_insert(h64, ppage)` | sondeo de hasta 8 pasos; inserta en hueco libre o en la entrada coincidente; si la tabla está llena, **reemplazo round-robin determinista** (contador estático) |
| `mfs_cfx_reset()` | vacía el índice (formateo/remontaje) |

Hueco de la deduplicación:

- Sólo se intenta con `cfg->allow_convergent` y modo ≥ `MFS_MODE_BALANCED`: en
  modos pequeños no hay presupuesto ni para el hash.
- El hash es `mfs_b3_256(NULL, 0, work, wlen, h32)` y la clave del índice son los
  **8 primeros bytes LE** del digest (`mfs_ld64(h32)`).
- En un acierto se escribe únicamente la **entrada L2P** del LBA hacia el
  `ppage` ya existente (`mfs_l2p_put`): no se programa flash. Como el registro
  original sigue siendo la versión vigente, el GC lo relocalizará cuando
  corresponda y ambos LBAs seguirán apuntando al mismo dato.
- Con `allow_convergent = 0` (por defecto en medios cifrados con nonce por
  registro) la deduplicación **se desactiva**: compartir páginas entre LBAs
  distintos rompería la vinculación dato↔nonce.

## Escritura y lectura de página

`mfs_data_write(fs, lba, pl, len, hotness, kind, &ppage)`:

1. Copia el payload a `work` y calcula `hs = mfs_e2g_hdr(fs)`.
2. LZ4 si `mode > ULTRA_NANO` y hay SRB; acepta si `c > 0 && c + hs <= page`.
3. Dedup convergente (si procede) ⇒ `MFS_OK` con el `ppage` existente.
4. Si no, reserva zona con reintento (`mfs_zone_alloc_open` → `mfs_gld_maybe_gc`
   → `mfs_gc_force`); sin zona ⇒ `MFS_ENOSPC`.
5. `mfs_rec_write` con `hotness`, `dictid = flags`, y publica `ppage`.

`mfs_data_read(fs, ppage, lba, buf, bufsize, &rlen)`:

1. `mfs_rec_read` valida E2G (LBA + generación + CRC-32C) y descifra el AEAD si
   la suite lo requiere.
2. Si `dictid & 1` ⇒ `mfs_lz4_decompress`; un fallo ⇒ `MFS_ECORRUPT`.
3. Sin compresión: si `r > bufsize` ⇒ `MFS_EOVERFLOW` (§18.2); si no, copia.

## Errores

| Código | Causa |
|---|---|
| `MFS_ENOSPC` | no hay zona de datos disponible ni tras GC |
| `MFS_ECORRUPT` | flujo comprimido ilegible |
| `MFS_EOVERFLOW` | el contenido excede el búfer de destino |
| `MFS_EBADMSG` | CRC/LBA/generación inválidos en E2G (nivel 4-5 §12) |
| propagados | los del medio y de la suite (`mfs_rec_write`/`mfs_rec_read`) |
