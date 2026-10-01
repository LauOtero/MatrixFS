/* mfs_vfs.c — adaptador VFS portable sobre la API pública (§21) + permisos.
 *
 * Responsabilidades:
 *   · Construir la mfs_config y montar/formatear el volumen sobre un mfs_blk.
 *   · Traducir rutas ('/' POSIX y '\' Windows) a la forma canónica del núcleo.
 *   · Serializar el acceso al núcleo (no reentrante) con un mutex.
 *   · Ofrecer E/S con desplazamiento explícito (pread/pwrite) sobre handles
 *     con generación, válidos para FUSE (`fi->fh`) y WinFsp (contexto).
 *   · Exponer y aplicar permisos POSIX (modo/uid/gid) persistidos on-flash.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* pthread_* vía mfs_plat.h */
#endif

#include "mfs_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfs_plat.h"

/* El adaptador aloja la instancia del núcleo por valor (diseño de instancia
 * única: `g_mfs_instance`) y consulta su geometría efectiva para statfs; por
 * eso necesita la definición interna de mf_t. Los front-ends FUSE/WinFsp NO
 * incluyen este encabezado: sólo consumen mfs_vfs.h. */
#include "mfs_internal.h"

#define MFS_VFS_MAX_FH 64u
/* Cota de ruta del adaptador (el núcleo acota internamente a su presupuesto
 * por modo; aquí sólo se evita el desbordamiento del búfer de normalización).
 */
#define MFS_VFS_PATH_MAX 512u
/* Bytes del superblock que se leen al sondear un volumen (magic + modo +
 * etiqueta); el SB tiene 256 B según §22.1. */
#define MFS_SB_PROBE_SIZE 256u

typedef struct {
  mfs_file *f;
  uint32_t gen;
  bool used;
} vfs_slot_t;

struct mfs_vfs {
  mf_t fs;
  mfs_config cfg;
  mfs_media_geom geom;
  mfs_blk *blk;
  bool readonly;
  mfs_mutex_t mtx;
  vfs_slot_t slot[MFS_VFS_MAX_FH];
  uint32_t gen_next;
  char dev[MFS_BLK_PATH_MAX];
};

void mfs_mount_opts_default(mfs_mount_opts *o) {
  if (!o)
    return;
  memset(o, 0, sizeof(*o));
  mfs_blk_opts_default(&o->blk);
  o->suite = 0xFFu;
  o->mode = MFS_MODE_UNSUPPORTED;
  /* Presupuesto de RAM declarado por los front-ends de host (256 KiB): con el
   * reparto de viabilidad de §6.1 (firmware 50 %, pila 15 %, periféricos 8 %,
   * margen 10 %) deja disponible ≈44 KiB, suficiente para seleccionar el modo
   * Extended (chunk 4096, volumen máx. 512 MiB). Un valor menor degradaría el
   * modo y, con él, el tamaño de página del layout. */
  o->ram_total = 262144u;
  o->file_perm = MFS_DEFAULT_FILE_MODE;
  o->dir_perm = MFS_DEFAULT_DIR_MODE;
  o->label = "MatrixFS";
}

/* ============================ Utilidades ================================ */

/* Normaliza la ruta a la forma del núcleo ("/a/b"): separadores '\' → '/',
 * barra inicial garantizada y sin barra final (salvo la raíz). */
static void norm_path(const char *in, char *out, size_t cap) {
  if (!in || cap == 0u) {
    if (cap)
      out[0] = '\0';
    return;
  }
  size_t o = 0u;
  if (in[0] != '/' && in[0] != '\\') {
    if (o + 1u < cap)
      out[o++] = '/';
  }
  for (size_t i = 0u; in[i] != '\0' && o + 1u < cap; i++) {
    char c = in[i];
    if (c == '\\')
      c = '/';
    if (c == '/' && o > 0u && out[o - 1u] == '/')
      continue; /* colapsa barras duplicadas */
    out[o++] = c;
  }
  while (o > 1u && out[o - 1u] == '/')
    o--;
  out[o] = '\0';
  if (o == 0u)
    strcpy(out, "/");
}

