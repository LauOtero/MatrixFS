/* mfs_vfs_adapter.c — adaptador de version de ESP-IDF para el registro VFS de
 * MatrixFS.
 *
 * Este fichero NO forma parte del componente: es la pieza que demuestra, con el
 * componente REAL compilado, que hay una forma soportada de que
 * `matrixfs_esp_vfs.c` registre el sistema de ficheros tanto en ESP-IDF v5.x
 * como en v6.x. Se compila como una unidad de traduccion aparte.
 *
 * El problema (verificado en las cabeceras reales):
 *
 *   IDF v5.5  components/vfs/include/esp_vfs.h:
 *     esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx,
 *                                         int min_fd, int max_fd);  [publica]
 *
 *   IDF v6.1  components/vfs/include/esp_private/socket.h:
 *     esp_err_t esp_vfs_register_fd_range(const esp_vfs_fs_ops_t *vfs,
 *                                         int flags, void *ctx,
 *                                         int min_fd, int max_fd);  [privada]
 *
 * Es decir: el mismo nombre, tipo distinto y cabecera distinta. Una llamada
 * escrita para v5 no compila en v6, y las dos no pueden coexistir sin un
 * adaptador.
 *
 * La solucion que se implementa aqui es la que NO depende de APIs privadas:
 *
 *   v5.x: se mantiene `esp_vfs_register_fd_range(&vfs, ctx, min, max)`, que es
 *         publica y estable en toda la serie 5.x.
 *   v6.x: `esp_vfs_register_fd_range` es privada; se usa la API publica
 *         equivalente: `esp_vfs_register_with_id()` + `esp_vfs_register_fd()`,
 *         que reserva el mismo intervalo de descriptores uno a uno. Ninguna de
 *         las dos esta marcada como deprecada en v6.1.
 *
 * La seleccion la hace `mfs_vfs_adapter.h`, que se incluye ANTES de esp_vfs.h: en
 * el perfil 6.x ese encabezado define la macro de renombrado con la que la
 * llamada del componente se traduce a `mfs_esp_vfs_register_fd_range()`.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mfs_vfs_adapter.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "sdkconfig.h"

#define MFS_VFS_ADAPTER_TAG "matrixfs_vfs_adapter"

/* El componente reserva este intervalo de descriptores
 * (platform/esp-idf/matrixfs_esp_vfs.c: MFS_VFS_FD_BASE/MFS_VFS_FD_MAX). Se
 * replican aqui para que el adaptador de v6 sepa cual reservar sin incluir la
 * cabecera interna del componente. */
#define MFS_VFS_FD_BASE 100
#define MFS_VFS_FD_MAX (MFS_VFS_FD_BASE + CONFIG_MATRIXFS_VFS_MAX_FILES)

#if defined(MFS_VFS_FD_RANGE_IS_NATIVE)

/* ---- IDF v5.x: la firma del componente ya es la real ------------------- */
/* No hace falta envoltorio: `esp_vfs_register_fd_range` se usa directamente. */
typedef int mfs_vfs_adapter_v5_anchor; /* evita unidad de traduccion vacia */

#else /* IDF v6.x u otra version sin la API publica */

/* ---- IDF v6.x: reserva del intervalo por la API publica ---------------- */

/* Registra el VFS con `esp_vfs_t` (variante legacy, que sigue existiendo en
 * v6.1) y reserva <min_fd; max_fd) descriptor a descriptor. Devuelve ESP_OK o
 * el primer error, deshaciendo lo hecho para no dejar el VFS a medias. */
esp_err_t mfs_esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx,
                                       int min_fd, int max_fd) {
  if (!vfs || min_fd < 0 || max_fd <= min_fd)
    return ESP_ERR_INVALID_ARG;

  esp_vfs_id_t vfs_id = -1;
  esp_err_t err = esp_vfs_register_with_id(vfs, ctx, &vfs_id);
  if (err != ESP_OK)
    return err;

  for (int fd = min_fd; fd < max_fd; fd++) {
    int assigned = -1;
    err = esp_vfs_register_fd(vfs_id, &assigned);
    if (err != ESP_OK) {
      ESP_LOGE(MFS_VFS_ADAPTER_TAG,
               "no se pudo reservar el descriptor %d del intervalo [%d,%d): %s",
               fd, min_fd, max_fd, esp_err_to_name(err));
      (void)esp_vfs_unregister_with_id(vfs_id);
      return err;
    }
  }
  return ESP_OK;
}

#endif /* MFS_VFS_FD_RANGE_IS_NATIVE */
