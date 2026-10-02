/* sdkconfig.h — SELECCION DE PERFIL del shim de ESP-IDF.
 *
 * Este fichero vive en el directorio del shim COMPARTIDO y solo deberia
 * alcanzarse si la linea de compilacion no puso un perfil por delante. En ese
 * caso NO se adivina la version: se para con un mensaje accionable, porque
 * asumir un perfil produciria un falso verde (el shim diria "compila" contra una
 * version de IDF distinta de la que se cree).
 *
 * Uso correcto (el directorio del perfil SIEMPRE el primero):
 *   -Iplatform/esp-idf/test/shims/idf_v55 -Iplatform/esp-idf/test/shims ...
 *   -Iplatform/esp-idf/test/shims/idf_v61 -Iplatform/esp-idf/test/shims ...
 */
#ifndef MATRIXFS_TEST_SHIM_SDKCONFIG_H
#define MATRIXFS_TEST_SHIM_SDKCONFIG_H

#ifndef MATRIXFS_SHIM_IDF_PROFILE
#error "Perfil de ESP-IDF no seleccionado: pon -Iplatform/esp-idf/test/shims/idf_v55 (o .../idf_v61) ANTES de -Iplatform/esp-idf/test/shims"
#endif

#include "mfs_idf_component_config.h"

#endif /* MATRIXFS_TEST_SHIM_SDKCONFIG_H */
