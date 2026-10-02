# MatrixFS «ATLAS» — componente externo de ESP-IDF

Integración de **MatrixFS** (sistema de archivos embebido en C11, sin heap) como
**componente externo de ESP-IDF**. El componente enlaza el núcleo del proyecto
con el SDK de Espressif (ESP32, ESP32-S, ESP32-C, ESP32-H) apoyándose en la API de
particiones `esp_partition`.

El componente **no reimplementa** el sistema de archivos: compila las fuentes
del núcleo (`src/**`) y la capa embebida compartida
(`platform/embedded/mfs_embedded.c`), y solo aporta:

1. Las **primitivas obligatorias del puerto** (`mfs_port_*`, §20.2), que en un
   build de host aporta `sim/mfs_port_host.c` y en destino debe aportar el
   integrador. Aquí las aporta `matrixfs_esp.c`.
2. Los **callbacks de medio** (`read`/`prog`/`erase`) sobre `esp_partition`.
3. La **API pública** de montaje/formateo, la lectura de la configuración de
   `Kconfig` y, opcionalmente, el **registro POSIX VFS**.

> **Aviso de alcance.** El núcleo de MatrixFS es de **instancia única** (tablas
> estáticas, sin heap) y el componente aloja el descriptor de flash en estado
> estático. Por tanto, **solo puede haber un volumen MatrixFS montado a la vez**.
>
> **El núcleo no es reentrante.** Ver §6 antes de usarlo desde varias tareas.

---

## Índice

