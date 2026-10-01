# Zonas ZLF, E2G, L2P y GC/AGCB+

Spec: §8.2, §9.4, §11.2, §11.6, §22.2, §22.4, §24.1.
Implementación: `src/core/mfs_zone.c`.

## Zonas (§11.6, §24.1)

- Una zona agrupa 1–4 bloques; escritura estrictamente secuencial, sin GC
  intercalado dentro de la zona (`MFS-FTL-001`) ⇒ latencia acotada.
- Estados `EMPTY → OPEN → FULL → RECLAIMING → EMPTY` (FSM §24.1); el estado se
  **recalcula al montar** escaneando páginas hasta el primer hueco borrado.
- Cabecera de zona §22.2 (64 B, `mfs_zonehdr_write/read`).

## Registro E2G (§8.2)

Cabecera por registro, 20 B en NOR / 32 B en NAND:

```
magic2 | ver1 | (kind<<4|flags)1 | dictid1 | lba4 | gen1 | snapid1 | hotness1 | zrp1 | len2 | crc32c4
```

- Dirección de la página física: `ppage = (zona << 16) | idx`, donde
  `idx = (write_ptr − MFS_ZONEHDR_SIZE) / chunk`; la dirección física es
  `start_addr + MFS_ZONEHDR_SIZE + idx·chunk`.
- `dictid` porta los *flags de codec* (bit0 = LZ4) que el GC preserva al
  relocalizar.
- **MFS-FMT-001**: toda lectura valida `{LBA, generación, CRC-32C}`; la
  discrepancia devuelve `MFS_EBADMSG` y eleva el contador de fallos E2G del HCT.
- Cifrado AEAD por registro (§10.6): el nonce es `época‖(zona.seq + idx)`, con
  `zona.seq` persistido en la cabecera de zona para reproducibilidad tras
  remount.

## L2P (mapa lógico→físico)

Tabla estática (sin heap, `MFS-RES-001`) con sondeo lineal y clave = LBA lógico
completo.

- **Capacidad**: `MFS_L2P_SLOTS` entradas (potencia de dos). El valor por
  defecto es 4096 (≈32 KiB de `.bss`, perfil embebido); los front-ends de host
  lo amplían por build (`-DMFS_L2P_SLOTS=262144`, 2 MiB) para cubrir el volumen
  máximo del modo Extended (512 MiB). El layout on-flash **no** depende de este
  valor: sólo de cuántas páginas es capaz de mapear cada binario.
- **Borrado**: `mfs_l2p_drop(lba)` marca la entrada como **lápida**
  (`MFS_L2P_TOMB`) en lugar de vaciarla: el sondeo lineal de las claves que
  colisionan con ella debe continuar, y vaciar el hueco truncaría sus cadenas
  (perdiendo páginas vivas). `l2p_drop_zone` aplica el mismo criterio tras el
  borrado de una zona. Las lápidas se reciclan en inserciones posteriores.
- **Centinelas**: `MFS_L2P_FREE` = ausente (nunca puede confundirse con un
  destino válido porque `ppage = 0` *sí* es válido: zona 0, página 0).
- El GC usa `mfs_l2p_get(lba) == ppage` para decidir si un registro es la
  **versión vigente** y debe relocalizarse.

LBAs reservados:

| Rango | Uso |
|---|---|
| `0x100000 + ino` | registro `INODE` |
| `0x200000 + (ino<<16 | vpage)` | página de datos |
| `0x300000 + ino` | registro `DIRENT` |
| `0x400000` | reservado (registro `EXTENT`, ya no se emite) |
| `0x500000 + txid` | `TXMARK` |
| `0x900000/0xE00000 + época` | `HCTAGG` / `EPOCH` |

> Las páginas de datos no necesitan estructura de extents persistida: el propio
> registro `DATA` porta su LBA en la cabecera E2G y el montaje reconstruye el
> mapa con él (los 4 primeros extents inline del inodo son sólo caché §22.3).

## Reconstrucción al montar: orden cronológico

`zones_init_table`/`mfs_scan_zones` recuperan el `write_ptr` de cada zona
leyendo su cabecera; `mfs_recover_metadata` aplica los registros en **orden
cronológico de apertura de zona** (`z->seq`, persistido en la cabecera §22.2) y
no en orden de índice. El índice de zona no es un orden temporal: la GC libera
zonas de índice bajo y el asignador las reutiliza para escrituras nuevas, de
modo que una versión reciente puede vivir en una zona de índice menor que otra
antigua; aplicar en orden de índice haría que la versión vieja sobrescribiera a
la nueva (estado obsoleto tras remontar). Por el mismo motivo `fs->seq` se
restaura desde el superblock al montar (el contador debe continuar por encima
del último valor persistido).

## GLD y GC (AGCB+) (§9.4, §11.2)

- `mfs_gld_add(pages, meta)`: `+1` por página de datos, `+2` por metadatos.
- `mfs_gld_maybe_gc`: si `deuda ≥ D_max` (256, §25) y hay margen ELD, ejecuta
  slices de 500 µs. Con `free_zones ≤ 1` no arranca (reserva zona de trabajo).
- `mfs_gc_slice`: elige la víctima `FULL` de **menor utilidad** viva, relocaliza
  sus registros vigentes (copia literal del registro, conservando `kind`, `gen`,
  `snap` y `dictid`, que es lo que permite preservar el codec LZ4) y borra todos
  los bloques de la zona.
- Antes de programar el `erase` se comprueba (`zone_live_pages`) que no quede
  ninguna página viva sin relocalizar; si la hubiera, el slice se aborta con
  `MFS_ESTATE` en lugar de perder datos en silencio.
- Backpressure: `mf_write` fuerza GC a partir de `4·dirty-list` y rechaza con
  `MFS_EBACKPRESSURE` por encima de `8·dirty-list`.

## Huella de desgaste

`mfs_zone_erase` borra los 1–4 bloques de la zona; el simulador lleva contador
P/E por bloque (`vflash_t.pe`) para HCT/desgaste.