int mfs_vfs_errno(int st) {
  switch (st) {
  case MFS_OK:
    return 0;
  case MFS_EINVAL:
    return 22; /* EINVAL */
  case MFS_EIO:
    return 5; /* EIO    */
  case MFS_ENOSPC:
    return 28; /* ENOSPC */
  case MFS_EBACKPRESSURE:
    return 11; /* EAGAIN */
  case MFS_ETIMEDOUT_BUDGET:
    return 110; /* ETIMEDOUT */
  case MFS_ETABLEFULL:
    /* Tabla saturada (mapa L2P / ventana de inodos): para el usuario es
     * "no queda espacio", no un problema de descriptores abiertos. */
    return 28; /* ENOSPC */
  case MFS_EHW_UNSUPPORTED:
    return 95; /* EOPNOTSUPP */
  case MFS_EHEALTH_BLOCKED:
    return 5;
  case MFS_ESECURITY_STATE:
    return 13; /* EACCES */
  case MFS_EENERGY:
    return 11;
  case MFS_EPUF:
    return 5;
  case MFS_ECIPHER:
    return 95;
  case MFS_ESNAPMAX:
    return 28;
  case MFS_ENOTVIABLE:
    return 95;
  case MFS_EARCH:
    return 95;
  case MFS_EBUSY:
    return 16; /* EBUSY  */
  case MFS_ECORRUPT:
    return 5;
  case MFS_ENOTMOUNTED:
    return 19; /* ENODEV */
  case MFS_ENOENT:
    return 2; /* ENOENT */
  case MFS_EEXISTS:
    return 17; /* EEXIST */
  case MFS_EAGAIN:
    return 11;
  case MFS_ENOTSUP:
    return 95;
  case MFS_ESTATE:
    return 5;
  case MFS_EBADMSG:
    return 5;
  case MFS_EOVERFLOW:
    return 27; /* EFBIG  */
  case MFS_EDEADLK:
    return 35; /* EDEADLK */
  case MFS_EROFS:
    return 30; /* EROFS  */
  case MFS_EACCES:
    return 13; /* EACCES */
  default:
    return 5;
  }
}

/* ======================= Construcción de la config ====================== */

static int vfs_build_config(mfs_vfs *v, const char *device,
                            const mfs_mount_opts *o, bool readonly,
                            uint32_t ram_total_over) {
  memset(&v->cfg, 0, sizeof(v->cfg));
  v->cfg.drv = mfs_blk_driver(v->blk);
  mfs_blk_geom(v->blk, &v->geom);
  v->cfg.geom = &v->geom;
  v->cfg.drv_t0 = NULL;
  v->cfg.geom_t0 = NULL;
  v->cfg.ram_total = ram_total_over ? ram_total_over : (o ? o->ram_total : 0u);
  if (v->cfg.ram_total == 0u)
    v->cfg.ram_total = 262144u; /* presupuesto de host por defecto (§6.1) */
  v->cfg.arch_class = 2u;       /* 32-bit */
  v->cfg.forced_mode = o ? o->mode : MFS_MODE_UNSUPPORTED;
  v->cfg.suite_preferred = o ? o->suite : 0xFFu;
  v->cfg.bus_speed_hz = 0u; /* medido (sólo lectura §14.2) */
  v->cfg.profile_id = 0u;
  v->cfg.key = o ? o->key : NULL;
  v->cfg.rt_strict = o ? o->rt_strict : false;
  v->cfg.allow_convergent = o ? o->allow_convergent : false;
  v->cfg.zrp_enable = o ? o->zrp_enable : false;
  v->cfg.dedup_enable = o ? o->dedup_enable : false;
  v->cfg.cdc_enable = o ? o->cdc_enable : false;
  v->cfg.dab_enable = o ? o->dab_enable : false;
  v->cfg.dab_seed = 0x5EEDu;
  v->cfg.default_uid = o ? o->uid : 0u;
  v->cfg.default_gid = o ? o->gid : 0u;
  v->cfg.default_file_perm =
      (uint16_t)((o && o->file_perm) ? o->file_perm : MFS_DEFAULT_FILE_MODE);
  v->cfg.default_dir_perm =
      (uint16_t)((o && o->dir_perm) ? o->dir_perm : MFS_DEFAULT_DIR_MODE);
  v->readonly = readonly;
  (void)device;
  return 0;
}

/* ============================== Montaje ================================= */

/* Aplica y persiste la etiqueta de volumen pedida. `mf_format` limpia la
 * instancia completa (memset) y `mf_set_label` exige un volumen montado, por
 * lo que la etiqueta sólo puede fijarse tras formatear Y montar; en ese punto
 * `mf_set_label` la sella en el superblock (offset 123, fuera del CRC/MAC). */
