# MatrixFS Ultra «ATLAS» — componente externo de ESP-IDF

Integración de **MatrixFS Ultra** (sistema de archivos embebido en C11, sin heap)
como **componente externo de ESP-IDF**. El componente enlaza el núcleo del
proyecto con el SDK de Espressif (ESP32 / ESP32-S, ESP32-C, ESP32-H) apoyándose
en la API de particiones `esp_partition`.

El componente **no reimplementa** el sistema de archivos: compila las fuentes
del núcleo (`src/**`) y la capa embebida compartida
(`platform/embedded/mfs_embedded.c`), y sólo aporta:

1. Las **primitivas obligatorias del puerto** (`mfs_port_*`), que en un build de
   host aporta `sim/mfs_port_host.c` y que en destino debe aportar el
   integrador. En esta integración las aporta `matrixfs_esp.c`.
2. Los **callbacks de medio** (`read`/`prog`/`erase`) sobre `esp_partition`.
3. La **API pública** de montaje/formateo y la lectura de la configuración de
   Kconfig.

> Aviso de alcance: el núcleo de MatrixFS es de **instancia única** (tablas
> estáticas, sin heap) y el componente aloja el descriptor de flash en estado
> estático. Por tanto, **sólo puede haber un volumen MatrixFS montado a la vez**.

---

## 1. Contenido

| Fichero | Descripción |
|---|---|
| `CMakeLists.txt` | Registro del componente (`idf_component_register`): lista todas las fuentes del núcleo, la capa embebida y `matrixfs_esp.c`; expone los include dirs. |
| `idf_component.yml` | Manifest para el Component Registry (`idf: ">=5.0"`, licencia Apache-2.0). |
| `Kconfig` | Opciones de `menuconfig` bajo el menú «MatrixFS Ultra». |
| `include/matrixfs_esp.h` | API pública del componente. |
| `matrixfs_esp.c` | Implementación: puerto, callbacks de flash y montaje. |

---

## 2. Cómo añadirlo como componente externo

Este directorio (`platform/esp-idf/`) está pensado para colocarse como
`components/matrixfs/` dentro de un proyecto ESP-IDF, **manteniendo la ruta
relativa al árbol del repositorio** (el `CMakeLists.txt` referencia las fuentes
del núcleo con `../../src/...` y `../../platform/embedded/`).

### Opción A — carpeta de componente en el proyecto

1. Sitúa el componente de modo que `components/matrixfs/CMakeLists.txt` pueda
   resolver `../../src/` y `../../platform/embedded/`. La forma más sencilla es
   copiar (o enlazar) el repositorio completo y añadir el subdirectorio como
   componente:

   ```
   mi_proyecto/
     CMakeLists.txt
     main/
     components/
       matrixfs/            <- copia o enlace de platform/esp-idf/
     third_party/
       MatrixFS-ULTRA/      <- árbol del repositorio (src/, include/, platform/)
   ```

   Si `components/matrixfs/` no es un enlace directo a `platform/esp-idf/`,
   ajusta las rutas relativas del `CMakeLists.txt` (o define una variable con la
   raíz del repositorio) para que apunten a `src/`, `include/` y
   `platform/embedded/`.

2. Añade el componente a `EXTRA_COMPONENT_DIRS` (si no está ya bajo
   `components/`) o confírmalo con `idf.py reconfigure`.

3. Habilita la opción del menú `MatrixFS Ultra` con `idf.py menuconfig`.

### Opción B — dependencia gestionada (`idf_component.yml`)

Añade el componente como dependencia del proyecto mediante un
`main/idf_component.yml`:

```yaml
dependencies:
  matrixfs_ultra:
    path: third_party/MatrixFS-ULTRA/platform/esp-idf
```

o, si se publica en el Component Registry, por su nombre y versión:

```yaml
dependencies:
  lauotero/matrixfs_ultra: "~1.0.0"
```

`idf.py` descargará/resolverá el componente y aplicará su `Kconfig` y su
`CMakeLists.txt`. Las dependencias que declara el componente son
`esp_partition` y `esp_timer` (`freertos`, `esp_hw_support` y `log` son
componentes comunes de IDF, siempre disponibles).

> Nota sobre rutas de fuentes. Como el componente compila ficheros que están
> fuera de su directorio (`../../src/...`), algunas versiones de ESP-IDF pueden
> emitir un aviso del tipo «source file ... outside of component directory». Es
> un aviso, no un error. Si se prefiere evitarlo, sustituye las rutas por una
> variable con la raíz del repositorio calculada con
> `get_filename_component(MATRIXFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)`
> y pasa `${MATRIXFS_ROOT}/src/...` a `idf_component_register`.

