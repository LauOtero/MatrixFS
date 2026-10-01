# MatrixFS Ultra «ATLAS» — Integración con Linux (FUSE 3)

Documento oficial de integración de MatrixFS Ultra con Linux. Cubre
compatibilidad, requisitos de compilación, instalación, configuración, montaje
(manual, `/etc/fstab` y automático), resolución de problemas, validación y
detalles de implementación.

- Código y recursos: [`platform/linux/`](../platform/linux/) y
  [`platform/common/`](../platform/common/)
- Guía rápida: [`platform/linux/README.md`](../platform/linux/README.md)

---

## 1. Modelo de integración

MatrixFS Ultra se implementa en **espacio de usuario** mediante **libfuse 3**.
No requiere módulos del kernel, parches ni recompilación: el kernel de Linux
sólo necesita disponer del soporte FUSE genérico (presente en la línea
principal desde 2.6.14 y habilitado por defecto en todas las distribuciones
modernas).

```
        ┌──────────────────────────────────────────────┐
        │  Aplicaciones / Explorador / shell           │
        └───────────────────────┬──────────────────────┘
                                │ VFS del kernel
        ┌───────────────────────▼──────────────────────┐
        │  /dev/fuse  (módulo fuse del kernel)         │
        └───────────────────────┬──────────────────────┘
                                │ protocolo FUSE 3
        ┌───────────────────────▼──────────────────────┐
        │  matrixfs_fuse   (platform/linux)            │
        │    └── mfs_vfs   (platform/common)  ← mutex  │
        │          └── mfs_blk  (driver L2, RMW)       │
        │                └── imagen | /dev/sdX | MTD   │
        └──────────────────────────────────────────────┘
```

La semántica del sistema de archivos (permisos POSIX persistidos, metadatos,
E/S con desplazamiento explícito, errores tipificados) reside en la capa
portable `platform/common/mfs_vfs.c`, **compartida con el front-end WinFsp de
Windows**. El front-end FUSE es un envoltorio fino: sólo traduce el ABI de FUSE
a llamadas `mfs_vfs_*`. Gracias a ello el comportamiento es idéntico en ambos
sistemas operativos y el *layout* on-flash es plenamente intercambiable.

---

## 2. Compatibilidad

### 2.1 Kernels

| Línea de kernel | Soporte | Notas |
|---|---|---|
| 5.4 LTS | ✅ | Base de Ubuntu 20.04, Debian 10/11 (backports), RHEL 8. |
| 5.10 LTS | ✅ | Debian 11, RHEL 9, SLES 15 SP3+. |
| 5.15 LTS | ✅ | Ubuntu 22.04, SLES 15 SP4+. |
| 6.x | ✅ | Ubuntu 22.04/24.04, Fedora 36+, Debian 12. |
| 7.x (previsto) | ✅ | Sin dependencias fuera de la API FUSE 3 estable. |

La integración **no depende de la versión del kernel** más allá de lo que
libfuse 3 abstrae internamente; el requisito real es disponer de:

- `/dev/fuse` accesible (o el módulo `fuse` cargable).
- Capacidad de montar por parte del usuario o del grupo `fuse`.

### 2.2 Bibliotecas y herramientas

| Componente | Mínimo | Paquete (Debian/Ubuntu · Fedora/RHEL · Arch) |
|---|---|---|
| libfuse3 (desarrollo) | 3.9 | `libfuse3-dev` · `fuse3-devel` · `fuse3` |
| Runtime fuse3 | 3.9 | `libfuse3-3` (Ubuntu) / `libfuse3-4` (Debian 13) · `fuse3-libs` · `fuse3` |
| Compilador C11 | gcc 8 / clang 10 | `gcc` · `gcc` · `base-devel` |
| make, pkg-config | — | `make pkg-config` |

> El paquete de runtime cambia de nombre según la distribución: Ubuntu 22.04/24.04
> usan `libfuse3-3` (SONAME `libfuse3.so.3`) y Debian 13 usa `libfuse3-4`
> (SONAME `libfuse3.so.4`, libfuse 3.17). El paquete `.deb` declara ambas
> alternativas (`Depends: libfuse3-3 | libfuse3-4`); al compilar, la biblioteca
> se localiza siempre mediante `pkg-config fuse3`.

