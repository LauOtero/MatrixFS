# matrixfs_stm32.mk — fragmento de Makefile para el port STM32Cube de MatrixFS.
#
# Copyright 2026 MatrixFS contributors
# SPDX-License-Identifier: Apache-2.0
#
# USO (toolchain "Makefile" de STM32CubeMX / make a mano):
#
#   make -f Makefile -f /ruta/a/MatrixFS/platform/stm32cube/matrixfs_stm32.mk
#
# o, si prefieres un unico make, agrega ESTA linea al final del Makefile del
# proyecto:
#
#   include /ruta/a/MatrixFS/platform/stm32cube/matrixfs_stm32.mk
#
# NOTA IMPORTANTE: STM32CubeMX REGENERA el Makefile del proyecto. Por eso la
# forma estable es la primera (segundo -f) o invocar un wrapper propio; si
# editas el Makefile generado, la linea se pierde en la siguiente generacion.
# Para STM32CubeIDE, ver README §Integracion (la via recomendada es anadir la
# carpeta al proyecto, no parchear el Makefile).
#
# Requiere que MATRIXFS_ROOT apunte a la raiz del repositorio MatrixFS. Si no lo
# defines, se deduce de la ubicacion de este fichero.

MATRIXFS_ROOT ?= $(patsubst %/platform/stm32cube,%,$(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))

# ---------------------------------------------------------------------------
# 1. Comprobacion del arbol del repositorio
# ---------------------------------------------------------------------------
ifeq ($(wildcard $(MATRIXFS_ROOT)/src/core/mfs_fs.c),)
$(error MatrixFS: no se encuentra el arbol del repositorio en MATRIXFS_ROOT='$(MATRIXFS_ROOT)'. Define MATRIXFS_ROOT=/ruta/al/repositorio)
endif

# ---------------------------------------------------------------------------
# 2. Fuentes
# ---------------------------------------------------------------------------
MATRIXFS_STM32_DIR := $(MATRIXFS_ROOT)/platform/stm32cube
MATRIXFS_COMMON_DIR := $(MATRIXFS_ROOT)/platform/common
MATRIXFS_EMB_DIR := $(MATRIXFS_ROOT)/platform/embedded

