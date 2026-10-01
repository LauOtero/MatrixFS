#!/bin/sh
# build-deb.sh — construye el paquete .deb de matrixfs-ultra.
#
# Requisitos: gcc, make, libfuse3-dev, dpkg-deb (paquete dpkg-dev).
# Uso:
#   ./build-deb.sh            # genera ../matrixfs-ultra_1.0.0_amd64.deb
#
# La instalación se hace con PREFIX=/usr (FHS) y DESTDIR apuntando al árbol
# del paquete, de modo que las rutas de systemd (/usr/lib/systemd/system) y de
# udev (/usr/lib/udev/rules.d) coinciden con las del sistema.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LINUX_DIR=$(CDPATH= cd -- "$HERE/../.." && pwd)
VERSION=1.0.0
ARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
PKG=matrixfs-ultra_${VERSION}_${ARCH}
STAGE="$HERE/$PKG"
DEBIAN="$STAGE/DEBIAN"

command -v dpkg-deb >/dev/null 2>&1 || { echo "falta dpkg-deb (dpkg-dev)"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$DEBIAN"

cd "$LINUX_DIR"
make clean >/dev/null 2>&1 || true
make PREFIX=/usr
make install PREFIX=/usr DESTDIR="$STAGE"

cp "$HERE/control" "$DEBIAN/control"
sed -i "s/^Architecture: .*/Architecture: $ARCH/" "$DEBIAN/control"
install -m 0755 "$HERE/postinst" "$DEBIAN/postinst"
install -m 0755 "$HERE/prerm" "$DEBIAN/prerm"

dpkg-deb --build --root-owner-group "$STAGE" "$HERE/$PKG.deb"
echo "generado: $HERE/$PKG.deb"
