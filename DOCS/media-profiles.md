# Perfiles por tecnología de memoria y capacidad certificada

Spec: §3.2 (medios soportados), §5.1–§5.2 (detección y HWV), §6.1 (viabilidad),
§8.1 (layout y ventana de zonas), §18.2 (límites por modo), §22 (layout
on-flash).
Implementación: [`include/matrixfs/mfs_types.h`](../include/matrixfs/mfs_types.h)
(`mfs_media_profile_t`, `mfs_engine_t`), [`src/core/mfs_profile.c`](../src/core/mfs_profile.c),
gate en [`src/core/mfs_hal.c`](../src/core/mfs_hal.c) (**MFS-CAP-001**).

La estrategia de almacenamiento **la dicta el medio**, no una heurística única:
cada tecnología declara su unidad de asignación, su granularidad de borrado, sus
operaciones nativas y la capacidad máxima para la que está certificada. El HAL
configura el layout a partir de esa ficha y rechaza de forma tipificada un medio
que exceda su cota.

## 1. Motores

| Motor | Medios | Quién hace el mapeo físico | Consecuencia de diseño |
|---|---|---|---|
| **RAW** | NOR SPI/QSPI/OSPI, NAND raw, ONFI/Toggle, FRAM, MRAM, EEPROM | **MatrixFS**: ZLF + E2G + L2P, desgaste, ECC y recuperación propios | El medio no tiene FTL: el sistema asume el trabajo completo (y su coste en RAM) |
| **ZONED** | ZNS NAND / ZNS-like (UFS 4.0) | El medio expone zonas; mapeo **1:1 zona↔zona física** con reset nativo | No hay GC de medio que ocultar: el sistema ve y gestiona las zonas |
| **MANAGED** | SD (SDXC/SDUC), eMMC 5.1, USB flash, SSD NVMe | **El dispositivo** (su FTL): reubicación, desgaste, ECC y TRIM | El sistema asigna por **clúster** (unidad de asignación certificada), mantiene su propia tabla y journal y **no duplica** el FTL del controlador |

`mfs_media_profile_t` es la ficha: `max_bytes`, `alloc_unit`, `erase_unit`,
`page_size`, `pgm_gran`, `seek_unit`, `chunk`, `flags` (TRIM, PPP, BYTE_ADDR,
NO_ERASE, SUSPEND, MULTIPLANE, CQE, XIP, ECC_ON_DIE, REMOVABLE) y presupuestos
temporales (`t_prog_max_us`, `t_erase_max_us`, `t_read_max_us`).

## 2. Tabla de perfiles (`mfstool profiles`)

| Medio | Motor | Capacidad certificada | Clúster | Borrado | Página | Dir. bus | Chunk | Fuente |
|---|---|---|---|---|---|---|---|---|
| `NOR_SPI` | RAW | 4 GiB (cota del motor RAW; die ≤ 4 Gb = 512 MiB) | 64 KiB | 4 KiB | 256 B | 1 B | 4096 | JEDEC JESD216 (SFDP) |
| `NAND_RAW` | RAW | 4 GiB | 1 MiB | 128 KiB | 4 KiB | página | 4096 | datasheet NAND 2D/3D |
| `NAND_ONFI` | RAW | 4 GiB | 2 MiB | 1 MiB | 8 KiB | página | 4096 | JEDEC JESD230 (ONFI) |
| `ZNS_NAND` | ZONED | 2 TiB | 4 MiB | 4 MiB (zona) | 4 KiB | 4 KiB | 4096 | NVMe ZNS / UFS 4.0 |
| `FRAM` | RAW | 16 MiB | 4 KiB | no borrable | 1 B | 1 B | 4096 | datasheet FRAM SPI |
| `MRAM` | RAW | 1 GiB | 4 KiB | no borrable | 64 B | 1 B | 4096 | datasheet MRAM SPI |
| `EEPROM` | RAW | 1 MiB | 256 B | no borrable | 64 B | 1 B | 512 | datasheet EEPROM I²C/SPI |
| **`SD`** | MANAGED | **2 TB** (2 TiB; el estándar admite SDUC hasta 128 TB) | **4 MiB (AU)** | 4 MiB (AU) | 512 B | **512 B (LBA)** | 4096 | SD Physical Layer Spec |
| **`EMMC`** | MANAGED | **2 TB** | **4 MiB (`AU_SIZE`) ** | **512 KiB (`ERASE_GRP_SIZE`)** | 512 B | 512 B (LBA) | 4096 | JEDEC JESD84-B51 (eMMC 5.1) |
| **`USB`** | MANAGED | **2 TB** (práctico 1 TB) | 1 MiB | no se asume | 512 B | 512 B | 4096 | USB MSC / UASP (SCSI SBC-4) |
| **`NVME`** | MANAGED | **8 TiB** (M.2 2280 actual) | 1 MiB | `deallocate` (trim) | 4 KiB | 512 B | 4096 | NVMe 2.0 (NVM Command Set) |

Datos de especificación que fijan los valores:

- **SD.** El estándar direcciona por **bloque de 512 B** desde SDHC, y la unidad
  de asignación (`AU_SIZE`, registro de estado SD) típica de SDXC/SDUC es
  **4 MB**, que es también la granularidad de borrado (`CMD32`/`CMD33`/`CMD38`).
  SDXC llega a 2 TB y SDUC a 128 TB: el perfil certifica **2 TB** (2 TiB, que
  cubre el etiquetado comercial de 2 TB decimales).
