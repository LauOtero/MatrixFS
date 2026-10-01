# Alcance y desviaciones respecto a la especificación

Documento de honestidad técnica: qué está implementado, qué se ha adaptado y
qué **no** está cubierto. Alineado con §26 (lenguaje acotado) y §29.

## Desviaciones de layout

| Punto | Spec | Implementación | Motivo |
|---|---|---|---|
| SB B | `0x100` (mismo sector que A) | `erase_unit` (sector propio) | un `erase_unit` típico de 4 KB agrupa A/B y rompería la atomicidad A/B en NOR |
| HWV | @512, 64 B | @512, 64 B | conforme |
| Tokens | anillo con contador termométrico | anillo de 8 slots + contador monótono persistido | semántica equivalente y NOR-correcta (erase antes de reprogramar) |
| Extents | INO-L con 4 extents directos + árbol | 4 inline (caché) + resolución directa por L2P con el LBA del registro `DATA` | cubre ficheros de cualquier tamaño mapeable sin árbol ni tabla auxiliar |
| Nombres | registro DIRENT | registros `INODE` + `DIRENT` | el inodo no porta el nombre en la spec; aquí se persiste aparte |

## Funciones parciales o simplificadas

- **ART / L2P**: el mapeo usa L2P por hash estático, no un ART con
  path-copying. El snapshot es un re-fijado de raíz + registro `EPOCH`, no CoW
  real de páginas.
- **WOM-p, SLEC, EBA, RAS/TG, PEP, ELM, WEP, ZRP**: implementados (§11). WOM-p
  usa codificación bit-lane con header termométrico; ZRP reconstruye y
  **relocaliza** la página rescatada (sobre NVM no es posible reprogramar 0→1 en
  la página dañada).
- **HMT**: implementado el tiering T0/T1 (§11.11) — token primero en T0 y espejo
  de metadatos (inodo/dirent) en T0 con reescritura byte-directa y recuperación
  por último registro por clave. Los refcounts y el WAL completo no residen en
  T0.
- **ODT / TS-Delta**: `train-dict` genera un diccionario ODT simplificado;
  FSST-lite cubre strings. TS-Delta no implementado.
- **PUF / cadena PQ (LMS, SP 800-208) / XDAM / SDP / CQE**: implementados
  (§14.1, §15, §11.5). SLH-DSA se sustituye por LMS con parámetros
  (n=32, w=4, H=8); TrustZone y aceleradores criptográficos por HW no están
  cableados.
- **Cripto HW (AES/Ascon/B3 aceleradores)**: la API `mfs_l2_driver` reserva los
  enganches (`dma_read/dma_crc`), pero no hay ruta HW cableada en el núcleo.
- **Checkpoint incremental** (§9.5): se calcula la raíz Merkle de la ventana; no
  hay truncado *Panic-Safe* con readback ni rollup cada 16.
- **BMT**: el montaje reconstruye metadatos **escaneando** los registros E2G
  (acotado por el tamaño del medio). La cota fija de ≤ 12 ms @512 MB es un
  objetivo de diseño, no medida en target.
- **SRB in-place**: el pipeline usa un buffer rotativo, pero la compresión
  in-place estricta con ratio ≥ 1,1× es aproximada.
- **Savepoints**: el rollback restaura el **estado de metadatos** (tamaño y
  extents inline); las páginas ya programadas no se reescriben (quedan como
  basura reclamable por GC). Una sobreescritura en medio de una transacción no
  recupera el contenido previo (no hay CoW de datos), y las páginas que un
  `truncate` haya retirado del L2P dentro de la transacción no se remapean en el
  rollback (se recuperan al remontar, reconstruidas del medio).
- **Ventana de inodos**: `mf_t` mantiene `MFS_MAX_FILES_OPEN + 16` inodos en RAM
  (flash-first). Con más ficheros **distintos** simultáneamente residentes que
  ranuras, el inodo desalojado se persiste pero la resolución de ruta requiere
  remontar (no hay recarga de metadatos bajo demanda dentro de una sesión). En la
  reconstrucción al montar el desalojo es por **recencia** (`ino_window_take`),
  de modo que la ventana queda con los ficheros usados más recientemente.
