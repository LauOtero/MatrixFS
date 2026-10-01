#!/bin/sh
# install.sh — compila e instala el soporte Linux (FUSE 3) de MatrixFS Ultra.
#
# Uso:
#   sudo ./install.sh                 # instala en /usr/local (por defecto)
#   sudo PREFIX=/usr ./install.sh     # instalación de sistema (paquetes)
#
# Pasos:
#   1. Comprueba dependencias (gcc, make, libfuse3).
#   2. Compila matrixfs_fuse y matrixfs-ctl.
#   3. Instala binarios, mount helper, unidad systemd y regla udev.
#   4. Recarga systemd/udev y avisa de la configuración de /etc/fuse.conf.

set -eu

PREFIX=${PREFIX:-/usr/local}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

msg() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "se requieren privilegios de root (use sudo)."

command -v make >/dev/null 2>&1 || die "no se encuentra 'make'."
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || \
    die "no se encuentra un compilador de C (gcc/cc)."

pkg-config --exists fuse3 2>/dev/null || \
    msg "AVISO: pkg-config no encuentra fuse3. Instale libfuse3-dev (Debian/Ubuntu),
       fuse3-devel (Fedora/RHEL) o fuse3 (Arch) si la compilación falla."

cd "$SCRIPT_DIR"

msg "==> Compilando (PREFIX=$PREFIX)"
make clean >/dev/null 2>&1 || true
make PREFIX="$PREFIX"

msg "==> Instalando"
make install PREFIX="$PREFIX"

# mount(8) sólo busca los helpers en /sbin y /usr/sbin. Con PREFIX=/usr el
# helper ya queda en /usr/sbin; con otros prefijos se crea un enlace para que
# 'mount -t matrixfs' y las entradas de /etc/fstab funcionen.
if [ "$PREFIX" != "/usr" ] && [ -x "$PREFIX/sbin/mount.matrixfs" ]; then
    if [ ! -e /sbin/mount.matrixfs ]; then
        ln -sf "$PREFIX/sbin/mount.matrixfs" /sbin/mount.matrixfs
        msg "==> Enlace creado: /sbin/mount.matrixfs -> $PREFIX/sbin/mount.matrixfs"
    fi
fi

msg "==> Recargando systemd y udev"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=block || true
fi

# El automontaje con allow_other requiere habilitarlo explícitamente.
if [ -f /etc/fuse.conf ]; then
    if ! grep -q '^[[:space:]]*user_allow_other' /etc/fuse.conf; then
        msg ""
        msg "AVISO: para el automontaje con acceso multiusuario añada a /etc/fuse.conf:"
        msg "       user_allow_other"
    fi
else
    msg "AVISO: no existe /etc/fuse.conf; créelo con la línea 'user_allow_other'"
    msg "       si necesita que el volumen sea accesible por otros usuarios."
fi

msg ""
msg "Instalación completada. Pasos siguientes:"
msg "  1) Crear un volumen:   matrixfs-mkfs -L DATOS /dev/sdb1"
msg "  2) Montar a mano:      mount -t matrixfs /dev/sdb1 /mnt/datos"
msg "  3) Automático:         conectar el medio (udev) o usar /etc/fstab"
msg "     (véase $PREFIX/share/doc/matrixfs/fstab.example)"
