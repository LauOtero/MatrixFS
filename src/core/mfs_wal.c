/* mfs_wal.c — WAL+ (§9): registro anticipado con ventana BMT acotada,
 * anillo de tokens T1/T0 en sector dedicado con contador monótono
 * (equivalente al contador termométrico §8.4/§9.2), checkpoint §9.5 con
 * raíz Merkle BLAKE3 y recuperación acotada (§24.2).
 *
 * Invariantes normativas implementadas:
 *  MFS-TX-001: ninguna modificación de metadatos es visible antes del token T1.
 *  MFS-TX-002: el orden de escritura es WALENT(s) → … → T1(txid, crc, epoch).
 *  MFS-TX-003: tras corte, el montaje sólo valida entradas con token vigente.
 *  MFS-TX-004: la ventana WAL en RAM está acotada por W (§25); overflow ⇒
 *             checkpoint forzoso o MFS_EBACKPRESSURE.
 *  MFS-HMT-001: si hay T0 (FRAM/MRAM) el token se escribe primero en T0
 *             (escritura byte-directa, sin erase) y luego se espeja en T1.
 *  NOR-correcto: el anillo de 8 slots vive en su propio sector; al dar la
 *             vuelta se borra el sector antes de reprogramar (nunca se
 *             reprograma una celda sin erase previo).
 */
#include "mfs_internal.h"
#include <string.h>

/* ==== payload WALENT (orden determinista LE) ====
 * [0..3] txid | [4..7] lba | [8..9] len | [10] kind | [11] sp_depth | [12..]
 * datos */
#define WALHDR 12u

/* ==== anillo de tokens A/B (8 slots × 32 B en el sector dedicado §9.2) ==== */
#define MFS_TOK_SLOTS 8u
#define MFS_TOK_SIZE MFS_TOKEN_T1_SIZE

static uint32_t tok_slot_addr(mf_t *fs, uint32_t slot) {
  return mfs_tok_off(&fs->hwv) + (slot % MFS_TOK_SLOTS) * MFS_TOK_SIZE;
}

/* =====================================================================
 * append WAL (§9.1)
 * ===================================================================== */
mfs_st mfs_wal_append(mf_t *fs, uint32_t lba, uint8_t kind, const uint8_t *pl,
                      uint16_t len, uint32_t *ppage) {
  if (!fs->mounted)
    return MFS_ENOTMOUNTED;
  if ((uint32_t)len + WALHDR > mfs_page_bytes(fs))
    return MFS_EINVAL;

  /* Las escrituras de metadatos también participan en el control de deuda
   * GLD (la deuda la generan tanto datos como metadatos). */
  if (fs->debt_gld >= fs->d_max)
    mfs_gld_maybe_gc(fs);

  if (fs->wal_count >= MFS_WAL_WINDOW_MAX) { /* MFS-TX-004 */
    mfs_st st = mfs_checkpoint_write(fs);
    if (st != MFS_OK)
      return MFS_EBACKPRESSURE;
  }
  if ((int)fs->zone_wal < 0 || !mfs_zone(fs, fs->zone_wal) ||
      mfs_zone(fs, fs->zone_wal)->state != MFS_Z_OPEN) {
    int z = mfs_zone_alloc_open(fs, 0u);
    if (z < 0) {
      mfs_gld_maybe_gc(fs);
      z = mfs_zone_alloc_open(fs, 0u);
    }
    if (z < 0) {
      mfs_gc_force(fs);
      z = mfs_zone_alloc_open(fs, 0u);
    }
    if (z < 0)
      return MFS_ENOSPC;
    fs->zone_wal = (uint32_t)z;
  }

  static uint8_t rec[MFS_CHUNK_EXTENDED];
  mfs_st32(rec, fs->txid_cur);
  mfs_st32(rec + 4, lba);
  mfs_st16(rec + 8, len);
  rec[10] = kind;
  rec[11] = fs->sp_depth;
  if (len)
    memcpy(rec + WALHDR, pl, len);
  for (uint32_t i = WALHDR + len; i < mfs_page_bytes(fs); i++)
    rec[i] = 0x5Au;

  uint32_t pp = 0xFFFFFFFFu;
  mfs_st st = mfs_rec_write(fs, fs->zone_wal, lba, MFS_RT_WALENT,
                            (uint8_t)(fs->epoch & 0xFu), 0u, 0u, 0u, rec,
                            (uint16_t)(WALHDR + len), &pp);
  if (st == MFS_ENOSPC) { /* zona sellada a media */
    int z = mfs_zone_alloc_open(fs, 0u);
    if (z < 0)
      return MFS_ENOSPC;
    fs->zone_wal = (uint32_t)z;
    st = mfs_rec_write(fs, fs->zone_wal, lba, MFS_RT_WALENT,
                       (uint8_t)(fs->epoch & 0xFu), 0u, 0u, 0u, rec,
                       (uint16_t)(WALHDR + len), &pp);
  }
  if (st != MFS_OK)
    return st;

  uint16_t idx =
      (uint16_t)((fs->wal_head + fs->wal_count) % MFS_WAL_WINDOW_MAX);
  fs->wal[idx].txid = fs->txid_cur;
  fs->wal[idx].ppage = pp;
  fs->wal[idx].committed = fs->tx_open ? 0u : 1u;
  fs->wal[idx].valid = 1u;
  fs->wal_count++;
  if (ppage)
    *ppage = pp;
  return MFS_OK;
}

