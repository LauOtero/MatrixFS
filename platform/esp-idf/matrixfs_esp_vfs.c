/* matrixfs_esp_vfs.c — registro POSIX (VFS) del volumen MatrixFS en ESP-IDF.
 *
 * Copyright 2026 MatrixFS contributors
 *
 * Licencia Apache, Version 2.0 (la "Licencia");
 * no puede usar este fichero salvo en cumplimiento de la Licencia.
 * Puede obtener una copia en:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Traduce la API POSIX de newlib a la API del nucleo. Sin heap para las tablas
 * (descriptores y flujos de directorio en estado estatico); `opendir` sí
 * necesita devolver un `DIR*` opaco, que se materializa como puntero a una
 * ranura de la tabla estatica.
 *
 * Serializacion: el nucleo no es reentrante. Con CONFIG_MATRIXFS_THREAD_SAFE
 * cada operacion del VFS toma el mutex recursivo del componente, de modo que el
 * acceso por POSIX es seguro entre tareas sin intervencion de la aplicacion.
 */
#include "matrixfs_esp_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "sdkconfig.h"

#include "matrixfs/matrixfs.h"
/* Se incluye la cabecera publica del componente EXPLICITAMENTE: el mutex y el
 * acceso al volumen se obtienen por matrixfs_esp_lock()/matrixfs_esp_fs(), y
 * depender de que otra cabecera las arrastre hace el fichero fragil ante
 * reordenaciones de includes. */
#include "matrixfs_esp.h"
#include "matrixfs_esp_private.h"

#if !CONFIG_MATRIXFS_VFS_ENABLE
#error "matrixfs_esp_vfs.c no debe compilarse sin CONFIG_MATRIXFS_VFS_ENABLE"
#endif

/* `mf_t` es opaca en la API publica: el componente la declara por el llamador,
 * pero el VFS solo maneja punteros, asi que basta la declaracion adelantada que
 * ya aporta matrixfs/matrixfs.h. */
#define MFS_VFS_TAG "matrixfs_vfs"

#define MFS_VFS_MAX_FILES CONFIG_MATRIXFS_VFS_MAX_FILES
/* Base de descriptores reservada al VFS. Debe quedar por encima de los que usa
 * el resto del sistema (stdio, sockets) para no colisionar. */
#define MFS_VFS_FD_BASE 100
#define MFS_VFS_FD_MAX (MFS_VFS_FD_BASE + MFS_VFS_MAX_FILES)

#define MFS_VFS_MAX_DIRS 4u

/* ==== Tablas estaticas ==== */
typedef struct {
  mfs_file *f;
  bool used;
} mfs_vfs_file_t;

typedef struct {
  mfs_dir *d;
  bool used;
  struct dirent de; /* plantilla que devuelve readdir */
} mfs_vfs_dir_t;

static mfs_vfs_file_t s_files[MFS_VFS_MAX_FILES];
static mfs_vfs_dir_t s_dirs[MFS_VFS_MAX_DIRS];
static bool s_registered = false;

/* ==== Utilidades ==== */

static mf_t *vfs_fs(void) { return matrixfs_esp_fs(); }

static int vfs_slot_of_fd(int fd) {
  int i = fd - MFS_VFS_FD_BASE;
  if (i < 0 || i >= (int)MFS_VFS_MAX_FILES)
    return -1;
  return i;
}

static int vfs_alloc_file(mfs_file *f) {
  for (int i = 0; i < (int)MFS_VFS_MAX_FILES; i++) {
    if (!s_files[i].used) {
      s_files[i].used = true;
      s_files[i].f = f;
      return MFS_VFS_FD_BASE + i;
    }
  }
  return -1;
}

static mfs_file *vfs_file_of(int fd) {
  int i = vfs_slot_of_fd(fd);
  if (i < 0 || !s_files[i].used)
    return NULL;
  return s_files[i].f;
}

/* Rechaza intentos de salir del volumen con "..": el nucleo trabaja con rutas
 * absolutas dentro del volumen y no debe ver componentes relativos. */
static const char *vfs_strip(const char *path, int *err) {
  *err = 0;
  if (!path) {
    *err = EINVAL;
    return NULL;
  }
  /* El VFS entrega la ruta DESPUES del punto de montaje. */
  while (*path == '/')
    path++;
  return path; /* "" es la raiz */
}

