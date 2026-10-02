# MatrixFS «ATLAS» v1.0 — Makefile (§26 proceso de build)
#
# Objetivos:
#   make            → librería estática + suite de tests + mfstool
#   make lib        → libmatrixfs.a (núcleo + crypto)
#   make test       → compila y ejecuta la suite de verificación (§27)
#   make bench      → ejecuta MFS-Bench v2 reducido (§17)
#   make mfstool    → herramienta de build (§26)
#   make check-map  → verifica ausencia de ruta al heap (MFS-RES-002)
#   make plan       → genera matrixfs_resources.h + certificado RSC
#   make strict     → compila con -Werror
#   make clean
#
# El puerto de host (sim/mfs_port_host.c) se enlaza en builds de PC; en target
# el integrador aporta su propio puerto y excluye sim/.

CC       ?= gcc
CSTANDARD ?= -std=c11
WARN      = -Wall -Wextra
INC       = -Iinclude -Isrc -Isim -Itests -Iplatform/common
CFLAGS   ?= $(CSTANDARD) $(WARN) -O2 $(INC)
LDFLAGS  ?=
LDLIBS   ?=

ifeq ($(OS),Windows_NT)
  RM = cmd /C del /Q /F
  # En Windows el directorio actual está en la búsqueda de ejecutables; en
  # POSIX hay que invocar los binarios con './' o no se encuentran.
  RUN =
else
  RUN = ./
endif

CORE_SRC := $(wildcard src/core/*.c) $(wildcard src/crypto/*.c) \
            $(wildcard src/ftl/*.c) $(wildcard src/tier/*.c) \
            $(wildcard src/sec/*.c) $(wildcard src/xio/*.c)
SIM_SRC  := $(wildcard sim/*.c)
TEST_SRC := $(wildcard tests/*.c)
# Capa de integración portable (compartida por FUSE y WinFsp); el CLI mfsctl
# trae su propio main y se excluye de la suite.
PLAT_SRC := $(wildcard platform/common/*.c)
VFS_SRC  := $(filter-out platform/common/mfs_vfsctl.c,$(PLAT_SRC))
CORE_OBJ := $(CORE_SRC:.c=.o)
SIM_OBJ  := $(SIM_SRC:.c=.o)

LIB      := libmatrixfs.a
TESTS    := mfs_tests.exe
TOOL     := mfstool.exe
MFSCTL   := mfsctl.exe

# Drivers L2 para MCU (NOR/FRAM/EEPROM SPI-I2C, flash interna, SD-SPI).
# El puerto (§20.2) y el modelo de arquitectura viven en el núcleo
# (src/core/mfs_port_arch.c, src/core/mfs_arch.c); esta librería sólo aporta
# los drivers de dispositivo. Para un target de 8 bits, cross-compilar con
# -DMFS_ALLOW_8BIT_TARGET=1 (MFS-ARCH-010 rev.3) y el toolchain del MCU.
LIBMCU   := libmatrixfs_mcu.a
MCU_SRC  := platform/common/mfs_l2_8bit.c
MCU_OBJ  := $(MCU_SRC:.c=.o)

.PHONY: all lib test bench mfstool mfsctl mcu check-map plan strict clean platform-test platform-clean

all: lib $(TESTS) $(TOOL) $(MFSCTL)

lib: $(LIB)

# Drivers L2 para MCU (librería aparte; el núcleo ya aporta el puerto).
$(LIBMCU): $(MCU_OBJ)
	ar rcs $@ $^

mcu: $(LIBMCU)

$(LIB): $(CORE_OBJ)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTS): $(CORE_SRC) $(SIM_SRC) $(VFS_SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(TOOL): $(CORE_SRC) $(SIM_SRC) tools/mfstool.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(MFSCTL): $(CORE_SRC) $(SIM_SRC) $(VFS_SRC) platform/common/mfs_vfsctl.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test: $(TESTS)
	$(RUN)$(TESTS)

bench: $(TESTS)
	$(RUN)$(TESTS) --bench

mfstool: $(TOOL)

mfsctl: $(MFSCTL)

check-map: $(TOOL)
	$(RUN)$(TOOL) check-map

plan: $(TOOL)
	$(RUN)$(TOOL) plan

strict:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CSTANDARD) $(WARN) -Werror -O2 $(INC)" all

clean:
ifeq ($(OS),Windows_NT)
	-$(RM) $(subst /,\,$(CORE_OBJ) $(SIM_OBJ) $(MCU_OBJ) $(LIB) $(LIBMCU) $(TESTS) $(TOOL) $(MFSCTL)) 2>NUL
	-$(RM) matrixfs_resources.h rsc_certificate.txt 2>NUL
	-$(RM) $(subst /,\,$(ESP_TEST_EXE) $(STM32_TEST_EXE)) 2>NUL
else
	-rm -f $(CORE_OBJ) $(SIM_OBJ) $(MCU_OBJ) $(LIB) $(LIBMCU) $(TESTS) $(TOOL) $(MFSCTL)
	-rm -f matrixfs_resources.h rsc_certificate.txt
	-rm -f $(ESP_TEST_EXE) $(STM32_TEST_EXE)
endif

# ===========================================================================
# Verificación de los ports de plataforma (ESP-IDF y STM32Cube) EN HOST.
#
# Compila el codigo REAL de platform/esp-idf y platform/stm32cube contra el
# nucleo REAL, usando shims minimos de los SDK (platform/*/test/shims). Es la
# unica forma de verificar estas integraciones sin los SDK instalados: ya
# detecto un fallo de enlace en el componente ESP-IDF (faltaba
# src/core/mfs_arch.c) y un desbordamiento de buffer de 16 B en el backend de
# flash interna de STM32.
#
#   make platform-test   -> compila y ejecuta las dos suites
# ===========================================================================
CORE_NOPORT := $(filter-out src/core/mfs_port_arch.c src/core/mfs_port_rtos.c,$(CORE_SRC))

