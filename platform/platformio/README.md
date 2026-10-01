# MatrixFS — integración PlatformIO

Licencia: Apache-2.0.

Cómo integrar el envoltorio Arduino de MatrixFS (`platform/arduino`) en un
proyecto PlatformIO. El ejemplo de esta carpeta usa `board = esp32dev`,
`framework = arduino`.

> Estado: **no verificado** con SDK real. Las rutas y APIs pueden variar según la
> versión de cada plataforma. No se inventan flags: donde PlatformIO no ofrece un
> mecanismo estándar, se indica y se propone una alternativa explícita.

---

## 1. Por qué no basta con `library.json`

El manifiesto `platform/platformio/platformio.ini` y
`platform/arduino/library.json` describen sólo el **envoltorio** (`MatrixFS.cpp`).
PlatformIO **no permite** que una librería compile ficheros que están fuera de su
directorio mediante `build.srcFilter`, así que el núcleo (`../../src`,
`../../include`, `../../platform/embedded`) no entra solo. Hay que aportarlo.

---

## 2. Añadir el envoltorio como librería

`platform/arduino/` es una librería detectable (tiene `library.json`). Para que
PlatformIO la encuentre sin copiarla dentro del proyecto, usa `lib_extra_dirs`:

```ini
[platformio]
lib_extra_dirs = ../        ; desde platform/platformio deja ver platform/arduino
```

o fija explícitamente la dependencia local (una de las dos):

```ini
lib_deps =
    symlink://../arduino
    ; o, si prefieres copiarla:
    ; file://C:/ruta/a/MatrixFS-ULTRA/platform/arduino
```

---

## 3. Aportar el núcleo (obligatorio)

El núcleo C y la capa embebida deben compilarse y sus cabeceras estar en la ruta
de inclusión. Como no se pueden referenciar desde `library.json`, la vía
recomendada es **una segunda librería local** dentro del proyecto que contenga el
núcleo (copiado o por enlace simbólico):

```
tu-proyecto/
├── platformio.ini
├── partitions.csv
├── src/
│   └── main.cpp
└── lib/
    └── matrixfs-core/
        ├── include/matrixfs/*.h        <- de include/matrixfs
        ├── src/                        <- de src/ (core, crypto, ftl, tier, sec, xio, mfs_internal.h)
        └── platform/embedded/          <- de platform/embedded (mfs_embedded.h/.c)
```

PlatformIO añade automáticamente `lib/matrixfs-core/src` y (si existe)
`lib/matrixfs-core/include` al camino de inclusión, y compila los `.c` que
encuentre. Como `mfs_embedded.h` no está en `src/` ni `include/`, se añade el
resto de rutas con `build_flags`:

```ini
build_flags =
    -Ilib/matrixfs-core/include
    -Ilib/matrixfs-core/src
    -Ilib/matrixfs-core/platform/embedded
```

`-I` en `build_flags` es un mecanismo estándar de PlatformIO con rutas relativas
al directorio del proyecto; no es un flag inventado. Si prefieres no mantener
copias, crea el árbol con enlaces simbólicos; en Windows requiere permisos de
desarrollador o `mklink /D`.

Las fuentes a compilar son:

```
src/core/*.c  src/crypto/*.c  src/ftl/*.c  src/tier/*.c  src/sec/*.c
src/xio/*.c   platform/embedded/mfs_embedded.c
```

> No añadas `sim/mfs_port_host.c`: el envoltorio ya define `mfs_port_*`. Duplicar
> esos símbolos rompe el enlazado.

---

## 4. Reservar la partición

PlatformIO no reserva la partición por ti. Define una partición de datos
`matrixfs` en un `partitions.csv` del proyecto y apúntala desde el `platformio.ini`:

```ini
board_build.partitions = partitions.csv
```

Ejemplo de `partitions.csv` (flash 4 MB):

```csv
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x5000,
otadata,    data, ota,      0xe000,   0x2000,
app0,       app,  ota_0,    0x10000,  0x140000,
app1,       app,  ota_1,    0x150000, 0x140000,
matrixfs,   data, spiffs,   0x290000, 0x160000,
coredump,   data, coredump, 0x3f0000, 0x10000,
```

La etiqueta (`matrixfs`) debe coincidir con la del constructor C++
(`MatrixFS mfs("matrixfs")`); el envoltorio la localiza con
`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
"matrixfs")`. Se usa el *subtype* `spiffs` sólo para que el resto de herramientas
acepte la partición: **MatrixFS no lee ni escribe formato SPIFFS/LittleFS**, sólo
ocupa el rango.

En ESP8266 y RP2040 no hay particiones homogéneas: hay que reservar un rango y
pasarlo al constructor como `sizeBytes` y `baseAddr`.

---

## 5. Por qué NO se usa `board_build.filesystem = littlefs`

`board_build.filesystem` le dice al build de PlatformIO que empaquete/gestiona una
imagen del **sistema de ficheros del core** (LittleFS/SPIFFS) en una partición.
MatrixFS **no es LittleFS**: define su propio layout en flash (superblocks,
WAL, zonas), su propia inicialización (`mfs_embedded_format`) y su propio
formato. Dejar que PlatformIO coloque una imagen LittleFS en el mismo rango
corrompería el volumen (o viceversa). Por eso la partición MatrixFS se declara
como datos y **sin** `board_build.filesystem`.

---

## 6. Ejemplo de uso

`src/main.cpp`:

```cpp
#include <Arduino.h>
#include <MatrixFS.h>

static MatrixFS mfs("matrixfs");

void setup() {
  Serial.begin(115200);
  if (!mfs.begin(/*formatIfNeeded=*/true)) {
    Serial.print("fallo: ");
    Serial.println(mfs.lastErrorString());
    return;
  }
  const char *msg = "hola";
  mfs.writeFile("/a.txt", msg, 4);
  char buf[16]; size_t n = 0;
  mfs.readFile("/a.txt", buf, sizeof(buf) - 1, &n);
  buf[n] = '\0';
  Serial.println(buf);
  mfs.end();
}

void loop() {}
```

Flujo típico:

```
pio run              # compilar
pio run -t upload    # flashear
pio device monitor   # monitor serie @115200
```

---

## 7. Limitaciones y estado

- El layout del núcleo bajo `lib/matrixfs-core` hay que mantenerlo (copiado o
  symlink); PlatformIO no permite referenciar `../../src` directamente desde
  `library.json`.
- `platforms` del manifiesto son `espressif32`, `espressif8266` y `raspberrypi`.
  La variante STM32 está declarada pero **no implementada** en el envoltorio.
- **No verificado**: compilación real con cada plataforma, tamaños de `.bss`,
  comportamiento de la barrera de programación y encaje de la partición. Hay que
  validarlo en hardware antes de dar la integración por buena.