### Primitivas de puerto

`matrixfs_esp.c` implementa las cinco funciones obligatorias del contrato de
puerto:

| Función | Implementación ESP-IDF |
|---|---|
| `mfs_port_crit_enter` / `mfs_port_crit_exit` | `portENTER_CRITICAL` / `portEXIT_CRITICAL` sobre un `portMUX_TYPE` estático |
| `mfs_port_time_us` | `esp_timer_get_time()` (µs desde el arranque, truncado a 32 bits) |
| `mfs_port_cycles` | `esp_cpu_get_cycle_count()` |
| `mfs_port_wfi` | `taskYIELD()` (idle cooperativo, sin busy-wait) |

**Debe haber una sola definición de estas funciones en toda la imagen.** Si la
aplicación ya incorpora otro `mfs_port_*.c` (por ejemplo `sim/mfs_port_host.c`),
no enlaces ambos: en destino usa únicamente las de este componente.

---

## 3. Tabla de particiones de ejemplo

Añade una partición de tipo `data` para el volumen. Como el componente localiza
la partición con `ESP_PARTITION_TYPE_DATA` y `ESP_PARTITION_SUBTYPE_ANY`, sirve
cualquier subtipo (incluido uno propio ≥ `0x40`).

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
- `subtype` `0x40` es un subtipo de datos personalizado; también funciona
  `spiffs`, `fat`, etc., porque la búsqueda usa «cualquier subtipo».
- `size` `0x80000` = 512 KiB. Ver la nota de capacidad en la sección 7.

---

## 4. Configuración por Kconfig

Menú `menuconfig` → **MatrixFS Ultra**:

| Símbolo | Tipo | Defecto | Descripción |
|---|---|---|---|
| `MATRIXFS_PARTITION_LABEL` | string | `"matrixfs"` | Etiqueta de la partición de datos. |
| `MATRIXFS_RAM_BUDGET` | int | `32768` | RAM declarada al planificador de viabilidad (bytes); un valor menor puede degradar el modo. |
| `MATRIXFS_FORMAT_IF_NEEDED` | bool | `n` | Formatea y reintenta si el volumen no es válido. |
| `MATRIXFS_FORCE_MODE` | choice | Automático | Fuerza el modo (Ultra-Nano, Nano, Compact, Balanced, Extended) o lo negocia. |
| `MATRIXFS_ENABLE_CRYPTO` | bool | `n` | Aporta una clave de 32 bytes al núcleo. |
| `MATRIXFS_CRYPTO_KEY_HEX` | string | `""` | Clave en hexadecimal (64 dígitos). **Queda en texto claro en `sdkconfig`**; para producción, cárgala desde eFuse/NVS en tiempo de ejecución. |

---

## 5. Ejemplo de uso mínimo

La instancia del núcleo (`mf_t`) es **opaca** en `include/matrixfs`: para
declararla hay que incluir `mfs_internal.h` (directorio `src/`), igual que hace
`platform/common/mfs_vfs.c`. El componente expone `src/` entre sus include dirs.

```c
#include "matrixfs_esp.h"
#include "mfs_internal.h"          /* para declarar la instancia mf_t */

static mf_t s_fs;                  /* debe sobrevivir mientras esté montado */

void app_main(void) {
    /* Monta usando la configuración de Kconfig. */
    mfs_st st = matrixfs_esp_mount_default(&s_fs);
    if (st != MFS_OK) {
        printf("matrixfs: montaje fallido: %s\n", matrixfs_esp_last_error());
        return;
    }

    /* Escribe un fichero. */
    mfs_file *f = NULL;
    st = mf_open(&s_fs, "/hola.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
    if (st == MFS_OK) {
        size_t wr = 0;
        mf_write(f, "Hola, MatrixFS", 14, &wr);
        mf_close(f);
    }

    /* Lo vuelve a leer. */
    char buf[32] = {0};
    st = mf_open(&s_fs, "/hola.txt", MFS_O_RDONLY, &f);
    if (st == MFS_OK) {
        size_t rd = 0;
        mf_read(f, buf, sizeof(buf) - 1u, &rd);
        mf_close(f);
        printf("matrixfs: leido \"%s\"\n", buf);
    }

    mf_sync(&s_fs);
    mf_deinit(&s_fs);
}

/* Formateo explícito (destruye el contenido), p. ej. desde una utilidad: */
void formatear(void) {
    mfs_st st = matrixfs_esp_format("matrixfs", &s_fs, NULL);
    if (st != MFS_OK)
        printf("matrixfs: %s\n", matrixfs_esp_last_error());
}
```