- **eMMC.** El estándar publica `AU_SIZE` en `EXT_CSD[11]` (**4 MB** típico) y el
  grupo de borrado en `ERASE_GRP_SIZE[224]` / `ERASE_GRP_MULT[225]`
  (**512 KiB** en la mayoría de dispositivos); el direccionamiento es por bloque
  de 512 B.
- **NAND.** Página de 4–16 KB y bloque de borrado de cientos de páginas
  (**128 KB** en 2D SLC/MLC; 1–8 MB en 3D y SSD). El perfil toma el mínimo
  conservador para no asumir un bloque que el silicio no garantiza.
- **NVMe.** Bloques lógicos de 512 B o 4 KB, sin operación de borrado expuesta
  (`deallocate`/DSM hace de TRIM) y tamaño de transferencia óptimo del orden de
  128 KB.

## 3. Validación de capacidad (MFS-CAP-001)

`mfs_profile_check(medio, bytes)` devuelve:

| Situación | Estado |
|---|---|
| `bytes ≤ max_bytes` | `MFS_OK` |
| `bytes > max_bytes` | `MFS_ENOTVIABLE` (se rechaza de forma tipificada) |
| Medio desconocido (`MFS_MEDIA_NONE`) o tipo fuera de rango | `MFS_EINVAL` (no certificable) |

El gate se aplica en `mfs_hal_detect` para el medio T1 **y** para el tier T0, de
modo que ni el volumen ni su espejo pueden exceder su cota. En el adaptador VFS,
`MFS_ENOTVIABLE` se traduce a `EINVAL` en Linux y a `STATUS_INVALID_PARAMETER`
en Windows, y el mensaje de `mount` incluye el estado del núcleo.

Comprobación manual:

```sh
mfstool profiles                     # tabla completa
mfstool profiles --check SD 2T       # MFS_OK
mfstool profiles --check SD 3T       # MFS_ENOTVIABLE
mfstool profiles --check EMMC 1T
mfstool profiles --check NVME 8T
```

## 4. Techos vigentes y hoja de ruta

Estado medido del código (véase [`known-limitations.md`](known-limitations.md)):

| Cota | Valor hoy | Dónde |
|---|---|---|
| Dirección de medio (driver L2) | 32 bits de byte ⇒ 4 GiB | `include/matrixfs/mfs_port.h`, `platform/common/mfs_blk.c` |
| Ventana de zonas en RAM | 128 zonas × `blocks·erase_unit` | `src/core/mfs_fs.c` |
| Mapa L2P | tabla estática (4096 / 262 144 entradas) | `src/core/mfs_zone.c` |
| Tamaño máx. de fichero | `vpage` a 16 bits ⇒ 254,75 MiB con chunk 4096 | `src/core/mfs_fs.c` |

Hoja de ruta para alinear la capacidad gestionada con la certificada por perfil:

| Fase | Contenido | Prioridad |
|---|---|---|
| **F1 · hecho** | Perfiles por tecnología, validación MFS-CAP-001, `mfstool profiles`, suite `test_profile` | P1 (base, riesgo mínimo) |
| **F2** | Formato **único v2**: anchura de 64 bits en tamaños/direcciones (SB, E2G, inodos, WAL, geometría/HWV), eliminación del truncado a 4 GiB y del enmascarado de `vpage` | P1 (determinismo y corrección) |
| **F3** | **Motor MANAGED**: asignación por clúster de la unidad certificada (AU de SD/eMMC), tabla en flash con caché acotada, bitmap de libres, journal de metadatos y montaje sin escaneo completo ⇒ **2 TB en microSD/eMMC/USB** | P1 |
| **F4** | Motor **ZONED** (ZNS: zona↔zona con reset nativo) y ajuste fino del motor MANAGED para NVMe (`deallocate`, colas) | P2 |
| **F5** | Banco `mfstool bench --fill <medio> [--pct 95] [--rand N]`: escritura secuencial y aleatoria > 95 % de la partición, relectura exhaustiva con verificación de integridad, remontajes repetidos y FIH; ejecutable en hardware real y en host con disco | P1 (verificación) |
| **F6** | Ampliación del motor RAW a multi-die/LUN (> 4 GiB en NOR/NAND crudos) | P3 |

## 5. Pruebas

`tests/test_profile.c` (en `make test`):

- Coherencia de cada ficha: potencias de dos, `alloc_unit ≥ page_size`,
  `erase_unit` múltiplo de página, `pgm_gran ≤ page_size`, motor válido.
- Determinismo: la consulta devuelve el mismo contenido byte a byte.
- Asignación de motor por tecnología (RAW/ZONED/MANAGED).
- Cota exigida: SD, eMMC, USB y NVMe ≥ 2 TB decimales.
- Capacidad nativa: AU de SD = 4 MB = su borrado; erase group de eMMC =
  512 KiB y AU = 4 MB; `seek_unit` = 512 B en SD/eMMC/USB.
- `mfs_profile_check`: acepta el máximo exacto, rechaza máximo + 1 con
  `MFS_ENOTVIABLE`, y `MFS_EINVAL` para medios no certificables.
- Integración: formatear un volumen y **remontarlo declarando un medio por
  encima de su perfil** devuelve `MFS_ENOTVIABLE`; un tier T0 de 32 MB (FRAM,
  máx. 16 MB) también se rechaza.

La matriz completa de capacidad (llenado > 95 %, relectura total, remontajes
repetidos y FIH sobre 2 TB) requiere el banco de **F5** y hardware real: en el
entorno de desarrollo no hay 2 TB de medio ni de disco para respaldar el
simulador.
