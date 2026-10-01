# MatrixFS Ultra — módulo de usuario (usermod) para MicroPython

Integración de referencia de MatrixFS Ultra como **módulo C de usuario
(`USER_C_MODULE`)** de MicroPython. Añade el módulo `matrixfs` con la clase
`MatrixFS` y la clase `File`, apoyándose en la capa embebida compartida
(`platform/embedded/mfs_embedded.h`).

> **Estado: NO verificado en compilación.** Este código no se ha compilado
> contra ninguna versión concreta de MicroPython en este repositorio. Es una
> integración de referencia y las APIs de MicroPython usadas pueden requerir
> ajustes según la versión del port (ver "Limitaciones" y "Estado"). No se
> afirma que compile tal cual.

## Contenido

| Fichero | Descripción |
|---|---|
| `modmatrixfs.c` | Módulo C: clase `MatrixFS`, clase `File`, y las primitivas obligatorias del puerto MatrixFS. |
| `micropython.mk` | Fragmento de build (`SRC_USERMOD`, `CFLAGS_USERMOD`) y lista de fuentes del núcleo. |
| `mpconfigport_fragment.h` | Fragmento de configuración (`MICROPY_PY_MATRIXFS`). |
| `README.md` | Este documento. |

## Requisitos

- MicroPython con ports basados en **make** (unix, esp32, stm32, rp2, ...).
- El módulo usa `MP_DEFINE_CONST_OBJ_TYPE` en su **forma variádica**
  (MicroPython >= 1.19). En versiones anteriores, sustituir por una definición
  explícita de `const mp_obj_type_t` con `.make_new` y `.locals_dict`.
- Compilador C11 y el toolchain del port.
- Las fuentes del núcleo MatrixFS compiladas en el mismo binario (ver abajo).
- Un **medio de bloque**. Dos opciones:
  - un objeto *bdev* con `readblocks`/`writeblocks`/`ioctl` (protocolo de
    bloque estándar de MicroPython, el mismo que usa LittleFS); o
  - una **etiqueta de partición** (`str`) + tamaño, resuelta en ESP32 vía el
    módulo `esp32`/`esp` (best-effort, no verificado).

## Cómo compilar

El módulo se añade con `USER_C_MODULES`. Ejemplo (port unix):

```sh
make -C ports/unix \
  USER_C_MODULES=$PWD/platform/micropython/micropython.mk
```

La misma construcción sirve para `ports/esp32`, `ports/stm32` y `ports/rp2`
(ajustando el `make`/`idf.py` del port). Algunos ports esperan que
`USER_C_MODULES` apunte a un **directorio** cuyo subdirectorio contenga
`micropython.mk`; en ese caso apunta a `platform/` (se descubrirá
`platform/micropython/micropython.mk`). Ambas formas se documentan porque el
comportamiento cambió entre versiones de MicroPython.

### Fuentes del núcleo

`modmatrixfs.c` referencia símbolos del núcleo, por lo que hay que compilar
también su código. Con `micropython.mk` ya se añaden los encabezados; las
fuentes del núcleo son (mismas que `CORE_SRC` del Makefile raíz):

```
src/core/*.c  src/crypto/*.c  src/ftl/*.c  src/tier/*.c  src/sec/*.c  src/xio/*.c
platform/embedded/mfs_embedded.c
```

Añádelas a `SRC_USERMOD` (líneas ya presentes, comentadas, en
`micropython.mk`) o al build del port. **No** compiles `sim/mfs_port_host.c`
ni `platform/8bit/mfs_port_8bit.c` en el mismo binario: las primitivas de
puerto las aporta `modmatrixfs.c` y habría símbolos duplicados.

### Configuración del port

Incluye el contenido de `mpconfigport_fragment.h` en el `mpconfigport.h` del
port. El registro del módulo se hace con
`MP_REGISTER_MODULE(MP_QSTR_matrixfs, mp_module_matrixfs)` dentro de
`modmatrixfs.c`; no se necesita `MICROPY_MODULE_BUILTIN_INIT` ni entradas en
`MICROPY_PORT_BUILTIN_MODULES`.

## Uso (ejemplo de sesión REPL)

Con un bdev ya creado por el port (por ejemplo un `littlefs`/`BlockDevice`
propio), o una partición etiquetada en ESP32:

```python
>>> import matrixfs
>>> fs = matrixfs.MatrixFS(mi_bdev)              # bdev con readblocks/ioctl
>>> fs.format()                                  # formatea el volumen
>>> fs.mount(mode="balanced", ram=32768)         # monta
>>> f = fs.open("/hola.txt", "w")                # 'w' crea/trunca
>>> f.write(b"hola mundo")
7
>>> f.flush()                                    # fuerza sincronizacion
>>> f.close()
>>> f = fs.open("/hola.txt", "r")
>>> f.read()
b'hola mundo'
>>> f.seek(0)
0
>>> f.read(4)
b'hola'
>>> f.tell()
4
>>> f.close()
>>> fs.stat("/hola.txt")
{'size': 10, 'mode': 33188, 'uid': 0, 'gid': 0, 'mtime': 0}
>>> fs.listdir("/")
['hola.txt']
>>> fs.umount()
```

Forma con etiqueta de partición (ESP32, best-effort):

```python
>>> fs = matrixfs.MatrixFS("mfs", 512 * 1024)    # etiqueta + tamaño
>>> fs.mkfs()                                    # alias de format()
>>> fs.mount(format=True)                        # monta (formatea si hace falta)
```

Métodos keyword de `mount()`:

