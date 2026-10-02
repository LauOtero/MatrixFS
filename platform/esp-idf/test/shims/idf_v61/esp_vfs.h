/* esp_vfs.h — shim del perfil ESP-IDF v6.1.x.
 *
 * Reproduce la forma REAL de v6.1 (verificado en
 * components/vfs/include/esp_vfs.h de la rama v6.1):
 *   · `esp_vfs_t` sigue existiendo pero SUS CALLBACKS SIN CONTEXTO ESTAN
 *     DEPRECADOS (`write`/`open`/... sin sufijo `_p`), y los campos viven en
 *     uniones anonimas: `.open_p = f` sigue compilando;
 *   · ESP_VFS_FLAG_DEFAULT cambio de valor: en v5.5 era 0 y en v6.1 es (1 << 0);
 *   · `esp_vfs_register_fd_range` YA NO ESTA en la cabecera publica: se movio a
 *     `esp_private/socket.h` y SU FIRMA CAMBIO (primer parametro
 *     `esp_vfs_fs_ops_t *` en lugar de `esp_vfs_t *`, mas un parametro `flags`);
 *   · la API nueva es `esp_vfs_fs_ops_t` + `esp_vfs_register_fs*()`;
 *   · `esp_vfs_unregister(const char *)` sigue siendo publica e igual.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_VFS_H
#define MATRIXFS_TEST_SHIM_ESP_VFS_H

#ifndef MATRIXFS_SHIM_IDF_PROFILE
#define MATRIXFS_SHIM_IDF_PROFILE 2
#endif

#if MATRIXFS_SHIM_IDF_PROFILE != 2
#error "Este perfil es para IDF v6.x; define -DMATRIXFS_SHIM_IDF_PROFILE=2"
#endif

#if defined(_WIN32) && !defined(MATRIXFS_TEST_SHIM_NATIVE_DIRENT)
#define MATRIXFS_TEST_SHIM_NATIVE_DIRENT 1
#define _DIRENT_H_
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
struct dirent {
  long d_ino;
  unsigned short d_reclen;
  unsigned short d_namlen;
  unsigned char d_type;
  char d_name[260];
};
typedef struct shim_DIR DIR;
#endif /* _WIN32 */

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_err.h"
#include "matrixfs_esp.h"
#include "mfs_idf_component_config.h"

/* MinGW no define uid_t/gid_t. */
#ifndef MATRIXFS_TEST_SHIM_ID_TYPES
#define MATRIXFS_TEST_SHIM_ID_TYPES
typedef unsigned int mfs_shim_uid_t;
typedef unsigned int mfs_shim_gid_t;
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef mfs_shim_uid_t uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef mfs_shim_gid_t gid_t;
#endif
#endif

#include "matrixfs_esp_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Flags (valores REALES de v6.1) --- */
#define ESP_VFS_FLAG_DEFAULT (1 << 0)
#define ESP_VFS_FLAG_CONTEXT_PTR (1 << 1)
#define ESP_VFS_FLAG_READONLY_FS (1 << 2)
#define ESP_VFS_FLAG_STATIC (1 << 3)

#define ESP_VFS_PATH_MAX 15

/* --- esp_vfs_fs_ops_t: la API nueva de v6 ---
 * El shim reproduce la parte plana (sin los subcomponentes opcionales dir/
 * termios/select, que el componente no usa) mas `dir`, que si usa. */
typedef struct esp_vfs_dir_ops_s {
  int (*stat_p)(void *ctx, const char *path, struct stat *st);
  int (*unlink_p)(void *ctx, const char *path);
  int (*rename_p)(void *ctx, const char *src, const char *dst);
  DIR *(*opendir_p)(void *ctx, const char *name);
  struct dirent *(*readdir_p)(void *ctx, DIR *pdir);
  int (*closedir_p)(void *ctx, DIR *pdir);
  int (*mkdir_p)(void *ctx, const char *name, mode_t mode);
  int (*rmdir_p)(void *ctx, const char *name);
} esp_vfs_dir_ops_t;

typedef struct esp_vfs_fs_ops_s {
  ssize_t (*write_p)(void *ctx, int fd, const void *data, size_t size);
  off_t (*lseek_p)(void *ctx, int fd, off_t size, int mode);
  ssize_t (*read_p)(void *ctx, int fd, void *dst, size_t size);
  int (*open_p)(void *ctx, const char *path, int flags, int mode);
  int (*close_p)(void *ctx, int fd);
  int (*fstat_p)(void *ctx, int fd, struct stat *st);
  int (*fsync_p)(void *ctx, int fd);
  const esp_vfs_dir_ops_t *dir;
} esp_vfs_fs_ops_t;

typedef int esp_vfs_id_t;

/* --- esp_vfs_t legacy (deprecado en v6, pero presente) --- */
typedef struct {
  int flags;
  ssize_t (*write_p)(void *ctx, int fd, const void *data, size_t size);
  off_t (*lseek_p)(void *ctx, int fd, off_t offset, int whence);
  ssize_t (*read_p)(void *ctx, int fd, void *dst, size_t size);
  int (*open_p)(void *ctx, const char *path, int flags, int mode);
  int (*close_p)(void *ctx, int fd);
  int (*fstat_p)(void *ctx, int fd, struct stat *st);
  int (*stat_p)(void *ctx, const char *path, struct stat *st);
  int (*unlink_p)(void *ctx, const char *path);
  int (*rename_p)(void *ctx, const char *src, const char *dst);
  int (*mkdir_p)(void *ctx, const char *path, mode_t mode);
  int (*rmdir_p)(void *ctx, const char *path);
  DIR *(*opendir_p)(void *ctx, const char *path);
  struct dirent *(*readdir_p)(void *ctx, DIR *dirp);
  int (*closedir_p)(void *ctx, DIR *dirp);
  int (*fsync_p)(void *ctx, int fd);
} esp_vfs_t;

/* --- API publica de v6.1 --- */
esp_err_t esp_vfs_register(const char *base_path, const esp_vfs_t *vfs,
                           void *ctx);
esp_err_t esp_vfs_register_fs(const char *base_path,
                              const esp_vfs_fs_ops_t *vfs, int flags,
                              void *ctx);
esp_err_t esp_vfs_register_with_id(const esp_vfs_t *vfs, void *ctx,
                                   esp_vfs_id_t *vfs_id);
esp_err_t esp_vfs_register_fd(esp_vfs_id_t vfs_id, int *fd);
esp_err_t esp_vfs_unregister(const char *base_path);
esp_err_t esp_vfs_unregister_with_id(esp_vfs_id_t vfs_id);

/* NOTA: la cabecera publica de v6.1 NO declara `esp_vfs_register_fd_range`. El
 * contrato privado se reproduce en esp_private/socket.h, que el adaptador de
 * version incluye por separado (mfs_vfs_adapter.h). */

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_VFS_H */
