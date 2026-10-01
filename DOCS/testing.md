# Plan de verificación y resultados

Spec: §27. Ejecutable: `mfs_tests.exe` (`make test`; `--extreme` añade MFS-Bench
extremo §17).
Framework: `tests/mfs_test.h` (contadores de `checks`/`fallos`).

## Cobertura por bloque (§27)

| Bloque | Spec | Fichero | Contenido |
|---|---|---|---|
| KATs criptográficos | §27.1 | `tests/kat_crypto.c` | CRC-32C, SHA-256, HMAC (RFC 4231), BLAKE3 (vectores), round-trip autenticado S0–S3 + tamper/nonce |
| Rechazo temprano | §27.7 | `tests/test_gate.c` | `MFS_EARCH`, `MFS_ENOTVIABLE`, matriz de viabilidad §6.3 |
| Funcional | §21, §9, §10.8, §12.3, §13.1, §16 | `tests/test_fs.c` | POSIX-subset, multipágina, directorios, persistencia, TX/savepoints, snapshots, EDP, fsck/HCT, DAIO |
| FIH (matriz de fallos) | §27.2 | `tests/test_fih.c` | corte de energía (W6), escritura rasgada, disciplina NOR, bit-flip E2G |
| FTL Ultra 2 | §11.1, §11.2, §11.4, §11.7–§11.9, §8.5 | `tests/test_ftl2.c` | WOM-p multigeneración, SLEC, EBA, RAS+Thermal Governor, ELM/PEP, WEP, ZRP RS(16,15) |
| Tiering HMT | §11.11 | `tests/test_tier.c` | activación T0, espejo de metadatos en T0, degradación por fallo de T0 |
| Cripto avanzada | §15 | `tests/test_pq.c` | KAT HKDF-SHA256 (RFC 5869), PUF (enrolamiento/reproducción/fallback), LMS SP 800-208 |
| E/S acelerada | §14.1, §11.5 | `tests/test_xio.c` | XDAM (muestreo E2G, invalidación por época), SDP, CQE |
| Estrés | §27.2 | `tests/test_stress.c` | churn+GC/WAF, 1 000 cortes + remontaje, tormenta de metadatos, matriz S0–S3, agotamiento de zonas |
| Formal | §24 | `tests/test_formal.c` | replay determinista (DAB/CUSUM/EDP), invariantes WAL+ (replay/gating) |
| Capa de integración (VFS portable) | §21, §22 | `tests/test_vfs.c` | formato, montaje, permisos POSIX persistidos (chmod/chown), metadatos, E/S con desplazamiento, readdir, rename, truncado, statfs/label/verify, persistencia tras remontaje, sólo lectura, mapeo de errno |
| Bench | §17 | `tests/bench.c`, `tests/bench_extreme.c`, `mfstool bench` | W1/W3/W4, percentiles, WAF, montaje, coste por subsistema, huella estática |

### Verificación de la capa de integración sin kernel

`tests/test_vfs.c` ejerce `platform/common/mfs_vfs.c` — la **misma ruta de
código** que consumen los front-ends FUSE 3 (Linux) y WinFsp (Windows) — sobre
una imagen de 1 MiB con `mfs_blk`, sin necesidad de FUSE, WinFsp ni
privilegios. Cubre 82 comprobaciones: formato y sondeo, montaje, `mkdir`/
`create` con permisos explícitos, escritura y relectura con desplazamiento,
`getattr` con `mode`/`uid`/`gid`, `chmod` vía `setattr`, comprobación de acceso
POSIX (propietario/grupo/otros y root), `readdir`, `rename`, truncado,
`statfs`/`health`/`verify`, **persistencia de permisos y datos tras remontar**,
montaje de sólo lectura (`EROFS`) y traducción de estados a `errno`.

Además, el CLI `matrixfs-ctl` (objetivo `make mfsctl`) permite reproducir
manualmente el ciclo completo sobre cualquier medio:

```sh
make mfsctl
./mfsctl format vol0.img --label DATOS
./mfsctl probe  vol0.img
./mfsctl mkdir  vol0.img /docs
./mfsctl write  vol0.img /docs/nota.txt "hola"
./mfsctl ls     vol0.img /docs
./mfsctl cat    vol0.img /docs/nota.txt
./mfsctl chmod  vol0.img /docs/nota.txt 0600
./mfsctl statfs vol0.img
./mfsctl verify vol0.img --full
```

## Qué verifica cada prueba FIH

- **Corte de energía**: 100 ciclos `vf_crash()`/`vf_recover()` + remount; el
  fichero confirmado se lee idéntico en todos ellos (cero corrupción).
- **Escritura rasgada**: fallo del medio en la próxima programación y datos sin
  confirmar; tras el corte el fichero no reaparece con contenido distinto al
  confirmado.
