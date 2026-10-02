/* esp_vfs.h — shim minimo de ESP-IDF (VFS) para IDF v5.x.
 *
 * Declara EXACTAMENTE la forma que usa platform/esp-idf/matrixfs_esp_vfs.c:
 * `esp_vfs_t` con los campos `*_p` (variante de IDF v5 con
 * ESP_VFS_FLAG_DEFAULT) y `esp_vfs_register_fd_range(&vfs, ctx, min, max)`.
 * El despacho POSIX esta en el propio shim (idf_shim.c + idf_shim.h) y replica
 * el comportamiento de IDF: busca el punto de montaje mas largo que sea prefijo
 * de la ruta y entrega al callback la ruta SIN ese prefijo.
 *
 * ADAPTACION A MinGW. El `struct dirent` de newlib (el que usa IDF en el
 * target) lleva `d_type` y las constantes DT_*; el de MinGW no, y
 * matrixfs_esp_vfs.c rellena `d_type` en readdir. Para no tocar la fuente real
 * del componente, este shim:
 *   · reclama la guarda `_DIRENT_H_` de MinGW ANTES de nada, de modo que en
 *     cuanto el componente incluya <dirent.h> reciba la version de aqui;
 *   · define `struct dirent` conservando el layout de MinGW
 *     (d_ino/d_reclen/d_namlen/d_name) y anadiendo `d_type` al final;
 *   · declara `DIR` como tipo opaco (el componente solo maneja punteros).
 * En una libc con la semantica de newlib (glibc) no se toca nada.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_VFS_H
#define MATRIXFS_TEST_SHIM_ESP_VFS_H

#if defined(_WIN32) && !defined(MATRIXFS_TEST_SHIM_NATIVE_DIRENT)
#define MATRIXFS_TEST_SHIM_NATIVE_DIRENT 1
/* Guarda de <dirent.h> de MinGW: su struct no tiene d_type. */
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
  long d_ino;              /* siempre 0 en MinGW */
  unsigned short d_reclen; /* siempre 0 en MinGW */
  unsigned short d_namlen; /* longitud del nombre en MinGW */
  unsigned char d_type;    /* anadido por el shim (newlib lo trae de serie) */
  char d_name[260];        /* [FILENAME_MAX] */
};
typedef struct shim_DIR DIR; /* opaco: el componente solo usa punteros */
#endif /* _WIN32 */

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_err.h"
#include "matrixfs_esp.h"
#include "mfs_idf_component_config.h"

/* El perfil de version lo aporta shims/idf_v55/sdkconfig.h o
 * shims/idf_v61/sdkconfig.h. El valor por defecto (5.x) solo sirve para no
 * romper builds antiguos de un unico perfil; la linea de compilacion de la suite
 * de dos perfiles SIEMPRE elige perfil, y `mfs_idf_contract_profile.h` comprueba
 * que las constantes de esta cabecera coinciden con la version simulada. */
#ifndef MATRIXFS_SHIM_IDF_PROFILE
#define MATRIXFS_SHIM_IDF_PROFILE 1 /* 1 = v5.x legacy, 2 = v6.x */
#endif

#include "mfs_idf_contract_profile.h"

/* MinGW no define uid_t/gid_t (no son tipos POSIX garantizados en Windows) y el
 * componente los usa para rellenar struct stat, igual que haria newlib en el
 * target. Se aportan aqui, antes de los usos, para que la fuente REAL del
 * componente compile sin tocarla. */
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
#endif /* MATRIXFS_TEST_SHIM_ID_TYPES */

/* El componente delega en la cabecera publica las declaraciones de
 * matrixfs_esp_lock/unlock y matrixfs_esp_fs (via matrixfs_esp_private.h); en
 * el CMake del componente ambos .c se compilan con los mismos include dirs, de
 * modo que aqui se replica esa visibilidad. */
#include "matrixfs_esp_private.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int flags;
  ssize_t (*write_p)(void *ctx, int fd, const void *data, size_t size);
  off_t (*lseek_p)(void *ctx, int fd, off_t offset, int whence);
  ssize_t (*read_p)(void *ctx, int fd, void *dst, size_t size);
  int (*open_p)(void *ctx, const char *path, int flags, int mode);
  int (*close_p)(void *ctx, int fd);
  int (*fstat_p)(void *ctx, int fd, struct stat *st);
  int (*fsync_p)(void *ctx, int fd);
  int (*stat_p)(void *ctx, const char *path, struct stat *st);
  int (*unlink_p)(void *ctx, const char *path);
  int (*rename_p)(void *ctx, const char *src, const char *dst);
  int (*mkdir_p)(void *ctx, const char *path, mode_t mode);
  int (*rmdir_p)(void *ctx, const char *path);
  DIR *(*opendir_p)(void *ctx, const char *path);
  struct dirent *(*readdir_p)(void *ctx, DIR *dirp);
  int (*closedir_p)(void *ctx, DIR *dirp);
} esp_vfs_t;

/* ==== Tipos de la API de v6, que idf_shim.c necesita SIEMPRE ====
 *
 * idf_shim.c implementa el despacho de las DOS familias de API (la legacy
 * `esp_vfs_t` y la nueva `esp_vfs_fs_ops_t` de v6) con expresiones
 * condicionales, de modo que AMBOS juegos de tipos deben estar declarados en
 * cualquier perfil, aunque el perfil activo sea el de v5.x. Misma forma que
 * shims/idf_v61/esp_vfs.h: parte plana + `dir`, que es lo que usa el shim. */
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

/* ESP_VFS_FLAG_DEFAULT vale 0 en IDF v5.x y (1 << 0) en v6.x: se toma el valor
 * del perfil activo para que la validacion del shim sea la real. */
#if ESP_IDF_VERSION_MAJOR >= 6
#define ESP_VFS_FLAG_DEFAULT (1 << 0)
#else
#define ESP_VFS_FLAG_DEFAULT 0
#endif
#define ESP_VFS_FLAG_CONTEXT_PTR (1 << 1)
#define ESP_VFS_FLAG_READONLY_FS (1 << 2)
#define ESP_VFS_FLAG_STATIC (1 << 3)

/* Contrato del perfil: comprueba en COMPILACION que los valores de esta
 * cabecera coinciden con los de la version de IDF simulada. */
#include "mfs_idf_contract_profile.h"

esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx, int min_fd,
                                    int max_fd);

/* API que ASOCIA UN PUNTO DE MONTAJE. Es la que usa el componente: con
 * `esp_vfs_register_fd_range` el volumen queda sin ruta. */
esp_err_t esp_vfs_register(const char *base_path, const esp_vfs_t *vfs,
                           void *ctx);

esp_err_t esp_vfs_unregister(const char *base_path);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_VFS_H */
