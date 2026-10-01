# MatrixFS Ultra «ATLAS» — Integración con Windows 10/11 (WinFsp)

Documento oficial de integración de MatrixFS Ultra con Windows. Cubre
compatibilidad, requisitos de compilación, instalación, configuración,
asignación de letras de unidad, integración con el Explorador de Archivos,
resolución de problemas, validación y detalles de implementación.

- Código y recursos: [`platform/windows/`](../platform/windows/) y
  [`platform/common/`](../platform/common/)
- Guía rápida: [`platform/windows/README.md`](../platform/windows/README.md)

---

## 1. Modelo de integración

Windows no permite implementar sistemas de archivos en espacio de usuario sin
un *driver* intermedio. MatrixFS Ultra utiliza **WinFsp** (Windows File System
Proxy), un marco de trabajo de código abierto y ampliamente desplegado que
actúa de puente entre el gestor de E/S de Windows y un proceso de usuario.
Gracias a ello **no se requiere ningún driver propio** ni su firma.

```
        ┌──────────────────────────────────────────────┐
        │  Explorador de Archivos · cmd · PowerShell   │
        └───────────────────────┬──────────────────────┘
                                │ gestor de E/S (NT)
        ┌───────────────────────▼──────────────────────┐
        │  Driver WinFsp  (winfsp-x64.sys, firmado)    │
        └───────────────────────┬──────────────────────┘
                                │ ABI FSP (NTSTATUS)
        ┌───────────────────────▼──────────────────────┐
        │  matrixfs_winfsp.exe        (platform/windows)│
        │    └── mfs_vfs   (platform/common)  ← mutex  │
        │          └── mfs_blk  (driver L2, RMW)       │
        │                └── imagen | \\.\X: | PhysicalDriveN
        └──────────────────────────────────────────────┘
```

La semántica del sistema de archivos reside en la capa portable
`platform/common/mfs_vfs.c`, **compartida con el front-end FUSE de Linux**. El
front-end WinFsp sólo traduce `NTSTATUS`/`FSP_FSCTL_*` a llamadas `mfs_vfs_*`,
por lo que el comportamiento y el *layout* on-flash son idénticos en ambos
sistemas y un volumen creado en Windows se monta en Linux sin conversión
alguna.

---

## 2. Compatibilidad

### 2.1 Versiones de Windows

| Versión | Soporte | Notas |
|---|---|---|
| Windows 10 1809 (LTSC 2019) | ✅ | ABI de WinFsp estable. |
| Windows 10 21H2 / 22H2 | ✅ | Versiones más extendidas. |
| Windows 11 21H2, 22H2, 23H2, 24H2 | ✅ | Incluye integración con el Explorador moderno. |
| Windows Server 2019 / 2022 | ✅ | Igual que las versiones cliente equivalentes. |
| x86 (32 bits) | ⚠️ | WinFsp ofrece `winfsp-x86`; el CMake de este proyecto construye x64. |
| ARM64 | ⚠️ | WinFsp dispone de build ARM64; requiere adaptar el CMake. |

### 2.2 Componentes

