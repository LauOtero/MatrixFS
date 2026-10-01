# MatrixFS «ATLAS» — soporte Linux (FUSE 3)

Front-end Linux de MatrixFS. Implementa el sistema de archivos en
espacio de usuario mediante **libfuse 3**, de modo que no requiere módulos del
kernel ni parches: *cualquier* kernel con FUSE (5.4+, 6.x y 7.x) puede montar
volúmenes MatrixFS.

Toda la semántica de sistema de archivos —permisos POSIX persistidos, metadatos,
E/S con desplazamiento explícito y traducción de errores— vive en la capa
portable [`platform/common/mfs_vfs.c`](../common/mfs_vfs.c), idéntica a la que
usa el front-end WinFsp de Windows. Este directorio sólo contiene el envoltorio
FUSE y los recursos de integración con el sistema.

---

## 1. Contenido

| Elemento | Descripción |
|---|---|
| `matrixfs_fuse.c` | Front-end FUSE 3 (envoltorio fino sobre `mfs_vfs`). |
| `Makefile` | Compila `matrixfs_fuse` y `matrixfs-ctl`; instala todo. |
| `scripts/matrixfs-mkfs` | Crea y formatea un volumen (imagen o dispositivo). |
| `scripts/matrixfs-mount` | Helper de montaje para `/etc/fstab` (`/sbin/mount.matrixfs`). |
| `scripts/matrixfs-probe` | Sondeo para udev (salida `clave=valor`). |
| `systemd/matrixfs@.service` | Unidad de montaje por dispositivo. |
| `udev/99-matrixfs.rules` | Detección y automontaje al conectar el medio. |
| `fstab.example` | Ejemplos de entradas de `/etc/fstab`. |
| `install.sh`, `uninstall.sh` | Instalación/desinstalación asistida. |
| `packaging/deb`, `packaging/rpm` | Empaquetado para Debian/Ubuntu y Fedora/RHEL. |

---

## 2. Requisitos

**Compilación**

- `gcc` (o `clang`) con soporte C11
- `make`, `pkg-config`
- `libfuse3-dev` (Debian/Ubuntu), `fuse3-devel` (Fedora/RHEL), `fuse3` (Arch)

**Ejecución**

- `libfuse3` — el paquete de runtime cambia de nombre según la distribución:
  `libfuse3-3` (Ubuntu 22.04/24.04) o `libfuse3-4` (Debian 13, libfuse 3.17)
- `fuse3` (aporta el binario `fusermount3`, necesario para montar sin ser root)
- Grupo `fuse` (en distribuciones que lo usan) para montajes de usuario
- `systemd` y `udev` sólo para el automontaje; el montaje manual no los necesita

**Kernels soportados.** Cualquier kernel ≥ 5.4 con soporte FUSE (incluido en el
kernel estándar desde 2.6.14) funciona. Se ha validado la integración con la
API de FUSE 3, que no depende de la versión del kernel más allá de lo que
libfuse3 gestione internamente.

---

## 3. Compilación e instalación

```sh
cd platform/linux
make                    # -> build/matrixfs_fuse y build/matrixfs-ctl
sudo make install       # instala en /usr/local (PREFIX configurable)
```

Instalación de sistema (FHS) o mediante el script asistido:

```sh
sudo PREFIX=/usr make install
# o bien:
sudo ./install.sh               # PREFIX=/usr/local por defecto
```

Empaquetado:

```sh
sudo apt install libfuse3-dev dpkg-dev
./packaging/deb/build-deb.sh          # -> matrixfs-ultra_1.0.0_<arch>.deb

rpmbuild -bb --define "_sourcedir $PWD/../.." packaging/rpm/matrixfs.spec
```

---

## 4. Crear un volumen

```sh
# Partición dedicada (se destruye su contenido):
sudo matrixfs-mkfs -L DATOS /dev/sdb1

# Imagen de fichero de 64 MiB (útil para pruebas y contenedores):
matrixfs-mkfs -L VOL0 -s 64M /var/lib/matrixfs/vol0.img

# Memoria flash cruda (MTD):
sudo matrixfs-mkfs -L NAND /dev/mtd0
```

Comprobación y diagnóstico sin montar:

```sh
matrixfs-ctl probe /dev/sdb1        # ¿es un volumen MatrixFS?
matrixfs-ctl label /dev/sdb1        # etiqueta de volumen
matrixfs-ctl statfs /dev/sdb1       # capacidad total/libre/usada
matrixfs-ctl verify /dev/sdb1 --full  # verificación de integridad
```

> **Límite de 4 GiB.** El núcleo direcciona con 32 bits, por lo que sólo se
> exponen los primeros 4 GiB del medio (véase `DOCS/known-limitations.md`).

---

## 5. Montaje manual

```sh
sudo mkdir -p /mnt/datos
sudo mount -t matrixfs /dev/sdb1 /mnt/datos
# o directamente con el front-end:
sudo matrixfs_fuse /dev/sdb1 /mnt/datos -f -o uid=1000,gid=1000,allow_other

sudo umount /mnt/datos
```

Opciones admitidas por `matrixfs_fuse` (además de las estándar de FUSE):

