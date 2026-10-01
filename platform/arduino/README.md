# MatrixFS Ultra — integración Arduino (ESP32 / ESP8266 / RP2040)

Licencia: Apache-2.0.

Este directorio contiene el **envoltorio Arduino** de MatrixFS Ultra: la clase
C++ `MatrixFS`, las primitivas de puerto `mfs_port_*` y los callbacks de flash.
Es una fachada fina sobre el núcleo C; **no** es el sistema de archivos.

> Estado: la integración está escrita contra las APIs documentadas de cada core,
> pero **no se ha verificado compilando con los SDK reales** (véase
> [Sin verificar](#sin-verificar)).

---

## 1. Contenido

| Ruta | Descripción |
|---|---|
| `library.properties` | Manifiesto para Arduino IDE (Library Manager). |
| `library.json` | Manifiesto equivalente para PlatformIO. |
| `src/MatrixFS.h` | Clase C++ `MatrixFS` (API de conveniencia). |
| `src/MatrixFS.cpp` | Primitivas de puerto + callbacks de flash + implementación. |
| `examples/Basic/Basic.ino` | Ejemplo mínimo: monta, formatea si hace falta, escribe y lee. |

El envoltorio **necesita además el núcleo**:

```
src/core/*.c  src/crypto/*.c  src/ftl/*.c  src/tier/*.c  src/sec/*.c
src/xio/*.c   platform/embedded/mfs_embedded.c
```

con las rutas de inclusión `include/`, `src/` y `platform/embedded/`.

---

## 2. Instalación en Arduino IDE 2.x

Arduino IDE **no** resuelve rutas fuera de la carpeta de la librería, así que el
núcleo no se compila solo. Dos opciones:

### Opción A — librería "completa" (recomendada para el IDE)

Crea una carpeta de librería `MatrixFS` dentro de tu sketchbook
(`.../Arduino/libraries/MatrixFS/`) con esta estructura y copia (o enlaza) el
núcleo dentro:

```
Arduino/libraries/MatrixFS/
├── library.properties          <- copia de platform/arduino/library.properties
├── src/
│   ├── MatrixFS.h              <- de platform/arduino/src/
│   ├── MatrixFS.cpp            <- de platform/arduino/src/
│   ├── core/                   <- copia de src/core/
│   ├── crypto/                 <- copia de src/crypto/
│   ├── ftl/                    <- copia de src/ftl/
│   ├── tier/                   <- copia de src/tier/
│   ├── sec/                    <- copia de src/sec/
│   ├── xio/                    <- copia de src/xio/
│   ├── mfs_internal.h          <- de src/
│   ├── matrixfs/               <- copia de include/matrixfs/
│   └── mfs_embedded.h / .c     <- de platform/embedded/
```

Con todo bajo `src/`, Arduino añade `src/` al camino de inclusión y los `#include
"matrixfs/matrixfs.h"`, `"mfs_embedded.h"` y `"mfs_internal.h"` se resuelven sin
tocar flags. `MatrixFS.cpp` incluye `"mfs_internal.h"`, por lo que su directorio
debe estar en la ruta de búsqueda (aquí, `src/`).

### Opción B — compilar el núcleo en el sketch

Añade los `.c` del núcleo como ficheros del sketch y activa las rutas de
inclusión por placa; más frágil, no recomendado.

> IMPORTANTE: no enlaces además `sim/mfs_port_host.c` ni
> `platform/8bit/mfs_port_8bit.c` con `MFS_8BIT_PORT_GLUE`: este envoltorio ya
> define `mfs_port_crit_enter/exit`, `mfs_port_cycles`, `mfs_port_time_us` y
> `mfs_port_wfi`. Duplicarlas provoca error de enlazado.

---

## 3. Partición (tabla para ESP32)

En ESP32, la región se localiza por etiqueta de partición de tipo `data`. Añade
una partición `matrixfs` a tu `partitions.csv` (subtype `spiffs` para que el
resto de herramientas la acepten; MatrixFS no usa LittleFS/SPIFFS, sólo el
rango). Ejemplo de CSV completo (flash de 4 MB):

```csv
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x5000,
otadata,    data, ota,      0xe000,   0x2000,
app0,       app,  ota_0,    0x10000,  0x140000,
app1,       app,  ota_1,    0x150000, 0x140000,
matrixfs,   data, spiffs,   0x290000, 0x160000,
coredump,   data, coredump, 0x3f0000, 0x10000,
```

En Arduino IDE 2.x: *Tools > Partition Scheme* apunta a esquemas predefinidos;
para una partición propia hay que usar un `partitions.csv` con PlatformIO o
colocar el CSV en el core (`tools/partitions/`) y seleccionar el esquema
personalizado. La etiqueta del CSV debe coincidir con la del constructor
(`MatrixFS mfs("matrixfs")`).

En **ESP8266** y **RP2040** no hay API homogénea de particiones: hay que indicar
explícitamente `sizeBytes` y `baseAddr` en el constructor y reservar ese rango.

---

## 4. Uso

```cpp
#include <MatrixFS.h>

static MatrixFS mfs("matrixfs");        // ESP32
// static MatrixFS mfs("matrixfs", 32768u, 0x100000u, 0x100000u); // ESP8266/RP2040

void setup() {
  Serial.begin(115200);
  if (!mfs.begin(/*formatIfNeeded=*/true)) {
    Serial.println(mfs.lastErrorString());
    return;
  }
  mfs.writeFile("/hola.txt", "texto", 5);
  char buf[32]; size_t n = 0;
  mfs.readFile("/hola.txt", buf, sizeof(buf), &n);
  mfs.end();
}
```

Constructor: `MatrixFS(etiqueta, ramTotal = 0, sizeBytes = 0, baseAddr = 0)`.

---

## 5. Primitivas de puerto (`mfs_port_*`)

El núcleo exige estas funciones; `MatrixFS.cpp` las implementa por arquitectura:

| Arquitectura | Crítica | Tiempo | WFI |
|---|---|---|---|
| ESP32 / ESP8266 | `noInterrupts()/interrupts()` | `micros()` | `yield()` + `delay(0)` |
| RP2040 (Arduino-Pico) | `save_and_disable_interrupts()/restore_interrupts()` | `micros()` | `__wfi()` |

`mfs_port_cycles()` devuelve también `micros()`: los cores Arduino no exponen un
contador de ciclos portable. Los presupuestos temporales del núcleo usan
`mfs_port_time_us()`.

---

## 6. Limitaciones y decisiones

- **Instancia única.** `mf_t` se declara como miembro estático de `MatrixFS`
  (ocupa memoria en `.bss`, del orden de decenas de KB según `MFS_MAX_*`). Sólo
  se admite un volumen montado a la vez; `begin()` de una segunda instancia
  devuelve `MFS_EBUSY`.
- **Sin heap.** El núcleo no usa `malloc`; el estado vive en la instancia
  estática y en `mfs_embedded_flash_t` (miembro del objeto).
- **ESP8266.** `ESP.flashWrite` recibe un `const uint32_t *`; se hace *cast* del
  puntero del núcleo y se asume alineación/al menos acceso de 4 bytes. El origen
  debe estar en RAM (lo está: buffers estáticos del núcleo). Verificar en tu
  versión del core.
- **RP2040.** `flash_range_program`/`flash_range_erase` se ejecutan con
  interrupciones deshabilitadas; la región reservada debe estar **excluida del
  linker** y de cualquier otro uso (p. ej. del sistema de ficheros del core). El
  `baseAddr` es un offset sobre `XIP_BASE` y debe estar alineado a 4096.
- **STM32 y otras MCU.** `library.properties` declara `stm32`, pero `MatrixFS.cpp`
  **no** implementa callbacks para STM32 (la flash interna no es una región NOR
  plana): si compilas ahí, se dispara un `#error` que pide aportar los callbacks
  y las primitivas de puerto. Es intencionado (fallo explícito, no silencioso).
- **Tamaños.** `erase_unit = 4096`, `page_size = 256`; cualquier `baseAddr` debe
  estar alineado a 4096 y `sizeBytes` debe ser múltiplo de 4096.

---

## 7. Sin verificar

No se ha podido compilar ni ejecutar con los SDK reales en este repositorio.
Queda por validar en hardware:

- Compilación con cada core: `esp32` (Arduino-ESP32), `esp8266` (ESP8266 core) y
  `rp2040` (Arduino-Pico). En particular las firmas de `ESP.flashRead/
  flashWrite/flashEraseSector` y de `flash_range_erase/program`.
- Consumo real de RAM/flash de la instancia `mf_t` y encaje del `.bss`.
- Que la región reservada queda realmente disponible y excluida de otros usos.
- El comportamiento de la barrera de programación (WOB) exigida por el contrato.
- Funcionamiento con `format_if_needed` ante un volumen corrupto.
