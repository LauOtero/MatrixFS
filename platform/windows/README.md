# MatrixFS «ATLAS» — soporte Windows 10/11 (WinFsp)

Front-end Windows de MatrixFS. Expone un volumen MatrixFS como una
**unidad con letra** (`X:`) visible en el **Explorador de Archivos**, con
operaciones nativas completas: copiar, mover, pegar, eliminar, renombrar,
cambiar atributos y tamaño, crear carpetas, etc.

Se implementa como sistema de archivos en espacio de usuario mediante
**WinFsp**, por lo que no requiere ningún driver propio ni la firma de un
controlador del kernel: basta con el runtime de WinFsp.

Toda la semántica de sistema de archivos —permisos POSIX persistidos, metadatos,
E/S con desplazamiento explícito y traducción de errores— vive en la capa
portable [`platform/common/mfs_vfs.c`](../common/mfs_vfs.c), idéntica a la que
usa el front-end FUSE de Linux. Así, un volumen formateado en Windows se lee y
escribe igual en Linux, y viceversa.

---

## 1. Contenido

| Elemento | Descripción |
|---|---|
| `matrixfs_winfsp.c` | Front-end WinFsp (envoltorio fino sobre `mfs_vfs`). |
| `service/matrixfs_automount.c` | Servicio de detección y automontaje con letra libre. |
| `CMakeLists.txt` | Compila los tres ejecutables con MSVC o clang-cl. |
| `build.ps1` | Compilación asistida; deja los binarios en `dist\<Config>\`. |
| `install.ps1`, `uninstall.ps1` | Instalación/desinstalación y registro del servicio. |
| `installer/matrixfs.iss` | Instalador Inno Setup (incluye el registro del servicio). |

---

## 2. Requisitos

**Compilación**

- Windows 10 (1809+) o Windows 11, x64
- Visual Studio 2019/2022 con herramientas de C++ **o** LLVM/clang-cl
- CMake ≥ 3.20
- **WinFsp 1.12+** con SDK (cabeceras `inc\`, bibliotecas `lib\`)

**Ejecución**

- **WinFsp 1.12+** (runtime). Descarga: <https://winfsp.dev>
  El instalador añade `…\WinFsp\bin` al `PATH`; sin eso el ejecutable termina con
  `0xC0000135` (`STATUS_DLL_NOT_FOUND`) y sin mensaje alguno.
- Permisos de administrador para montar volúmenes crudos (`\\.\X:`,
  `\\.\PhysicalDriveN`, `\\?\Volume{…}\`). Montar imágenes de fichero también
  funciona como usuario normal.

> **Compilador**: el front-end `matrixfs_winfsp.exe` **debe** compilarse con
> **MSVC o clang-cl**; las cabeceras del SDK de WinFsp usan intrínsecos de MSVC
> (`_ReadWriteBarrier`), `static_assert` y `#pragma warning`, que MinGW GCC no
> procesa. El CLI y el servicio de automontaje sí compilan con MinGW.
> Además, `<winfsp/winfsp.h>` debe incluirse **antes** que `<windows.h>` y sin
> `WIN32_LEAN_AND_MEAN`.

---

## 3. Compilación

```powershell
# Si WinFsp está en una ruta no estándar:
$env:WINFSP = "D:\SDK\WinFsp"

cd platform\windows
.\build.ps1                 # Release x64 -> dist\Release\*.exe
.\build.ps1 -Config Debug -Clean
```

Manualmente con CMake:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Artefactos:

| Binario | Función |
|---|---|
| `matrixfs_winfsp.exe` | Monta un volumen en una letra de unidad. |
| `matrixfs-ctl.exe` | Formatear, sondear, listar, verificar (sin WinFsp). |
| `matrixfs_automount.exe` | Servicio de automontaje. |

---

## 4. Crear un volumen

```powershell
# Imagen de fichero de 64 MiB (no requiere privilegios)
.\dist\Release\matrixfs-ctl.exe format D:\vol0.img --label DATOS

# Partición cruda (requiere administrador y destruye su contenido)
.\dist\Release\matrixfs-ctl.exe format \\.\X: --label DATOS
```

Comprobación y diagnóstico sin montar:

