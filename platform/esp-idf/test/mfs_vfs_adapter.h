/* mfs_vfs_adapter.h — contrato de version del registro VFS para MatrixFS.
 *
 * El componente (platform/esp-idf/matrixfs_esp_vfs.c) registra el volumen con
 * `esp_vfs_register_fd_range(&s_vfs, NULL, base, max)`. Esa llamada es correcta
 * en ESP-IDF v5.x, donde la funcion es PUBLICA y tiene esa firma, y NO compila
 * en v6.x, donde:
 *   · la funcion se movio a `esp_private/socket.h`, y
 *   · su firma cambio a
 *     `(const esp_vfs_fs_ops_t *vfs, int flags, void *ctx, int min, int max)`.
 *
 * Este encabezado existe para que la MISMA linea del componente funcione en
 * ambas series. DEBE incluirse ANTES de cualquier cabecera de ESP-IDF que
 * declare `esp_vfs_register_fd_range` (en particular antes de `esp_vfs.h`),
 * porque en el perfil 6.x define la macro que traduce la llamada al adaptador.
 *
 * Uso (una sola linea, al principio de la unidad de traduccion o del fichero
 * que la incluya primero):
 *
 *     #include "mfs_vfs_adapter.h"   // -> renombra la llamada en v6.x
 *     #include "matrixfs_esp_vfs.h"  // -> .../esp_vfs.h
 *
 * Aplicado a `matrixfs_esp_vfs.c` no hay que tocar nada mas: la llamada
 * `.flags = ESP_VFS_FLAG_CONTEXT_PTR` y el resto de la tabla siguen igual.
 *
 * NO se usa ninguna API privada en el camino de v6.x: se emplea
 * `esp_vfs_register_with_id()` + `esp_vfs_register_fd()`, ambas publicas.
 */
#ifndef MATRIXFS_TEST_VFS_ADAPTER_H
#define MATRIXFS_TEST_VFS_ADAPTER_H

#if defined(MATRIXFS_SHIM_IDF_PROFILE) && MATRIXFS_SHIM_IDF_PROFILE >= 2

/* Renombra la llamada del componente a la implementacion del adaptador. Se
 * define ANTES de incluir esp_vfs.h para que la sustitucion alcance al codigo
 * del componente, sin tocar su fuente. */
#ifndef esp_vfs_register_fd_range
#define esp_vfs_register_fd_range mfs_esp_vfs_register_fd_range
#endif

#include <sys/types.h>

#include "esp_err.h"

/* Replica de la cabecera PRIVADA de v6.1 (esp_private/socket.h) para que el
 * tipo `esp_vfs_fs_ops_t` este disponible si el llamador lo necesita. */
#include "esp_private/socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registra el VFS (variante legacy `esp_vfs_t`, vigente en v6.1) y reserva el
 * intervalo de descriptores <min_fd; max_fd) uno a uno con la API publica.
 *
 * Devuelve ESP_OK, ESP_ERR_INVALID_ARG si los limites no son validos, o el
 * error de `esp_vfs_register_with_id`/`esp_vfs_register_fd`; en caso de fallo
 * desregistra el VFS para no dejarlo a medias. */
esp_err_t mfs_esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx,
                                       int min_fd, int max_fd);

#ifdef __cplusplus
}
#endif

#else /* perfil 5.x */

/* En v5.x no hay nada que traducir: la llamada del componente ya usa la API
 * publica con la firma correcta. */
#include "esp_vfs.h"

#endif /* MATRIXFS_SHIM_IDF_PROFILE >= 2 */

#endif /* MATRIXFS_TEST_VFS_ADAPTER_H */
