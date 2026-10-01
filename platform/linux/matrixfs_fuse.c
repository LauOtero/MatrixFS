/* matrixfs_fuse.c — front-end FUSE 3 de MatrixFS Ultra para Linux.
 *
 * Este fichero es un envoltorio fino sobre el adaptador portable
 * `platform/common/mfs_vfs.c`: toda la semántica de sistema de archivos
 * (permisos POSIX persistidos, metadatos, E/S con desplazamiento explícito,
 * errores tipificados) vive en la capa común, idéntica a la que usa el
 * front-end WinFsp de Windows. Aquí sólo se traduce entre el ABI de FUSE 3 y
 * el contrato mfs_vfs_*.
 *
 * Compatibilidad: FUSE 3 (libfuse 3.x) sobre Linux 5.4+, 6.x y 7.x.
 *   · Se exige la API de FUSE 3 (fuse_operations con fuse_config/init de dos
 *     argumentos y readdir con flags); libfuse 2 NO es soportado.
 *
 * Montaje:
 *   matrixfs_fuse <dispositivo> <punto_de_montaje> [opciones]
 * donde <dispositivo> puede ser:
 *   · una imagen de fichero (p.ej. /var/lib/matrixfs/vol0.img),
 *   · una partición de bloque (/dev/sdb1, /dev/nvme0n1p2),
 *   · un dispositivo MTD (/dev/mtd0).
 *
 * Opciones propias (además de las estándar de FUSE):
 *   -o device=PATH      dispositivo (alternativa al argumento posicional)
 *   -o label=NAME       etiqueta de volumen (al formatear o al montar)
 *   -o format           formatea el medio antes de montar
 *   -o ro               montaje de sólo lectura
 *   -o uid=N,gid=N      propietario por defecto de los nodos nuevos
 *   -o fmask=OCT        permisos por defecto de ficheros  (p.ej. 644)
 *   -o dmask=OCT        permisos por defecto de directorios (p.ej. 755)
 *   -o ram=N            presupuesto RAM declarado en bytes (por defecto 262144)
 *   -o erase_unit=N     bloque de borrado en bytes (por defecto: derivado del
 *                       tamaño del medio para cubrirlo con MFS_ZONE_MAX zonas)
 *   -o allow_other      permite acceso a otros usuarios (ver /etc/fuse.conf)
 *
 * Copyright (c) 2026 MatrixFS Ultra «ATLAS» v1.0
 * Licencia: la del proyecto MatrixFS Ultra.
 */
#define FUSE_USE_VERSION 34

/* Compilar con `-std=c11` (ISO estricto) oculta las declaraciones POSIX de
 * glibc (strdup, UTIME_NOW, statvfs, pread/pwrite, pthread_*). Se solicitan
 * antes de cualquier inclusión. */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__has_include)
#if __has_include(<fuse3/fuse.h>)
#include <fuse3/fuse.h>
#else
#include <fuse.h>
#endif
#else
#include <fuse.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "mfs_vfs.h"

/* ============================ Opciones ================================== */

struct mfs_fuse_opts {
  char *device;
  char *label;
  int uid;
  int gid;
  int fmask; /* octal sin prefijo, p.ej. 644 */
  int dmask; /* octal sin prefijo, p.ej. 755 */
  int ram;
  int erase_unit;
  int readonly;
  int do_format;
  int showed_help;
};

#define MFS_OPT(t, p, v) {t, offsetof(struct mfs_fuse_opts, p), v}

static const struct fuse_opt mfs_opts[] = {
    MFS_OPT("device=%s", device, 0),
    MFS_OPT("label=%s", label, 0),
    MFS_OPT("uid=%d", uid, 0),
    MFS_OPT("gid=%d", gid, 0),
    MFS_OPT("fmask=%o", fmask, 0), /* octal: -o fmask=644 */
    MFS_OPT("dmask=%o", dmask, 0),
    MFS_OPT("ram=%d", ram, 0),
    MFS_OPT("erase_unit=%d", erase_unit, 0),
    MFS_OPT("ro", readonly, 1),
    MFS_OPT("format", do_format, 1),
    FUSE_OPT_END};

