# Prueba de integración del componente ESP-IDF de MatrixFS en host

Verificación de que las fuentes **reales** del componente (`matrixfs_esp.c`,
`matrixfs_esp_vfs.c`) compilan, enlazan y se ejecutan contra el **núcleo real**
del repositorio usando *shims* mínimos de ESP-IDF. No se modificó ningún fichero
del componente, del núcleo ni del sistema de build.

## Comando exacto que funciona

```powershell
$gcc = "C:\ProgramData\mingw64\mingw64\bin\gcc.exe"
$core = Get-ChildItem src/core/*.c, src/crypto/*.c, src/ftl/*.c, src/tier/*.c, `
        src/sec/*.c, src/xio/*.c |
        Where-Object { $_.Name -ne 'mfs_port_arch.c' -and $_.Name -ne 'mfs_port_rtos.c' } |
        ForEach-Object { $_.FullName }
& $gcc -std=c11 -Wall -Wextra -O2 -g `
  -include platform/esp-idf/test/shims/esp_vfs.h `
  -Iplatform/esp-idf/test/shims -Iplatform/esp-idf -Iplatform/esp-idf/include `
  -Iinclude -Isrc -Iplatform/embedded -Itests `
  -o mfs_esp_test.exe `
  platform/esp-idf/test/test_esp_idf.c `
  platform/esp-idf/test/esp_partition_fake.c `
  platform/esp-idf/test/shims/idf_shim.c `
  platform/esp-idf/matrixfs_esp.c `
  platform/esp-idf/matrixfs_esp_vfs.c `
  platform/embedded/mfs_embedded.c `
  @core
```

Dos detalles del comando que **no** son opcionales:

* `-include platform/esp-idf/test/shims/esp_vfs.h` — `matrixfs_esp_vfs.c` incluye
  `<dirent.h>` **antes** de `"esp_vfs.h"`, y el `struct dirent` de MinGW no
  tiene `d_type` (newlib sí). El shim necesita entrar primero para reclamar la
  guarda `_DIRENT_H_` y aportar la versión con `d_type`. Sin `-include` la
  compilación falla con *redefinition of 'struct dirent'*. Es un límite de
  MinGW, no del componente.
* La lista de fuentes del núcleo excluye `mfs_port_arch.c` y `mfs_port_rtos.c`,
  igual que hace `platform/esp-idf/CMakeLists.txt` (el puerto §20.2 lo aporta
  `matrixfs_esp.c`; incluirlos daría símbolos `mfs_port_*` duplicados).

`-Werror` también compila limpio: no hay ningún aviso procedente ni del
componente ni de los ficheros de prueba.

## Salida final

```
== 0. VFS sin volumen montado
== 1. formateo + montaje
W (451181332) matrixfs_esp: particion 'matrixfs' de 1048576 B: solo se aprovecharan 524288 B (MFS_ZONE_MAX=128 zonas de 4096 B). Sube MATRIXFS_ZONE_MAX si necesitas mas capacidad.
   ultimo error: 'particion 'etiqueta_inexistente' no encontrada'
-- formateo + montaje ok
== 2. diagnostico de capacidad
   capacidad=524288 B  particion=1048576 B  MFS_ZONE_MAX=128
-- diagnostico de capacidad ok
== 3. ida y vuelta sin cifrado (offset no alineado)
   [core] ruta 'sin_cifrado.bin': 700 B en offset 3 (offset%16=3, offset%4=3)
-- ida y vuelta sin cifrado ok
== 4. cifrado de flash activo (regresion del alineamiento a 16 B)
W (451181332) matrixfs_esp: particion 'matrixfs' de 1048576 B: solo se aprovecharan 524288 B (MFS_ZONE_MAX=128 zonas de 4096 B). Sube MATRIXFS_ZONE_MAX si necesitas mas capacidad.
   [core] ruta 'cifrado.bin': 700 B en offset 3 (offset%16=3, offset%4=3)
   escrituras=24  granularidad_min=16  rechazos_alineacion=0
-- cifrado de flash activo ok
== 5. VFS POSIX
I (451181332) matrixfs_vfs: MatrixFS registrado en VFS en '/matrixfs'
   readdir: 3 entradas (hola.txt=1, dir=1)
-- VFS POSIX ok
== 6. recursividad de lock/unlock
   recursion tras 2 lock: 2
   recursion tras 2 unlock: 0
-- recursividad de lock/unlock ok
== 7. formas de ruta con y sin barra inicial
   ino '/hola.txt'=5  ino 'hola.txt'=5
-- formas de ruta ok
== 8. desregistro del VFS
-- desregistro del VFS ok
checks: 77 fallos: 0
```

## Ficheros creados

| Fichero | Papel |
|---|---|
| `shims/sdkconfig.h` | `CONFIG_*` del componente (todos sobreescribibles con `-D`); `CONFIG_MATRIXFS_ENABLE_CRYPTO` sin definir a propósito |
| `shims/esp_err.h` | `esp_err_t` + códigos + `esp_err_to_name` |
| `shims/esp_attr.h` | `WORD_ALIGNED_ATTR` |
| `shims/esp_cpu.h` | `esp_cpu_get_cycle_count` (reloj monótono, base 100 MHz) |
| `shims/esp_timer.h` | `esp_timer_get_time` (µs desde `CLOCK_MONOTONIC`) |
| `shims/esp_flash.h` | `esp_flash_encryption_enabled` + `shim_flash_set_encryption` |
| `shims/esp_log.h` | `ESP_LOGE/W/I/D` a stdout con tag |
| `shims/esp_partition.h` | `esp_partition_t` + API usada por el componente |
| `shims/esp_vfs.h` | `esp_vfs_t` con campos `*_p` (IDF v5), `esp_vfs_register_fd_range`; adaptación de `struct dirent`/`uid_t`/`gid_t` a MinGW |
| `shims/freertos/{FreeRTOS,semphr,task}.h` | FreeRTOS mínimo |
| `shims/idf_shim.h` | superficie del shim + ayudantes `shim_vfs_*` + contabilidad de la partición |
| `shims/idf_shim.c` | implementación: reloj, errores, cifrado, FreeRTOS, registro VFS y **despacho POSIX** (recorta el punto de montaje) |
| `esp_partition_fake.c` | partición NOR fiel: 1 MiB en **un único array `static`** (256×4096), semántica `old & new`, alineación de 16 B con cifrado, borrado por sectores, contabilidad |
| `test_esp_idf.c` | suite (77 comprobaciones) |
| `REPORT_esp_idf_host.md` | este informe |

## Cobertura

1. **formateo + montaje** `MFS_OK`, etiqueta inexistente → `MFS_ENOENT` con
   `matrixfs_esp_last_error()` no vacío, `fs == NULL` → `MFS_EINVAL`.
2. **capacidad**: `524288 == MFS_ZONE_MAX * 4096` y
   `matrixfs_esp_capacity_wasteful() == true` (fórmula de `esp_prepare`,
   `matrixfs_esp.c:384-387`).
3. **ida y vuelta sin cifrado**: 700 B en offset 3 (no múltiplo de 16 ni de 4),
   `mf_write` + `mf_sync` + `mf_read` + `memcmp`.
4. **cifrado de flash activo**: `shim_partition_reset()` +
   `shim_flash_set_encryption(true)` + formateo + montaje + ida y vuelta; se
   comprueba que las 24 ventanas programadas son múltiplos de 16 B y que hay 0
   rechazos. Control negativo: `esp_partition_write` con 4 B en offset 516 y con
   12 B en offset 512 devuelven `ESP_ERR_INVALID_ARG` (y una de 16 B se acepta).
5. **VFS**: registro `MFS_OK`, segundo registro `MFS_EBUSY`, y por el despacho:
   create + write + close + reopen + read + `fstat` + `stat` + `mkdir` +
   `opendir`/`readdir` + `rename` + `unlink`; `errno == ENOENT` al abrir y al
   hacer `stat` de una ruta inexistente; desregistro `MFS_OK` y segundo
   desregistro `MFS_ENOTMOUNTED`. También se comprueba que el recorte del punto
   de montaje no es un `strncmp` ingenuo (`/matrixfsX/...` **no** se considera
   montado).
6. **recursividad del mutex**: dos `lock` → profundidad 2, dos `unlock` → 0.
7. **formas de ruta**: `"/hola.txt"` y `"hola.txt"` dan el mismo `ino` (5).

## Incidencias reales encontradas en el componente / capa compartida

**Ninguna.** El componente compila sin avisos (`-Wall -Wextra -Werror`), enlaza
y se comporta como documenta su cabecera.

Dos observaciones que **no** son defectos, pero conviene conocer:

* `matrixfs_esp_vfs.c:33` incluye `"esp_vfs.h"` después de `<dirent.h>`, y ese
  fichero confía en que `matrixfs_esp_lock/unlock` estén declarados por
  inclusión previa (`matrixfs_esp_vfs.c:129-135` usa el macro
  `MFS_VFS_WITH_LOCK`, que llama a `matrixfs_esp_lock()` sin incluir
  `matrixfs_esp.h`). En el build de IDF funciona porque `esp_vfs.h` /
  `matrixfs_esp_private.h` acaban aportando esa declaración, pero es una
  dependencia de orden de inclusión: si se reordena, aparece
  `implicit declaration of function 'matrixfs_esp_lock'`. Es fragilidad de
  estilo, no un fallo funcional.
* `matrixfs_esp_vfs.c:498-499` usa `slot->de.d_type` y `DT_DIR`/`DT_REG`, que
  newlib (target) sí tiene pero MinGW no. El shim lo resuelve; en el target no
  hay nada que arreglar.

## ¿Habría fallado la prueba de cifrado con alineamiento de 4 B en vez de 16?

**Con el código actual: no, y he comprobado empíricamente que la prueba pasa
igual. La respuesta honesta es que esa aserción NO discrimina hoy.** Detalle:

* Cumplí la petición de comprobar la hipótesis: parcheé una **copia temporal**
  de `matrixfs_esp.c` (nunca el original) cambiando
  `MFS_ESP_PGM_GRAN_ENCRYPTED 16u` → `4u` y recompilé la suite. Resultado:
  `checks: 77 fallos: 0`, es decir, **la prueba pasa también con 4 B**.
* Instrumenté `esp_partition_write` y la causa es clara: en esta ruta el núcleo
  **solo programa páginas completas de 256 B en offsets múltiplos de 256**. Las
  24 escrituras observadas son `off ∈ {0, 256, 512, …, 20544}` con
  `size ∈ {32, 64, 256}`; con `g = 4` la ventana del read-modify-write queda
  `[off & ~3, align_up(off+len,4))`, que al partir de un `off` ya alineado a 256
  sigue siendo múltiplo de 16. El caso que rompería (`g = 4` con `off % 16 != 0`)
  no llega a producirse porque la capa de diario/zonas del núcleo agrupa la
  escritura de 700 B en páginas alineadas.
* Lo que **sí** demuestra la suite, y es verificable:
  1. el modelo de partición **rechaza de verdad** la programación desalineada
     (control negativo: 4 B en offset 516 y 12 B en offset 512 →
     `ESP_ERR_INVALID_ARG`, contador de rechazos +2);
  2. el componente **hoy emite exclusivamente ventanas múltiplo de 16 B**
     (`granularidad_min == 16`, `rechazos_alineacion == 0`), que es la garantía
     que el cifrado exige;
  3. `MFS_ESP_PGM_GRAN_ENCRYPTED` se selecciona con
     `esp_flash_encryption_enabled()` en `esp_prepare` (`matrixfs_esp.c:353-354`).
* Análisis del código para el caso que no se alcanza: si el núcleo emitiera, por
  ejemplo, `off = 3, len = 4`, con `g = 16` la ventana es `[0, 16)` (válida),
  pero con `g = 4` sería `[0, 8)`: `8 % 16 != 0` → `ESP_ERR_INVALID_ARG` →
  `esp_flash_prog` devuelve `MFS_EIO` → el `mf_write`/`mf_sync` falla. El test
  lo detectaría **si** la escritura del núcleo llegase desalineada; hoy no llega.
  La aserción queda como documentación de la propiedad actual, y por eso añadí el
  control negativo que sí falla con 4 B.

## Lo que no he podido verificar

* **No hay ESP-IDF real**, así que no se validó contra las cabeceras auténticas
  de IDF v5: el `esp_vfs_t` del shim se escribió a mano a partir del uso que
  hace `matrixfs_esp_vfs.c` (campos `*_p`, `ESP_VFS_FLAG_DEFAULT`) y de la
  documentación del componente. Si IDF cambiase `write_p` a `size_t`, el shim
  no lo notaría (se corrigió a `ssize_t`, que es lo que usa IDF v5).
* El shim **sí** valida el contrato importante: `esp_vfs_register_fd_range`
  rechaza un `esp_vfs_t` cuyo `flags != ESP_VFS_FLAG_DEFAULT`, así que una
  confusión de variante (con/sin contexto) se detectaría.
* No se probó `CONFIG_MATRIXFS_THREAD_SAFE=0` (la ruta sin mutex) ni
  `CONFIG_MATRIXFS_ENABLE_CRYPTO=1` (clave del núcleo) ni los
  `CONFIG_MATRIXFS_FORCE_MODE_*`; el shim los soporta
  (`-DCONFIG_MATRIXFS_ENABLE_CRYPTO=1` define la clave) pero no los ejercité.
* El shim de FreeRTOS es de un solo hilo: no reproduce concurrencia real ni
  contextos de ISR, sólo la semántica de recursividad del mutex.
* `matrixfs_esp_mount_default()` no se ejecutó (sólo se usa desde el test a
  través de las otras entradas); queda sin cubrir `esp_kconfig_mode()`.
* El modelo NOR no simula desgaste, bits atascados ni pérdida de energía; la
  fidelidad se limita a límites, semántica `old & new`, alineación de cifrado y
  granularidad de borrado.