- **Disciplina NOR**: workload mixto (crear/escribir/sync/remount/borrar);
  `n_violations == 0` ⇒ no se reprograma ninguna celda sin borrado previo.
- **Bit-flip E2G**: se voltea un bit de una celda programada; el montaje es
  válido o tipificado, y la lectura nunca entrega datos incorrectos
  (coincidencia exacta, cero bytes o ceros).

## Resultado de referencia (host: gcc 14.2 y MSVC 14.51, `-O2`)

```
checks: 935   fallos: 0

Estrés: churn GC+WAF OK · 1 000 cortes sin corrupción · tormenta de metadatos OK
        matriz S0–S3 con remontaje OK · agotamiento con errores tipificados
FormalCore: replay determinista OK · 64 combinaciones WAL+ con 0 violaciones

W1 append 96×492 B : 71.61 MB/s  (p50=6 us, p99=9 us, p99.9=9 us)
W3 random 246 B    : 96/96 ok    (p50=6 us, p99=23 us, p99.9=23 us)
HCT: prog=394 host=341  WAF=1.155  GC-reloc=0  deuda=155
Montaje: 547 us  (≤ 12000 us)
vflash: violations=0
```

Las tablas completas del MFS-Bench extremo (rendimiento por modo, throughput
criptográfico, coste por subsistema, WAF por workload y huella estática) están
publicadas en el README raíz; se regeneran con `mfs_tests.exe --extreme`.

Los objetivos §17.2 (p99.9 commit RT-A ≤ 2.5 ms, montaje ≤ 12 ms, WAF(W4) ≤
1.25) se cumplen en el banco reducido. Los objetivos de tasas absolutas (W1 ≥
28 MB/s) dependen del bus real: el simulador no modela latencias de programación
(un `prog` es `memcpy`), por lo que las cifras de MB/s no son concluyentes en
host.

## CI sugerido

```sh
make strict      # -Werror: sin warnings
make test        # KAT + puertas + funcional + FIH + capa VFS
make check-map   # ausencia de heap (MFS-RES-001/002)
make plan        # certificado RSC
make bench
make mfsctl      # CLI de la capa de integración
```

## Validación de los front-ends de plataforma

Los front-ends de plataforma dependen de marcos externos (libfuse3 y el SDK de
WinFsp), por lo que su validación se reparte entre la lógica portable (probada
en cualquier host) y la compilación/ejecución nativa.

### Resultados obtenidos

| Elemento | Entorno | Resultado |
|---|---|---|
| Semántica del sistema de archivos | `tests/test_vfs.c` + `mfsctl` sobre imagen real | ✅ formato y sondeo, montaje, permisos POSIX (`chmod`/`chown`), metadatos, E/S con desplazamiento, `readdir`, `rename`, truncado, `statfs`/`label`/`verify`, persistencia tras remontar y montaje `ro`; misma ruta que usan FUSE y WinFsp |
| Todo el proyecto | **Linux** (Debian 13, kernel 6.18, gcc 14.2) con `-std=c11 -Wall -Wextra -Werror` | ✅ 0 avisos; suite **935 / 0** |
| Todo el proyecto | **Windows** (MSVC 14.51 `/W4`, CMake + Visual Studio) | ✅ 0 avisos; suite **935 / 0** |
| **Montaje real en Linux** (FUSE 3, `mount -t matrixfs`) | Debian 13 + **libfuse3 3.17.2** real, root | ✅ **39 OK / 0 fallos**: aparece en `/proc/mounts` como `fuse.matrixfs`, fichero aleatorio de 24 MiB íntegro (`cmp` y md5), `cp`, `truncate` con prefijo intacto, `rename`, borrado, `chmod`/`chown` persistentes, `statfs`, remontaje con md5 idéntico, montaje `ro` (`EROFS`) y enlaces simbólicos rechazados |
| **Interoperabilidad Linux ↔ Windows** | gcc 14.2 + MSVC 14.51 | ✅ volumen creado en Windows leído en Linux (contenido y `verify=MFS_OK`) y volumen creado en Linux leído en Windows (contenido idéntico, `ls` y `verify=MFS_OK`) |
| `matrixfs_fuse` (FUSE 3) | Linux + **libfuse3 3.17.2** real | ✅ compila, enlaza (`libfuse3.so.4`) y **monta volúmenes reales** |
| `matrixfs_winfsp.exe` | Windows + **MSVC 14.51** + SDK de WinFsp real (`/W4`) | ✅ compila sin avisos y enlaza con `winfsp-x64.lib`; carga `winfsp-x64.dll` y llama a `FspFileSystemCreate` |
| `matrixfs-ctl.exe`, `matrixfs_automount.exe` | MSVC 14.51 (`/W4`) | ✅ compilan sin avisos y enlazan; el CLI funciona de extremo a extremo (`format`/`probe`/`mkdir`/`write`/`ls`/`cat`/`statfs`/`verify`) |
| `matrixfs_winfsp.exe` con MinGW | MinGW GCC | ⛔ el SDK de WinFsp no es compatible con MinGW (intrínsecos de MSVC); requiere MSVC/clang-cl |