static void mfs_usage(const char *prog) {
  fprintf(stderr,
          "uso: %s <dispositivo> <punto_de_montaje> [opciones de FUSE]\n"
          "     -o device=PATH     dispositivo (imagen, /dev/sdX, /dev/mtdN)\n"
          "     -o label=NOMBRE    etiqueta de volumen\n"
          "     -o format          formatea antes de montar\n"
          "     -o ro              sólo lectura\n"
          "     -o uid=N,gid=N     propietario por defecto\n"
          "     -o fmask=OCT,dmask=OCT  permisos por defecto (644 / 755)\n"
          "     -o ram=N           presupuesto RAM declarado\n"
          "     -o erase_unit=N    bloque de borrado (0/omitido: derivado)\n",
          prog);
}

/* Primer argumento no-opción = dispositivo; los demás se dejan a FUSE. */
static int mfs_opt_proc(void *data, const char *arg, int key,
                        struct fuse_args *outargs) {
  (void)outargs;
  struct mfs_fuse_opts *o = (struct mfs_fuse_opts *)data;
  if (key == FUSE_OPT_KEY_NONOPT) {
    if (!o->device) {
      o->device = strdup(arg);
      return 0; /* consumido */
    }
  }
  return 1; /* conservar (punto de montaje y opciones de FUSE) */
}

/* ======================= Puente de errores ============================== */

/* FUSE devuelve -errno; mfs_vfs_errno ya entrega el errno positivo. */
static int mfs_ret(int st) { return (st == MFS_OK) ? 0 : -mfs_vfs_errno(st); }

static mfs_vfs *mfs_vol(void) {
  return (mfs_vfs *)fuse_get_context()->private_data;
}

/* Traduce flags POSIX (fi->flags) al subconjunto MFS_O_* del núcleo. */
static uint32_t mfs_flags_posix(int flags) {
  uint32_t m;
  switch (flags & O_ACCMODE) {
  case O_RDONLY:
    m = MFS_O_RDONLY;
    break;
  case O_WRONLY:
    m = MFS_O_WRONLY;
    break;
  default:
    m = MFS_O_RDWR;
    break;
  }
  if (flags & O_CREAT)
    m |= MFS_O_CREAT;
  if (flags & O_EXCL)
    m |= MFS_O_EXCL;
  if (flags & O_TRUNC)
    m |= MFS_O_TRUNC;
  if (flags & O_APPEND)
    m |= MFS_O_APPEND;
#ifdef O_DIRECT
  if (flags & O_DIRECT)
    m |= MFS_O_DIRECT;
#endif
  return m;
}

static void mfs_fill_stat(const mfs_vfs_attr *a, struct stat *st) {
  memset(st, 0, sizeof(*st));
  st->st_ino = a->ino;
  st->st_mode = (mode_t)a->mode;
  st->st_nlink = a->nlink ? a->nlink : 1;
  st->st_size = (off_t)a->size;
  st->st_uid = a->uid;
  st->st_gid = a->gid;
  st->st_mtime = (time_t)a->mtime;
  st->st_atime = st->st_mtime;
  st->st_ctime = st->st_mtime;
}

/* ============================ Operaciones =============================== */

static int mfs_fuse_getattr(const char *path, struct stat *st,
                            struct fuse_file_info *fi) {
  (void)fi;
  mfs_vfs_attr a;
  int r = mfs_vfs_getattr(mfs_vol(), path, &a);
  if (r != MFS_OK)
    return mfs_ret(r);
  mfs_fill_stat(&a, st);
  return 0;
}

static int mfs_fuse_access(const char *path, int mask) {
  struct fuse_context *ctx = fuse_get_context();
  uint32_t m = 0u;
  if (mask & R_OK)
    m |= 4u;
  if (mask & W_OK)
    m |= 2u;
  if (mask & X_OK)
    m |= 1u;
  if (mask & F_OK)
    m = 0u; /* sólo existencia: lo valida getattr dentro de access */
  return mfs_ret(mfs_vfs_access(mfs_vol(), path, (uint32_t)ctx->uid,
                                (uint32_t)ctx->gid, m));
}

typedef struct {
  void *buf;
  fuse_fill_dir_t filler;
} mfs_rdctx;