ESP_TEST_EXE  := mfs_esp_test.exe
ESP_TEST_SRC  := platform/esp-idf/test/test_esp_idf.c \
                 platform/esp-idf/test/esp_partition_fake.c \
                 platform/esp-idf/test/shims/idf_shim.c \
                 platform/esp-idf/matrixfs_esp.c \
                 platform/esp-idf/matrixfs_esp_vfs.c \
                 platform/embedded/mfs_embedded.c
ESP_TEST_INC  := -Iplatform/esp-idf/test/shims -Iplatform/esp-idf \
                 -Iplatform/esp-idf/include -Iinclude -Isrc \
                 -Iplatform/embedded -Itests
# El shim de esp_vfs.h debe entrar ANTES que <dirent.h>: en MinGW struct dirent
# no lleva d_type y el componente lo rellena (newlib si lo trae).
# Perfil de IDF emulado: 1 = v5.x, 2 = v6.x. El banco de pruebas soporta ambos
# (shims/ para v5, shims/idf_v61/ para v6) y exige declararlo de forma explicita
# en vez de depender del orden de inclusion. Se prueba el perfil v5.x, que es el
# minimo declarado en idf_component.yml.
ESP_TEST_DEFS := -DMATRIXFS_SHIM_IDF_PROFILE=1 \
                 -include platform/esp-idf/test/shims/esp_vfs.h

STM32_TEST_EXE := mfs_stm32_test.exe
STM32_TEST_SRC := platform/stm32cube/test/test_stm32.c \
                  platform/stm32cube/test/stubs_backends.c \
                  platform/stm32cube/test/shims/stm32_hal_shim.c \
                  platform/stm32cube/src/matrixfs_stm32.c \
                  platform/stm32cube/src/mfs_stm32_backend.c \
                  platform/stm32cube/src/mfs_stm32_flash_map.c \
                  platform/stm32cube/src/mfs_stm32_hal.c \
                  platform/stm32cube/src/mfs_stm32_port.c \
                  platform/stm32cube/src/mfs_stm32_iflash.c \
                  platform/embedded/mfs_embedded.c \
                  platform/common/mfs_sectors.c \
                  platform/common/mfs_l2_8bit.c \
                  platform/common/mfs_l2_managed.c
STM32_TEST_INC := -Iplatform/stm32cube/test/shims -Iplatform/stm32cube/include \
                  -Iplatform/stm32cube/src -Iinclude -Isrc -Iplatform/embedded \
                  -Iplatform/common -Itests
# La unidad de programacion y la ECC se prueban en cuatro variantes, que cubren
# el rango de familias: 2 B (F0/F1), 4 B (F4/F7), 8 B+ECC (L4/G4/G0) y
# 16 B+ECC (H5/H7/U5). El modelo de flash del shim respeta la unidad y la ECC,
# de modo que cada variante ejerce un camino distinto del RMW.
MFS_STM32_BUILD = $(CC) $(CSTANDARD) $(WARN) -Werror -O2 \
    -DMFS_STM32_HOST_TEST=1 $(STM32_TEST_INC) \
    -o $(STM32_TEST_EXE) $(STM32_TEST_SRC) $(CORE_NOPORT)

.PHONY: platform-test platform-clean
platform-test: $(ESP_TEST_EXE)
	@echo "=== ESP-IDF (host, con shims de esp_partition/FreeRTOS/VFS) ==="
	$(RUN)$(ESP_TEST_EXE)
	@echo "=== STM32Cube: unidad 2 B sin ECC (STM32F0/F1) ==="
	@$(MFS_STM32_BUILD) -DMFS_STM32_HOST_PGM_UNIT=2 -DMFS_STM32_HOST_ECC=0
	@$(RUN)$(STM32_TEST_EXE)
	@echo "=== STM32Cube: unidad 4 B sin ECC (STM32F4/F7) ==="
	@$(MFS_STM32_BUILD) -DMFS_STM32_HOST_PGM_UNIT=4 -DMFS_STM32_HOST_ECC=0
	@$(RUN)$(STM32_TEST_EXE)
	@echo "=== STM32Cube: unidad 8 B CON ECC (STM32L4/G4/G0) ==="
	@$(MFS_STM32_BUILD) -DMFS_STM32_HOST_PGM_UNIT=8 -DMFS_STM32_HOST_ECC=1
	@$(RUN)$(STM32_TEST_EXE)
	@echo "=== STM32Cube: unidad 16 B CON ECC (STM32H5/H7/U5) ==="
	@$(MFS_STM32_BUILD) -DMFS_STM32_HOST_PGM_UNIT=16 -DMFS_STM32_HOST_ECC=1
	@$(RUN)$(STM32_TEST_EXE)

$(ESP_TEST_EXE): $(ESP_TEST_SRC) $(CORE_NOPORT)
	$(CC) $(CSTANDARD) $(WARN) -Werror -O2 -g $(ESP_TEST_DEFS) $(ESP_TEST_INC) \
	  -o $@ $(ESP_TEST_SRC) $(CORE_NOPORT)

platform-clean:
ifeq ($(OS),Windows_NT)
	-$(RM) $(subst /,\,$(ESP_TEST_EXE) $(STM32_TEST_EXE)) 2>NUL
else
	-rm -f $(ESP_TEST_EXE) $(STM32_TEST_EXE)
endif
