# MatrixFS Ultra «ATLAS» — Documentación de implementación

Índice de la documentación por módulo. La referencia normativa es
`DOCS/MatrixFS Ultra - Technical Specifications and Implementation Guide.md`
(MFS-SPEC-003 · Edición 1.0 «ATLAS»). Cada documento enlaza las secciones (§)
que implementa.

| Documento | Contenido | Spec |
|---|---|---|
| [architecture.md](architecture.md) | Capas, layout físico, modos, viabilidad | §4–§8, §18, §20.4, §23 |
| [hal.md](hal.md) | Cascada de detección, HWV, bus read-only | §5, §6, §14.2 |
| [core.md](core.md) | API, POSIX-subset, TX/savepoints, snap/FPT, EDP/DAB/CUSUM/HCT | §9.6, §10.8, §12.3, §16, §21, §24 |
| [wal.md](wal.md) | WAL+, tokens T1/T0, TXMARK, checkpoint, replay | §8.4, §9 |
| [zone.md](zone.md) | ZLF, E2G, L2P, GLD/GC/AGCB+ | §8.2, §9.4, §11.2, §11.6, §22.2, §24.1 |
| [compact.md](compact.md) | Pipeline CCD: SRB, LZ4, Gear-CDC, FSST, CFX | §10.1–§10.4 |
| [crypto.md](crypto.md) | Suites S0–S3, BLAKE3, SHA-256/HMAC, KATs | §10.5, §10.6, §15, §27.1 |
| [sim.md](sim.md) | vFlash y puerto de host | §20.2, §27.2 |
| [tooling.md](tooling.md) | Build (Make/CMake) y `mfstool` | §26 |
| [testing.md](testing.md) | Plan de verificación, resultados, CI | §27 |
| [linux-integration.md](linux-integration.md) | Integración con Linux: FUSE 3, kernels, `fstab`, systemd/udev, paquetes | §21, §22 |
| [windows-integration.md](windows-integration.md) | Integración con Windows 10/11: WinFsp, letra de unidad, Explorador, servicio, instalador | §21, §22 |
| [known-limitations.md](known-limitations.md) | Desviaciones y alcance no cubierto | — |

## Estado

Implementado y verificado sobre `sim/vflash.c` (NOR) y `sim/vfram.c` (tier T0
byte-addressable): HAL/viabilidad, HWV, WAL+ con tokens y gating por transacción,
CCD (LZ4/Gear/FSST-lite/CFX), suites S0–S3, ZLF con L2P y GC, E2G por registro,
snapshots/FPT, EDP, DAB/CUSUM, HCT con export CBOR+COSE, API POSIX-subset y DAIO.

Subsistemas de Fase 3–4 (código, sin guía separada):

| Módulo | Fichero | Spec |
|---|---|---|
| FTL Ultra 2: WOM-p · SLEC · EBA · RAS+TG · ELM/PEP · WEP · ZRP | `src/ftl/mfs_ftl2.c` | §8.5, §11.1–§11.9 |
| Tiering heterogéneo T0/T1 | `src/tier/mfs_hmt.c` | §11.11 |
| HKDF · PUF · LMS (SP 800-208) | `src/sec/mfs_pq.c` | §15 |
| XDAM · SDP · CQE | `src/xio/mfs_xio.c` | §11.5, §14.1 |

Integración con el sistema operativo (§21, §22):

| Componente | Ficheros | Documento |
|---|---|---|
| Capa portable (mutex, driver L2, adaptador VFS, CLI) | `platform/common/` | — |
| Linux: front-end FUSE 3, helper de `fstab`, systemd, udev, paquetes | `platform/linux/` | [linux-integration.md](linux-integration.md) |
| Windows 10/11: front-end WinFsp, servicio de automontaje, instalador | `platform/windows/` | [windows-integration.md](windows-integration.md) |

Suite de verificación: `make test` (KAT §27.1, puertas §27.7, funcional, FIH
§27.2, FTL 2, HMT, PQ, XIO, estrés, formal y **capa de integración VFS**) y
`mfstool bench` / `mfs_tests.exe --extreme` (MFS-Bench §17). Total:
**935 checks / 0 fallos**.

Validación de los front-ends de plataforma: **montaje real en Linux** con
`mount -t matrixfs` (FUSE 3) — **39 OK / 0 fallos** — e **interoperabilidad
bidireccional Linux ↔ Windows**; la compilación de los binarios de Windows
(MSVC `/W4`) y de Linux (gcc `-Werror`) queda sin avisos. El montaje real en
Windows queda pendiente por requerir sesión elevada. Detalles en
[testing.md](testing.md).
