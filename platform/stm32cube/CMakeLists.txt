# CMakeLists.txt — port STM32Cube de MatrixFS para CMake / STM32CubeCLT / CI.
#
# Copyright 2026 MatrixFS contributors
# SPDX-License-Identifier: Apache-2.0
#
# USO en un proyecto CMake (incluido STM32CubeCLT):
#
#   add_subdirectory(third_party/MatrixFS/platform/stm32cube)
#   target_link_libraries(mi_firmware PRIVATE matrixfs_stm32)
#
# La libreria `matrixfs_stm32` arrastra el nucleo, la capa embebida y los
# drivers L2 portables, y expone los include dirs necesarios.
#
# IMPORTANTE: exclude_from_all / orden de enlace
#   El nucleo se compila aqui como libreria estatica; el enlazador resuelve los
#   simbolos por orden, de modo que `matrixfs_stm32` debe ir ANTES que las
#   librerias de la HAL si hubiera dependencias cruzadas. La practica segura es
#   enlazarla junto al resto del firmware en una unica target.

cmake_minimum_required(VERSION 3.15)
project(matrixfs_stm32 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 1. Raiz del repositorio, con error accionable
# ---------------------------------------------------------------------------
get_filename_component(MATRIXFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if (NOT EXISTS "${MATRIXFS_ROOT}/src/core/mfs_fs.c")
    message(FATAL_ERROR
        "MatrixFS: no se encuentra el arbol del repositorio.\n"
        "  Buscado en : ${MATRIXFS_ROOT}\n"
        "  Esperaba   : ${MATRIXFS_ROOT}/src/core/mfs_fs.c")
endif()

# ---------------------------------------------------------------------------
# 2. Fuentes del nucleo (glob = misma fuente de verdad que el resto del repo)
# ---------------------------------------------------------------------------
file(GLOB MATRIXFS_CORE_SRC CONFIGURE_DEPENDS
    "${MATRIXFS_ROOT}/src/core/*.c"
    "${MATRIXFS_ROOT}/src/crypto/*.c"
    "${MATRIXFS_ROOT}/src/ftl/*.c"
    "${MATRIXFS_ROOT}/src/tier/*.c"
    "${MATRIXFS_ROOT}/src/sec/*.c"
    "${MATRIXFS_ROOT}/src/xio/*.c")

# El puerto (§20.2) lo aporta mfs_stm32_port.c: los envoltorios del nucleo
# quedan fuera para no duplicar simbolos mfs_port_*.
list(REMOVE_ITEM MATRIXFS_CORE_SRC
    "${MATRIXFS_ROOT}/src/core/mfs_port_arch.c"
    "${MATRIXFS_ROOT}/src/core/mfs_port_rtos.c")

# Los backends de medio son opcionales: se pueden excluir los que no se usen
# para ahorrar codigo (la HAL tampoco estara habilitada). Definir, por ejemplo,
# MATRIXFS_NO_OSPI=1 para no compilar mfs_stm32_ospi.c.
set(MATRIXFS_PORT_SRC
    "${CMAKE_CURRENT_LIST_DIR}/src/matrixfs_stm32.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_backend.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_flash_map.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_hal.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_port.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_mem.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_sd.c")
if (NOT MATRIXFS_NO_OSPI)
    list(APPEND MATRIXFS_PORT_SRC
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_ospi.c")
endif()
if (NOT MATRIXFS_NO_IFLASH)
    list(APPEND MATRIXFS_PORT_SRC
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_iflash.c")
endif()

add_library(matrixfs_stm32 STATIC
    ${MATRIXFS_CORE_SRC}
    ${MATRIXFS_PORT_SRC}
    "${MATRIXFS_ROOT}/platform/embedded/mfs_embedded.c"
    "${MATRIXFS_ROOT}/platform/common/mfs_sectors.c"
    "${MATRIXFS_ROOT}/platform/common/mfs_l2_8bit.c"
    "${MATRIXFS_ROOT}/platform/common/mfs_l2_managed.c")

target_include_directories(matrixfs_stm32 PUBLIC
    "${MATRIXFS_ROOT}/include"
    "${MATRIXFS_ROOT}/src"
    "${MATRIXFS_ROOT}/platform/embedded"
    "${MATRIXFS_ROOT}/platform/common"
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/src")

if (NOT MSVC)
    target_compile_options(matrixfs_stm32 PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 3. Presupuesto de RAM y capacidad
#
# El consumo se fija en COMPILACION: son tablas estaticas. Ver README
# §Presupuesto de RAM. Sobrescribibles desde el proyecto:
#   set(MFS_L2P_SLOTS 1024 CACHE STRING "")
#   set(MFS_ZONE_MAX  64   CACHE STRING "")
# ---------------------------------------------------------------------------
set(MFS_L2P_SLOTS 4096 CACHE STRING "Entradas del mapa L2P (potencia de dos)")
set(MFS_ZONE_MAX 128 CACHE STRING "Maximo de zonas ZLF en RAM")

target_compile_definitions(matrixfs_stm32 PUBLIC
    MFS_L2P_SLOTS=${MFS_L2P_SLOTS}
    MFS_ZONE_MAX=${MFS_ZONE_MAX}u)

# Habilitar matrixfs_stm32_mount_default() con mfs_stm32_conf.h.
option(MATRIXFS_USE_CONF "Ofrecer matrixfs_stm32_mount_default()" OFF)
if (MATRIXFS_USE_CONF)
    target_compile_definitions(matrixfs_stm32 PUBLIC MFS_STM32_USE_CONF=1)
endif()

# ---------------------------------------------------------------------------
# 4. Build de verificacion en host (sin HAL)
#
# Compila el port contra los shims de platform/stm32cube/test, de modo que el
# algoritmo de la flash interna (RMW + ECC + ventana uniforme) se prueba de
# verdad sin silicio. Se activa con -DMATRIXFS_STM32_HOST_TEST=ON.
# ---------------------------------------------------------------------------
option(MATRIXFS_STM32_HOST_TEST "Compilar y ejecutar los tests de host" OFF)
if (MATRIXFS_STM32_HOST_TEST)
    enable_testing()
    file(GLOB MATRIXFS_STM32_TEST_SRC
        "${CMAKE_CURRENT_LIST_DIR}/test/*.c"
        "${CMAKE_CURRENT_LIST_DIR}/test/shims/*.c")
    add_executable(mfs_stm32_test
        ${MATRIXFS_STM32_TEST_SRC}
        "${CMAKE_CURRENT_LIST_DIR}/src/matrixfs_stm32.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_backend.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_flash_map.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_hal.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_port.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/mfs_stm32_iflash.c"
        "${MATRIXFS_ROOT}/platform/embedded/mfs_embedded.c"
        "${MATRIXFS_ROOT}/platform/common/mfs_sectors.c"
        ${MATRIXFS_CORE_SRC})
    target_include_directories(mfs_stm32_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/test/shims"
        "${MATRIXFS_ROOT}/tests")
    target_compile_definitions(mfs_stm32_test PRIVATE MFS_STM32_HOST_TEST=1)
    add_test(NAME matrixfs_stm32_host COMMAND mfs_stm32_test)
endif()
