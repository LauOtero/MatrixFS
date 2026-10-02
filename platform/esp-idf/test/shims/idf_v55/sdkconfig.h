/* sdkconfig.h — perfil de configuracion ESP-IDF v5.5.x.
 *
 * Selecciona la version simulada de IDF. Los CONFIG_* del componente los aporta
 * el sdkconfig comun (shims/sdkconfig.h), que se incluye al final.
 */
#ifndef MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V55_H
#define MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V55_H

#ifdef MATRIXFS_SHIM_IDF_PROFILE
#if MATRIXFS_SHIM_IDF_PROFILE != 1
#error "Se incluyo el sdkconfig.h del perfil v5.5 con MATRIXFS_SHIM_IDF_PROFILE != 1: revisa el orden de los -I"
#endif
#else
#define MATRIXFS_SHIM_IDF_PROFILE 1 /* 1 = v5.x legacy, 2 = v6.x */
#endif

#ifndef ESP_IDF_VERSION_MAJOR
#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_MINOR 5
#define ESP_IDF_VERSION_PATCH 0
#define ESP_IDF_VERSION_VAL(major, minor, patch)                               \
  (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION                                                        \
  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,            \
                      ESP_IDF_VERSION_PATCH)
#endif

/* Los CONFIG_* del componente, en un unico sitio. */
#include "mfs_idf_component_config.h"

#endif /* MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V55_H */
