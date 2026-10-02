/* sdkconfig.h — perfil de configuracion ESP-IDF v6.1.x. */
#ifndef MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V61_H
#define MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V61_H

#ifdef MATRIXFS_SHIM_IDF_PROFILE
#if MATRIXFS_SHIM_IDF_PROFILE != 2
#error "Se incluyo el sdkconfig.h del perfil v6.1 con MATRIXFS_SHIM_IDF_PROFILE != 2: revisa el orden de los -I"
#endif
#else
#define MATRIXFS_SHIM_IDF_PROFILE 2 /* 1 = v5.x legacy, 2 = v6.x */
#endif

#ifndef ESP_IDF_VERSION_MAJOR
#define ESP_IDF_VERSION_MAJOR 6
#define ESP_IDF_VERSION_MINOR 1
#define ESP_IDF_VERSION_PATCH 0
#define ESP_IDF_VERSION_VAL(major, minor, patch)                               \
  (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION                                                        \
  ESP_IDF_VERSION_VAL(ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,            \
                      ESP_IDF_VERSION_PATCH)
#endif

#include "mfs_idf_component_config.h"

#endif /* MATRIXFS_TEST_SHIM_PROFILE_SDKCONFIG_V61_H */