# Nucleo: MISMA fuente de verdad que el resto del repositorio (glob), no una
# lista a mano. Una lista a mano ya dejo fuera src/core/mfs_arch.c en el
# componente de ESP-IDF, que por eso no enlazaba.
MATRIXFS_CORE_SRC := $(wildcard $(MATRIXFS_ROOT)/src/core/*.c) \
                     $(wildcard $(MATRIXFS_ROOT)/src/crypto/*.c) \
                     $(wildcard $(MATRIXFS_ROOT)/src/ftl/*.c) \
                     $(wildcard $(MATRIXFS_ROOT)/src/tier/*.c) \
                     $(wildcard $(MATRIXFS_ROOT)/src/sec/*.c) \
                     $(wildcard $(MATRIXFS_ROOT)/src/xio/*.c)

# El puerto (§20.2) lo aporta mfs_stm32_port.c: los envoltorios del nucleo
# quedan FUERA para no duplicar los simbolos mfs_port_*.
MATRIXFS_CORE_SRC := $(filter-out $(MATRIXFS_ROOT)/src/core/mfs_port_arch.c \
                                  $(MATRIXFS_ROOT)/src/core/mfs_port_rtos.c, \
                                  $(MATRIXFS_CORE_SRC))

# Port STM32Cube + capa embebida compartida + drivers L2 portables.
MATRIXFS_STM32_SRC := \
    $(wildcard $(MATRIXFS_STM32_DIR)/src/*.c) \
    $(MATRIXFS_EMB_DIR)/mfs_embedded.c \
    $(MATRIXFS_COMMON_DIR)/mfs_sectors.c \
    $(MATRIXFS_COMMON_DIR)/mfs_l2_8bit.c \
    $(MATRIXFS_COMMON_DIR)/mfs_l2_managed.c

# Backends OPCIONALES, con la MISMA semantica que el CMakeLists del port. Hay
# que excluir el backend cuyo periferico NO exista en tu dispositivo, o la
# compilacion falla: p. ej. un F407 (sin QUADSPI) no compila mfs_stm32_ospi.c.
#   make ... MATRIXFS_NO_OSPI=1      # sin OSPI/QUADSPI  (F4 sin QSPI, F1, ...)
#   make ... MATRIXFS_NO_IFLASH=1    # sin flash interna como medio
ifdef MATRIXFS_NO_OSPI
MATRIXFS_STM32_SRC := $(filter-out $(MATRIXFS_STM32_DIR)/src/mfs_stm32_ospi.c,$(MATRIXFS_STM32_SRC))
endif
ifdef MATRIXFS_NO_IFLASH
MATRIXFS_STM32_SRC := $(filter-out $(MATRIXFS_STM32_DIR)/src/mfs_stm32_iflash.c,$(MATRIXFS_STM32_SRC))
endif

# ---------------------------------------------------------------------------
# 3. Include dirs y defines
# ---------------------------------------------------------------------------
MATRIXFS_INCLUDES := \
    -I$(MATRIXFS_ROOT)/include \
    -I$(MATRIXFS_ROOT)/src \
    -I$(MATRIXFS_EMB_DIR) \
    -I$(MATRIXFS_COMMON_DIR) \
    -I$(MATRIXFS_STM32_DIR)/include \
    -I$(MATRIXFS_STM32_DIR)/src

# MFS_STM32_USE_CONF habilita matrixfs_stm32_mount_default() con
# mfs_stm32_conf.h. Quitalo si montas siempre con una mfs_stm32_cfg explicita.
MATRIXFS_DEFS := -DMFS_STM32_USE_CONF=1

# AJUSTE DE RAM Y CAPACIDAD (ver README §Presupuesto de RAM).
#   MFS_L2P_SLOTS : entradas del mapa logico->fisico (8 B cada una, en .bss).
#                   capacidad mapeable ~= MFS_L2P_SLOTS * chunk_size.
#   MFS_ZONE_MAX  : zonas en RAM (dentro de mf_t). Volumen util ~= MFS_ZONE_MAX
#                   * erase_unit.
# Los valores por defecto (4096 y 128) consumen ~49 KB de .bss y ~17 KB de mf_t:
# NO caben en un STM32 con menos de ~96 KB de RAM. Bajalos en partes pequenas.
MATRIXFS_DEFS += -DMFS_L2P_SLOTS=4096 -DMFS_ZONE_MAX=128

# ---------------------------------------------------------------------------
# 4. Volcado a las variables del Makefile de CubeMX
#
# Se usa += para no pisar lo que ya haya definido el proyecto.
# ---------------------------------------------------------------------------
C_SOURCES += $(MATRIXFS_CORE_SRC) $(MATRIXFS_STM32_SRC)
C_INCLUDES += $(MATRIXFS_INCLUDES)
C_DEFS += $(MATRIXFS_DEFS)

# ---------------------------------------------------------------------------
# 5. Regla de comodidad: biblioteca estatica precompilada
#
# Util para quien prefiera enlazar un .a en lugar de compilar el arbol entero
# (mas rapido y mas facil de integrar en STM32CubeIDE). Requiere el mismo
# CC/CFLAGS del proyecto, de modo que se invoca desde el propio make:
#
#   make -f Makefile -f .../matrixfs_stm32.mk matrixfs_stm32_lib
# ---------------------------------------------------------------------------
MATRIXFS_LIB := libmatrixfs_stm32.a
MATRIXFS_OBJS := $(MATRIXFS_CORE_SRC:.c=.o) $(MATRIXFS_STM32_SRC:.c=.o)

.PHONY: matrixfs_stm32_lib matrixfs_stm32_clean
matrixfs_stm32_lib: $(MATRIXFS_LIB)

$(MATRIXFS_LIB): $(MATRIXFS_OBJS)
	$(AR) rcs $@ $^

matrixfs_stm32_clean:
	-rm -f $(MATRIXFS_OBJS) $(MATRIXFS_LIB)