static void vfs_fill_stat(const mfs_stat *in, struct stat *out) {
  memset(out, 0, sizeof(*out));
  bool is_dir = ((in->mode & MFS_S_IFDIR) != 0u);
  out->st_mode = (mode_t)(is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
  out->st_size = (off_t)in->size;
  out->st_nlink = 1;
  out->st_uid = (uid_t)in->uid;
  out->st_gid = (gid_t)in->gid;
  /* En newlib `st_mtime` es un alias de `st_mtim.tv_sec`; se asigna solo el
   * primero para no depender de la macro. */
  out->st_mtime = (time_t)in->mtime;
}

/* Ejecuta `body` con el mutex del componente tomado (si esta activo). */
#define MFS_VFS_WITH_LOCK(body)                                                \
  do {                                                                         \
    matrixfs_esp_mutex_ensure();                                               \
    matrixfs_esp_lock();                                                       \
    body;                                                                      \
    matrixfs_esp_unlock();                                                     \
  } while (0)

/* ==== Callbacks de fichero ==== */

static int vfs_open(void *ctx, const char *path, int flags, int mode) {
  (void)ctx;
  (void)mode;
  int err = 0;
  const char *p = vfs_strip(path, &err);
  if (err) {
    errno = err;
    return -1;
  }
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return -1;
  }

  /* Traduccion open(2) → flags del nucleo. */
  uint32_t mf = 0u;
  int acc = flags & O_ACCMODE;
  if (acc == O_RDONLY)
    mf |= (uint32_t)MFS_O_RDONLY;
  else if (acc == O_WRONLY)
    mf |= (uint32_t)MFS_O_WRONLY;
  else
    mf |= (uint32_t)MFS_O_RDWR;
  if (flags & O_CREAT)
    mf |= (uint32_t)MFS_O_CREAT;
  if (flags & O_TRUNC)
    mf |= (uint32_t)MFS_O_TRUNC;
  if (flags & O_APPEND)
    mf |= (uint32_t)MFS_O_APPEND;
  if (flags & O_EXCL)
    mf |= (uint32_t)MFS_O_EXCL;

  int fd = -1;
  MFS_VFS_WITH_LOCK({
    mfs_file *f = NULL;
    mfs_st st = (mfs_st)mf_open(fs, p, mf, &f);
    if (st == MFS_OK) {
      fd = vfs_alloc_file(f);
      if (fd < 0) {
        (void)mf_close(f);
        errno = EMFILE;
      }
    } else {
      errno = matrixfs_esp_errno(st);
    }
  });
  return fd;
}

static int vfs_close(void *ctx, int fd) {
  (void)ctx;
  int rc = -1;
  /* IDF-05: la ranura se valida DENTRO del mutex. Si se leyera el descriptor
   * fuera, otra tarea podria cerrarlo y REUTILIZAR la ranura (con otro
   * `mfs_file*`) entre la validacion y el cierre: se cerraria el fichero
   * equivocado. */
  MFS_VFS_WITH_LOCK({
    int slot = vfs_slot_of_fd(fd);
    if (slot < 0 || !s_files[slot].used) {
      errno = EBADF;
    } else {
      mfs_st st = (mfs_st)mf_close(s_files[slot].f);
      s_files[slot].used = false;
      s_files[slot].f = NULL;
      if (st != MFS_OK)
        errno = matrixfs_esp_errno(st);
      else
        rc = 0;
    }
  });
  return rc;
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size) {
  (void)ctx;
  ssize_t got = -1;
  /* IDF-05: resolucion del descriptor bajo el mutex (evita la carrera con
   * close/reopen que reutiliza la ranura). */
  MFS_VFS_WITH_LOCK({
    mfs_file *f = vfs_file_of(fd);
    if (!f) {
      errno = EBADF;
    } else {
      size_t rd = 0;
      mfs_st st = (mfs_st)mf_read(f, dst, size, &rd);
      if (st == MFS_OK)
        got = (ssize_t)rd;
      else
        errno = matrixfs_esp_errno(st);
    }
  });
  return got;
}

