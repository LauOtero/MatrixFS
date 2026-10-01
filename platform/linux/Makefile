# MatrixFS Ultra «ATLAS» v1.0 — build del soporte Linux (FUSE 3)
#
# Compila dos binarios:
#   matrixfs_fuse  front-end FUSE 3 (requiere libfuse3 y sus cabeceras)
#   matrixfs-ctl   CLI de validación/administración (sin dependencias)
#
# Uso (desde platform/linux):
#   make                 # compila ambos binarios en ./build
#   make check-fuse      # verifica que libfuse3 está disponible
#   sudo make install    # instala binarios, mount helper, systemd y udev
#   sudo make uninstall
#   make clean
#
# Requisitos de compilación:
#   · gcc (o clang) con C11
#   · libfuse3-dev  (pkg-config fuse3) para matrixfs_fuse
#   · make, pkg-config
#
# Nota sobre el puerto: se reutiliza `sim/mfs_port_host.c`, el puerto POSIX
# del proyecto (contador monotónico y tiempo). Sus primitivas de sección
# crítica son vacías porque TODAS las llamadas al núcleo las serializa el
# mutex del adaptador VFS (el núcleo MatrixFS no es reentrante).

ROOT     := ../..
BUILD    := build
PREFIX   ?= /usr/local
DESTDIR  ?=
SYSCONFDIR ?= /etc
UNITDIR  ?= $(PREFIX)/lib/systemd/system
UDEVDIR  ?= $(PREFIX)/lib/udev/rules.d
DOCDIR   ?= $(PREFIX)/share/doc/matrixfs

CC       ?= gcc
CSTANDARD ?= -std=c11
WARN      = -Wall -Wextra

CPPFLAGS += -D_FILE_OFFSET_BITS=64 \
            -I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/platform/common

# Dimensión del mapa L2P (tabla estática RAM, sin heap) para el perfil de host.
# El valor por defecto del núcleo (4096) es el del objetivo embebido y sólo
# permite mapear ~16 MiB con chunk 4096; con 2^18 entradas (2 MiB de .bss) se
# cubre el volumen máximo del modo Extended (512 MiB). Debe ser el MISMO valor
# en todos los binarios que toquen un volumen (formateo, CLI y demonio FUSE).
L2P_SLOTS ?= 262144
CPPFLAGS  += -DMFS_L2P_SLOTS=$(L2P_SLOTS)

# libfuse3: preferimos pkg-config; si no está, se asume el layout estándar.
FUSE_PKG := $(shell pkg-config --exists fuse3 2>/dev/null && echo yes)
ifeq ($(FUSE_PKG),yes)
  FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
  FUSE_LIBS   := $(shell pkg-config --libs fuse3)
else
  FUSE_CFLAGS := -I/usr/include/fuse3
  FUSE_LIBS   := -lfuse3
endif

CFLAGS ?= $(CSTANDARD) $(WARN) -O2
LDLIBS += -lpthread