/* =====================================================================
 * Token de commit (§9.2)
 * T1 (flash, 32 B):
 * magic2|ver1|flags1|txid4|n4|body_crc4|epoch4|seq4|merkle4|crc4 T0 (HMT, 16
 * B):   magic2|ver1|flags1|txid4|body_crc4|seq4
 * ===================================================================== */
mfs_st mfs_token_commit(mf_t *fs, uint32_t txid, uint32_t crc) {
  uint8_t t1[MFS_TOK_SIZE];
  memset(t1, 0xFFu, sizeof(t1));
  t1[0] = MFS_TOK_MAGIC_HI;
  t1[1] = MFS_TOK_MAGIC_LO;
  t1[2] = 1u;
  t1[3] = 0u;
  mfs_st32(t1 + 4, txid);
  uint32_t n = 0;
  for (uint32_t i = 0; i < fs->wal_count; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    if (fs->wal[j].valid && fs->wal[j].txid == txid)
      n++;
  }
  mfs_st32(t1 + 8, n);
  mfs_st32(t1 + 12, crc);
  mfs_st32(t1 + 16, fs->epoch);
  uint32_t seq = ++fs->tok_seq_a; /* contador monótono de commits */
  mfs_st32(t1 + 20, seq);
  memcpy(t1 + 24, fs->merkle_root, 4u);
  mfs_st32(t1 + 28, mfs_crc32c(t1, 28u, 0u));

  /* 1) HMT: token primero en T0 (byte-directo, MFS-HMT-001) */
  if (mfs_has_t0(fs)) {
    uint8_t t0[MFS_TOKEN_T0_SIZE];
    memset(t0, 0xFFu, sizeof(t0));
    t0[0] = t1[0];
    t0[1] = t1[1];
    t0[2] = 1u;
    t0[3] = 0u;
    mfs_st32(t0 + 4, txid);
    mfs_st32(t0 + 8, crc);
    mfs_st32(t0 + 12, seq);
    mfs_st st0 = mfs_t0_write(fs, (seq % MFS_TOK_SLOTS) * MFS_TOKEN_T0_SIZE, t0,
                              sizeof(t0));
    if (st0 != MFS_OK)
      return st0;
  }

  /* 2) Espejo T1 en el sector de tokens; al dar la vuelta, erase previo */
  uint32_t slot = seq % MFS_TOK_SLOTS;
  uint32_t slot_addr = tok_slot_addr(fs, seq);
  bool need_erase = (slot == 0u && seq != 0u);
  if (!need_erase) {
    /* defensa: si el slot no está virgen (p.ej. reanudación de seq tras
     * montaje), forzar erase del sector para no reprogramar sin borrar */
    uint8_t cur[MFS_TOK_SIZE];
    if (mfs_read(fs, slot_addr, cur, sizeof(cur)) == MFS_OK) {
      for (uint32_t i = 0; i < sizeof(cur); i++)
        if (cur[i] != 0xFFu) {
          need_erase = true;
          break;
        }
    }
  }
  if (need_erase) {
    mfs_st est = mfs_erase(fs, mfs_tok_off(&fs->hwv));
    if (est != MFS_OK)
      return est;
  }
  mfs_st st = mfs_write(fs, slot_addr, t1, sizeof(t1));
  if (st != MFS_OK)
    return st;

  /* 3) marcar entradas comprometidas en la ventana RAM */
  for (uint32_t i = 0; i < fs->wal_count; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    if (fs->wal[j].valid && fs->wal[j].txid == txid)
      fs->wal[j].committed = 1u;
  }
  fs->tx_open = false;
  fs->sp_depth = 0u;
  return MFS_OK;
}