- **Reserva de GC**: el asignador reserva una zona como destino de
  relocalización (`MFS_GC_RESERVE`) y libera directamente zonas totalmente
  obsoletas cuando no queda ninguna libre. Si el medio llega a llenarse por
  completo con registros **vivos** en todas las zonas, la reclamación requiere
  rotación dinámica de la reserva (no implementada): el sistema devuelve
  `MFS_ENOSPC`/`MFS_EBACKPRESSURE` tipificados y sigue siendo consistente. Antes
  de borrar una zona víctima se verifica que no queden páginas vivas sin
  relocalizar; si las hubiera, el pase se aborta en lugar de perder datos.
- **Mapa L2P**: una entrada por página direccionable, en tabla estática
  (`MFS_L2P_SLOTS`, potencia de dos). El valor de fábrica (4096) sólo permite
  mapear ~16 MiB con chunk 4096: los builds de host lo amplían a 262144
  (`-DMFS_L2P_SLOTS`, 2 MiB de `.bss`) para cubrir los 512 MiB del modo Extended.
  Al agotarse se devuelve `MFS_ETABLEFULL`, que el adaptador traduce a `ENOSPC`.

## Integración con el sistema operativo (Linux / Windows)

- **Una instancia por proceso.** El núcleo mantiene estado global (mapa L2P,
  pools CFX, ventana WAL, ventana de inodos) y **no es reentrante**. La
  capa de integración lo serializa con un mutex, pero cada volumen montado
  requiere su propio proceso (`matrixfs_fuse` / `matrixfs_winfsp.exe`) y, por
  tanto, su propia instancia. No hay multiplexación de varios volúmenes en un
  mismo proceso.
- **Límite de 4 GiB por medio.** El núcleo direcciona con 32 bits; el driver de
  bloque (`platform/common/mfs_blk.c`) trunca la geometría a los primeros 4 GiB
  del dispositivo o imagen. Es una limitación de diseño del núcleo, no de la
  capa de integración.
- **Dependencia de marcos externos.** El montaje depende de **libfuse 3** en
  Linux y de **WinFsp 1.12+** en Windows. No se implementa ningún driver propio
  del kernel ni de NT; en Windows, además, WinFsp debe estar instalado para que
  la unidad aparezca en el Explorador.
- **Sin ACLs de NTFS.** El front-end WinFsp no implementa descriptores de
  seguridad (`PersistentAcls = 0`); la autorización efectiva son los permisos
  POSIX persistidos del volumen. Los atributos NTFS «oculto», «sistema»,
  «comprimido» e «indexado» no tienen equivalente y se ignoran.
- **Enlaces simbólicos y duros.** No se exponen: el núcleo no los soporta, de
  modo que los front-ends devuelven los errores tipificados correspondientes
  (`ENOTSUP`/`STATUS_NOT_SUPPORTED`). Tampoco hay soporte de streams alternos
  NTFS, reparse points ni ficheros dispersos (sparse).
- **Escrituras dispersas.** El núcleo no implementa archivos dispersos; escribir
  más allá del final con un hueco rellena con ceros dentro de la extensión
  asignada, con el coste de programación correspondiente.
- **Rendimiento en medios crudos.** Sin FTL de dispositivo (NOR/NAND crudos), el
  rendimiento de escritura aleatoria depende del RMW alineado a sector de
  `mfs_blk` y del `erase_unit` configurado. En medios gestionados (eMMC/SD/USB)
  manda el FTL del propio dispositivo.

## Almacenamiento flash y RTOS