- [1. Contenido](#1-contenido)
- [2. Cómo añadirlo como componente](#2-cómo-añadirlo-como-componente)
- [3. Tabla de particiones](#3-tabla-de-particiones)
- [4. Configuración por Kconfig](#4-configuración-por-kconfig)
- [5. Uso](#5-uso)
- [6. Concurrencia](#6-concurrencia)
- [7. Integración POSIX (VFS)](#7-integración-posix-vfs)
- [8. Presupuesto de RAM y capacidad útil](#8-presupuesto-de-ram-y-capacidad-útil)
- [9. Alineación y cifrado de flash](#9-alineación-y-cifrado-de-flash)
- [10. Limitaciones](#10-limitaciones)
- [11. Estado de verificación](#11-estado-de-verificación)

---

## 1. Contenido

| Fichero | Descripción |
|---|---|
| `CMakeLists.txt` | Registro del componente: compila el núcleo como librería estática enlazada, más `matrixfs_esp.c` y (opcional) `matrixfs_esp_vfs.c`. |
| `idf_component.yml` | Manifest para el Component Registry (`idf: ">=5.0"`, Apache-2.0). |
| `Kconfig` | Opciones de `menuconfig` bajo el menú «MatrixFS». |
| `include/matrixfs_esp.h` | API pública del componente. |
| `include/matrixfs_esp_vfs.h` | API del registro POSIX (solo con `CONFIG_MATRIXFS_VFS_ENABLE`). |
| `matrixfs_esp.c` | Implementación: puerto, callbacks de flash, montaje y serialización. |
| `matrixfs_esp_vfs.c` | Registro en el VFS de ESP-IDF (opcional). |
| `matrixfs_esp_private.h` | Superficie interna compartida entre esas dos unidades. |
| `test/` | *Shims* de ESP-IDF y tests que **ejecutan** el componente en host. |

### Sobre las fuentes del núcleo

El `CMakeLists.txt` descubre las fuentes del núcleo con un **glob**
(`CONFIGURE_DEPENDS`) y **excluye** `src/core/mfs_port_arch.c` y
`src/core/mfs_port_rtos.c`, porque los envoltorios del contrato §20.2 los aporta
`matrixfs_esp.c` y definirlos dos veces produciría símbolos duplicados.

> Esto no es un detalle de estilo. Una lista de fuentes escrita a mano en este
> componente **omitió `src/core/mfs_arch.c`**, que define `mfs_arch_detect()` y
> `mfs_arch_crc32c_hw_available()`: ambas se invocan de forma **incondicional**
> desde `mfs_hal.c` (montaje) y `mfs_util.c` (todo CRC), de modo que el
> componente **no enlazaba**. Con el glob, un fichero nuevo del núcleo entra solo
> y el defecto no puede repetirse. Análisis completo en
> [`../compatibility-analysis.md`](../compatibility-analysis.md).

---

## 2. Cómo añadirlo como componente

Este directorio (`platform/esp-idf/`) está pensado para colocarse como
`components/matrixfs/` dentro de un proyecto ESP-IDF **manteniendo la ruta
relativa al árbol del repositorio**, porque compila `src/**` y
`platform/embedded/`. El `CMakeLists.txt` resuelve la raíz y **falla con un
mensaje accionable** si no la encuentra.

### Opción A — dependencia por ruta (recomendada)

```yaml
# main/idf_component.yml
dependencies:
  matrixfs_ultra:
    path: third_party/MatrixFS-ULTRA/platform/esp-idf
```

Con esta forma el árbol del repositorio se mantiene intacto y las rutas
relativas resuelven sin copiar nada.

### Opción B — carpeta de componente en el proyecto

```
mi_proyecto/
  CMakeLists.txt
  main/
  components/
    matrixfs -> ../../third_party/MatrixFS-ULTRA/platform/esp-idf   (enlace)
  third_party/
    MatrixFS-ULTRA/     <- árbol completo (src/, include/, platform/)
```

> Si `components/matrixfs/` es una **copia** y no un enlace, `../..` ya no
> apunta al repositorio y la configuración falla con un error explícito. Copia o
> enlaza el árbol **completo**, no solo `platform/esp-idf/`.

**Nota sobre rutas de fuentes.** ESP-IDF **no** emite error ni aviso por listar
fuentes fuera del directorio del componente (verificado en
`tools/cmake/component.cmake` de v5.3 y master): `__component_add_sources`
resuelve cada ruta relativa al directorio del componente y la pasa a
`add_library`, sin comprobar su ubicación. Lo que sí hace IDF es **abortar** con
`FATAL_ERROR "Include directory ... is not a directory."` si un directorio de
`INCLUDE_DIRS` no existe. Por eso este `CMakeLists.txt` valida primero el árbol
del repositorio y falla con un mensaje accionable, y por eso el núcleo se compila
como **librería estática propia** enlazada al componente: así el componente sigue
funcionando aunque se reubique o se instale desde el registro.

### Opción C — registro de componentes

```yaml
dependencies:
  lauotero/matrixfs_ultra: "~1.0.0"
```

### Dependencias que declara

`esp_partition`, `esp_flash` (`esp_flash_encryption_enabled`), `esp_timer`,
`esp_hw_support` (`esp_cpu.h`), `freertos`, `log` y `vfs`. Todas como
`PRIV_REQUIRES`: la cabecera pública **no** expone tipos de IDF, así que los
consumidores no heredan sus *include dirs*.

---

## 3. Tabla de particiones

Añade una partición de tipo `data` para el volumen. El componente la localiza con
`ESP_PARTITION_TYPE_DATA` y `ESP_PARTITION_SUBTYPE_ANY`, de modo que sirve
cualquier subtipo (incluido uno propio ≥ `0x40`) y también particiones de flash
externo registradas con `esp_partition_register_external()`.

`partitions.csv`:

```csv
# name,   type, subtype, offset,  size,     flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
matrixfs, data, 0x40,    0x12000, 0x80000,
```

- Etiqueta (`label`) `matrixfs` — debe coincidir con
  `CONFIG_MATRIXFS_PARTITION_LABEL`.
- `size` `0x80000` = 512 KiB. Ver la nota de capacidad en §8.

Para un subtipo propio lo **documentado** por Espressif es declararlo con el
property CMake `EXTRA_PARTITION_SUBTYPES` (`<tipo>,<nombre>,<valor>`), que genera
la constante `ESP_PARTITION_SUBTYPE_<tipo>_<nombre>`:

```cmake
idf_component_register(...
    REQUIRES esp_partition)
# en el CMakeLists del proyecto:
set(EXTRA_PARTITION_SUBTYPES "data,matrixfs,0x40")
```

El subtipo puramente numérico del CSV (`data, 0x40`) funciona —la búsqueda es una
comparación numérica sin validación en tiempo de ejecución— pero no está
documentado; compruébalo con `idf.py partition-table`.

**Flash externo.** También sirve una partición registrada con
`esp_partition_register_external()`: el componente la localiza igual, porque
busca con `ESP_PARTITION_TYPE_DATA` y `ESP_PARTITION_SUBTYPE_ANY`, y el puntero
devuelto por `esp_partition_find_first()` es válido durante toda la vida de la
aplicación.

> **Trampa de versión (verificada).** En **ESP-IDF v5.3**
> `esp_partition_register_external()` **no inicializa `erase_size`**: el struct se
> reserva con `calloc` y el campo queda a **0**. El componente lo trata como
> 4096 B, pero si tu código calcula alineaciones con `part->erase_size`
> directamente obtendrás una división por cero. En v5.5 ya se fija a 4096.
> Además, en v5.3 esa función **no acepta `flash_chip == NULL`** (desreferencia
> el puntero); en v5.5 `NULL` significa el chip de flash por defecto.

---

## 4. Configuración por Kconfig

Menú `menuconfig` → **MatrixFS**:

| Símbolo | Tipo | Defecto | Descripción |
|---|---|---|---|
| `MATRIXFS_PARTITION_LABEL` | string | `"matrixfs"` | Etiqueta de la partición de datos. |
| `MATRIXFS_RAM_BUDGET` | int | `32768` | RAM **declarada** al planificador de viabilidad. **No reserva memoria** (§8). |
| `MATRIXFS_FORMAT_IF_NEEDED` | bool | `n` | Formatea y reintenta si el volumen no es válido. |
| `MATRIXFS_THREAD_SAFE` | bool | `y` | Serializa la superficie pública con un mutex recursivo (§6). |
| `MATRIXFS_FORCE_MODE` | choice | Automático | Fuerza el modo operativo o lo negocia. |
| `MATRIXFS_ENABLE_CRYPTO` | bool | `n` | Aporta una clave de 32 bytes al núcleo. |
| `MATRIXFS_CRYPTO_KEY_HEX` | string | `""` | Clave en hexadecimal (64 dígitos). **Queda en claro en `sdkconfig`**; para producción cárgala de eFuse/NVS. |
| `MATRIXFS_L2P_SLOTS` | int | `4096` | Entradas del mapa L2P (8 B cada una). Fija la capacidad mapeable. |
| `MATRIXFS_ZONE_MAX` | int | `128` | Zonas en RAM. Fija la capacidad útil. |
| `MATRIXFS_VFS_ENABLE` | bool | `n` | Registra el volumen en el VFS (§7). |
| `MATRIXFS_VFS_MOUNT_POINT` | string | `"/matrixfs"` | Punto de montaje POSIX. |
| `MATRIXFS_VFS_MAX_FILES` | int | `8` | Descriptores simultáneos. |
| `MATRIXFS_PAGE_SIZE`, `MATRIXFS_T_*_MAX_US` | int | 256 / 700 / 45000 / 100 | Geometría y presupuestos temporales. |

---

## 5. Uso

```c
#include "matrixfs_esp.h"
#include "mfs_internal.h"          /* mf_t es OPACO en include/matrixfs */

static mf_t s_fs;                  /* debe sobrevivir mientras esté montado */

void app_main(void) {
    /* Monta usando la configuración de Kconfig. */
    mfs_st st = matrixfs_esp_mount_default(&s_fs);
    if (st != MFS_OK) {
        printf("matrixfs: montaje fallido: %s\n", matrixfs_esp_last_error());
        return;
    }

    /* Aviso de capacidad: si la partición es mayor que el volumen útil. */
    printf("matrixfs: capacidad util %u B\n", matrixfs_esp_capacity_bytes());

    /* Escribe un fichero. */
    mfs_file *f = NULL;
    st = (mfs_st)mf_open(&s_fs, "/hola.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
    if (st == MFS_OK) {
        size_t wr = 0;
        mf_write(f, "Hola, MatrixFS", 14, &wr);
        mf_close(f);
    }

    /* Lo vuelve a leer. */
    char buf[32] = {0};
    st = (mfs_st)mf_open(&s_fs, "/hola.txt", MFS_O_RDONLY, &f);
    if (st == MFS_OK) {
        size_t rd = 0;
        mf_read(f, buf, sizeof(buf) - 1u, &rd);
        mf_close(f);
        printf("matrixfs: leido \"%s\"\n", buf);
    }

    mf_sync(&s_fs);
    mf_deinit(&s_fs);
}
```

Para pasar opciones explícitas (sin depender de Kconfig), usa
`matrixfs_esp_mount(label, &s_fs, &opts)` con `mfs_embedded_opts` rellenado por
`mfs_embedded_opts_default()`.

### API pública

```c
mfs_st matrixfs_esp_mount(const char *partition_label, mf_t *out_fs,
                          const mfs_embedded_opts *opts);
mfs_st matrixfs_esp_format(const char *partition_label, mf_t *fs,
                           const mfs_embedded_opts *opts);
mfs_st matrixfs_esp_mount_default(mf_t *out_fs);
const char  *matrixfs_esp_last_error(void);
uint32_t     matrixfs_esp_capacity_bytes(void);
bool         matrixfs_esp_capacity_wasteful(void);
void         matrixfs_esp_lock(void);
void         matrixfs_esp_unlock(void);
```

- `partition_label == NULL` o vacío usa `CONFIG_MATRIXFS_PARTITION_LABEL`.
- `opts == NULL` usa los valores por defecto de `mfs_embedded_opts_default()`
  (sin formateo automático).
- `matrixfs_esp_last_error()` devuelve el último mensaje legible (nunca `NULL`);
  con `CONFIG_MATRIXFS_THREAD_SAFE` el buffer es **por tarea**.

---

## 6. Concurrencia

El núcleo mantiene estado global (mapa L2P, pools, ventana WAL y de inodos) y
**no es reentrante**: dos tareas llamando a `mf_write` a la vez corrompen el
volumen. Una aplicación ESP-IDF es multitarea desde el primer `xTaskCreate`.

Con `CONFIG_MATRIXFS_THREAD_SAFE=y` (por defecto), el componente:

- serializa `matrixfs_esp_mount()`, `matrixfs_esp_format()` y **toda** la capa
  VFS (§7) con un mutex recursivo de FreeRTOS;
- expone `matrixfs_esp_lock()` / `matrixfs_esp_unlock()` para que la aplicación
  agrupe varias llamadas **directas** al núcleo:

```c
matrixfs_esp_lock();
mf_stat(&s_fs, "/cfg.bin", &st);
mf_open(&s_fs, "/cfg.bin", MFS_O_RDONLY, &f);
matrixfs_esp_unlock();
```

Las llamadas directas al núcleo (`mf_open`, `mf_write`…) **no pasan por el
componente**, así que no pueden serializarse solas. Si el volumen se usa desde
más de una tarea y no usas el VFS, envuélvelas con las funciones de arriba.

Las ranuras de `matrixfs_esp_last_error()` son **por tarea**: el componente
mantiene un conjunto acotado de mensajes y, si todas están ocupadas por otras
tareas, desaloja la usada menos recientemente (IDF-10). Así una tarea viva nunca
se queda sin mensaje aunque otras hayan terminado.

En la capa VFS, la validación del descriptor se hace **dentro** del mutex
(IDF-05): de lo contrario, entre la comprobación y la operación otra tarea podría
cerrar el descriptor y reutilizar su ranura, y la operación recaería sobre el
fichero equivocado.

Desactivar `MATRIXFS_THREAD_SAFE` solo es correcto si garantizas el acceso desde
una única tarea.

---

## 7. Integración POSIX (VFS)

Con `CONFIG_MATRIXFS_VFS_ENABLE=y` el volumen se puede usar con la API POSIX de
newlib, igual que SPIFFS o FAT:

```c
#include "matrixfs_esp_vfs.h"

matrixfs_esp_mount_default(&s_fs);
matrixfs_esp_vfs_register();            /* registra CONFIG_MATRIXFS_VFS_MOUNT_POINT */

FILE *f = fopen("/matrixfs/hola.txt", "w");
fputs("hola", f);
fclose(f);

struct stat st;
stat("/matrixfs/hola.txt", &st);

DIR *d = opendir("/matrixfs");
struct dirent *e;
while ((e = readdir(d)) != NULL) printf("%s\n", e->d_name);
closedir(d);

matrixfs_esp_vfs_unregister();
```

Operaciones soportadas: `open`, `close`, `read`, `write`, `lseek`, `fstat`,
`fsync`, `stat`, `unlink`, `rename`, `mkdir`, `opendir`, `readdir`, `closedir`.
`rmdir` devuelve `ENOTSUP`: el núcleo no expone borrado de directorios.

Los errores del núcleo se traducen a `errno`
(`MFS_ENOENT`→`ENOENT`, `MFS_ENOSPC`/`MFS_ETABLEFULL`→`ENOSPC`, `MFS_EROFS`→`EROFS`,
`MFS_EACCES`→`EACCES`, `MFS_ENOTSUP`→`ENOSYS`, resto→`EIO`).

> **Versión de IDF.** `esp_vfs_t` cambió de nombres de campo entre IDF v4
> (`open`/`read`/`write`) y v5 (`open_p`/`read_p`/`write_p` con
> `ESP_VFS_FLAG_DEFAULT`). Esta implementación usa la forma de **v5.x**, que es
> la que exige `idf_component.yml`. Si compilas contra otra versión, el único
> fichero a ajustar es `matrixfs_esp_vfs.c`.

---

## 8. Presupuesto de RAM y capacidad útil

### El consumo se fija en compilación

`MATRIXFS_RAM_BUDGET` **no reserva memoria**: solo alimenta al planificador de
viabilidad que elige el modo operativo. El consumo real lo fijan
`MATRIXFS_L2P_SLOTS`, `MATRIXFS_ZONE_MAX` y las cotas internas del núcleo
(`MFS_SCRATCH_MAX`, `MFS_MAX_FILES_OPEN`, `MFS_WAL_WINDOW_MAX`).

Medido sobre este repositorio (sección `.bss` del núcleo + capa embebida, sin la
aplicación, y `sizeof(mf_t)`):

| Configuración | `.bss` | `mf_t` | Pila de tarea | Total |
|---|---|---|---|---|
| Por defecto (`L2P=4096`, `ZONE_MAX=128`) | ≈198 KB | 17,2 KB | ≥ 16 KB | **≈ 232 KB** |
| `L2P=1024` | ≈162 KB | 17,2 KB | ≥ 16 KB | ≈ 196 KB |
| `L2P=1024`, `ZONE_MAX=64` | ≈159 KB | 14,7 KB | ≥ 16 KB | ≈ 190 KB |

Para ESP32 (520 KB de SRAM) es holgado; para ESP32-C3/H2 (400 KB) también. **La
pila de la tarea que llama al núcleo debe ser ≥ 16 KB**: varias rutas declaran
buffers de `MFS_SCRATCH_MAX` (4096 B) en la pila y pueden anidarse. No uses el
valor por defecto de `xTaskCreate` (≈3,5 KB).

### Capacidad útil

El núcleo direcciona como máximo `MFS_ZONE_MAX` zonas de `erase_unit` bytes tras
los 3 sectores reservados (SB A, SB B y anillo de tokens):

```
capacidad_util ≈ MFS_ZONE_MAX × erase_unit
```

Con `erase_unit = 4096` y las 128 zonas por defecto son **512 KiB**: una
partición mayor **no se aprovecha**. El componente lo detecta y lo reporta —
`matrixfs_esp_capacity_bytes()` da la cifra real,
`matrixfs_esp_capacity_wasteful()` devuelve `true` y se emite un `ESP_LOGW` al
montar. Sube `MATRIXFS_ZONE_MAX` si necesitas más (y asume ≈96 B extra por zona).

También hay un tope por el mapa L2P: `MATRIXFS_L2P_SLOTS × chunk_size` bytes de
páginas lógicas. Con 4096 entradas y chunk de 4096 B son ~16 MiB. Al agotarse se
devuelve `MFS_ETABLEFULL`, traducido a `ENOSPC`.

---

## 9. Alineación y cifrado de flash

`esp_partition_write` exige, en particiones con **cifrado de flash**, que offset y
longitud sean múltiplos del bloque de cifrado (**16 B**). El componente detecta
el estado con `esp_flash_encryption_enabled()` y ajusta su
*read-modify-write* a **16 B** cuando el cifrado está activo y a **4 B** cuando
no. El valor además se declara al núcleo como `program_granularity`.

El RMW reprograma la ventana alineada completa. Es seguro en NOR porque solo se
limpian bits 1→0: reescribir el mismo dato no altera nada, y los bytes del tramo
pedido los gestiona el núcleo, que borra antes de programar.

---

## 10. Limitaciones

- **Una sola instancia.** El núcleo usa tablas estáticas y el componente aloja el
  descriptor de flash en estado estático: no se pueden montar dos volúmenes.
- **Capacidad direccionable.** `MFS_ZONE_MAX × erase_unit` (§8). Una partición
  mayor no se aprovecha completa.
- **Geometría.** Se toma `erase_size` de la partición (típicamente 4096) y
  `page_size` de `CONFIG_MATRIXFS_PAGE_SIZE`. Este último es **declarativo**
  (IDF-07): se publica en el HWV, pero el backend no trocea por él porque
  `esp_partition_write` ya resuelve los cruces de página. Los presupuestos
  `t_*_max_us` son configurables y por defecto los valores NOR del estándar.
- **Secciones críticas.** El puerto usa `portENTER_CRITICAL`, que deshabilita
  interrupciones en el núcleo actual; el núcleo las usa solo en tramos cortos
  (alrededor de las lecturas).
- **`mfs_port_cycles`** (IDF-09) usa `esp_cpu_get_cycle_count()` en partes de un
  solo núcleo (contador global) y en partes **duales** (ESP32, ESP32-S3) deriva
  una base común de `esp_timer_get_time()` escalada por la frecuencia de CPU,
  porque el CCOUNT del hardware es **por núcleo** e incomparable entre tareas.
  `mfs_port_time_us` usa `esp_timer_get_time()`, que ya es global.
- **No ISR-safe.** `portENTER_CRITICAL` es de contexto de tarea. La API asíncrona
  del núcleo (`mf_submit`/`mf_poll`) no debe ejercerse desde una ISR a través de
  este puerto.
- **Una sola definición de `mfs_port_*`.** Si la aplicación ya incorpora otro
  `mfs_port_*.c` (por ejemplo `sim/mfs_port_host.c`), no enlaces ambos.

---

## 11. Estado de verificación

**Verificado en host** (sin ESP-IDF instalado, con *shims* de las cabeceras del
SDK en `test/`): el componente **compila y enlaza** contra el núcleo real;
formatea, monta, escribe y relee a través de `esp_partition` simulado, incluido
el caso de **partición cifrada** (alineación de 16 B); el registro VFS funciona de
extremo a extremo (crear, escribir, leer, `stat`, `mkdir`, `readdir`, `unlink`,
traducción de `errno`); y el mutex recursivo se comporta como tal. Ver
`test/` y el apartado «Verificación sin los SDK» de
[`../compatibility-analysis.md`](../compatibility-analysis.md).

**No verificado (requiere SDK y hardware):**

- `idf.py build` con una instalación real de ESP-IDF 5.x, y ejecución en ESP32
  real o en QEMU. El *shim* no reproduce el planificador de IDF, la tabla de
  particiones real ni el motor de cifrado de flash.
- Las firmas exactas de `esp_vfs_t` en cada versión menor de IDF 5.x (ver §7).
- Comportamiento de `esp_partition_write` sobre particiones cifradas en silicio.

**Verificado por inspección estática:** coherencia con las firmas reales del
núcleo (`mf_init`, `mf_format`, `mf_open`, `mf_read`, `mf_write`, `mf_close`,
`mf_deinit`, `mf_sync`, `mf_stat`, `mf_mkdir`, `mf_unlink`, `mf_rename`,
`mf_opendir`, `mf_readdir`, `mf_closedir`, `mfs_ststr`) y con la capa embebida
(`mfs_embedded_opts_default`, `mfs_embedded_mount`, `mfs_embedded_format`).
