/* mfs_hmt.c — Heterogeneous Media Tiering (§11.11).
 *
 * Con T0 (FRAM/MRAM/EEPROM) presente y presupuesto suficiente, los metadatos
 * calientes (INODE/DIRENT/EXTENT/EPOCH) se espejan en un log append-only
 * byte-addressable en T0 (sin erase, sin hazard). T1 conserva la copia
 * durable en el WAL; la recuperación prefiere el registro de T0 de mayor
 * secuencia por clave (§11.11).
 *
 * Formato de registro T0 (20 B de cabecera + payload):
 *   [0..1] magic 'M','0' | [2] ver | [3] kind | [4..7] seq abs | [8..11] lba
 *   [12..13] len | [14..17] crc32c(cabecera 0..13 ‖ payload) | [18..19] rsv
 */
#include "mfs_internal.h"
#include <string.h>

#define HMT_MAGIC0 'M'
#define HMT_MAGIC1 '0'
#define HMT_HDR 20u
#define HMT_KEYS 256u

bool mfs_hmt_active(const mf_t *fs) { return fs && fs->hmt_active != 0u; }

mfs_st mfs_hmt_init(mf_t *fs) {
  if (!fs)
    return MFS_EINVAL;
  fs->hmt_active = 0u;
  fs->hmt_t0_off = 0u;
  fs->hmt_t0_seq = 0u;
  if (!fs->cfg->drv_t0 || fs->hwv.t0_size == 0u)
    return MFS_ENOTSUP;
  /* Presupuesto T0 (§11.11): Balanced+ exige ≥ 32 KB; si no, degradar con
   * evento y seguir en mono-medio (nunca degradación silenciosa). */
  uint32_t need = (fs->mode >= MFS_MODE_BALANCED) ? MFS_HMT_T0_BUDGET
                                                  : (MFS_HMT_T0_BUDGET / 4u);
  if (fs->hwv.t0_size < need) {
    mfs_hct_event(fs, MFS_EV_HMT, 0x8000u | (fs->hwv.t0_size & 0x7FFFu));
    return MFS_ENOTVIABLE;
  }
  fs->hmt_active = 1u;
  mfs_hct_event(fs, MFS_EV_HMT, fs->hwv.t0_size);
  return MFS_OK;
}

static uint32_t hmt_budget(const mf_t *fs) {
  uint32_t b = fs->hwv.t0_size;
  if (b > 65536u)
    b = 65536u; /* cota de trabajo del log */
  return b;
}

mfs_st mfs_hmt_t0_write(mf_t *fs, uint8_t kind, uint32_t lba,
                        const uint8_t *data, uint16_t len, uint32_t *t0_addr) {
  if (!fs || !data)
    return MFS_EINVAL;
  if (!fs->hmt_active)
    return MFS_ENOTSUP;
  uint32_t budget = hmt_budget(fs);
  uint32_t total = HMT_HDR + len;
  if (total > budget)
    return MFS_EINVAL;
  if (fs->hmt_t0_off + total > budget)
    fs->hmt_t0_off = 0u; /* wrap (caché) */

  uint8_t hdr[HMT_HDR];
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = HMT_MAGIC0;
  hdr[1] = HMT_MAGIC1;
  hdr[2] = 1u;
  hdr[3] = kind;
  fs->hmt_t0_seq++;
  mfs_st32(hdr + 4, fs->hmt_t0_seq);
  mfs_st32(hdr + 8, lba);
  mfs_st16(hdr + 12, len);
  uint32_t crc = mfs_crc32c(hdr, 14u, 0u);
  crc = mfs_crc32c(data, len, crc);
  mfs_st32(hdr + 14, crc);

  mfs_st st = mfs_t0_write(fs, fs->hmt_t0_off, hdr, HMT_HDR);
  if (st != MFS_OK) {
    fs->hmt_active = 0u; /* T0 en fallo ⇒ degradar a mono-medio */
    mfs_hct_event(fs, MFS_EV_HMT, 0xC000u);
    return st;
  }
  if (len) {
    st = mfs_t0_write(fs, fs->hmt_t0_off + HMT_HDR, data, len);
    if (st != MFS_OK) {
      fs->hmt_active = 0u;
      return st;
    }
  }
  if (t0_addr)
    *t0_addr = fs->hmt_t0_off;
  fs->hmt_t0_off += total;
  return MFS_OK;
}