static int mfs_rd_emit(void *ctx, const char *name, const mfs_vfs_attr *a) {
  mfs_rdctx *c = (mfs_rdctx *)ctx;
  struct stat st;
  mfs_fill_stat(a, &st);
  if (c->filler(c->buf, name, &st, 0, 0) != 0)
    return 1; /* búfer lleno: detener la iteración */
  return 0;
}

static int mfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t off, struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags) {
  (void)off;
  (void)fi;
  (void)flags;
  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);
  mfs_rdctx c = {buf, filler};
  int r = mfs_vfs_readdir(mfs_vol(), path, mfs_rd_emit, &c);
  if (r == MFS_EBUSY)
    return 0; /* el búfer se llenó: listado parcial válido */
  return mfs_ret(r);
}

static int mfs_fuse_mkdir(const char *path, mode_t mode) {
  struct fuse_context *ctx = fuse_get_context();
  return mfs_ret(mfs_vfs_mkdir(mfs_vol(), path, (uint16_t)(mode & 07777u),
                               (uint32_t)ctx->uid, (uint32_t)ctx->gid));
}

static int mfs_fuse_unlink(const char *path) {
  return mfs_ret(mfs_vfs_unlink(mfs_vol(), path));
}

static int mfs_fuse_rmdir(const char *path) {
  return mfs_ret(mfs_vfs_rmdir(mfs_vol(), path));
}

static int mfs_fuse_rename(const char *from, const char *to,
                           unsigned int flags) {
  (void)flags; /* el núcleo no soporta RENAME_EXCHANGE/NOREPLACE */
  return mfs_ret(mfs_vfs_rename(mfs_vol(), from, to));
}

static int mfs_fuse_chmod(const char *path, mode_t mode,
                          struct fuse_file_info *fi) {
  (void)fi;
  mfs_attr a;
  memset(&a, 0, sizeof(a));
  a.mode = (uint16_t)(mode & 07777u);
  return mfs_ret(mfs_vfs_setattr(mfs_vol(), path, MFS_ATTR_MODE, &a));
}

static int mfs_fuse_chown(const char *path, uid_t uid, gid_t gid,
                          struct fuse_file_info *fi) {
  (void)fi;
  mfs_attr a;
  uint32_t mask = 0u;
  memset(&a, 0, sizeof(a));
  if (uid != (uid_t)-1) {
    a.uid = (uint32_t)uid;
    mask |= MFS_ATTR_UID;
  }
  if (gid != (gid_t)-1) {
    a.gid = (uint32_t)gid;
    mask |= MFS_ATTR_GID;
  }
  if (mask == 0u)
    return 0;
  return mfs_ret(mfs_vfs_setattr(mfs_vol(), path, mask, &a));
}

static int mfs_fuse_utimens(const char *path, const struct timespec tv[2],
                            struct fuse_file_info *fi) {
  (void)fi;
  mfs_attr a;
  memset(&a, 0, sizeof(a));
  if (tv == NULL) {
    a.mtime = (uint32_t)time(NULL);
    return mfs_ret(mfs_vfs_setattr(mfs_vol(), path, MFS_ATTR_MTIME, &a));
  }
  if (tv[1].tv_nsec == UTIME_OMIT)
    return 0; /* mtime sin cambios */
  a.mtime = (tv[1].tv_nsec == UTIME_NOW) ? (uint32_t)time(NULL)
                                         : (uint32_t)tv[1].tv_sec;
  return mfs_ret(mfs_vfs_setattr(mfs_vol(), path, MFS_ATTR_MTIME, &a));
}

static int mfs_fuse_truncate(const char *path, off_t size,
                             struct fuse_file_info *fi) {
  if (fi && fi->fh)
    return mfs_ret(mfs_vfs_truncate(mfs_vol(), fi->fh, (uint64_t)size));
  return mfs_ret(mfs_vfs_truncate_path(mfs_vol(), path, (uint64_t)size));
}

static int mfs_fuse_open(const char *path, struct fuse_file_info *fi) {
  mfs_vfs_fh fh = 0;
  int r = mfs_vfs_open(mfs_vol(), path, mfs_flags_posix(fi->flags), &fh);
  if (r != MFS_OK)
    return mfs_ret(r);
  fi->fh = fh;
  return 0;
}

