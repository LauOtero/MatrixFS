# CMakeLists.txt — build del soporte Windows (WinFsp) de MatrixFS «ATLAS».
#
# Requisitos:
#   · Visual Studio 2019/2022 (MSVC, x64) o LLVM/clang-cl, y CMake >= 3.20
#   · WinFsp 1.12+ con sus cabeceras y bibliotecas (https://winfsp.dev)
#
# Configuración y compilación:
#   cmake -B build -A x64
#   cmake --build build --config Release
#
# La localización del SDK de WinFsp sigue este orden:
#   1) variable de entorno WINFSP (p. ej. C:\Program Files (x86)\WinFsp)
#   2) rutas estándar de instalación del programa
#
# Artefactos (en build\Release):
#   matrixfs_winfsp.exe     front-end WinFsp (monta una unidad con letra)
#   matrixfs-ctl.exe        herramienta de administración/validación
#   matrixfs_automount.exe  servicio de automontaje

cmake_minimum_required(VERSION 3.20)

project(MatrixFS_Ultra_Windows
        VERSION 1.0.0
        DESCRIPTION "MatrixFS «ATLAS» — soporte Windows (WinFsp)"
        LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR
            "Este proyecto sólo compila en Windows (requiere el SDK de WinFsp).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Los front-ends usan las variantes seguras de la CRT (wcscpy_s, _snwprintf_s).
#
# NO se define WIN32_LEAN_AND_MEAN: la cabecera de WinFsp necesita que
# <windows.h> incluya <winternl.h> sin recortar para disponer de
# NTSTATUS/PNTSTATUS (véase la nota en matrixfs_winfsp.c).
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _FILE_OFFSET_BITS=64)

# Dimensión del mapa L2P (tabla estática en RAM, sin heap). El valor por defecto
# del núcleo (4096) es el del objetivo embebido y sólo permite mapear ~16 MiB
# con chunk 4096; con 2^18 entradas (2 MiB de .bss) se cubre el volumen máximo
# del modo Extended (512 MiB). Debe coincidir con el valor de Linux
# (platform/linux/Makefile) para que ambos lean el mismo layout.
if(NOT DEFINED MFS_L2P_SLOTS)
    set(MFS_L2P_SLOTS 262144)
endif()
add_compile_definitions(MFS_L2P_SLOTS=${MFS_L2P_SLOTS})

get_filename_component(MFS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# Localización del SDK de WinFsp
# ---------------------------------------------------------------------------
set(WINFSP_HINTS
    "$ENV{WINFSP}"
    "C:/Program Files (x86)/WinFsp"
    "C:/Program Files/WinFsp")

find_path(WINFSP_INCLUDE_DIR
          NAMES winfsp/winfsp.h
          HINTS ${WINFSP_HINTS}
          PATH_SUFFIXES inc include)
find_library(WINFSP_LIBRARY
             NAMES winfsp-x64 winfsp
             HINTS ${WINFSP_HINTS}
             PATH_SUFFIXES lib bin)

if(NOT WINFSP_INCLUDE_DIR OR NOT WINFSP_LIBRARY)
    message(FATAL_ERROR
            "No se encuentra el SDK de WinFsp.\n"
            "  Instale WinFsp desde https://winfsp.dev y, si lo instaló en una "
            "ruta no estándar, defina la variable de entorno WINFSP apuntando a "
            "su directorio raíz (el que contiene inc\\ y lib\\).")
endif()
message(STATUS "WinFsp: cabeceras en ${WINFSP_INCLUDE_DIR}")
message(STATUS "WinFsp: biblioteca en ${WINFSP_LIBRARY}")

# ---------------------------------------------------------------------------
# Fuentes comunes (núcleo + capa de integración portable)
# ---------------------------------------------------------------------------
file(GLOB MFS_CORE_SRC
     "${MFS_ROOT}/src/core/*.c"
     "${MFS_ROOT}/src/crypto/*.c"
     "${MFS_ROOT}/src/ftl/*.c"
     "${MFS_ROOT}/src/tier/*.c"
     "${MFS_ROOT}/src/sec/*.c"
     "${MFS_ROOT}/src/xio/*.c")

# Puerto POSIX/Windows del proyecto (contador monotónico y tiempo).
set(MFS_PORT_SRC "${MFS_ROOT}/sim/mfs_port_host.c")

# Capa de integración compartida con el front-end FUSE de Linux.
set(MFS_PLAT_SRC
    "${MFS_ROOT}/platform/common/mfs_plat.c"
    "${MFS_ROOT}/platform/common/mfs_blk.c"
    "${MFS_ROOT}/platform/common/mfs_vfs.c")

set(MFS_COMMON ${MFS_CORE_SRC} ${MFS_PORT_SRC} ${MFS_PLAT_SRC})

set(MFS_INCLUDES
    "${MFS_ROOT}/include"
    "${MFS_ROOT}/src"
    "${MFS_ROOT}/platform/common")

# ---------------------------------------------------------------------------
# matrixfs-ctl — herramienta de administración y validación (sin WinFsp)
# ---------------------------------------------------------------------------
add_executable(matrixfs-ctl
    "${MFS_ROOT}/platform/common/mfs_vfsctl.c"
    ${MFS_COMMON})
target_include_directories(matrixfs-ctl PRIVATE ${MFS_INCLUDES})
if(MSVC)
    target_compile_options(matrixfs-ctl PRIVATE /W4)
else()
    target_compile_options(matrixfs-ctl PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# matrixfs_winfsp — front-end WinFsp (unidad con letra en el Explorador)
# ---------------------------------------------------------------------------
add_executable(matrixfs_winfsp
    "${CMAKE_CURRENT_SOURCE_DIR}/matrixfs_winfsp.c"
    ${MFS_COMMON})
target_include_directories(matrixfs_winfsp PRIVATE
    ${MFS_INCLUDES} ${WINFSP_INCLUDE_DIR})
target_link_libraries(matrixfs_winfsp PRIVATE ${WINFSP_LIBRARY})
if(MSVC)
    # C4324 («estructura rellenada por el especificador de alineación») lo emite
    # la propia cabecera del SDK de WinFsp (<winfsp/fsctl.h>), no el código del
    # proyecto: no es corregible desde aquí.
    target_compile_options(matrixfs_winfsp PRIVATE /W4 /wd4324)
else()
    target_compile_options(matrixfs_winfsp PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# matrixfs_automount — servicio de detección y automontaje
# ---------------------------------------------------------------------------
add_executable(matrixfs_automount
    "${CMAKE_CURRENT_SOURCE_DIR}/service/matrixfs_automount.c"
    ${MFS_COMMON})
target_include_directories(matrixfs_automount PRIVATE ${MFS_INCLUDES})
target_link_libraries(matrixfs_automount PRIVATE Advapi32)
if(MSVC)
    target_compile_options(matrixfs_automount PRIVATE /W4)
else()
    target_compile_options(matrixfs_automount PRIVATE -Wall -Wextra)
endif()

install(TARGETS matrixfs_winfsp matrixfs-ctl matrixfs_automount
        RUNTIME DESTINATION .)