### 2.3 Requisito de compilación: extensiones POSIX

El proyecto se compila con **C11 estricto** (`-std=c11`), lo que en glibc oculta
las declaraciones POSIX (`pthread_*`, `clock_gettime`, `pread`/`pwrite`,
`nanosleep`, `strdup`, `UTIME_NOW`). Los ficheros que las usan solicitan el
rasgo explícitamente al principio del fichero:

```c
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
```

Por eso **no es necesario** añadir `-D_GNU_SOURCE` ni `-D_DEFAULT_SOURCE` al
compilar; basta con las banderas que ya genera el `Makefile`.

### 2.4 Medios soportados

| Medio | Ruta típica | Notas |
|---|---|---|
| Imagen de fichero | `/var/lib/matrixfs/vol0.img` | Acceso byte-directo; ideal para pruebas, contenedores y CI. |
| Partición de bloque | `/dev/sdb1`, `/dev/nvme0n1p2` | E/S alineada a sector con RMW transparente. |
| Disco completo | `/dev/sdb`, `/dev/nvme0n1` | Igual que la partición. |
| Memoria flash cruda (MTD) | `/dev/mtd0` | Se usa `MEMGETINFO`/`MEMERASE`; geometría real del chip. |
| MTD por bloque | `/dev/mtdblock0` | Se trata como dispositivo de bloque. |

> **Límite de 4 GiB.** El núcleo direcciona con 32 bits; sólo se exponen los
> primeros 4 GiB del medio. Véase
> [`known-limitations.md`](known-limitations.md).

---

## 3. Estructura de carpetas

```
platform/
├── common/                     # Capa de integración portable (Linux + Windows)
│   ├── mfs_plat.{h,c}          #   Mutex y tiempo portables
│   ├── mfs_blk.{h,c}           #   Driver L2: imagen / /dev/sdX / MTD (RMW alineado)
│   ├── mfs_vfs.{h,c}           #   Adaptador VFS: montaje, formato, E/S, permisos
│   └── mfs_vfsctl.c            #   CLI de validación (format/probe/ls/cat/…)
└── linux/                      # Todo lo específico de Linux
    ├── matrixfs_fuse.c         #   Front-end FUSE 3
    ├── Makefile                #   Build e install (PREFIX/DESTDIR)
    ├── scripts/
    │   ├── matrixfs-mkfs       #   Crear y formatear volúmenes
    │   ├── matrixfs-mount      #   Helper de /etc/fstab (/sbin/mount.matrixfs)
    │   └── matrixfs-probe      #   Sondeo para udev (salida clave=valor)
    ├── systemd/matrixfs@.service  # Montaje por dispositivo
    ├── udev/99-matrixfs.rules     # Detección y automontaje
    ├── fstab.example           #   Ejemplos de /etc/fstab
    ├── install.sh, uninstall.sh   # Instalación asistida
    └── packaging/{deb,rpm}     #   Empaquetado
```

---

## 4. Requisitos de compilación

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libfuse3-dev

# Fedora / RHEL / openSUSE
sudo dnf install gcc make pkgconf-pkg-config fuse3-devel

# Arch
sudo pacman -S base-devel pkgconf fuse3
```

Compilación:

```sh
cd platform/linux
make check-fuse          # informa de la versión de libfuse3 detectada
make                     # -> build/matrixfs_fuse y build/matrixfs-ctl
make strict              # idéntico con -Werror
```

`pkg-config` es la vía preferente para localizar libfuse3; si no está
disponible, el Makefile asume `-I/usr/include/fuse3 -lfuse3`.

---

## 5. Instalación

### 5.1 Instalación directa

```sh
cd platform/linux
sudo make install                     # PREFIX=/usr/local (por defecto)

# Instalación de sistema (FHS), recomendada para paquetes:
sudo make install PREFIX=/usr