### Pendiente (requiere privilegios)

El montaje efectivo **en Linux ya se ha completado** (con `root`, véase la tabla
anterior). Queda pendiente el montaje real en Windows, que exige una sesión
elevada:

- **Windows**: `FspFileSystemCreate` devuelve `0xC000000E`
  (`STATUS_NO_SUCH_DEVICE`) porque el driver FSD de WinFsp no es accesible desde
  una sesión no elevada (el entorno tiene un despliegue SxS de desarrollo con
  `fsptool load` como paso previo).

Los procedimientos completos están en
[`linux-integration.md`](linux-integration.md#9-validación-realizada) y
[`windows-integration.md`](windows-integration.md#101-pendiente-de-ejecutar-requiere-privilegios).

### Defectos corregidos gracias a la compilación nativa

1. Extensiones POSIX invisibles con `-std=c11` estricto en glibc → se añadió
   `_POSIX_C_SOURCE=200809L` en el puerto y en la capa portable.
2. Nombre real del paquete de runtime de libfuse en Debian 13 (`libfuse3-4`) →
   `Depends: libfuse3-3 | libfuse3-4` en el `.deb`.
3. `<winfsp/winfsp.h>` debe preceder a `<windows.h>` y sin
   `WIN32_LEAN_AND_MEAN`, o `NTSTATUS`/`PNTSTATUS` no se definen.
4. `FSP_FILE_SYSTEM_INTERFACE` real de WinFsp 1.12 (32 entradas) →
   inicializadores designados.
5. `FSP_FSCTL_DIR_INFO` no tiene `FileInfoSize` (campo asumido por error).
6. El borrado desde el Explorador no se ejecutaba: se implementó en `Cleanup`
   con el flag `FspCleanupDelete`.
7. **Capacidad**: el volumen de 64 MiB sólo era usable en ~512 KiB. La tabla de
   extents de desbordamiento (256 entradas) se sustituyó por resolución directa
   por L2P (el propio registro `DATA` porta su LBA) y el L2P se dimensiona por
   build (`MFS_L2P_SLOTS`); un fallo de escritura devolvía `ENFILE` en lugar de
   `ENOSPC`.
8. **Geometría de zona**: el bloque de borrado se fijaba en un binario y se
   derivaba en otro ⇒ layouts distintos del mismo medio (etiqueta y datos
   perdidos). Ahora se deriva de forma determinista del tamaño del medio.
9. **Páginas de la zona 0**: `mfs_extent_read` usaba `0` como "página ausente",
   pero el ppage 0 es válido ⇒ corrupción silenciosa. Introducidos los
   centinelas `MFS_L2P_FREE` y `MFS_PPAGE_ERROR`.
10. **Estado obsoleto tras remontar**: la reconstrucción aplicaba los registros
    por índice de zona, y el GC reutiliza zonas de índice bajo. Ahora el orden
    es cronológico por secuencia de zona (`recovery_zone_order`) y la secuencia
    se restaura del superblock.
11. **Ventana de inodos**: al ordenar cronológicamente, la ventana se llenaba
    con los inodos más antiguos y un fichero reciente desaparecía. Desalojo por
    **recencia** (`ino_window_take`).
12. **Salida binaria del CLI en Windows**: la CRT traducía `\n` → `\r\n` al
    volcar un fichero (`cat` de 1 MiB devolvía 1 052 601 bytes).
    `_setmode(_O_BINARY)` sobre `stdout`.
13. **Truncamientos con MSVC `/W4`**: conversiones implícitas en la
    reconstrucción de metadatos (entre ellas el inodo padre a 16 bits), casts de
    constantes de 16 bits a 8 bits y un `-1` pasado a un parámetro `UINT32`.
    Compilación final **sin avisos** en MSVC y en gcc (el único resto era un
    `C4324` de la cabecera del SDK de WinFsp, silenciado explícitamente).

## Cobertura pendiente (§27)

- **MC/DC ≥ 90 %** en WAL/token/recovery/E2G/GLD: requiere `gcov`/`llvm-cov`
  (no incluido en este repositorio).
- **Model-checking TLA+/Promela**: no incluido; `tests/test_formal.c`
  (FormalCore) cubre replay determinista y exploración acotada de invariantes
  WAL+, y **FIH a 10⁵ cortes / 10⁴ drenajes**: la suite ejecuta una versión
  reducida por coste de CI.
- **Stack-painting** por modo/target y **matriz de MCU** real: requieren target.
