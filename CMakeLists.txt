# MatrixFS Ultra «ATLAS» v1.0 — CMake (§26)
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
