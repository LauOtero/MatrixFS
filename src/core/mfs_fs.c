/* mfs_fs.c — núcleo MatrixFS Ultra «ATLAS» v1.0 (§21–§24)
 * Ciclo de vida (FSM de montaje §24.2), POSIX-subset, VIO/DAIO (§13.1),
 * transacciones/savepoints (§9.6), snapshots O(1)/FlashPatch (§10.8),
 * EDP (§12.3), DAB EXP3 (§24.4), CUSUM (§16), HCT (§16) y verificación.
 *
 * Layout físico (sectores alineados al erase_unit para cumplir la semántica
 * NOR: prohibido reprogramar sin erase previo):
 *   sector 0 : SB A (@0) + HWV (@512)
 *   sector 1 : SB B (@erase_unit)
 *   sector 2 : anillo de tokens T1
 *   sector 3…: zonas ZLF (MFS_MAX_ZONES)
 */
#include "mfs_internal.h"
#include <string.h>

bool g_mfs_edp_inject;           /* hook test: simula caída de rail */
static uint32_t g_lba_next;      /* asignador LBA lógico por instancia única */
static uint32_t g_ino_next = 2u; /* ino 0=root implícito, 1=reservado */

#define ROOT_INO 0u

/* Permisos/propietario del inodo (§21.2) */
static uint16_t mfs_ino_perm(const mf_t *fs, const mfs_inode_ram_t *ino);
static void mfs_ino_defaults(mf_t *fs, mfs_inode_ram_t *ino);

/* Copia de nombre acotada y siempre NUL-terminada (evita strncpy/memcpy
 * con posible truncado; §20.1 sin behavior indefinido). */
static void mfs_name_set(char *dst, const char *src, size_t cap) {
  size_t n = strlen(src);
  if (n >= cap)
    n = cap - 1u;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* =====================================================================
 * Superblock (§22.1) — 256 B, slots A/B en sectores distintos
 * ===================================================================== */
static mfs_st sb_serialize(mf_t *fs, uint32_t epoch, uint32_t gen,
                           uint8_t sb[MFS_SB_SIZE]) {
  memset(sb, 0xFFu, MFS_SB_SIZE);
  sb[0] = MFS_SB_MAGIC0;
  sb[1] = MFS_SB_MAGIC1;
  sb[2] = MFS_SB_MAGIC2;
  sb[3] = MFS_SB_MAGIC3;
  mfs_st32(sb + 4, epoch);
  mfs_st32(sb + 8, gen);
  mfs_st16(sb + 12, 0u); /* secuencia global alta (reservado) */
  sb[16] = fs->suite;
  sb[17] = fs->mode;
  mfs_st16(sb + 18, 0u);
  mfs_st16(sb + 20,
           (uint16_t)(fs->root_art >> 16)); /* raíz ART {zone:2,page:4} */
  mfs_st32(sb + 22, fs->root_art & 0xFFFFu);
  mfs_st32(sb + 26, fs->zone_wal);
  mfs_st16(sb + 30, (uint16_t)fs->wal_count);
  mfs_st16(sb + 32, (uint16_t)(MFS_WAL_WINDOW_MAX));
  sb[34] = fs->snap_count;
  for (uint8_t i = 0; i < fs->snap_count && i < 8u; i++) {
    mfs_st16(sb + 35 + i * 6u, (uint16_t)(fs->snap_roots[i] >> 16));
    mfs_st32(sb + 37 + i * 6u, fs->snap_roots[i] & 0xFFFFu);
  }
  for (uint8_t i = 0; i < 4u; i++)
    sb[83 + i] = 0u; /* fingerprints ODT */
  mfs_st32(sb + 87, MFS_HWV_OFFSET);
  mfs_st32(sb + 91, fs->hwv.crc);
  mfs_st64(sb + 95, (uint64_t)fs->seq);
  mfs_st32(sb + 103, mfs_crc32c(sb, 103u, 0u)); /* golden_crc sobre 0..102 */
  /* MAC (B3-keyed/HMAC truncado a 16 B) si hay clave (§15 MFS-SEC-004) */
  if (fs->cfg->key) {
    uint8_t mac[32];
    mfs_hmac_sha256(fs->cfg->key, 32u, sb, 107u, mac);
    memcpy(sb + 107, mac, 16u);
  }
  /* etiqueta de volumen (informativa, fuera del CRC y del MAC para no romper
   * la compatibilidad de los slots ya existentes) */
  for (uint32_t i = 0; i < MFS_LABEL_MAX; i++)
    sb[123 + i] = (uint8_t)fs->label[i];
  return MFS_OK;
}

static mfs_st sb_read(mf_t *fs, uint32_t addr, uint32_t *epoch, uint32_t *gen,
                      uint32_t *seq_out) {
  uint8_t sb[MFS_SB_SIZE];
  mfs_st st = mfs_read(fs, addr, sb, MFS_SB_SIZE);
  if (st != MFS_OK)
    return st;
  if (sb[0] != MFS_SB_MAGIC0 || sb[1] != MFS_SB_MAGIC1 ||
      sb[2] != MFS_SB_MAGIC2 || sb[3] != MFS_SB_MAGIC3)
    return MFS_ECORRUPT;
  if (mfs_crc32c(sb, 103u, 0u) != mfs_ld32(sb + 103))
    return MFS_ECORRUPT;
  if (fs->cfg->key) {
    uint8_t mac[32], want[16];
    mfs_hmac_sha256(fs->cfg->key, 32u, sb, 107u, mac);
    memcpy(want, sb + 107, 16u);
    /* verificación en tiempo constante (MFS-SEC-005) */
    uint8_t d = 0u;
    for (uint32_t i = 0; i < 16u; i++)
      d |= (uint8_t)(want[i] ^ mac[i]);
    if (d != 0u)
      return MFS_ESECURITY_STATE;
  }
  *epoch = mfs_ld32(sb + 4);
  if (gen)
    *gen = mfs_ld32(sb + 8);
  if (seq_out)
    *seq_out = (uint32_t)mfs_ld64(sb + 95);
  return MFS_OK;
}

static mfs_st sb_write(mf_t *fs, uint32_t slot, uint32_t epoch, uint32_t gen) {
  uint32_t base = slot ? fs->hwv.erase_unit : 0u;
  mfs_st st = mfs_erase(fs, base);
  if (st != MFS_OK)
    return st;
  if (slot == 0u) { /* HWV comparte sector con SB A */
    uint8_t h[64];
    mfs_hwv_serialize(&fs->hwv, h);
    st = mfs_write(fs, MFS_HWV_OFFSET, h, 64u);
    if (st != MFS_OK)
      return st;
  }
  uint8_t sb[MFS_SB_SIZE];
  sb_serialize(fs, epoch, gen, sb);
  return mfs_write(fs, base, sb, MFS_SB_SIZE);
}

/* =====================================================================
 * Tabla de zonas: dimensionada por geometría real del medio
 * ===================================================================== */
static void zones_init_table(mf_t *fs) {
  uint32_t base = mfs_zone_base(&fs->hwv);
  uint32_t total = fs->hwv.media_size;
  uint32_t eu = fs->hwv.erase_unit;
  uint32_t chunk = mfs_page_bytes(fs);
  /* Una zona agrupa 1..4 bloques (§8.1) de forma que quepa al menos un
   * registro completo (chunk + cabecera de zona). */
  uint32_t blocks = 0;
  for (uint32_t b = 1u; b <= 4u; b++) {
    if (b * eu >= chunk + MFS_ZONEHDR_SIZE) {
      blocks = b;
      break;
    }
  }
  if (blocks == 0u)
    blocks = 4u;
  fs->zone_blocks = (uint8_t)blocks;
  uint32_t stride = blocks * eu;
  uint32_t cap = (total > base) ? (total - base) / stride : 0u;
  if (cap > MFS_MAX_ZONES)
    cap = MFS_MAX_ZONES;
  fs->zone_cap = cap;
  for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
    mfs_zone_t *z = &fs->zones[i];
    memset(z, 0, sizeof(*z));
    z->zone_id = (uint16_t)i;
    z->state = MFS_Z_EMPTY;
    z->start_addr = base + i * stride;
    z->size = stride;
    z->total_pages = (uint16_t)((z->size - MFS_ZONEHDR_SIZE) / chunk);
    /* perfil de desgaste (§11.1): NOR ~100 k ciclos, NAND ~3 k. `pe_max` y la
     * API ELM son de 16 bits, así que el perfil NOR se satura dentro del rango
     * representable (el ELM lo usa como referencia relativa de salud). */
    z->pe_max = (fs->hwv.media_type == MFS_MEDIA_NOR_SPI) ? 60000u : 3000u;
    z->pe_cycles = 0u;
    z->ber_x1e6 = 0u;
    z->ber_slope_x1000 = 0u;
    z->trend = 0u;
    z->slc = 0u;
  }
}

/* =====================================================================
 * Inodos (ventana flash-first) + persistencia con DIRENT (§22.3)
 * ===================================================================== */
mfs_inode_ram_t *mfs_ino_get(mf_t *fs, uint32_t ino) {
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
    if (fs->inos[i].valid && fs->inos[i].ino == ino)
      return &fs->inos[i];
  return NULL;
}

mfs_inode_ram_t *mfs_ino_alloc(mf_t *fs) {
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
    if (!fs->inos[i].valid) {
      mfs_inode_ram_t *n = &fs->inos[i];
      memset(n, 0, sizeof(*n));
      n->valid = 1u;
      n->ino = g_ino_next++;
      return n;
    }
  }
  /* flash-first: desalojar el inodo no-open menos recientemente usado */
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
    mfs_inode_ram_t *n = &fs->inos[i];
    if (!n->valid || n->ino == ROOT_INO)
      continue;
    bool open = false;
    for (uint32_t f = 0; f < MFS_MAX_FILES_OPEN; f++)
      if (fs->open_files[f] && fs->open_files[f]->ino == n->ino)
        open = true;
    if (!open) {
      mfs_meta_flush(fs, n->ino);
      memset(n, 0, sizeof(*n));
      n->valid = 1u;
      n->ino = g_ino_next++;
      return n;
    }
  }
  return NULL; /* todos calientes: ETABLEFULL */
}

void mfs_ino_evict(mf_t *fs, mfs_inode_ram_t *ino) {
  if (!ino || ino->ino == ROOT_INO)
    return;
  mfs_meta_flush(fs, ino->ino);
  memset(ino, 0, sizeof(*ino));
}

uint32_t mfs_next_lba(mf_t *fs) {
  (void)fs;
  return g_lba_next++;
}

/* serializar inodo a registro INODE (orden determinista LE) */
static void ino_ser(const mfs_inode_ram_t *n, uint8_t *b, uint16_t *len) {
  uint16_t o = 0;
  mfs_st32(b + o, n->ino);
  o += 4;
  mfs_st32(b + o, n->size);
  o += 4;
  mfs_st32(b + o, n->mtime);
  o += 4;
  mfs_st16(b + o, n->nlink);
  o += 2;
  b[o++] = n->type;
  b[o++] = n->gen;
  b[o++] = n->snapid;
  b[o++] = n->hotness;
  mfs_st32(b + o, n->parent);
  o += 4;
  for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
    mfs_st32(b + o, n->ext[e].vpage);
    o += 4;
    mfs_st32(b + o, n->ext[e].ppage);
    o += 4;
    mfs_st16(b + o, n->ext[e].npages);
    o += 2;
    mfs_st16(b + o, n->ext[e].flags);
    o += 2;
  }
  /* extensión §21.2: propietario y permisos (compatible con registros previos
   * de 70 B: el lector sólo los interpreta si len >= 82) */
  mfs_st32(b + o, n->uid);
  o += 4;
  mfs_st32(b + o, n->gid);
  o += 4;
  mfs_st16(b + o, n->perm);
  o += 2;
  mfs_st16(b + o, n->pad2);
  o += 2;
  *len = o;
}

void mfs_ino_deser(mfs_inode_ram_t *n, const uint8_t *b, uint16_t len) {
  if (len < 22u)
    return;
  uint16_t o = 0;
  n->valid = 1u;
  n->ino = mfs_ld32(b + o);
  o += 4;
  n->size = mfs_ld32(b + o);
  o += 4;
  n->mtime = mfs_ld32(b + o);
  o += 4;
  n->nlink = mfs_ld16(b + o);
  o += 2;
  n->type = b[o++];
  n->gen = b[o++];
  n->snapid = b[o++];
  n->hotness = b[o++];
  n->parent = mfs_ld32(b + o);
  o += 4;
  for (uint32_t e = 0; e < MFS_EXT_INLINE && o + 12u <= len; e++) {
    n->ext[e].vpage = mfs_ld32(b + o);
    o += 4;
    n->ext[e].ppage = mfs_ld32(b + o);
    o += 4;
    n->ext[e].npages = mfs_ld16(b + o);
    o += 2;
    n->ext[e].flags = mfs_ld16(b + o);
    o += 2;
  }
  /* extensión §21.2 (opcional: registros antiguos de 70 B no la portan) */
  if (o + 12u <= len) {
    n->uid = mfs_ld32(b + o);
    o += 4;
    n->gid = mfs_ld32(b + o);
    o += 4;
    n->perm = mfs_ld16(b + o);
    o += 2;
    n->pad2 = mfs_ld16(b + o);
  }
}