| Componente | Mínimo | Origen |
|---|---|---|
| WinFsp (runtime + driver firmado) | 1.12 | <https://winfsp.dev> |
| WinFsp SDK (`inc\`, `lib\`) | 1.12 | Incluido en el instalador de WinFsp |
| Visual Studio 2019/2022 (C++) o LLVM/clang-cl | — | — |
| CMake | 3.20 | <https://cmake.org> |
| Inno Setup (sólo para el instalador) | 6.x | <https://jrsoftware.org/isinfo.php> |

> **Compilador.** El SDK de WinFsp está escrito para la CRT de Microsoft: usa
> `static_assert`, `#pragma warning` e intrínsecos como `_ReadWriteBarrier`.
> Por tanto el front-end **debe compilarse con MSVC o clang-cl**; MinGW GCC falla
> al procesar `winfsp.h`. La capa portable, el CLI y el servicio de automontaje
> sí compilan con MinGW (véase `build.ps1` y §4).
>
> **Orden de inclusión (obligatorio).** `matrixfs_winfsp.c` incluye
> `<winfsp/winfsp.h>` **antes** que `<windows.h>` y **nunca define
> `WIN32_LEAN_AND_MEAN`**: la cabecera de WinFsp necesita que `<windows.h>`
> incluya `<winternl.h>` sin recortar para disponer de `NTSTATUS`/`PNTSTATUS`.
> Omitirlo produce errores del tipo «'PNTSTATUS': el nombre de la lista de
> parámetros formales no es válido».

> **Runtime.** El ejecutable enlaza contra `winfsp-x64.dll`. El instalador de
> WinFsp añade `C:\Program Files (x86)\WinFsp\bin` al `PATH`; si no está, el
> proceso termina inmediatamente con `0xC0000135` (`STATUS_DLL_NOT_FOUND`) y sin
> ningún mensaje. Copie la DLL junto al ejecutable o añada ese directorio al
> `PATH`.

### 2.3 Medios soportados

| Medio | Ruta | Privilegios |
|---|---|---|
| Imagen de fichero | `D:\vol0.img` | Usuario normal |
| Volumen lógico | `\\.\X:` | Administrador |
| Volumen por GUID | `\\?\Volume{…}\` | Administrador |
| Disco físico | `\\.\PhysicalDrive2` | Administrador |

La geometría se obtiene con `IOCTL_DISK_GET_LENGTH_INFO` y la alineación física
con `IOCTL_STORAGE_QUERY_PROPERTY`. Toda la E/S sobre volúmenes crudos se
realiza **alineada a sector**, tal y como exige Windows.

> **Límite de 4 GiB.** El núcleo direcciona con 32 bits; sólo se exponen los
> primeros 4 GiB del medio. Véase
> [`known-limitations.md`](known-limitations.md).

---

## 3. Estructura de carpetas

```
platform/
├── common/                        # Capa de integración portable (Linux + Windows)
│   ├── mfs_plat.{h,c}             #   Mutex (CRITICAL_SECTION) y tiempo
│   ├── mfs_blk.{h,c}              #   Driver L2: imagen / \\.\X: / PhysicalDriveN
│   ├── mfs_vfs.{h,c}              #   Adaptador VFS: montaje, formato, E/S, permisos
│   └── mfs_vfsctl.c               #   CLI de validación (format/probe/ls/cat/…)
└── windows/                       # Todo lo específico de Windows
    ├── matrixfs_winfsp.c          #   Front-end WinFsp (unidad con letra)
    ├── service/matrixfs_automount.c  # Servicio de automontaje
    ├── CMakeLists.txt             #   Build MSVC/clang-cl
    ├── build.ps1                  #   Compilación asistida -> dist\<Config>\
    ├── install.ps1, uninstall.ps1 #   Instalación y registro del servicio
    └── installer/matrixfs.iss     #   Instalador Inno Setup
```

---

## 4. Requisitos de compilación

1. Instale **Visual Studio 2022** con la carga de trabajo «Desarrollo para el
   escritorio con C++» (o LLVM/clang-cl).
2. Instale **CMake 3.20+** y asegúrese de que está en el `PATH`.
3. Instale **WinFsp** desde <https://winfsp.dev>. Si lo instala en una ruta no
   estándar, defina la variable de entorno `WINFSP` apuntando a su raíz (la que
   contiene `inc\` y `lib\`).

> **Dimensión del mapa L2P.** El núcleo embebido usa una tabla L2P estática de
> 4096 entradas (objetivo MCU). Para que un build de host cubra el volumen
> completo del modo Extended, `platform/windows/CMakeLists.txt` compila con
> `MFS_L2P_SLOTS=262144` (2 MiB de `.bss`, sin heap). Puede cambiarse con
> `-DMFS_L2P_SLOTS=N` (potencia de dos). **Debe coincidir con el valor de Linux**
> ([`platform/linux/Makefile`](../platform/linux/Makefile), variable `L2P_SLOTS`)
> para que ambos sistemas lean el mismo layout; el valor **no** se persiste en el
> medio.

```powershell
$env:WINFSP = "D:\SDK\WinFsp"

cd platform\windows
.\build.ps1                       # Release x64
.\build.ps1 -Config Debug -Clean
```

Equivalente con CMake puro:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Artefactos resultantes en `dist\Release\`:

| Binario | Función | Toolchain |
|---|---|---|
| `matrixfs_winfsp.exe` | Monta un volumen en una letra de unidad (requiere WinFsp). | **MSVC/clang-cl** (SDK de WinFsp) |
| `matrixfs-ctl.exe` | Formatear, sondear, listar, verificar (no requiere WinFsp). | MSVC, clang-cl o MinGW |
| `matrixfs_automount.exe` | Servicio de detección y automontaje. | MSVC, clang-cl o MinGW |

Equivalente sin CMake (por ejemplo con MSVC desde un *Developer Command Prompt*):

```bat
cl /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /D_FILE_OFFSET_BITS=64 ^
   /Iinclude /Isrc /Iplatform\common /I"%WINFSP%\inc" ^
   platform\windows\matrixfs_winfsp.c src\core\*.c src\crypto\*.c src\ftl\*.c ^
   src\tier\*.c src\sec\*.c src\xio\*.c sim\mfs_port_host.c ^
   platform\common\mfs_plat.c platform\common\mfs_blk.c platform\common\mfs_vfs.c ^
   /Fe:matrixfs_winfsp.exe /link /LIBPATH:"%WINFSP%\lib" winfsp-x64.lib
```

---

## 5. Instalación

### 5.1 Instalación manual

```powershell
# PowerShell como Administrador
cd platform\windows
.\install.ps1                    # copia en %ProgramFiles%\MatrixFS + servicio
.\install.ps1 -InstallDir D:\MatrixFS -AddToPath
.\install.ps1 -NoService         # sólo los binarios
```

El script copia los tres ejecutables, opcionalmente los añade al `PATH` del
sistema y registra el servicio **`MatrixFS-Automount`** con arranque automático
y reinicio ante fallos (`sc failure … restart/5000/restart/10000/restart/30000`).

### 5.2 Instalador (Inno Setup)

```powershell
iscc installer\matrixfs.iss
# -> installer\Output\MatrixFS-Ultra-Setup-1.0.0.exe
```

El instalador:

1. Copia los binarios en `%ProgramFiles%\MatrixFS`.
2. Instala **WinFsp** silenciosamente si no está presente (coloque
   `winfsp.msi` en `installer\redist\`; descargue el `.msi` desde las
   *releases* de WinFsp).
3. Registra y arranca el servicio de automontaje.
4. Al desinstalar detiene y elimina el servicio, finaliza los montajes activos
   y borra los binarios **sin tocar nunca los volúmenes del usuario**.

---

## 6. Creación de volúmenes

```powershell
# Imagen de fichero de 64 MiB
& "$env:ProgramFiles\MatrixFS\matrixfs-ctl.exe" format D:\vol0.img --label DATOS

# Partición o disco crudo (requiere administrador; destruye su contenido)
& "$env:ProgramFiles\MatrixFS\matrixfs-ctl.exe" format \\.\X: --label DATOS
& "$env:ProgramFiles\MatrixFS\matrixfs-ctl.exe" format \\.\PhysicalDrive2 --erase-unit 4096
```

Diagnóstico sin montar:

```powershell
matrixfs-ctl.exe probe  D:\vol0.img          # ¿es un volumen MatrixFS?
matrixfs-ctl.exe label  D:\vol0.img          # etiqueta
matrixfs-ctl.exe statfs D:\vol0.img          # total / libre / usado
matrixfs-ctl.exe verify D:\vol0.img [--full] # integridad
matrixfs-ctl.exe ls     D:\vol0.img /
matrixfs-ctl.exe cat    D:\vol0.img /nota.txt
```

---

## 7. Montaje y asignación de letra de unidad

```powershell
# Unidad X: (primer plano; Ctrl+C desmonta)
matrixfs_winfsp.exe D:\vol0.img X:

# Sin letra explícita: WinFsp asigna la primera disponible
matrixfs_winfsp.exe D:\vol0.img

# Volumen crudo en sólo lectura
matrixfs_winfsp.exe \\.\PhysicalDrive2 Y: -o ro

# Montaje en una carpeta (junction) en lugar de una letra
matrixfs_winfsp.exe D:\vol0.img C:\mnt\vol0
```

| Opción | Efecto |
|---|---|
| `-o label=NOMBRE` | Etiqueta de volumen (se aplica al formatear). |
| `-o format` | Formatea el medio antes de montar. |
| `-o ro` | Sólo lectura; el volumen se muestra con atributo de sólo lectura. |
| `-o ram=N` | Presupuesto RAM declarado (por defecto 262144 → modo Extended, chunk 4096). |
| `-o erase_unit=N` | Bloque de borrado en bytes. Por omisión se **deriva del tamaño del medio** (potencia de dos ≥ `tamaño / 128`, acotada a 4 MiB). **Debe coincidir con el valor usado al formatear**: cambiarlo altera el layout. |
| `-d` | Traza de depuración de WinFsp por consola. |

### 7.1 Integración con el Explorador de Archivos

| Función del Explorador | Operación WinFsp | Resultado |
|---|---|---|
| Ver la unidad y su etiqueta | `GetVolumeInfo` | Nombre de volumen (`MatrixFS`) y etiqueta; capacidad y espacio libre. |
| Navegar carpetas | `Create`/`Open` + `ReadDirectory` | Listado con atributos, tamaño y fecha. |
| Doble clic / abrir archivo | `Open` + `Read` | Lectura por bloques desde el medio. |
| Copiar / pegar (entrada) | `Create`/`Write` | Creación y escritura con tamaño y atributos. |
| Copiar / pegar (salida) | `Read` | Lectura secuencial o por bloques. |
| Eliminar | `Cleanup`/`Close` y retirada de la entrada | Borrado del fichero o directorio. |
| Renombrar (F2) | `Rename` | Renombrado atómico con reemplazo. |
| Propiedades → tamaño | `SetFileSize` | Truncado o extensión. |
| Propiedades → sólo lectura | `SetBasicInfo` | Traducido a los bits POSIX de escritura (persistidos). |
| Propiedades → fecha | `SetBasicInfo` | `mtime` persistido en el volumen. |
| Pegar sobrescribiendo | `Overwrite` | Truncado a cero y reescritura. |
| Arrastrar y soltar | Combinación de `Create`/`Read`/`Write`/`Rename` | Soporte completo. |

---

## 8. Automontaje

El servicio **`MatrixFS-Automount`** (`matrixfs_automount.exe`) se ejecuta como
`LocalSystem` y realiza un ciclo cada 3 s:

1. Enumera los volúmenes con `FindFirstVolumeW`/`FindNextVolumeW`.
2. Sondea cada uno con la capa VFS portable (`mfs_vfs_probe`): lectura de los
   dos superblocks, 512 B, sin escritura.
3. Si detecta un volumen MatrixFS Ultra aún no montado, elige la primera letra
   libre desde `D:` y lanza `matrixfs_winfsp.exe <volumen> <letra>:` como
   proceso hijo sin ventana.
4. Supervisa los procesos hijos; cuando uno termina o el volumen desaparece,
   libera la letra y la entrada de la tabla.

```powershell
Get-Service MatrixFS-Automount
Start-Service MatrixFS-Automount
Stop-Service  MatrixFS-Automount
Restart-Service MatrixFS-Automount

# Pasada única de detección sin instalar el servicio:
matrixfs_automount.exe --scan

# Diagnóstico: mensajes del servicio en DebugView (Sysinternals)
```

Con esto, al conectar un medio con un volumen MatrixFS Ultra aparece
automáticamente una unidad nueva en «Este equipo», sin intervención del
usuario.

---

## 9. Resolución de problemas

| Síntoma | Diagnóstico y solución |
|---|---|
| `matrixfs: montaje de '…' falló: MFS_EIO` con `\\.\X:` | Ejecute como administrador o compruebe que el volumen existe (`Get-Volume`). |
| No aparece ninguna unidad nueva | WinFsp no instalado, servicio detenido o medio sin volumen MatrixFS. Verifique con `matrixfs-ctl.exe probe` y `Get-Service MatrixFS-Automount`. |
| El proceso termina al instante **sin ningún mensaje**; código `-1073741515` (`0xC0000135`) | No se encuentra `winfsp-x64.dll`. Añada `C:\Program Files (x86)\WinFsp\bin` al `PATH` o copie la DLL junto al ejecutable. |
| `matrixfs: FspFileSystemCreate falló (0xC000000E)` | `STATUS_NO_SUCH_DEVICE`: el **driver FSD de WinFsp no está accesible** (no está cargado, o la sesión no tiene privilegios para abrir su dispositivo). Instale/repare WinFsp y ejecute desde una consola elevada. |
| `FspFileSystemCreate falló (0xC0000035)` | Ya existe un punto de montaje con ese nombre; elija otra letra. |
| Errores del tipo `'PNTSTATUS': el nombre de la lista de parámetros formales no es válido` al compilar | Se incluyó `<windows.h>` antes que `<winfsp/winfsp.h>` o se definió `WIN32_LEAN_AND_MEAN`. Corrija el orden de inclusión y elimine esa macro. |
| Errores en `winfsp.h` con MinGW GCC (`_ReadWriteBarrier`, `static_assert`, `#pragma warning`) | El SDK de WinFsp requiere MSVC o clang-cl. Use `build.ps1`/CMake con Visual Studio. |
| `CreateProcessW falló (740)` | El servicio no tiene privilegios; reinstálelo desde una consola elevada. |
| `matrixfs: no se pudo asignar el punto de montaje` | La letra está en uso por otro volumen; pruebe otra o deje que WinFsp la asigne. |
| Windows pide formatear la unidad | Windows no reconoce el sistema de archivos (igual que con `ext4`); **no formatee**: instale WinFsp y el servicio, o monte el volumen en Linux. |
| `no contiene un volumen MatrixFS válido` | Formatee con `matrixfs-ctl.exe format` o sondee otro dispositivo. |
| `montaje fallido: MFS_ENOTVIABLE` | Presupuesto RAM insuficiente: aumente `-o ram=N` (p. ej. 32768). |
| `acceso denegado` al abrir ficheros | Los permisos POSIX persistidos del volumen no conceden acceso; ajuste desde Linux con `chmod` o acceda con un usuario propietario. |
| El servicio no arranca y el Visor de eventos está vacío | Falta `matrixfs_winfsp.exe` junto a `matrixfs_automount.exe`; ambos deben estar en el mismo directorio. |
| Sólo se ven los primeros 4 GiB | Límite por direccionamiento de 32 bits del núcleo (`known-limitations.md`). |
| Rendimiento bajo en escritura aleatoria | El medio es NOR/NAND crudo sin FTL; use `-o erase_unit` acorde al chip o un medio gestionado (eMMC/SD). |

Diagnóstico:

```powershell
Get-Service MatrixFS-Automount | Format-List *
Get-Volume | Format-Table DriveLetter, FileSystemLabel, FileSystemType
matrixfs-ctl.exe verify \\.\X:
Get-WinEvent -LogName Application -MaxEvents 20 | Where-Object { $_.Message -match 'MatrixFS' }
```

---

## 10. Validación realizada

Compilación y ejecución **reales sobre Windows** (Windows 10/11 x64 con
MSVC 14.51 «VS 18 BuildTools» + SDK de WinFsp 2025, y MinGW-w64 GCC 11):

| Prueba | Toolchain | Resultado |
|---|---|---|
| Suite completa del proyecto | MSVC 14.51 (`/W4`, CMake + VS 18 BuildTools) | ✅ **1 134 checks / 0 fallos** |
| `test_vfs_portable` (formato, montaje, permisos, E/S, readdir, rename, truncado, statfs, verify, persistencia, sólo lectura, errno) | MSVC 14.51 | ✅ |
| `matrixfs-ctl.exe` | **MSVC 14.51** | ✅ ciclo completo sobre imagen (`format`→`probe`→`mkdir`→`write`→`ls`→`cat`→`statfs`→`verify`) |
| `matrixfs_automount.exe` | MSVC 14.51 | ✅ compila y enlaza |
| `matrixfs_winfsp.exe` | **MSVC 14.51** (SDK de WinFsp real, `/std:c11 /W4`) | ✅ compila y enlaza con `winfsp-x64.lib` |
| Carga de `winfsp-x64.dll` y llamada a la API | MSVC | ✅ `FspFileSystemCreate` responde con un `NTSTATUS` tipificado |
| Compilación limpia de los tres binarios | **MSVC 14.51** (`/std:c11 /W4`) | ✅ sin avisos ni errores |
| **Interoperabilidad Linux ↔ Windows** | MSVC 14.51 + gcc 14.2 | ✅ un volumen creado en Windows se lee en Linux (contenido y `verify=MFS_OK`) y uno creado en Linux se lee en Windows (contenido idéntico, `ls` y `verify=MFS_OK`) |
| `matrixfs_winfsp.exe` con MinGW GCC | MinGW | ⛔ el SDK de WinFsp no es compatible con MinGW (`_ReadWriteBarrier`, `static_assert`, `#pragma warning`) |

Defectos reales detectados y corregidos gracias a esta compilación:

1. **Orden de inclusión**: `<winfsp/winfsp.h>` debe preceder a `<windows.h>` y
   `WIN32_LEAN_AND_MEAN` debe eliminarse (si no, `PNTSTATUS` no se define).
2. **`FSP_FILE_SYSTEM_INTERFACE`**: la estructura real de WinFsp 1.12 tiene 32
   entradas (empieza por `SetVolumeLabel` y añade `CanDelete`, reparse points,
   streams, EA…). Se pasó a inicializadores designados para que el compilador
   valide cada firma y el código no dependa del orden.
3. **`FSP_FSCTL_DIR_INFO`**: no existe el campo `FileInfoSize` que se había
   asumido; el tamaño se calcula como `sizeof(FSP_FSCTL_DIR_INFO) + nombre`.
4. **Borrado no implementado**: eliminar un fichero en el Explorador no lo
   borraba. El borrado se completa en `Cleanup` cuando llega el flag
   `FspCleanupDelete` (`rmdir`/`unlink` según el tipo de nodo).
5. `WIN32_LEAN_AND_MEAN` retirado también de `CMakeLists.txt` (era global y
   rompía el front-end).
6. **`matrixfs-ctl cat` corrompía la salida binaria**: la CRT traducía `\n` a
   `\r\n`, de modo que extraer un fichero de 1 MiB producía 1 052 601 bytes. El
   CLI pone `stdout` en modo binario (`_setmode(_fileno(stdout), _O_BINARY)`).
7. **`erase_unit` incoherente entre binarios**: fijarlo en un ejecutable y
   derivarlo en otro producía **layouts distintos** del mismo medio (pérdida de
   etiqueta y de datos entre Linux y Windows). Ahora la geometría se deriva de
   forma determinista del tamaño del medio y ningún binario la fuerza.
8. **Avisos de truncamiento con MSVC `/W4`**: conversiones implícitas en la
   reconstrucción de metadatos (entre ellas `keep_parent`, que truncaba el inodo
   padre a 16 bits), casts de constantes de 16 bits a 8 bits y un `-1` pasado a
   un parámetro `UINT32`. Corregidos: la compilación de los tres binarios queda
   **sin avisos**. El único aviso restante procedía de la propia cabecera del
   SDK de WinFsp (`fsctl.h`, `C4324`), no del código del proyecto, y se silencia
   de forma explícita sólo para ese objetivo (`/wd4324`).

### 10.1 Pendiente de ejecutar (requiere privilegios)

- **Montaje real con letra de unidad en el Explorador.** En el entorno de
  validación, `matrixfs_winfsp.exe` llega hasta `FspFileSystemCreate`, que
  devuelve `0xC000000E` (`STATUS_NO_SUCH_DEVICE`) porque el driver FSD de WinFsp
  no es accesible desde una sesión no elevada (la instalación presente es un
  despliegue SxS de desarrollo, con `fsptool load` como paso previo).
- Pruebas manuales en el Explorador de Windows 10 y 11 (copiar, pegar, arrastrar,
  Propiedades) y en el servicio de automontaje.

Procedimiento de validación recomendado (consola **elevada**):

```powershell
# 1. Capa portable (sin WinFsp)
.\build\matrixfs_tests.exe ; .\matrixfs-ctl.exe probe D:\vol0.img

# 2. Montaje real y verificación funcional en el Explorador
.\install.ps1                                  # registra el servicio
.\matrixfs_winfsp.exe D:\vol0.img X:
#   - crear carpeta, copiar un fichero grande (>4 MB), renombrar, borrar
#   - comprobar Propiedades: tamaño, fecha, sólo lectura
#   - comprobar capacidad y espacio libre en «Este equipo»
# 3. Coherencia entre plataformas
#   - desmontar, montar el mismo medio en Linux (mount -t matrixfs) y
#     verificar con 'matrixfs-ctl verify --full' + comparación de checksums
# 4. Automontaje
Get-Service MatrixFS-Automount ; (re)conectar el medio
```

---

## 11. Detalles de implementación

- **Reentrancia.** El núcleo MatrixFS mantiene estado global y **no es
  reentrante**: admite una única instancia por proceso. El adaptador `mfs_vfs`
  serializa todas las llamadas con un mutex (`CRITICAL_SECTION` en Windows), por
  lo que el front-end atiende con seguridad las peticiones concurrentes de
  WinFsp (estrategia `FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE`). Cada
  letra de unidad corresponde a un proceso y, por tanto, a una instancia.
- **Rutas.** WinFsp entrega rutas UTF-16 con separadores `\`; el front-end las
  convierte a UTF-8 con `/` antes de entregarlas al núcleo, de modo que la
  semántica de nombres coincide con la de Linux. El driver de bloque abre los
  dispositivos con `CreateFileW` (necesario para rutas `\\?\Volume{…}\`) y
  acepta la ruta en UTF-8 con reserva a ANSI.
- **Alineación.** `mfs_blk` detecta el tamaño de sector físico con
  `IOCTL_STORAGE_QUERY_PROPERTY` y realiza RMW alineado, requisito de Windows
  para handles de volumen y disco.
- **Tiempos.** Los `FILETIME` de WinFsp (100 ns desde 1601) se convierten a
  epoch Unix (segundos desde 1970) y viceversa, de modo que `mtime` es
  coherente entre plataformas.
- **Atributos.** El atributo «sólo lectura» se refleja en los bits POSIX de
  escritura del volumen (persistidos on-flash). El resto de atributos NTFS
  (oculto, sistema, comprimido, indexado) no tiene equivalente en el núcleo y
  se ignora silenciosamente.
- **Seguridad.** No se implementan descriptores de seguridad NTFS
  (`PersistentAcls = 0`); la autorización real la realizan los permisos POSIX
  persistidos del volumen, evaluados en el adaptador VFS.
- **Errores.** `mfs_nt` traduce los 29 estados tipificados del núcleo a
  `NTSTATUS`, de forma que el Explorador muestra los mensajes nativos
  («acceso denegado», «no se encuentra el archivo», «el disco está lleno», …).
- **Parada del servicio.** Al detener `MatrixFS-Automount` se termina el
  proceso de montaje de cada volumen. El núcleo es *crash-only*: en el
  siguiente montaje recupera el último estado consistente (comportamiento
  cubierto por la suite con 1 000 ciclos de corte).

---

## 12. Referencias

- [`DOCS/linux-integration.md`](linux-integration.md) — integración equivalente
  en Linux (FUSE 3), con el mismo *layout* on-flash.
- [`DOCS/architecture.md`](architecture.md) — capas del núcleo y layout físico.
- [`DOCS/testing.md`](testing.md) — plan de verificación y resultados.
- [`DOCS/known-limitations.md`](known-limitations.md) — límites y desviaciones.
- WinFsp: <https://winfsp.dev> · <https://github.com/winfsp/winfsp>