static int mfs_fuse_create(const char *path, mode_t mode,
                           struct fuse_file_info *fi) {
  struct fuse_context *ctx = fuse_get_context();
  mfs_vfs_fh fh = 0;
  int r = mfs_vfs_create(mfs_vol(), path, (uint16_t)(mode & 07777u),
                         (uint32_t)ctx->uid, (uint32_t)ctx->gid, &fh);
  if (r != MFS_OK)
    return mfs_ret(r);
  fi->fh = fh;
  return 0;
}

static int mfs_fuse_read(const char *path, char *buf, size_t size, off_t off,
                         struct fuse_file_info *fi) {
  (void)path;
  if (off < 0)
    return -EINVAL;
  size_t rd = 0u;
  int r = mfs_vfs_read(mfs_vol(), fi->fh, buf, (uint64_t)off, size, &rd);
  if (r != MFS_OK)
    return mfs_ret(r);
  return (int)rd;
}

static int mfs_fuse_write(const char *path, const char *buf, size_t size,
                          off_t off, struct fuse_file_info *fi) {
  (void)path;
  if (off < 0)
    return -EINVAL;
  size_t wr = 0u;
  int r = mfs_vfs_write(mfs_vol(), fi->fh, buf, (uint64_t)off, size, &wr);
  if (r != MFS_OK)
    return mfs_ret(r);
  return (int)wr;
}

static int mfs_fuse_flush(const char *path, struct fuse_file_info *fi) {
  (void)path;
  return mfs_ret(mfs_vfs_flush(mfs_vol(), fi->fh));
}

static int mfs_fuse_fsync(const char *path, int datasync,
                          struct fuse_file_info *fi) {
  (void)path;
  (void)datasync;
  return mfs_ret(mfs_vfs_flush(mfs_vol(), fi->fh));
}

static int mfs_fuse_release(const char *path, struct fuse_file_info *fi) {
  (void)path;
  return mfs_ret(mfs_vfs_release(mfs_vol(), fi->fh));
}

static int mfs_fuse_statfs(const char *path, struct statvfs *st) {
  (void)path;
  uint64_t total = 0, freeb = 0, used = 0;
  int r = mfs_vfs_statfs(mfs_vol(), &total, &freeb, &used);
  if (r != MFS_OK)
    return mfs_ret(r);
  const uint64_t bs = 4096u;
  memset(st, 0, sizeof(*st));
  st->f_bsize = (unsigned long)bs;
  st->f_frsize = (unsigned long)bs;
  st->f_blocks = (fsblkcnt_t)(total / bs);
  st->f_bfree = (fsblkcnt_t)(freeb / bs);
  st->f_bavail = st->f_bfree;
  st->f_files = 0;
  st->f_ffree = 0;
  st->f_namemax = 63u;
  return 0;
}

/* ============================ Ciclo de vida ============================= */

static void *mfs_fuse_init(struct fuse_conn_info *conn,
                           struct fuse_config *cfg) {
  (void)conn;
  /* El núcleo ya cachea internamente (L2P + zona WAL); se activa la caché de
   * página del kernel para lecturas y se desactiva la coherencia estricta. */
  cfg->kernel_cache = 1;
  cfg->use_ino = 1;
  cfg->entry_timeout = 1.0;
  cfg->attr_timeout = 1.0;
  cfg->negative_timeout = 0.0;
  cfg->nullpath_ok = 0;
  return fuse_get_context()->private_data;
}

static struct fuse_operations mfs_ops = {
    .init = mfs_fuse_init,
    .getattr = mfs_fuse_getattr,
    .access = mfs_fuse_access,
    .readdir = mfs_fuse_readdir,
    .mkdir = mfs_fuse_mkdir,
    .unlink = mfs_fuse_unlink,
    .rmdir = mfs_fuse_rmdir,
    .rename = mfs_fuse_rename,
    .chmod = mfs_fuse_chmod,
    .chown = mfs_fuse_chown,
    .utimens = mfs_fuse_utimens,
    .truncate = mfs_fuse_truncate,
    .open = mfs_fuse_open,
    .create = mfs_fuse_create,
    .read = mfs_fuse_read,
    .write = mfs_fuse_write,
    .flush = mfs_fuse_flush,
    .fsync = mfs_fuse_fsync,
    .release = mfs_fuse_release,
    .statfs = mfs_fuse_statfs,
};