# Instalación en un árbol de paquete:
make install PREFIX=/usr DESTDIR=/tmp/stage
```

Instalación asistida (incluye recarga de systemd/udev y comprobación de
`/etc/fuse.conf`):

```sh
sudo ./install.sh                     # PREFIX=/usr/local
sudo PREFIX=/usr ./install.sh
```

### 5.2 Contenido instalado

| Ruta | Elemento |
|---|---|
| `$PREFIX/bin/matrixfs_fuse` | Front-end FUSE 3 |
| `$PREFIX/bin/matrixfs-ctl` | CLI de administración y validación |
| `$PREFIX/sbin/mount.matrixfs` | Helper de montaje para `/etc/fstab` |
| `$PREFIX/sbin/matrixfs-mkfs` | Creación/formateo de volúmenes |
| `$PREFIX/lib/matrixfs/matrixfs-probe` | Sondeo para udev |
| `$PREFIX/lib/systemd/system/matrixfs@.service` | Unidad de montaje |
| `$PREFIX/lib/udev/rules.d/99-matrixfs.rules` | Detección/automontaje |
| `$PREFIX/share/doc/matrixfs/` | `fstab.example`, `linux-integration.md` |

Con `PREFIX=/usr`, las rutas coinciden con las de systemd y udev del sistema
(`/usr/lib/systemd/system`, `/usr/lib/udev/rules.d`). En distribuciones con
`/sbin` separado, `install.sh` avisa para enlazar
`/sbin/mount.matrixfs → /usr/sbin/mount.matrixfs` (el paquete DEB lo hace en su
`postinst`).

### 5.3 Paquetes

```sh
# DEB (requiere dpkg-dev)
sudo apt install libfuse3-dev dpkg-dev
./packaging/deb/build-deb.sh
# -> packaging/deb/matrixfs-ultra_1.0.0_<arch>.deb

# RPM
rpmbuild -bb --define "_sourcedir $PWD/../.." packaging/rpm/matrixfs.spec
```

Ambos paquetes instalan con `PREFIX=/usr`, registran el helper, la unidad
systemd y la regla udev, y ejecutan las recargas necesarias en sus scripts de
mantenimiento.

---

## 6. Creación de volúmenes

```sh
matrixfs-mkfs -L DATOS /dev/sdb1            # partición (destruye su contenido)
matrixfs-mkfs -L VOL0 -s 64M vol0.img       # crea una imagen y la formatea
sudo matrixfs-mkfs -L NAND -e 65536 /dev/mtd0   # MTD con erase unit de 64 KiB
```

Opciones de `matrixfs-mkfs`: `-L etiqueta`, `-s tamaño`, `-e erase_unit`
(por omisión el bloque de borrado se **deriva** del tamaño del medio, véase §7.2).
El CLI equivalente (más opciones) es:

```sh
matrixfs-ctl format <dispositivo> [--label L] [--erase-unit N] [--size-limit N]
                                   [--ram N] [--key HEX]
```

Diagnóstico sin montar:

```sh
matrixfs-ctl probe  <dispositivo>          # ¿es un volumen MatrixFS?
matrixfs-ctl label  <dispositivo>          # etiqueta de volumen
matrixfs-ctl statfs <dispositivo>          # capacidad total/libre/usada
matrixfs-ctl verify <dispositivo> [--full] # verificación de integridad
```

---

## 7. Montaje

### 7.1 Manual

```sh
sudo mkdir -p /mnt/datos
sudo mount -t matrixfs /dev/sdb1 /mnt/datos
sudo umount /mnt/datos
```

Directamente con el front-end (permite todas las opciones):

```sh
sudo matrixfs_fuse /dev/sdb1 /mnt/datos -f \
     -o uid=1000,gid=1000,fmask=644,dmask=755,allow_other
