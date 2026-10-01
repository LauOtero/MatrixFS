# platform/micropython/micropython.mk — fragmento de build del usermod MatrixFS.
#
# Licencia: Apache-2.0 (ver LICENSE en la raíz del repositorio).
#
# Este fichero es un "usermod" de MicroPython (make). MicroPython lo incluye
# con la variable USERMOD_DIR apuntando a este directorio. Compila el módulo C
# (clase MatrixFS + File) y, para que sea enlazable, hay que añadir también las
# fuentes del núcleo MatrixFS (ver más abajo).

# Ficheros del módulo (este directorio contiene modmatrixfs.c).
SRC_USERMOD += $(USERMOD_DIR)

# Rutas de encabezados: el propio módulo, la API pública (include/), las
# estructuras internas (src/, para incluir "mfs_internal.h") y la capa
# embebida compartida (platform/embedded/).
CFLAGS_USERMOD += -I$(USERMOD_DIR) \
                  -I$(USERMOD_DIR)/../../include \
                  -I$(USERMOD_DIR)/../../src \
                  -I$(USERMOD_DIR)/../../platform/embedded

# ---------------------------------------------------------------------------
# Núcleo MatrixFS: fuentes que deben compilarse en el build del port.
# ---------------------------------------------------------------------------
# modmatrixfs.c referencia los símbolos del núcleo; si el port no los compila
# por su cuenta, el enlace fallará. Añádelos a SRC_USERMOD (o al build del
# port). Lista exacta (misma que la del Makefile raíz, sección CORE_SRC):
#
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/core/*.c)
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/crypto/*.c)
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/ftl/*.c)
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/tier/*.c)
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/sec/*.c)
# SRC_USERMOD += $(wildcard $(USERMOD_DIR)/../../src/xio/*.c)
# SRC_USERMOD += $(USERMOD_DIR)/../../platform/embedded/mfs_embedded.c
#
# Nota: compilar estas fuentes como parte del usermod usa los CFLAGS del port.
# Si algún fichero del núcleo necesita opciones adicionales (p. ej. rutas de
# include extra), añádelas a CFLAGS_USERMOD.
#
# Las primitivas de puerto obligatorias de MatrixFS (mfs_port_crit_enter,
# mfs_port_crit_exit, mfs_port_cycles, mfs_port_time_us, mfs_port_wfi) las
# aporta modmatrixfs.c. NO compiles sim/mfs_port_host.c ni
# platform/8bit/mfs_port_8bit.c en el mismo binario: colisionarían.