/* persistir inodo (registro INODE) + nombre (registro DIRENT) vía WAL */
mfs_st mfs_meta_flush(mf_t *fs, uint32_t ino) {
  mfs_inode_ram_t *n = mfs_ino_get(fs, ino);
  if (!n)
    return MFS_OK;
  uint8_t buf[MFS_SCRATCH_MAX];
  uint16_t len = 0;
  ino_ser(n, buf, &len);
  mfs_st st = mfs_wal_append(fs, 0x100000u + ino, MFS_RT_INODE, buf, len, NULL);
  if (st != MFS_OK)
    return st;
  /* HMT (§11.11): espejo del metadato caliente en T0 */
  if (fs->hmt_active)
    (void)mfs_hmt_t0_write(fs, MFS_RT_INODE, 0x100000u + ino, buf, len, NULL);
  uint8_t d[2u + 4u + 4u + MFS_NAME_MAX];
  mfs_st32(d, ino);
  mfs_st32(d + 4, n->parent);
  size_t nl = strlen(n->name);
  if (nl > MFS_NAME_MAX - 1u)
    nl = MFS_NAME_MAX - 1u;
  d[8] = (uint8_t)nl;
  memcpy(d + 9, n->name, nl);
  {
    uint16_t dlen = (uint16_t)(9u + nl);
    st = mfs_wal_append(fs, 0x300000u + ino, MFS_RT_DIRENT, d, dlen, NULL);
    if (st == MFS_OK && fs->hmt_active)
      (void)mfs_hmt_t0_write(fs, MFS_RT_DIRENT, 0x300000u + ino, d, dlen, NULL);
  }
  return st;
}

/* ==== lookup de rutas sobre la ventana de inodos RAM ==== */
int mfs_lookup(mf_t *fs, const char *path, uint32_t *parent_out) {
  if (path == NULL)
    return MFS_EINVAL;
  while (*path == '/')
    path++;
  if (*path == '\0') {
    if (parent_out)
      *parent_out = ROOT_INO;
    return (int)ROOT_INO;
  }
  uint32_t parent = ROOT_INO;
  char comp[MFS_NAME_MAX];
  for (;;) {
    uint32_t i = 0;
    while (path[i] != '\0' && path[i] != '/' && i < MFS_NAME_MAX - 1u) {
      comp[i] = path[i];
      i++;
    }
    comp[i] = '\0';
    path += i;
    while (*path == '/')
      path++;
    int found = -1;
    for (uint32_t k = 0; k < sizeof(fs->inos) / sizeof(fs->inos[0]); k++) {
      mfs_inode_ram_t *n = &fs->inos[k];
      if (n->valid && n->parent == parent && strcmp(n->name, comp) == 0) {
        found = (int)n->ino;
        break;
      }
    }
    if (found < 0)
      return MFS_ENOENT;
    parent = (uint32_t)found;
    if (*path == '\0') {
      if (parent_out)
        *parent_out = parent;
      return (int)parent;
    }
  }
}

/* =====================================================================
 * Extents de datos — resolución por L2P (resistente a GC/relocación).
 * Los 4 primeros extents de cada inodo son inline (§22.3 INO-L); el resto de
 * páginas (vpage >= MFS_EXT_INLINE) se localizan directamente por L2P con la
 * clave `extent_lba(ino, vpage)`. El propio registro DATA porta ese LBA en su
 * cabecera E2G, de modo que el montaje reconstruye el mapa sin estructuras
 * adicionales: no hay tabla de desbordamiento en RAM ni registros EXTENT
 * (eran redundantes y duplicaban el coste de mapeo y de medio por página).
 * ===================================================================== */
static uint32_t extent_lba(uint32_t ino, uint32_t vpage) {
  return 0x200000u + ((ino << 16) | (vpage & 0xFFFFu));
}

uint32_t mfs_extent_read(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                         uint8_t *buf, uint16_t bufsize, uint16_t *rlen) {
  uint32_t lba = extent_lba(ino->ino, vpage);
  uint32_t pp = mfs_l2p_get(lba);
  if (pp == MFS_L2P_FREE)
    return MFS_PPAGE_NONE; /* hueco: el lector devuelve ceros */
  uint16_t r = 0;
  mfs_st st = mfs_data_read(fs, pp, lba, buf, bufsize, &r);
  if (st != MFS_OK)
    return MFS_PPAGE_ERROR; /* página viva ilegible: no es un hueco */
  if (rlen)
    *rlen = r;
  return pp;
}

mfs_st mfs_extent_write(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                        const uint8_t *buf, uint16_t len) {
  uint32_t lba = extent_lba(ino->ino, vpage);
  uint32_t pp = 0;
  mfs_st st = mfs_data_write(fs, lba, buf, len, ino->hotness, MFS_RT_DATA, &pp);
  if (st != MFS_OK)
    return st;
  if (vpage < MFS_EXT_INLINE) { /* caché inline del inodo (§22.3 INO-L) */
    uint32_t slot = vpage;
    ino->ext[slot].vpage = vpage;
    ino->ext[slot].ppage = pp;
    ino->ext[slot].npages = 1u;
    ino->ext[slot].flags = (uint16_t)(fs->epoch & 0xFu);
  }
  return MFS_OK;
}

/* Páginas lógicas que cubre un tamaño de fichero con el payload vigente. */
uint32_t mfs_vpages_of(mf_t *fs, uint32_t size) {
  uint32_t pb = mfs_payload_bytes(fs);
  if (pb == 0u)
    return 0u;
  return (size + pb - 1u) / pb;
}

/* Libera del L2P las páginas de datos de `ino` en [from_vpage, to_vpage):
 * dejan de ser páginas vivas para la GC (evita fugas de espacio al truncar o
 * borrar ficheros grandes). */
void mfs_extent_drop_range(mf_t *fs, uint32_t ino, uint32_t from_vpage,
                           uint32_t to_vpage) {
  for (uint32_t vp = from_vpage; vp < to_vpage; vp++) {
    uint32_t lba = extent_lba(ino, vp);
    if (mfs_l2p_get(lba) == MFS_L2P_FREE)
      continue;
    mfs_l2p_drop(lba);
    mfs_gld_add(fs, 1u, false);
  }
}

/* =====================================================================
 * Recuperación de metadatos al montar (reconstrucción por escaneo E2G)
 * ===================================================================== */
typedef struct {
  uint8_t len;
  char name[MFS_NAME_MAX];
  uint32_t parent;
} dir_pend_t;
/* ==== ventana de inodos (flash-first) ==== */

/* Marca circular de recencia: se actualiza con cada registro INODE aplicado. */
static uint8_t g_ino_recency;

/* Devuelve un hueco de la ventana de inodos: uno libre si lo hay y, si la
 * ventana está llena, el del inodo actualizado hace más tiempo. Los registros
 * se aplican en orden cronológico durante la reconstrucción, así que desalojar
 * por recencia conserva en RAM los ficheros más recientes (que son los que el
 * usuario acaba de usar) en lugar de descartar sus registros. El inodo
 * desalojado permanece íntegro en el medio: se reconstruye al remontar. */
static mfs_inode_ram_t *ino_window_take(mf_t *fs) {
  uint32_t n = sizeof(fs->inos) / sizeof(fs->inos[0]);
  for (uint32_t i = 1u; i < n; i++)
    if (!fs->inos[i].valid)
      return &fs->inos[i];
  uint32_t best = 0u;
  uint8_t best_age = 0u;
  for (uint32_t i = 1u; i < n; i++) {
    uint8_t age = (uint8_t)(g_ino_recency - fs->inos[i].next);
    if (best == 0u || age > best_age) {
      best = i;
      best_age = age;
    }
  }
  if (best == 0u)
    return NULL;
  memset(&fs->inos[best], 0, sizeof(fs->inos[best]));
  return &fs->inos[best];
}

mfs_st mfs_inode_apply(mf_t *fs, const uint8_t *pl, uint16_t len,
                       uint16_t *max_ino) {
  mfs_inode_ram_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  mfs_ino_deser(&tmp, pl, len);
  if (tmp.ino == ROOT_INO)
    return MFS_OK;
  if (tmp.nlink == 0u) { /* lápida: el inodo fue eliminado */
    mfs_inode_ram_t *old = mfs_ino_get(fs, tmp.ino);
    if (old) {
      /* las páginas del fichero borrado dejan de estar vivas: si no, la GC
       * las relocalizaría indefinidamente tras cada remontaje */
      mfs_extent_drop_range(fs, tmp.ino, 0u, mfs_vpages_of(fs, old->size));
      memset(old, 0, sizeof(*old));
    }
    return MFS_OK;
  }
  mfs_inode_ram_t *n = mfs_ino_get(fs, tmp.ino);
  if (!n)
    n = ino_window_take(fs);
  if (!n)
    return MFS_ETABLEFULL;
  uint32_t keep_parent = n->parent; /* el nombre/padre lo fija DIRENT */
  char keep_name[MFS_NAME_MAX];
  memcpy(keep_name, n->name, sizeof(keep_name));
  uint8_t keep_valid = n->valid;
  *n = tmp;
  n->valid = 1u;
  n->parent = keep_valid ? keep_parent : tmp.parent;
  memcpy(n->name, keep_name, sizeof(n->name));
  n->next = (uint8_t)++g_ino_recency; /* marca de recencia */
  mfs_ino_defaults(fs, n); /* normaliza registros previos sin permisos */
  if ((uint16_t)tmp.ino > *max_ino)
    *max_ino = (uint16_t)tmp.ino;
  return MFS_OK;
}

void mfs_dirent_apply(mf_t *fs, const uint8_t *pl, uint16_t len) {
  if (len < 9u)
    return;
  uint32_t ino = mfs_ld32(pl);
  if (ino == ROOT_INO)
    return;
  mfs_inode_ram_t *n = mfs_ino_get(fs, ino);
  if (!n)
    return;
  uint8_t nl = pl[8];
  if ((uint16_t)nl + 9u > len)
    nl = (uint8_t)(len - 9u);
  if (nl >= MFS_NAME_MAX)
    nl = MFS_NAME_MAX - 1u;
  n->parent = mfs_ld32(pl + 4);
  memcpy(n->name, pl + 9, nl);
  n->name[nl] = '\0';
}

void mfs_epoch_apply(mf_t *fs, const uint8_t *pl, uint16_t len) {
  if (len < 1u)
    return;
  uint8_t cnt = pl[0];
  if (cnt > MFS_MAX_SNAPS)
    cnt = MFS_MAX_SNAPS;
  fs->snap_count = cnt;
  for (uint8_t i = 0; i < cnt && (uint16_t)(1u + i * 8u + 8u) <= len; i++) {
    fs->snaps[i] = mfs_ld32(pl + 1 + i * 8u);
    fs->snap_roots[i] = mfs_ld32(pl + 5 + i * 8u);
  }
}

/* Los metadatos viajan envueltos en un registro WALENT (MFS-TX-002). Esta
 * función desempaqueta la envoltura y devuelve el kind interno + su payload. */
static bool unwrap(const uint8_t *in, uint16_t inlen, uint8_t outer_kind,
                   uint8_t *kind_out, const uint8_t **data, uint16_t *dlen,
                   uint32_t *txid_out) {
  if (txid_out)
    *txid_out = 0u;
  if (outer_kind == MFS_RT_WALENT) {
    if (inlen < 12u)
      return false;
    *kind_out = in[10];
    uint16_t dl = mfs_ld16(in + 8);
    if ((uint32_t)12u + dl > inlen)
      return false;
    if (txid_out)
      *txid_out = mfs_ld32(in);
    *data = in + 12;
    *dlen = dl;
    return true;
  }
  *kind_out = outer_kind;
  *data = in;
  *dlen = inlen;
  return true;
}

/* ==== marcas de transacción (TXMARK) para replay determinista (MFS-TX-003)
 * ==== */
#define MFS_TXMARK_MAX 64u
typedef struct {
  uint32_t txid;
  uint8_t committed;
  uint8_t used;
} txmark_t;
static txmark_t g_txmarks[MFS_TXMARK_MAX];

static void txmark_reset(void) {
  for (uint32_t i = 0; i < MFS_TXMARK_MAX; i++)
    g_txmarks[i].used = 0u;
}

