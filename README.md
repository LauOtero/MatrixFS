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
| 6 | **FTL Ultra 2** | ZLF log-structured, L2P, AGCB+ sobre deuda GLD, E2G extremo-a-extremo (§8.2, §11) |
| 7 | **Resiliencia energética** | EDP drenaje 5 niveles, DAB bandit determinista EXP3, presupuesto ELD (§12.3, §24.4, §13.3) |
| 8 | **Tiempo real** | DAIO v2 con deadlines, clases RT-A/B/C, TCB-DA, p99.9 ≤ 3 ms RT-A (§13) |
| 9 | **Seguridad industrial** | Crypto-agility S0→S3, ancla post-cuántica hash-based (SLH-DSA/LMS), PUF, cero secretos en reposo (§15) |
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
│   └── crypto/
│       ├── mfs_sha256.c                   #    FIPS 180-4 + HMAC RFC 4231 (streaming, sin heap)
│       ├── mfs_blake3.c                   #    BLAKE3-256 completo (hash/keyed/árbol) + MAC tokens
│       └── mfs_suites.c                   #    Suites S0–S3: AES-CTR+HMAC · AES-GCM · Ascon-128a · ChaCha20-Poly1305
├── sim/                                   # 🧪 vFlash host (simulador NOR/NAND dual-medio)
│   └── vflash.h                           #    Geometría, crash/recover, inyección de fallos
├── tests/                                 # ✅ Plan de verificación §27 (KATs, FIH, conformidad)
├── tools/                                 # 🔧 mfstool: manifiestos, trazas HCT, bench, analizador
├── docs/                                  # 📚 Documentación de diseño e implementación
├── build/                                 # 🛠️ Makefile / CMake (host + targets ARM Cortex-M / MSP430)
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

- **KATs:** CRC-32C `"123456789"` = `0xE3069283`; SHA-256/HMAC (RFC 4231); BLAKE3 vectors oficiales; round-trip AEAD S0–S3.
- **Invariantes:** orden WAL↔datos, integridad E2G, monotonía TFC, coherencia L2P↔ART.
- **FIH (fault injection):** 10⁵ cortes aleatorios sin corrupción; 10⁴ drenajes EDP; fatiga acelerada de bloques.
- **Conformidad:** matriz §27 con huella de cada regla MFS-* sobre el código.
- **Formal (Fase 3):** modelos TLA+ de transaccional y autómatas; MC/DC ≥ 90 %.

Ejecutar todo: `make test && make fih-short` (el FIH completo requiere CI nocturno).

---

## 🗺️ Roadmap y estado

| Fase | Contenido | Estado |
|---|---|---|
| **1 — Núcleo y viabilidad** | Puerto, HWV, cascada HAL, viabilidad/modos, RSC+pools, extents+L2P, WAL+, UN/Nano/Compact | ✅ Código implementado · 🔄 tests KAT en curso |
| **2 — Diferenciación** | GLD/WOM-p/TFC/E2G/ZLF/HCT/CCD/AGCB+/CFX+CDC/MFS-Snap/FPT/ELD | ✅ Núcleo funcional · 🔄 validación MFS-Bench |
| **3 — Industrial** | HAWL+·ELM/WEP/CV/RAS+TG/EBA/EDP/ZRP/PUF/cadena PQ/FormalCore+vFlash CI | 📋 Planificado |
| **4 — Optimización avanzada** | Hint RT-C/PEP/SDP/FSST/ZLF-Z/CQE/diccionarios on-device | 📋 Parcial (FSST-lite ✓) |
| **5 — Certificación** | Dossier SIL-2/21434, deprecación de suites, tooling de flota | 📋 Planificado |

**Estado actual:** Fase 1–2 con todos los módulos del núcleo compilando limpio (`-std=c11 -Wall -Wextra`); fases de simulación/test/build/documentación en integración. Ver [CHANGELOG.md](CHANGELOG.md).

---

## 📚 Documentación

- **[DOCS/…Guide.md](DOCS/MatrixFS%20Ultra%20-%20Technical%20Specifications%20and%20Implementation%20Guide.md)** — 📐 Especificación normativa completa (31 secciones): principios, HAL, viabilidad, formato físico, WAL+, pipeline CCD, FTL, EDP/DAIO, seguridad, HCT, modos, guía de implementación (API, estructuras on-flash, FSMs, constantes), plan de verificación.
- **[docs/](docs/)** — 📘 Guías de diseño e implementación por módulo (arquitectura, HAL/viabilidad, E2G, WAL/transacciones, suites criptográficas, RT/energía, layout flash, contrato de puertos, conformidad/tests).
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