static void vfs_apply_label(mfs_vfs *v, const mfs_mount_opts *o) {
  const char *lab = (o && o->label && o->label[0]) ? o->label : "MatrixFS";
  char tmp[MFS_LABEL_MAX];
  snprintf(tmp, sizeof(tmp), "%s", lab); /* trunca a MFS_LABEL_MAX-1 */
  (void)mf_set_label(&v->fs, tmp);
}

static mfs_vfs *vfs_alloc(void) {
  mfs_vfs *v = (mfs_vfs *)calloc(1, sizeof(*v));
  if (!v)
    return NULL;
  mfs_mutex_init(&v->mtx);
  return v;
}

int mfs_vfs_mount(const char *device, const mfs_mount_opts *o, mfs_vfs **out,
                  char *err, size_t errlen) {
  if (!device || !out)
    return MFS_EINVAL;
  mfs_mount_opts def;
  if (!o) {
    mfs_mount_opts_default(&def);
    o = &def;
  }
  *out = NULL;
  mfs_vfs *v = vfs_alloc();
  if (!v) {
    if (err && errlen)
      snprintf(err, errlen, "sin memoria");
    return MFS_EIO;
  }
  snprintf(v->dev, sizeof(v->dev), "%s", device);
  v->blk = mfs_blk_open(device, o->readonly, &o->blk, err, errlen);
  if (!v->blk) {
    mfs_mutex_destroy(&v->mtx);
    free(v);
    return MFS_EIO;
  }
  vfs_build_config(v, device, o, o->readonly, 0u);
  v->fs.cfg = &v->cfg;

  int st = mf_init(&v->fs, &v->cfg);
  if (st == MFS_ECORRUPT && o->format_if_needed && !o->readonly) {
    st = mf_format(&v->fs, NULL);
    if (st == MFS_OK) {
      st = mf_init(&v->fs, &v->cfg);
      if (st == MFS_OK)
        vfs_apply_label(v,
                        o); /* el volumen recién formateado toma la etiqueta */
    }
  }
  if (st != MFS_OK) {
    if (err && errlen)
      snprintf(err, errlen, "montaje fallido: %s", mfs_ststr((mfs_st)st));
    mfs_blk_close(v->blk);
    mfs_mutex_destroy(&v->mtx);
    free(v);
    return st;
  }
  /* El montaje no modifica metadatos del volumen (la etiqueta se fija al
   * formatear); se consulta con mfs_vfs_label(). */
  for (uint32_t i = 0; i < MFS_VFS_MAX_FH; i++) {
    v->slot[i].used = false;
    v->slot[i].gen = 0u;
  }
  v->gen_next = 1u;
  *out = v;
  return MFS_OK;
}

void mfs_vfs_unmount(mfs_vfs *v) {
  if (!v)
    return;
  mfs_mutex_lock(&v->mtx);
  for (uint32_t i = 0; i < MFS_VFS_MAX_FH; i++) {
    if (v->slot[i].used && v->slot[i].f) {
      (void)mf_close(v->slot[i].f);
      v->slot[i].used = false;
    }
  }
  if (v->fs.mounted)
    (void)mf_deinit(&v->fs);
  mfs_blk_flush(v->blk);
  mfs_mutex_unlock(&v->mtx);
  mfs_blk_close(v->blk);
  mfs_mutex_destroy(&v->mtx);
  free(v);
}

int mfs_vfs_sync(mfs_vfs *v) {
  if (!v)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  int st = mf_sync(&v->fs);
  mfs_blk_flush(v->blk);
  mfs_mutex_unlock(&v->mtx);
  return st;
}