/* ================================ main ================================== */

int main(int argc, char **argv) {
  struct mfs_fuse_opts o;
  memset(&o, 0, sizeof(o));
  o.uid = -1; /* -1 = no sobrescribir el propietario por defecto */
  o.gid = -1;
  o.ram = 0;
  /* 0 ⇒ geometría derivada del medio. Fijar un valor distinto al usado al
   * formatear cambia el layout (la zona y el offset de SB B dependen de él). */
  o.erase_unit = 0;

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  if (fuse_opt_parse(&args, &o, mfs_opts, mfs_opt_proc) == -1)
    return 2;

  if (!o.device) {
    mfs_usage(argv[0]);
    fuse_opt_free_args(&args);
    return 2;
  }

  /* ---- Configuración de montaje ---- */
  mfs_mount_opts mo;
  mfs_mount_opts_default(&mo);
  mo.readonly = o.readonly != 0;
  if (o.uid >= 0)
    mo.uid = (uint32_t)o.uid;
  if (o.gid >= 0)
    mo.gid = (uint32_t)o.gid;
  if (o.fmask)
    mo.file_perm = (uint16_t)o.fmask;
  if (o.dmask)
    mo.dir_perm = (uint16_t)o.dmask;
  if (o.ram)
    mo.ram_total = (uint32_t)o.ram;
  if (o.erase_unit)
    mo.blk.erase_unit = (uint32_t)o.erase_unit;
  if (o.label)
    mo.label = o.label;

  char err[256];
  err[0] = '\0';

  /* ---- Formateo opcional ---- */
  if (o.do_format) {
    int fr = mfs_vfs_format(o.device, &mo, err, sizeof(err));
    if (fr != MFS_OK) {
      fprintf(stderr, "matrixfs: formateo de '%s' falló: %s\n", o.device,
              err[0] ? err : mfs_ststr((mfs_st)fr));
      fuse_opt_free_args(&args);
      return 1;
    }
  }

  /* ---- Comprobación previa (mensajes claros antes de montar) ---- */
  {
    char lab[32];
    uint8_t mode = 0xFFu;
    int pr = mfs_vfs_probe(o.device, &mo.blk, lab, sizeof(lab), &mode);
    if (pr != MFS_OK) {
      fprintf(stderr,
              "matrixfs: '%s' no contiene un volumen MatrixFS válido (%s).\n"
              "          Use 'matrixfs-mkfs %s' para crearlo.\n",
              o.device, mfs_ststr((mfs_st)pr), o.device);
      fuse_opt_free_args(&args);
      return 1;
    }
  }

  /* ---- Montaje del volumen ---- */
  mfs_vfs *vol = NULL;
  int mr = mfs_vfs_mount(o.device, &mo, &vol, err, sizeof(err));
  if (mr != MFS_OK) {
    fprintf(stderr, "matrixfs: montaje de '%s' falló: %s\n", o.device,
            err[0] ? err : mfs_ststr((mfs_st)mr));
    fuse_opt_free_args(&args);
    return 1;
  }

  /* Identificación en /proc/mounts y aplicación de la política de permisos del
   * kernel (default_permissions): el núcleo aplica mode/uid/gid de getattr. */
  {
    char fsname[128];
    snprintf(fsname, sizeof(fsname), "-ofsname=%s", o.device);
    fuse_opt_add_arg(&args, "-osubtype=matrixfs");
    fuse_opt_add_arg(&args, fsname);
  }
  if (o.readonly)
    fuse_opt_add_arg(&args, "-oro");
  fuse_opt_add_arg(&args, "-odefault_permissions");
  fuse_opt_add_arg(&args, "-onoatime");

  int rc = fuse_main(args.argc, args.argv, &mfs_ops, vol);

  mfs_vfs_unmount(vol);
  fuse_opt_free_args(&args);
  free(o.device);
  free(o.label);
  return rc;
}
