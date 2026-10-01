/* mfs_vfs.h — adaptador VFS portable sobre la API pública de MatrixFS.
 *
 * Toda la semántica de sistema de archivos (permisos, metadatos, E/S con
 * desplazamiento explícito, errores tipificados) vive aquí, de forma que los
 * front-ends de plataforma son envoltorios finos:
 *   · Linux  → platform/linux/matrixfs_fuse.c   (FUSE 3)
 *   · Windows→ platform/windows/matrixfs_winfsp.c (WinFsp)
 *   · Validación → platform/common/mfs_vfsctl.c  (sin kernel)
 *
 * El núcleo MatrixFS no es reentrante: el adaptador serializa todas las
 * llamadas con un mutex interno, por lo que los front-ends pueden ser
 * multi-hilo.
 */
#ifndef MFS_VFS_H
#define MFS_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matrixfs/matrixfs.h"
#include "mfs_blk.h"

typedef struct mfs_vfs mfs_vfs;

/* ==== Opciones de montaje (comunes a Linux y Windows) ==== */
typedef struct {
  mfs_blk_opts blk;      /* geometría física/lógica del medio            */
  const uint8_t *key;    /* 32 B (cifrado) o NULL                        */
  uint8_t suite;         /* mfs_suite_t o 0xFF = negociar                */
  mfs_mode_t mode;       /* MFS_MODE_UNSUPPORTED = automático            */
  bool readonly;         /* montaje de sólo lectura                      */
  bool format_if_needed; /* formatea si no hay superblock válido         */
  bool allow_convergent;
  bool zrp_enable;
  bool dedup_enable;
  bool cdc_enable;
  bool dab_enable;
  bool rt_strict;
  uint32_t ram_total; /* presupuesto RAM declarado (0 ⇒ 32768)        */
  uint32_t uid, gid;  /* propietario de nodos nuevos                  */
  uint16_t file_perm, dir_perm;
  const char *label; /* etiqueta de volumen (0 ⇒ "MatrixFS")         */
} mfs_mount_opts;

void mfs_mount_opts_default(mfs_mount_opts *o);

/* ==== Atributos de nodo expuestos a los front-ends ==== */
typedef struct {
  uint64_t size;
  uint32_t mode; /* MFS_S_IFMT | permisos */
  uint32_t uid, gid;
  uint32_t mtime; /* epoch seconds */
  uint32_t nlink;
  uint32_t ino;
} mfs_vfs_attr;

/* ==== Ciclo de vida ==== */
int mfs_vfs_mount(const char *device, const mfs_mount_opts *o, mfs_vfs **out,
                  char *err, size_t errlen);
void mfs_vfs_unmount(mfs_vfs *v);
int mfs_vfs_sync(mfs_vfs *v);
/* Formatea (borra y crea superblock/HWV). `o` puede ser NULL. */
int mfs_vfs_format(const char *device, const mfs_mount_opts *o, char *err,
                   size_t errlen);
/* Sondea si el medio contiene un volumen MatrixFS válido (sin montarlo). */
int mfs_vfs_probe(const char *device, const mfs_blk_opts *bopts,
                  char *label_out, size_t label_len, uint8_t *mode_out);
bool mfs_vfs_is_readonly(const mfs_vfs *v);
uint32_t mfs_vfs_uid(const mfs_vfs *v);
uint32_t mfs_vfs_gid(const mfs_vfs *v);
const char *mfs_vfs_device(const mfs_vfs *v);

/* ==== Metadatos ==== */
int mfs_vfs_getattr(mfs_vfs *v, const char *path, mfs_vfs_attr *out);
typedef int (*mfs_vfs_iter_fn)(void *ctx, const char *name,
                               const mfs_vfs_attr *st);
int mfs_vfs_readdir(mfs_vfs *v, const char *path, mfs_vfs_iter_fn fn,
                    void *ctx);
/* Comprueba permisos POSIX (`mask`: 4=R, 2=W, 1=X). */
int mfs_vfs_access(mfs_vfs *v, const char *path, uint32_t uid, uint32_t gid,
                   uint32_t mask);
int mfs_vfs_setattr(mfs_vfs *v, const char *path, uint32_t mask,
                    const mfs_attr *a);

/* ==== Ficheros (desplazamiento explícito: pread/pwrite) ==== */
typedef uint64_t mfs_vfs_fh; /* 0 = inválido */
int mfs_vfs_open(mfs_vfs *v, const char *path, uint32_t flags, mfs_vfs_fh *fh);
int mfs_vfs_create(mfs_vfs *v, const char *path, uint16_t mode, uint32_t uid,
                   uint32_t gid, mfs_vfs_fh *fh);
int mfs_vfs_read(mfs_vfs *v, mfs_vfs_fh fh, void *buf, uint64_t off, size_t len,
                 size_t *rd);
int mfs_vfs_write(mfs_vfs *v, mfs_vfs_fh fh, const void *buf, uint64_t off,
                  size_t len, size_t *wr);
int mfs_vfs_flush(mfs_vfs *v, mfs_vfs_fh fh);
int mfs_vfs_release(mfs_vfs *v, mfs_vfs_fh fh);
int mfs_vfs_truncate(mfs_vfs *v, mfs_vfs_fh fh, uint64_t size);
/* Truncado por ruta (FUSE truncate/chmod sin handle abierto). */
int mfs_vfs_truncate_path(mfs_vfs *v, const char *path, uint64_t size);

/* ==== Directorios ==== */
int mfs_vfs_mkdir(mfs_vfs *v, const char *path, uint16_t mode, uint32_t uid,
                  uint32_t gid);
int mfs_vfs_rmdir(mfs_vfs *v, const char *path);
int mfs_vfs_unlink(mfs_vfs *v, const char *path);
int mfs_vfs_rename(mfs_vfs *v, const char *from, const char *to);

/* ==== Volumen / salud ==== */
int mfs_vfs_statfs(mfs_vfs *v, uint64_t *total_bytes, uint64_t *free_bytes,
                   uint64_t *used_bytes);
int mfs_vfs_health(mfs_vfs *v, mfs_health_t *h);
int mfs_vfs_label(mfs_vfs *v, char *out, size_t len);
int mfs_vfs_verify(mfs_vfs *v, mfs_verify_level lvl);

/* Traduce un estado MatrixFS (mfs_st) al errno POSIX equivalente; los
 * front-ends lo usan para FUSE y para derivar el NTSTATUS de WinFsp. */
int mfs_vfs_errno(int st);

#endif /* MFS_VFS_H */
