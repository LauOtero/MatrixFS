# Build y `mfstool`

Spec: §26. Ficheros: `Makefile`, `CMakeLists.txt`, `tools/mfstool.c`.

## Build

```sh
make            # libmatrixfs.a + mfs_tests.exe + mfstool.exe + mfsctl.exe
make lib        # sólo la librería estática del núcleo + crypto
make test       # compila y ejecuta la suite de verificación (§27) + capa VFS
make bench      # ejecuta MFS-Bench v2 reducido (§17)
make mfsctl     # CLI de la capa de integración (platform/common)
make check-map  # verifica ausencia de heap (MFS-RES-002)
make plan       # genera matrixfs_resources.h + certificado RSC
make strict     # compila con -Werror
make clean
```

### Build de los front-ends de plataforma

Los front-ends de sistema operativo tienen su propio build porque dependen de
marcos externos:

| Plataforma | Comando | Requisitos | Documento |
|---|---|---|---|
| Linux | `cd platform/linux && make [install]` | libfuse3 + pkg-config | [linux-integration.md](linux-integration.md) |
| Windows | `cd platform/windows && .\build.ps1` | MSVC + CMake + SDK de WinFsp | [windows-integration.md](windows-integration.md) |

Ambos comparten la capa portable `platform/common/`, que también se compila en
el build raíz (para la suite y el CLI `mfsctl`).

Variables: `CC`, `CFLAGS`, `LDFLAGS`, `LDLIBS`. Con CMake:

```sh
cmake -S . -B build-cmake && cmake --build build-cmake && ctest --test-dir build-cmake
```

Pipeline obligatorio (§26): `plan → build → check-map → KATs → vFlash (FIH) →
bench → firmar certificado`.

## `mfstool`

| Comando | Función |
|---|---|
| `plan` | Genera `matrixfs_resources.h` (con `static_assert` por modo) y `rsc_certificate.txt` firmado (RAM por modo, WCET DAIO, cotas §17.2, suites/fallback, MAC RSC). |
| `check-map [mapfile]` | Escanea `src/core` y `src/crypto` en busca de `malloc/calloc/realloc/strdup/alloca` y, opcionalmente, un mapa de enlazado. Falla si hay ruta al heap (`MFS-RES-001/002`). |
| `train-dict <corpus> [out] [bytes]` | Entrena un diccionario ODT (bigramas frecuentes) con cabecera `MFOD`. |
| `sign-profile <perfil> <clave-hex64> [out]` | Firma un perfil con BLAKE3-keyed (append de 32 B de MAC). |
| `verify-hwv <hwv.bin> [out.pin]` | Deserializa y valida un HWV de 64 B; opcionalmente lo pinnea para producción. |
| `bench` | Banco reducido sobre vFlash (W1 append, WAF, violaciones NOR). |

### Certificado RSC (§7.1)

`plan` emite un certificado que incluye:

- Peor caso de RAM por modo (sumas exactas §23.2) con `static_assert` de build.
- WCET por clase DAIO (τ RT-A/B/C), cotas de autómatas (DAB/CUSUM/EDP).
- Cotas globales §17.2 (montaje ≤ 12 ms, p99.9 RT-A ≤ 2.5 ms, corrupción 0).
- Suite cripto y perfil de fallback.
- MAC de build (BLAKE3-keyed).

> Un release sin certificado RSC firmado **no debe distribuirse** (§26).

## Integración en target

1. `make plan` → incluir `matrixfs_resources.h` y verificar `static_assert`.
2. Implementar `mfs_port.h` (secciones críticas, ciclos, WFI) en el BSP.
3. Registrar el driver L2 y la geometría en `mfs_config`.
4. Compilar sin `sim/` y con `-DMFS_DEBUG` en CI para los asserts de fase RSC.