- `ram` (int): RAM declarada al núcleo (presupuesto de viabilidad). Defecto 32768.
- `mode` (str): `"ultra-nano"`, `"nano"`, `"compact"`, `"balanced"`,
  `"extended"`, y los 8-bit `"8bit-ultra"`, `"8bit-nano"`, `"8bit-compact"`.
- `key` (bytes de 32): clave de cifrado. Sin ella, volumen sin cifrado.
- `format` (bool): formatea si el volumen no es válido y reintenta el montaje.

## API

### `MatrixFS(source[, size][, ram=])`

- `source`: un objeto bdev con `readblocks`/`writeblocks`/`ioctl`, o una
  etiqueta de partición (`str`).
- `size`: tamaño en bytes de la región (sólo para la forma con etiqueta).
- `ram`: RAM declarada al núcleo (presupuesto de viabilidad) usada por defecto
  si `mount()`/`format()` no la reciben. Defecto 32768.

Métodos: `format()`, `mkfs()` (alias), `mount(**kw)`, `umount()`,
`open(path, mode="r")`, `stat(path)`, `listdir([path])`.

### `File`

`read([n])`, `readinto(buf)`, `write(buf)`, `seek(pos[, whence])`,
`tell()`, `close()`, `flush()`.

## Tabla de mapeo de errores

Los códigos de estado del núcleo (`mfs_st`, negativos) se traducen a
excepciones `OSError` de MicroPython:

| `mfs_st` | errno MicroPython |
|---|---|
| `MFS_ENOENT` | `ENOENT` (2) |
| `MFS_EEXISTS` | `EEXIST` (17) |
| `MFS_EINVAL` | `EINVAL` (22) |
| `MFS_ENOSPC` | `ENOSPC` (28) |
| cualquier otro error negativo | `EIO` (5) |

Estados no-montado o sin medio preparado se reportan como `EINVAL`.

## Integración con el VFS

MicroPython ya trae su propio VFS y una implementación de LittleFS. **MatrixFS
se añade como un módulo adicional y no sustituye a ese VFS.**

MicroPython permite montar objetos que implementan el **protocolo VFS**
(`mount`, `umount`, `open`, `stat`, `ilistdir`, ...) mediante
`os.mount(obj, "/ruta")`. Este usermod **no implementa** ese protocolo: expone
su propia API (`MatrixFS.open`, `listdir`, `stat`, ...). Por tanto, **no es
posible** registrarlo con `os.mount()`/`vfs.mount()` tal cual; para hacerlo
habría que añadir un objeto adaptador (una capa VFS análoga a
`extmod/vfs_lfs.c`) que traduzca la interfaz VFS a las llamadas de
`MatrixFS`/`File`. Ese adaptador queda fuera del alcance de esta integración.

## Primitivas de puerto aportadas

Este módulo implementa el contrato obligatorio de puerto de MatrixFS (§20.2):

| Función | Implementación |
|---|---|
| `mfs_port_crit_enter` / `mfs_port_crit_exit` | `MICROPY_BEGIN_ATOMIC_SECTION()` / `MICROPY_END_ATOMIC_SECTION()` |
| `mfs_port_time_us` | `mp_hal_ticks_us()` |
| `mfs_port_cycles` | `mp_hal_ticks_us()` (MicroPython no expone ciclos de forma portable) |
| `mfs_port_wfi` | `mp_hal_delay_us(0)` |

No deben definirse en ningún otro fichero del build (ver "Fuentes del núcleo").

## Limitaciones

- **Instancia única**: `mf_t`, la descripción de flash y la clave viven en
  memoria estática (BSS). Crear dos objetos `MatrixFS` comparte ese estado;
  sólo hay un volumen activo a la vez. Es la consecuencia de no usar heap para
  el núcleo.
- **Forma con etiqueta (`str`)**: sólo se intenta resolver en ESP32 mediante
  `esp32.Partition.find(label=...)` y `esp.flash_read/write/erase`. No
  verificado. En otros ports, usar un bdev.
- **Bdev**: el `block_size` debe ser potencia de dos y <= 4096 bytes; la
  programación parcial se hace con *read-modify-write* de bloque completo
  (necesario porque el bdev escribe bloques enteros y el núcleo programa a
  granularidad de página).
- **Modelo de medio**: la capa compartida `mfs_embedded` describe la región
  como flash NOR (tipo `MFS_MEDIA_NOR_SPI`). Para medios gestionados
  (SD/eMMC/NVMe) la geometría/engine puede no ser la adecuada; esta integración
  no añade perfiles de medio propios.
- **Llamadas Python dentro del núcleo**: los callbacks de lectura/escritura
  invocan métodos Python del bdev. Si el núcleo entrase en una sección crítica
  durante una operación de E/S, se podría producir un problema de reentrada;
  las excepciones del bdev se capturan con `nlr` y se traducen a `MFS_EIO`.
- **Rutas**: se normalizan a formato canónico del núcleo con un búfer de 256
  bytes.
- **`stat`** devuelve un `dict` con `size`, `mode`, `uid`, `gid`, `mtime`.
- **`File`** no tiene finalizador (`__del__`): si no se llama a `close()`, el
  handle queda tomado hasta `umount()` o el fin del programa.

## Estado

- Integración escrita y revisada estáticamente; **no compilada ni ejecutada**
  contra MicroPython. Los puntos a validar en la primera compilación son:
  - la firma variádica de `MP_DEFINE_CONST_OBJ_TYPE` y el slot `locals_dict`
    según la versión de MicroPython;
  - la disponibilidad de `mp_hal_ticks_us()` en el port objetivo (algunas
    versiones usan `mp_hal_time_us()`);
  - la API concreta del módulo `esp`/`esp32` para la forma con etiqueta.
