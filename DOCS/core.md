# Núcleo: API, POSIX-subset, transacciones, snapshots, EDP, DAB, HCT

Spec: §9.6, §10.8, §12.3, §13.1, §16, §21, §24. Implementación: `src/core/mfs_fs.c`.

## Ciclo de vida (FSM de montaje §24.2)

1. `mf_init`: detecta geometría → lee HWV persistido (o lo usa de la detección)
   → modo/viabilidad → selección de suite → superblock A/B → escaneo de zonas
   → **reconstrucción de metadatos** → checkpoint/replay → raíz.
2. `mf_format`: exige `fs->cfg` previo (blanco: el integrador fija `fs.cfg` o
   monta primero). Borra región baja + todas las zonas, escribe HWV y SB A/B.
3. `mf_sync`: flush de inodos → checkpoint → rotación de superblock (reescribe
   el de menor época) → flush HCT → GC si procede.
4. `mf_deinit`: sync + **ceroización** de la instancia (`MFS-SEC-002`).

Errores de arranque: `MFS_EARCH` (arquitectura), `MFS_ENOTVIABLE` (RAM
insuficiente), `MFS_ECORRUPT` (sin superblock válido → requiere `mf_format`).

## POSIX-subset (§21.1)

`mf_open/close/read/write/seek/tell/stat/unlink/rename/mkdir/truncate` y
`mf_opendir/readdir/closedir`.

- La **granularidad de datos** es `mfs_payload_bytes()` = `chunk − cabecera E2G
  (− tag AEAD)`. `mf_read` nunca entrega bytes más allá de EOF.
- Nombres y padres se persisten como registros `DIRENT`; los inodos como
  `INODE` (tombstone `nlink == 0` para borrado durable).
- `mf_write` es transaccional por llamada; `O_RAW` (sin caché) usa E2G
  obligatorio.

## Extents

4 extents inline por inodo (§22.3, INO-L) que actúan como **caché**; el resto de
páginas (y también las 4 primeras) se resuelven por **L2P** con la clave
`extent_lba(ino, vpage)`. Cada registro `DATA` porta ese LBA en su cabecera E2G,
de modo que el montaje reconstruye el mapa escaneando el medio: **no hay tabla de
desbordamiento persistida ni en RAM** (los registros `EXTENT` y la tabla
`MFS_EXT_OVF_MAX` se retiraron por redundantes: duplicaban el consumo de entradas
L2P y una página de medio por cada página de datos más allá de la 4ª). El tamaño
de fichero alcanzable queda por tanto acotado sólo por el mapa L2P y las zonas,
no por una tabla auxiliar.

Si una página viva resulta ilegible (CRC/E2G), `mfs_extent_read` la distingue de
un hueco (`MFS_PPAGE_ERROR`) y `mf_read` propaga `MFS_ECORRUPT` en lugar de
entregar ceros: una pérdida de datos nunca se disfraza de hueco. Ojo: el ppage 0
(zona 0, página 0) es un destino **válido**, por lo que el valor "ausente" es
`MFS_L2P_FREE` (`0xFFFFFFFF`), nunca 0.

## Transacciones y savepoints (§9, §9.6)

- `mf_tx_begin/commit/abort`. Al **commit** se emite token T1 + marca
  `TXMARK(commit)`; al **abort** se emite `TXMARK(abort)` y se restaura el
  estado en RAM.
- Savepoints anidados (profundidad según modo): `mf_sp_create/rollback/release`.
  Se toma un **snapshot del estado de inodos** (tamaño, mtime y extents inline)
  y el rollback lo restaura; las páginas ya programadas quedan como basura
  reclamable por GC. Las páginas que un `truncate` retiró del L2P durante la
  transacción no se remapean (el mapeo se reconstruye del medio al remontar).
- El *replay* sólo aplica registros de transacciones confirmadas
  (`TXMARK == commit`) o autocommit (`txid == 0`) → `MFS-TX-003`.

## Ventana de inodos y reconstrucción

La ventana RAM de inodos es acotada (`MFS_MAX_FILES_OPEN + 16` = 48 entradas).
En la reconstrucción al montar los registros llegan en orden cronológico, así
que cuando la ventana se llena se desaloja el inodo **actualizado hace más
tiempo** (`ino_window_take`, marca circular de recencia); así se conservan en
RAM los ficheros más recientes en lugar de descartar sus registros. Los inodos
desalojados permanecen íntegros en el medio y se recuperan en el siguiente
montaje.

## Snapshots y FPT (§10.8)

- `mf_snap_create` fija la raíz Merkle (O(1)) y persiste la tabla (registro
  `EPOCH`); `mf_snap_revert` es un re-fijado de raíz (< 1 ms);
  `mf_snap_delete` libera por GLD. Límite por modo ⇒ `MFS_ESNAPMAX`.
- `mf_fpt_begin/apply/activate/rollback`: parche OTA por delta con
  anti-rollback de época; `activate` incrementa la época y sincroniza.

## EDP (§12.3)

`mfs_edp_check` evalúa el rail (driver `rail_ok` o hook de test) y escala por
niveles 1–5; nivel ≥ 4 activa SPDR (rechazo `MFS_EAGAIN` de trabajo no-RT),
nivel 5 dispara `mfs_edp_drain` (flush ordenado: inodos → checkpoint/token →
superblock) dejando `MFS_EDP_DONE`. `mfs_edp_window_us` implementa el modelo
`E = ½·C·(V0² − Vmin²)`.

## DAB y CUSUM (§24.4, §16)

`mfs_dab_init/select_arm/feedback` implementan EXP3 determinista sembrado
(`dab_seed` ⇒ replay reproducible), con estados `FROZEN/EXPLOIT/EXPLORE`,
freeze-on-violation en `rt_strict`. `mfs_cusum_update/alarm` implementan el
control SPC sobre latencia (k=0,5; h=5).

## HCT v2 (§16)

Ring de eventos + agregados; en modos ≤ Nano los agregados viajan a flash en
`sync`/`deinit`. Export `mf_export_health` en **CBOR** (`mfs_cbor_health`) +
firma **COSE_Sign1** simplificada (HMAC-SHA256 truncado) si hay clave.

## DAIO (§13.1)

`mf_submit` encola en un ring SPSC (ISR-safe, nunca bloquea; rechaza con
`MFS_EBACKPRESSURE` si está lleno). `mf_poll` drena respetando τ por clase
RT-A/B/C; una violación de τ en RT-A se reporta como `MFS_ETIMEDOUT_BUDGET` y
alimenta CUSUM. `cb->buf` apunta a una estructura cuyo primer campo es el
`mfs_file*` y a continuación el buffer de datos.

## Verificación (§21.1)

`mf_verify(QUICK|META|FULL)`: superblock, extents vía L2P + lectura E2G, y
comparación de la raíz Merkle con el token vigente.