int mfs_vfs_format(const char *device, const mfs_mount_opts *o, char *err,
                   size_t errlen) {
  if (!device)
    return MFS_EINVAL;
  mfs_mount_opts def;
  if (!o) {
    mfs_mount_opts_default(&def);
    o = &def;
  }
  if (o->readonly) {
    if (err && errlen)
      snprintf(err, errlen, "no se puede formatear en modo solo-lectura");
    return MFS_EROFS;
  }
  mfs_blk *blk = mfs_blk_open(device, false, &o->blk, err, errlen);
  if (!blk)
    return MFS_EIO;
  mfs_vfs *v = vfs_alloc();
  if (!v) {
    mfs_blk_close(blk);
    if (err && errlen)
      snprintf(err, errlen, "sin memoria");
    return MFS_EIO;
  }
  v->blk = blk;
  vfs_build_config(v, device, o, false, 0u);
  v->fs.cfg = &v->cfg;
  int st = mf_format(&v->fs, NULL);
  if (st == MFS_OK) {
    /* La etiqueta sólo puede persistirse con el volumen montado (§22.1). */
    st = mf_init(&v->fs, &v->cfg);
    if (st == MFS_OK) {
      vfs_apply_label(v, o);
      (void)mf_deinit(&v->fs);
    }
  }
  if (st != MFS_OK && err && errlen)
    snprintf(err, errlen, "formateo fallido: %s", mfs_ststr((mfs_st)st));
  mfs_blk_flush(blk);
  mfs_mutex_destroy(&v->mtx);
  free(v);
  mfs_blk_close(blk);
  return st;
}

/* Lectores LE locales (el sondeo no depende de los helpers internos). */
static uint32_t vfs_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t vfs_le64(const uint8_t *p) {
  return (uint64_t)vfs_le32(p) | ((uint64_t)vfs_le32(p + 4u) << 32);
}
/* Slot de superblock íntegro según magic + golden CRC (§22.1). */
static bool vfs_sb_ok(const uint8_t *sb) {
  if (sb[0] != MFS_SB_MAGIC0 || sb[1] != MFS_SB_MAGIC1 ||
      sb[2] != MFS_SB_MAGIC2 || sb[3] != MFS_SB_MAGIC3)
    return false;
  return mfs_crc32c(sb, 103u, 0u) == vfs_le32(sb + 103);
}

int mfs_vfs_probe(const char *device, const mfs_blk_opts *bopts,
                  char *label_out, size_t label_len, uint8_t *mode_out) {
  if (!device)
    return MFS_EINVAL;
  char err[128];
  mfs_blk *blk = mfs_blk_open(device, true, bopts, err, sizeof(err));
  if (!blk)
    return MFS_EIO;
  const mfs_l2_driver *d = mfs_blk_driver(blk);
  mfs_media_geom g;
  mfs_blk_geom(blk, &g);
  uint8_t sba[MFS_SB_PROBE_SIZE], sbb[MFS_SB_PROBE_SIZE];
  bool okA = d->read(d->ctx, 0u, sba, sizeof(sba)) == MFS_OK && vfs_sb_ok(sba);
  bool okB = d->read(d->ctx, g.erase_unit, sbb, sizeof(sbb)) == MFS_OK &&
             vfs_sb_ok(sbb);
  int result = MFS_ECORRUPT;
  if (okA || okB) {
    /* slot ganador por {época, seq}, igual que mf_init */
    bool useB;
    if (okA && okB) {
      uint32_t epA = vfs_le32(sba + 4), epB = vfs_le32(sbb + 4);
      uint64_t sqA = vfs_le64(sba + 95), sqB = vfs_le64(sbb + 95);
      useB = (epB > epA) || (epB == epA && sqB >= sqA);
    } else
      useB = okB;
    const uint8_t *sb = useB ? sbb : sba;
    result = MFS_OK;
    if (mode_out)
      *mode_out = sb[17];
    if (label_out && label_len) {
      size_t n = 0u;
      while (n + 1u < label_len && n < MFS_LABEL_MAX && sb[123 + n] != 0 &&
             sb[123 + n] != 0xFFu) {
        label_out[n] = (char)sb[123 + n];
        n++;
      }
      label_out[n] = '\0';
      if (label_out[0] == '\0')
        snprintf(label_out, label_len, "MatrixFS");
    }
  }
  mfs_blk_close(blk);
  return result;
}

bool mfs_vfs_is_readonly(const mfs_vfs *v) { return v ? v->readonly : true; }
uint32_t mfs_vfs_uid(const mfs_vfs *v) { return v ? v->cfg.default_uid : 0u; }
uint32_t mfs_vfs_gid(const mfs_vfs *v) { return v ? v->cfg.default_gid : 0u; }
const char *mfs_vfs_device(const mfs_vfs *v) { return v ? v->dev : ""; }

/* ============================== Handles ================================= */