static void txmark_set(uint32_t txid, uint8_t committed) {
  for (uint32_t i = 0; i < MFS_TXMARK_MAX; i++) {
    if (g_txmarks[i].used && g_txmarks[i].txid == txid) {
      g_txmarks[i].committed = committed;
      return;
    }
  }
  for (uint32_t i = 0; i < MFS_TXMARK_MAX; i++) {
    if (!g_txmarks[i].used) {
      g_txmarks[i].used = 1u;
      g_txmarks[i].txid = txid;
      g_txmarks[i].committed = committed;
      return;
    }
  }
}

/* -1 = desconocido (⇒ se descarta, conservador) */
static int txmark_get(uint32_t txid) {
  for (uint32_t i = 0; i < MFS_TXMARK_MAX; i++)
    if (g_txmarks[i].used && g_txmarks[i].txid == txid)
      return g_txmarks[i].committed;
  return -1;
}

static bool record_applies(uint32_t txid) {
  if (txid == 0u)
    return true; /* autocommit */
  return txmark_get(txid) == 1;
}

/* Orden cronológico de las zonas para la reconstrucción al montar. El índice de
 * zona NO sirve como orden temporal: la GC libera zonas de índice bajo y el
 * asignador las reutiliza para escrituras nuevas, de modo que una versión
 * reciente puede vivir en una zona de índice menor que otra antigua. Aplicar en
 * orden de índice haría que la versión vieja sobrescribiera a la nueva (estado
 * obsoleto tras remontar). `z->seq` (persistido en la cabecera de zona) es el
 * valor del contador global cuando la zona se abrió, así que ordenar por él
 * restituye el orden real de escritura. */
static uint32_t recovery_zone_order(const mf_t *fs, uint32_t *out) {
  uint32_t n = 0u;
  for (uint32_t i = 0; i < fs->zone_cap; i++) {
    const mfs_zone_t *z = &fs->zones[i];
    if (z->state == MFS_Z_EMPTY || z->state == MFS_Z_QUARANTINE)
      continue;
    out[n++] = i;
  }
  for (uint32_t a = 1u; a < n;
       a++) { /* inserción por seq (n ≤ MFS_MAX_ZONES) */
    uint32_t key = out[a];
    uint32_t kseq = fs->zones[key].seq;
    uint32_t b = a;
    while (b > 0u && fs->zones[out[b - 1u]].seq > kseq) {
      out[b] = out[b - 1u];
      b--;
    }
    out[b] = key;
  }
  return n;
}

mfs_st mfs_recover_metadata(mf_t *fs) {
  uint32_t pb = mfs_page_bytes(fs);
  uint16_t hs = mfs_e2g_hdr(fs);
  static uint8_t payload[MFS_SCRATCH_MAX];
  static uint32_t order[MFS_MAX_ZONES];
  uint32_t nz = recovery_zone_order(fs, order);
  uint16_t max_ino = 1u;
  txmark_reset();

  /* pasada 0: recoger marcas TXMARK (commit/abort) para el replay gating */
  for (uint32_t oi = 0; oi < nz; oi++) {
    uint32_t zi = order[oi];
    mfs_zone_t *z = &fs->zones[zi];
    for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
      uint8_t raw[MFS_E2G_NAND_HDR];
      if (mfs_read(fs, z->start_addr + off, raw, hs) != MFS_OK)
        continue;
      if (raw[0] != MFS_REC_MAGIC_HI || raw[1] != MFS_REC_MAGIC_LO)
        continue;
      if ((uint8_t)(raw[3] >> 4) != MFS_RT_WALENT)
        continue;
      uint32_t ppage = (zi << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
      uint8_t k, g, s, dict;
      uint16_t rlen = 0;
      if (mfs_rec_read(fs, ppage, 0xFFFFFFFFu, 0xFFu, &k, &g, &s, &dict,
                       payload, (uint16_t)sizeof(payload), &rlen) != MFS_OK)
        continue;
      uint8_t ik;
      const uint8_t *data;
      uint16_t dl;
      uint32_t txid = 0;
      if (!unwrap(payload, rlen, MFS_RT_WALENT, &ik, &data, &dl, &txid))
        continue;
      if (ik == MFS_RT_TXMARK && dl >= 1u)
        txmark_set(txid, data[0]);
    }
  }

  /* pasada 1: inodos + snapshots + reconstrucción de L2P (el propio registro
   * DATA porta su LBA, así que el mapa de páginas se recupera sin estructuras
   * auxiliares). Orden cronológico ⇒ la última versión de cada LBA prevalece.
   * Los registros de transacciones abortadas/no confirmadas se descartan
   * (MFS-TX-003). */
  for (uint32_t oi = 0; oi < nz; oi++) {
    uint32_t zi = order[oi];
    mfs_zone_t *z = &fs->zones[zi];
    for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
      uint8_t raw[MFS_E2G_NAND_HDR];
      if (mfs_read(fs, z->start_addr + off, raw, hs) != MFS_OK)
        continue;
      if (raw[0] != MFS_REC_MAGIC_HI || raw[1] != MFS_REC_MAGIC_LO)
        continue;
      uint8_t outer = (uint8_t)(raw[3] >> 4);
      uint32_t lba = mfs_ld32(raw + 5);
      uint32_t ppage = (zi << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
      uint8_t k, g, s, dict;
      uint16_t rlen = 0;
      if (mfs_rec_read(fs, ppage, 0xFFFFFFFFu, 0xFFu, &k, &g, &s, &dict,
                       payload, (uint16_t)sizeof(payload), &rlen) != MFS_OK)
        continue;
      uint8_t ik = outer;
      const uint8_t *data = payload;
      uint16_t dl = rlen;
      uint32_t txid = 0;
      if (!unwrap(payload, rlen, outer, &ik, &data, &dl, &txid))
        continue;
      if (ik == MFS_RT_TXMARK)
        continue;
      if (!record_applies(txid))
        continue;
      (void)mfs_l2p_put(lba, ppage);
      if (ik == MFS_RT_INODE)
        (void)mfs_inode_apply(fs, data, dl, &max_ino);
      else if (ik == MFS_RT_EPOCH)
        mfs_epoch_apply(fs, data, dl);
    }
  }
  /* pasada 2: nombres/padres (DIRENT) — mismo orden cronológico */
  for (uint32_t oi = 0; oi < nz; oi++) {
    uint32_t zi = order[oi];
    mfs_zone_t *z = &fs->zones[zi];
    for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
      uint8_t raw[MFS_E2G_NAND_HDR];
      if (mfs_read(fs, z->start_addr + off, raw, hs) != MFS_OK)
        continue;
      if (raw[0] != MFS_REC_MAGIC_HI || raw[1] != MFS_REC_MAGIC_LO)
        continue;
      uint32_t ppage = (zi << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
      uint8_t k, g, s, dict;
      uint16_t rlen = 0;
      if (mfs_rec_read(fs, ppage, 0xFFFFFFFFu, 0xFFu, &k, &g, &s, &dict,
                       payload, (uint16_t)sizeof(payload), &rlen) != MFS_OK)
        continue;
      uint8_t ik = (uint8_t)(raw[3] >> 4);
      const uint8_t *data = payload;
      uint16_t dl = rlen;
      uint32_t txid = 0;
      if (!unwrap(payload, rlen, ik, &ik, &data, &dl, &txid))
        continue;
      if (ik != MFS_RT_DIRENT || !record_applies(txid))
        continue;
      mfs_dirent_apply(fs, data, dl);
    }
  }
  if ((uint32_t)max_ino + 1u > g_ino_next)
    g_ino_next = (uint32_t)max_ino + 1u;
  return MFS_OK;
}

/* =====================================================================
 * Ciclo de vida (§21.1, FSM montaje §24.2)
 * ===================================================================== */
int mf_format(mf_t *fs, const void *opts) {
  (void)opts;
  if (fs == NULL || fs->cfg == NULL)
    return MFS_EINVAL;
  const mfs_config *cfg = fs->cfg;
  /* MFS-ARCH-010 rev.2: 0=8-bit, 1=16-bit, 2=32-bit son válidos; >2 se rechaza
   */
  if (cfg->arch_class > 2u)
    return MFS_EARCH;

  memset(fs, 0, sizeof(*fs));
  fs->cfg = cfg;
  g_mfs_instance = fs;

  mfs_st st = mfs_hal_detect(fs, cfg, &fs->hwv);
  if (st != MFS_OK)
    return st;
  if (fs->hwv.arch_class > 2u) {
    mfs_hct_event(fs, MFS_EV_ARCH_REJECT, 0);
    return MFS_EARCH;
  }
  mfs_mode_t mode = mfs_select_mode(&fs->hwv, cfg);
  if (mode == MFS_MODE_UNSUPPORTED) {
    mfs_hct_event(fs, MFS_EV_NOTVIABLE, fs->hwv.ram_total);
    return MFS_ENOTVIABLE;
  }
  fs->mode = (uint8_t)mode;
  fs->suite = mfs_negotiate_suite(&fs->hwv, cfg, mode);
  g_last_hwv = fs->hwv;
  g_last_selected = mode;
  fs->hwv.base_reserved_off = mfs_zone_base(&fs->hwv);
  fs->epoch = 1u;

  /* formatear: borrar región baja y todas las zonas (NOR: erase previo) */
  zones_init_table(fs);
  st = mfs_erase(fs, 0u);
  if (st != MFS_OK)
    return st;
  st = mfs_erase(fs, fs->hwv.erase_unit);
  if (st != MFS_OK)
    return st;
  st = mfs_erase(fs, mfs_tok_off(&fs->hwv));
  if (st != MFS_OK)
    return st;
  for (uint32_t i = 0; i < fs->zone_cap; i++) {
    st = mfs_zone_erase(fs, &fs->zones[i]);
    if (st != MFS_OK)
      return st;
  }

  /* HWV + SB A/B */
  if (fs->label[0] == '\0')
    memcpy(fs->label, "MatrixFS", 9u);
  uint8_t hwvser[64];
  mfs_hwv_serialize(&fs->hwv, hwvser);
  st = mfs_write(fs, MFS_HWV_OFFSET, hwvser, 64u);
  if (st != MFS_OK)
    return st;
  st = sb_write(fs, 0u, fs->epoch, 0u);
  if (st != MFS_OK)
    return st;
  st = sb_write(fs, 1u, fs->epoch, 0u);
  if (st != MFS_OK)
    return st;

  mfs_l2p_reset();
  fs->zone_wal = 0xFFFFFFFFu;
  mfs_srb_attach(fs);
  mfs_cfx_reset();
  g_lba_next = 0;
  g_ino_next = 2u;

  /* raíz: inodo 0 directorio */
  mfs_inode_ram_t *root = &fs->inos[0];
  memset(root, 0, sizeof(*root));
  root->valid = 1u;
  root->ino = ROOT_INO;
  root->type = 1u;
  root->nlink = 2u;
  root->parent = ROOT_INO;
  strcpy(root->name, "/");
  root->uid = fs->cfg ? fs->cfg->default_uid : 0u;
  root->gid = fs->cfg ? fs->cfg->default_gid : 0u;
  root->perm = (uint16_t)((fs->cfg && fs->cfg->default_dir_perm)
                              ? fs->cfg->default_dir_perm
                              : MFS_DEFAULT_DIR_MODE);
  fs->free_pages = mfs_zone_count_free(fs);
  fs->slec_zone = 0xFFFFu;
  fs->tg_temp_c = 25;
  fs->tg_state = MFS_TG_NOMINAL;
  fs->tg_disturb_threshold = 1000u;
  fs->hmt_active = 0u;
  mfs_xdam_reset(fs);
  mfs_wep_init(fs, (const uint8_t *)"MFSUNSET");
  fs->wep_k1 = 0u;
  fs->wep_k2 = 0u; /* WEP deshabilitado por defecto */
  fs->mounted = true;
  mfs_dab_init(fs);
  mfs_hct_event(fs, MFS_EV_BOOT, fs->mode);
  return MFS_OK;
}

