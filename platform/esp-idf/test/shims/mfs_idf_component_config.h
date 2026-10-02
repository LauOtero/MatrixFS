/* mfs_idf_component_config.h — CONFIG_* del componente MatrixFS, comunes a los
 * perfiles de version de IDF (shims/idf_v55/sdkconfig.h y shims/idf_v61/...).
 *
 * Cada valor se puede sobrescribir con -DCONFIG_...=... en la linea de
 * compilacion. CONFIG_MATRIXFS_ENABLE_CRYPTO queda SIN DEFINIR a proposito: el
 * componente lo usa con `#if CONFIG_MATRIXFS_ENABLE_CRYPTO`, de modo que un
 * macro inexistente se evalua como 0 y la ruta de clave queda fuera. Para
 * activarla:
 *   -DCONFIG_MATRIXFS_ENABLE_CRYPTO=1
 * (y, si se quiere una clave distinta, -DCONFIG_MATRIXFS_CRYPTO_KEY_HEX=...).
 */
#ifndef MATRIXFS_TEST_SHIM_COMPONENT_CONFIG_H
#define MATRIXFS_TEST_SHIM_COMPONENT_CONFIG_H

#ifndef CONFIG_MATRIXFS_PARTITION_LABEL
#define CONFIG_MATRIXFS_PARTITION_LABEL "matrixfs"
#endif

#ifndef CONFIG_MATRIXFS_RAM_BUDGET
#define CONFIG_MATRIXFS_RAM_BUDGET 32768
#endif

#ifndef CONFIG_MATRIXFS_THREAD_SAFE
#define CONFIG_MATRIXFS_THREAD_SAFE 1
#endif

#ifndef CONFIG_MATRIXFS_VFS_ENABLE
#define CONFIG_MATRIXFS_VFS_ENABLE 1
#endif

#ifndef CONFIG_MATRIXFS_VFS_MOUNT_POINT
#define CONFIG_MATRIXFS_VFS_MOUNT_POINT "/matrixfs"
#endif

#ifndef CONFIG_MATRIXFS_VFS_MAX_FILES
#define CONFIG_MATRIXFS_VFS_MAX_FILES 8
#endif

#ifndef CONFIG_MATRIXFS_PAGE_SIZE
#define CONFIG_MATRIXFS_PAGE_SIZE 256
#endif

#ifndef CONFIG_MATRIXFS_T_PROG_MAX_US
#define CONFIG_MATRIXFS_T_PROG_MAX_US 700
#endif

#ifndef CONFIG_MATRIXFS_T_ERASE_MAX_US
#define CONFIG_MATRIXFS_T_ERASE_MAX_US 45000
#endif

#ifndef CONFIG_MATRIXFS_T_READ_MAX_US
#define CONFIG_MATRIXFS_T_READ_MAX_US 100
#endif

/* Ruta de cifrado del nucleo (desactivada por defecto). Solo se usa si se
 * compila con -DCONFIG_MATRIXFS_ENABLE_CRYPTO=1; el valor es una clave de
 * PRUEBA. */
#ifndef CONFIG_MATRIXFS_CRYPTO_KEY_HEX
#define CONFIG_MATRIXFS_CRYPTO_KEY_HEX                                          \
  "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#endif

/* Flag de IDF que el componente usa en el inicializador designado de su
 * `esp_vfs_t`. En IDF v6 es necesario ademas marcar el contexto como estatico;
 * el shim no lo necesita, pero se declara para que el codigo del componente
 * pueda usarlo si se quiere anotar. */
#ifndef ESP_VFS_FLAG_STATIC
#define ESP_VFS_FLAG_STATIC (1 << 3)
#endif

#endif /* MATRIXFS_TEST_SHIM_COMPONENT_CONFIG_H */