```

### 7.2 Opciones

| Opción | Efecto |
|---|---|
| `-o device=PATH` | Dispositivo (alternativa al argumento posicional). |
| `-o label=NOMBRE` | Etiqueta de volumen (se aplica al formatear; al montar es informativa). |
| `-o format` | Formatea el medio antes de montar. |
| `-o ro` | Montaje de sólo lectura (toda escritura devuelve `EROFS`). |
| `-o uid=N` `-o gid=N` | Propietario por defecto de los nodos nuevos. |
| `-o fmask=OCT` `-o dmask=OCT` | Permisos por defecto de ficheros y directorios (octal, p. ej. `644`, `755`). |
| `-o ram=N` | Presupuesto RAM declarado en bytes (por defecto 262144 → modo Extended, chunk 4096). |
| `-o erase_unit=N` | Bloque de borrado en bytes. Por omisión se **deriva del tamaño del medio** (potencia de dos ≥ `tamaño / 128`, acotada a 4 MiB) para que la ventana de 128 zonas ZLF lo cubra completo: un volumen de 64 MiB usa 512 KiB (125 zonas útiles). **Debe coincidir con el valor usado al formatear**: cambiarlo altera el layout (`SB B` y el origen de zonas dependen de él). |
| `-o allow_other` | Acceso por otros usuarios (requiere `user_allow_other` en `/etc/fuse.conf`). |
| `-o allow_root` | Acceso por root además del usuario que monta. |
| `-o nonempty` | Permitir montar sobre un directorio no vacío. |
| `-o auto_unmount` | Desmontar automáticamente al terminar el proceso FUSE. |

El montaje se realiza con `-o default_permissions`, de modo que el kernel
aplica los permisos POSIX que devuelve `getattr`. Los permisos **se persisten
en el volumen**: un `chmod`/`chown` sobrevive al desmontaje, al remontaje y al
traslado del medio a otro equipo (incluido Windows).

### 7.3 Montaje automático con `/etc/fstab`

```
/dev/sdb1  /mnt/datos  matrixfs  defaults,uid=1000,gid=1000,fmask=644,dmask=755  0 0
/dev/sdc1  /mnt/arch   matrixfs  ro,allow_other,nofail                          0 0
```

`mount /mnt/datos` (o `mount -a` en el arranque) delega en
`/sbin/mount.matrixfs`, que filtra las opciones de libro mayor (`defaults`,
`noauto`, `user`, `nofail`, `_netdev`, `x-systemd.*`) y traduce el resto al
front-end FUSE. Véase [`platform/linux/fstab.example`](../platform/linux/fstab.example).

### 7.4 Montaje automático con udev + systemd

Al conectar un medio (o al arrancar con él presente):

1. `99-matrixfs.rules` invoca `matrixfs-probe <devnode>`, que lee los dos
   superblocks (512 B) y valida su CRC — operación de sólo lectura y coste
   despreciable.
2. Si el volumen es válido, udev publica `ENV{MATRIXFS_VOLUME}=1`, marca
   `ENV{ID_FS_TYPE}="matrixfs"` (para que udisks/GNOME/KDE lo identifiquen) y
   solicita `matrixfs@<dispositivo>.service`.
3. La unidad monta el volumen en `/run/media/matrixfs/<dispositivo>` con
   `allow_other` y `default_permissions`.

```sh
systemctl status matrixfs@sdb1.service
systemctl start  matrixfs@sdb1.service
systemctl stop   matrixfs@sdb1.service
journalctl -u    matrixfs@sdb1.service -n 50
```

Al extraer el medio, `BindsTo=dev-%i.device` detiene y desmonta la unidad.

Requiere permiso multiusuario:

```sh
echo user_allow_other | sudo tee -a /etc/fuse.conf
sudo systemctl daemon-reload
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=block
```

---

## 8. Resolución de problemas

| Síntoma | Diagnóstico y solución |
|---|---|
| `matrixfs_fuse: no se encuentra` | Compile e instale (`make && sudo make install`) o defina `MATRIXFS_FUSE=/ruta/al/binario`. |
| `mount: unknown filesystem type 'matrixfs'` | Falta el helper: compruebe `$PREFIX/sbin/mount.matrixfs` y el enlace en `/sbin` con `PREFIX=/usr`. |
| `fuse: device not found, try 'modprobe fuse' first` | Cargue el módulo (`sudo modprobe fuse`) o exponga `/dev/fuse` en el contenedor (`--device /dev/fuse --cap-add SYS_ADMIN`). |
| `On calling fusermount posix_spawn failed: No such file or directory` (código de salida 4) | Falta el binario `fusermount3` (paquete `fuse3`). Sin él, libfuse sólo puede montar como root usando `mount(2)`. Instale `fuse3` o monte con `sudo`. |
| `fuse: failed to open /dev/fuse: Permission denied` | Añada el usuario al grupo `fuse` (`sudo usermod -aG fuse $USER`) y reinicie la sesión, o monte con `sudo`. |
| `allow_other` ignorado | Añada `user_allow_other` a `/etc/fuse.conf`. |
| `'…' no contiene un volumen MatrixFS válido` | Formatee con `matrixfs-mkfs` o compruebe con `matrixfs-ctl probe`. |
| `montaje fallido: MFS_ENOTVIABLE` | Presupuesto RAM declarado insuficiente para la geometría del medio: aumente `-o ram=N` (el valor de host por defecto es 262144). No lo reduzca: el presupuesto determina el **modo** y, con él, el tamaño de página del layout. |
| `montaje fallido: MFS_EARCH` | Se exige una arquitectura de 16/32 bits y geometría que reserve al menos un sector por estructura crítica. |
| `verify` devuelve error tras un corte de energía | Ejecute `matrixfs-ctl verify --full`; el núcleo es *crash-only* y recupera el último estado consistente, pero conviene confirmar la integridad. |
| Integridad tras extraer el medio en caliente | Desmonte siempre antes de extraer (`umount`); si se extrae en caliente, remonte y ejecute `verify`. |
| Sólo se ven los primeros 4 GiB | Límite por direccionamiento de 32 bits del núcleo (`known-limitations.md`). |
| Escrituras muy lentas en MTD | Ajuste `-e` al tamaño de bloque de borrado real del chip y verifique que `MEMERASE` funciona (`dmesg`). |

Diagnóstico:

```sh
dmesg | tail -30
matrixfs-ctl verify /dev/sdb1
cat /proc/mounts | grep matrixfs
ls -l /dev/fuse
```

---

## 9. Validación realizada

Compilación y ejecución **reales sobre Linux** (Debian 13 «trixie», kernel
6.18.33 en WSL2, gcc 14.2.0, GNU make 4.4.1, libfuse3 3.17.2, `fusermount3`
presente, sesión root):

| Prueba | Comando | Resultado |
|---|---|---|
| Compilación estricta de todo el proyecto | `make CFLAGS='-std=c11 -Wall -Wextra -Werror -O2 …' all` | ✅ 0 avisos, 0 errores (`libmatrixfs.a`, `mfs_tests.exe`, `mfstool.exe`, `mfsctl.exe`) |
| Suite completa del proyecto **en Linux** | `make test` | ✅ **935 checks / 0 fallos** (incluye `test_vfs_portable`, la ruta que usan FUSE y WinFsp) |
| Ausencia de heap en el núcleo | `make check-map` | ✅ `PASS: sin asignación dinámica en el núcleo (MFS-RES-001/002)` |
| Compilación del front-end y CLI | `cd platform/linux && make strict` | ✅ `-Werror` limpio enlazando `libfuse3.so.4` real |
| Instalación real | `sudo make install PREFIX=/usr` | ✅ binarios, `mount.matrixfs`, unidad systemd, regla udev y documentación en su sitio |
| **Montaje real vía `mount -t matrixfs`** | `mount -t matrixfs /var/lib/matrixfs/vol0.img /mnt/mfs` | ✅ aparece en `/proc/mounts` como `fuse.matrixfs`; `df` reporta 63 MiB de capacidad y 62 MiB libres |
| Operaciones nativas | `mkdir`/`echo`/`chmod`/`chown`/`dd` (24 MiB)/`cp`/`cmp`/`truncate`/`mv`/`rm`/`rmdir` | ✅ todas correctas; el fichero de 24 MiB y su copia son **idénticos byte a byte** y el truncado conserva el prefijo |
| Persistencia | `umount` + `mount -t matrixfs` | ✅ `md5` del fichero de 24 MiB, permisos (`600`), propietario (`1000:1000`) y contenido intactos; `verify --full` = `MFS_OK` |
| Montaje de sólo lectura | `mount -t matrixfs -o ro` | ✅ toda escritura rechazada con `EROFS` |
| Interoperabilidad con Windows | volumen de 64 MiB creado y escrito en Linux, leído por `matrixfs-ctl.exe` (MSVC) | ✅ etiqueta/modo/`statfs`/`ls`/`cat`/`verify --full` correctos y **md5 del binario de 4 MiB coincidente** |
| Scripts de integración | `sh -n` sobre los 8 scripts (mkfs, mount, probe, install/uninstall, packaging) | ✅ sintaxis válida |

Resultado global del guion funcional sobre el punto de montaje real (39
comprobaciones): **39 OK / 0 fallos**, reproducible en ejecuciones sucesivas.

Defectos reales detectados y corregidos durante esta validación:

1. `erase_unit` incoherente: el script `matrixfs-mkfs` y el front-end FUSE
   fijaban 4096 mientras el CLI derivaba otro valor ⇒ cada uno montaba el
   volumen con un **layout distinto** (pérdida de etiqueta y de datos). Ahora la
   geometría se deriva de forma determinista del tamaño del medio y ningún
   binario la fuerza.
2. Límite de capacidad: el L2P de fábrica (4096 entradas) y la tabla de
   extents (256) limitaban un "volumen de 64 MiB" a ~512 KiB útiles. Se amplía el
   L2P en los builds de host y se elimina la tabla redundante (véase §7.2 y
   `known-limitations.md`).
3. **Corrupción silenciosa de páginas en la zona 0**: `mfs_extent_read`
   devolvía `0` para "sin página física", pero el ppage 0 es válido (zona 0,
   página 0), de modo que esas páginas se leían como ceros. Se introduce el
   centinela `MFS_L2P_FREE`/`MFS_PPAGE_ERROR` (`DOCS/core.md`).
4. **Estado obsoleto tras remontar**: la reconstrucción aplicaba los registros
   en orden de índice de zona, que deja de ser cronológico en cuanto la GC
   reutiliza zonas de índice bajo. Ahora se ordena por `z->seq` (y `fs->seq` se
   restaura del superblock), conservando además los inodos más recientes en la
   ventana RAM por recencia (`DOCS/zone.md`, `DOCS/core.md`).
5. `make test`/`make check-map` fallaban en POSIX por invocar los binarios sin
   `./`; corregido en el `Makefile` raíz.

---

## 10. Detalles de implementación

- **Reentrancia.** El núcleo MatrixFS mantiene estado global (mapa L2P, pools
  CFX, ventana WAL, ventana de inodos) y **no es reentrante**: admite
  una única instancia por proceso. El adaptador `mfs_vfs` serializa todas las
  llamadas al núcleo con un mutex, de modo que el front-end FUSE (multihilo por
  defecto) es seguro. Cada punto de montaje usa su propio proceso
  `matrixfs_fuse` y, por tanto, su propia instancia.
- **Puerto de plataforma.** Se reutiliza `sim/mfs_port_host.c` (contador
  monotónico y tiempo de pared). Sus primitivas de sección crítica son vacías
  porque la exclusión mutua efectiva la proporciona el mutex del adaptador VFS.
- **E/S alineada.** `mfs_blk` detecta si el destino exige alineación a sector
  (`BLKSSZGET` en Linux, `IOCTL_STORAGE_QUERY_PROPERTY` en Windows) y, en ese
  caso, realiza lectura-modificación-escritura del sector completo. Con ello la
  programación de granularidad fina (NOR, 1 byte) se emula de forma
  transparente y el layout on-flash es idéntico en todas las plataformas.
- **Permisos.** `mode`, `uid` y `gid` se persisten en el registro INODE
  on-flash (extensión de 12 B al final del registro, leída de forma tolerante
  para mantener compatibilidad con volúmenes anteriores). `chmod`/`chown`
  (FUSE) y los atributos de Windows (WinFsp) modifican los mismos bits.
- **Etiqueta de volumen.** Se almacena en el superblock (offset 123, fuera del
  CRC y del MAC para no romper la compatibilidad de los slots existentes) y se
  lee del **slot ganador** por `{época, seq}`, igual que el resto del
  superblock.
- **Caché.** El front-end activa la caché de página del kernel
  (`cfg->kernel_cache`) con tiempos de validez de atributos y entradas de 1 s.
  El núcleo mantiene además su propia caché L2P y ventana WAL.
- **Errores.** `mfs_vfs_errno` traduce los 29 estados tipificados del núcleo a
  `errno` POSIX; el front-end devuelve `-errno`, que es lo que el kernel de
  Linux propaga a la aplicación.

---

## 11. Referencias

- [`DOCS/windows-integration.md`](windows-integration.md) — integración
  equivalente en Windows (WinFsp), con el mismo *layout* on-flash.
- [`DOCS/architecture.md`](architecture.md) — capas del núcleo y layout físico.
- [`DOCS/testing.md`](testing.md) — plan de verificación y resultados.
- [`DOCS/known-limitations.md`](known-limitations.md) — límites y desviaciones.
- libfuse: <https://github.com/libfuse/libfuse>