CORE_SRC := $(wildcard $(ROOT)/src/core/*.c) $(wildcard $(ROOT)/src/crypto/*.c) \
            $(wildcard $(ROOT)/src/ftl/*.c) $(wildcard $(ROOT)/src/tier/*.c) \
            $(wildcard $(ROOT)/src/sec/*.c) $(wildcard $(ROOT)/src/xio/*.c)
PORT_SRC := $(ROOT)/sim/mfs_port_host.c
PLAT_SRC := $(ROOT)/platform/common/mfs_plat.c \
            $(ROOT)/platform/common/mfs_blk.c \
            $(ROOT)/platform/common/mfs_vfs.c
COMMON   := $(CORE_SRC) $(PORT_SRC) $(PLAT_SRC)

FUSE_BIN := $(BUILD)/matrixfs_fuse
CTL_BIN  := $(BUILD)/matrixfs-ctl

.PHONY: all check-fuse install uninstall clean strict

all: $(FUSE_BIN) $(CTL_BIN)

check-fuse:
ifneq ($(FUSE_PKG),yes)
	@echo "AVISO: pkg-config no encuentra fuse3; se asume -I/usr/include/fuse3 -lfuse3."
	@echo "       Instale libfuse3-dev (Debian/Ubuntu), fuse3-devel (Fedora/RHEL)"
	@echo "       o fuse3 (Arch) si la compilación falla."
else
	@echo "libfuse3 detectada: $(FUSE_LIBS)"
endif

$(FUSE_BIN): $(COMMON) matrixfs_fuse.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(LDFLAGS) $(FUSE_LIBS) $(LDLIBS)

$(CTL_BIN): $(COMMON) $(ROOT)/platform/common/mfs_vfsctl.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

strict:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CSTANDARD) $(WARN) -Werror -O2" all

install: all
	install -D -m 0755 $(FUSE_BIN) $(DESTDIR)$(PREFIX)/bin/matrixfs_fuse
	install -D -m 0755 $(CTL_BIN)  $(DESTDIR)$(PREFIX)/bin/matrixfs-ctl
	# Helper de montaje para /etc/fstab (mount.matrixfs)
	install -D -m 0755 scripts/matrixfs-mount $(DESTDIR)$(PREFIX)/sbin/mount.matrixfs
	install -D -m 0755 scripts/matrixfs-mkfs  $(DESTDIR)$(PREFIX)/sbin/matrixfs-mkfs
	install -D -m 0755 scripts/matrixfs-probe $(DESTDIR)$(PREFIX)/lib/matrixfs/matrixfs-probe
	@# mount(8) sólo busca helpers en /sbin y /usr/sbin (PREFIX=/usr es correcto).
	@if [ "$(PREFIX)" != "/usr" ] && [ ! -e /sbin/mount.matrixfs ]; then \
	  echo "AVISO: mount(8) busca los helpers en /sbin y /usr/sbin."; \
	  echo "       Para 'mount -t matrixfs' y /etc/fstab ejecute:"; \
	  echo "         ln -sf $(PREFIX)/sbin/mount.matrixfs /sbin/mount.matrixfs"; \
	fi
	# Integración con systemd y udev (rutas sustituidas por @BINDIR@)
	install -d $(DESTDIR)$(UNITDIR) $(DESTDIR)$(UDEVDIR)
	sed -e 's|@BINDIR@|$(PREFIX)/bin|g' -e 's|@LIBDIR@|$(PREFIX)/lib|g' \
	    systemd/matrixfs@.service > $(DESTDIR)$(UNITDIR)/matrixfs@.service
	chmod 0644 $(DESTDIR)$(UNITDIR)/matrixfs@.service
	sed -e 's|@BINDIR@|$(PREFIX)/bin|g' -e 's|@LIBDIR@|$(PREFIX)/lib|g' \
	    udev/99-matrixfs.rules > $(DESTDIR)$(UDEVDIR)/99-matrixfs.rules
	chmod 0644 $(DESTDIR)$(UDEVDIR)/99-matrixfs.rules
	# Documentación y ejemplo de fstab
	install -D -m 0644 fstab.example $(DESTDIR)$(DOCDIR)/fstab.example
	install -D -m 0644 README.md     $(DESTDIR)$(DOCDIR)/linux-integration.md
	@echo "Instalado. Para montajes automáticos revise /etc/fuse.conf (user_allow_other)"
	@echo "y recargue systemd/udev:  systemctl daemon-reload && udevadm control --reload"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/matrixfs_fuse
	rm -f $(DESTDIR)$(PREFIX)/bin/matrixfs-ctl
	rm -f $(DESTDIR)$(PREFIX)/sbin/mount.matrixfs
	rm -f $(DESTDIR)$(PREFIX)/sbin/matrixfs-mkfs
	rm -f $(DESTDIR)$(PREFIX)/lib/matrixfs/matrixfs-probe
	rm -f $(DESTDIR)$(UNITDIR)/matrixfs@.service
	rm -f $(DESTDIR)$(UDEVDIR)/99-matrixfs.rules
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -rf $(BUILD)