static int slot_alloc_locked(mfs_vfs *v, mfs_file *f, mfs_vfs_fh *out) {
  for (uint32_t i = 0; i < MFS_VFS_MAX_FH; i++) {
    if (!v->slot[i].used) {
      v->slot[i].used = true;
      v->slot[i].f = f;
      v->slot[i].gen = v->gen_next++;
      if (v->gen_next == 0u)
        v->gen_next = 1u;
      *out = (((mfs_vfs_fh)v->slot[i].gen) << 8) | (mfs_vfs_fh)(i + 1u);
      return MFS_OK;
    }
  }
  return MFS_ETABLEFULL;
}

static mfs_file *slot_get_locked(mfs_vfs *v, mfs_vfs_fh fh, uint32_t *idx) {
  if (fh == 0u)
    return NULL;
  uint32_t i = (uint32_t)(fh & 0xFFu);
  uint32_t gen = (uint32_t)(fh >> 8);
  if (i == 0u || i > MFS_VFS_MAX_FH)
    return NULL;
  vfs_slot_t *s = &v->slot[i - 1u];
  if (!s->used || s->gen != gen || !s->f)
    return NULL;
  if (idx)
    *idx = i - 1u;
  return s->f;
}

static void fill_attr(const mfs_stat *st, mfs_vfs_attr *a) {
  a->size = st->size;
  a->mode = st->mode;
  a->uid = st->uid;
  a->gid = st->gid;
  a->mtime = st->mtime;
  a->nlink = st->nlink;
  a->ino = st->ino;
}

/* ============================== Metadatos =============================== */

int mfs_vfs_getattr(mfs_vfs *v, const char *path, mfs_vfs_attr *out) {
  if (!v || !path || !out)
    return MFS_EINVAL;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_stat st;
  mfs_mutex_lock(&v->mtx);
  int r = mf_stat(&v->fs, p, &st);
  mfs_mutex_unlock(&v->mtx);
  if (r != MFS_OK)
    return r;
  fill_attr(&st, out);
  return MFS_OK;
}

int mfs_vfs_readdir(mfs_vfs *v, const char *path, mfs_vfs_iter_fn fn,
                    void *ctx) {
  if (!v || !path || !fn)
    return MFS_EINVAL;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  mfs_dir *d = NULL;
  int r = mf_opendir(&v->fs, p, &d);
  if (r != MFS_OK) {
    mfs_mutex_unlock(&v->mtx);
    return r;
  }
  int rc = MFS_OK;
  for (;;) {
    mfs_dirent de;
    memset(&de, 0, sizeof(de));
    int rr = mf_readdir(d, &de);
    if (rr == MFS_ENOENT)
      break;
    if (rr != MFS_OK) {
      rc = rr;
      break;
    }
    mfs_vfs_attr a;
    fill_attr(&de.st, &a);
    if (fn(ctx, de.name, &a) != 0) {
      rc = MFS_EBUSY; /* el consumidor detuvo la iteración */
      break;
    }
  }
  (void)mf_closedir(d);
  mfs_mutex_unlock(&v->mtx);
  return rc;
}

/* Permisos POSIX: lectura=4, escritura=2, ejecución/búsqueda=1.
 * `fuid/fgid` son el propietario del nodo; `ruid/rgid` quien solicita. */
static bool perm_allows(uint32_t mode, uint32_t fuid, uint32_t fgid,
                        uint32_t mask, uint32_t ruid, uint32_t rgid) {
  if (ruid == 0u)
    return true; /* root (§21.2) */
  uint32_t bits;
  if (ruid == fuid)
    bits = (mode >> 6) & 7u;
  else if (rgid == fgid)
    bits = (mode >> 3) & 7u;
  else
    bits = mode & 7u;
  return (bits & mask) == mask;
}

int mfs_vfs_access(mfs_vfs *v, const char *path, uint32_t uid, uint32_t gid,
                   uint32_t mask) {
  mfs_vfs_attr a;
  int r = mfs_vfs_getattr(v, path, &a);
  if (r != MFS_OK)
    return r;
  if (v->readonly && (mask & 2u))
    return MFS_EROFS;
  return perm_allows(a.mode & 0777u, a.uid, a.gid, mask, uid, gid) ? MFS_OK
                                                                   : MFS_EACCES;
}

int mfs_vfs_setattr(mfs_vfs *v, const char *path, uint32_t mask,
                    const mfs_attr *a) {
  if (!v || !path || !a)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  int r = mf_setattr(&v->fs, p, mask & ~MFS_ATTR_SIZE, a);
  mfs_mutex_unlock(&v->mtx);
  if (r != MFS_OK)
    return r;
  if (mask & MFS_ATTR_SIZE)
    return mfs_vfs_truncate_path(v, p, a->size);
  return MFS_OK;
}

