/* esp_vfs.h — shim del perfil ESP-IDF v5.5.x.
 *
 * Reproduce la forma REAL de v5.5 (verificado en
 * components/vfs/include/esp_vfs.h de la rama release/v5.5):
 *   · `esp_vfs_t` con campos en uniones anonimas (`open_p`, `read_p`, ...);
 *   · ESP_VFS_FLAG_DEFAULT vale 0 en v5.5 (en v6.1 pasa a (1 << 0));
 *   · `esp_vfs_register_fd_range(const esp_vfs_t *, void *, int, int)` es una
 *     funcion PUBLICA con esa firma exacta: es la que usa el componente.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_VFS_H
#define MATRIXFS_TEST_SHIM_ESP_VFS_H

#ifndef MATRIXFS_SHIM_IDF_PROFILE
#define MATRIXFS_SHIM_IDF_PROFILE 1
#endif

#if MATRIXFS_SHIM_IDF_PROFILE != 1
#error "Este perfil es para IDF v5.x; define -DMATRIXFS_SHIM_IDF_PROFILE=1"
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

/* --- Flags (valores REALES de v5.5) --- */
#define ESP_VFS_FLAG_DEFAULT 0
#define ESP_VFS_FLAG_CONTEXT_PTR (1 << 1)
#define ESP_VFS_FLAG_READONLY_FS (1 << 2)
#define ESP_VFS_FLAG_STATIC (1 << 3)

#define ESP_VFS_PATH_MAX 15

/* --- esp_vfs_t de v5.5 --- */
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

/* --- API publica de v5.5 --- */
esp_err_t esp_vfs_register(const char *base_path, const esp_vfs_t *vfs,
                           void *ctx);
esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx, int min_fd,
                                    int max_fd);
esp_err_t esp_vfs_unregister(const char *base_path);

/* El adaptador de version (mfs_vfs_adapter.h) reutiliza esta declaracion tal
 * cual: en v5.x la firma del componente YA es la real, no hay nada que
 * traducir. */
#define MFS_VFS_FD_RANGE_IS_NATIVE 1

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_VFS_H */
