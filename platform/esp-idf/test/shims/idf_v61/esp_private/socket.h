/* esp_private/socket.h — shim de la cabecera PRIVADA de IDF v6.1.
 *
 * En v6.1, `esp_vfs_register_fd_range` dejo de estar en la cabecera publica
 * `esp_vfs.h` y se movio a `components/vfs/include/esp_private/socket.h` con
 * una firma nueva (verificado en la rama v6.1):
 *
 *   v5.5 (publica):  esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs,
 *                                                        void *ctx, int min_fd,
 *                                                        int max_fd);
 *   v6.1 (privada):  esp_err_t esp_vfs_register_fd_range(
 *                            const esp_vfs_fs_ops_t *vfs, int flags, void *ctx,
 *                            int min_fd, int max_fd);
 *
 * El shim NO puede dar dos definiciones del mismo nombre con firmas distintas,
 * de modo que la variante de v6 se expone aqui como
 * `esp_vfs_register_range_fsops` (sufijo que indica el primer parametro).
 * El contrato que se conserva es el que importa: mismo nombre en IDF, tipo
 * distinto, cabecera distinta y API privada.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_PRIVATE_SOCKET_H
#define MATRIXFS_TEST_SHIM_ESP_PRIVATE_SOCKET_H

#include "esp_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Equivalente a la `esp_vfs_register_fd_range` privada de v6.1. */
esp_err_t esp_vfs_register_range_fsops(const esp_vfs_fs_ops_t *vfs,
                                         int flags, void *ctx, int min_fd,
                                         int max_fd);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_PRIVATE_SOCKET_H */
