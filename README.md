<!--
Este README se sirve en español como idioma predeterminado.
La traducción automática de GitHub (README translations) está configurada en
.github/languages.yaml — default: es | en, zh-CN, de, ja, fr, pt-BR, ru, ko, it.
-->

<div align="center">

# 🟩 MatrixFS «ATLAS»

### *Sistema de archivos embebido determinista, transaccional y cripto-agile*

**NOR / NAND / FRAM / MRAM / SD-eMMC — desde 512 B de RAM (8, 16, 32 y 64 bits)**

*Implementación de la especificación técnica MFS-SPEC-003 Edición 1.0 «ATLAS»*

[![License: Apache 2.0][badge-license]](LICENSE)
[![C Standard: C11][badge-c11]](https://en.cppreference.com/w/c/11)
[![Heap: ZERO][badge-heap]](#-garantías-normativas)
![MISRA C:2012-oriented][badge-misra]
[![Status: Fases 1–7 implementadas][badge-status]](#-roadmap-y-estado)
![Arch: 8/16/32/64-bit][badge-arch]
[![Tests: 1232 passing][badge-tests]]()

[🇪🇸 Español](#-matrixfs-atlas) · [🇺🇸 English](#-matrixfs-atlas) · [🇩🇪 Deutsch](#-matrixfs-atlas) · [🇫🇷 Français](#-matrixfs-atlas) · [🇨🇳 中文](#-matrixfs-atlas) · [🇯🇵 日本語](#-matrixfs-atlas) · [🇧🇷 Português](#-matrixfs-atlas)

*Este README usa la **traducción automática de GitHub**: el idioma predeterminado es el **español** y los demás idiomas aparecen en el selector de arriba. Configuración oficial en [`.github/languages.yaml`](.github/languages.yaml).*

</div>

---

## 📋 Índice

- [📖 ¿Qué es MatrixFS?](#-qué-es-matrixfs)
- [✨ Características principales (los 12 pilares)](#-características-principales-los-12-pilares)
- [🧠 Subsistemas avanzados (detalle)](#-subsistemas-avanzados-detalle)
- [🏗️ Arquitectura del repositorio](#-arquitectura-del-repositorio)
- [🚀 Inicio rápido](#-inicio-rápido)
- [🔒 Garantías normativas](#-garantías-normativas)
- [🧪 Verificación (§27)](#-verificación-27)
- [📊 Resultados de rendimiento extremo (MFS-Bench §17)](#-resultados-de-rendimiento-extremo-mfs-bench-17)
- [🛠️ Herramientas](#-herramientas)
- [🖥️ Plataformas y ecosistemas soportados](#-plataformas-y-ecosistemas-soportados)
- [🗺️ Roadmap y estado](#-roadmap-y-estado)
- [🌐 Multiidioma](#-multiidioma)
- [📚 Documentación](#-documentación)
- [📣 Notificar mejoras, errores y desviaciones](#-notificar-mejoras-errores-y-desviaciones)
- [⚖️ Licencia](#-licencia)

---

## 📖 ¿Qué es MatrixFS?

> [!IMPORTANT]
> **MatrixFS** es un sistema de archivos diseñado para dispositivos industriales, IoT crítico, automoción, equipamiento médico y registro seguro, donde **fiabilidad, vida útil del medio, energía, seguridad y determinismo pesan tanto como el rendimiento**. Opera directamente sobre memoria no volátil controlable por el MCU:

| Medio | Interfaces | Notas |
|:---|:---|:---|
| **NOR SPI/QSPI/OSPI** | XIP opcional | Byte-addressable, WEP por entropía |
| **NAND raw / ONFI / Toggle** | Bus de datos 8/16 | Semántica de zonas ZNS-like opcional |
| **FRAM / MRAM / EEPROM** | I²C/SPI | Sin borrado por bloque, E2G simplificado |
| **SD / eMMC 5.1** | CMD/DAT, CQE | Bad-block table, DMA alineado |
| **UFS 3.1/4.0** | UniPro / UFSHCI | JEDEC JESD220; motor MANAGED, TRIM/UNMAP |
| **SSD NVMe / SATA** | PCIe / AHCI | Motor MANAGED; `deallocate`/`unmap` (DSM) |
| **Dual-medio heterogéneo** | NVM + flash de bloques | Un único árbol de directorios (HMT) |

> [!NOTE]
> Para las unidades **gestionadas** (SD/eMMC/UFS/USB/NVMe/SATA) el mapeo físico,
> el *wear leveling*, el *bad block management* y el ECC los resuelve el propio
> dispositivo; MatrixFS asigna por clúster y **no duplica** su FTL. Para los
> medios **RAW** (NOR/NAND/FRAM/MRAM/EEPROM) el FTL es del núcleo. Detalle en
> [💾 DOCS/storage-integration.md](DOCS/storage-integration.md).

Su rasgo distintivo: **cada promesa se convierte en un artefacto auditable** — sin heap dinámico certificado en build-time, viabilidad honesta (`MFS_ENOTVIABLE`/`MFS_EARCH` en lugar de degradación silenciosa), y recuperación verificada ante cortes de energía.

> [!TIP]
> ✅ **Soporte de 8, 16, 32 y 64 bits** — regla normativa **MFS-ARCH-010 rev. 3**, con **autodetección y autoconfiguración**. El núcleo clasifica el objetivo (declarado o `MFS_ARCH_AUTO`), **detecta las capacidades de aceleración por hardware** (CRC-32C por instrucción, AES/SHA/CLMUL/SIMD/RNG/CAS) y adapta la clase, el presupuesto de RAM y la suite. Los MCU de 8 bits (AVR, 8051, STM8, PIC16/18, Z80) usan los modos **8-bit** (≤ 2 KB de RAM, sin AEAD, CRC-32C con tabla de *nibble*); los objetivos de 16/32/64 bits usan la familia clásica (Ultra-Nano … Extended). Sólo se rechaza (`MFS_EARCH`) una clase de arquitectura desconocida (`arch_class > 3`).

---

## ✨ Características principales (los 12 pilares)

| # | Pilar | Implementación |
|:--|:---|:---|
| 1 | **Autoconfiguración segura** | Cascada HAL §5.1 (JEDEC/SFDP/CFI/assets), **detección de arquitectura y aceleradores HW** (MFS-ARCH-010 rev.3), HWV de 64 B persistido @LBA 512 con CRC-32C |
| 2 | **Memoria estática certificada** | Contratos RSC §7, pools estáticos, overlay por modo, cero `malloc()` (MFS-RES-001) |
| 3 | **Viabilidad honesta** | Análisis firmware+stack+perif+margen ≥10 % (§6.1); rechazo explícito sin arranque |
| 4 | **Transaccionalidad WAL+ v3** | Tokens A/B con contador termométrico TFC, checkpoint con raíz BLAKE3, replay acotado por cota BMT (§9) |
| 5 | **Pipeline CCD v2** | Dedup CFX (cuckoo) + CDC Gear, compresión LZ4/FSST-lite, suites S0–S3 (§10) |
| 6 | **FTL Ultra 2** | ZLF log-structured, L2P, AGCB+ sobre deuda GLD, E2G E2E, WOM-p, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP RS(16,15) (§8.2, §8.5, §11) |
| 7 | **Resiliencia energética** | EDP drenaje 5 niveles, DAB bandit determinista EXP3, presupuesto ELD (§12.3, §24.4, §13.3) |
| 8 | **Tiempo real** | DAIO v2 con deadlines, clases RT-A/B/C, TCB-DA, p99.9 ≤ 3 ms RT-A (§13) |
| 9 | **Seguridad industrial** | Crypto-agility S0→S3, HKDF-SHA256, PUF (fuzzy extractor) y ancla post-cuántica hash-based LMS (SP 800-208), cero secretos en reposo (§15) |
| 10 | **Observabilidad HCT v2** | Telemetría de salud exportable CBOR+COSE, KPIs ligados a MFS-Bench v2 (§16, §17) |
| 11 | **Ciclo de vida** | Snapshots O(1) MFS-Snap, parche OTA atómico FPT con revert instantáneo (§10.8) |
| 12 | **Determinismo verificable** | Presupuestos desde `T_max`, autómatas model-checked, WCET por stack-painting (§24) |

### 🎛️ Modos operativos y límites normativos (§18.2)

| Modo | RAM mínima | Stack | Chunk | Escenario |
|:---|---:|---:|---:|:---|
| **8-bit Ultra** | 512 B | 32 B | 64 B | AVR/8051/STM8/PIC: sólo metadatos + CRC |
| **8-bit Nano** | 1 KB | 48 B | 128 B | MCU 8-bit con integridad (CRC-32C) |
| **8-bit Compact** | 2 KB | 64 B | 256 B | MCU 8-bit con más ficheros/snapshots |
| **Ultra-Nano** | 720 B | 64 B | 128 B | MCUs 16/32-bit 8 KB (STM32F0/F1, MSP430FR) |
| **Nano** | 1.5 KB | 96 B | 256 B | MCUs 16/32-bit 8–16 KB |
| **Compact** | 3.5 KB | 256 B | 512 B | MCUs 16/32-bit 16–20 KB |
| **Balanced** | 11.5 KB | 512 B | 4096 B | MCUs 16/32-bit 64 KB+ |
| **Extended** | 21.5 KB | 1 KB | 4096 B | 16/32/64-bit: dual-medio, PQ, ML certificable |

> [!NOTE]
> Los modos **8-bit** forman una familia independiente; el selector los elige
> automáticamente cuando `arch_class == 0`. No se comparan por orden con los
> modos clásicos (ver `mfs_mode_is_8bit()` en la cabecera pública).

---

## 🧠 Subsistemas avanzados (detalle)

Además de los 12 pilares, MatrixFS implementa capacidades avanzadas que
respaldan sus garantías industriales. Se documentan aquí para que el lector
pueda auditar la superficie real del sistema.

### 🧩 Arquitecturas y aceleración por hardware (MFS-ARCH-010 rev. 3)
- **Clases de 8/16/32/64 bits** con un único modelo en el núcleo
  (`src/core/mfs_arch.c`) y autodetección (`MFS_ARCH_AUTO`) o declaración
  explícita en `mfs_config.arch_class`.
- **Puerto del núcleo** por arquitectura (`src/core/mfs_port_arch.c`): AVR, 8051,
  STM8, PIC16/18, Z80 y *fallback* C genérico (16/32/64 bits).
- **Detección de aceleradores** (`MFS_HWACCEL_*`): CRC-32C por instrucción, AES,
  SHA-256, CLMUL, SIMD, RNG y CAS atómicos; macros del compilador + refinado en
  runtime (`__builtin_cpu_supports`/CPUID).
- **Aceleración efectiva**: el CRC-32C usa la instrucción CRC32 (x86 SSE4.2 /
  ARMv8 CRC32) cuando existe, con resultado idéntico a la tabla software.
- **Honestidad de capacidades** (MFS-HW-001): el HWV sólo declara lo que el
  núcleo puede *ejecutar*; el resto se reporta como diagnóstico.
- **Modos de RAM mínima** para 8 bits (Ultra/Nano/Compact) y dimensionado
  condicional de pools y *scratch* con `MFS_ALLOW_8BIT_TARGET`.

### 🔄 Transaccionalidad (WAL+ v3)
- **Tokens A/B** con **Contador Termométrico (TFC)**.
- **Checkpoint** con raíz **BLAKE3** para recuperación rápida.
- **Replay determinista** acotado por **cota BMT** (Bit Map Table).
- **Snapshots y clones O(1)** vía `mf_snap_create()`.
- **FormalCore**: verificación formal del replay y de invariantes del WAL+.

### 💾 FTL Ultra 2
- **ZLF** (Zoned Log-Structured File) log-structured.
- **L2P** (Logical-to-Physical) mapping.
- **AGCB+** (Adaptive Garbage Collection Bandit) sobre deuda GLD.
- **WOM-p** para FRAM/MRAM (Write-Once Memory, Feistel 16 b).
- **SLEC**, **EBA**, **WEP**, **ZRP RS(16,15)** (Reed-Solomon).
- **RAS + Thermal Governor** para gestión térmica.
- **ELM/PEP** (Endurance Lifetime Model / Predictor).
- **Bad-block table** para SD/eMMC.

### ⚡ Gestión energética (EDP)
- Modelo de drenaje de **5 niveles**.
- **ELD** (Energy Leakage Debt) presupuestado.
- **DAB bandit determinista EXP3** para reparto de energía.

### ⏱️ E/S determinista (DAIO v2)
- Clases **RT-A / RT-B / RT-C** con *deadlines*.
- **TCB-DA** (Time-Constrained Bandit for Deadline Allocation).
- Latencia garantizada **p99.9 ≤ 3 ms en RT-A**.
- **WCET** verificado por *stack-painting* y autómatas *model-checked*.

### 🔐 Seguridad cripto-agile
- Suites **S0–S3**: AES-256-CTR+HMAC-SHA256 · AES-256-GCM · Ascon-128a · ChaCha20-Poly1305.
- **HKDF-SHA256** para derivación de claves.
- **Nonce normativo** (época × seq monotónico) y ceroización de claves/plaintext.
- **Negociación de suite** en montaje y **deprecación programada** con migración *rewrap*.
- **Ancla post-cuántica LMS** (NIST SP 800-208).
- **PUF** con *fuzzy extractor* para anclaje de claves.

### 🗜️ Compresión y deduplicación (CCD v2)
- **Deduplicación con filtro Cuckoo (CFX)**.
- **CDC** (Content-Defined Chunking) con **Gear hash**.
- **LZ4** y **FSST-lite** a nivel de página.

### 🧬 Medios heterogéneos (HMT)
- **Tiering heterogéneo T0/T1** (ej. FRAM + NAND).
- Árbol de directorios único sobre ambos medios.
- Soporte para **NAND ZNS-like** opcional.

### 📡 E/S extendida (XIO)
- **XDAM** (Extended Direct Access Mapping).
- **SDP** (Sensor→DMA/CRC→pool→O_RAW).

---

## 🏗️ Arquitectura del repositorio

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

## 🚀 Inicio rápido

### 📦 Requisitos

- Compilador C11 (`gcc ≥ 9`, `clang ≥ 12`, o IAR/ARMCC para targets)
- `make` o `cmake ≥ 3.16`
- Host Linux/macOS/Windows-WSL para simulación y tests
- Para MCU: el toolchain del objetivo (`arm-none-eabi-gcc`, `avr-gcc`, `sdcc`,
  `xc8`, …). El núcleo no depende de plataforma; sólo hay que aportar el driver
  L2 del medio (los drivers genéricos de MCU están en `platform/common/`).

### 🔨 Construir (host + vFlash)

```bash
git clone https://github.com/<org>/matrixfs-ultra.git
cd matrixfs-ultra
make            # genera libmatrixfs.a + binarios de test sobre sim/vFlash
make test       # ejecuta KATs §27.1 y suite de ciclo de vida
make bench      # MFS-Bench v2 (W1–W11) sobre vFlash
make mcu        # drivers L2 para MCU (libmatrixfs_mcu.a)
```

### 🧑‍💻 Ejemplo mínimo de uso

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

### 🎯 Cross-compilación a target

El núcleo no tiene dependencias de plataforma: se compila con el toolchain del
MCU. El **puerto** (§20.2) y el **modelo de arquitectura** ya viven en el núcleo
(`src/core/mfs_port_arch.c`, `src/core/mfs_arch.c`); sólo hay que aportar el
driver L2 del medio. Para objetivos de 8 bits, define `MFS_ALLOW_8BIT_TARGET=1`,
que habilita los modos 8-bit y reduce pools y *scratch* al mínimo.

```bash
# Drivers L2 para MCU (NOR/FRAM/EEPROM SPI-I2C, flash interna, SD-SPI)
make mcu                          # libmatrixfs_mcu.a
cmake -DMATRIXFS_BUILD_MCU=ON ..  # equivalente en CMake

# Cross-compilación del núcleo a un MCU de 8 bits (ejemplo AVR)
avr-gcc -std=c11 -Os -DMFS_ALLOW_8BIT_TARGET=1 ... # (usa tu toolchain real)
```

### 🔌 Integración con ecosistemas embebidos

Además del host (Linux/Windows), el repositorio incluye integraciones listas
para los ecosistemas más usados de MCU de 32 bits:

| Ecosistema | Carpeta | Uso |
|:---|:---|:---|
| **Arduino** (ESP32, ESP8266, RP2040) | [`platform/arduino/`](platform/arduino/) | Librería C++ `MatrixFS` + ejemplos; reserva una partición/región de flash |
| **ESP-IDF** (Espressif) | [`platform/esp-idf/`](platform/esp-idf/) | Componente externo sobre `esp_partition` + `Kconfig` |
| **PlatformIO** | [`platform/platformio/`](platform/platformio/) | Proyecto de ejemplo que consume la librería de Arduino |
| **MicroPython** | [`platform/micropython/`](platform/micropython/) | Usermod con el módulo `matrixfs` (`MatrixFS`, `File`) |

Todas comparten la capa [`platform/embedded/`](platform/embedded/) (driver L2
sobre una región de flash plana) y aportan las primitivas de puerto obligatorias
(`mfs_port_*`) y un driver L2 sobre la flash del dispositivo. Guía completa:
[📘 DOCS/embedded-integration.md](DOCS/embedded-integration.md).

El **soporte de 8 bits está integrado en el núcleo**, igual que 16/32/64: el
puerto por arquitectura (AVR, 8051, STM8, PIC16/18, Z80) vive en
[`src/core/mfs_port_arch.c`](src/core/mfs_port_arch.c) y la detección y
autoconfiguración de arquitectura/aceleradores en
[`src/core/mfs_arch.c`](src/core/mfs_arch.c). Los drivers de dispositivo
(NOR/FRAM/EEPROM SPI-I2C, flash interna, SD-SPI) están en
[`platform/common/mfs_l2_8bit.c`](platform/common/mfs_l2_8bit.c).

> [!WARNING]
> ⚠️ Las integraciones se han revisado por inspección estática y estructuran el
> build correctamente, pero **no se han compilado con los SDK de terceros**
> (Arduino-ESP32, ESP-IDF, MicroPython) en este repositorio. Cada `README`
> detalla qué queda sin verificar.

### 🐧 Montar un volumen en Linux (FUSE 3)

```bash
cd platform/linux && make && sudo make install
matrixfs-mkfs -L DATOS /dev/sdb1              # crear y formatear
sudo mount -t matrixfs /dev/sdb1 /mnt/datos   # montar
# o añadir a /etc/fstab:  /dev/sdb1 /mnt/datos matrixfs defaults 0 0
```

El automontaje por udev/systemd monta el volumen en
`/run/media/matrixfs/<dispositivo>` al conectar el medio. Guía completa:
[🐧 DOCS/linux-integration.md](DOCS/linux-integration.md).

### 🪟 Montar un volumen en Windows 10/11 (WinFsp)

```powershell
cd platform\windows
.\build.ps1            # requiere WinFsp SDK + Visual Studio
.\install.ps1          # copia los binarios y registra el servicio de automontaje
matrixfs-ctl.exe format D:\vol0.img --label DATOS
matrixfs_winfsp.exe D:\vol0.img X:            # o conectar el medio (automontaje)
```

La unidad aparece en el Explorador de Archivos con operaciones nativas
(copiar, pegar, eliminar, renombrar). Guía completa:
[🪟 DOCS/windows-integration.md](DOCS/windows-integration.md).

---

## 🔒 Garantías normativas

Estas reglas son **auditables en build y runtime**, no aspiracionales:

| ID | Regla | Mecanismo |
|:---|:---|:---|
| **MFS-RES-001** | Cero heap dinámico | Pools estáticos; revisión de mapa de símbolos (`check-map`) |
| **MFS-ARCH-010 rev. 3** | Clases 8/16/32/64-bit | Autodetección de clase y de aceleradores HW; rechazo `MFS_EARCH` sólo si `arch_class > 3` |
| **MFS-VIA-001/002** | Viabilidad honesta | Desglose fw+stack+perif+margen ≥10 % → `MFS_ENOTVIABLE` |
| **MFS-BUS-001..004** | No interferencia | Medición de bus SOLO lectura; cero RAM persistente compartida |
| **MFS-SEC-001** | Nonce normativo | época × seq monotónico; nunca reutilización |
| **MFS-SEC-002** | Ceroización | Claves/nonce/plaintext limpiados tras uso |
| **MFS-SEC-005** | EtM en S0 | Encrypt-then-MAC (AES-256-CTR + HMAC-SHA256) |
| **MFS-HW-001** | Fallback SW | Aceleradores autodetectados (CRC-32C por instrucción) con ruta SW equivalente e idéntica |
| **MFS-B3-001** | BLAKE3 conforme | Modo árbol estándar; KAT publicado |
| **MFS-SCOPE-001** | Claims ligados | Todo KPI citable ↔ escenario MFS-Bench v2 |

**Suites criptográficas (§10.6):** `S0` AES-256-CTR+HMAC-SHA256 · `S1` AES-256-GCM · `S2` Ascon-128a (SP 800-232) · `S3` ChaCha20-Poly1305 (RFC 8439). Negociación en montaje según HWV; deprecación programada con migración rewrap.

---

## 🧪 Verificación (§27)

- **Suite completa:** **1 232 comprobaciones, 0 fallos** (`mfs_tests.exe`), ejecutada **en Linux y en Windows**, incluyendo KAT, puerta de viabilidad, FIH, extremos funcionales, estrés, determinismo formal, la **capa de integración VFS** (permisos POSIX, metadatos, E/S y persistencia sobre un medio real, la misma ruta que usan FUSE y WinFsp), el **medio gestionado** sobre el adaptador L2 (RMW + TRIM, dispositivo simulado) y el **puerto RTOS** (detección, registro y contrato §20.2).
- **Montaje real en Linux:** `mount -t matrixfs` sobre FUSE 3 (libfuse3 3.17.2) con **39 comprobaciones / 0 fallos**: fichero aleatorio de 24 MiB íntegro (`cmp` y md5), `cp`, `truncate` con prefijo intacto, `rename`, borrado, `chmod`/`chown` persistentes, remontaje con md5 idéntico y montaje de sólo lectura (`EROFS`).
- **Interoperabilidad Linux ↔ Windows:** el mismo *layout* on-flash se lee en ambos sentidos (volumen creado en Windows leído en Linux y viceversa), con `verify=MFS_OK` y contenido idéntico.
- **Compilación nativa verificada:** todo el proyecto con `-std=c11 -Wall -Wextra -Werror` en Linux (gcc 14.2) y Windows; `matrixfs_fuse` enlazado contra **libfuse3 3.17.2** real; los tres binarios de Windows compilados con **MSVC 14.51 `/W4` sin avisos** contra el SDK de WinFsp real. Detalles y pasos pendientes (privilegios) en [🧪 DOCS/testing.md](DOCS/testing.md).
- **KATs:** CRC-32C `"123456789"` = `0xE3069283`; SHA-256/HMAC (RFC 4231); HKDF-SHA256 (RFC 5869 caso 1); BLAKE3 vectors oficiales; round-trip AEAD S0–S3.
- **Invariantes:** orden WAL↔datos, integridad E2G, monotonía TFC, coherencia L2P↔ART.
- **FIH (fault injection):** 10⁵ cortes aleatorios sin corrupción; 10⁴ drenajes EDP; fatiga acelerada de bloques; barrido de 1 000 cortes con fichero canario intacto.
- **Extremos:** churn con GC/WAF, tormenta de metadatos, matriz S0–S3 con cifrado y remontaje, agotamiento de zonas con errores tipificados.
- **Conformidad:** matriz §27 con huella de cada regla MFS-* sobre el código.
- **Formal (FormalCore):** replay determinista (DAB/CUSUM/EDP) y 64 combinaciones de reanudación WAL+ sin violaciones.

Ejecutar todo: `make test && make fih-short` · Rendimiento: `mfs_tests.exe --extreme` (el FIH completo requiere CI nocturno).

---

## 📊 Resultados de rendimiento extremo (MFS-Bench §17)

Medición sobre el puerto host con `vFlash` NOR (1 MiB, sector 4 KiB), compilación `-O2`. Ejecutable: `mfs_tests.exe --extreme`.

### 🚀 Rendimiento por modo

| modo | chunk B | zonas | escritura MB/s | lectura MB/s | mount µs | WAF | estado |
|:---|---:|---:|---:|---:|---:|---:|:--:|
| Ultra-Nano | 108 | 128 | 32,69 | 130,96 | 690 | 1,04 | OK |
| Nano | 236 | 128 | 23,97 | 168,80 | 842 | 1,08 | OK |
| Compact | 492 | 128 | 20,55 | 183,48 | 1 356 | 1,15 | OK |
| Balanced | 4 076 | 126 | 17,24 | 236,48 | 1 974 | 2,04 | OK |
| Extended | 4 076 | 126 | 19,20 | 236,48 | 2 015 | 2,04 | OK |

### 🔐 Throughput criptográfico por suite (§10.6)

| suite | seal MB/s | open MB/s | tag B | nota |
|:---|---:|---:|---:|:---|
| S0 AES-256-CTR + HMAC-SHA256 | 4,8 | 68,5 | 16 | EtM/AEAD verificado |
| S1 AES-256-GCM | 1,4 | 2,1 | 16 | EtM/AEAD verificado |
| S2 Ascon-128a | 1,6 | 1,6 | 16 | EtM/AEAD verificado |
| S3 ChaCha20-Poly1305 | 75,9 | 426,3 | 16 | EtM/AEAD verificado |

### 🧮 Coste por operación de subsistemas FTL/seguridad (§11, §15)

| operación | coste µs | nota |
|:---|---:|:---|
| `crc32c(4 KiB)` | 16,29 | integridad E2G |
| `HKDF-SHA256` | 7,40 | derivación de claves (§15) |
| `ELM health` | 0,010 | modelo de vida §11.1 |
| `PEP score (4×32)` | 0,236 | WCET < 5 µs §11.1 |
| `LMS keygen (H=8)` | 249 055 | ancla post-cuántica §15 (una vez, enrolamiento) |
| `LMS verify` | 456 | secure-boot (una vez por arranque) |
| `ZRP encode (16 pág)` | 12,0 | +6,25 % en zonas frías §8.5 |
| `ZRP recover (1 pág)` | 12,0 | reconstrucción RS(16,15), nivel 5 §12 |
| `WEP place (caché)` | 0,094 | Feistel 16 b §11.2 Balanced+ |

### 🛡️ WAF y durabilidad por workload (§17.1/§17.3)

| workload | ops | WAF | GC reloc | resultado |
|:---|---:|---:|---:|:--:|
| W1 append secuencial | 260 | 1,188 | 0 | MFS_OK |
| W3 random en fichero | 400 | 1,161 | 780 | MFS_OK |
| W4 mixto churn 90/10 | 260 | 1,254 | 138 | MFS_OK |

### 📐 Huella de memoria estática (MFS-RES-001: sin heap)

| estructura | bytes | uso |
|:---|---:|:---|
| `mf_t` (núcleo, host 64-bit) | ≈ 17 240 | instancia única §23.1 |
| `mf_t` (build 8-bit, `MFS_ALLOW_8BIT_TARGET`) | ≈ 1 840 | pools y scratch reducidos (≤ 2 KB) |
| `mfs_inode_ram_t` | 148 | ventana flash-first §22.3 |
| `mfs_zone_t` | 36 | tabla de zonas §24.1 |
| `mfs_iocb` | 32 | ring DAIO §13.1 |
| `mfs_wom_t` | 12 | WOM-p §11.4 |
| `mfs_health_t` | 168 | telemetría HCT §16 |
| `mfs_hwv_t` | 76 | HWV (en flash) §5.2 |
| Tabla CRC-32C (16/32/64-bit vs 8-bit) | 1024 → 64 | tabla completa vs *nibble* |

> [!NOTE]
> **Nota metodológica:** los valores son cotas superiores medidas en host (no son WCET de target); la huella estática es determinista y verificable en compilación. Las desviaciones y límites conocidos se detallan en [⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md).

---

## 🛠️ Herramientas

### 🧰 `mfstool`
CLI para:
- Generar **manifiestos** de build y despliegue.
- Analizar **trazas HCT** (Health Check Trace).
- Ejecutar el banco de pruebas **MFS-Bench v2**.
- Analizador de conformidad contra MFS-SPEC-003.

### 🧪 `vFlash` / `vFRAM`
Simuladores de memoria para desarrollo en host:
- Inyección de fallos y **cortes de energía** (crash/recover).
- Modelado de latencias y geometrías (NOR/NAND).
- Tier **T0 byte-addressable** (FRAM/MRAM) sin erase.

### ✅ `FormalCore`
- Verificación formal del **replay determinista** (DAB/CUSUM/EDP).
- Comprobación de **invariantes del WAL+** (64 combinaciones de reanudación).

---

## 🖥️ Plataformas y ecosistemas soportados

### 🖥️ Sistemas operativos de escritorio

| SO | Integración | Estado |
|:---|:---|:---|
| **Linux 5.4+/6.x/7.x** | FUSE 3, `fstab`, systemd, udev, deb/rpm | ✅ Verificado (39/0) |
| **Windows 10/11** | WinFsp, letra de unidad, servicio, Inno Setup | ✅ Compilado · ⏳ montaje real pendiente |

### 🧩 Frameworks embebidos

| Framework | Carpeta | Estado |
|:---|:---|:---|
| **Arduino** (ESP32, ESP8266, RP2040) | `platform/arduino/` | 🟡 Implementado · ⏳ sin compilar con SDK |
| **ESP-IDF** (Espressif) | `platform/esp-idf/` | 🟡 Implementado · ⏳ sin compilar con SDK |
| **PlatformIO** | `platform/platformio/` | 🟡 Implementado · ⏳ sin compilar con SDK |
| **MicroPython** | `platform/micropython/` | 🟡 Implementado · ⏳ sin compilar con SDK |

### 🏗️ Arquitecturas de MCU (integradas en el núcleo)

| Clase | Puerto | Modos | Notas |
|:---|:---|:---|:---|
| **8 bits** (AVR, 8051, STM8, PIC16/18, Z80) | `src/core/mfs_port_arch.c` | 8-bit Ultra / Nano / Compact | ≤ 2 KB de RAM, CRC-32C con tabla de *nibble*, sin AEAD |
| **16 / 32 bits** (ARM Cortex-M, MSP430, AVR32…) | `src/core/mfs_port_arch.c` (genérico) o el del integrador | Ultra-Nano … Extended | Familia clásica; suites S0–S3 |
| **64 bits** (x86-64, ARM64, RISC-V 64) | `src/core/mfs_port_arch.c` (genérico) o el del integrador | Extended como techo | Suite clásica; **aceleración HW autodetectada** |

La **detección y autoconfiguración** (`src/core/mfs_arch.c`) clasifica la
arquitectura (`MFS_ARCH_AUTO` o declarada) y detecta aceleradores
(CRC-32C por instrucción, AES, SHA-256, CLMUL, SIMD, RNG, CAS atómicos). El
**CRC-32C se acelera automáticamente** cuando la CPU expone la instrucción
(x86 SSE4.2 / ARMv8 CRC32, mismo polinomio de Castagnoli ⇒ resultado idéntico);
el resto de capacidades se reportan para diagnóstico y planificación, y su ruta
efectiva sigue siendo software (MFS-HW-001: nunca se declara una capacidad que
no se pueda ejecutar).

### 🤖 MCU de 8 bits
- **AVR, 8051, STM8, PIC16/18, Z80** vía el puerto del núcleo `src/core/mfs_port_arch.c`.
- Modos **8-bit Ultra / Nano / Compact**.
- Drivers SPI/I²C para NOR/FRAM/EEPROM/flash-interna/SD-SPI (`platform/common/mfs_l2_8bit.c`).
- Autodetección de capacidades y autoadaptación de la configuración.

### ⏱️ RTOS
- **Puerto RTOS genérico en el núcleo** (`src/core/mfs_port_rtos.c`,
  `include/matrixfs/mfs_port_rtos.h`): mapea el contrato `mfs_port_*` a las
  primitivas nativas del RTOS y se selecciona por detección en tiempo de
  compilación.
- **Adaptadores nativos cableados**: **FreeRTOS** (`taskENTER/EXIT_CRITICAL`),
  **Zephyr RTOS** (`irq_lock/unlock`, `k_cycle_get_32`) y **Eclipse ThreadX**
  (`tx_interrupt_control`, `tx_time_get`).
- **Detección + registro** para **Mbed OS, Apache NuttX, RIOT OS, Apache Mynewt,
  RT-Thread y PX5 RTOS**, con la plantilla `platform/rtos/mfs_rtos_port_template.c`.
- Guía completa: [⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md).

### 🏭 SDKs de fabricantes
- Integración a través de los puntos de enganche: driver de flash del SDK →
  capa `mfs_embedded` / `mfs_l2_managed`, y RTOS del SDK → puerto RTOS.
- **Silicon Labs** (Gecko SDK), **Texas Instruments** (SimpleLink/MSPM0),
  **Infineon** (ModusToolbox), **Renesas** (FSP): guía y puntos de enganche en
  [⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md) §4 (depende del SoC
  concreto).

---

## 🗺️ Roadmap y estado

| Fase | Contenido | Estado |
|:---|:---|:---|
| **1 — Núcleo y viabilidad** | Puerto, HWV, cascada HAL, viabilidad/modos, RSC+pools, extents+L2P, WAL+, UN/Nano/Compact | ✅ Implementado · ✅ KATs y suite en verde |
| **2 — Diferenciación** | GLD/WOM-p/TFC/E2G/ZLF/HCT/CCD/AGCB+/CFX+CDC/MFS-Snap/FPT/ELD | ✅ Implementado · ✅ MFS-Bench (tablas arriba) |
| **3 — Industrial** | HAWL+ · ELM/WEP/CV/RAS+TG/EBA/EDP/ZRP/PUF/cadena PQ/FormalCore + vFlash CI | ✅ Implementado (FormalCore = replay determinista + invariantes WAL+) |
| **4 — Optimización avanzada** | Hint RT-C/PEP/SDP/FSST/ZLF-Z/CQE/diccionarios on-device | ✅ Implementado (PEP, SDP, FSST-lite, ZRP, CQE) |
| **5 — Certificación** | Dossier SIL-2/21434, deprecación de suites, tooling de flota | 🟡 Tooling listo · dossier de certificación pendiente (no es código) |
| **6 — Integración con el SO** | Linux (FUSE 3, kernels 5.4+/6.x/7.x, `fstab`, systemd, udev, deb/rpm) · Windows 10/11 (WinFsp, letra de unidad, Explorador, servicio de automontaje, Inno Setup) | ✅ Implementado · ✅ montaje real verificado en Linux (39/0) · ✅ interoperabilidad Linux↔Windows · ⏳ montaje real en Windows pendiente (requiere sesión elevada) |
| **7 — Arquitecturas y ecosistemas embebidos** | Clases **8/16/32/64-bit** con autodetección y **autoconfiguración de aceleración HW** · puerto y modelo de arquitectura en el núcleo (`src/core/mfs_arch.c`, `mfs_port_arch.c`) · drivers MCU (`platform/common/mfs_l2_8bit.c`) · capa común `platform/embedded` · integraciones **Arduino, ESP-IDF, PlatformIO y MicroPython** | ✅ Implementado · ✅ 64-bit, CRC-32C acelerado y capa embebida verificados en host · ⏳ compilación con SDK/toolchains de terceros pendiente |
| **8 — RTOS y SDKs de fabricantes** | Puerto **RTOS genérico** en el núcleo con adaptadores nativos de **FreeRTOS, Zephyr y ThreadX**; detección + registro para Mbed OS, NuttX, RIOT, Mynewt, RT-Thread y PX5; puntos de enganche para Silicon Labs, TI, Infineon y Renesas | 🟡 Implementado · ✅ detección/registro/contrato verificados en host · ⏳ adaptadores nativos de RTOS sin compilar aquí (sin toolchain del RTOS) |
| **9 — Almacenamiento flash completo** | Medios **SATA** y **UFS** en el modelo + perfiles; **adaptador L2 MANAGED** genérico (sectores + TRIM + RMW); simulador de dispositivo gestionado y test en host | 🟡 Implementado · ✅ verificado en host (simulador) · ⏳ drivers de silicio (SD/eMMC/UFS/NVMe/SATA) dependen del SDK del SoC |

**Estado actual:** subsistemas de Fase 3–4 implementados y verificados — WOM-p, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP (RS(16,15)), tiering HMT T0/T1, HKDF/PUF/LMS (SP 800-208), XDAM/SDP/CQE y FormalCore. La capa de integración con el sistema operativo (carpetas `platform/linux` y `platform/windows`, con la capa portable compartida `platform/common`) está implementada y **verificada con montaje real en Linux** (39 OK / 0 fallos) e **interoperabilidad bidireccional Linux ↔ Windows**. Sobre ella se añade el **soporte de arquitecturas 8/16/32/64 bits integrado en el núcleo** (`src/core/mfs_arch.c` + `mfs_port_arch.c`, con detección y autoconfiguración de aceleración por hardware), los **drivers L2 para MCU**, las **integraciones para Arduino, ESP-IDF, PlatformIO y MicroPython** sobre la capa común `platform/embedded`, el **puerto RTOS genérico** (`src/core/mfs_port_rtos.c`, con adaptadores nativos de FreeRTOS/Zephyr/ThreadX y registro para el resto) y el **adaptador L2 para medios gestionados** (`platform/common/mfs_l2_managed.c`: SD/eMMC/UFS/USB/NVMe/SATA). Suite completa **1 232 checks / 0 fallos** con `-std=c11 -Wall -Wextra -Werror` sin avisos en Linux y Windows. Los límites y desviaciones vigentes están inventariados en [⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md). Ver [📋 CHANGELOG.md](CHANGELOG.md).

---

## 🌐 Multiidioma

Este README activa la **traducción automática de GitHub** (*README translations*) mediante el archivo de configuración
[`.github/languages.yaml`](.github/languages.yaml), conforme a las especificaciones oficiales del *feature*
([github.com/readme-translations/translation-config](https://github.com/readme-translations/translation-config)).

- **Idioma predeterminado (fuente):** 🇪🇸 **Español (`es`)** — todo el contenido original de este README está redactado en español.
- **Idiomas soportados (en orden de prioridad tras el principal):** 🇺🇸 inglés (`en`), 🇨🇳 chino simplificado (`zh-CN`), 🇩🇪 alemán (`de`), 🇯🇵 japonés (`ja`), 🇫🇷 francés (`fr`), 🇧🇷 portugués de Brasil (`pt-BR`), 🇷🇺 ruso (`ru`), 🇰🇷 coreano (`ko`) e 🇮🇹 italiano (`it`).

GitHub muestra un **selector de idiomas** sobre el README y sirve la traducción automática correspondiente; los enlaces relativos, imágenes y bloques de código funcionan sin cambios en todos los idiomas porque la traducción sólo afecta al texto renderizado. Los `README.md` de las subcarpetas del proyecto (`DOCS/`, `platform/*/`, etc.) mantienen su contenido y **no se ven afectados** por esta configuración.

| Idioma | Código | Rol |
|:---|:---|:---|
| 🇪🇸 Español | `es` | **Predeterminado / fuente** |
| 🇺🇸 English | `en` | Traducción automática · prioridad 1 |
| 🇨🇳 中文（简体） | `zh-CN` | Traducción automática · prioridad 2 |
| 🇩🇪 Deutsch | `de` | Traducción automática · prioridad 3 |
| 🇯🇵 日本語 | `ja` | Traducción automática · prioridad 4 |
| 🇫🇷 Français | `fr` | Traducción automática · prioridad 5 |
| 🇧🇷 Português (Brasil) | `pt-BR` | Traducción automática · prioridad 6 |
| 🇷🇺 Русский | `ru` | Traducción automática · prioridad 7 |
| 🇰🇷 한국어 | `ko` | Traducción automática · prioridad 8 |
| 🇮🇹 Italiano | `it` | Traducción automática · prioridad 9 |

---

## 📚 Documentación

- **[📐 DOCS/…Guide.md](DOCS/MatrixFS%20-%20Technical%20Specifications%20and%20Implementation%20Guide.md)** — Especificación normativa completa (31 secciones): principios, HAL, viabilidad, formato físico, WAL+, pipeline CCD, FTL, EDP/DAIO, seguridad, HCT, modos, guía de implementación (API, estructuras on-flash, FSMs, constantes), plan de verificación.
- **[📘 DOCS/](DOCS/)** — Guías de diseño e implementación por módulo (arquitectura, HAL/viabilidad, E2G, WAL/transacciones, suites criptográficas, RT/energía, layout flash, contrato de puertos, conformidad/tests).
- **[🐧 DOCS/linux-integration.md](DOCS/linux-integration.md)** — Integración con Linux: matriz de kernels, requisitos de compilación, instalación, `fstab`, systemd/udev, paquetes deb/rpm, resolución de problemas y validación.
- **[🪟 DOCS/windows-integration.md](DOCS/windows-integration.md)** — Integración con Windows 10/11: WinFsp, compilación con MSVC, letra de unidad, tabla de operaciones del Explorador, servicio de automontaje, instalador y validación.
- **[🔌 DOCS/embedded-integration.md](DOCS/embedded-integration.md)** — Integración embebida: puerto y arquitectura en el núcleo (8/16/32/64-bit), capa común `platform/embedded`, drivers MCU y ecosistemas Arduino, ESP-IDF, PlatformIO y MicroPython.
- **[💾 DOCS/storage-integration.md](DOCS/storage-integration.md)** — Almacenamiento flash: motores RAW/ZONED/MANAGED, adaptador L2 para medios gestionados (SD/eMMC/UFS/USB/NVMe/SATA), ejemplos y validación.
- **[⏱️ DOCS/rtos-integration.md](DOCS/rtos-integration.md)** — RTOS y plataformas de silicio: puerto RTOS genérico, adaptadores nativos (FreeRTOS/Zephyr/ThreadX), plantilla de portado y puntos de enganche (Silicon Labs, TI, Infineon, Renesas).
- **[⚠️ DOCS/known-limitations.md](DOCS/known-limitations.md)** — Límites y desviaciones vigentes respecto a la spec.
- **[🧪 DOCS/testing.md](DOCS/testing.md)** — Pasos de verificación y privilegios requeridos.
- **[🤝 CONTRIBUTING.md](CONTRIBUTING.md)** — Cómo proponer mejoras, reportar desviaciones respecto a la spec y enviar KATs.
- **[🔧 tools/](tools/)** — `mfstool`: generador de manifiestos, analizador de trazas HCT, banco MFS-Bench v2, emparejador formal.

---

## 📣 Notificar mejoras, errores y desviaciones

El desarrollo de **MatrixFS** sigue un proceso abierto y trazable. Cualquier mejora propuesta pasa por revisión contra las reglas normativas MFS-* antes de merge.

| Tipo | Canal | Plantilla / campos obligatorios |
|:---|:---|:---|
| 🐛 **Bug / corrupción** | [GitHub Issues → Bug report](/issues/new?labels=bug) | Reproducción con vFlash, secuencia de operaciones, punto de corte, logs HCT exportados |
| 💡 **Mejora / feature** | [GitHub Issues → Enhancement](/issues/new?labels=enhancement) | Sección(s) de MFS-SPEC-003 afectadas, impacto en RSC/pila/WCET, KPI MFS-Bench esperado |
| 📏 **Desviación vs. spec** | Issue con etiqueta `spec-deviation` | ID de regla (p. ej. `MFS-VIA-002`), comportamiento observado vs. normativo |
| 🔐 **Vulnerabilidad de seguridad** | **NO abrir issue público** — escribir a `security@matrixfs.example` | CVSS preliminar, suite/media afectados, PoC confidencial |
| 📝 **Mejora de documentación** | PR directo o issue `documentation` | Enlace a sección y texto propuesto |

**Política de triage:** issues etiquetados en ≤ 3 días hábiles · blocker de corrupción = prioridad P0 con análisis forense obligatorio · toda mejora aceptada se registra en [📋 CHANGELOG.md](CHANGELOG.md) con referencia a la sección de la especificación. Las contribuciones externas se aceptan bajo Apache 2.0 (ver §5 del LICENSE) y deben incluir DCO sign-off (`git commit -s`).

---

## ⚖️ Licencia

Distribuido bajo **Apache License 2.0** — ver [LICENSE](LICENSE).

**¿Por qué Apache 2.0?** MatrixFS apunta a producto industrial (automoción, médico, infraestructura crítica). Apache 2.0:

1. **Permite uso comercial y cierre propietario** — integradores pueden usarlo en firmware de producción sin copyleft, lo que favorece adopción en sectores regulados.
2. **Otorga licencia de patentes expresa** de cada contribuyente (§3) — esencial dado que la especificación define técnicas susceptibles de patente (GC adaptativo por deuda, contadores termométricos, drenaje EDP multi-nivel, bandit determinista EXP3).
3. **Exige conservar atribución y notices** (§4) y marcar archivos modificados (§4b) — mantiene auditables las derivaciones frente a la spec MFS-SPEC-003.
4. **Sin garantía** (§7–8) — coherente con la naturaleza de referencia normativa del proyecto: cualquier certificación SIL-2/ISO 26262 exige validación propia del integrador.

Los documentos de especificación en `DOCS/` comparten la misma licencia. Las marcas «MatrixFS», «ATLAS» y «MFS-Bench» no se conceden con la licencia (§6).

© 2026 The MatrixFS contributors.

---

<div align="center">

**MatrixFS «ATLAS»** — *donde cada promesa es un artefacto auditable.*

[⬆ Volver arriba](#-matrixfs-atlas) · [📋 Índice](#-índice)

</div>

<!-- ==================== Definiciones de badges (reference-style) ==================== -->
[badge-license]: https://img.shields.io/badge/License-Apache%202.0-blue.svg
[badge-c11]: https://img.shields.io/badge/C%20Standard-C11-blue
[badge-heap]: https://img.shields.io/badge/heap-ZERO-brightgreen
[badge-misra]: https://img.shields.io/badge/MISRA%20C%3A2012-oriented-orange
[badge-status]: https://img.shields.io/badge/status-Fases%201--7%20implementadas-yellow
[badge-arch]: https://img.shields.io/badge/arch-8%2F16%2F32%2F64--bit-orange
[badge-tests]: https://img.shields.io/badge/tests-1232%20passing-brightgreen