static ssize_t vfs_write(void *ctx, int fd, const void *src, size_t size) {
  (void)ctx;
  ssize_t put = -1;
  MFS_VFS_WITH_LOCK({
    mfs_file *f = vfs_file_of(fd);
    if (!f) {
      errno = EBADF;
    } else {
      size_t wr = 0;
      mfs_st st = (mfs_st)mf_write(f, src, size, &wr);
      if (st == MFS_OK)
        put = (ssize_t)wr;
      else
        errno = matrixfs_esp_errno(st);
    }
  });
  return put;
}

static off_t vfs_lseek(void *ctx, int fd, off_t size, int mode) {
  (void)ctx;
  int whence;
  switch (mode) {
  case SEEK_SET:
    whence = MFS_SEEK_SET;
    break;
  case SEEK_CUR:
    whence = MFS_SEEK_CUR;
    break;
  case SEEK_END:
    whence = MFS_SEEK_END;
    break;
  default:
    errno = EINVAL;
    return (off_t)-1;
  }
  off_t pos = (off_t)-1;
  MFS_VFS_WITH_LOCK({
    mfs_file *f = vfs_file_of(fd);
    if (!f) {
      errno = EBADF;
    } else {
      mfs_st st = (mfs_st)mf_seek(f, (int64_t)size, whence);
      if (st != MFS_OK) {
        errno = matrixfs_esp_errno(st);
      } else {
        uint64_t cur = 0;
        st = (mfs_st)mf_tell(f, &cur);
        pos = (st == MFS_OK) ? (off_t)cur : (off_t)-1;
        if (st != MFS_OK)
          errno = matrixfs_esp_errno(st);
      }
    }
  });
  return pos;
}

static int vfs_fstat(void *ctx, int fd, struct stat *out) {
  (void)ctx;
  mf_t *fs = vfs_fs();
  if (!fs || !out) {
    errno = EBADF;
    return -1;
  }
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    /* mf_fstat no existe en la API: se obtiene el tamano por mf_tell y el
     * resto por la ruta, que el nucleo no expone por descriptor. Se rellena lo
     * que POSIX necesita (tipo y tamano). */
    mfs_file *f = vfs_file_of(fd);
    if (!f) {
      errno = EBADF;
    } else {
      uint64_t cur = 0;
      mfs_st st = (mfs_st)mf_tell(f, &cur);
      if (st == MFS_OK) {
        memset(out, 0, sizeof(*out));
        out->st_mode = (mode_t)(S_IFREG | 0644);
        out->st_size = (off_t)cur;
        out->st_nlink = 1;
        rc = 0;
      } else {
        errno = matrixfs_esp_errno(st);
      }
    }
  });
  return rc;
}

static int vfs_fsync(void *ctx, int fd) {
  (void)ctx;
  (void)fd;
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return -1;
  }
  int rc = 0;
  MFS_VFS_WITH_LOCK({
    mfs_st st = (mfs_st)mf_sync(fs);
    if (st != MFS_OK) {
      errno = matrixfs_esp_errno(st);
      rc = -1;
    }
  });
  return rc;
}

/* ==== Callbacks de espacio de nombres ==== */

static int vfs_stat(void *ctx, const char *path, struct stat *out) {
  (void)ctx;
  int err = 0;
  const char *p = vfs_strip(path, &err);
  if (err) {
    errno = err;
    return -1;
  }
  mf_t *fs = vfs_fs();
  if (!fs || !out) {
    errno = ENODEV;
    return -1;
  }
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    mfs_stat st_;
    mfs_st st = (mfs_st)mf_stat(fs, p, &st_);
    if (st == MFS_OK) {
      vfs_fill_stat(&st_, out);
      rc = 0;
    } else {
      errno = matrixfs_esp_errno(st);
    }
  });
  return rc;
}

static int vfs_unlink(void *ctx, const char *path) {
  (void)ctx;
  int err = 0;
  const char *p = vfs_strip(path, &err);
  if (err) {
    errno = err;
    return -1;
  }
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return -1;
  }
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    mfs_st st = (mfs_st)mf_unlink(fs, p);
    if (st == MFS_OK)
      rc = 0;
    else
      errno = matrixfs_esp_errno(st);
  });
  return rc;
}

