<div align="center">

# 🟩 MatrixFS Ultra «ATLAS»

**Sistema de archivos embebido determinista, transaccional y cripto-agile para NOR/NAND/FRAM/MRAM/SD-eMMC — desde 8 KB de RAM (16/32 bits).**

*Implementación de la especificación técnica MFS-SPEC-003 Edición 1.0 «ATLAS»*

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C Standard](https://img.shields.io/badge/C-C11-blue)](https://en.cppreference.com/w/c/11)
[![No Heap](https://img.shields.io/badge/heap-ZERO-brightgreen)](#-garantías-normativas)
[![MISRA](https://img.shields.io/badge/MISRA%20C%3A2012-oriented-orange)]()
[![Status](https://img.shields.io/badge/status-Fase%201--2%20(en%20desarrollo)-yellow)](#-roadmap-y-estado)

</div>

---

## 📖 ¿Qué es MatrixFS Ultra?

**MatrixFS Ultra** es un sistema de archivos diseñado para dispositivos industriales, IoT crítico, automoción, equipamiento médico y registro seguro, donde **fiabilidad, vida útil del medio, energía, seguridad y determinismo pesan tanto como el rendimiento**. Opera directamente sobre memoria no volátil controlable por el MCU:

| Medio | Interfaces | Notas |
|---|---|---|
| **NOR SPI/QSPI/OSPI** | XIP opcional | Byte-addressable, WEP por entropía |
| **NAND raw / ONFI / Toggle** | Bus de datos 8/16 | Semántica de zonas ZNS-like opcional |
| **FRAM / MRAM / EEPROM** | I²C/SPI | Sin borrado por bloque, E2G simplificado |
| **SD / eMMC 5.1** | CMD/DAT, CQE | Bad-block table, DMA alineado |
| **Dual-medio heterogéneo** | NVM + flash de bloques | Un único árbol de directorios (HMT) |

Su rasgo distintivo: **cada promesa se convierte en un artefacto auditable** — sin heap dinámico certificado en build-time, viabilidad honesta (`MFS_ENOTVIABLE`/`MFS_EARCH` en lugar de degradación silenciosa), y recuperación verificada ante cortes de energía.

> ⚠️ **Las arquitecturas de 8 bits (AVR, 8051, PIC, STM8) NO están soportadas** — regla normativa **MFS-ARCH-010**, aplicada con `static_assert` en compile-time y rechazo `MFS_EARCH` si el HWV declara `arch_class = 0`. Soporte mínimo real: **16 y 32 bits**.

---

## ✨ Características principales (los 12 pilares)

| # | Pilar | Implementación |
|---|---|---|
| 1 | **Autoconfiguración segura** | Cascada HAL §5.1 (JEDEC/SFDP/CFI/assets), HWV de 64 B persistido @LBA 512 con CRC-32C |
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

### Modos operativos y límites normativos (§18.2)

| Modo | RAM mínima | Stack | Chunk | Escenario |
|---|---|---|---|---|
| **Ultra-Nano** | 720 B | 512 B | 256 B | MCUs 8 KB (STM32F0/F1, MSP430FR) |
| **Nano** | 1.5 KB | 1 KB | 512 B | MCUs 8–16 KB |
| **Compact** | 3.5 KB | 1.5 KB | 1 KB | MCUs 16–20 KB |
| **Balanced** | 11.5 KB | 3 KB | 4 KB | MCUs 64 KB+ |
| **Extended** | 21.5 KB | 6 KB | 16 KB | Dual-medio, PQ, ML certificable |

---

## 🏗️ Arquitectura del repositorio

```
matrixfs-ultra/
├── DOCS/                                  # 📄 Especificación normativa completa (MFS-SPEC-003)
│   └── MatrixFS Ultra - Technical Specifications and Implementation Guide.md
├── include/matrixfs/                      # 🔌 Cabeceras públicas (contrato estable)
│   ├── mfs_types.h                        #    Tipos, 26 códigos de estado, modos, clases RT
│   ├── mfs_port.h                         #    Contrato de puerto §20 (HAL ops, driver L2, HWV)
│   └── matrixfs.h                         #    API pública §21 (POSIX-subset + VIO/DAIO + tx)
├── src/                                   # ⚙️ Núcleo (sin heap, MISRA-oriented)
│   ├── mfs_internal.h                     #    Layout on-flash §22, FSMs §24, pools
│   ├── core/
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
├── platform/                              # 🖥️ Integración con el sistema operativo (§21, §22)
│   ├── common/                            #    Capa portable compartida Linux + Windows
│   │   ├── mfs_plat.{h,c}                 #      Mutex (CRITICAL_SECTION/pthread) y tiempo
│   │   ├── mfs_blk.{h,c}                  #      Driver L2: imagen · /dev/sdX · MTD · \\.\X: (RMW alineado)
│   │   ├── mfs_vfs.{h,c}                  #      Adaptador VFS: montaje, formato, E/S, permisos POSIX
│   │   └── mfs_vfsctl.c                   #      CLI de validación (format/probe/ls/cat/verify)
│   ├── linux/                             #    Linux 5.4+/6.x/7.x: FUSE 3, fstab, systemd, udev, deb/rpm
│   └── windows/                           #    Windows 10/11: WinFsp, letra de unidad, servicio, Inno Setup
├── sim/                                   # 🧪 vFlash/vFRAM host (NOR/NAND + byte-addressable T0)
│   ├── vflash.h                           #    Geometría, crash/recover, inyección de fallos
│   └── vfram.h                            #    Tier T0 byte-addressable (FRAM/MRAM), sin erase
├── tests/                                 # ✅ Plan de verificación §27 (KATs, FIH, estrés, extremos, formal, VFS)
├── tools/                                 # 🔧 mfstool: manifiestos, trazas HCT, bench, analizador
├── DOCS/                                  # 📚 Documentación de diseño e implementación
├── Makefile · CMakeLists.txt              # 🛠️ Build de host (lib, suite, mfstool) + targets ARM Cortex-M / MSP430
├── LICENSE                                # ⚖️ Apache 2.0
├── CONTRIBUTING.md                        # 🤝 Proceso de contribución y notificación de mejoras
├── CHANGELOG.md                           # 📋 Mejoras y cambios por versión
└── README.md                              # ← estás aquí
```

---

## 🚀 Inicio rápido

### Requisitos

- Compilador C11 (`gcc ≥ 9`, `clang ≥ 12`, o IAR/ARMCC para targets)
- `make` o `cmake ≥ 3.16`
- Host Linux/macOS/Windows-WSL para simulación y tests

### Construir (host + vFlash)

```bash
git clone https://github.com/<org>/matrixfs-ultra.git
cd matrixfs-ultra
make            # genera libmatrixfs.a + binarios de test sobre sim/vFlash
make test       # ejecuta KATs §27.1 y suite de ciclo de vida
make bench      # MFS-Bench v2 (W1–W11) sobre vFlash
```

### Ejemplo mínimo de uso

```c
#include <matrixfs/matrixfs.h>

/* 1. Configurar puerto: driver L2 + geometría + HAL ops */
mfs_config_t cfg;
mfs_config_init(&cfg);
cfg.media.driver      = &my_spi_nor_driver;   /* read/program/erase/suspend/resume */
cfg.media.geometry    = my_nor_geom;          /* 16 MiB NOR, 4 KiB blocks */
cfg.ram_budget_bytes  = 12 * 1024;            /* presupuesto REAL del MCU */
cfg.stack_budget_bytes= 2 * 1024;
cfg.mode_hint         = MFS_MODE_BALANCED;

/* 2. Inicializar (detección → HWV → viabilidad → montaje FSM §24.2) */
mf_t fs;
mfs_status_t st = mf_init(&fs, &cfg);
if (st == MFS_ENOTVIABLE) { /* recursos insuficientes: NO arranca, razón en st_detail */ }

/* 3. Escritura transaccional (commit T1 = durable antes de retornar) */
st = mf_tx_begin(&fs);
int fd = mf_open(&fs, "/var/log/event.bin", MFS_O_WRONLY | MFS_O_CREAT);
mf_write(&fs, fd, payload, len);
mf_close(&fs, fd);
st = mf_tx_commit(&fs);        /* WAL token A+B · E2G · checkpoint si procede */

/* 4. Snapshot O(1) y telemetría */
uint32_t snap_id;
mf_snapshot_create(&fs, "pre-update", &snap_id);
mfs_health_t h;
mf_health_get(&fs, &h);        /* WAF, TG, ELD, deuda GLD, errores recuperados */
```

### Cross-compilación a target

```bash
make TARGET=cortex-m4 CROSS=arm-none-eabi-   # libmatrixfs_cm4.a
make TARGET=cortex-m0+ CROSS=arm-none-eabi-  # Nano/UN en 8 KB
```

### Montar un volumen en Linux (FUSE 3)

```bash
cd platform/linux && make && sudo make install
matrixfs-mkfs -L DATOS /dev/sdb1              # crear y formatear
sudo mount -t matrixfs /dev/sdb1 /mnt/datos   # montar
# o añadir a /etc/fstab:  /dev/sdb1 /mnt/datos matrixfs defaults 0 0
```

El automontaje por udev/systemd monta el volumen en
`/run/media/matrixfs/<dispositivo>` al conectar el medio. Guía completa:
[DOCS/linux-integration.md](DOCS/linux-integration.md).

### Montar un volumen en Windows 10/11 (WinFsp)

```powershell
cd platform\windows
.\build.ps1            # requiere WinFsp SDK + Visual Studio
.\install.ps1          # copia los binarios y registra el servicio de automontaje
matrixfs-ctl.exe format D:\vol0.img --label DATOS
matrixfs_winfsp.exe D:\vol0.img X:            # o conectar el medio (automontaje)
```

La unidad aparece en el Explorador de Archivos con operaciones nativas
(copiar, pegar, eliminar, renombrar). Guía completa:
[DOCS/windows-integration.md](DOCS/windows-integration.md).

---

## 🔒 Garantías normativas

Estas reglas son **auditables en build y runtime**, no aspiracionales:

| ID | Regla | Mecanismo |
|---|---|---|
| **MFS-RES-001** | Cero heap dinámico | Pools estáticos; revisión de mapa de símbolos (`check-map`) |
| **MFS-ARCH-010** | No 8 bits | `static_assert` + rechazo `MFS_EARCH` si HWV arch_class=0 |
| **MFS-VIA-001/002** | Viabilidad honesta | Desglose fw+stack+perif+margen≥10 % → `MFS_ENOTVIABLE` |
| **MFS-BUS-001..004** | No interferencia | Medición de bus SOLO lectura; cero RAM persistente compartida |
| **MFS-SEC-001** | Nonce normativo | época ‖ seq monotónico; nunca reutilización |
| **MFS-SEC-002** | Ceroización | Claves/nonce/plaintext limpiados tras uso |
| **MFS-SEC-005** | EtM en S0 | Encrypt-then-MAC (AES-256-CTR + HMAC-SHA256) |
| **MFS-HW-001** | Fallback SW | Negociación de aceleradores sin bajar garantías |
| **MFS-B3-001** | BLAKE3 conforme | Modo árbol estándar; KAT publicado |
| **MFS-SCOPE-001** | Claims ligados | Todo KPI citable ↔ escenario MFS-Bench v2 |

**Suites criptográficas (§10.6):** `S0` AES-256-CTR+HMAC-SHA256 · `S1` AES-256-GCM · `S2` Ascon-128a (SP 800-232) · `S3` ChaCha20-Poly1305 (RFC 8439). Negociación en montaje según HWV; deprecación programada con migración rewrap.

---

## 🧪 Verificación (§27)

- **Suite completa:** **935 comprobaciones, 0 fallos** (`mfs_tests.exe`), ejecutada **en Linux y en Windows**, incluyendo KAT, puerta de viabilidad, FIH, extremos funcionales, estrés, determinismo formal y la **capa de integración VFS** (permisos POSIX, metadatos, E/S y persistencia sobre un medio real, la misma ruta que usan FUSE y WinFsp).
- **Montaje real en Linux:** `mount -t matrixfs` sobre FUSE 3 (libfuse3 3.17.2) con **39 comprobaciones / 0 fallos**: fichero aleatorio de 24 MiB íntegro (`cmp` y md5), `cp`, `truncate` con prefijo intacto, `rename`, borrado, `chmod`/`chown` persistentes, remontaje con md5 idéntico y montaje de sólo lectura (`EROFS`).
- **Interoperabilidad Linux ↔ Windows:** el mismo *layout* on-flash se lee en ambos sentidos (volumen creado en Windows leído en Linux y viceversa), con `verify=MFS_OK` y contenido idéntico.
- **Compilación nativa verificada:** todo el proyecto con `-std=c11 -Wall -Wextra -Werror` en Linux (gcc 14.2) y Windows; `matrixfs_fuse` enlazado contra **libfuse3 3.17.2** real; los tres binarios de Windows compilados con **MSVC 14.51 `/W4` sin avisos** contra el SDK de WinFsp real. Detalles y pasos pendientes (privilegios) en [DOCS/testing.md](DOCS/testing.md).
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

### Rendimiento por modo

| modo | chunk B | zonas | escritura MB/s | lectura MB/s | mount µs | WAF | estado |
|---|---:|---:|---:|---:|---:|---:|:--:|
| Ultra-Nano | 108 | 128 | 32,69 | 130,96 | 690 | 1,04 | OK |
| Nano | 236 | 128 | 23,97 | 168,80 | 842 | 1,08 | OK |
| Compact | 492 | 128 | 20,55 | 183,48 | 1 356 | 1,15 | OK |
| Balanced | 4 076 | 126 | 17,24 | 236,48 | 1 974 | 2,04 | OK |
| Extended | 4 076 | 126 | 19,20 | 236,48 | 2 015 | 2,04 | OK |

### Throughput criptográfico por suite (§10.6)

| suite | seal MB/s | open MB/s | tag B | nota |
|---|---:|---:|---:|---|
| S0 AES-256-CTR + HMAC-SHA256 | 4,8 | 68,5 | 16 | EtM/AEAD verificado |
| S1 AES-256-GCM | 1,4 | 2,1 | 16 | EtM/AEAD verificado |
| S2 Ascon-128a | 1,6 | 1,6 | 16 | EtM/AEAD verificado |
| S3 ChaCha20-Poly1305 | 75,9 | 426,3 | 16 | EtM/AEAD verificado |

### Coste por operación de subsistemas FTL/seguridad (§11, §15)

| operación | coste µs | nota |
|---|---:|---|
| `crc32c(4 KiB)` | 16,29 | integridad E2G |
| `HKDF-SHA256` | 7,40 | derivación de claves (§15) |
| `ELM health` | 0,010 | modelo de vida §11.1 |
| `PEP score (4×32)` | 0,236 | WCET < 5 µs §11.1 |
| `LMS keygen (H=8)` | 249 055 | ancla post-cuántica §15 (una vez, enrolamiento) |
| `LMS verify` | 456 | secure-boot (una vez por arranque) |
| `ZRP encode (16 pág)` | 12,0 | +6,25 % en zonas frías §8.5 |
| `ZRP recover (1 pág)` | 12,0 | reconstrucción RS(16,15), nivel 5 §12 |
| `WEP place (caché)` | 0,094 | Feistel 16 b §11.2 Balanced+ |

### WAF y durabilidad por workload (§17.1/§17.3)

| workload | ops | WAF | GC reloc | resultado |
|---|---:|---:|---:|:--:|
| W1 append secuencial | 260 | 1,188 | 0 | MFS_OK |
| W3 random en fichero | 400 | 1,161 | 780 | MFS_OK |
| W4 mixto churn 90/10 | 260 | 1,254 | 138 | MFS_OK |

### Huella de memoria estática (MFS-RES-001: sin heap)

| estructura | bytes | uso |
|---|---:|---|
| `mf_t` (núcleo) | 16 648 | instancia única §23.1 |
| `mfs_inode_ram_t` | 136 | ventana flash-first §22.3 |
| `mfs_zone_t` | 36 | tabla de zonas §24.1 |
| `mfs_iocb` | 32 | ring DAIO §13.1 |
| `mfs_wom_t` | 12 | WOM-p §11.4 |
| `mfs_health_t` | 168 | telemetría HCT §16 |
| `mfs_hwv_t` | 76 | HWV (en flash) §5.2 |

> **Nota metodológica:** los valores son cotas superiores medidas en host (no son WCET de target); la huella estática es determinista y verificable en compilación. Las desviaciones y límites conocidos se detallan en [DOCS/known-limitations.md](DOCS/known-limitations.md).

---

## 🗺️ Roadmap y estado

| Fase | Contenido | Estado |
|---|---|---|
| **1 — Núcleo y viabilidad** | Puerto, HWV, cascada HAL, viabilidad/modos, RSC+pools, extents+L2P, WAL+, UN/Nano/Compact | ✅ Implementado · ✅ KATs y suite en verde |
| **2 — Diferenciación** | GLD/WOM-p/TFC/E2G/ZLF/HCT/CCD/AGCB+/CFX+CDC/MFS-Snap/FPT/ELD | ✅ Implementado · ✅ MFS-Bench (tablas arriba) |
| **3 — Industrial** | HAWL+ · ELM/WEP/CV/RAS+TG/EBA/EDP/ZRP/PUF/cadena PQ/FormalCore + vFlash CI | ✅ Implementado (FormalCore = replay determinista + invariantes WAL+) |
| **4 — Optimización avanzada** | Hint RT-C/PEP/SDP/FSST/ZLF-Z/CQE/diccionarios on-device | ✅ Implementado (PEP, SDP, FSST-lite, ZRP, CQE) |
| **5 — Certificación** | Dossier SIL-2/21434, deprecación de suites, tooling de flota | 🟡 Tooling listo · dossier de certificación pendiente (no es código) |
| **6 — Integración con el SO** | Linux (FUSE 3, kernels 5.4+/6.x/7.x, `fstab`, systemd, udev, deb/rpm) · Windows 10/11 (WinFsp, letra de unidad, Explorador, servicio de automontaje, Inno Setup) | ✅ Implementado · ✅ montaje real verificado en Linux (39/0) · ✅ interoperabilidad Linux↔Windows · ⏳ montaje real en Windows pendiente (requiere sesión elevada) |

**Estado actual:** subsistemas de Fase 3–4 implementados y verificados — WOM-p, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP (RS(16,15)), tiering HMT T0/T1, HKDF/PUF/LMS (SP 800-208), XDAM/SDP/CQE y FormalCore. La capa de integración con el sistema operativo (carpetas `platform/linux` y `platform/windows`, con la capa portable compartida `platform/common`) está implementada y **verificada con montaje real en Linux** (39 OK / 0 fallos) e **interoperabilidad bidireccional Linux ↔ Windows**. Suite completa **935 checks / 0 fallos** con `-std=c11 -Wall -Wextra -Werror` sin avisos en Linux y Windows. Los límites y desviaciones vigentes están inventariados en [DOCS/known-limitations.md](DOCS/known-limitations.md). Ver [CHANGELOG.md](CHANGELOG.md).

---

## 📚 Documentación

- **[DOCS/…Guide.md](DOCS/MatrixFS%20Ultra%20-%20Technical%20Specifications%20and%20Implementation%20Guide.md)** — 📐 Especificación normativa completa (31 secciones): principios, HAL, viabilidad, formato físico, WAL+, pipeline CCD, FTL, EDP/DAIO, seguridad, HCT, modos, guía de implementación (API, estructuras on-flash, FSMs, constantes), plan de verificación.
- **[DOCS/](DOCS/)** — 📘 Guías de diseño e implementación por módulo (arquitectura, HAL/viabilidad, E2G, WAL/transacciones, suites criptográficas, RT/energía, layout flash, contrato de puertos, conformidad/tests).
- **[DOCS/linux-integration.md](DOCS/linux-integration.md)** — 🐧 Integración con Linux: matriz de kernels, requisitos de compilación, instalación, `fstab`, systemd/udev, paquetes deb/rpm, resolución de problemas y validación.
- **[DOCS/windows-integration.md](DOCS/windows-integration.md)** — 🪟 Integración con Windows 10/11: WinFsp, compilación con MSVC, letra de unidad, tabla de operaciones del Explorador, servicio de automontaje, instalador y validación.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — 🤝 Cómo proponer mejoras, reportar desviaciones respecto a la spec y enviar KATs.
- **[tools/](tools/)** — 🔧 `mfstool`: generador de manifiestos, analizador de trazas HCT, banco MFS-Bench v2, emparejador formal.

---

## 📣 Notificar mejoras, errores y desviaciones

El desarrollo de **MatrixFS Ultra** sigue un proceso abierto y trazable. Cualquier mejora propuesta pasa por revisión contra las reglas normativas MFS-* antes de merge.

| Tipo | Canal | Plantilla / campos obligatorios |
|---|---|---|
| 🐞 **Bug / corrupción** | [GitHub Issues → Bug report](../../issues/new?labels=bug) | Reproducción con vFlash, secuencia de operaciones, punto de corte, logs HCT exportados |
| 💡 **Mejora / feature** | [GitHub Issues → Enhancement](../../issues/new?labels=enhancement) | Sección(s) de MFS-SPEC-003 afectadas, impacto en RSC/pila/WCET, KPI MFS-Bench esperado |
| 📏 **Desviación vs. spec** | Issue con etiqueta `spec-deviation` | ID de regla (p. ej. `MFS-VIA-002`), comportamiento observado vs. normativo |
| 🔐 **Vulnerabilidad de seguridad** | **NO abrir issue público** — escribir a `security@matrixfs.example` | CVSS preliminar, suite/media afectados, PoC confidencial |
| 📝 **Mejora de documentación** | PR directo o issue `documentation` | Enlace a sección y texto propuesto |

**Política de triage:** issues etiquetados en ≤ 3 días hábiles · blocker de corrupción = prioridad P0 con análisis forense obligatorio · toda mejora aceptada se registra en [CHANGELOG.md](CHANGELOG.md) con referencia a la sección de la especificación. Las contribuciones externas se aceptan bajo Apache 2.0 (ver §5 del LICENSE) y deben incluir DCO sign-off (`git commit -s`).

---

## ⚖️ Licencia

Distribuido bajo **Apache License 2.0** — ver [LICENSE](LICENSE).

**¿Por qué Apache 2.0?** MatrixFS Ultra apunta a producto industrial (automoción, médico, infraestructura crítica). Apache 2.0:

1. **Permite uso comercial y cierre propietario** — integradores pueden usarlo en firmware de producción sin copyleft, lo que favorece adopción en sectores regulados.
2. **Otorga licencia de patentes expresa** de cada contribuyente (§3) — esencial dado que la especificación define técnicas susceptibles de patente (GC adaptativo por deuda, contadores termométricos, drenaje EDP multi-nivel, bandit determinista EXP3).
3. **Exige conservar atribución y notices** (§4) y marcar archivos modificados (§4b) — mantiene auditables las derivaciones frente a la spec MFS-SPEC-003.
4. **Sin garantía** (§7–8) — coherente con la naturaleza de referencia normativa del proyecto: cualquier certificación SIL-2/ISO 26262 exige validación propia del integrador.

Los documentos de especificación en `DOCS/` comparten la misma licencia. Las marcas «MatrixFS», «ATLAS» y «MFS-Bench» no se conceden con la licencia (§6).

© 2026 The MatrixFS Ultra contributors.

---

<div align="center">

**MatrixFS Ultra «ATLAS»** — *donde cada promesa es un artefacto auditable.*

[⬆ Volver arriba](#--matrixfs-ultra-atlas)

</div>