/* =============================== Ficheros =============================== */

int mfs_vfs_open(mfs_vfs *v, const char *path, uint32_t flags, mfs_vfs_fh *fh) {
  if (!v || !path || !fh)
    return MFS_EINVAL;
  /* El modo de acceso es `flags & MFS_O_RDWR`: MFS_O_RDONLY (0x1) comparte bit
   * con MFS_O_RDWR (0x3), por lo que la prueba de escritura debe aislar el bit
   * MFS_O_WRONLY. */
  if (v->readonly && (flags & MFS_O_WRONLY))
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_file *f = NULL;
  mfs_mutex_lock(&v->mtx);
  int r = mf_open(&v->fs, p, flags, &f);
  if (r == MFS_OK)
    r = slot_alloc_locked(v, f, fh);
  if (r != MFS_OK && f)
    (void)mf_close(f);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_create(mfs_vfs *v, const char *path, uint16_t mode, uint32_t uid,
                   uint32_t gid, mfs_vfs_fh *fh) {
  if (!v || !path || !fh)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_file *f = NULL;
  mfs_mutex_lock(&v->mtx);
  int r = mf_open(&v->fs, p, MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
  if (r == MFS_OK && (mode || uid || gid)) {
    mfs_attr a;
    uint32_t mask = 0u;
    memset(&a, 0, sizeof(a));
    if (mode) {
      a.mode = mode;
      mask |= MFS_ATTR_MODE;
    }
    if (uid) {
      a.uid = uid;
      mask |= MFS_ATTR_UID;
    }
    if (gid) {
      a.gid = gid;
      mask |= MFS_ATTR_GID;
    }
    (void)mf_setattr(&v->fs, p, mask, &a);
  }
  if (r == MFS_OK)
    r = slot_alloc_locked(v, f, fh);
  if (r != MFS_OK && f)
    (void)mf_close(f);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_read(mfs_vfs *v, mfs_vfs_fh fh, void *buf, uint64_t off, size_t len,
                 size_t *rd) {
  if (!v)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  mfs_file *f = slot_get_locked(v, fh, NULL);
  int r = MFS_EINVAL;
  if (f) {
    r = mf_seek(f, (int64_t)off, MFS_SEEK_SET);
    if (r == MFS_OK) {
      size_t got = 0u;
      r = mf_read(f, buf, len, &got);
      if (rd)
        *rd = got;
    }
  }
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_write(mfs_vfs *v, mfs_vfs_fh fh, const void *buf, uint64_t off,
                  size_t len, size_t *wr) {
  if (!v)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  mfs_mutex_lock(&v->mtx);
  mfs_file *f = slot_get_locked(v, fh, NULL);
  int r = MFS_EINVAL;
  if (f) {
    r = mf_seek(f, (int64_t)off, MFS_SEEK_SET);
    if (r == MFS_OK) {
      size_t put = 0u;
      r = mf_write(f, buf, len, &put);
      if (wr)
        *wr = put;
    }
  }
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_flush(mfs_vfs *v, mfs_vfs_fh fh) {
  if (!v)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  mfs_file *f = slot_get_locked(v, fh, NULL);
  int r = f ? mf_sync(&v->fs) : MFS_EINVAL;
  if (r == MFS_OK)
    (void)mfs_blk_flush(v->blk);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_release(mfs_vfs *v, mfs_vfs_fh fh) {
  if (!v)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  uint32_t idx = 0u;
  mfs_file *f = slot_get_locked(v, fh, &idx);
  int r = MFS_EINVAL;
  if (f) {
    r = mf_close(f);
    v->slot[idx].used = false;
    v->slot[idx].f = NULL;
  }
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_truncate(mfs_vfs *v, mfs_vfs_fh fh, uint64_t size) {
  if (!v)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  mfs_mutex_lock(&v->mtx);
  mfs_file *f = slot_get_locked(v, fh, NULL);
  int r = f ? mf_truncate(f, size) : MFS_EINVAL;
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_truncate_path(mfs_vfs *v, const char *path, uint64_t size) {
  if (!v || !path)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  mfs_file *f = NULL;
  int r = mf_open(&v->fs, p, MFS_O_RDWR, &f);
  if (r == MFS_OK) {
    r = mf_truncate(f, size);
    (void)mf_close(f);
  }
  mfs_mutex_unlock(&v->mtx);
  return r;
}

/* ============================= Directorios ============================== */

int mfs_vfs_mkdir(mfs_vfs *v, const char *path, uint16_t mode, uint32_t uid,
                  uint32_t gid) {
  if (!v || !path)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  int r = mf_mkdir(&v->fs, p);
  if (r == MFS_OK && (mode || uid || gid)) {
    mfs_attr a;
    uint32_t mask = 0u;
    memset(&a, 0, sizeof(a));
    if (mode) {
      a.mode = mode;
      mask |= MFS_ATTR_MODE;
    }
    if (uid) {
      a.uid = uid;
      mask |= MFS_ATTR_UID;
    }
    if (gid) {
      a.gid = gid;
      mask |= MFS_ATTR_GID;
    }
    (void)mf_setattr(&v->fs, p, mask, &a);
  }
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_rmdir(mfs_vfs *v, const char *path) {
  if (!v || !path)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  int r = mf_unlink(&v->fs, p); /* el núcleo valida directorio vacío (EBUSY) */
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_unlink(mfs_vfs *v, const char *path) {
  if (!v || !path)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char p[MFS_VFS_PATH_MAX];
  norm_path(path, p, sizeof(p));
  mfs_mutex_lock(&v->mtx);
  int r = mf_unlink(&v->fs, p);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_rename(mfs_vfs *v, const char *from, const char *to) {
  if (!v || !from || !to)
    return MFS_EINVAL;
  if (v->readonly)
    return MFS_EROFS;
  char a[MFS_VFS_PATH_MAX];
  char b[MFS_VFS_PATH_MAX];
  norm_path(from, a, sizeof(a));
  norm_path(to, b, sizeof(b));
  mfs_mutex_lock(&v->mtx);
  int r = mf_rename(&v->fs, a, b);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

/* ========================== Volumen y salud ============================= */

int mfs_vfs_statfs(mfs_vfs *v, uint64_t *total_bytes, uint64_t *free_bytes,
                   uint64_t *used_bytes) {
  if (!v)
    return MFS_EINVAL;
  mfs_health_t h;
  mfs_mutex_lock(&v->mtx);
  int r = mf_ioctl(&v->fs, MFS_IOCTL_HEALTH, &h);
  uint32_t zone_blocks = v->fs.zone_blocks ? v->fs.zone_blocks : 1u;
  uint64_t zone_bytes = (uint64_t)zone_blocks * v->fs.hwv.erase_unit;
  /* Área de datos = ventana de zonas ZLF; la región baja (SB A/B, HWV, anillo
   * de tokens) no es direccionable por el usuario. */
  uint64_t total = (uint64_t)v->fs.zone_cap * zone_bytes;
  /* Cada página de datos consume además una entrada del mapa L2P: la capacidad
   * efectiva es el menor de {espacio libre en zonas, entradas libres del mapa}.
   */
  uint64_t l2p_bytes = (uint64_t)(mfs_l2p_capacity() - mfs_l2p_used()) *
                       (uint64_t)mfs_payload_bytes(&v->fs);
  mfs_mutex_unlock(&v->mtx);
  if (r != MFS_OK)
    return r;
  uint64_t freeb = (uint64_t)h.free_pages * zone_bytes;
  if (freeb > l2p_bytes)
    freeb = l2p_bytes;
  if (freeb > total)
    freeb = total;
  if (total_bytes)
    *total_bytes = total;
  if (free_bytes)
    *free_bytes = freeb;
  if (used_bytes)
    *used_bytes = total - freeb;
  return MFS_OK;
}

int mfs_vfs_health(mfs_vfs *v, mfs_health_t *h) {
  if (!v || !h)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  int r = mf_ioctl(&v->fs, MFS_IOCTL_HEALTH, h);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_label(mfs_vfs *v, char *out, size_t len) {
  if (!v || !out || len == 0u)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  int r = mf_get_label(&v->fs, out, len);
  mfs_mutex_unlock(&v->mtx);
  return r;
}

int mfs_vfs_verify(mfs_vfs *v, mfs_verify_level lvl) {
  if (!v)
    return MFS_EINVAL;
  mfs_mutex_lock(&v->mtx);
  int r = mf_verify(&v->fs, lvl);
  mfs_mutex_unlock(&v->mtx);
  return r;
}
