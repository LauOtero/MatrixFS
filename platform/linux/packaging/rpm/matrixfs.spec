# matrixfs.spec — empaquetado RPM de MatrixFS Ultra «ATLAS» (FUSE 3).
#
# El árbol fuente NO se distribuye como tarball: el paquete se compila
# directamente del árbol de trabajo del proyecto, que se indica con
# `_sourcedir`. Construcción (desde platform/linux):
#
#   rpmbuild -bb --define "_sourcedir $PWD/../.." packaging/rpm/matrixfs.spec
#
# Requisitos de construcción:
#   dnf install gcc make pkgconfig fuse3-devel rpm-build
#
# El paquete instala: front-end FUSE 3, CLI matrixfs-ctl, helper de montaje
# /sbin/mount.matrixfs, matrixfs-mkfs, el sondeo de udev, la unidad de systemd
# y la regla de udev para el automontaje.
#
# Al desinstalar NO se toca el contenido de los volúmenes: toda la información
# (ficheros, metadatos y permisos) vive en el propio medio.

Name:           matrixfs-ultra
Version:        1.0.0
Release:        1%{?dist}
Summary:        Sistema de archivos determinista sobre memoria no volátil (FUSE 3)

License:        MIT
URL:            https://example.invalid/matrixfs
# Sin Source0: se compila del árbol de trabajo apuntado por %{_sourcedir}.

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  fuse3-devel
Requires:       fuse3-libs
Recommends:     fuse3

%description
MatrixFS Ultra es un sistema de archivos para memoria no volátil (NOR, NAND,
eMMC, FRAM) diseñado con criterios de determinismo, integridad y mínimo
consumo de memoria: sin asignación dinámica, con registro de escritura WAL,
recuperación tras corte de energía y verificación de integridad.

Este paquete incluye el front-end FUSE 3 para Linux (espacio de usuario, sin
módulos del kernel), la herramienta de administración matrixfs-ctl, el helper
de montaje para /etc/fstab, la unidad de systemd matrixfs@.service y la regla
de udev para el automontaje al conectar el medio.

El layout en el medio es idéntico entre sistemas operativos: un volumen creado
en Linux es legible y escribible en Windows (WinFsp) y viceversa.

%prep
# No hay tarball que desempaquetar; se valida que %{_sourcedir} apunte a un
# árbol MatrixFS Ultra completo.
test -f %{_sourcedir}/include/matrixfs/matrixfs.h \
    || { echo "error: defina _sourcedir a la raíz del proyecto MatrixFS Ultra" >&2; exit 1; }
test -f %{_sourcedir}/platform/linux/Makefile \
    || { echo "error: no se encuentra platform/linux en %{_sourcedir}" >&2; exit 1; }

%build
# El Makefile usa C11 y activa los avisos estrictos; se le pasan los optflags
# del constructor para respetar la distribución.
make -C %{_sourcedir}/platform/linux \
     PREFIX=%{_prefix} \
     CC="%{__cc}" \
     CFLAGS="%{optflags} -std=c11 -Wall -Wextra" \
     all

%install
rm -rf %{buildroot}
make -C %{_sourcedir}/platform/linux \
     PREFIX=%{_prefix} \
     DESTDIR=%{buildroot} \
     install

%post
# Registro de la unidad y de la regla sin reiniciar el sistema. No se arranca
# ningún servicio: el montaje lo dispara udev al conectar el medio.
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules >/dev/null 2>&1 || :
    udevadm trigger --subsystem-match=block >/dev/null 2>&1 || :
fi
# El automontaje usa allow_other: libfuse exige habilitarlo en /etc/fuse.conf.
if [ -f /etc/fuse.conf ] && ! grep -q '^[[:space:]]*user_allow_other' /etc/fuse.conf; then
    echo "MatrixFS: para el acceso multiusuario añada 'user_allow_other' a /etc/fuse.conf"
fi

%preun
# Detener los montajes automáticos antes de borrar los binarios. El contenido
# de los volúmenes queda intacto (los datos viven en el medio).
if [ "$1" = 0 ] && command -v systemctl >/dev/null 2>&1; then
    for u in $(systemctl list-units --type=service --all --no-legend \
        'matrixfs@*.service' 2>/dev/null | awk '{print $1}'); do
        systemctl stop "$u" >/dev/null 2>&1 || :
    done
fi

%postun
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules >/dev/null 2>&1 || :
fi

%files
%{_bindir}/matrixfs_fuse
%{_bindir}/matrixfs-ctl
%{_sbindir}/mount.matrixfs
%{_sbindir}/matrixfs-mkfs
%{_prefix}/lib/matrixfs/matrixfs-probe
%{_prefix}/lib/systemd/system/matrixfs@.service
%{_prefix}/lib/udev/rules.d/99-matrixfs.rules
%doc %{_docdir}/matrixfs

%changelog
* Wed Sep 30 2026 MatrixFS Ultra Project <matrixfs@example.invalid> - 1.0.0-1
- Versión inicial: front-end FUSE 3, matrixfs-ctl, helper de montaje,
  automontaje por udev/systemd y paquete .deb equivalente.
