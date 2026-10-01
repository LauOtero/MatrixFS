# Changelog — MatrixFS Ultra «ATLAS»

Formato basado en [Keep a Changelog](https://keepachangelog.com/es/1.1.0/) y este proyecto adhiere a [Versionado Semántico](https://semver.org/lang/es/).

Las entradas referencian secciones de la especificación normativa **MFS-SPEC-003 Edición 1.0 «ATLAS»** (`DOCS/`).

---

## [Unreleased]

### Añadido — MCU de 8 bits (MFS-ARCH-010 rev. 2) y ecosistemas embebidos
- 🟪 **Soporte de MCU de 8 bits**: se admite `arch_class = 0` en runtime (8/16/32 bits); sólo se rechaza una clase desconocida (`> 2`) con `MFS_EARCH`. Nuevos modos **8-bit Ultra/Nano/Compact** (512 B / 1 KB / 2 KB) con chunk de 64/128/256 B, elegidos automáticamente por `mfs_select_mode()` cuando `arch_class == 0`.
  - **`platform/8bit/`**: primitivas de puerto por arquitectura (**AVR, 8051, STM8, PIC16/18, Z80** + genérico) con autodetección por macros; drivers L2 (**NOR SPI, FRAM SPI/I2C, EEPROM SPI/I2C, flash interna, SD-SPI**) con barrera WOB y timeouts acotados; **autodetección de capacidades** del MCU y autoadaptación de `mfs_config`.
  - **Mínima RAM**: CRC-32C con tabla de *nibble* (64 B en `.rodata` en vez de 1 KiB en `.bss`, mismo resultado bit a bit) y dimensionado condicional de pools/scratch con `MFS_ALLOW_8BIT_TARGET` (`mf_t` ≈ 17 KB → ≈ 1.8 KB; `MFS_SCRATCH_MAX` 4096 → 256 B).
  - 🐞 **Corregidas** comparaciones de modo que asumían orden monótono (`mode >= MFS_MODE_EXTENDED`, etc.) y que habrían aplicado semántica de 16/32 bits a los modos 8-bit; nuevas funciones de familia `mfs_mode_is_8bit()` / `mfs_mode_classic_ge()`.
- 🔌 **Integraciones embebidas** (capa común `platform/embedded/` + envoltorios):
  - **`platform/embedded/`**: driver L2 sobre una región de flash plana y helpers `mfs_embedded_{setup,mount,format}` con formateo opcional.
  - **`platform/arduino/`**: librería C++ `MatrixFS` (ESP32/ESP8266/RP2040) + ejemplos; **`platform/platformio/`**: proyecto de ejemplo; **`platform/esp-idf/`**: componente externo sobre `esp_partition` con `Kconfig`; **`platform/micropython/`**: usermod con el módulo `matrixfs`.
- 🛠️ **Build**: `make 8bit` / `-DMATRIXFS_BUILD_8BIT=ON` (CMake) para la capa 8-bit; `mfstool plan` ahora cubre los 8 modos.
- 📘 **Documentación**: nueva guía [DOCS/embedded-integration.md](DOCS/embedded-integration.md) y actualización de `README.md`, `DOCS/README.md`, `DOCS/hal.md` y `DOCS/testing.md`.
- ✅ **Verificación**: suite ampliada a **1 134 checks / 0 fallos** (host Windows, gcc 16.1, `-Wall -Wextra`); capa `platform/embedded` validada de extremo a extremo sobre `sim/vflash.c` (formato, montaje, E/S y persistencia tras remontaje). Pendiente: compilación con toolchains/SDK de terceros reales (avr-gcc, sdcc, xc8, Arduino-ESP32, ESP-IDF, MicroPython).

### Reconstruido — ficheros de soporte y verificación final
Tras una pérdida accidental del árbol de trabajo, se recompusieron contra sus
contratos los ficheros de soporte (simuladores, arnés, herramientas, tests,
instaladores y documentación de módulo) y se repitió la verificación completa:
- 🧱 **Simulación**: `sim/vflash.c` (NOR con regla 1→0, violaciones y barrera WOB), `sim/vfram.{h,c}` (tier T0 byte-addressable sin borrado).
- 🧪 **Suite**: `tests/kat_crypto.c` (KATs CRC-32C, SHA-256, HMAC, BLAKE3 y round-trip AEAD S1–S3), `tests/test_gate.c` (puerta §27.7), `tests/test_tier.c` (HMT), `tests/test_xio.c` (XDAM/SDP/CQE) y `tests/test_vfs.c` (capa VFS portable).
- 🧰 **`tools/mfstool.c`** (`plan`, `check-map`, `train-dict`, `sign-profile`, `verify-hwv`, `bench`).
- 🐧 **Linux**: `scripts/matrixfs-probe`, `systemd/matrixfs@.service`, `fstab.example`, `packaging/deb/{postinst,prerm}` y `packaging/rpm/matrixfs.spec`.
- 🪟 **Windows**: `build.ps1`, `install.ps1`, `uninstall.ps1` e `installer/matrixfs.iss`.
- 📘 **Documentación de módulo**: `DOCS/{hal,wal,compact,crypto,sim}.md`.
- 🛠️ **CMake multi-toolchain**: los avisos se eligen por compilador (`/W4` en MSVC —`-Wall -Wextra` no es válido— y `-Wall -Wextra` en GCC/Clang) y `mfs_tests`/`mfsctl` incluyen la capa portable `platform/common`. `_CRT_SECURE_NO_WARNINGS` evita el ruido C4996 de la CRT.
- 🛠️ **`mfstool check-map`**: corregido el recorrido de directorios en Windows (se buscaba `*` en el directorio actual y no `<dir>/*`, de modo que no encontraba ningún fichero) y añadida política *fail-closed*: un escaneo sin fuentes falla en lugar de declarar `PASS`.
- ✅ **Verificación final**: suite **935 checks / 0 fallos** en Linux (gcc 14.2, `-Werror`) y Windows (MSVC 14.51, `/W4`) sin avisos; `check-map` y `bench` en verde; **montaje real en Linux con 39 OK / 0 fallos** (24 MiB íntegros, `truncate`, `chmod`/`chown`, `statfs`, remontaje y `ro`) e **interoperabilidad bidireccional Linux ↔ Windows**.

### Añadido — Integración con el sistema operativo (§21, §22)
- 🖥️ **`platform/common/`** — capa de integración portable, compartida por Linux y Windows:
  - `mfs_plat.{h,c}`: mutex portable (`CRITICAL_SECTION`/`pthread_mutex_t`) y tiempo de pared — el núcleo no es reentrante y todas las llamadas se serializan aquí.
  - `mfs_blk.{h,c}`: driver L2 sobre imagen, `/dev/sdX`, `/dev/mtdN` (con `MEMERASE`), `\\.\X:`, `\\.\PhysicalDriveN` y `\\?\Volume{…}\`. Detecta la alineación física (`BLKSSZGET` / `IOCTL_STORAGE_QUERY_PROPERTY`), obtiene la geometría de volúmenes crudos (`IOCTL_DISK_GET_LENGTH_INFO`) y emula la programación de granularidad fina con **RMW alineado a sector** ⇒ layout on-flash idéntico entre plataformas.
  - `mfs_vfs.{h,c}`: adaptador VFS completo (montaje, formato, sondeo, `getattr`/`readdir`/`access`/`setattr`, `open`/`create`/`read`/`write`/`flush`/`release`/`truncate`, `mkdir`/`rmdir`/`unlink`/`rename`, `statfs`/`health`/`label`/`verify`, traducción de estado a `errno`), con tabla de handles con generación.
  - `mfs_vfsctl.c`: CLI de validación sin kernel (`format`, `probe [--kv]`, `label`, `statfs`, `verify [--full]`, `ls`, `stat`, `cat`, `write`, `mkdir`, `rm`, `chmod`).
- 🐧 **`platform/linux/`** — soporte Linux (kernels 5.4+, 6.x y 7.x) mediante **FUSE 3**:
  - `matrixfs_fuse.c`: front-end FUSE 3 (envoltorio fino sobre `mfs_vfs`), con `-o device/label/format/ro/uid/gid/fmask/dmask/ram/erase_unit/allow_other`, `default_permissions` y caché de página.
  - `Makefile` (build + `install` con `PREFIX`/`DESTDIR`), `scripts/matrixfs-mkfs`, `scripts/matrixfs-mount` (*mount helper* para `/etc/fstab`), `scripts/matrixfs-probe` (sondeo para udev), `systemd/matrixfs@.service`, `udev/99-matrixfs.rules`, `fstab.example`, `install.sh`/`uninstall.sh`.
  - Empaquetado `packaging/deb` (control + scripts de mantenimiento + `build-deb.sh`) y `packaging/rpm/matrixfs.spec`.
- 🪟 **`platform/windows/`** — soporte Windows 10/11 mediante **WinFsp**:
  - `matrixfs_winfsp.c`: front-end WinFsp que expone el volumen como **unidad con letra** en el Explorador de Archivos (crear, copiar, pegar, eliminar, renombrar, cambiar atributos y tamaño), con traducción de los 29 estados del núcleo a `NTSTATUS` y conversión de rutas UTF-16 ↔ UTF-8.
  - `service/matrixfs_automount.c`: servicio `MatrixFS-Automount` (enumera volúmenes con `FindFirstVolumeW`, sondea con la capa VFS, asigna la primera letra libre y lanza un proceso de montaje por volumen, con supervisión y liberación).
  - `CMakeLists.txt`, `build.ps1`, `install.ps1`, `uninstall.ps1` e instalador `installer/matrixfs.iss` (Inno Setup, con instalación opcional de WinFsp y registro del servicio).
- 🔐 **Permisos POSIX persistidos en el núcleo** (§21.2): `uid`/`gid`/`perm` se añaden al registro INODE on-flash (extensión tolerante de 12 B, compatible con volúmenes previos); nuevas API `mf_setattr` (modo/uid/gid/mtime/tamaño) y `mf_get_label`/`mf_set_label`; `mfs_stat` extendido con `uid`/`gid`; nuevos estados `MFS_EROFS` y `MFS_EACCES`.
- 📄 **Documentación**: `DOCS/linux-integration.md` y `DOCS/windows-integration.md` (compatibilidad, requisitos de compilación, instalación, montaje, `/etc/fstab`, systemd/udev, letra de unidad, tabla de operaciones del Explorador, resolución de problemas, validación y detalles de implementación), más `platform/linux/README.md` y `platform/windows/README.md`.
- 🧪 **`tests/test_vfs.c`** (objetivo `make test` y `make mfsctl`): comprobaciones sobre la capa de integración — formato, montaje, permisos, metadatos, E/S con desplazamiento, `readdir`, `rename`, truncado, `statfs`/`label`/`verify`, **persistencia tras remontaje**, sólo lectura y mapeo de `errno`. Suite total: **935 checks, 0 fallos**.

### Corregido — compilación y ejecución nativas en Linux y Windows
Defectos detectados al compilar y ejecutar de verdad en ambos sistemas:
- 🛠️ **Portabilidad POSIX (Linux)**: con `-std=c11` estricto, glibc oculta `pthread_*`, `clock_gettime`/`CLOCK_MONOTONIC`, `pread`/`pwrite`, `nanosleep`, `strdup` y `UTIME_NOW`. Se solicita `_POSIX_C_SOURCE=200809L` al principio del puerto (`sim/mfs_port_host.c`) y de la capa portable (`mfs_plat.{h,c}`, `mfs_blk.c`, `mfs_vfs.c`, `matrixfs_fuse.c`). Compila limpio con gcc 14.2 (`-Wall -Wextra -Werror`).
- 🛠️ **WinFsp**: `<winfsp/winfsp.h>` debe incluirse **antes** que `<windows.h>` y **nunca** con `WIN32_LEAN_AND_MEAN`, o `NTSTATUS`/`PNTSTATUS` no se definen. Retirada también la macro global de `platform/windows/CMakeLists.txt`.
- 🛠️ **WinFsp `FSP_FILE_SYSTEM_INTERFACE`**: la estructura real tiene 32 entradas (empieza por `SetVolumeLabel` y añade `CanDelete`, reparse points, streams, EA, `DispatcherStopped`…). El inicializador posicional estaba desalineado; ahora usa inicializadores designados, con validación de firma por el compilador.
- 🛠️ **WinFsp `FSP_FSCTL_DIR_INFO`**: no existe el campo `FileInfoSize`; el tamaño de cada entrada se calcula como `sizeof(FSP_FSCTL_DIR_INFO) + nombre`.
- 🛠️ **Borrado desde el Explorador (Windows)**: no se ejecutaba. Implementado en `Cleanup` al recibir el flag `FspCleanupDelete` (usa `mfs_vfs_rmdir`/`mfs_vfs_unlink` según el tipo de nodo).
- 🛠️ **Empaquetado DEB**: el runtime de libfuse3 se llama `libfuse3-4` en Debian 13 (SONAME `libfuse3.so.4`) y `libfuse3-3` en Ubuntu ⇒ `Depends: libfuse3-3 | libfuse3-4`.
- 🛠️ **`.gitignore`**: el patrón `/build/` no cubría `platform/linux/build` ni `platform/windows/build`.

### Corregido — capacidad, recuperación y compilación limpia
Defectos detectados al validar el montaje real en Linux (39 OK / 0 fallos) y la
interoperabilidad con Windows:
- 🛠️ **Capacidad insuficiente**: un volumen de 64 MiB sólo era usable en ~512 KiB. Se elimina la tabla de extents de desbordamiento y los registros WAL `EXTENT` (redundantes: el registro `DATA` ya porta su LBA y la recuperación reconstruye el L2P), se resuelven los extents directamente por L2P y se dimensiona el mapa por build (`MFS_L2P_SLOTS`, 262144 en host; 4096 en el perfil embebido). Un volumen lleno devolvía `ENFILE`; ahora `MFS_ETABLEFULL` → `ENOSPC`.
- 🛠️ **Geometría de zona derivada**: el bloque de borrado se fijaba en un binario (FUSE, `matrixfs-mkfs`) y se derivaba en otro, produciendo **layouts distintos del mismo medio** (pérdida de etiqueta y datos). Ahora se deriva de forma determinista del tamaño del medio (`erase_unit` potencia de dos ≥ `tamaño/128`, acotada a 4 MiB) y ningún binario lo fuerza.
- 🛠️ **Corrupción silenciosa de la zona 0**: `mfs_extent_read` devolvía `0` como "página ausente", pero el ppage 0 es un destino válido. Introducidos los centinelas `MFS_L2P_FREE` (hueco) y `MFS_PPAGE_ERROR` (página viva ilegible ⇒ `MFS_ECORRUPT`).
- 🛠️ **Estado obsoleto tras remontar**: la reconstrucción aplicaba los registros por índice de zona y el GC reutiliza zonas de índice bajo, resucitando versiones antiguas. Ahora el orden es **cronológico por secuencia de zona** (`recovery_zone_order`) y `fs->seq` se restaura del superblock.
- 🛠️ **Ventana de inodos**: con el orden cronológico, la ventana se llenaba con los inodos más antiguos y un fichero reciente desaparecía. Desalojo por **recencia** (`ino_window_take`). Además, `keep_parent` truncaba el inodo padre a 16 bits.
- 🛠️ **GC**: se verifica el invariante `zone_live_pages == 0` antes de borrar una zona, y la relocalización copia el registro **literalmente** (el flag `dict` viaja en el registro).
- 🛠️ **`cat` binario en Windows**: la CRT traducía `\n` → `\r\n` al volcar un fichero (1 MiB ⇒ 1 052 601 B). `stdout` en modo binario (`_setmode(_O_BINARY)`).
- 🛠️ **Avisos MSVC `/W4`**: casts de constantes de 16 bits a 8 bits (`MFS_REC_MAGIC`/`MFS_TOK_MAGIC`) y conversiones implícitas en la reconstrucción. Los tres binarios de Windows compilan **sin avisos**, igual que gcc con `-Werror`.
- ✅ **Validación real**: `mount -t matrixfs` en Linux (FUSE 3) con **39 OK / 0 fallos** e **interoperabilidad bidireccional Linux ↔ Windows**.

### Corregido — etiqueta de superblock y entrada en caliente de medios
- 🛠️ **Lectura de la etiqueta de volumen**: `mf_init` leía la etiqueta del **primer slot con etiqueta válida** (siempre A), de modo que devolvía etiquetas obsoletas tras la rotación del superblock. Ahora se lee del **slot ganador por `{época, seq}`**, coherente con la selección del superblock vigente. `mfs_vfs_probe` aplica la misma regla.
- 🛠️ **Aplicación de la etiqueta al formatear**: `mf_format` limpia la instancia (`memset`) y `mf_set_label` exige volumen montado, por lo que la etiqueta pedida no llegaba al medio. Ahora se aplica tras formatear y montar (en `mfs_vfs_format` y en la ruta `format_if_needed`).
- 🛠️ **Driver de bloque en Windows**: `GetFileSizeEx` devuelve 0 sobre handles de volumen ⇒ el montaje de particiones crudas fallaba. Se consulta la longitud con `IOCTL_DISK_GET_LENGTH_INFO`; además se usa `CreateFileW` (necesario para rutas `\\?\Volume{…}\`) y la E/S se realiza alineada a sector también en lectura.

### Añadido
- 📄 **README.md** para GitHub con resumen del proyecto, arquitectura, inicio rápido, garantías normativas, roadmap y política de notificación de mejoras.
- ⚖️ **LICENSE**: Apache License 2.0 (seleccionada por compatibilidad industrial + concesión expresa de patentes; justificación completa en README §Licencia).
- 🤝 `CONTRIBUTING.md` con proceso de propuestas de mejora, reporte de desviaciones vs. spec y divulgación responsable de seguridad.
- 🗂️ Estructura de carpetas `tests/`, `tools/`, `DOCS/`, `build/`.
- 🧪 **`sim/vflash.c`**: simulador NOR completo (§27.2) — regla "prog sólo aclara bits" con contador de violaciones de disciplina, read-back post-program (barrera WOB), contador P/E por bloque, corte de energía (`vf_crash/vf_recover`) e inyección de fallo (`vf_fail_next_prog`).
- 🔌 **`sim/mfs_port_host.c`**: puerto de host (§20.2) para ejecutar el núcleo sobre vFlash en PC.
- 🧰 **`tools/mfstool`** (§26): `plan` (genera `matrixfs_resources.h` con `static_assert` + certificado RSC firmado), `check-map` (MFS-RES-002), `train-dict`, `sign-profile`, `verify-hwv`, `bench`.
- 🏗️ **Build**: `Makefile` (lib/test/bench/check-map/plan/strict) y `CMakeLists.txt` con `ctest`.
- ✅ **Suite de verificación** (§27): KATs criptográficos (§27.1), tests de puerta (§27.7), funcionales y FIH (§27.2), más MFS-Bench v2 reducido (§17). **230 checks, 0 fallos**.
- 📚 **Documentación `DOCS/`**: índice, arquitectura, HAL, núcleo, WAL, zonas, pipeline CCD, cripto, simulador, tooling, plan de verificación y limitaciones conocidas.
- 🚫 **`.gitignore`** específico: artefactos de build, generados por `mfstool`, imágenes vFlash, trazas HCT y material de clave (MFS-SEC-002).

### Añadido — Fase 3–5 (subsistemas industriales)
- 🧬 **`src/ftl/mfs_ftl2.c`** — FTL Ultra 2 (§11): **WOM-p** bit-lane multigeneración (§11.4), **SLEC** (§11.7), **EBA** ECC adaptativa por región (§11.8), **RAS + Thermal Governor** con retención efectiva por temperatura y read-disturb (§11.9), **ELM** (salud/RUL/EMA/proactivo) y **PEP** (§11.1), **WEP** Feistel 16 b (§11.2), **ZRP** RS(16,15) sobre GF(2⁸) con relocalización del rescate (§8.5).
- 🗄️ **`src/tier/mfs_hmt.c`** — tiering heterogéneo T0/T1 (§11.11): token primero en T0, espejo de metadatos en T0 y recuperación por último registro por clave.
- 🔐 **`src/sec/mfs_pq.c`** — **HKDF-SHA256** (RFC 5869, KAT caso 1), **PUF** con *fuzzy extractor* y fallback, **LMS** hash-based SP 800-208 (keygen/firma/verificación, n=32, w=4, H=8).
- 🔌 **`src/xio/mfs_xio.c`** — **XDAM** (XIP con muestreo E2G e invalidación por época, §14.1), **SDP** (sensor → DMA/CRC → pool → `O_RAW` al WAL, §14.1), **CQE** (cola HW de tareas DAIO, §11.5).
- 🧪 **`sim/vfram.c`** — simulador de tier T0 byte-addressable (FRAM/MRAM, sin erase).
- ✅ **Tests extremos**: `test_ftl2.c`, `test_tier.c`, `test_pq.c`, `test_xio.c`, `test_stress.c` (churn+GC/WAF, barrido de 1 000 cortes, tormenta de metadatos, matriz S0–S3 con remontaje, agotamiento de zonas), `test_formal.c` (FormalCore) y `bench_extreme.c` (`--extreme`). Suite total: **795 checks, 0 fallos**.
- 📊 **MFS-Bench extremo** (§17) integrado en el **README.md**: rendimiento por modo, throughput criptográfico por suite, coste por operación de subsistema, WAF por workload y huella estática de memoria.
- 🧮 **Telemetría HCT** ampliada con 17 contadores nuevos (`zrp_*`, `slec_folded`, `eba_violations`, `tg_deferred`, `elm_proactive`, `pep_overrides`, `wom_gen`, `xdam_*`, `sdp_*`, `cqe_*`, `puf_*`, `pq_verifies`).

### Corregido — asignación de zonas, GC y backpressure
- 🛠️ **Fuga de zonas**: `mfs_zone_alloc_open` abandonaba una zona `OPEN` sin espacio para otra página y abría otra nueva, dejando la anterior invisible para la GC (irrecuperable). Ahora se **sella** para que la GC pueda reclamarla.
- 🛠️ **Contabilidad de deuda GLD**: la deuda crecía en cada página superada pero sólo se reducía al **relocalizar** un registro, de modo que las páginas obsoletas nunca la saldaban ⇒ backpressure permanente tras pocas escrituras. Ahora cada página procesada por la GC salda su deuda y la relocalización no contabiliza la copia que se está reclamando.
- 🛠️ **Reclamación forzosa**: la heurística de deuda se reinicia tras un montaje, dejando un medio lleno inescribible. Añadido `mfs_gc_force()` invocado por el asignador cuando no hay zona libre, y zona de **reserva de GC** (`MFS_GC_RESERVE`) como destino de relocalización.
- 🛠️ **Zonas totalmente obsoletas**: se liberan directamente (sin destino de relocalización) cuando no queda ninguna zona libre, permitiendo progresar, y el drenaje de deuda itera de forma acotada.
- 🛠️ **Metadatos**: las escrituras de metadatos (`mfs_wal_append`) también disparan el control de deuda, no sólo las de datos.
- 🛠️ **WOM-p**: la programación de la generación *g* ahora preserva los bits de generaciones previas (`val = lane & cur`), evitando violaciones de la regla NOR 1→0.
- 🛠️ **XDAM**: la invalidación por época comparaba la época del fichero en lugar de `xdam_epoch`, por lo que el *bump* no invalidaba los activos.
- 🛠️ **CRC de registro cifrado**: el campo de longitud se fijaba después de recalcular el CRC ⇒ fallos de integridad tras remount.

### Estado de las fases
- ✅ Núcleo completo Fase 1–2 compilando limpio bajo `-std=c11 -Wall -Wextra` (y `-Werror`): HAL/viabilidad (§5–6), HWV+CRC-32C+TFC (§5.2/§8.4), ZLF/E2G/L2P/GLD/AGCB+/ELD (§8–13), WAL+ tokens T1/T0 + checkpoint BLAKE3 + TXMARK (§9), pipeline CCD SRB/LZ4/Gear-CDC/FSST-lite/CFX (§10), suites S0–S3 (§10.6), API pública POSIX-subset/VIO/DAIO/tx/snapshots/FPT/fsck (§21).
- ✅ Fase 3–4 implementada (WOM-p, SLEC, EBA, RAS/TG, ELM/PEP, WEP, HMT, ZRP, PUF, LMS, XDAM/SDP/CQE, FormalCore).
- ✅ Fase 6 (integración con el SO) implementada y **verificada**: Linux (FUSE 3) y Windows 10/11 (WinFsp) sobre la capa portable `platform/common`, con montaje, permisos persistidos, automontaje y empaquetado. Montaje real en Linux con **39 OK / 0 fallos** e **interoperabilidad bidireccional Linux ↔ Windows**. Suite de **935 checks / 0 fallos**.
- 🟡 Pendiente: dossier de certificación (SIL-2/21434), model-checking TLA+ y MC/DC ≥ 90 %, rotación dinámica de la reserva de GC al llenado total del medio, montaje real en Windows (requiere sesión elevada) y ejecución en la matriz de kernels/versiones de Windows. Ver `DOCS/known-limitations.md`.

### Corregido (bugs funcionales del núcleo)
- 🛠️ **Direcciones de zona**: las zonas se direccionaban de forma inconsistente (colisión con superblock/HWV). Ahora la región baja ocupa sectores propios (SB A/HWV, SB B, anillo de tokens) y las zonas empiezan tras ellos.
- 🛠️ **Índice de página física**: la lectura no descontaba la cabecera de zona (`idx = off/chunk` vs `ppage`), lo que impedía releer datos. Corregido a `idx = (off − ZONEHDR)/chunk`.
- 🛠️ **CRC del superblock**: se calculaba sobre un rango que incluía el propio campo CRC ⇒ imposible de validar. Ahora cubre 0..102.
- 🛠️ **Mapa L2P**: era un array indexado por LBA que nunca se poblaba para datos; GC podía borrar datos vivos. Sustituido por tabla hash estática con resolución correcta de versiones.
- 🛠️ **Descompresión**: LZ4 se aplicaba al escribir pero nunca al leer ⇒ datos corruptos en modos > Ultra-Nano. Corregido en `mfs_data_read` (flag de codec preservado por GC).
- 🛠️ **Persistencia de metadatos**: la recuperación no desenvolvía la envoltura `WALENT`, por lo que tras remount se perdían nombres y datos. Ahora se desempaqueta y se despacha por tipo interno.
- 🛠️ **Rollback de transacciones**: `mf_tx_abort` no deshacía el estado en RAM y `mf_sync` resucitaba los cambios abortados. Añadidas **marcas TXMARK** (gating de replay) y restauración de estado.
- 🛠️ **Savepoints**: el rollback no restauraba tamaño/extents. Ahora se toma snapshot de inodos + tabla de extents de desbordamiento.
- 🛠️ **`mf_read`**: entregaba bytes más allá de EOF. Corregido el acotado por tamaño de inodo.
- 🛠️ **Capacidad de payload**: `mf_write` usaba el chunk completo sin descontar cabecera E2G ⇒ `MFS_EINVAL`. Introducido `mfs_payload_bytes()`.
- 🛠️ **Zonas multi-bloque** (§8.1): un chunk de 4 KB no cabía en un sector de 4 KB; ahora una zona agrupa 1–4 bloques según geometría y modo.
- 🛠️ **Tokens**: el contador de secuencia no se restauraba tras remount y se reprogramaban slots sin borrar (violación NOR). Ahora persiste y se verifica el slot antes de programar.
- 🛠️ **BLAKE3**: la salida de la función de compresión XOR-eaba el CV dos veces y los contadores de chunk del árbol eran incorrectos ⇒ no coincidía con los vectores oficiales.
- 🛠️ **Suites cripto**: S1 (AES-GCM) y S2 (Ascon) verificaban el tag sobre el texto claro (no sobre el ciphertext) ⇒ fallo de autenticación; S0 no autenticaba el nonce. Corregidas (verificación antes de descifrar, nonce autenticado en S0).
- 🛠️ **DAIO**: `mf_poll` interpretaba mal `cb->buf` (handle de archivo) ⇒ fallo de acceso. Corregido.
- 🛠️ **Símbolos**: `mfs_gld_maybe_gc`/`mfs_zone_count_free`/`mfs_zone_erase` sin definir, `mfs_assert_fail` duplicado y solape de campos en la serialización del HWV.

---

## [0.1.0] — 2026-09 (esqueleto inicial)

### Añadido
- 📐 Especificación normativa MFS-SPEC-003 «ATLAS» (31 secciones) versionada en `DOCS/`.
- 🔌 Cabeceras públicas: `mfs_types.h` (26 códigos de estado, modos con presupuestos §18.2, clases RT-A/B/C, guards MFS-ARCH-010), `mfs_port.h` (contrato de puerto §20, HWV 64 B, config de viabilidad §6.1), `matrixfs.h` (API §21).
- ⚙️ Módulo interno `mfs_internal.h`: layout on-flash §22 (superblock 256 B, HWV@512, cabeceras E2G NOR 20 B / NAND 32 B, tokens 32/16 B), FSMs zona/EDP/DAB §24, pools estáticos (MFS-RES-001).
- 🔐 Crypto sin heap: SHA-256 + HMAC (FIPS 180-4/RFC 4231), BLAKE3-256 completo con modo árbol (MFS-B3-001), suites S0 (AES-256-CTR+HMAC EtM, MFS-SEC-005), S1 (AES-256-GCM), S2 (Ascon-128a SP 800-232), S3 (ChaCha20-Poly1305 RFC 8439); nonce época‖seq (MFS-SEC-001), ceroización (MFS-SEC-002), comparación constant-time.
- 🏗️ Core: cascada de detección §5.1, medición de bus SOLO lectura (MFS-BUS-001), selección de modo con desglose y margen ≥10 % (MFS-VIA-001/002 → `MFS_ENOTVIABLE`), negociación de suites §10.6; ZLF log-structured con registro E2G, cuantía de deuda GLD con meta ×2, backpressure, GC slice AGCB+ acotado temporalmente (§11.2), modelo energético ELD (§13.3); WAL+ con contador termométrico, replay acotado BMT, savepoints anidados; fsck read-only 3 niveles; HCT + export CBOR/COSE; snapshots O(1) + FlashPatch OTA.
- 🧰 `.gitignore` específico del proyecto.

[Unreleased]: https://github.com/<org>/matrixfs-ultra/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/<org>/matrixfs-ultra/releases/tag/v0.1.0