Para pasar opciones explícitas (sin depender de Kconfig), usa
`matrixfs_esp_mount(label, &s_fs, &opts)` con `mfs_embedded_opts` rellenado por
`mfs_embedded_opts_default()`.

---

## 6. API pública

```c
mfs_st matrixfs_esp_mount(const char *partition_label, mf_t *out_fs,
                          const mfs_embedded_opts *opts);
mfs_st matrixfs_esp_format(const char *partition_label, mf_t *fs,
                           const mfs_embedded_opts *opts);
mfs_st matrixfs_esp_mount_default(mf_t *out_fs);
const char  *matrixfs_esp_last_error(void);
```

- `partition_label == NULL` o vacío usa `CONFIG_MATRIXFS_PARTITION_LABEL`.
- `opts == NULL` usa los valores por defecto de `mfs_embedded_opts_default()`
  (sin formateo automático).
- `matrixfs_esp_last_error()` devuelve el último mensaje legible (nunca `NULL`);
  no es thread-safe y es válido hasta la siguiente llamada desde la misma tarea.

---

## 7. Limitaciones

- **Una sola instancia.** El núcleo usa tablas estáticas y el componente aloja
  el descriptor de flash en estado estático: no se pueden montar dos volúmenes
  simultáneamente.
- **Capacidad direccionable.** La ventana de zonas en RAM es de
  `MFS_ZONE_MAX = 128` zonas y, en esta integración, una zona equivale a un
  bloque borrable (`zones_per_block = 1`, `zone_size = erase_unit`). Con
  `erase_unit = 4096` y las 3 zonas reservadas de la región baja (SB A, SB B y
  anillo de tokens), el área de datos utilizada ronda los ~500 KiB por volumen.
  Una partición mayor no se aprovecha completa en el build por defecto.
- **Geometría.** Se toma `erase_size` de la partición (típicamente 4096) y
  `page_size = 256`. Los presupuestos temporales `t_*_max_us` quedan a 0 y la
  capa embebida aplica los valores NOR por defecto.
- **Programación alineada.** `esp_partition_write` puede exigir alineación
  (bloque de cifrado de 16 B en particiones cifradas). Los callbacks hacen
  *read-modify-write* alineado a 4 B con un buffer estático de 256 B; conviene
  revisar este punto si se habilita el cifrado de flash a nivel de partición.
- **Secciones críticas.** El puerto usa `portENTER_CRITICAL`, que deshabilita
  interrupciones en el núcleo actual; el núcleo las usa sólo en tramos cortos.
- **`mfs_port_wfi`** cede la CPU (`taskYIELD`), por lo que el núcleo debe
  invocarse desde contexto de tarea, no desde una ISR.
- **ISR-safe.** La API asíncrona del núcleo (`mf_submit`/`mf_poll`) no se ejerce
  desde este componente; si se usa desde ISR, revisar las garantías del núcleo.

---

## 8. Estado de verificación

**No verificado (pendiente):**

- **El componente no se ha compilado contra ESP-IDF en este repositorio.** No se
  ha ejecutado `idf.py build` ni `idf_component_register` con una instalación
  real de ESP-IDF; por tanto no está confirmado que compile ni que enlace.
- No se ha probado el montaje, la escritura/lectura ni el formateo en hardware
  ni en el emulador de ESP-IDF (QEMU).
- La disponibilidad exacta de `esp_cpu_get_cycle_count()` y del campo
  `erase_size` de `esp_partition_t` puede variar según la versión de IDF; se
  asumen disponibles a partir de IDF v5.0. Si el IDF usado los expone con otro
  nombre, ajustar `matrixfs_esp.c`.
- El comportamiento de alineación de `esp_partition_write` no se ha verificado
  experimentalmente con particiones cifradas.

**Verificado por inspección estática:**

- Coherencia estricta con las firmas reales del núcleo
  (`mf_init`, `mf_format`, `mf_open`, `mf_read`, `mf_write`, `mf_close`,
  `mf_deinit`, `mf_sync`, `mfs_ststr`, etc.) y con la API de la capa embebida
  (`mfs_embedded_opts_default`, `mfs_embedded_mount`, `mfs_embedded_format`).
- Firmas de los callbacks (`read`/`prog`/`erase`) y campos de
  `mfs_embedded_flash_t` / `mfs_embedded_opts` conforme a
  `platform/embedded/mfs_embedded.h`.
- Uso de la API de particiones conforme a su contrato documentado
  (`esp_partition_find_first`, `esp_partition_read`, `esp_partition_write`,
  `esp_partition_erase_range`).
- Las primitivas de puerto se definen una única vez y sin depender de heap.