| Opción | Efecto |
|---|---|
| `-o label=NOMBRE` | Etiqueta informativa del volumen (no la modifica al montar). |
| `-o format` | Formatea el medio antes de montar. |
| `-o ro` | Montaje de sólo lectura. |
| `-o uid=N,gid=N` | Propietario por defecto de los nodos nuevos. |
| `-o fmask=OCT,dmask=OCT` | Permisos por defecto de ficheros/directorios (p. ej. `644`, `755`). |
| `-o ram=N` | Presupuesto RAM declarado en bytes (por defecto 262144 → modo Extended, chunk 4096). |
| `-o erase_unit=N` | Bloque de borrado en bytes. Por omisión se **deriva del tamaño del medio** (potencia de dos ≥ `tamaño / 128`, acotada a 4 MiB) para que la ventana de 128 zonas ZLF lo cubra completo. **Debe coincidir con el valor usado al formatear.** |
| `-o allow_other` | Permite el acceso a otros usuarios (requiere `user_allow_other` en `/etc/fuse.conf`). |
| `-o allow_root` | Permite el acceso a root además del usuario que monta. |

El montaje se realiza con `default_permissions`, de modo que el kernel aplica
los permisos POSIX (`mode`/`uid`/`gid`) que devuelve `getattr`. Los permisos se
persisten **en el propio volumen**: un `chmod` o `chown` sobrevive a un
desmontaje/remontaje y a la conexión del medio en otro equipo.

---

## 6. Montaje automático con `/etc/fstab`

Añada una línea como (véase `fstab.example` para más variantes):

```
/dev/sdb1  /mnt/datos  matrixfs  defaults,uid=1000,gid=1000,fmask=644,dmask=755  0 0
```

y monte con `mount /mnt/datos`. El helper `/sbin/mount.matrixfs` se encarga de
traducir las opciones al front-end FUSE. `mount -a` en el arranque funciona
siempre que el dispositivo esté presente; añada `nofail` si no lo está.

---

## 7. Montaje automático por udev/systemd

Al conectar un medio con un volumen MatrixFS válido, la regla
`99-matrixfs.rules` sondea el dispositivo (lectura de 512 B) y pide a systemd
que arranque `matrixfs@<dispositivo>.service`, que lo monta en
`/run/media/matrixfs/<dispositivo>`.

```sh
systemctl status  matrixfs@sdb1.service
systemctl start   matrixfs@sdb1.service
systemctl stop    matrixfs@sdb1.service
journalctl -u     matrixfs@sdb1.service
```

Para que un volumen sea accesible por usuarios distintos del que arranca el
servicio, añada a `/etc/fuse.conf`:

```
user_allow_other
```

Tras instalar o modificar unidades y reglas:

```sh
sudo systemctl daemon-reload
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=block
```

---

## 8. Resolución de problemas

| Síntoma | Causa probable / solución |
|---|---|
| `no se encuentra matrixfs_fuse` | Compile e instale (`make && sudo make install`) o exporte `MATRIXFS_FUSE=/ruta/a/matrixfs_fuse`. |
| `mount: opción desconocida` al usar `mount -t matrixfs` | Falta `/sbin/mount.matrixfs`; compruebe `make install` o el enlace `/sbin → /usr/sbin`. |
| `fuse: device not found` | El módulo `fuse` no está cargado o el contenedor no tiene `/dev/fuse`. Ejecute `sudo modprobe fuse` y exponga `/dev/fuse` (p. ej. `--device /dev/fuse`). |
| `On calling fusermount posix_spawn failed: No such file or directory` (salida 4) | Falta el binario `fusermount3`: instale el paquete `fuse3`. Sin él, libfuse sólo puede montar siendo root mediante `mount(2)`. |
| `Permission denied` al montar | Añada su usuario al grupo `fuse`, o monte con `sudo`. |
| `allow_other` no tiene efecto | Añada `user_allow_other` a `/etc/fuse.conf`. |
| `'...' no contiene un volumen MatrixFS válido` | Formatee con `matrixfs-mkfs` o verifique con `matrixfs-ctl probe`. |
| `montaje fallido: MFS_ENOTVIABLE` | El presupuesto RAM declarado es insuficiente para la geometría del medio; aumente `-o ram=N` (p. ej. 32768). |
| El medio pierde datos tras un corte de energía | Es esperado que el sistema recupere el último estado consistente; ejecute `matrixfs-ctl verify` para confirmar la integridad. |
| Sólo se ven los primeros 4 GiB | Límite por direccionamiento de 32 bits del núcleo (`DOCS/known-limitations.md`). |

Diagnóstico rápido:

```sh
dmesg | tail -20                                  # errores del kernel/libfuse
journalctl -u matrixfs@sdb1.service -n 50         # errores del montaje automático
matrixfs-ctl verify /dev/sdb1                     # integridad del volumen
```

---

## 9. Notas de implementación

- **Reentrancia.** El núcleo MatrixFS usa una instancia global (tablas L2P,
  pools CFX, extents de desbordamiento) y **no es reentrante**. Todas las
  llamadas al núcleo se serializan con un mutex dentro de `mfs_vfs`, por lo que
  el front-end puede recibir peticiones concurrentes de FUSE sin riesgo.
- **Puerto de plataforma.** Se reutiliza `sim/mfs_port_host.c` (contador
  monotónico y tiempo). Sus primitivas de sección crítica son vacías porque la
  serialización efectiva la realiza el mutex del adaptador VFS.
- **Un volumen por proceso.** Consecuencia de la instancia única del núcleo:
  cada punto de montaje tiene su propio proceso `matrixfs_fuse` y, por tanto,
  su propia instancia.
- **Layout on-flash idéntico entre sistemas.** El driver de bloque
  (`platform/common/mfs_blk.c`) emula la programación de granularidad fina
  mediante lectura-modificación-escritura alineada a sector cuando el destino
  lo exige, de modo que una imagen creada en Linux es legible en Windows y
  viceversa.
