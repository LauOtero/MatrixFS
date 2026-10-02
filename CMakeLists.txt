# MatrixFS «ATLAS» v1.0 — CMake (§26)
cmake_minimum_required(VERSION 3.15)
project(matrixfs_ultra C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if (MSVC)
  add_compile_options(/W4)          # MSVC no entiende -Wall/-Wextra
  # strcpy/sscanf son parte de C11 y aquí se usan con cotas verificadas; el
  # aviso C4996 es una recomendación de la CRT de Microsoft, no un defecto.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

file(GLOB CORE_SRC
  ${CMAKE_SOURCE_DIR}/src/core/*.c
  ${CMAKE_SOURCE_DIR}/src/crypto/*.c
  ${CMAKE_SOURCE_DIR}/src/ftl/*.c
  ${CMAKE_SOURCE_DIR}/src/tier/*.c
  ${CMAKE_SOURCE_DIR}/src/sec/*.c
  ${CMAKE_SOURCE_DIR}/src/xio/*.c)
file(GLOB SIM_SRC  ${CMAKE_SOURCE_DIR}/sim/*.c)
file(GLOB TEST_SRC ${CMAKE_SOURCE_DIR}/tests/*.c)

# Capa portable (§21): driver de bloque, puerto de plataforma y adaptador VFS.
# Se lista explícitamente: platform/common también contiene la CLI
# (mfs_vfsctl.c, con su propio main) que es un ejecutable aparte.
set(PLAT_SRC
  ${CMAKE_SOURCE_DIR}/platform/common/mfs_plat.c
  ${CMAKE_SOURCE_DIR}/platform/common/mfs_blk.c
  ${CMAKE_SOURCE_DIR}/platform/common/mfs_l2_managed.c
  ${CMAKE_SOURCE_DIR}/platform/common/mfs_vfs.c)

# --- Núcleo (sin puerto de host: lo aporta el integrador en target) ---
add_library(matrixfs STATIC ${CORE_SRC})
target_include_directories(matrixfs PUBLIC
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# --- Suite de verificación sobre vFlash (host) ---
add_executable(mfs_tests ${TEST_SRC} ${CORE_SRC} ${SIM_SRC} ${PLAT_SRC})
target_include_directories(mfs_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/sim ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}/platform/common)

# --- mfstool (§26) ---
add_executable(mfstool tools/mfstool.c ${CORE_SRC} ${SIM_SRC})
target_include_directories(mfstool PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/sim)

# --- matrixfs-ctl: CLI de administración (misma capa VFS que los front-ends) ---
add_executable(mfsctl ${CMAKE_SOURCE_DIR}/platform/common/mfs_vfsctl.c
               ${CORE_SRC} ${SIM_SRC} ${PLAT_SRC})
target_include_directories(mfsctl PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/sim
  ${CMAKE_SOURCE_DIR}/platform/common)

enable_testing()
add_test(NAME matrixfs_suite COMMAND mfs_tests)

# --- Drivers L2 para MCU (opcional) ---
# El puerto (§20.2) y el modelo de arquitectura/aceleración HW viven en el
# NÚCLEO: src/core/mfs_port_arch.c y src/core/mfs_arch.c (ya incluidos en
# CORE_SRC). Esta librería sólo aporta los drivers de dispositivo (NOR/FRAM/
# EEPROM SPI-I2C, flash interna del MCU y SD-SPI), compartidos por objetivos de
# 8/16/32 bits. Para cross-compilar a 8 bits, definir MFS_ALLOW_8BIT_TARGET=1
# (MFS-ARCH-010 rev.3).
option(MATRIXFS_BUILD_MCU "Compila los drivers L2 para MCU" OFF)
if (MATRIXFS_BUILD_MCU)
  add_library(matrixfs_mcu STATIC
    ${CMAKE_SOURCE_DIR}/platform/common/mfs_l2_8bit.c)
  target_include_directories(matrixfs_mcu PUBLIC
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/platform/common)
endif()

# --- Verificación de los ports de plataforma EN HOST (opcional) ---
# Compila el código real de platform/esp-idf y platform/stm32cube contra el
# núcleo real y los shims de SDK, y ejecuta las suites. Es la forma de verificar
# estas integraciones sin los SDK instalados; con `make platform-test` se hace lo
# mismo sin CMake.
#
#   cmake -DMATRIXFS_PLATFORM_TESTS=ON ..
#
# Se exige GCC/Clang porque los shims usan -include para reclamar la guarda de
# <dirent.h> (el struct dirent de MinGW no lleva d_type).
option(MATRIXFS_PLATFORM_TESTS "Compila y ejecuta los tests de los ports de plataforma" OFF)
if (MATRIXFS_PLATFORM_TESTS)
  if (MSVC)
    message(FATAL_ERROR "MATRIXFS_PLATFORM_TESTS requiere GCC o Clang (los shims usan -include).")
  endif()

  set(MFS_CORE_NOPORT ${CORE_SRC})
  list(REMOVE_ITEM MFS_CORE_NOPORT
    ${CMAKE_SOURCE_DIR}/src/core/mfs_port_arch.c
    ${CMAKE_SOURCE_DIR}/src/core/mfs_port_rtos.c)

  add_executable(mfs_esp_test
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/test/test_esp_idf.c
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/test/esp_partition_fake.c
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/test/shims/idf_shim.c
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/matrixfs_esp.c
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/matrixfs_esp_vfs.c
    ${CMAKE_SOURCE_DIR}/platform/embedded/mfs_embedded.c
    ${MFS_CORE_NOPORT})
  target_include_directories(mfs_esp_test PRIVATE
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/test/shims
    ${CMAKE_SOURCE_DIR}/platform/esp-idf
    ${CMAKE_SOURCE_DIR}/platform/esp-idf/include
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/platform/embedded ${CMAKE_SOURCE_DIR}/tests)
  # Perfil de IDF emulado: 1 = v5.x, 2 = v6.x (el banco soporta ambos).
  target_compile_definitions(mfs_esp_test PRIVATE MATRIXFS_SHIM_IDF_PROFILE=1)
  target_compile_options(mfs_esp_test PRIVATE
    -include ${CMAKE_SOURCE_DIR}/platform/esp-idf/test/shims/esp_vfs.h)
  add_test(NAME matrixfs_esp_idf_host COMMAND mfs_esp_test)

  # STM32Cube: cuatro variantes de unidad de programacion y ECC.
  foreach (variant "2:0" "4:0" "8:1" "16:1")
    string(REPLACE ":" ";" parts "${variant}")
    list(GET parts 0 unit)
    list(GET parts 1 ecc)
    set(tname "mfs_stm32_test_u${unit}_e${ecc}")
    add_executable(${tname}
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/test/test_stm32.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/test/stubs_backends.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/test/shims/stm32_hal_shim.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/matrixfs_stm32.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/mfs_stm32_backend.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/mfs_stm32_flash_map.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/mfs_stm32_hal.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/mfs_stm32_port.c
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src/mfs_stm32_iflash.c
      ${CMAKE_SOURCE_DIR}/platform/embedded/mfs_embedded.c
      ${CMAKE_SOURCE_DIR}/platform/common/mfs_sectors.c
      ${CMAKE_SOURCE_DIR}/platform/common/mfs_l2_8bit.c
      ${CMAKE_SOURCE_DIR}/platform/common/mfs_l2_managed.c
      ${MFS_CORE_NOPORT})
    target_include_directories(${tname} PRIVATE
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/test/shims
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/include
      ${CMAKE_SOURCE_DIR}/platform/stm32cube/src
      ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/platform/embedded ${CMAKE_SOURCE_DIR}/platform/common
      ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(${tname} PRIVATE
      MFS_STM32_HOST_TEST=1
      MFS_STM32_HOST_PGM_UNIT=${unit}
      MFS_STM32_HOST_ECC=${ecc})
    add_test(NAME matrixfs_stm32_host_u${unit}_e${ecc} COMMAND ${tname})
  endforeach()
endif()