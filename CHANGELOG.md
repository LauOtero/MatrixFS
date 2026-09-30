# Changelog — MatrixFS Ultra «ATLAS»

Formato basado en [Keep a Changelog](https://keepachangelog.com/es/1.1.0/) y este proyecto adhiere a [Versionado Semántico](https://semver.org/lang/es/).

Las entradas referencian secciones de la especificación normativa **MFS-SPEC-003 Edición 1.0 «ATLAS»** (`DOCS/`).

---

## [Unreleased]

### Añadido
- 📄 **README.md** para GitHub con resumen del proyecto, arquitectura, inicio rápido, garantías normativas, roadmap y política de notificación de mejoras.
- ⚖️ **LICENSE**: Apache License 2.0 (seleccionada por compatibilidad industrial + concesión expresa de patentes; justificación completa en README §Licencia).
- 🤝 `CONTRIBUTING.md` con proceso de propuestas de mejora, reporte de desviaciones vs. spec y divulgación responsable de seguridad.
- 🧪 `sim/vflash.h`: interfaz del simulador host de medios NOR/NAND dual-medio con crash/recover e inyección de fallos (§26).
- 🗂️ Estructura de carpetas `tests/`, `tools/`, `docs/`, `build/` como contenedores de las fases siguientes.

### En progreso
- ✅ Núcleo completo Fase 1–2 compilando limpio bajo `-std=c11 -Wall -Wextra`: HAL/viabilidad (§5–6), HWV+CRC-32C+TFC (§5.2/§8.4), ZLF/E2G/GLD/AGCB+/ELD (§8–13), WAL+ tokens T1/T0 + checkpoint BLAKE3 (§9), pipeline CCD SRB/LZ4/Gear-CDC/FSST-lite/CFX (§10), suites S0–S3 (§10.6), API pública POSIX-subset/VIO/DAIO/tx/snapshots/FPT/fsck (§21).
- 🔄 Tests KAT §27.1 (CRC-32C `0xE3069283`, SHA-256/HMAC RFC 4231, BLAKE3 vectors, round-trip AEAD) sobre vFlash.
- 🔄 Build system (Makefile/CMake host + cortex-m/mMsp430), `mfstool` y documentación por módulo en `docs/`.

---

## [0.1.0] — 2026-09 (esqueleto inicial)

### Añadido
- 📐 Especificación normativa MFS-SPEC-003 «ATLAS» (31 secciones) versionada en `DOCS/`.
- 🔌 Cabeceras públicas: `mfs_types.h` (26 códigos de estado, modos con presupuestos §18.2, clases RT-A/B/C, guards MFS-ARCH-010), `mfs_port.h` (contrato de puerto §20, HWV 64 B, config de viabilidad §6.1), `matrixfs.h` (API §21).
- ⚙️ Módulo interno `mfs_internal.h`: layout on-flash §22 (superblock 256 B, HWV@512, cabeceras E2G NOR 20 B / NAND 32 B, tokens 32/16 B), FSMs zona/EDP/DAB §24, pools estáticos (MFS-RES-001).
- 🔐 Crypto sin heap: SHA-256 + HMAC (FIPS 180-4/RFC 4231), BLAKE3-256 completo con modo árbol (MFS-B3-001), suites S0 (AES-256-CTR+HMAC EtM, MFS-SEC-005), S1 (AES-256-GCM), S2 (Ascon-128a SP 800-232), S3 (ChaCha20-Poly1305 RFC 8439); nonce época‖seq (MFS-SEC-001), ceroización (MFS-SEC-002), comparación constant-time.
- 🏗️ Core: cascada de detección §5.1, medición de bus SOLO lectura (MFS-BUS-001), selección de modo con desglose y margen ≥10 % (MFS-VIA-001/002 → `MFS_ENOTVIABLE`), negociación de suites §10.6; ZLF log-structured con registro E2G, cuantía de deuda GLD con meta ×2, backpressure, GC slice AGCB+ acotado temporalmente (§11.2), modelo energético ELD (§13.3); WAL+ con contador termométrico, replay acotado BMT, savepoints anidados; fsck read-only 3 niveles; HCT + export CBOR/COSE; snapshots O(1) + FlashPatch OTA.
- 🧰 `.gitignore` específico del proyecto: artefactos de build, imágenes vFlash, trazas HCT, material de clave (nunca comiteable, alineado con MFS-SEC-002).

[Unreleased]: https://github.com/<org>/matrixfs-ultra/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/<org>/matrixfs-ultra/releases/tag/v0.1.0