int mf_init(mf_t *fs, const mfs_config *cfg) {
  if (fs == NULL || cfg == NULL || cfg->drv == NULL)
    return MFS_EINVAL;
  /* MFS-ARCH-010 rev.2: se aceptan clases 0 (8-bit), 1 (16-bit) y 2 (32-bit);
   * sólo se rechaza una clase desconocida (> 2). */
  if (cfg->arch_class > 2u)
    return MFS_EARCH;
  memset(fs, 0, sizeof(*fs));
  fs->cfg = cfg;
  g_mfs_instance = fs;

  /* paso 1: detección (geometría autoritativa + capacidades) */
  mfs_st st = mfs_hal_detect(fs, cfg, &fs->hwv);
  if (st != MFS_OK)
    return st;
  fs->hwv.arch_class = cfg->arch_class;
  fs->hwv.mode_forced = (cfg->forced_mode == MFS_MODE_UNSUPPORTED)
                            ? 0xFFu
                            : (uint8_t)cfg->forced_mode;
  if (cfg->ram_total)
    fs->hwv.ram_total = cfg->ram_total;

  /* HWV persistido (MFS-HWV-001): capacidades autoritativas si es válido */
  uint8_t ser[64];
  if (mfs_read(fs, MFS_HWV_OFFSET, ser, 64u) == MFS_OK) {
    mfs_hwv_t ph;
    mfs_hwv_deserialize(&ph, ser);
    mfs_st vst = mfs_hwv_validate(&ph);
    if (vst == MFS_EARCH) {
      mfs_hct_event(fs, MFS_EV_ARCH_REJECT, 0);
      return MFS_EARCH;
    }
    if (vst == MFS_OK) {
      ph.media_type = fs->hwv.media_type;
      ph.media_size = fs->hwv.media_size;
      ph.erase_unit = fs->hwv.erase_unit;
      ph.program_granularity = fs->hwv.program_granularity;
      ph.arch_class = cfg->arch_class;
      ph.mode_forced = fs->hwv.mode_forced;
      if (cfg->ram_total)
        ph.ram_total = cfg->ram_total;
      fs->hwv = ph;
    }
  }
  g_last_hwv = fs->hwv;

  /* paso 3: modo + viabilidad (MFS-VIAB-002) */
  mfs_mode_t mode = mfs_select_mode(&fs->hwv, cfg);
  if (mode == MFS_MODE_UNSUPPORTED) {
    mfs_hct_event(fs, MFS_EV_NOTVIABLE, fs->hwv.ram_total);
    return MFS_ENOTVIABLE;
  }
  fs->mode = (uint8_t)mode;
  g_last_selected = mode;
  fs->suite = mfs_negotiate_suite(&fs->hwv, cfg, mode);
  fs->hwv.base_reserved_off = mfs_zone_base(&fs->hwv);
  mfs_hct_event(fs, MFS_EV_SUITE_NEG, fs->suite);

  /* paso 4: superblock A/B — mayor {época, seq} con integridad válida */
  uint32_t epA = 0, gA = 0, sqA = 0, epB = 0, gB = 0, sqB = 0;
  mfs_st sA = sb_read(fs, mfs_sb_a_off(), &epA, &gA, &sqA);
  mfs_st sB = sb_read(fs, mfs_sb_b_off(&fs->hwv), &epB, &gB, &sqB);
  if (sA != MFS_OK && sB != MFS_OK)
    return MFS_ECORRUPT; /* requiere mf_format */
  if (sA == MFS_OK && sB == MFS_OK) {
    if (epB > epA || (epB == epA && sqB >= sqA)) {
      fs->epoch = epB;
    } else
      fs->epoch = epA;
  } else
    fs->epoch = (sA == MFS_OK) ? epA : epB;
  if (fs->epoch == 0u)
    fs->epoch = 1u;
  /* Contador global de operaciones: debe continuar por encima del último valor
   * persistido. La cabecera de zona guarda el valor que tenía al abrirse
   * (`z->seq`), y de ahí depende el orden cronológico de la reconstrucción al
   * montar: reiniciarlo a 0 haría que las zonas nuevas parecieran anteriores a
   * las ya existentes. */
  fs->seq = (sqA > sqB) ? sqA : sqB;

  /* etiqueta de volumen (§22.1): la del slot ganador {época, seq} —el mismo
   * que fija fs->epoch— y, si éste no la trae, la del otro slot. Leer siempre
   * el slot A devolvería etiquetas obsoletas, ya que la rotación del
   * superblock reescribe primero el slot de época menor. */
  fs->label[0] = '\0';
  {
    uint32_t pref = 0u;
    if (sA == MFS_OK && sB == MFS_OK)
      pref = (epB > epA || (epB == epA && sqB >= sqA)) ? 1u : 0u;
    else if (sB == MFS_OK)
      pref = 1u;
    const uint32_t offs[2] = {mfs_sb_a_off(), mfs_sb_b_off(&fs->hwv)};
    for (uint32_t k = 0; k < 2u && fs->label[0] == '\0'; k++) {
      uint32_t i = (k == 0u) ? pref : (1u - pref);
      uint8_t sbl[MFS_SB_SIZE];
      if (mfs_read(fs, offs[i], sbl, MFS_SB_SIZE) != MFS_OK)
        continue;
      if (sbl[123] == 0xFFu || sbl[123] == 0u)
        continue;
      memcpy(fs->label, sbl + 123, MFS_LABEL_MAX);
      fs->label[MFS_LABEL_MAX - 1u] = '\0';
    }
  }
  if (fs->label[0] == '\0')
    memcpy(fs->label, "MatrixFS", 9u);

  /* paso 5: reconstrucción de zonas + metadatos + l2p */
  zones_init_table(fs);
  st = mfs_scan_zones(fs);
  if (st != MFS_OK)
    return st;
  mfs_l2p_reset();
  fs->zone_wal = 0xFFFFFFFFu;
  (void)mfs_hmt_init(fs); /* HMT (§11.11): tier T0 si procede */
  st = mfs_recover_metadata(fs);
  if (st != MFS_OK)
    return st;
  if (fs->hmt_active)
    (void)mfs_hmt_scan(fs); /* T0 (más reciente) prevalece */

  if (mfs_zone_count_free(fs) == 0u) {
    /* volumen lleno de zonas FULL: al menos permitir GC reclamando */
    fs->free_pages = 0u;
  }

  /* paso 6: checkpoint + replay acotado + detección de corte */
  st = mfs_checkpoint_load(fs);
  if (st != MFS_OK)
    return st;
  st = mfs_wal_replay(fs, MFS_WAL_WINDOW_MAX);
  if (st != MFS_OK)
    return st;

  /* paso 7: raíz */
  mfs_inode_ram_t *root = &fs->inos[0];
  if (!root->valid) {
    memset(root, 0, sizeof(*root));
    root->valid = 1u;
    root->ino = ROOT_INO;
    root->type = 1u;
    root->nlink = 2u;
    root->parent = ROOT_INO;
    strcpy(root->name, "/");
  }
  /* La raíz no se persiste como registro INODE (la reconstrucción la ignora),
   * así que su propietario y permisos se derivan de la configuración del
   * montaje (§21.2): sin esto el directorio raíz aparecería como 0:0. */
  if (root->perm == 0u)
    root->perm = (uint16_t)((fs->cfg && fs->cfg->default_dir_perm)
                                ? fs->cfg->default_dir_perm
                                : MFS_DEFAULT_DIR_MODE);
  if (root->uid == 0u && root->gid == 0u) {
    root->uid = fs->cfg ? fs->cfg->default_uid : 0u;
    root->gid = fs->cfg ? fs->cfg->default_gid : 0u;
  }
  mfs_srb_attach(fs);
  mfs_cfx_reset();
  fs->slec_zone = 0xFFFFu;
  fs->tg_temp_c = 25;
  fs->tg_state = MFS_TG_NOMINAL;
  fs->tg_disturb_threshold = 1000u;
  mfs_xdam_reset(fs);
  mfs_wep_init(fs, (const uint8_t *)"MFSUNSET");
  fs->wep_k1 = 0u;
  fs->wep_k2 = 0u; /* WEP deshabilitado salvo manifiesto */
  fs->mounted = true;
  fs->free_pages = mfs_zone_count_free(fs);
  fs->edp_state = MFS_EDP_MONITOR;
  mfs_dab_init(fs);
  mfs_hct_event(fs, MFS_EV_MOUNT_OK, fs->epoch);
  fs->hct.mount_time_us = mfs_port_time_us();
  return MFS_OK;
}

int mf_sync(mf_t *fs) {
  if (!fs || !fs->mounted)
    return MFS_ENOTMOUNTED;
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
    if (fs->inos[i].valid)
      mfs_meta_flush(fs, fs->inos[i].ino);
  mfs_st st = mfs_checkpoint_write(fs);
  if (st != MFS_OK)
    return st;
  /* superblock rotatorio: reescribir el sector con época menor (§22.1) */
  uint32_t epA = 0, epB = 0;
  mfs_st rA = sb_read(fs, mfs_sb_a_off(), &epA, NULL, NULL);
  mfs_st rB = sb_read(fs, mfs_sb_b_off(&fs->hwv), &epB, NULL, NULL);
  uint32_t slot;
  if (rA != MFS_OK && rB != MFS_OK)
    slot = 0u;
  else if (rB != MFS_OK)
    slot = 1u;
  else if (rA == MFS_OK && epA < epB)
    slot = 0u;
  else
    slot = 1u;
  st = sb_write(fs, slot, fs->epoch, (uint32_t)fs->seq);
  mfs_hct_flush(fs);
  mfs_gld_maybe_gc(fs);
  /* mantenimiento de fondo en ventana idle (§11.7, §11.9), sujeto a ELD */
  (void)mfs_tg_step(fs, 200u);
  (void)mfs_slec_fold(fs, 200u);
  return st;
}

int mf_deinit(mf_t *fs) {
  if (!fs)
    return MFS_EINVAL;
  if (fs->mounted) {
    mf_sync(fs);
    fs->mounted = false;
  }
  volatile uint8_t *p = (volatile uint8_t *)fs;
  for (uint32_t i = 0; i < sizeof(*fs); i++)
    p[i] = 0u; /* MFS-SEC-002 */
  mfs_cfx_reset();
  return MFS_OK;
}

/* =====================================================================
 * POSIX-subset
 * ===================================================================== */
int mf_open(mf_t *fs, const char *path, uint32_t flags, mfs_file **fout) {
  if (!fs || !fs->mounted || path == NULL || fout == NULL)
    return MFS_ENOTMOUNTED;
  int r = mfs_lookup(fs, path, NULL);
  mfs_inode_ram_t *ino = NULL;
  if (r >= 0) {
    if ((flags & MFS_O_CREAT) && (flags & MFS_O_EXCL))
      return MFS_EEXISTS;
    ino = mfs_ino_get(fs, (uint32_t)r);
  } else if (r == MFS_ENOENT) {
    if (!(flags & MFS_O_CREAT))
      return MFS_ENOENT;
    ino = mfs_ino_alloc(fs);
    if (!ino)
      return MFS_ETABLEFULL;
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
      char parent_path[MFS_PATH_MAX];
      uint32_t n = (uint32_t)(slash - path);
      if (n >= MFS_PATH_MAX)
        n = MFS_PATH_MAX - 1u;
      memcpy(parent_path, path, n);
      parent_path[n] = '\0';
      int pr = mfs_lookup(fs, parent_path, NULL);
      if (pr < 0 && pr != MFS_ENOENT)
        return pr;
      ino->parent = (pr == MFS_ENOENT) ? ROOT_INO : (uint32_t)pr;
      mfs_name_set(ino->name, slash + 1, MFS_NAME_MAX);
    } else {
      ino->parent = ROOT_INO;
      mfs_name_set(ino->name, slash ? slash + 1 : path, MFS_NAME_MAX);
    }
    ino->name[MFS_NAME_MAX - 1u] = '\0';
    ino->type = 0u;
    ino->nlink = 1u;
    ino->hotness = (uint8_t)((flags >> MFS_O_HOTNESS_SHIFT) & 3u);
    mfs_ino_defaults(fs, ino);
    mfs_meta_flush(fs, ino->ino);
  } else
    return r;

  if (ino == NULL)
    return MFS_ENOENT;
  if (ino->type == 1u)
    return MFS_EINVAL; /* dir: usar opendir */

  mfs_file *f = NULL;
  for (uint32_t i = 0; i < MFS_MAX_FILES_OPEN; i++) {
    if (fs->open_files[i] == NULL) {
      static mfs_file pool[MFS_MAX_FILES_OPEN];
      f = &pool[i];
      fs->open_files[i] = f;
      break;
    }
  }
  if (!f)
    return MFS_ETABLEFULL;
  memset(f, 0, sizeof(*f));
  f->ino = ino->ino;
  f->fs = fs;
  f->ref = 1u;
  f->flags = (uint16_t)flags;
  if (flags & MFS_O_APPEND)
    f->pos = ino->size;
  if (flags & MFS_O_TRUNC) {
    ino->size = 0u;
    mfs_meta_flush(fs, ino->ino);
  }
  *fout = f;
  return MFS_OK;
}

int mf_close(mfs_file *f) {
  if (!f || !f->fs)
    return MFS_EINVAL;
  mf_t *fs = f->fs;
  mfs_meta_flush(fs, f->ino);
  for (uint32_t i = 0; i < MFS_MAX_FILES_OPEN; i++)
    if (fs->open_files[i] == f)
      fs->open_files[i] = NULL;
  f->ref = 0u;
  return MFS_OK;
}

