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
| [embedded-integration.md](embedded-integration.md) | Integración embebida: arquitecturas 8/16/32/64-bit en el núcleo (puerto + `mfs_arch`), capa `platform/embedded`, drivers MCU y ecosistemas Arduino, ESP-IDF, PlatformIO y MicroPython | §3.1, §5, §6, §20, §21, MFS-ARCH-010 rev. 3 |
| [storage-integration.md](storage-integration.md) | Almacenamiento flash: motores RAW/ZONED/MANAGED, adaptador L2 para medios gestionados (SD/eMMC/UFS/USB/NVMe/SATA), ejemplo y validación | §3.2, §5.1, §8.1, MFS-CAP-001 |
| [rtos-integration.md](rtos-integration.md) | RTOS y plataformas de silicio: puerto RTOS genérico, adaptadores nativos (FreeRTOS/Zephyr/ThreadX), plantilla de portado y puntos de enganche (Silicon Labs, TI, Infineon, Renesas) | §20.2–§20.3, §13, MFS-HW-001 |
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
| Capa embebida común (L2 sobre región de flash, `mf_t` + montaje) | `platform/embedded/` | [embedded-integration.md](embedded-integration.md) |
| Arquitecturas 8/16/32/64-bit: puerto (AVR/8051/STM8/PIC16-18/Z80/genérico) y detección de arquitectura + aceleración HW | `src/core/mfs_port_arch.*`, `src/core/mfs_arch.c` | [embedded-integration.md](embedded-integration.md) |
| Drivers L2 de dispositivo para MCU (NOR/FRAM/EEPROM SPI-I2C, flash interna, SD-SPI) | `platform/common/mfs_l2_8bit.*` | [embedded-integration.md](embedded-integration.md) |
| Adaptador L2 para medios gestionados (SD/eMMC/UFS/USB/NVMe/SATA: sectores + TRIM + RMW) | `platform/common/mfs_l2_managed.*` | [storage-integration.md](storage-integration.md) |
| Puerto RTOS genérico (FreeRTOS/Zephyr/ThreadX nativos + registro) y plantilla de portado | `src/core/mfs_port_rtos.*`, `platform/rtos/` | [rtos-integration.md](rtos-integration.md) |
| Arduino (ESP32/ESP8266/RP2040), ESP-IDF, PlatformIO, MicroPython | `platform/{arduino,esp-idf,platformio,micropython}/` | [embedded-integration.md](embedded-integration.md) |

Suite de verificación: `make test` (KAT §27.1, puertas §27.7, funcional, FIH
§27.2, FTL 2, HMT, PQ, XIO, estrés, formal, **capa de integración VFS**,
**medio gestionado** y **puerto RTOS**) y `mfstool bench` /
`mfs_tests.exe --extreme` (MFS-Bench §17). Total: **1 232 checks / 0 fallos**.

Validación de los front-ends de plataforma: **montaje real en Linux** con
`mount -t matrixfs` (FUSE 3) — **39 OK / 0 fallos** — e **interoperabilidad
bidireccional Linux ↔ Windows**; la compilación de los binarios de Windows
(MSVC `/W4`) y de Linux (gcc `-Werror`) queda sin avisos. El montaje real en
Windows queda pendiente por requerir sesión elevada. Detalles en
[testing.md](testing.md).