```powershell
.\matrixfs-ctl.exe probe  D:\vol0.img     # ¿es un volumen MatrixFS?
.\matrixfs-ctl.exe label  D:\vol0.img     # etiqueta de volumen
.\matrixfs-ctl.exe statfs D:\vol0.img     # capacidad total/libre/usada
.\matrixfs-ctl.exe verify D:\vol0.img --full
```

> **Límite de 4 GiB.** El núcleo direcciona con 32 bits, por lo que sólo se
> exponen los primeros 4 GiB del medio (véase `DOCS/known-limitations.md`).

---

## 5. Montaje manual

```powershell
# Montar en la unidad X: (el proceso queda en primer plano; Ctrl+C desmonta)
.\matrixfs_winfsp.exe D:\vol0.img X:

# Partición cruda, sólo lectura, con etiqueta mostrada en el Explorador
.\matrixfs_winfsp.exe \\.\PhysicalDrive2 Y: -o ro

# Traza de depuración de WinFsp por consola
.\matrixfs_winfsp.exe D:\vol0.img X: -d
```

Opciones:

| Opción | Efecto |
|---|---|
| `-o label=NOMBRE` | Etiqueta de volumen (se fija al formatear). |
| `-o format` | Formatea el medio antes de montar. |
| `-o ro` | Montaje de sólo lectura (el volumen se muestra como sólo lectura). |
| `-o ram=N` | Presupuesto RAM declarado en bytes (por defecto 262144 → modo Extended, chunk 4096). |
| `-o erase_unit=N` | Bloque de borrado en bytes. Por omisión se **deriva del tamaño del medio** (potencia de dos ≥ `tamaño / 128`, acotada a 4 MiB). **Debe coincidir con el valor usado al formatear.** |
| `-d` | Activa la traza de depuración de WinFsp. |

El nombre del volumen y la etiqueta aparecen en el Explorador; el espacio libre
y la capacidad se comunican mediante `GetVolumeInfo`, y los atributos y
tamaños mediante `FSP_FSCTL_FILE_INFO`.

---

## 6. Instalación en el sistema y automontaje

```powershell
# PowerShell como Administrador
.\install.ps1                    # copia en %ProgramFiles%\MatrixFS y registra el servicio
.\install.ps1 -AddToPath         # además, añade el directorio al PATH
```

El servicio **`MatrixFS-Automount`** se inicia con el sistema y, cada 3 s:

1. Enumera los volúmenes con `FindFirstVolumeW`/`FindNextVolumeW`.
2. Sondea cada uno con la capa VFS portable (lectura de los dos superblocks,
   512 B, sin escritura).
3. Si encuentra un volumen MatrixFS válido y aún no está montado, le
   asigna la primera letra de unidad libre desde `D:` y arranca
   `matrixfs_winfsp.exe` para ese volumen.
4. Libera la letra cuando el volumen desaparece o el proceso termina.

```powershell
Get-Service MatrixFS-Automount            # estado del servicio
Start-Service MatrixFS-Automount
Stop-Service  MatrixFS-Automount

# Pasada manual de detección (sin instalar el servicio):
.\matrixfs_automount.exe --scan

# Diagnóstico: mensajes del servicio en DebugView (Sysinternals)
```

Con esto, al conectar un medio con un volumen MatrixFS aparece
automáticamente una unidad nueva en «Este equipo».

### Instalador

```powershell
# Requiere Inno Setup 6 y los binarios en dist\Release
iscc installer\matrixfs.iss
# -> installer\Output\MatrixFS-Ultra-Setup-1.0.0.exe
```