int mf_read(mfs_file *f, void *buf, size_t len, size_t *rd) {
  if (!f || !buf)
    return MFS_EINVAL;
  mf_t *fs = f->fs;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, f->ino);
  if (!ino)
    return MFS_ENOENT;
  uint8_t *dst = (uint8_t *)buf;
  size_t done = 0;
  uint32_t pb = mfs_payload_bytes(fs);
  if (pb == 0u)
    return MFS_EINVAL;
  while (done < len && f->pos < ino->size) {
    uint32_t vp = (uint32_t)(f->pos / pb);
    uint32_t off = (uint32_t)(f->pos % pb);
    uint8_t page[MFS_SCRATCH_MAX];
    uint16_t rlen = 0;
    uint32_t got =
        mfs_extent_read(fs, ino, vp, page, (uint16_t)sizeof(page), &rlen);
    size_t take = len - done;
    if (take > pb - off)
      take = pb - off;
    size_t avail = (size_t)(ino->size - f->pos);
    if (take > avail)
      take = avail; /* no leer más allá de EOF */
    if (got == MFS_PPAGE_ERROR) {
      /* Página viva ilegible (integridad E2G): se propaga el error en vez de
       * entregar ceros, que ocultarían la pérdida de datos al llamante. */
      if (rd)
        *rd = done;
      return MFS_ECORRUPT;
    }
    if (got == MFS_PPAGE_NONE || off >= rlen) {
      memset(dst + done, 0, take); /* hueco → ceros */
    } else {
      if (take > rlen - off)
        take = rlen - off;
      memcpy(dst + done, page + off, take);
    }
    done += take;
    f->pos += take;
    if (take == 0u)
      break;
  }
  if (rd)
    *rd = done;
  return MFS_OK;
}

int mf_write(mfs_file *f, const void *buf, size_t len, size_t *wr) {
  if (!f || !buf)
    return MFS_EINVAL;
  mf_t *fs = f->fs;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, f->ino);
  if (!ino)
    return MFS_ENOENT;
  const uint8_t *src = (const uint8_t *)buf;
  size_t done = 0;
  uint32_t pb = mfs_payload_bytes(fs);
  if (pb == 0u)
    return MFS_EINVAL;
  uint64_t maxsz = (uint64_t)mfs_limits[fs->mode].max_file_size_kb * 1024ull;
  if (f->pos + len > maxsz)
    return MFS_EOVERFLOW;
  if (fs->debt_gld > MFS_GL_DLIST_MAX * 4u) {
    mfs_gld_maybe_gc(fs);
    if (fs->debt_gld > MFS_GL_DLIST_MAX * 8u)
      return MFS_EBACKPRESSURE;
  }
  mfs_edp_check(fs);
  if (fs->edp_level >= 4u)
    return MFS_EAGAIN; /* SPDR activo */
  while (done < len) {
    uint32_t vp = (uint32_t)((f->pos + done) / pb);
    uint32_t off = (uint32_t)((f->pos + done) % pb);
    uint8_t page[MFS_SCRATCH_MAX];
    uint16_t plen = 0;
    if (off != 0u) {
      uint16_t rlen = 0;
      uint32_t got =
          mfs_extent_read(fs, ino, vp, page, (uint16_t)sizeof(page), &rlen);
      if (got == MFS_PPAGE_ERROR)
        return MFS_ECORRUPT; /* no reescribir sobre una página ilegible */
      plen = rlen;
      if (plen < off) {
        memset(page + plen, 0, off - plen);
        plen = (uint16_t)off;
      }
    }
    size_t take = len - done;
    if (take > pb - off)
      take = pb - off;
    memcpy(page + off, src + done, take);
    if ((uint32_t)(off + take) > plen)
      plen = (uint16_t)(off + take);
    mfs_st st = mfs_extent_write(fs, ino, vp, page, plen);
    if (st != MFS_OK)
      return st;
    done += take;
    uint64_t np = f->pos + done;
    if (np > ino->size)
      ino->size = (uint32_t)np;
  }
  f->pos += done;
  ino->mtime = (uint32_t)(fs->epoch * 1000u + (uint32_t)f->pos);
  if (wr)
    *wr = done;
  return MFS_OK;
}

int mf_seek(mfs_file *f, int64_t off, int whence) {
  if (!f)
    return MFS_EINVAL;
  mfs_inode_ram_t *ino = mfs_ino_get(f->fs, f->ino);
  if (!ino)
    return MFS_ENOENT;
  int64_t np = (whence == MFS_SEEK_SET)   ? off
               : (whence == MFS_SEEK_CUR) ? (int64_t)f->pos + off
                                          : (int64_t)ino->size + off;
  if (np < 0)
    return MFS_EINVAL;
  f->pos = (uint64_t)np;
  return MFS_OK;
}

int mf_tell(mfs_file *f, uint64_t *pos) {
  if (!f || !pos)
    return MFS_EINVAL;
  *pos = f->pos;
  return MFS_OK;
}

int mf_stat(mf_t *fs, const char *path, mfs_stat *st) {
  if (!fs || !path || !st)
    return MFS_EINVAL;
  int r = mfs_lookup(fs, path, NULL);
  if (r < 0)
    return r;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
  if (!ino)
    return MFS_ENOENT;
  st->ino = ino->ino;
  st->mode = (uint16_t)(((ino->type == 1u) ? MFS_S_IFDIR : MFS_S_IFREG) |
                        mfs_ino_perm(fs, ino));
  st->nlink = ino->nlink;
  st->size = ino->size;
  st->mtime = ino->mtime;
  st->gen = ino->gen;
  st->snapid = ino->snapid;
  st->hotness = ino->hotness;
  st->pad = 0u;
  st->uid = ino->uid;
  st->gid = ino->gid;
  return MFS_OK;
}

/* Permisos efectivos de un inodo: si el registro no los porta (versión previa
 * del layout) se aplican los defaults de la configuración. */
static uint16_t mfs_ino_perm(const mf_t *fs, const mfs_inode_ram_t *ino) {
  if (ino->perm != 0u)
    return (uint16_t)(ino->perm & 07777u);
  if (ino->type == 1u)
    return (uint16_t)((fs && fs->cfg && fs->cfg->default_dir_perm)
                          ? fs->cfg->default_dir_perm
                          : MFS_DEFAULT_DIR_MODE);
  return (uint16_t)((fs && fs->cfg && fs->cfg->default_file_perm)
                        ? fs->cfg->default_file_perm
                        : MFS_DEFAULT_FILE_MODE);
}

/* Rellena propietario/permisos por defecto en un nodo recién creado. */
static void mfs_ino_defaults(mf_t *fs, mfs_inode_ram_t *ino) {
  if (!fs || !ino)
    return;
  if (ino->uid == 0u && ino->gid == 0u) {
    ino->uid = fs->cfg ? fs->cfg->default_uid : 0u;
    ino->gid = fs->cfg ? fs->cfg->default_gid : 0u;
  }
  if (ino->perm == 0u) {
    ino->perm =
        (uint16_t)((ino->type == 1u) ? (fs->cfg && fs->cfg->default_dir_perm
                                            ? fs->cfg->default_dir_perm
                                            : MFS_DEFAULT_DIR_MODE)
                                     : (fs->cfg && fs->cfg->default_file_perm
                                            ? fs->cfg->default_file_perm
                                            : MFS_DEFAULT_FILE_MODE));
  }
}

int mf_setattr(mf_t *fs, const char *path, uint32_t mask, const mfs_attr *a) {
  if (!fs || !path || !a)
    return MFS_EINVAL;
  if (fs->edp_level >= 4u)
    return MFS_EAGAIN; /* SPDR activo (§13.1) */
  int r = mfs_lookup(fs, path, NULL);
  if (r < 0)
    return r;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
  if (!ino)
    return MFS_ENOENT;
  if (mask & MFS_ATTR_MODE)
    ino->perm = (uint16_t)(a->mode & 07777u);
  if (mask & MFS_ATTR_UID)
    ino->uid = a->uid;
  if (mask & MFS_ATTR_GID)
    ino->gid = a->gid;
  if (mask & MFS_ATTR_MTIME)
    ino->mtime = a->mtime;
  /* El tamaño se gestiona con mf_truncate (requiere handle abierto): aquí sólo
   * se valida el rango para dar un error tipificado si se solicita. */
  if (mask & MFS_ATTR_SIZE) {
    uint64_t maxsz = (uint64_t)mfs_limits[fs->mode].max_file_size_kb * 1024ull;
    if (a->size > maxsz)
      return MFS_EOVERFLOW;
  }
  if (mask & (MFS_ATTR_MODE | MFS_ATTR_UID | MFS_ATTR_GID | MFS_ATTR_MTIME))
    return mfs_meta_flush(fs, ino->ino);
  return MFS_OK;
}

int mf_get_label(mf_t *fs, char *out, size_t len) {
  if (!fs || !out || len == 0u)
    return MFS_EINVAL;
  size_t n = strlen(fs->label);
  if (n > len - 1u)
    n = len - 1u;
  memcpy(out, fs->label, n);
  out[n] = '\0';
  return MFS_OK;
}

int mf_set_label(mf_t *fs, const char *label) {
  if (!fs || !label)
    return MFS_EINVAL;
  if (!fs->mounted)
    return MFS_ENOTMOUNTED;
  size_t n = strlen(label);
  if (n > MFS_LABEL_MAX - 1u)
    return MFS_EINVAL;
  memset(fs->label, 0, sizeof(fs->label));
  memcpy(fs->label, label, n);
  return mf_sync(fs); /* persiste el superblock vigente (§22.1) */
}

int mf_unlink(mf_t *fs, const char *path) {
  if (!fs || !path)
    return MFS_EINVAL;
  int r = mfs_lookup(fs, path, NULL);
  if (r < 0)
    return r;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
  if (!ino)
    return MFS_ENOENT;
  if (ino->type == 1u) {
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
      if (fs->inos[i].valid && fs->inos[i].parent == ino->ino)
        return MFS_EBUSY;
  }
  uint32_t npages = mfs_vpages_of(fs, ino->size);
  for (uint32_t e = 0; e < MFS_EXT_INLINE; e++)
    if (ino->ext[e].npages)
      mfs_gld_add(fs, 1u, false);
  if (ino->ino != ROOT_INO) {
    /* lápida durable: INODE con nlink=0 + DIRENT vacío (§9 crash-only) */
    ino->nlink = 0u;
    ino->size = 0u;
    ino->name[0] = '\0';
    (void)mfs_meta_flush(fs, ino->ino);
    /* liberar el mapeo de sus páginas: dejan de ser vivas para la GC */
    mfs_extent_drop_range(fs, ino->ino, 0u, npages);
    memset(ino, 0, sizeof(*ino));
  }
  return MFS_OK;
}

int mf_rename(mf_t *fs, const char *from, const char *to) {
  if (!fs || !from || !to)
    return MFS_EINVAL;
  int r = mfs_lookup(fs, from, NULL);
  if (r < 0)
    return r;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
  if (!ino)
    return MFS_ENOENT;
  const char *slash = strrchr(to, '/');
  uint32_t newpar = ROOT_INO;
  char nm[MFS_NAME_MAX];
  if (slash && slash != to) {
    char pp[MFS_PATH_MAX];
    uint32_t n = (uint32_t)(slash - to);
    if (n >= MFS_PATH_MAX)
      n = MFS_PATH_MAX - 1u;
    memcpy(pp, to, n);
    pp[n] = '\0';
    int pr = mfs_lookup(fs, pp, NULL);
    if (pr < 0)
      return pr;
    newpar = (uint32_t)pr;
    mfs_name_set(nm, slash + 1, MFS_NAME_MAX);
  } else {
    mfs_name_set(nm, slash ? slash + 1 : to, MFS_NAME_MAX);
  }
  bool own_tx = !fs->tx_open;
  if (own_tx)
    mf_tx_begin(fs);
  ino->parent = newpar;
  mfs_name_set(ino->name, nm, MFS_NAME_MAX);
  mfs_meta_flush(fs, ino->ino);
  if (own_tx)
    return mf_tx_commit(fs);
  return MFS_OK;
}

int mf_mkdir(mf_t *fs, const char *path) {
  if (!fs || !path)
    return MFS_EINVAL;
  int r = mfs_lookup(fs, path, NULL);
  if (r >= 0)
    return MFS_EEXISTS;
  mfs_inode_ram_t *ino = mfs_ino_alloc(fs);
  if (!ino)
    return MFS_ETABLEFULL;
  const char *slash = strrchr(path, '/');
  if (slash && slash != path) {
    char pp[MFS_PATH_MAX];
    uint32_t n = (uint32_t)(slash - path);
    if (n >= MFS_PATH_MAX)
      n = MFS_PATH_MAX - 1u;
    memcpy(pp, path, n);
    pp[n] = '\0';
    int pr = mfs_lookup(fs, pp, NULL);
    if (pr < 0 && pr != MFS_ENOENT)
      return pr;
    ino->parent = (pr == MFS_ENOENT) ? ROOT_INO : (uint32_t)pr;
    mfs_name_set(ino->name, slash + 1, MFS_NAME_MAX);
  } else {
    ino->parent = ROOT_INO;
    mfs_name_set(ino->name, slash ? slash + 1 : path, MFS_NAME_MAX);
  }
  ino->name[MFS_NAME_MAX - 1u] = '\0';
  ino->type = 1u;
  ino->nlink = 2u;
  mfs_ino_defaults(fs, ino);
  return mfs_meta_flush(fs, ino->ino);
}

