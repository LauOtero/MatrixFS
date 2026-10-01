# Arquitectura y layout físico

Spec: §4, §5.2, §8.1, §18, §20.4, §22, §23.

## Capas (§4)

```
L8 API POSIX-subset · DAIO v2 · VIO
L7 WAL+ v3 · tokens · GLD
L6 CCD v2 · LZ4 · Gear-CDC · FSST · CFX · suites S0–S3
L5 inodos/extents (ventana flash-first) · snapshots
L4 ZLF · E2G · L2P · AGCB+/GC
L3 HAL SafeProfile · HWV persistido · viabilidad
L2 driver L2 (NOR/NAND/FRAM…) — `mfs_l2_driver`
L1 puerto — `mfs_port.h` (secciones críticas, ciclos, WFI)
```

Implementación: `src/core/` (HAL, núcleo, WAL, zonas, pipeline), `src/crypto/`
(primitivas y suites), `sim/` (host), `tests/`, `tools/`.

Sobre el núcleo se sitúa la **capa de integración con el sistema operativo**,
que no forma parte del núcleo embebido y se compila aparte:

```
platform/common/   adaptador VFS portable + driver de bloque (imagen/dev/volumen)
platform/linux/    front-end FUSE 3, fstab, systemd, udev, paquetes
platform/windows/  front-end WinFsp, servicio de automontaje, instalador
```

La semántica del sistema de archivos (permisos POSIX persistidos, metadatos,
E/S, errores) vive en `platform/common/mfs_vfs.c`; los front-ends son
envoltorios finos. Véanse [`linux-integration.md`](linux-integration.md) y
[`windows-integration.md`](windows-integration.md).

## Layout físico de la región baja (§8.1, adaptado a semántica NOR)

El medio se organiza en **sectores de `erase_unit`**. Para respetar la regla
«borrar antes de reprogramar», cada estructura crítica vive en su propio
sector:

| Región | Dirección | Contenido |
|---|---|---|
| sector 0 | `0` | **SB A** (@0, 256 B) + **HWV** (@512, 64 B) |
| sector 1 | `erase_unit` | **SB B** (256 B) |
| sector 2 | `2·erase_unit` | anillo de **tokens T1** (8 × 32 B) |
| sector 3… | `3·erase_unit` | **zonas ZLF** (hasta `MFS_MAX_ZONES` = 128) |

- `mf_sync` reescribe el superblock de **menor época** (borra su sector primero),
  por lo que un corte deja siempre un SB válido ⇒ arranque determinista.
- El anillo de tokens conserva una **secuencia monótona** persistida
  (`tok_seq_a`, restaurada en `mfs_checkpoint_load`) y sólo borra el sector al
  dar la vuelta; además verifica que el slot destino esté virgen antes de
  programar.

> Nota: la spec sitúa SB B en `0x100` (mismo sector que A). Se separa a un
> sector propio porque un `erase_unit` típico de 4 KB los agruparía, rompiendo
> la atomicidad A/B. Ver `known-limitations.md`.

## Zonas ZLF (§8.1, §11.6)

- Una zona agrupa **1–4 bloques** (`zone_blocks`) elegidos en `zones_init_table`
  para que quepa al menos `chunk + 64 B` de cabecera. En Compact (chunk 512)
  basta 1 bloque; en Balanced/Extended (chunk 4096) se usan 2.
- Escritura estrictamente secuencial; el estado se **recalcula al montar**
  escaneando páginas hasta el primer hueco borrado (`mfs_scan_zones`).
- Cabecera de zona §22.2 en `mfs_zonehdr_write/read` (magic, id, estado, seq,
  clase, CRC-32C).

## Superblock (§22.1, 256 B)

Campos: magic `MFSU`, época, generación, `suite_id`/`mode_id`, raíz ART,
descriptor WAL, raíces de snapshot (8), `golden_crc` (CRC-32C sobre 0..102) y
**MAC** de 16 B (HMAC-SHA256 truncado) si hay clave. Selección A/B por mayor
`{época, seq}` con integridad válida.

## Modos y viabilidad (§6, §18)

`mfs_select_mode` aplica `RAM_total − firmware(50 %) − stack(15 %) −
periféricos(8 %) − margen(≥10 %)` y devuelve el mayor modo que cabe; si no cabe
ni Ultra-Nano ⇒ `MFS_MODE_UNSUPPORTED` y `mf_init` retorna `MFS_ENOTVIABLE`.
Arquitecturas de 8 bits se rechazan en build (`mfs_types.h`) y en runtime
(`MFS_EARCH`, `arch_class == 0`).

## Estructuras en RAM (§23)

`mf_t` contiene las tablas estáticas (inodos ventana flash-first, zonas,
ventana WAL, ring iocb) — sin heap (`MFS-RES-001`). El mapa L2P, la tabla de
transacciones y los snapshots de savepoint son estáticos en `mfs_zone.c` /
`mfs_fs.c`.

## Serialización (§20.4)

Todo el formato on-flash es little-endian y se accede **exclusivamente** por
los accesores `mfs_ld16/32/64`, `mfs_st16/32/64` de `mfs_port.h`.