El instalador copia los binarios, registra el servicio y arranca la unidad de
forma inmediata. Si se coloca `winfsp.msi` en `installer\redist\`, también
instala WinFsp silenciosamente cuando no está presente.

---

## 7. Resolución de problemas

| Síntoma | Causa probable / solución |
|---|---|
| El proceso termina al instante **sin ningún mensaje**, código `-1073741515` (`0xC0000135`) | No se encuentra `winfsp-x64.dll`: añada `C:\Program Files (x86)\WinFsp\bin` al `PATH` o copie la DLL junto al ejecutable. |
| `matrixfs: FspFileSystemCreate falló (0xC000000E)` | `STATUS_NO_SUCH_DEVICE`: el driver FSD de WinFsp no está cargado o la sesión no puede abrirlo. Ejecute desde una consola **elevada** y repare/instale WinFsp. |
| Errores en `winfsp.h` al compilar: `'PNTSTATUS' … no es válido` / `_ReadWriteBarrier` / `static_assert` | (a) `<winfsp/winfsp.h>` debe ir antes de `<windows.h>` y sin `WIN32_LEAN_AND_MEAN`; (b) el SDK de WinFsp **no compila con MinGW GCC**: use MSVC o clang-cl (`build.ps1` + Visual Studio). |
| `matrixfs: montaje de '…' falló: MFS_EIO` al usar `\\.\X:` | Falta ejecutar como administrador, o el volumen no existe. |
| No aparece ninguna unidad nueva | WinFsp no está instalado, el servicio no arranca o el medio no tiene un volumen MatrixFS válido. Compruebe con `matrixfs-ctl.exe probe`. |
| El servicio no arranca | Revise el Visor de eventos y `DebugView`; confirme que `matrixfs_winfsp.exe` está junto a `matrixfs_automount.exe`. |
| `no contiene un volumen MatrixFS válido` | Formatee con `matrixfs-ctl.exe format` o verifique con `probe`. |
| `montaje fallido: MFS_ENOTVIABLE` | El presupuesto RAM declarado es insuficiente para la geometría del medio; aumente `-o ram=N` (p. ej. 32768). |
| `CreateProcessW falló (740)` | El servicio no dispone de privilegios elevados; reinstálelo desde una consola elevada. |
| El Explorador muestra «acceso denegado» | Los permisos POSIX persistidos del volumen no conceden el acceso; ajuste con `chmod` (vía Linux) o monte con `-o ro` y copie los datos. |
| Sólo se ven los primeros 4 GiB | Límite por direccionamiento de 32 bits del núcleo (`DOCS/known-limitations.md`). |
| Windows pide formatear la unidad al conectarla | Windows no conoce el sistema de archivos (igual que con ext4); **no** formatee: instale WinFsp y el servicio, o use `mount` en Linux. |

Diagnóstico rápido:

```powershell
.\matrixfs-ctl.exe probe  D:\vol0.img
.\matrixfs-ctl.exe verify D:\vol0.img
Get-Service MatrixFS-Automount | Format-List *
```

---

## 8. Notas de implementación

- **Reentrancia.** El núcleo MatrixFS usa una instancia global (tablas L2P,
  pools CFX, extents de desbordamiento) y **no es reentrante**. Todas las
  llamadas al núcleo se serializan con un mutex dentro de `mfs_vfs`, por lo que
  el front-end puede atender peticiones concurrentes de WinFsp sin riesgo. Se
  usa la estrategia de guarda `FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE`.
- **Un volumen por proceso.** Consecuencia de la instancia única: cada montaje
  (cada letra de unidad) tiene su propio proceso `matrixfs_winfsp.exe` y, por
  tanto, su propia instancia del núcleo.
- **Volúmenes crudos.** El driver de bloque abre los dispositivos con
  `CreateFileW` (necesario para las rutas `\\?\Volume{…}\`), obtiene la
  geometría con `IOCTL_DISK_GET_LENGTH_INFO` y realiza toda la E/S **alineada a
  sector** (lectura-modificación-escritura), tal y como exige Windows sobre
  handles de volumen.
- **Nombres.** WinFsp entrega rutas UTF-16; el front-end las convierte a UTF-8
  con separadores `/` antes de entregarlas al núcleo, de modo que la semántica
  de nombres es la misma que en Linux.
- **Atributos.** El atributo «sólo lectura» de Windows se refleja en los bits
  POSIX de escritura del volumen (persistidos on-flash), de forma que la
  información sobrevive al desmontaje y es coherente entre ambas plataformas.
- **Interrupción del servicio.** Al detener el servicio se termina el proceso
  de montaje; el núcleo es *crash-only* y recupera el último estado consistente
  en el siguiente montaje (validado en la suite de pruebas).