int mf_truncate(mfs_file *f, uint64_t size) {
  if (!f)
    return MFS_EINVAL;
  mfs_inode_ram_t *ino = mfs_ino_get(f->fs, f->ino);
  if (!ino)
    return MFS_ENOENT;
  uint32_t pb = mfs_payload_bytes(f->fs);
  if (pb == 0u)
    return MFS_EINVAL;
  if (size < ino->size) {
    uint32_t keep = (uint32_t)((size + pb - 1u) / pb);
    for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
      if (ino->ext[e].npages && ino->ext[e].vpage >= keep) {
        mfs_gld_add(f->fs, 1u, false);
        ino->ext[e].npages = 0;
      }
    }
    /* el resto de páginas se resuelven por L2P: se retiran del mapa */
    mfs_extent_drop_range(f->fs, ino->ino, keep,
                          mfs_vpages_of(f->fs, ino->size));
  }
  ino->size = (uint32_t)size;
  if (f->pos > size)
    f->pos = size;
  return mfs_meta_flush(f->fs, ino->ino);
}

int mf_opendir(mf_t *fs, const char *path, mfs_dir **dout) {
  if (!fs || !path || !dout)
    return MFS_EINVAL;
  int r = mfs_lookup(fs, path, NULL);
  if (r < 0)
    return r;
  mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
  if (!ino || ino->type != 1u)
    return MFS_EINVAL;
  static mfs_dir dpool;
  dpool.fs = fs;
  dpool.parent_ino = ino->ino;
  dpool.idx = 0u;
  dpool.used = 1u;
  *dout = &dpool;
  return MFS_OK;
}

int mf_readdir(mfs_dir *d, mfs_dirent *de) {
  if (!d || !de)
    return MFS_EINVAL;
  mf_t *fs = d->fs;
  uint32_t n = sizeof(fs->inos) / sizeof(fs->inos[0]);
  while (d->idx < n) {
    mfs_inode_ram_t *ino = &fs->inos[d->idx++];
    if (ino->valid && ino->ino != ROOT_INO && ino->parent == d->parent_ino) {
      mfs_name_set(de->name, ino->name, 64u);
      de->name[63] = '\0';
      de->st.ino = ino->ino;
      de->st.mode = (uint16_t)(((ino->type == 1u) ? MFS_S_IFDIR : MFS_S_IFREG) |
                               mfs_ino_perm(fs, ino));
      de->st.nlink = ino->nlink;
      de->st.size = ino->size;
      de->st.mtime = ino->mtime;
      de->st.gen = ino->gen;
      de->st.snapid = ino->snapid;
      de->st.hotness = ino->hotness;
      de->st.uid = ino->uid;
      de->st.gid = ino->gid;
      return MFS_OK;
    }
  }
  return MFS_ENOENT; /* fin de iteración */
}

int mf_closedir(mfs_dir *d) {
  if (!d)
    return MFS_EINVAL;
  d->used = 0u;
  return MFS_OK;
}

/* =====================================================================
 * VIO / DAIO (§13.1)
 * ===================================================================== */
int mf_readv(mfs_file *f, const mfs_iovec *v, int n) {
  int st = MFS_OK;
  for (int i = 0; i < n; i++) {
    size_t rd = 0;
    int r = mf_read(f, v[i].iov_base, v[i].iov_len, &rd);
    if (r != MFS_OK)
      st = r;
    if (rd != v[i].iov_len)
      break;
  }
  return st;
}

int mf_writev(mfs_file *f, const mfs_iovec *v, int n) {
  int st = MFS_OK;
  for (int i = 0; i < n; i++) {
    size_t wr = 0;
    int r = mf_write(f, v[i].iov_base, v[i].iov_len, &wr);
    if (r != MFS_OK)
      st = r;
    if (wr != v[i].iov_len)
      break;
  }
  return st;
}

/* mf_submit: ISR-safe (§21.3). Encola en ring SPSC; nunca bloquea ni recorre
 * estructuras compartidas más allá del índice. */
int mf_submit(mf_t *fs, mfs_iocb *cb) {
  if (!fs || !cb)
    return MFS_EINVAL;
  if (!fs->mounted)
    return MFS_ENOTMOUNTED;
  uint8_t next = (uint8_t)((fs->ring_head + 1u) % MFS_MAX_IOCB);
  if (next == fs->ring_tail)
    return MFS_EBACKPRESSURE; /* ring lleno */
  mfs_port_crit_enter();
  fs->ring[fs->ring_head] = cb;
  cb->status = 0;
  fs->ring_head = next;
  mfs_port_crit_exit();
  return MFS_OK;
}

int mf_poll(mf_t *fs, mfs_iocb **done, int max, uint32_t timeout_us) {
  if (!fs)
    return MFS_EINVAL;
  uint32_t t0 = mfs_port_time_us();
  int n = 0;
  while (fs->ring_tail != fs->ring_head && n < max) {
    if (mfs_port_time_us() - t0 > timeout_us)
      break;
    mfs_iocb *cb = fs->ring[fs->ring_tail];
    uint8_t cls = (uint8_t)(cb->class_flags & 3u);
    uint32_t tau = (cls == MFS_RT_A)   ? fs->tau[0]
                   : (cls == MFS_RT_B) ? fs->tau[1]
                                       : fs->tau[2];
    if (tau == 0u)
      tau = (cls == MFS_RT_A)   ? MFS_TAU_RT_A_US
            : (cls == MFS_RT_B) ? MFS_TAU_RT_B_US
                                : MFS_TAU_RT_C_US;
    mfs_port_crit_enter();
    fs->ring[fs->ring_tail] = NULL;
    fs->ring_tail = (uint8_t)((fs->ring_tail + 1u) % MFS_MAX_IOCB);
    mfs_port_crit_exit();
    mfs_file *f =
        *(mfs_file **)cb->buf; /* cb->buf apunta al handle de archivo */
    int rc = MFS_EINVAL;
    uint32_t op_t0 = mfs_port_time_us();
    if (f && cb->op == MFS_AREAD) {
      size_t rd = 0;
      if (mf_seek(f, (int64_t)cb->file_off, MFS_SEEK_SET) == MFS_OK)
        rc = mf_read(f, (char *)cb->buf + sizeof(void *), cb->len, &rd);
    } else if (f && cb->op == MFS_AWRITE) {
      size_t wr = 0;
      if (mf_seek(f, (int64_t)cb->file_off, MFS_SEEK_SET) == MFS_OK)
        rc = mf_write(f, (const char *)cb->buf + sizeof(void *), cb->len, &wr);
    }
    uint32_t lat = mfs_port_time_us() - op_t0;
    fs->hct.ops_count++;
    int16_t latx = (lat > 320u) ? 32000 : (int16_t)(lat * 100u);
    mfs_cusum_update(fs, latx);
    if (cls == MFS_RT_A && lat > tau) {
      cb->status = MFS_ETIMEDOUT_BUDGET; /* violación de contrato TCB */
      fs->hct.cusum_alarms++;
      mfs_hct_event(fs, MFS_EV_CUSUM_ALARM, lat);
    } else {
      cb->status = rc;
    }
    if (done)
      done[n] = cb;
    n++;
  }
  return n;
}

/* =====================================================================
 * Transacciones y savepoints (§9, §9.6)
 * ===================================================================== */
/* Snapshots de estado por transacción/savepoint (definidos más abajo, §9.6) */
static void sp_snapshot(mf_t *fs, uint8_t lvl);
static void sp_restore(mf_t *fs, uint8_t lvl);

int mf_tx_begin(mf_t *fs) {
  if (!fs || !fs->mounted)
    return MFS_ENOTMOUNTED;
  if (fs->tx_open)
    return MFS_EDEADLK;
  fs->tx_open = true;
  fs->sp_depth = 0u;
  fs->txid_cur = ++fs->txid_next;
  fs->sp_marks[0] = fs->wal_count;
  sp_snapshot(fs, 0u); /* estado al inicio de la transacción */
  return MFS_OK;
}

int mf_tx_commit(mf_t *fs) {
  if (!fs || !fs->mounted)
    return MFS_ENOTMOUNTED;
  if (!fs->tx_open)
    return MFS_ESTATE;
  uint32_t crc = 0u;
  for (uint32_t i = 0; i < fs->wal_count; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    if (fs->wal[j].valid && fs->wal[j].txid == fs->txid_cur)
      crc = mfs_crc32c((const uint8_t *)&fs->wal[j], sizeof(fs->wal[j]), crc);
  }
  uint32_t txid = fs->txid_cur;
  mfs_st st = mfs_token_commit(fs, txid, crc);
  if (st == MFS_OK) {
    uint8_t mark = 1u; /* TXMARK: tx confirmada */
    fs->txid_cur = txid;
    (void)mfs_wal_append(fs, 0x500000u + txid, MFS_RT_TXMARK, &mark, 1u, NULL);
    fs->txid_cur = 0u;
  }
  return st;
}

int mf_tx_abort(mf_t *fs) {
  if (!fs || !fs->mounted)
    return MFS_ENOTMOUNTED;
  if (!fs->tx_open)
    return MFS_ESTATE;
  uint16_t w = 0;
  for (uint16_t i = 0; i < fs->wal_count; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    if (fs->wal[j].valid && fs->wal[j].txid != fs->txid_cur)
      fs->wal[w++] = fs->wal[j];
  }
  fs->wal_count = w;
  fs->tx_open = false;
  fs->sp_depth = 0u;
  sp_restore(fs, 0u); /* deshacer el estado en RAM (§9) */
  {
    /* TXMARK: tx abortada ⇒ su replay se descarta (MFS-TX-003) */
    uint8_t mark = 0u;
    uint32_t txid = fs->txid_cur;
    (void)mfs_wal_append(fs, 0x500000u + txid, MFS_RT_TXMARK, &mark, 1u, NULL);
  }
  fs->txid_cur = 0u;
  return MFS_OK;
}

/* ==== Snapshot de estado por savepoint (§9.6) ====
 * Permite rollback real del estado de metadatos (tamaño + extents inline de
 * los inodos vivos), sin reescribir el medio: las páginas ya programadas se
 * vuelven basura reclamada por GC. Las páginas que un truncado retiró del L2P
 * durante la transacción no se remapean (el mapeo se reconstruye del medio en
 * el siguiente montaje); el rollback restaura tamaño y extents inline. */
#define MFS_SP_SNAP_INO 64u
typedef struct {
  uint32_t ino, size, mtime, parent;
  mfs_extent_t ext[MFS_EXT_INLINE];
  uint8_t valid, nlink, type, hotness;
} sp_ino_snap_t;
static sp_ino_snap_t g_sp_snap[MFS_SAVEPOINT_DEPTH + 1u][MFS_SP_SNAP_INO];
static uint16_t g_sp_n[MFS_SAVEPOINT_DEPTH + 1u];

static void sp_snapshot(mf_t *fs, uint8_t lvl) {
  uint16_t n = 0;
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
    mfs_inode_ram_t *in = &fs->inos[i];
    if (!in->valid || n >= MFS_SP_SNAP_INO)
      continue;
    sp_ino_snap_t *s = &g_sp_snap[lvl][n++];
    s->ino = in->ino;
    s->size = in->size;
    s->mtime = in->mtime;
    s->parent = in->parent;
    s->valid = 1u;
    s->nlink = (uint8_t)in->nlink;
    s->type = in->type;
    s->hotness = in->hotness;
    memcpy(s->ext, in->ext, sizeof(s->ext));
  }
  g_sp_n[lvl] = n;
}