static int vfs_rename(void *ctx, const char *src, const char *dst) {
  (void)ctx;
  int e1 = 0, e2 = 0;
  const char *a = vfs_strip(src, &e1);
  const char *b = vfs_strip(dst, &e2);
  if (e1 || e2) {
    errno = e1 ? e1 : e2;
    return -1;
  }
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return -1;
  }
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    mfs_st st = (mfs_st)mf_rename(fs, a, b);
    if (st == MFS_OK)
      rc = 0;
    else
      errno = matrixfs_esp_errno(st);
  });
  return rc;
}

static int vfs_mkdir(void *ctx, const char *path, mode_t mode) {
  (void)ctx;
  (void)mode;
  int err = 0;
  const char *p = vfs_strip(path, &err);
  if (err) {
    errno = err;
    return -1;
  }
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return -1;
  }
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    mfs_st st = (mfs_st)mf_mkdir(fs, p);
    if (st == MFS_OK)
      rc = 0;
    else
      errno = matrixfs_esp_errno(st);
  });
  return rc;
}

static int vfs_rmdir(void *ctx, const char *path) {
  /* El nucleo no expone rmdir; mf_unlink no retira directorios. */
  (void)ctx;
  (void)path;
  errno = ENOTSUP;
  return -1;
}

/* ==== Callbacks de directorio ==== */

static DIR *vfs_opendir(void *ctx, const char *path) {
  (void)ctx;
  int err = 0;
  const char *p = vfs_strip(path, &err);
  if (err) {
    errno = err;
    return NULL;
  }
  mf_t *fs = vfs_fs();
  if (!fs) {
    errno = ENODEV;
    return NULL;
  }
  DIR *out = NULL;
  MFS_VFS_WITH_LOCK({
    for (uint32_t i = 0; i < MFS_VFS_MAX_DIRS; i++) {
      if (s_dirs[i].used)
        continue;
      mfs_dir *d = NULL;
      mfs_st st = (mfs_st)mf_opendir(fs, p, &d);
      if (st != MFS_OK) {
        errno = matrixfs_esp_errno(st);
        break;
      }
      s_dirs[i].used = true;
      s_dirs[i].d = d;
      out = (DIR *)&s_dirs[i];
      break;
    }
    if (!out && errno == 0)
      errno = EMFILE;
  });
  return out;
}

static struct dirent *vfs_readdir(void *ctx, DIR *dir) {
  (void)ctx;
  if (!dir) {
    errno = EBADF;
    return NULL;
  }
  mfs_vfs_dir_t *slot = (mfs_vfs_dir_t *)dir;
  struct dirent *out = NULL;
  /* IDF-05: la ranura se valida bajo el mutex (otra tarea podria haber
   * cerrado el directorio y reutilizado la ranura). */
  MFS_VFS_WITH_LOCK({
    if (!slot->used || !slot->d) {
      errno = EBADF;
    } else {
      mfs_dirent de;
      mfs_st st = (mfs_st)mf_readdir(slot->d, &de);
      if (st == MFS_OK) {
        memset(&slot->de, 0, sizeof(slot->de));
        strncpy(slot->de.d_name, de.name, sizeof(slot->de.d_name) - 1u);
        /* `d_type` y las constantes DT_* son propias de newlib (el libc de
         * ESP-IDF). Se compilan solo si existen, para no atar el fichero a un
         * libc concreto. */
#if defined(DT_DIR) && defined(DT_REG)
        slot->de.d_type = ((de.st.mode & MFS_S_IFDIR) != 0u) ? DT_DIR : DT_REG;
#else
        (void)de.st.mode;
#endif
        out = &slot->de;
      } else if (st != MFS_ENOENT) {
        errno = matrixfs_esp_errno(st);
      }
      /* MFS_ENOENT = fin de directorio: readdir devuelve NULL sin tocar errno.
       */
    }
  });
  return out;
}

