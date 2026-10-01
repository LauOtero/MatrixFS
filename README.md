<div align="center">

# 🟩 MatrixFS «ATLAS»

### *Deterministic, transactional and crypto-agile embedded file system*

**NOR / NAND / FRAM / MRAM / SD-eMMC — from 512 B of RAM (8, 16, 32 and 64-bit)**

*Implementation of the MFS-SPEC-003 Edition 1.0 «ATLAS» technical specification*

[![License: Apache 2.0][badge-license]](LICENSE)
[![C Standard: C11][badge-c11]](https://en.cppreference.com/w/c/11)
[![Heap: ZERO][badge-heap]](#-normative-guarantees)
![MISRA C:2012-oriented][badge-misra]
[![Status: Phases 1–7 implemented][badge-status]](#-roadmap-and-status)
![Arch: 8/16/32/64-bit][badge-arch]
[![Tests: 1232 passing][badge-tests]]()

[🇺🇸 English](README.md) · [🇪🇸 Español](README.es.md) · [🇩🇪 Deutsch](README.de.md) · [🇫🇷 Français](README.fr.md) · [🇨🇳 中文](README.zh-CN.md) · [🇯🇵 日本語](README.ja.md) · [🇧🇷 Português](README.pt-BR.md) · [🇷🇺 Русский](README.ru.md) · [🇰🇷 한국어](README.ko.md) · [🇮🇹 Italiano](README.it.md)

*This README is written in **English** (base language) in [`README.md`](README.md); the header language selector links to the other language versions generated automatically with **GitHub Actions** on every change. Language configuration in [`.github/languages.yaml`](.github/languages.yaml).*

</div>

---

## 📋 Table of Contents

- [📖 What is MatrixFS?](#-what-is-matrixfs)
- [✨ Key features (the 12 pillars)](#-key-features-the-12-pillars)
- [🧠 Advanced subsystems (detail)](#-advanced-subsystems-detail)
- [🏗️ Repository architecture](#-repository-architecture)
- [🚀 Quick start](#-quick-start)
- [🔒 Normative guarantees](#-normative-guarantees)
- [🧪 Verification (§27)](#-verification-27)
- [📊 Extreme performance results (MFS-Bench §17)](#-extreme-performance-results-mfs-bench-17)
- [🛠️ Tools](#-tools)
- [🖥️ Supported platforms and ecosystems](#-supported-platforms-and-ecosystems)
- [🗺️ Roadmap and status](#-roadmap-and-status)
- [🌐 Multilingual](#-multilingual)
- [📚 Documentation](#-documentation)
- [📣 Report improvements, bugs and deviations](#-report-improvements-bugs-and-deviations)
- [⚖️ License](#-license)

---

## 📖 What is MatrixFS?

> [!IMPORTANT]
> **MatrixFS** is a file system designed for industrial devices, critical IoT, automotive, medical equipment and secure logging, where **reliability, media lifespan, energy, security and determinism weigh as much as performance**. It operates directly on non-volatile memory controllable by the MCU:

| Media | Interfaces | Notes |
|:---|:---|:---|
| **NOR SPI/QSPI/OSPI** | XIP optional | Byte-addressable, WEP by entropy |
| **NAND raw / ONFI / Toggle** | 8/16 data bus | Optional ZNS-like zone semantics |
| **FRAM / MRAM / EEPROM** | I²C/SPI | No block erase, simplified E2G |
| **SD / eMMC 5.1** | CMD/DAT, CQE | Bad-block table, aligned DMA |
| **UFS 3.1/4.0** | UniPro / UFSHCI | JEDEC JESD220; MANAGED engine, TRIM/UNMAP |
| **NVMe / SATA SSD** | PCIe / AHCI | MANAGED engine; `deallocate`/`unmap` (DSM) |
| **Heterogeneous dual-media** | NVM + block flash | A single directory tree (HMT) |

> [!NOTE]
> For **managed** drives (SD/eMMC/UFS/USB/NVMe/SATA) the physical mapping,
> *wear leveling*, *bad block management* and ECC are resolved by the device
> itself; MatrixFS allocates per cluster and **does not duplicate** its FTL. For
> **RAW** media (NOR/NAND/FRAM/MRAM/EEPROM) the FTL belongs to the core. Details in
> [💾 DOCS/storage-integration.md](DOCS/storage-integration.md).

Its distinctive trait: **every promise becomes an auditable artifact** — with no dynamic heap certified at build-time, honest viability (`MFS_ENOTVIABLE`/`MFS_EARCH` instead of silent degradation), and verified recovery after power cuts.

> [!TIP]
> ✅ **Support for 8, 16, 32 and 64-bit** — normative rule **MFS-ARCH-010 rev. 3**, with **autodetection and autoconfiguration**. The core classifies the target (declared or `MFS_ARCH_AUTO`), **detects the hardware acceleration capabilities** (CRC-32C by instruction, AES/SHA/CLMUL/SIMD/RNG/CAS) and adapts the class, the RAM budget and the suite. 8-bit MCUs (AVR, 8051, STM8, PIC16/18, Z80) use the **8-bit** modes (≤ 2 KB of RAM, no AEAD, CRC-32C with *nibble* table); 16/32/64-bit targets use the classic family (Ultra-Nano … Extended). Only an unknown architecture class (`arch_class > 3`) is rejected (`MFS_EARCH`).

---

## ✨ Key features (the 12 pillars)

| # | Pillar | Implementation |
|:--|:---|:---|
| 1 | **Secure autoconfiguration** | HAL cascade §5.1 (JEDEC/SFDP/CFI/assets), **architecture and HW accelerator detection** (MFS-ARCH-010 rev.3), HWV of 64 B persisted @LBA 512 with CRC-32C |
| 2 | **Certified static memory** | RSC contracts §7, static pools, per-mode overlay, zero `malloc()` (MFS-RES-001) |
| 3 | **Honest viability** | Firmware+stack+periph+margin ≥10 % analysis (§6.1); explicit rejection without boot |
| 4 | **WAL+ v3 transactionality** | A/B tokens with thermometric counter TFC, checkpoint with BLAKE3 root, replay bounded by BMT bound (§9) |
| 5 | **CCD v2 pipeline** | CFX (cuckoo) dedup + CDC Gear, LZ4/FSST-lite compression, S0–S3 suites (§10) |
| 6 | **FTL Ultra 2** | Log-structured ZLF, L2P, AGCB+ on GLD debt, E2E E2G, WOM-p, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP RS(16,15) (§8.2, §8.5, §11) |
| 7 | **Energy resilience** | EDP 5-level draining, deterministic DAB bandit EXP3, ELD budget (§12.3, §24.4, §13.3) |
| 8 | **Real time** | DAIO v2 with deadlines, RT-A/B/C classes, TCB-DA, p99.9 ≤ 3 ms RT-A (§13) |
| 9 | **Industrial security** | Crypto-agility S0→S3, HKDF-SHA256, PUF (fuzzy extractor) and hash-based post-quantum anchor LMS (SP 800-208), zero secrets at rest (§15) |
| 10 | **HCT v2 observability** | Exportable health telemetry CBOR+COSE, KPIs tied to MFS-Bench v2 (§16, §17) |
| 11 | **Lifecycle** | MFS-Snap O(1) snapshots, atomic FPT OTA patch with instant revert (§10.8) |
| 12 | **Verifiable determinism** | Budgets from `T_max`, model-checked automata, WCET by stack-painting (§24) |

### 🎛️ Operating modes and normative limits (§18.2)

| Mode | Minimum RAM | Stack | Chunk | Scenario |
|:---|---:|---:|---:|:---|
| **8-bit Ultra** | 512 B | 32 B | 64 B | AVR/8051/STM8/PIC: metadata + CRC only |
| **8-bit Nano** | 1 KB | 48 B | 128 B | 8-bit MCU with integrity (CRC-32C) |
| **8-bit Compact** | 2 KB | 64 B | 256 B | 8-bit MCU with more files/snapshots |
| **Ultra-Nano** | 720 B | 64 B | 128 B | 16/32-bit MCUs 8 KB (STM32F0/F1, MSP430FR) |
| **Nano** | 1.5 KB | 96 B | 256 B | 16/32-bit MCUs 8–16 KB |
| **Compact** | 3.5 KB | 256 B | 512 B | 16/32-bit MCUs 16–20 KB |
| **Balanced** | 11.5 KB | 512 B | 4096 B | 16/32-bit MCUs 64 KB+ |
| **Extended** | 21.5 KB | 1 KB | 4096 B | 16/32/64-bit: dual-media, PQ, certifiable ML |

> [!NOTE]
> The **8-bit** modes form an independent family; the selector picks them
> automatically when `arch_class == 0`. They are not compared in order with the
> classic modes (see `mfs_mode_is_8bit()` in the public header).

---

## 🧠 Advanced subsystems (detail)

In addition to the 12 pillars, MatrixFS implements advanced capabilities that
support its industrial guarantees. They are documented here so that the reader
can audit the real surface of the system.

### 🧩 Architectures and hardware acceleration (MFS-ARCH-010 rev. 3)
- **8/16/32/64-bit classes** with a single model in the core
  (`src/core/mfs_arch.c`) and autodetection (`MFS_ARCH_AUTO`) or explicit
  declaration in `mfs_config.arch_class`.
- **Core port** per architecture (`src/core/mfs_port_arch.c`): AVR, 8051,
  STM8, PIC16/18, Z80 and a generic C *fallback* (16/32/64-bit).
- **Accelerator detection** (`MFS_HWACCEL_*`): CRC-32C by instruction, AES,
  SHA-256, CLMUL, SIMD, RNG and atomic CAS; compiler macros + runtime
  refinement (`__builtin_cpu_supports`/CPUID).
- **Effective acceleration**: CRC-32C uses the CRC32 instruction (x86 SSE4.2 /
  ARMv8 CRC32) when it exists, with a result identical to the software table.
- **Capability honesty** (MFS-HW-001): the HWV only declares what the core can
  *execute*; the rest is reported as diagnostics.
- **Minimum RAM modes** for 8 bits (Ultra/Nano/Compact) and conditional sizing
  of pools and *scratch* with `MFS_ALLOW_8BIT_TARGET`.

### 🔄 Transactionality (WAL+ v3)
- **A/B tokens** with a **Thermometric Counter (TFC)**.
- **Checkpoint** with a **BLAKE3** root for fast recovery.
- **Deterministic replay** bounded by the **BMT bound** (Bit Map Table).
- **O(1) snapshots and clones** via `mf_snap_create()`.
- **FormalCore**: formal verification of the replay and of the WAL+ invariants.

### 💾 FTL Ultra 2
- **ZLF** (Zoned Log-Structured File) log-structured.
- **L2P** (Logical-to-Physical) mapping.
- **AGCB+** (Adaptive Garbage Collection Bandit) on GLD debt.
- **WOM-p** for FRAM/MRAM (Write-Once Memory, Feistel 16 b).
- **SLEC**, **EBA**, **WEP**, **ZRP RS(16,15)** (Reed-Solomon).
- **RAS + Thermal Governor** for thermal management.
- **ELM/PEP** (Endurance Lifetime Model / Predictor).
- **Bad-block table** for SD/eMMC.

### ⚡ Energy management (EDP)
- **5-level** draining model.
- Budgeted **ELD** (Energy Leakage Debt).
- **Deterministic DAB bandit EXP3** for energy allocation.

### ⏱️ Deterministic I/O (DAIO v2)
- **RT-A / RT-B / RT-C** classes with *deadlines*.
- **TCB-DA** (Time-Constrained Bandit for Deadline Allocation).
- Guaranteed latency **p99.9 ≤ 3 ms on RT-A**.
- **WCET** verified by *stack-painting* and *model-checked* automata.

### 🔐 Crypto-agile security
- **S0–S3** suites: AES-256-CTR+HMAC-SHA256 · AES-256-GCM · Ascon-128a · ChaCha20-Poly1305.
- **HKDF-SHA256** for key derivation.
- **Normative nonce** (epoch × monotonic seq) and zeroization of keys/plaintext.
- **Suite negotiation** at mount time and **scheduled deprecation** with *rewrap* migration.
- **LMS post-quantum anchor** (NIST SP 800-208).
- **PUF** with a *fuzzy extractor* for key anchoring.

### 🗜️ Compression and deduplication (CCD v2)
- **Deduplication with Cuckoo filter (CFX)**.
- **CDC** (Content-Defined Chunking) with **Gear hash**.
- **LZ4** and **FSST-lite** at page level.

### 🧬 Heterogeneous media (HMT)
- **T0/T1 heterogeneous tiering** (e.g. FRAM + NAND).
- Single directory tree over both media.
- Support for optional **ZNS-like NAND**.

### 📡 Extended I/O (XIO)
- **XDAM** (Extended Direct Access Mapping).
- **SDP** (Sensor→DMA/CRC→pool→O_RAW).

---

## 🏗️ Repository architecture

```text
matrixfs-ultra/
├── DOCS/                                  # 📄 Especificación normativa completa (MFS-SPEC-003)
│   └── MatrixFS - Technical Specifications and Implementation Guide.md
├── include/matrixfs/                      # 🔌 Cabeceras públicas (contrato estable)
│   ├── mfs_types.h                        #    Tipos, 29 códigos de estado, modos, clases RT y de arquitectura
│   ├── mfs_port.h                         #    Contrato de puerto §20 (HAL ops, driver L2, HWV)
│   └── matrixfs.h                         #    API pública §21 (POSIX-subset + VIO/DAIO + tx)
├── src/                                   # ⚙️ Núcleo (sin heap, MISRA-oriented)
│   ├── mfs_internal.h                     #    Layout on-flash §22, FSMs §24, pools
│   ├── core/
│   │   ├── mfs_arch.c                     #    §3.1 MFS-ARCH-010 rev.3: clases 8/16/32/64-bit + aceleración HW
│   │   ├── mfs_port_arch.c                #    §20.2 puerto del núcleo (AVR/8051/STM8/PIC/Z80/genérico/host)
│   │   ├── mfs_port_rtos.c                #    §20.2 puerto RTOS genérico (FreeRTOS/Zephyr/ThreadX + registro)
│   │   ├── mfs_hal.c                      #    §5.1 cascada · §6 viabilidad · §10.6 suites
│   │   ├── mfs_util.c                     #    CRC-32C · HWV serialize/validate · TFC §8.4
│   │   ├── mfs_zone.c                     #    §8.2 E2G · §11 ZLF/L2P · §9.4 GLD/AGCB+ · §13.3 ELD
│   │   ├── mfs_wal.c                      #    §9 WAL+ tokens T1/T0 · replay BMT · checkpoint Merkle
│   │   ├── mfs_compact.c                  #    §10 SRB/LZ4/Gear-CDC/FSST-lite/CFX dedup
│   │   └── mfs_fs.c                       #    §21 API · montaje FSM §24.2 · POSIX · tx · snapshots · fsck
│   ├── ftl/
│   │   └── mfs_ftl2.c                     #    §11 FTL Ultra 2: WOM-p · SLEC · EBA · RAS+TG · ELM/PEP · WEP · ZRP
│   ├── tier/
│   │   └── mfs_hmt.c                      #    §11.11 Tiering heterogéneo T0/T1 (FRAM/MRAM + NAND)
│   ├── sec/
│   │   └── mfs_pq.c                       #    §15 HKDF-SHA256 · PUF (fuzzy extractor) · LMS SP 800-208
│   ├── xio/
│   │   └── mfs_xio.c                      #    §14.1 XDAM · SDP (sensor→DMA/CRC→pool→O_RAW) · §11.5 CQE
│   └── crypto/
│       ├── mfs_sha256.c                   #    FIPS 180-4 + HMAC RFC 4231 (streaming, sin heap)
│       ├── mfs_blake3.c                   #    BLAKE3-256 completo (hash/keyed/árbol) + MAC tokens
│       └── mfs_suites.c                   #    Suites S0–S3: AES-CTR+HMAC · AES-GCM · Ascon-128a · ChaCha20-Poly1305
├── platform/                              # 🖥️ Integración con SO y ecosistemas (§21, §22)
│   ├── common/                            #    Capa portable compartida Linux + Windows
│   │   ├── mfs_plat.{h,c}                 #      Mutex (CRITICAL_SECTION/pthread) y tiempo
│   │   ├── mfs_blk.{h,c}                  #      Driver L2: imagen · /dev/sdX · MTD · \\.\X: (RMW alineado)
│   │   ├── mfs_l2_managed.{h,c}           #      Driver L2 para medios gestionados (sectores + TRIM, RMW)
│   │   ├── mfs_vfs.{h,c}                  #      Adaptador VFS: montaje, formato, E/S, permisos POSIX
│   │   └── mfs_vfsctl.c                   #      CLI de validación (format/probe/ls/cat/verify)
│   ├── embedded/                          #    Capa embebida común (MCU): L2 sobre región de flash
│   ├── arduino/                           #    Librería Arduino (ESP32, ESP8266, RP2040) + ejemplos
│   ├── esp-idf/                           #    Componente externo de ESP-IDF (esp_partition, Kconfig)
│   ├── platformio/                        #    Proyecto PlatformIO de ejemplo
│   ├── micropython/                       #    Usermod de MicroPython (módulo `matrixfs`)
│   ├── rtos/                              #    Plantilla de portado RTOS (mfs_rtos_port_template.c)
│   ├── linux/                             #    Linux 5.4+/6.x/7.x: FUSE 3, fstab, systemd, udev, deb/rpm
│   └── windows/                           #    Windows 10/11: WinFsp, letra de unidad, servicio, Inno Setup
├── sim/                                   # 🧪 vFlash/vFRAM host (NOR/NAND + byte-addressable T0)
│   ├── vflash.h                           #    Geometría, crash/recover, inyección de fallos
│   └── vfram.h                            #    Tier T0 byte-addressable (FRAM/MRAM), sin erase
├── tests/                                 # ✅ Plan de verificación §27 (KATs, FIH, estrés, extremos, formal, VFS)
├── tools/                                 # 🔧 mfstool: manifiestos, trazas HCT, bench, analizador
├── Makefile · CMakeLists.txt              # 🛠️ Build de host (lib, suite, mfstool, mfsctl) + drivers MCU (`make mcu`)
├── LICENSE                                # ⚖️ Apache 2.0
├── CONTRIBUTING.md                        # 🤝 Proceso de contribución y notificación de mejoras
├── CHANGELOG.md                           # 📋 Mejoras y cambios por versión
└── README.md                              # ← estás aquí
```

---

## 🚀 Quick start

### 📦 Requirements

- C11 compiler (`gcc ≥ 9`, `clang ≥ 12`, or IAR/ARMCC for targets)
- `make` or `cmake ≥ 3.16`
- Linux/macOS/Windows-WSL host for simulation and tests
- For MCU: the target toolchain (`arm-none-eabi-gcc`, `avr-gcc`, `sdcc`,
  `xc8`, …). The core is platform-independent; only the media L2
  driver needs to be provided (the generic MCU drivers are in `platform/common/`).

### 🔨 Build (host + vFlash)

```bash
git clone https://github.com/<org>/matrixfs-ultra.git
cd matrixfs-ultra
make            # genera libmatrixfs.a + binarios de test sobre sim/vFlash
make test       # ejecuta KATs §27.1 y suite de ciclo de vida
make bench      # MFS-Bench v2 (W1–W11) sobre vFlash
make mcu        # drivers L2 para MCU (libmatrixfs_mcu.a)
```

### 🧑‍💻 Minimal usage example

```c
#include <matrixfs/matrixfs.h>
/* mf_t es OPACO en la API pública: la instancia se declara con la definición
 * interna (la misma vía que usan las integraciones de plataforma). */
#include "mfs_internal.h"

/* 1. Driver L2 + geometría del medio (los aporta el BSP / integración) */
extern const mfs_l2_driver  my_spi_nor_driver; /* read/prog/erase/suspend */
extern const mfs_media_geom my_nor_geom;        /* 16 MiB NOR, 4 KiB bloques */

/* 2. Configuración de instancia (mfs_config, §21.1) */
static mfs_config cfg;
memset(&cfg, 0, sizeof cfg);
cfg.drv             = &my_spi_nor_driver;
cfg.geom            = &my_nor_geom;
cfg.ram_total       = 16u * 1024u;        /* RAM física REAL del MCU */
cfg.arch_class      = 2u;                 /* 0=8-bit · 1=16-bit · 2=32-bit · 3=64-bit · 0xFF=auto */
cfg.forced_mode     = MFS_MODE_UNSUPPORTED;/* automático (§6.2) */
cfg.suite_preferred = 0xFFu;              /* negociar (§10.6) */
cfg.key             = NULL;               /* NULL ⇒ sin cifrado */

/* 3. Montaje: detección → HWV → viabilidad → FSM §24.2 */
static mf_t fs;
int st = mf_init(&fs, &cfg);
if (st == MFS_ENOTVIABLE) { /* presupuesto RAM insuficiente: NO arranca */ }
if (st == MFS_ECORRUPT)   { mf_format(&fs, NULL); st = mf_init(&fs, &cfg); }

/* 4. Escritura (commit T1 = durable antes de retornar) */
mfs_file *f = NULL;
mf_open(&fs, "/log/event.bin", MFS_O_WRONLY | MFS_O_CREAT | MFS_O_TRUNC, &f);
size_t wr = 0;
mf_write(f, payload, len, &wr);
mf_close(f);
mf_sync(&fs);                 /* WAL token A+B · E2G · checkpoint */

/* 5. Snapshot O(1) y telemetría de salud */
mfs_snap_id snap;
mf_snap_create(&fs, &snap);
mfs_health_t h;
mf_ioctl(&fs, MFS_IOCTL_HEALTH, &h); /* WAF, TG, ELD, deuda GLD, errores */
mf_deinit(&fs);
```

### 🎯 Cross-compilation to target

The core has no platform dependencies: it is compiled with the MCU
toolchain. The **port** (§20.2) and the **architecture model** already live in the core
(`src/core/mfs_port_arch.c`, `src/core/mfs_arch.c`); only the media L2
driver needs to be provided. For 8-bit targets, define `MFS_ALLOW_8BIT_TARGET=1`,
which enables the 8-bit modes and reduces pools and *scratch* to a minimum.

```bash
# Drivers L2 para MCU (NOR/FRAM/EEPROM SPI-I2C, flash interna, SD-SPI)
make mcu                          # libmatrixfs_mcu.a
cmake -DMATRIXFS_BUILD_MCU=ON ..  # equivalente en CMake

# Cross-compilación del núcleo a un MCU de 8 bits (ejemplo AVR)
avr-gcc -std=c11 -Os -DMFS_ALLOW_8BIT_TARGET=1 ... # (usa tu toolchain real)
```

### 🔌 Integration with embedded ecosystems

In addition to the host (Linux/Windows), the repository includes ready-made integrations
for the most widely used 32-bit MCU ecosystems:

| Ecosystem | Folder | Usage |
|:---|:---|:---|
| **Arduino** (ESP32, ESP8266, RP2040) | [`platform/arduino/`](platform/arduino/) | `MatrixFS` C++ library + examples; reserves a flash partition/region |
| **ESP-IDF** (Espressif) | [`platform/esp-idf/`](platform/esp-idf/) | External component on top of `esp_partition` + `Kconfig` |
| **PlatformIO** | [`platform/platformio/`](platform/platformio/) | Example project that consumes the Arduino library |
| **MicroPython** | [`platform/micropython/`](platform/micropython/) | Usermod with the `matrixfs` module (`MatrixFS`, `File`) |

All of them share the [`platform/embedded/`](platform/embedded/) layer (L2 driver
over a flat flash region) and provide the mandatory port primitives
(`mfs_port_*`) and an L2 driver over the device flash. Full guide:
[📘 DOCS/embedded-integration.md](DOCS/embedded-integration.md).

**8-bit support is integrated into the core**, just like 16/32/64: the
per-architecture port (AVR, 8051, STM8, PIC16/18, Z80) lives in
[`src/core/mfs_port_arch.c`](src/core/mfs_port_arch.c) and the detection and
autoconfiguration of architecture/accelerators in
[`src/core/mfs_arch.c`](src/core/mfs_arch.c). The device drivers
(NOR/FRAM/EEPROM SPI-I2C, internal flash, SD-SPI) are in
[`platform/common/mfs_l2_8bit.c`](platform/common/mfs_l2_8bit.c).

> [!WARNING]
> ⚠️ The integrations have been reviewed by static inspection and structure the
> build correctly, but **have not been compiled with the third-party SDKs**
> (Arduino-ESP32, ESP-IDF, MicroPython) in this repository. Each `README`
> details what remains unverified.

### 🐧 Mounting a volume on Linux (FUSE 3)

```bash
cd platform/linux && make && sudo make install
matrixfs-mkfs -L DATOS /dev/sdb1              # crear y formatear
sudo mount -t matrixfs /dev/sdb1 /mnt/datos   # montar
# o añadir a /etc/fstab:  /dev/sdb1 /mnt/datos matrixfs defaults 0 0
```

The udev/systemd automount mounts the volume at
`/run/media/matrixfs/<device>` when the media is connected. Full guide:
[🐧 DOCS/linux-integration.md](DOCS/linux-integration.md).

### 🪟 Mounting a volume on Windows 10/11 (WinFsp)

```powershell
cd platform\windows
.\build.ps1            # requiere WinFsp SDK + Visual Studio
.\install.ps1          # copia los binarios y registra el servicio de automontaje
matrixfs-ctl.exe format D:\vol0.img --label DATOS
matrixfs_winfsp.exe D:\vol0.img X:            # o conectar el medio (automontaje)
```

The drive appears in File Explorer with native operations
(copy, paste, delete, rename). Full guide:
[🪟 DOCS/windows-integration.md](DOCS/windows-integration.md).

---

## 🔒 Normative guarantees

These rules are **auditable at build and runtime**, not aspirational:

| ID | Rule | Mechanism |
|:---|:---|:---|
| **MFS-RES-001** | Zero dynamic heap | Static pools; symbol map review (`check-map`) |
| **MFS-ARCH-010 rev. 3** | 8/16/32/64-bit classes | Autodetection of class and of HW accelerators; `MFS_EARCH` rejection only if `arch_class > 3` |
| **MFS-VIA-001/002** | Honest viability | Breakdown fw+stack+periph+margin ≥10 % → `MFS_ENOTVIABLE` |
| **MFS-BUS-001..004** | Non-interference | READ-ONLY bus measurement; zero shared persistent RAM |
| **MFS-SEC-001** | Normative nonce | epoch × monotonic seq; never reuse |
| **MFS-SEC-002** | Zeroization | Keys/nonce/plaintext wiped after use |
| **MFS-SEC-005** | EtM in S0 | Encrypt-then-MAC (AES-256-CTR + HMAC-SHA256) |
| **MFS-HW-001** | SW fallback | Autodetected accelerators (CRC-32C by instruction) with an equivalent and identical SW path |
| **MFS-B3-001** | BLAKE3 compliant | Standard tree mode; published KAT |
| **MFS-SCOPE-001** | Tied claims | Every citable KPI ↔ MFS-Bench v2 scenario |

**Cryptographic suites (§10.6):** `S0` AES-256-CTR+HMAC-SHA256 · `S1` AES-256-GCM · `S2` Ascon-128a (SP 800-232) · `S3` ChaCha20-Poly1305 (RFC 8439). Negotiation at mount time according to HWV; scheduled deprecation with rewrap migration.

---

## 🧪 Verification (§27)

- **Full suite:** **1,232 checks, 0 failures** (`mfs_tests.exe`), run **on Linux and on Windows**, including KAT, viability gate, FIH, functional extremes, stress, formal determinism, the **VFS integration layer** (POSIX permissions, metadata, I/O and persistence on a real medium, the same path used by FUSE and WinFsp), the **managed medium** on the L2 adapter (RMW + TRIM, simulated device) and the **RTOS port** (detection, registration and §20.2 contract).
- **Real mount on Linux:** `mount -t matrixfs` on FUSE 3 (libfuse3 3.17.2) with **39 checks / 0 failures**: 24 MiB random file intact (`cmp` and md5), `cp`, `truncate` with prefix intact, `rename`, deletion, persistent `chmod`/`chown`, remount with identical md5 and read-only mount (`EROFS`).
- **Linux ↔ Windows interoperability:** the same on-flash *layout* is read in both directions (volume created on Windows read on Linux and vice versa), with `verify=MFS_OK` and identical content.
- **Verified native compilation:** the whole project with `-std=c11 -Wall -Wextra -Werror` on Linux (gcc 14.2) and Windows; `matrixfs_fuse` linked against real **libfuse3 3.17.2**; the three Windows binaries compiled with **MSVC 14.51 `/W4` without warnings** against the real WinFsp SDK. Details and pending steps (privileges) in [🧪 DOCS/testing.md](DOCS/testing.md).
- **KATs:** CRC-32C `"123456789"` = `0xE3069283`; SHA-256/HMAC (RFC 4231); HKDF-SHA256 (RFC 5869 case 1); official BLAKE3 vectors; AEAD S0–S3 round-trip.
- **Invariants:** WAL↔data ordering, E2G integrity, TFC monotonicity, L2P↔ART coherence.
- **FIH (fault injection):** 10⁵ random cuts without corruption; 10⁴ EDP drainings; accelerated block fatigue; sweep of 1,000 cuts with canary file intact.
- **Extremes:** churn with GC/WAF, metadata storm, S0–S3 matrix with encryption and remount, zone exhaustion with typed errors.
- **Conformity:** §27 matrix with the fingerprint of each MFS-* rule on the code.
- **Formal (FormalCore):** deterministic replay (DAB/CUSUM/EDP) and 64 WAL+ resume combinations without violations.

Run everything: `make test && make fih-short` · Performance: `mfs_tests.exe --extreme` (the full FIH requires nightly CI).

---

## 📊 Extreme performance results (MFS-Bench §17)

Measurement on the host port with NOR `vFlash` (1 MiB, 4 KiB sector), `-O2` build. Executable: `mfs_tests.exe --extreme`.

### 🚀 Performance by mode

| mode | chunk B | zones | write MB/s | read MB/s | mount µs | WAF | status |
|:---|---:|---:|---:|---:|---:|---:|:--:|
| Ultra-Nano | 108 | 128 | 32.69 | 130.96 | 690 | 1.04 | OK |
| Nano | 236 | 128 | 23.97 | 168.80 | 842 | 1.08 | OK |
| Compact | 492 | 128 | 20.55 | 183.48 | 1,356 | 1.15 | OK |
| Balanced | 4,076 | 126 | 17.24 | 236.48 | 1,974 | 2.04 | OK |
| Extended | 4,076 | 126 | 19.20 | 236.48 | 2,015 | 2.04 | OK |

### 🔐 Cryptographic throughput per suite (§10.6)

| suite | seal MB/s | open MB/s | tag B | note |
|:---|---:|---:|---:|:---|
| S0 AES-256-CTR + HMAC-SHA256 | 4.8 | 68.5 | 16 | EtM/AEAD verified |
| S1 AES-256-GCM | 1.4 | 2.1 | 16 | EtM/AEAD verified |
| S2 Ascon-128a | 1.6 | 1.6 | 16 | EtM/AEAD verified |
| S3 ChaCha20-Poly1305 | 75.9 | 426.3 | 16 | EtM/AEAD verified |

### 🧮 Cost per FTL/security subsystem operation (§11, §15)

| operation | cost µs | note |
|:---|---:|:---|
| `crc32c(4 KiB)` | 16.29 | E2G integrity |
| `HKDF-SHA256` | 7.40 | key derivation (§15) |
| `ELM health` | 0.010 | lifetime model §11.1 |
| `PEP score (4×32)` | 0.236 | WCET < 5 µs §11.1 |
| `LMS keygen (H=8)` | 249,055 | post-quantum anchor §15 (once, enrollment) |
| `LMS verify` | 456 | secure-boot (once per boot) |
| `ZRP encode (16 pág)` | 12.0 | +6.25 % in cold zones §8.5 |
| `ZRP recover (1 pág)` | 12.0 | RS(16,15) reconstruction, level 5 §12 |
| `WEP place (caché)` | 0.094 | Feistel 16 b §11.2 Balanced+ |

### 🛡️ WAF and durability per workload (§17.1/§17.3)

| workload | ops | WAF | GC reloc | result |
|:---|---:|---:|---:|:--:|
| W1 sequential append | 260 | 1.188 | 0 | MFS_OK |
| W3 random within file | 400 | 1.161 | 780 | MFS_OK |
| W4 mixed churn 90/10 | 260 | 1.254 | 138 | MFS_OK |

### 📐 Static memory footprint (MFS-RES-001: no heap)

| structure | bytes | usage |
|:---|---:|:---|
| `mf_t` (core, 64-bit host) | ≈ 17,240 | single instance §23.1 |
| `mf_t` (8-bit build, `MFS_ALLOW_8BIT_TARGET`) | ≈ 1,840 | reduced pools and scratch (≤ 2 KB) |
| `mfs_inode_ram_t` | 148 | flash-first window §22.3 |
| `mfs_zone_t` | 36 | zone table §24.1 |
| `mfs_iocb` | 32 | DAIO ring §13.1 |
| `mfs_wom_t` | 12 | WOM-p §11.4 |
| `mfs_health_t` | 168 | HCT telemetry §16 |
| `mfs_hwv_t` | 76 | HWV (in flash) §5.2 |
| CRC-32C table (16/32/64-bit vs 8-bit) | 1024 → 64 | full table vs *nibble* |

> [!NOTE]
> **Methodological note:** the values are upper bounds measured on host (they are not target WCET); the static footprint is deterministic and verifiable at compile time. Deviations and known limits are detailed in [⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md).

---

## 🛠️ Tools

### 🧰 `mfstool`
CLI for:
- Generating build and deployment **manifests**.
- Analyzing **HCT traces** (Health Check Trace).
- Running the **MFS-Bench v2** test bench.
- Conformity analyzer against MFS-SPEC-003.

### 🧪 `vFlash` / `vFRAM`
Memory simulators for host development:
- Fault injection and **power cuts** (crash/recover).
- Latency and geometry modeling (NOR/NAND).
- **T0 byte-addressable** tier (FRAM/MRAM) without erase.

### ✅ `FormalCore`
- Formal verification of the **deterministic replay** (DAB/CUSUM/EDP).
- **WAL+ invariant** checking (64 resume combinations).

---

## 🖥️ Supported platforms and ecosystems

### 🖥️ Desktop operating systems

| OS | Integration | Status |
|:---|:---|:---|
| **Linux 5.4+/6.x/7.x** | FUSE 3, `fstab`, systemd, udev, deb/rpm | ✅ Verified (39/0) |
| **Windows 10/11** | WinFsp, drive letter, service, Inno Setup | ✅ Compiled · ⏳ real mount pending |

### 🧩 Embedded frameworks

| Framework | Folder | Status |
|:---|:---|:---|
| **Arduino** (ESP32, ESP8266, RP2040) | `platform/arduino/` | 🟡 Implemented · ⏳ not compiled with SDK |
| **ESP-IDF** (Espressif) | `platform/esp-idf/` | 🟡 Implemented · ⏳ not compiled with SDK |
| **PlatformIO** | `platform/platformio/` | 🟡 Implemented · ⏳ not compiled with SDK |
| **MicroPython** | `platform/micropython/` | 🟡 Implemented · ⏳ not compiled with SDK |

### 🏗️ MCU architectures (integrated into the core)

| Class | Port | Modes | Notes |
|:---|:---|:---|:---|
| **8-bit** (AVR, 8051, STM8, PIC16/18, Z80) | `src/core/mfs_port_arch.c` | 8-bit Ultra / Nano / Compact | ≤ 2 KB of RAM, CRC-32C with *nibble* table, no AEAD |
| **16 / 32-bit** (ARM Cortex-M, MSP430, AVR32…) | `src/core/mfs_port_arch.c` (generic) or the integrator's | Ultra-Nano … Extended | Classic family; suites S0–S3 |
| **64-bit** (x86-64, ARM64, RISC-V 64) | `src/core/mfs_port_arch.c` (generic) or the integrator's | Extended as ceiling | Classic suite; **autodetected HW acceleration** |

**Detection and autoconfiguration** (`src/core/mfs_arch.c`) classifies the
architecture (`MFS_ARCH_AUTO` or declared) and detects accelerators
(CRC-32C by instruction, AES, SHA-256, CLMUL, SIMD, RNG, atomic CAS). **CRC-32C
is accelerated automatically** when the CPU exposes the instruction
(x86 SSE4.2 / ARMv8 CRC32, same Castagnoli polynomial ⇒ identical result);
the rest of the capabilities are reported for diagnosis and planning, and their
effective path remains software (MFS-HW-001: a capability that cannot be
executed is never declared).

### 🤖 8-bit MCUs
- **AVR, 8051, STM8, PIC16/18, Z80** via the core port `src/core/mfs_port_arch.c`.
- **8-bit Ultra / Nano / Compact** modes.
- SPI/I²C drivers for NOR/FRAM/EEPROM/internal-flash/SD-SPI (`platform/common/mfs_l2_8bit.c`).
- Capability autodetection and automatic adaptation of the configuration.

### ⏱️ RTOS
- **Generic RTOS port in the core** (`src/core/mfs_port_rtos.c`,
  `include/matrixfs/mfs_port_rtos.h`): maps the `mfs_port_*` contract to the
  RTOS native primitives and is selected by detection at compile
  time.
- **Hardwired native adapters**: **FreeRTOS** (`taskENTER/EXIT_CRITICAL`),
  **Zephyr RTOS** (`irq_lock/unlock`, `k_cycle_get_32`) and **Eclipse ThreadX**
  (`tx_interrupt_control`, `tx_time_get`).
- **Detection + registration** for **Mbed OS, Apache NuttX, RIOT OS, Apache Mynewt,
  RT-Thread and PX5 RTOS**, with the `platform/rtos/mfs_rtos_port_template.c` template.
- Full guide: [⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md).

### 🏭 Vendor SDKs
- Integration through the hook points: SDK flash driver →
  `mfs_embedded` / `mfs_l2_managed` layer, and SDK RTOS → RTOS port.
- **Silicon Labs** (Gecko SDK), **Texas Instruments** (SimpleLink/MSPM0),
  **Infineon** (ModusToolbox), **Renesas** (FSP): guide and hook points in
  [⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md) §4 (depends on the concrete
  SoC).

---

## 🗺️ Roadmap and status

| Phase | Content | Status |
|:---|:---|:---|
| **1 — Core and viability** | Port, HWV, HAL cascade, viability/modes, RSC+pools, extents+L2P, WAL+, UN/Nano/Compact | ✅ Implemented · ✅ KATs and suite green |
| **2 — Differentiation** | GLD/WOM-p/TFC/E2G/ZLF/HCT/CCD/AGCB+/CFX+CDC/MFS-Snap/FPT/ELD | ✅ Implemented · ✅ MFS-Bench (tables above) |
| **3 — Industrial** | HAWL+ · ELM/WEP/CV/RAS+TG/EBA/EDP/ZRP/PUF/PQ chain/FormalCore + vFlash CI | ✅ Implemented (FormalCore = deterministic replay + WAL+ invariants) |
| **4 — Advanced optimization** | RT-C hint/PEP/SDP/FSST/ZLF-Z/CQE/on-device dictionaries | ✅ Implemented (PEP, SDP, FSST-lite, ZRP, CQE) |
| **5 — Certification** | SIL-2/21434 dossier, suite deprecation, fleet tooling | 🟡 Tooling ready · certification dossier pending (not code) |
| **6 — OS integration** | Linux (FUSE 3, kernels 5.4+/6.x/7.x, `fstab`, systemd, udev, deb/rpm) · Windows 10/11 (WinFsp, drive letter, Explorer, automount service, Inno Setup) | ✅ Implemented · ✅ real mount verified on Linux (39/0) · ✅ Linux↔Windows interoperability · ⏳ real mount on Windows pending (requires elevated session) |
| **7 — Embedded architectures and ecosystems** | **8/16/32/64-bit** classes with autodetection and **HW acceleration autoconfiguration** · port and architecture model in the core (`src/core/mfs_arch.c`, `mfs_port_arch.c`) · MCU drivers (`platform/common/mfs_l2_8bit.c`) · common `platform/embedded` layer · **Arduino, ESP-IDF, PlatformIO and MicroPython** integrations | ✅ Implemented · ✅ 64-bit, accelerated CRC-32C and embedded layer verified on host · ⏳ compilation with third-party SDKs/toolchains pending |
| **8 — RTOS and vendor SDKs** | **Generic RTOS port** in the core with native **FreeRTOS, Zephyr and ThreadX** adapters; detection + registration for Mbed OS, NuttX, RIOT, Mynewt, RT-Thread and PX5; hook points for Silicon Labs, TI, Infineon and Renesas | 🟡 Implemented · ✅ detection/registration/contract verified on host · ⏳ native RTOS adapters not compiled here (no RTOS toolchain) |
| **9 — Complete flash storage** | **SATA** and **UFS** media in the model + profiles; generic **MANAGED L2 adapter** (sectors + TRIM + RMW); managed device simulator and host test | 🟡 Implemented · ✅ verified on host (simulator) · ⏳ silicon drivers (SD/eMMC/UFS/NVMe/SATA) depend on the SoC SDK |

**Current status:** Phase 3–4 subsystems implemented and verified — WOM-p, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP (RS(16,15)), HMT T0/T1 tiering, HKDF/PUF/LMS (SP 800-208), XDAM/SDP/CQE and FormalCore. The operating system integration layer (`platform/linux` and `platform/windows` folders, with the shared portable layer `platform/common`) is implemented and **verified with a real mount on Linux** (39 OK / 0 failures) and **bidirectional Linux ↔ Windows interoperability**. On top of it are added the **8/16/32/64-bit architecture support integrated into the core** (`src/core/mfs_arch.c` + `mfs_port_arch.c`, with detection and autoconfiguration of hardware acceleration), the **L2 drivers for MCU**, the **integrations for Arduino, ESP-IDF, PlatformIO and MicroPython** on top of the common `platform/embedded` layer, the **generic RTOS port** (`src/core/mfs_port_rtos.c`, with native FreeRTOS/Zephyr/ThreadX adapters and registration for the rest) and the **L2 adapter for managed media** (`platform/common/mfs_l2_managed.c`: SD/eMMC/UFS/USB/NVMe/SATA). Full suite **1,232 checks / 0 failures** with `-std=c11 -Wall -Wextra -Werror` without warnings on Linux and Windows. The current limits and deviations are inventoried in [⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md). See [📋 CHANGELOG.md](CHANGELOG.md).

---

## 🌐 Multilingual

This README is written in **English** in [`README.md`](README.md) (base language) and the rest of the languages are
translated automatically with **GitHub Actions**. GitHub does **not** offer native README translation, so the
project implements it with the workflow
[`.github/workflows/translate-readme.yml`](.github/workflows/translate-readme.yml), which uses
[`.github/languages.yaml`](.github/languages.yaml) as the **single source of truth** for the languages.

- **Base (source) language:** 🇺🇸 **English (`en`)** — file [`README.md`](README.md).
- **Generated languages:** �🇸 Spanish (`es`), 🇨🇳 Simplified Chinese (`zh-CN`), 🇩🇪 German (`de`), 🇯🇵 Japanese (`ja`), 🇫🇷 French (`fr`), 🇧🇷 Brazilian Portuguese (`pt-BR`), 🇷🇺 Russian (`ru`), 🇰🇷 Korean (`ko`) and 🇮🇹 Italian (`it`).

On every *push* that modifies `README.md`, the workflow regenerates `README.<code>.md` for each target language and
**commits them automatically**. The header language selector links to those files; relative links, images and code
blocks are preserved because the translation only affects text. The `README.md` of the project subfolders
(`DOCS/`, `platform/*/`, etc.) **are not translated**.

> [!NOTE]
> The workflow requires the `ACTION_BOT` secret (PAT with `repo` and `workflow` scopes) in *Settings → Secrets and
> variables → Actions*. Optionally `OPENAI_API_KEY` (gpt-4o) or `ZHIPUAI_API_KEY` (glm-4-flash) improve the
> quality; without any key the free `g4f` backend is used.

| Language | Code | File | Role |
|:---|:---|:---|:---|
| �🇸 English | `en` | [`README.md`](README.md) | **Base / source** |
| �🇸 Español | `es` | [`README.es.md`](README.es.md) | Auto translation · priority 1 |
| 🇨🇳 中文（简体） | `zh-CN` | [`README.zh-CN.md`](README.zh-CN.md) | Auto translation · priority 2 |
| 🇩🇪 Deutsch | `de` | [`README.de.md`](README.de.md) | Auto translation · priority 3 |
| 🇯🇵 日本語 | `ja` | [`README.ja.md`](README.ja.md) | Auto translation · priority 4 |
| 🇫🇷 Français | `fr` | [`README.fr.md`](README.fr.md) | Auto translation · priority 5 |
| 🇧🇷 Português (Brasil) | `pt-BR` | [`README.pt-BR.md`](README.pt-BR.md) | Auto translation · priority 6 |
| 🇷🇺 Русский | `ru` | [`README.ru.md`](README.ru.md) | Auto translation · priority 7 |
| 🇰🇷 한국어 | `ko` | [`README.ko.md`](README.ko.md) | Auto translation · priority 8 |
| 🇮🇹 Italiano | `it` | [`README.it.md`](README.it.md) | Auto translation · priority 9 |

---

## 📚 Documentation

- **[📐 DOCS/…Guide.md](DOCS/MatrixFS%20-%20Technical%20Specifications%20and%20Implementation%20Guide.md)** — Complete normative specification (31 sections): principles, HAL, viability, physical format, WAL+, CCD pipeline, FTL, EDP/DAIO, security, HCT, modes, implementation guide (API, on-flash structures, FSMs, constants), verification plan.
- **[📘 DOCS/](DOCS/)** — Design and implementation guides per module (architecture, HAL/viability, E2G, WAL/transactions, cryptographic suites, RT/energy, flash layout, port contract, conformity/tests).
- **[🐧 DOCS/linux-integration.md](DOCS/linux-integration.md)** — Linux integration: kernel matrix, build requirements, installation, `fstab`, systemd/udev, deb/rpm packages, troubleshooting and validation.
- **[🪟 DOCS/windows-integration.md](DOCS/windows-integration.md)** — Windows 10/11 integration: WinFsp, MSVC build, drive letter, Explorer operations table, automount service, installer and validation.
- **[🔌 DOCS/embedded-integration.md](DOCS/embedded-integration.md)** — Embedded integration: port and architecture in the core (8/16/32/64-bit), common `platform/embedded` layer, MCU drivers and Arduino, ESP-IDF, PlatformIO and MicroPython ecosystems.
- **[💾 DOCS/storage-integration.md](DOCS/storage-integration.md)** — Flash storage: RAW/ZONED/MANAGED engines, L2 adapter for managed media (SD/eMMC/UFS/USB/NVMe/SATA), examples and validation.
- **[⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md)** — RTOS and silicon platforms: generic RTOS port, native adapters (FreeRTOS/Zephyr/ThreadX), porting template and hook points (Silicon Labs, TI, Infineon, Renesas).
- **[⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md)** — Current limits and deviations from the spec.
- **[🧪 DOCS/testing.md](DOCS/testing.md)** — Verification steps and required privileges.
- **[🤝 CONTRIBUTING.md](CONTRIBUTING.md)** — How to propose improvements, report deviations from the spec and submit KATs.
- **[🔧 tools/](tools/)** — `mfstool`: manifest generator, HCT trace analyzer, MFS-Bench v2 bench, formal matcher.

---

## 📣 Report improvements, bugs and deviations

**MatrixFS** development follows an open and traceable process. Any proposed improvement goes through review against the MFS-* normative rules before merge.

| Type | Channel | Template / required fields |
|:---|:---|:---|
| 🐛 **Bug / corruption** | [GitHub Issues → Bug report](/issues/new?labels=bug) | Reproduction with vFlash, operation sequence, cut point, exported HCT logs |
| 💡 **Improvement / feature** | [GitHub Issues → Enhancement](/issues/new?labels=enhancement) | Affected MFS-SPEC-003 section(s), impact on RSC/stack/WCET, expected MFS-Bench KPI |
| 📏 **Deviation vs. spec** | Issue with `spec-deviation` label | Rule ID (e.g. `MFS-VIA-002`), observed vs. normative behavior |
| 🔐 **Security vulnerability** | **Do NOT open a public issue** — write to `security@matrixfs.example` | Preliminary CVSS, affected suite/media, confidential PoC |
| 📝 **Documentation improvement** | Direct PR or `documentation` issue | Link to section and proposed text |

**Triage policy:** issues labeled in ≤ 3 business days · corruption blocker = P0 priority with mandatory forensic analysis · every accepted improvement is recorded in [📋 CHANGELOG.md](CHANGELOG.md) with a reference to the specification section. External contributions are accepted under Apache 2.0 (see §5 of the LICENSE) and must include DCO sign-off (`git commit -s`).

---

## ⚖️ License

Distributed under the **Apache License 2.0** — see [LICENSE](LICENSE).

**Why Apache 2.0?** MatrixFS targets industrial product (automotive, medical, critical infrastructure). Apache 2.0:

1. **Allows commercial use and proprietary closing** — integrators can use it in production firmware without copyleft, which favors adoption in regulated sectors.
2. **Grants an express patent license** from each contributor (§3) — essential given that the specification defines techniques susceptible to patent (adaptive GC by debt, thermometric counters, multi-level EDP draining, deterministic bandit EXP3).
3. **Requires preserving attribution and notices** (§4) and marking modified files (§4b) — keeps derivations auditable against the MFS-SPEC-003 spec.
4. **No warranty** (§7–8) — consistent with the project's normative reference nature: any SIL-2/ISO 26262 certification requires the integrator's own validation.

The specification documents in `DOCS/` share the same license. The «MatrixFS», «ATLAS» and «MFS-Bench» trademarks are not granted with the license (§6).

© 2026 The MatrixFS contributors.

---

<div align="center">

**MatrixFS «ATLAS»** — *where every promise is an auditable artifact.*

[⬆ Back to top](#-matrixfs-atlas) · [📋 Table of Contents](#-table-of-contents)

</div>

<!-- ==================== Badge definitions (reference-style) ==================== -->
[badge-license]: https://img.shields.io/badge/License-Apache%202.0-blue.svg
[badge-c11]: https://img.shields.io/badge/C%20Standard-C11-blue
[badge-heap]: https://img.shields.io/badge/heap-ZERO-brightgreen
[badge-misra]: https://img.shields.io/badge/MISRA%20C%3A2012-oriented-orange
[badge-status]: https://img.shields.io/badge/status-Fases%201--7%20implementadas-yellow
[badge-arch]: https://img.shields.io/badge/arch-8%2F16%2F32%2F64--bit-orange
[badge-tests]: https://img.shields.io/badge/tests-1232%20passing-brightgreen