static void sp_restore(mf_t *fs, uint8_t lvl) {
  /* 1) eliminar los inodos creados después de la marca (no están en el
   * snapshot) */
  for (uint32_t i = 1; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
    mfs_inode_ram_t *in = &fs->inos[i];
    if (!in->valid)
      continue;
    bool present = false;
    for (uint32_t k = 0; k < g_sp_n[lvl]; k++)
      if (g_sp_snap[lvl][k].valid && g_sp_snap[lvl][k].ino == in->ino) {
        present = true;
        break;
      }
    if (!present) {
      mfs_extent_drop_range(fs, in->ino, 0u, mfs_vpages_of(fs, in->size));
      memset(in, 0, sizeof(*in));
    }
  }
  /* 2) restaurar el estado de los inodos existentes en la marca */
  for (uint32_t k = 0; k < g_sp_n[lvl]; k++) {
    sp_ino_snap_t *s = &g_sp_snap[lvl][k];
    if (!s->valid)
      continue;
    mfs_inode_ram_t *in = mfs_ino_get(fs, s->ino);
    if (!in) {
      for (uint32_t i = 1; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
        if (!fs->inos[i].valid) {
          in = &fs->inos[i];
          break;
        }
      if (!in)
        continue;
      memset(in, 0, sizeof(*in));
      in->valid = 1u;
      in->ino = s->ino;
    }
    in->size = s->size;
    in->mtime = s->mtime;
    in->parent = s->parent;
    in->nlink = s->nlink;
    in->type = s->type;
    in->hotness = s->hotness;
    memcpy(in->ext, s->ext, sizeof(in->ext));
  }
}

int mf_sp_create(mf_t *fs, mfs_sp *sp) {
  if (!fs || !sp)
    return MFS_EINVAL;
  if (!fs->tx_open)
    return MFS_ESTATE;
  uint8_t maxsp = mfs_limits[fs->mode].max_savepoints;
  if (fs->sp_depth >= maxsp)
    return MFS_EOVERFLOW;
  fs->sp_depth++;
  fs->sp_marks[fs->sp_depth] = fs->wal_count;
  sp_snapshot(fs, fs->sp_depth);
  sp->raw[0] = fs->txid_cur;
  sp->raw[1] = fs->sp_depth;
  return MFS_OK;
}

int mf_sp_rollback(mfs_sp sp) {
  mf_t *fs = g_mfs_instance;
  if (!fs || !fs->tx_open)
    return MFS_ESTATE;
  if (sp.raw[0] != fs->txid_cur || sp.raw[1] == 0u || sp.raw[1] > fs->sp_depth)
    return MFS_EDEADLK;
  uint32_t mark = fs->sp_marks[sp.raw[1]];
  if (mark <= fs->wal_count)
    fs->wal_count = (uint16_t)mark;
  sp_restore(fs, (uint8_t)sp.raw[1]);
  fs->sp_depth = (uint8_t)(sp.raw[1] - 1u);
  return MFS_OK;
}

int mf_sp_release(mfs_sp sp) {
  mf_t *fs = g_mfs_instance;
  if (!fs || !fs->tx_open)
    return MFS_ESTATE;
  if (sp.raw[0] != fs->txid_cur || sp.raw[1] == 0u || sp.raw[1] > fs->sp_depth)
    return MFS_EDEADLK;
  fs->sp_depth = (uint8_t)(sp.raw[1] - 1u);
  return MFS_OK;
}

/* =====================================================================
 * Snapshots O(1) y FlashPatch OTA (§10.8)
 * ===================================================================== */
int mf_snap_create(mf_t *fs, mfs_snap_id *id) {
  if (!fs || !id)
    return MFS_EINVAL;
  if (!fs->mounted)
    return MFS_ENOTMOUNTED;
  uint8_t maxs = (uint8_t)mfs_limits[fs->mode].max_snapshots;
  if (fs->snap_count >= maxs)
    return MFS_ESNAPMAX;
  mfs_st st = mfs_checkpoint_write(fs);
  if (st != MFS_OK)
    return st;
  mfs_snap_id sid = (mfs_snap_id)((fs->epoch << 8) | fs->snap_count);
  fs->snaps[fs->snap_count] = sid;
  fs->snap_roots[fs->snap_count] = fs->root_art;
  fs->snap_count++;
  st = mfs_snap_persist(fs);
  if (st != MFS_OK)
    return st;
  *id = sid;
  fs->hct.snapshots_active = fs->snap_count;
  mfs_hct_event(fs, MFS_EV_SNAP, sid);
  return MFS_OK;
}

int mf_snap_delete(mf_t *fs, mfs_snap_id id) {
  if (!fs)
    return MFS_EINVAL;
  for (uint8_t i = 0; i < fs->snap_count; i++) {
    if (fs->snaps[i] == id) {
      memmove(&fs->snaps[i], &fs->snaps[i + 1],
              (size_t)(fs->snap_count - i - 1u) * sizeof(fs->snaps[0]));
      memmove(&fs->snap_roots[i], &fs->snap_roots[i + 1],
              (size_t)(fs->snap_count - i - 1u) * sizeof(fs->snap_roots[0]));
      fs->snap_count--;
      mfs_gld_add(fs, 2u, true);
      fs->hct.snapshots_active = fs->snap_count;
      return mfs_snap_persist(fs);
    }
  }
  return MFS_ENOENT;
}

int mf_snap_revert(mf_t *fs, mfs_snap_id id) {
  if (!fs)
    return MFS_EINVAL;
  for (uint8_t i = 0; i < fs->snap_count; i++) {
    if (fs->snaps[i] == id) {
      fs->root_art = fs->snap_roots[i]; /* re-fijar raíz: <1 ms */
      mfs_hct_event(fs, MFS_EV_SNAP, id);
      return MFS_OK;
    }
  }
  return MFS_ENOENT;
}

int mf_fpt_begin(mf_t *fs, mfs_fpt *h) {
  if (!fs || !h)
    return MFS_EINVAL;
  if (fs->suite == (uint8_t)MFS_SUITE_NONE)
    return MFS_ECIPHER; /* exige AEAD */
  h->raw[0] = fs->epoch + 1u;
  h->raw[1] = 0u;
  h->raw[2] = mfs_crc32c(NULL, 0u, 0u);
  h->raw[3] = 0u;
  mfs_hct_event(fs, MFS_EV_FPT, h->raw[0]);
  return MFS_OK;
}

int mf_fpt_apply(mfs_fpt h, const void *delta, size_t len) {
  mf_t *fs = g_mfs_instance;
  if (!fs || h.raw[3] != 0u)
    return MFS_ESTATE;
  (void)delta;
  h.raw[1] += (uint32_t)len;
  h.raw[2] = mfs_crc32c((const uint8_t *)delta, (uint32_t)len, h.raw[2]);
  /* MFS-FPT-001(2): overlay de extents en streaming con buffers 2×chunk */
  uint8_t buf[MFS_SCRATCH_MAX];
  uint16_t n = (uint16_t)((len > sizeof(buf)) ? sizeof(buf) : len);
  mfs_st st = mfs_wal_append(fs, 0x800000u + fs->epoch, MFS_RT_DATA,
                             (const uint8_t *)delta, n, NULL);
  return st;
}

int mf_fpt_activate(mfs_fpt h) {
  mf_t *fs = g_mfs_instance;
  if (!fs)
    return MFS_EINVAL;
  if (h.raw[0] <= fs->epoch)
    return MFS_ESECURITY_STATE; /* anti-rollback */
  fs->epoch = h.raw[0];
  h.raw[3] = 1u;
  mfs_hct_event(fs, MFS_EV_FPT, fs->epoch);
  fs->hct.fpt_events++;
  return mf_sync(fs);
}

int mf_fpt_rollback(mfs_fpt h) {
  mf_t *fs = g_mfs_instance;
  if (!fs)
    return MFS_EINVAL;
  (void)h;
  if (fs->snap_count == 0u)
    return MFS_ENOENT;
  return mf_snap_revert(fs, fs->snaps[fs->snap_count - 1u]);
}

/* =====================================================================
 * EDP (§12.3), DAB EXP3 (§24.4), CUSUM (§16)
 * ===================================================================== */
uint32_t mfs_edp_window_us(const mfs_rail_state *r, uint16_t vmin_mv,
                           uint32_t cap_uf, uint32_t i_ma) {
  if (r == NULL || r->mv <= vmin_mv || i_ma == 0u)
    return 0u;
  uint64_t v0 = r->mv, vm = vmin_mv;
  uint64_t e_j_x1e6 = (uint64_t)cap_uf * (v0 * v0 - vm * vm) / 2ull;
  uint64_t p_mw = (uint64_t)r->mv * i_ma;
  if (p_mw == 0u)
    return 0u;
  uint64_t us = e_j_x1e6 * 1000ull / p_mw;
  return (us > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)us;
}

static uint8_t edp_level_for(uint32_t rem_us) {
  if (rem_us > 20000u)
    return 1u;
  if (rem_us > 8000u)
    return 2u;
  if (rem_us > 3000u)
    return 3u;
  if (rem_us > 800u)
    return 4u;
  return 5u;
}

void mfs_edp_check(mf_t *fs) {
  const mfs_l2_driver *d = fs->cfg->drv;
  bool trig = g_mfs_edp_inject;
  if (!trig && d && d->rail_ok) {
    mfs_rail_state rs;
    if (d->rail_ok(d->ctx, &rs) && !rs.ok)
      trig = true;
  }
  if (!trig) {
    if (fs->edp_state != MFS_EDP_MONITOR) {
      fs->edp_state = MFS_EDP_MONITOR;
      fs->edp_level = 0u;
    }
    return;
  }
  mfs_rail_state rs;
  memset(&rs, 0, sizeof(rs));
  if (d && d->rail_ok)
    (void)d->rail_ok(d->ctx, &rs);
  else {
    rs.mv = 3300u;
    rs.t_remaining_us = g_mfs_edp_inject ? 1200u : 0u;
  }
  uint32_t rem = rs.t_remaining_us;
  uint8_t lvl = edp_level_for(rem);
  if (lvl > fs->edp_level) {
    fs->edp_level = lvl;
    fs->edp_state = (lvl >= 4u) ? MFS_EDP_DRAIN : MFS_EDP_ARMED;
    mfs_hct_event(fs, MFS_EV_EDP_DRAIN, lvl);
  }
  if (lvl >= 5u)
    mfs_edp_drain(fs);
}

void mfs_edp_drain(mf_t *fs) {
  fs->edp_state = MFS_EDP_DRAIN;
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
    if (fs->inos[i].valid)
      mfs_meta_flush(fs, fs->inos[i].ino);
  if (fs->tx_open)
    (void)mf_tx_commit(fs);
  else
    (void)mfs_checkpoint_write(fs);
  uint32_t slot = (fs->tok_seq_a & 1u) ? 0u : 1u;
  (void)sb_write(fs, slot, fs->epoch, (uint32_t)fs->seq);
  fs->edp_state = MFS_EDP_DONE;
  fs->hct.edp_drains++;
  mfs_hct_event(fs, MFS_EV_EDP_DRAIN, fs->edp_level);
}

void mfs_dab_init(mf_t *fs) {
  fs->dab_rng = fs->cfg->dab_seed ? fs->cfg->dab_seed : 0xDAB5EEDu;
  fs->tau[0] = MFS_TAU_RT_A_US;
  fs->tau[1] = MFS_TAU_RT_B_US;
  fs->tau[2] = MFS_TAU_RT_C_US;
  fs->d_max = 256u; /* §25 D_max default */
  fs->dab_state = MFS_DAB_FROZEN;
  for (int i = 0; i < 3; i++)
    fs->w_exp3[i] = 1.0f / 3.0f;
}

uint32_t mfs_exp3_update(float w[3], const float r[3], const float x[3][3],
                         uint32_t *rng, int n_arms) {
  if (n_arms > 3)
    n_arms = 3;
  float sum = 0;
  for (int i = 0; i < n_arms; i++)
    sum += w[i];
  if (sum <= 0.0f) {
    for (int i = 0; i < n_arms; i++)
      w[i] = 1.0f / (float)n_arms;
    sum = 1.0f;
  }
  *rng = (*rng) * 1103515245u + 12345u;
  float u = (float)((*rng >> 8) & 0xFFFFFFu) / 16777216.0f * sum;
  int arm = 0;
  float acc = 0.0f;
  for (int i = 0; i < n_arms; i++) {
    acc += w[i];
    if (u <= acc) {
      arm = i;
      break;
    }
    arm = i;
  }
  float p_arm = w[arm] / sum;
  for (int i = 0; i < n_arms; i++) {
    float xi =
        (i == arm) ? (r[arm] / (p_arm > 0.0001f ? p_arm : 0.0001f)) : 0.0f;
    if (xi > 3.0f)
      xi = 3.0f;
    w[i] *= expf_fast(0.1f * xi / (float)n_arms);
  }
  (void)x;
  return (uint32_t)arm;
}

float mfs_dab_reward_latency(uint32_t us) {
  if (us <= MFS_TAU_RT_C_US)
    return 1.0f;
  if (us >= MFS_TAU_RT_C_US * 3u)
    return 0.0f;
  return (float)(MFS_TAU_RT_C_US * 3u - us) / (float)(MFS_TAU_RT_C_US * 2u);
}

uint8_t mfs_dab_select_arm(mf_t *fs) {
  if (fs->dab_state == MFS_DAB_FROZEN)
    return 0u;
  float r[3] = {0, 0, 0}, x[3][3] = {{0}};
  uint32_t arm = mfs_exp3_update(fs->w_exp3, r, x, &fs->dab_rng, 3);
  return (uint8_t)arm;
}

void mfs_dab_feedback(mf_t *fs, uint8_t arm, uint32_t latency_us) {
  float r[3] = {0, 0, 0};
  if (arm < 3u)
    r[arm] = mfs_dab_reward_latency(latency_us);
  float x[3][3] = {{0}};
  mfs_exp3_update(fs->w_exp3, r, x, &fs->dab_rng, 3);
  fs->ops_since_dab++;
  if (fs->cfg->rt_strict && fs->dab_state == MFS_DAB_EXPLORE)
    fs->dab_state = MFS_DAB_EXPLOIT;
  if (fs->ops_since_dab >= 256u) { /* §25 DAB N=256 ops */
    fs->ops_since_dab = 0;
    fs->dab_state =
        (fs->dab_state == MFS_DAB_EXPLOIT) ? MFS_DAB_EXPLORE : MFS_DAB_EXPLOIT;
    fs->hct.dab_changes++;
    mfs_hct_event(fs, MFS_EV_DAB_CHANGE, fs->dab_state);
  }
}

void mfs_cusum_update(mf_t *fs, int16_t x_x100) {
  /* CUSUM sobre la media adaptativa (α = 1/16): detecta cambios de régimen
   * de latencia sin depender de una referencia fija del perfil (§16). */
  int x = (int)x_x100;
  if (!fs->cusum_init) {
    fs->cusum_last_mean_x100 = (int16_t)x;
    fs->cusum_init = 1u;
  }
  int mean = fs->cusum_last_mean_x100;
  int drift = 500; /* k default §25 */
  int sl = (x - mean) - drift;
  int sr = (mean - x) - drift;
  int pos = (int)fs->cusum_pos + (sl > 0 ? sl : 0);
  int neg = (int)fs->cusum_neg + (sr > 0 ? sr : 0);
  fs->cusum_pos = (uint16_t)((pos > 60000) ? 60000 : pos);
  fs->cusum_neg = (uint16_t)((neg > 60000) ? 60000 : neg);
  fs->cusum_last_mean_x100 = (int16_t)(mean + (x - mean) / 16);
  if (mfs_cusum_alarm(fs)) {
    fs->hct.cusum_alarms++;
    mfs_hct_event(fs, MFS_EV_CUSUM_ALARM, (uint32_t)fs->cusum_pos);
    if (fs->cfg->rt_strict)
      fs->dab_state = MFS_DAB_FROZEN;
  }
}

bool mfs_cusum_alarm(const mf_t *fs) {
  const uint32_t h = 20000u; /* h default §25 */
  return fs->cusum_pos > h;
}

/* =====================================================================
 * HCT (§16): anillo de eventos + agregados; export CBOR+COSE (§15)
 * ===================================================================== */
typedef struct {
  uint32_t t_us;
  uint16_t ev;
  uint16_t pad;
  uint32_t val;
} hct_ent_t;
static hct_ent_t hct_ring[64];
static uint8_t hct_head, hct_n;

void mfs_hct_event(mf_t *fs, uint16_t ev, uint32_t val) {
  (void)fs;
  uint8_t i = (uint8_t)((hct_head + hct_n) % 64u);
  if (hct_n < 64u)
    hct_n++;
  else
    hct_head = (uint8_t)((hct_head + 1u) % 64u);
  hct_ent_t *e = &hct_ring[i];
  e->t_us = mfs_port_time_us();
  e->ev = ev;
  e->val = val;
  e->pad = 0u;
}

void mfs_hct_flush(mf_t *fs) {
  if (((mfs_mode_is_classic(fs->mode) && fs->mode <= MFS_MODE_NANO) ||
       mfs_mode_is_8bit(fs->mode)) &&
      hct_n > 0u) {
    uint8_t agg[MFS_SCRATCH_MAX];
    uint16_t o = 0;
    for (uint8_t i = 0; i < hct_n && o + 12u <= sizeof(agg); i++) {
      hct_ent_t *e = &hct_ring[(hct_head + i) % 64u];
      mfs_st32(agg + o, e->t_us);
      o += 4;
      mfs_st16(agg + o, e->ev);
      o += 2;
      mfs_st16(agg + o, e->pad);
      o += 2;
      mfs_st32(agg + o, e->val);
      o += 4;
    }
    uint32_t lba = 0x900000u + fs->epoch;
    (void)mfs_wal_append(fs, lba, MFS_RT_HCTAGG, agg, o, NULL);
    hct_head = 0;
    hct_n = 0;
  }
}

static uint32_t cbor_head(uint8_t *out, uint32_t cap, uint8_t major,
                          uint64_t v) {
  uint32_t n = 0;
  uint8_t mt = (uint8_t)(major << 5);
  if (v < 24u) {
    if (n < cap)
      out[n] = (uint8_t)(mt | v);
    n++;
  } else if (v < 0x100u) {
    if (n + 2u <= cap) {
      out[n] = mt | 24u;
      out[n + 1] = (uint8_t)v;
    }
    n += 2u;
  } else if (v < 0x10000u) {
    if (n + 3u <= cap) {
      out[n] = mt | 25u;
      mfs_st16(out + n + 1, (uint16_t)v);
    }
    n += 3u;
  } else {
    if (n + 5u <= cap) {
      out[n] = mt | 26u;
      mfs_st32(out + n + 1, (uint32_t)v);
    }
    n += 5u;
  }
  return n;
}

uint32_t mfs_cbor_health(const mfs_health_t *h, uint8_t *out, uint32_t cap) {
  uint32_t o = 0;
  o += cbor_head(out + o, cap - o, 5u, 14u);
#define PUTK(k, v)                                                             \
  do {                                                                         \
    if (o < cap)                                                               \
      o += cbor_head(out + o, cap - o, 0u, (k));                               \
    if (o < cap)                                                               \
      o += cbor_head(out + o, cap - o, 0u, (v));                               \
  } while (0)
  PUTK(1, h->mode);
  PUTK(2, h->suite);
  PUTK(3, h->epoch);
  PUTK(4, h->seq);
  PUTK(5, h->writes_prog);
  PUTK(6, h->writes_host);
  PUTK(7, h->gc_relocated);
  PUTK(8, h->quarantined_blocks);
  PUTK(9, h->power_events);
  PUTK(10, h->energy_mj_total);
  PUTK(11, h->p999_commit_us);
  PUTK(12, h->snapshots_active);
  PUTK(13, h->cusum_alarms);
  PUTK(14, h->dab_changes);
#undef PUTK
  return o;
}

int mf_export_health(mf_t *fs, void *buf, size_t len) {
  if (!fs || !buf)
    return MFS_EINVAL;
  uint8_t *out = (uint8_t *)buf;
  mfs_health_t h = fs->hct;
  h.mode = fs->mode;
  h.suite = fs->suite;
  h.epoch = fs->epoch;
  h.seq = fs->seq;
  h.edp_state = fs->edp_state;
  h.dab_state = fs->dab_state;
  h.debt_gld = fs->debt_gld;
  h.free_pages = (uint16_t)fs->free_pages;
  h.snapshots_active = fs->snap_count;
  uint32_t body = mfs_cbor_health(&h, out, (uint32_t)len);
  if ((size_t)body + 32u > len)
    return MFS_EOVERFLOW;
  if (fs->cfg->key) {
    uint8_t sig[32];
    mfs_hmac_sha256(fs->cfg->key, 32u, out, body, sig);
    memcpy(out + body, sig, 32u); /* COSE_Sign1 detached-style */
    return (int)(body + 32u);
  }
  return (int)body;
}

int mf_ioctl(mf_t *fs, uint32_t cmd, void *arg) {
  if (!fs)
    return MFS_EINVAL;
  switch ((mfs_ioctl_cmd)cmd) {
  case MFS_IOCTL_HEALTH: {
    if (!arg)
      return MFS_EINVAL;
    mfs_health_t *h = (mfs_health_t *)arg;
    *h = fs->hct;
    h->mode = fs->mode;
    h->suite = fs->suite;
    h->epoch = fs->epoch;
    h->seq = fs->seq;
    h->edp_state = fs->edp_state;
    h->dab_state = fs->dab_state;
    h->debt_gld = fs->debt_gld;
    h->free_pages = (uint16_t)fs->free_pages;
    h->snapshots_active = fs->snap_count;
    return MFS_OK;
  }
  case MFS_IOCTL_STATS:
  case MFS_IOCTL_SNAPSHOT_LIST: {
    if (!arg)
      return MFS_EINVAL;
    mfs_snap_id *lst = (mfs_snap_id *)arg;
    for (uint8_t i = 0; i < fs->snap_count; i++)
      lst[i] = fs->snaps[i];
    return (int)fs->snap_count;
  }
  case MFS_IOCTL_SET_MODE_HINT:
    if (fs->mounted)
      return MFS_EBUSY;
    return MFS_OK;
  case MFS_IOCTL_FREEZE_DAB:
    fs->dab_state = arg ? MFS_DAB_EXPLOIT : MFS_DAB_FROZEN;
    mfs_hct_event(fs, MFS_EV_DAB_CHANGE, fs->dab_state);
    return MFS_OK;
  case MFS_IOCTL_EXPORT_HWV:
    if (!arg)
      return MFS_EINVAL;
    *(mfs_hwv_t *)arg = fs->hwv;
    return MFS_OK;
  case MFS_IOCTL_VERIFY_BEGIN:
    return mf_verify(fs, MFS_VERIFY_QUICK);
  case MFS_IOCTL_EDP_INJECT:
    g_mfs_edp_inject = (arg != NULL) && (*(bool *)arg);
    return MFS_OK;
  case MFS_IOCTL_GET_JPEROP: {
    if (!arg)
      return MFS_EINVAL;
    uint32_t ops = fs->hct.ops_count ? fs->hct.ops_count : 1u;
    *(uint32_t *)arg = fs->eld_spent_mj / ops;
    return MFS_OK;
  }
  default:
    return MFS_EINVAL;
  }
}

/* fsck read-only (§21.1): niveles quick/meta/full */
int mf_verify(mf_t *fs, mfs_verify_level lvl) {
  if (!fs || !fs->mounted)
    return MFS_ENOTMOUNTED;
  uint32_t ep = 0, gn = 0;
  if (sb_read(fs, mfs_sb_a_off(), &ep, &gn, NULL) != MFS_OK &&
      sb_read(fs, mfs_sb_b_off(&fs->hwv), &ep, &gn, NULL) != MFS_OK)
    return MFS_ECORRUPT;
  if (lvl == MFS_VERIFY_QUICK)
    return MFS_OK;
  for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
    mfs_inode_ram_t *n = &fs->inos[i];
    if (!n->valid || n->type == 1u)
      continue;
    /* todas las páginas lógicas del fichero (no sólo los 4 extents inline):
     * el resto se resuelve por L2P */
    uint32_t vp_end = mfs_vpages_of(fs, n->size);
    for (uint32_t vp = 0; vp < vp_end; vp++) {
      uint32_t lba = extent_lba(n->ino, vp);
      uint32_t pp = mfs_l2p_get(lba);
      if (pp == MFS_L2P_FREE)
        return MFS_ECORRUPT;
      if (lvl >= MFS_VERIFY_META) {
        uint8_t kind, gen, snap, dict;
        uint16_t rlen;
        static uint8_t tmp[MFS_SCRATCH_MAX];
        mfs_st st = mfs_rec_read(fs, pp, lba, 0xFFu, &kind, &gen, &snap, &dict,
                                 tmp, (uint16_t)sizeof(tmp), &rlen);
        if (st == MFS_EBADMSG || st == MFS_ESECURITY_STATE)
          return MFS_ECORRUPT;
      }
    }
  }
  if (lvl == MFS_VERIFY_FULL) {
    uint8_t t1[MFS_TOKEN_T1_SIZE];
    if (mfs_tok_latest(fs, t1) && memcmp(t1 + 24, fs->merkle_root, 4u) != 0)
      return MFS_ECORRUPT;
  }
  return MFS_OK;
}

/* persistencia de tabla de snapshots (§10.8): registro EPOCH */
mfs_st mfs_snap_persist(mf_t *fs) {
  uint8_t buf[MFS_SCRATCH_MAX];
  uint16_t o = 0;
  buf[o++] = fs->snap_count;
  for (uint8_t i = 0; i < fs->snap_count && o + 8u <= sizeof(buf); i++) {
    mfs_st32(buf + o, fs->snaps[i]);
    o += 4;
    mfs_st32(buf + o, fs->snap_roots[i]);
    o += 4;
  }
  return mfs_wal_append(fs, 0xE00000u + fs->epoch, MFS_RT_EPOCH, buf, o, NULL);
}