/* leer el token válido más reciente (mayor seq) del anillo */
static bool tok_read_latest(mf_t *fs, uint8_t out[MFS_TOK_SIZE]) {
  bool found = false;
  uint32_t best = 0;
  for (uint32_t s = 0; s < MFS_TOK_SLOTS; s++) {
    uint8_t t[MFS_TOK_SIZE];
    if (mfs_read(fs, tok_slot_addr(fs, s), t, sizeof(t)) != MFS_OK)
      continue;
    if (t[0] != MFS_TOK_MAGIC_HI || t[1] != MFS_TOK_MAGIC_LO)
      continue;
    if (mfs_crc32c(t, 28u, 0u) != mfs_ld32(t + 28))
      continue;
    uint32_t sq = mfs_ld32(t + 20);
    if (!found || sq > best) {
      best = sq;
      memcpy(out, t, sizeof(t));
      found = true;
    }
  }
  return found;
}

bool mfs_tok_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE]);
bool mfs_tok_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE]) {
  return tok_read_latest(fs, out);
}

/* =====================================================================
 * Recuperación de montaje (§9.1/§24.2 paso 6)
 * La reconstrucción efectiva de metadatos la realiza mfs_recover_metadata()
 * escaneando los registros E2G (fuente de verdad auto-descriptiva); aquí se
 * detecta el corte de energía y se valida la coherencia del token.
 * ===================================================================== */
mfs_st mfs_wal_replay(mf_t *fs, uint32_t window) {
  (void)window;
  uint8_t t1[MFS_TOK_SIZE];
  if (tok_read_latest(fs, t1)) {
    fs->hct.power_events++;
    mfs_hct_event(fs, MFS_EV_POWER_LOSS, mfs_ld32(t1 + 20));
    /* epoch anti-rollback desde el token (§15 MFS-SEC-003) */
    uint32_t ep = mfs_ld32(t1 + 16);
    if (ep > fs->epoch)
      fs->epoch = ep;
  } else {
    mfs_hct_event(fs, MFS_EV_MOUNT_OK, fs->epoch);
  }
  return MFS_OK;
}

/* =====================================================================
 * Checkpoint §9.5 — raíz Merkle BLAKE3 del cuerpo + token sintético
 * ===================================================================== */
mfs_st mfs_checkpoint_write(mf_t *fs) {
  if (fs->wal_count == 0u)
    return MFS_OK;
  uint8_t lvl[64][32];
  uint32_t cnt = fs->wal_count;
  if (cnt > 64u)
    cnt = 64u;
  for (uint32_t i = 0; i < cnt; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    mfs_b3_256(NULL, 0u, (const uint8_t *)&fs->wal[j], sizeof(fs->wal[j]),
               lvl[i]);
  }
  while (cnt > 1u) {
    uint32_t next = (cnt + 1u) / 2u;
    for (uint32_t i = 0; i + 1u < cnt; i += 2u) {
      uint8_t pair[64];
      memcpy(pair, lvl[i], 32u);
      memcpy(pair + 32, lvl[i + 1u], 32u);
      mfs_b3_256(NULL, 0u, pair, 64u, lvl[i / 2u]);
    }
    if (cnt & 1u)
      memcpy(lvl[next - 1u], lvl[cnt - 1u], 32u);
    cnt = next;
  }
  memcpy(fs->merkle_root, lvl[0], 8u);

  uint32_t saved_tx = fs->txid_cur;
  fs->txid_cur = 0xCECE0000u ^ (uint32_t)fs->seq;
  mfs_st st = mfs_token_commit(fs, fs->txid_cur, 0u);
  fs->txid_cur = saved_tx;
  if (st != MFS_OK)
    return st;

  fs->hct.scrub_done++;
  /* conservar sólo entradas no comprometidas */
  uint16_t w = 0;
  for (uint16_t i = 0; i < fs->wal_count; i++) {
    uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
    if (fs->wal[j].valid && fs->wal[j].committed != 1u)
      fs->wal[w++] = fs->wal[j];
  }
  fs->wal_head = 0;
  fs->wal_count = w;
  if (fs->wal_count > 0u)
    fs->tx_open = true; /* hay tx pendiente en curso */
  return MFS_OK;
}

mfs_st mfs_checkpoint_load(mf_t *fs) {
  uint8_t t1[MFS_TOK_SIZE];
  if (!tok_read_latest(fs, t1))
    return MFS_OK; /* volumen virgen */
  memcpy(fs->merkle_root, t1 + 24, 4u);
  uint32_t ep = mfs_ld32(t1 + 16);
  if (ep > fs->epoch)
    fs->epoch = ep;
  /* continuidad del contador de tokens (evita reprogramar sin erase) */
  fs->tok_seq_a = mfs_ld32(t1 + 20);
  return MFS_OK;
}
