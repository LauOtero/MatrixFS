/* mfs_idf_contract_profile.h — CONTRATO ABI/API del perfil de ESP-IDF activo.
 *
 * Aqui se declaran los valores y tipos que DIFEREN entre IDF v5.x y v6.x y que
 * el componente (o el shim) deben respetar. Si el shim se equivoca con un
 * perfil, estas comprobaciones fallan en COMPILACION en lugar de dejar pasar un
 * falso verde: es lo que hace que probar dos perfiles tenga valor.
 *
 * Perfil 5.x:
 *   · ESP_VFS_FLAG_DEFAULT == 0            (en v6.1 es (1 << 0))
 *   · ESP_VFS_FLAG_CONTEXT_PTR == (1 << 1) en ambos
 *   · esp_vfs_register_fd_range(const esp_vfs_t *, void *, int, int) publica
 * Perfil 6.x:
 *   · ESP_VFS_FLAG_DEFAULT == (1 << 0)
 *   · esp_vfs_register_fd_range es PRIVADA (esp_private/socket.h) y su firma
 *     cambia a (const esp_vfs_fs_ops_t *, int flags, void *ctx, int, int)
 *   · ESP_VFS_FLAG_STATIC == (1 << 3)
 */
#ifndef MATRIXFS_TEST_SHIM_CONTRACT_PROFILE_H
#define MATRIXFS_TEST_SHIM_CONTRACT_PROFILE_H

#if MATRIXFS_SHIM_IDF_PROFILE == 1

_Static_assert(ESP_VFS_FLAG_DEFAULT == 0,
               "perfil IDF 5.x: ESP_VFS_FLAG_DEFAULT debe ser 0");
_Static_assert(ESP_VFS_FLAG_CONTEXT_PTR == (1 << 1),
               "perfil IDF 5.x: ESP_VFS_FLAG_CONTEXT_PTR debe ser (1 << 1)");
_Static_assert(ESP_VFS_FLAG_STATIC == (1 << 3),
               "perfil IDF 5.x: ESP_VFS_FLAG_STATIC debe ser (1 << 3)");
_Static_assert(ESP_IDF_VERSION_MAJOR == 5,
               "perfil IDF 5.x: ESP_IDF_VERSION_MAJOR debe ser 5");

#elif MATRIXFS_SHIM_IDF_PROFILE == 2

_Static_assert(ESP_VFS_FLAG_DEFAULT == (1 << 0),
               "perfil IDF 6.x: ESP_VFS_FLAG_DEFAULT debe ser (1 << 0)");
_Static_assert(ESP_VFS_FLAG_CONTEXT_PTR == (1 << 1),
               "perfil IDF 6.x: ESP_VFS_FLAG_CONTEXT_PTR debe ser (1 << 1)");
_Static_assert(ESP_VFS_FLAG_STATIC == (1 << 3),
               "perfil IDF 6.x: ESP_VFS_FLAG_STATIC debe ser (1 << 3)");
_Static_assert(ESP_IDF_VERSION_MAJOR == 6,
               "perfil IDF 6.x: ESP_IDF_VERSION_MAJOR debe ser 6");

#elif defined(MATRIXFS_SHIM_IDF_PROFILE)

#error "MATRIXFS_SHIM_IDF_PROFILE debe ser 1 (v5.x) o 2 (v6.x)"

#else

#error "Falta el perfil de IDF: compila con -Iplatform/esp-idf/test/shims/idf_v55 o -Iplatform/esp-idf/test/shims/idf_v61 ANTES de -Iplatform/esp-idf/test/shims (su sdkconfig.h define MATRIXFS_SHIM_IDF_PROFILE)"

#endif

#endif /* MATRIXFS_TEST_SHIM_CONTRACT_PROFILE_H */
