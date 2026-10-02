# CMakeLists.txt — componente externo de ESP-IDF para MatrixFS «ATLAS».
#
# Copyright 2026 MatrixFS contributors
#
# Licencia Apache, Version 2.0 (la "Licencia");
# no puede usar este fichero salvo en cumplimiento de la Licencia.
# Puede obtener una copia en:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Salvo que la ley aplicable exija lo contrario o se acuerde por escrito, el
# software distribuido bajo la Licencia se distribuye "TAL CUAL", SIN GARANTIAS
# NI CONDICIONES DE NINGUN TIPO, ni expresas ni implicitas. Consulte la
# Licencia para conocer el permiso y las limitaciones especificas.
# SPDX-License-Identifier: Apache-2.0
#
# ---------------------------------------------------------------------------
# Este componente NO replica el codigo del nucleo: compila las fuentes de
# MatrixFS desde el arbol del repositorio y anade su propio puerto (§20.2),
# los callbacks de flash sobre esp_partition y (opcionalmente) el registro VFS.
#
# Layout asumido del repositorio:
#   platform/esp-idf/CMakeLists.txt      <- este fichero
#   platform/esp-idf/matrixfs_esp.c      <- implementacion del componente
#   platform/esp-idf/matrixfs_esp_vfs.c  <- registro POSIX (opcional)
#   platform/esp-idf/include/            <- cabecera publica
#   src/**                               <- nucleo
#   include/                             <- API publica del nucleo
#   platform/embedded/mfs_embedded.c/.h  <- capa embebida compartida
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 1) Localizar el arbol del repositorio, con error accionable.
#
# El componente compila el nucleo, que vive FUERA de este directorio. ESP-IDF
# avisa (y segun la version rechaza) ficheros fuente fuera del directorio del
# componente, de modo que el nucleo se compila como libreria estatica propia y
# se enlaza al componente: es la forma soportada y elimina ese aviso.
# ---------------------------------------------------------------------------
get_filename_component(MATRIXFS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if (NOT EXISTS "${MATRIXFS_ROOT}/src/core/mfs_fs.c")
    message(FATAL_ERROR
        "MatrixFS: no se encuentra el arbol del repositorio.\n"
        "  Buscado en : ${MATRIXFS_ROOT}\n"
        "  Esperaba   : ${MATRIXFS_ROOT}/src/core/mfs_fs.c\n"
        "Este componente necesita el repositorio MatrixFS completo (src/,\n"
        "include/, platform/embedded/). Si copiaste SOLO platform/esp-idf/\n"
        "dentro de components/, copia o enlaza tambien el resto del arbol, o\n"
        "declara la dependencia por ruta (ver README, 'Como anadirlo').")
endif()

# ---------------------------------------------------------------------------
# 2) Fuentes del nucleo: MISMA fuente de verdad que el resto del repositorio.
#
# Se usa un glob —como el Makefile raiz y el CMakeLists raiz— en lugar de una
# lista escrita a mano. Una lista a mano ya dejo fuera src/core/mfs_arch.c, que
# define mfs_arch_detect() y mfs_arch_crc32c_hw_available(): ambas se invocan de
# forma INCONDICIONAL desde mfs_hal.c (montaje) y mfs_util.c (todo CRC), de modo
# que el componente NO ENLAZABA.
#
# OJO: NO se usa CONFIGURE_DEPENDS. ESP-IDF parsea este CMakeLists en MODO
# SCRIPT (component_get_requirements.cmake del gestor de componentes) para
# resolver dependencias, y en ese modo `CONFIGURE_DEPENDS` es un ERROR
# ("CONFIGURE_DEPENDS is invalid for script and find package modes"), que
# aborta el build. El glob sin CONFIGURE_DEPENDS recoge igualmente todos los
# ficheros del nucleo en cada configuracion (que es cuando importa); la unica
# contrapartida es que anadir un .c nuevo exige reconfigurar (borrar build/ o
# tocar este fichero), algo que en la practica ya ocurre al actualizar el repo.
# ---------------------------------------------------------------------------
file(GLOB MATRIXFS_CORE_SRC
    "${MATRIXFS_ROOT}/src/core/*.c"
    "${MATRIXFS_ROOT}/src/crypto/*.c"
    "${MATRIXFS_ROOT}/src/ftl/*.c"
    "${MATRIXFS_ROOT}/src/tier/*.c"
    "${MATRIXFS_ROOT}/src/sec/*.c"
    "${MATRIXFS_ROOT}/src/xio/*.c")

# Los envoltorios del contrato de puerto (§20.2) los aporta ESTE componente
# (matrixfs_esp.c). Los del nucleo quedan fuera para no duplicar simbolos:
# el nucleo no los llama nunca (verificado), solo definen API propia.
list(REMOVE_ITEM MATRIXFS_CORE_SRC
    "${MATRIXFS_ROOT}/src/core/mfs_port_arch.c"
    "${MATRIXFS_ROOT}/src/core/mfs_port_rtos.c")

if (NOT MATRIXFS_CORE_SRC)
    message(FATAL_ERROR
        "MatrixFS: el glob del nucleo no encontro ninguna fuente en "
        "${MATRIXFS_ROOT}/src. Revisa el arbol del repositorio.")
endif()

# ---------------------------------------------------------------------------
# 3) Fuentes propias del componente.
# ---------------------------------------------------------------------------
set(MATRIXFS_ESP_SRCS "matrixfs_esp.c")
if (CONFIG_MATRIXFS_VFS_ENABLE)
    list(APPEND MATRIXFS_ESP_SRCS "matrixfs_esp_vfs.c")
endif()

# Todas las dependencias de IDF son PRIVADAS: la cabecera publica del
# componente solo expone tipos de MatrixFS, no de IDF. Declararlas como
# REQUIRES obligaba a los consumidores a heredar los include dirs de IDF.
idf_component_register(
    SRCS
        ${MATRIXFS_ESP_SRCS}
    INCLUDE_DIRS
        "include"
        "${MATRIXFS_ROOT}/include"
        # `mf_t` es OPACO en include/matrixfs: para declarar una instancia el
        # consumidor incluye "mfs_internal.h" (src/), igual que hace
        # platform/common/mfs_vfs.c. Por eso src/ es publico.
        "${MATRIXFS_ROOT}/src"
        "${MATRIXFS_ROOT}/platform/embedded"
    PRIV_INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        esp_partition
        spi_flash
        bootloader_support
        esp_timer
        esp_hw_support
        freertos
        log
        vfs)

# ---------------------------------------------------------------------------
# 4) Nucleo + capa embebida como libreria estatica enlazada al componente.
# ---------------------------------------------------------------------------
add_library(matrixfs_core STATIC
    ${MATRIXFS_CORE_SRC}
    "${MATRIXFS_ROOT}/platform/embedded/mfs_embedded.c")

target_include_directories(matrixfs_core PUBLIC
    "${MATRIXFS_ROOT}/include"
    "${MATRIXFS_ROOT}/src"
    "${MATRIXFS_ROOT}/platform/embedded")

target_compile_options(matrixfs_core PRIVATE -Wall -Wextra)

# NO se define MFS_PORT_RTOS_GLUE ni MFS_PORT_ARCH_GLUE: el puerto (§20.2) lo
# aporta matrixfs_esp.c. El nucleo usa `#if defined(...)`, de modo que definir
# cualquiera de los dos —incluso a 0— produciria simbolos mfs_port_* duplicados.

# ---------------------------------------------------------------------------
# 5) Presupuesto de RAM y capacidad (Kconfig).
#
# El consumo del nucleo se fija en COMPILACION, no en ejecucion: las tablas
# estaticas (mapa L2P, ventana de zonas, scratch de pagina) se dimensionan con
# estos defines. `CONFIG_MATRIXFS_RAM_BUDGET` solo alimenta al planificador de
# viabilidad (seleccion de modo), NO reserva memoria. Ver README §RAM.
#
# IMPORTANTE: los mismos defines deben aplicarse al componente, no solo al
# nucleo. MFS_ZONE_MAX lo usa tambien matrixfs_esp.c para calcular la capacidad
# util; si el componente y el nucleo discreparan, el diagnostico mentiria.
# ---------------------------------------------------------------------------
set(MATRIXFS_CAPACITY_DEFS "")
if (CONFIG_MATRIXFS_L2P_SLOTS)
    list(APPEND MATRIXFS_CAPACITY_DEFS
        MFS_L2P_SLOTS=${CONFIG_MATRIXFS_L2P_SLOTS})
endif()
if (CONFIG_MATRIXFS_ZONE_MAX)
    list(APPEND MATRIXFS_CAPACITY_DEFS
        MFS_ZONE_MAX=${CONFIG_MATRIXFS_ZONE_MAX}u)
endif()
# Limites de RAM sobrescribibles del nucleo (MFS_MAX_FILES_OPEN,
# MFS_WAL_WINDOW_MAX y MFS_SCRATCH_MAX). Sin estas lineas, esos macros se
# quedaban en su valor por defecto y el menuconfig no podia recortar el .bss.
if (CONFIG_MATRIXFS_MAX_FILES_OPEN)
    list(APPEND MATRIXFS_CAPACITY_DEFS
        MFS_MAX_FILES_OPEN=${CONFIG_MATRIXFS_MAX_FILES_OPEN}u)
endif()
if (CONFIG_MATRIXFS_WAL_WINDOW_MAX)
    list(APPEND MATRIXFS_CAPACITY_DEFS
        MFS_WAL_WINDOW_MAX=${CONFIG_MATRIXFS_WAL_WINDOW_MAX}u)
endif()
if (CONFIG_MATRIXFS_SCRATCH_MAX)
    list(APPEND MATRIXFS_CAPACITY_DEFS
        MFS_SCRATCH_MAX=${CONFIG_MATRIXFS_SCRATCH_MAX}u)
endif()

if (MATRIXFS_CAPACITY_DEFS)
    target_compile_definitions(matrixfs_core PRIVATE ${MATRIXFS_CAPACITY_DEFS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${MATRIXFS_CAPACITY_DEFS})
endif()

# El componente arrastra el nucleo a quien lo consuma.
target_link_libraries(${COMPONENT_LIB} INTERFACE matrixfs_core)
