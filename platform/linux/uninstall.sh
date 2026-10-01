#!/bin/sh
# uninstall.sh — desinstala el soporte Linux (FUSE 3) de MatrixFS Ultra.
#
# Uso:
#   sudo ./uninstall.sh              # desinstala de /usr/local
#   sudo PREFIX=/usr ./uninstall.sh  # desinstala de una instalación de sistema
#
# Antes de desinstalar, desmonta los volúmenes activos montados por la unidad
# matrixfs@<dispositivo>.service y detiene dichas unidades.

set -eu

PREFIX=${PREFIX:-/usr/local}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
msg() { printf '%s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || die "se requieren privilegios de root (use sudo)."

cd "$SCRIPT_DIR"

if command -v systemctl >/dev/null 2>&1; then
    msg "==> Deteniendo unidades matrixfs@*.service activas"
    for u in $(systemctl list-units --type=service --all --no-legend \
                   'matrixfs@*.service' 2>/dev/null | awk '{print $1}'); do
        systemctl stop "$u" 2>/dev/null || true
    done
    systemctl daemon-reload || true
fi

msg "==> Desinstalando de $PREFIX"
make uninstall PREFIX="$PREFIX"

# Enlace creado por install.sh para que mount(8) encuentre el helper.
if [ -L /sbin/mount.matrixfs ]; then
    rm -f /sbin/mount.matrixfs
    msg "==> Enlace /sbin/mount.matrixfs eliminado"
fi

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
fi

msg "Desinstalación completada."