- **Medios gestionados (SD/eMMC/UFS/USB/NVMe/SATA).** El adaptador L2
  (`platform/common/mfs_l2_managed.c`) presenta la API de sectores del SDK como
  driver de MatrixFS, con RMW alineado a sector y TRIM/UNMAP. **No incluye
  controladores de silicio**: el enganche con el controlador real
  (SDHCI/UFSHCI/AHCI/NVMe) lo aporta el integrador con 2-3 callbacks de sector.
  La verificación en host usa un dispositivo gestionado simulado
  (`tests/vblk_sim.c`); la validación sobre unidades físicas (llenado > 95 %,
  relectura total, remontajes y FIH) requiere hardware y el banco **F5** de
  [`media-profiles.md`](media-profiles.md).
- **SATA y UFS.** Añadidos al modelo de perfiles (MFS-CAP-001) con su geometría
  y presupuestos; el transporte (AHCI/UniPro) no se reimplementa.
- **Tope de 4 GiB.** El driver L2 direcciona con 32 bits (véase «Límite de 4 GiB
  por medio»); las capacidades certificadas de las unidades gestionadas
  (2–8 TB) exigen el formato v2 de 64 bits (fases F2–F3 de `media-profiles.md`).
- **Puerto RTOS.** El núcleo trae adaptadores nativos de **FreeRTOS, Zephyr y
  ThreadX** compilados **solo en el target del RTOS**; en este entorno no se
  compilan (no hay toolchain del RTOS) y por tanto **no se han verificado en
  silicio**. Mbed OS, NuttX, RIOT, Mynewt, RT-Thread y PX5 se **detectan** y se
  resuelven con la plantilla `platform/rtos/mfs_rtos_port_template.c`. En host se
  verifica la detección, el registro y el contrato §20.2 con un adaptador de
  prueba (`tests/test_rtos.c`).
- **SDKs de fabricantes.** Silicon Labs, TI, Infineon y Renesas se integran a
  través de sus drivers de flash (capa `mfs_embedded` / `mfs_l2_managed`) y de su
  RTOS (puerto RTOS); no se incluyen proyectos de ejemplo compilados con cada SDK.

## No cubierto en esta edición

- Certificación SIL-2 / ISO 26262 / IEC 62443 y dossier de seguridad.
- Model-checking TLA+/Promela y cobertura MC/DC ≥ 90 % (FormalCore cubre, en su
  lugar, replay determinista y exploración de invariantes WAL+ sobre el código).
- Matriz de MCU real, stack-painting y mediciones de bus en silicio.
- Soporte de NAND/ONFI/ZNS/eMMC más allá de la cabecera de registro NAND (32 B)
  y del modelo de geometría; el simulador implementa NOR y un tier T0
  byte-addressable (FRAM/MRAM).
- **Montaje efectivo sobre el kernel**: requiere privilegios que el entorno de
  validación no tiene. En Linux, `mount(2)` sobre `/dev/fuse` exige
  `CAP_SYS_ADMIN` y el helper `fusermount3` (el front-end compila, enlaza contra
  libfuse3 3.17.2 real y alcanza ese punto); en Windows, `FspFileSystemCreate`
  devuelve `STATUS_NO_SUCH_DEVICE` sin una sesión elevada con el driver FSD
  accesible. Sí se validan la semántica completa del sistema de archivos (suite
  `test_vfs` + CLI `matrixfs-ctl`, **1 232 checks / 0 fallos en Linux y en
  Windows**) y la compilación estricta de los front-ends (FUSE con `-Werror`
  contra libfuse3 real; WinFsp con MSVC `/W4` contra el SDK real). Véase
  [`testing.md`](testing.md).
- **Matriz de kernels y de versiones de Windows ejecutada**: los procedimientos
  están documentados en [`linux-integration.md`](linux-integration.md#91-pendiente-de-ejecutar-requiere-privilegios)
  y [`windows-integration.md`](windows-integration.md#101-pendiente-de-ejecutar-requiere-privilegios),
  pero no se han ejecutado (ni las pruebas manuales en el Explorador).

## Nota sobre §18.2 / §23.2

Las sumas de RAM por modo del certificado RSC reproducen las tablas §23.2. En
**Extended** la spec lista `margen 1640` y `total 21504`, pero la suma de sus
propios sumandos da 22504; el certificado usa el `total` normativo (21504) y
deriva el margen (640).