/* Lee el último registro válido por clave (lba) del log T0 y lo aplica. */
mfs_st mfs_hmt_scan(mf_t *fs) {
  if (!fs || !fs->hmt_active)
    return MFS_OK;
  typedef struct {
    uint32_t lba;
    uint32_t seq;
    uint32_t off;
    uint8_t kind;
    uint8_t used;
  } key_t;
  static key_t keys[HMT_KEYS];
  memset(keys, 0, sizeof(keys));
  uint32_t budget = hmt_budget(fs);

  uint32_t off = 0, found = 0;
  while (off + HMT_HDR <= budget) {
    uint8_t hdr[HMT_HDR];
    if (mfs_t0_read(fs, off, hdr, HMT_HDR) != MFS_OK)
      break;
    if (hdr[0] != HMT_MAGIC0 || hdr[1] != HMT_MAGIC1)
      break;
    uint8_t kind = hdr[3];
    uint32_t seq = mfs_ld32(hdr + 4);
    uint32_t lba = mfs_ld32(hdr + 8);
    uint16_t len = mfs_ld16(hdr + 12);
    if (off + HMT_HDR + len > budget)
      break;
    uint32_t crc = mfs_ld32(hdr + 14);
    static uint8_t pl[MFS_CHUNK_EXTENDED];
    if (len > sizeof(pl))
      break;
    if (len && mfs_t0_read(fs, off + HMT_HDR, pl, len) != MFS_OK)
      break;
    uint32_t calc = mfs_crc32c(hdr, 14u, 0u);
    calc = mfs_crc32c(pl, len, calc);
    if (calc != crc) {
      off += HMT_HDR + len;
      continue;
    } /* registro dañado */
    /* localizar/actualizar clave */
    key_t *slot = NULL;
    for (uint32_t i = 0; i < HMT_KEYS; i++) {
      if (keys[i].used && keys[i].lba == lba) {
        slot = &keys[i];
        break;
      }
      if (!keys[i].used && !slot)
        slot = &keys[i];
    }
    if (slot && (!slot->used || seq > slot->seq)) {
      slot->used = 1u;
      slot->lba = lba;
      slot->seq = seq;
      slot->off = off;
      slot->kind = kind;
      found++;
    }
    off += HMT_HDR + len;
  }

  uint16_t max_ino = 1u;
  for (uint32_t i = 0; i < HMT_KEYS; i++) {
    if (!keys[i].used)
      continue;
    uint8_t hdr[HMT_HDR];
    if (mfs_t0_read(fs, keys[i].off, hdr, HMT_HDR) != MFS_OK)
      continue;
    uint16_t len = mfs_ld16(hdr + 12);
    static uint8_t pl[MFS_CHUNK_EXTENDED];
    if (len > sizeof(pl))
      continue;
    if (len && mfs_t0_read(fs, keys[i].off + HMT_HDR, pl, len) != MFS_OK)
      continue;
    switch (keys[i].kind) {
    case MFS_RT_INODE:
      (void)mfs_inode_apply(fs, pl, len, &max_ino);
      break;
    case MFS_RT_DIRENT:
      mfs_dirent_apply(fs, pl, len);
      break;
    case MFS_RT_EPOCH:
      mfs_epoch_apply(fs, pl, len);
      break;
    default:
      break;
    }
  }
  (void)max_ino;
  fs->hmt_t0_seq =
      0u; /* la secuencia se reanuda desde 0 en el siguiente arranque */
  mfs_hct_event(fs, MFS_EV_HMT, found);
  return MFS_OK;
}
