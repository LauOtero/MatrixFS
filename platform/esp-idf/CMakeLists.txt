# CMakeLists.txt — componente externo de ESP-IDF para MatrixFS Ultra «ATLAS».
#
# Copyright 2026 MatrixFS Ultra contributors
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
# MatrixFS directamente desde el arbol del repositorio. Por eso todas las rutas
# de fuentes son relativas al propio componente (../../src/..., ../../platform/
# embedded), de modo que la carpeta `components/matrixfs/` pueda ser un enlace o
# copia del directorio platform/esp-idf dentro de un proyecto ESP-IDF.
#
# Layout asumido del repositorio:
#   platform/esp-idf/CMakeLists.txt      <- este fichero
#   platform/esp-idf/matrixfs_esp.c      <- implementacion del componente
#   platform/esp-idf/include/            <- cabecera publica
#   src/**                               <- nucleo
#   include/                             <- API publica del nucleo
#   platform/embedded/mfs_embedded.c/.h  <- capa embebida compartida
#
# Las primitivas de puerto (mfs_port_*) las aporta matrixfs_esp.c; se compilan
# UNA sola vez con este componente (ver README, seccion "Primitivas de puerto").
# ---------------------------------------------------------------------------

# `../../src` se expone como directorio PUBLICO porque la API del componente
# recibe/entrega una instancia `mf_t`, que es OPACA en include/matrixfs: para
# declararla el consumidor incluye "mfs_internal.h" (src/), igual que hace
# platform/common/mfs_vfs.c. El nucleo ya lo publica asi en el CMake raiz.
set(MATRIXFS_INCLUDE_DIRS
    "include"
    "../../include"
    "../../src"
    "../../platform/embedded")

idf_component_register(
    SRCS
        # Implementacion del componente + primitivas de puerto ESP-IDF.
        "matrixfs_esp.c"

        # Capa de integracion embebida compartida (adaptador de callbacks).
        "../../platform/embedded/mfs_embedded.c"

        # Nucleo: core / crypto / ftl / tier / sec / xio.
        "../../src/core/mfs_compact.c"
        "../../src/core/mfs_fs.c"
        "../../src/core/mfs_hal.c"
        "../../src/core/mfs_profile.c"
        "../../src/core/mfs_util.c"
        "../../src/core/mfs_wal.c"
        "../../src/core/mfs_zone.c"
        "../../src/crypto/mfs_blake3.c"
        "../../src/crypto/mfs_sha256.c"
        "../../src/crypto/mfs_suites.c"
        "../../src/ftl/mfs_ftl2.c"
        "../../src/tier/mfs_hmt.c"
        "../../src/sec/mfs_pq.c"
        "../../src/xio/mfs_xio.c"
    INCLUDE_DIRS
        ${MATRIXFS_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS
        "."          # cabeceras privadas del componente (hoy ninguna)
    REQUIRES
        esp_partition
        esp_timer)
