# MatrixFS Ultra «ATLAS» v1.0 — Makefile (§26 proceso de build)
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

.PHONY: all lib test bench mfstool mfsctl check-map plan strict clean

all: lib $(TESTS) $(TOOL) $(MFSCTL)

lib: $(LIB)

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
	-$(RM) $(subst /,\,$(CORE_OBJ) $(SIM_OBJ) $(LIB) $(TESTS) $(TOOL) $(MFSCTL)) 2>NUL
	-$(RM) matrixfs_resources.h rsc_certificate.txt 2>NUL
else
	-rm -f $(CORE_OBJ) $(SIM_OBJ) $(LIB) $(TESTS) $(TOOL) $(MFSCTL)
	-rm -f matrixfs_resources.h rsc_certificate.txt
endif