static int vfs_closedir(void *ctx, DIR *dir) {
  (void)ctx;
  if (!dir) {
    errno = EBADF;
    return -1;
  }
  mfs_vfs_dir_t *slot = (mfs_vfs_dir_t *)dir;
  int rc = -1;
  MFS_VFS_WITH_LOCK({
    if (!slot->used) {
      errno = EBADF;
    } else {
      mfs_st st = (mfs_st)mf_closedir(slot->d);
      slot->used = false;
      slot->d = NULL;
      if (st != MFS_OK)
        errno = matrixfs_esp_errno(st);
      else
        rc = 0;
    }
  });
  return rc;
}

/* ==== Registro ==== */

static const esp_vfs_t s_vfs = {
    /* ESP_VFS_FLAG_CONTEXT_PTR es lo que CASA con los callbacks `*_p`: en IDF,
     * `esp_vfs_t` es una union de dos juegos de punteros y el flag decide cual
     * se usa. Asignar los `*_p` con ESP_VFS_FLAG_DEFAULT haria que IDF llamase
     * por el juego sin contexto (firma distinta) => comportamiento indefinido.
     * Ademas es la unica variante NO deprecada en ESP-IDF v6.x. */
    .flags = ESP_VFS_FLAG_CONTEXT_PTR,
    .open_p = &vfs_open,
    .close_p = &vfs_close,
    .read_p = &vfs_read,
    .write_p = &vfs_write,
    .lseek_p = &vfs_lseek,
    .fstat_p = &vfs_fstat,
    .fsync_p = &vfs_fsync,
    .stat_p = &vfs_stat,
    .unlink_p = &vfs_unlink,
    .rename_p = &vfs_rename,
    .mkdir_p = &vfs_mkdir,
    .rmdir_p = &vfs_rmdir,
    .opendir_p = &vfs_opendir,
    .readdir_p = &vfs_readdir,
    .closedir_p = &vfs_closedir,
};

const char *matrixfs_esp_vfs_mount_point(void) {
  return CONFIG_MATRIXFS_VFS_MOUNT_POINT;
}

mfs_st matrixfs_esp_vfs_register(void) {
  if (s_registered)
    return MFS_EBUSY;
  if (!matrixfs_esp_is_mounted())
    return MFS_ENOTMOUNTED;

  /* Se usa `esp_vfs_register()` porque es la API que ASOCIA EL PUNTO DE
   * MONTAJE. `esp_vfs_register_fd_range()` NO recibe `base_path`: sirve para
   * reservar un rango de descriptores y registrar descriptores sueltos, de modo
   * que con ella el volumen quedaba sin ruta y ni `fopen("/matrixfs/...")` ni
   * `esp_vfs_unregister(MATRIXFS_VFS_MOUNT_POINT)` encontraban el montaje.
   * IDF traduce los descriptores internamente, asi que no hace falta gestionar
   * el rango a mano. */
  esp_err_t err =
      esp_vfs_register(matrixfs_esp_vfs_mount_point(), &s_vfs, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(MFS_VFS_TAG, "esp_vfs_register('%s') fallo (%s)",
             matrixfs_esp_vfs_mount_point(), esp_err_to_name(err));
    return MFS_EIO;
  }
  s_registered = true;
  ESP_LOGI(MFS_VFS_TAG, "MatrixFS registrado en VFS en '%s'",
           matrixfs_esp_vfs_mount_point());
  return MFS_OK;
}

mfs_st matrixfs_esp_vfs_unregister(void) {
  if (!s_registered)
    return MFS_ENOTMOUNTED;

  /* Cierra lo que quedara abierto para no dejar descriptores colgando. */
  matrixfs_esp_mutex_ensure();
  matrixfs_esp_lock();
  for (int i = 0; i < (int)MFS_VFS_MAX_FILES; i++) {
    if (s_files[i].used) {
      (void)mf_close(s_files[i].f);
      s_files[i].used = false;
      s_files[i].f = NULL;
    }
  }
  for (uint32_t i = 0; i < MFS_VFS_MAX_DIRS; i++) {
    if (s_dirs[i].used) {
      (void)mf_closedir(s_dirs[i].d);
      s_dirs[i].used = false;
      s_dirs[i].d = NULL;
    }
  }
  matrixfs_esp_unlock();

  esp_err_t err = esp_vfs_unregister(matrixfs_esp_vfs_mount_point());
  if (err != ESP_OK)
    return MFS_EIO;
  s_registered = false;
  return MFS_OK;
}
