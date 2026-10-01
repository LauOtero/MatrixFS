/* mfs_ftl2.c — FTL Ultra 2 (§11): WOM-p, SLEC, EBA, RAS+TG, ELM/PEP, WEP, ZRP.
 *
 * Implementación determinista y sin heap (MFS-RES-001). Todos los
 * subsistemas exponen telemetría a HCT (§16) y respetan las puertas de
 * ELD (§13.3), clase RT (§13.1) y salud (§11.1).
 */
#include "mfs_internal.h"
#include <string.h>

/* =====================================================================
 * WOM-p (§11.4) — reescritura program-only con codificación bit-lane.
 *
 * Marco: [hdr 8 B][lane bytes]. Cada generación g (0..7) escribe el bit g de
 * cada byte de payload; sólo se aclaran bits (1→0), de modo que caben hasta
 * 8 generaciones por borrado (spec: 2 por defecto, 3 en NAND multinivel).
 * La generación vigente se codifica termométricamente en el header y nunca
 * se reescribe (1 bit por generación).
 * ===================================================================== */
#define WOM_HDR 8u

mfs_st mfs_wom_begin(mf_t *fs, uint32_t addr, uint16_t lane_bytes,
                     uint8_t max_gen, mfs_wom_t *w) {
  if (!fs || !w || lane_bytes == 0u)
    return MFS_EINVAL;
  if (max_gen == 0u || max_gen > 8u)
    max_gen = 2u;
  /* gate: WOM-p exige partial_page_program verificado (§11.4) */
  if (!(fs->hwv.flags1 & MFS_HWV1_PPP) &&
      !(fs->hwv.flags1 & MFS_HWV1_BYTE_ADDR))
    return MFS_EHW_UNSUPPORTED;
  memset(w, 0, sizeof(*w));
  w->addr = addr;
  w->len = lane_bytes;
  w->max_gen = max_gen;
  w->gen = 0u;
  w->live = 0u;
  return MFS_OK;
}

mfs_st mfs_wom_program(mf_t *fs, mfs_wom_t *w, const uint8_t *bits,
                       uint16_t nbits) {
  if (!fs || !w || !bits)
    return MFS_EINVAL;
  /* Bit-lane WOM: un byte de lane por bit de payload; la generación g usa el
   * bit g de cada byte. Capacidad: lane_bytes (en bits) ≥ nbits. */
  if (nbits > w->len)
    return MFS_EINVAL;
  if (w->gen >= w->max_gen)
    return MFS_EBACKPRESSURE; /* agotado: reclaim */

  uint16_t nbytes = nbits;
  static uint8_t lane[WOM_MAX_LANE];
  memset(lane, 0xFFu, nbytes);
  for (uint16_t i = 0; i < nbits; i++) {
    if (((bits[i >> 3] >> (i & 7u)) & 1u) == 0u)
      continue; /* 0 ⇒ no programar */
    lane[i] &= (uint8_t)~(1u << w->gen);
  }
  uint32_t base = w->addr + WOM_HDR;
  /* programar sólo los bytes cuyo valor difiere (NOR: 1→0) */
  static uint8_t cur[WOM_MAX_LANE];
  mfs_st st = mfs_read(fs, base, cur, nbytes);
  if (st != MFS_OK)
    return st;
  for (uint16_t i = 0; i < nbytes; i++) {
    /* sólo se aclaran bits (1→0): las generaciones previas ya aclararon el
     * bit de su propia generación, que aquí debe preservarse */
    uint8_t val = (uint8_t)(lane[i] & cur[i]);
    if (cur[i] == val)
      continue;
    st = mfs_write(fs, base + i, &val, 1u);
    if (st != MFS_OK)
      return st;
  }
  /* marca termométrica de generación en el header (byte 0, bit gen) */
  uint8_t mark = 0xFFu;
  st = mfs_read(fs, w->addr, &mark, 1u);
  if (st != MFS_OK)
    return st;
  mark &= (uint8_t)~(1u << w->gen);
  st = mfs_write(fs, w->addr, &mark, 1u);
  if (st != MFS_OK)
    return st;
  w->gen++;
  w->live = 1u;
  mfs_hct_event(fs, MFS_EV_WOM_PROG, ((uint32_t)w->gen << 16) | w->len);
  return MFS_OK;
}

mfs_st mfs_wom_read(mf_t *fs, const mfs_wom_t *w, uint8_t *bits,
                    uint16_t nbits) {
  if (!fs || !w || !bits)
    return MFS_EINVAL;
  if (nbits > w->len)
    return MFS_EINVAL;
  uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
  static uint8_t lane[WOM_MAX_LANE];
  mfs_st st = mfs_read(fs, w->addr + WOM_HDR, lane, nbits);
  if (st != MFS_OK)
    return st;
  uint8_t g = (w->gen == 0u) ? 0u : (uint8_t)(w->gen - 1u); /* última escrita */
  memset(bits, 0, nbytes);
  for (uint16_t i = 0; i < nbits; i++)
    if (!((lane[i] >> g) & 1u))
      bits[i >> 3] |= (uint8_t)(1u << (i & 7u));
  return MFS_OK;
}

bool mfs_wom_exhausted(const mfs_wom_t *w) {
  return !w || w->gen >= w->max_gen;
}

/* =====================================================================
 * SLEC (§11.7) — ventana SLC dinámica para WAL/metadatos calientes.
 * Reserva una zona como SLC; el plegado a MLC/TLC se hace en idle bajo ELD.
 * ===================================================================== */
bool mfs_slec_enabled(mf_t *fs) {
  if (!fs)
    return false;
  uint32_t t = fs->hwv.media_type;
  bool mlc = (t == MFS_MEDIA_NAND_RAW || t == MFS_MEDIA_NAND_ONFI ||
              t == MFS_MEDIA_ZNS_NAND);
  /* sólo con NAND multinivel y sin T0 (con FRAM/MRAM no hace falta) */
  return mlc && !mfs_has_t0(fs) &&
         mfs_mode_classic_ge(fs->mode, MFS_MODE_COMPACT);
}

mfs_st mfs_slec_enter(mf_t *fs, uint32_t zone) {
  if (!fs)
    return MFS_EINVAL;
  if (!mfs_slec_enabled(fs))
    return MFS_ENOTSUP;
  mfs_zone_t *z = mfs_zone(fs, zone);
  if (!z)
    return MFS_EINVAL;
  fs->slec_zone = (uint16_t)zone;
  fs->slec_pages = 0u;
  mfs_hct_event(fs, MFS_EV_SLEC, zone);
  return MFS_OK;
}

mfs_st mfs_slec_fold(mf_t *fs, uint32_t budget_us) {
  if (!fs || !mfs_slec_enabled(fs))
    return MFS_ENOTSUP;
  if (fs->edp_level >= 4u)
    return MFS_EAGAIN; /* nunca en RT-A/EDP */
  if (fs->eld_budget_mj && fs->eld_spent_mj > fs->eld_budget_mj)
    return MFS_EENERGY; /* MFS-ELD-001 */
  uint32_t t0 = mfs_port_time_us();
  /* plegar ≤1 página por slice (§11.7) */
  while (fs->slec_pages > 0u && mfs_port_time_us() - t0 < budget_us) {
    fs->slec_pages--;
    fs->hct.slec_folded++;
    if (mfs_port_time_us() - t0 >= budget_us)
      break;
    break; /* ≤1 bloque/ventana */
  }
  if (fs->slec_zone != 0xFFFFu && fs->slec_pages == 0u) {
    mfs_zone_t *z = mfs_zone(fs, fs->slec_zone);
    if (z && z->state != MFS_Z_EMPTY)
      mfs_hct_event(fs, MFS_EV_SLEC, 0x8000u | fs->slec_zone);
  }
  return MFS_OK;
}

/* =====================================================================
 * EBA (§11.8) — ECC adaptativa por región; BER_región < capacidad/2.
 * ===================================================================== */
static const uint16_t eba_cap_x1e6[MFS_ECC_LDPC + 1u] = {
    0u, 3906u, 15625u, 31250u, 65535u /* ~1, ~4, ~8 bits por 256 B */
};

mfs_ecc_t mfs_eba_select(const mfs_hwv_t *hwv, uint32_t region) {
  (void)region;
  if (hwv && (hwv->flags0 & MFS_HWV0_ECC_ON_DIE))
    return MFS_ECC_LDPC;
  if (hwv && hwv->oob_bytes >= 64u)
    return MFS_ECC_BCH8;
  if (hwv && hwv->oob_bytes >= 16u)
    return MFS_ECC_BCH4;
  return MFS_ECC_SECDED;
}

void mfs_eba_report(mf_t *fs, uint32_t region, uint16_t ber_x1e6) {
  if (!fs || region >= MFS_EBA_MAX_REGIONS)
    return;
  /* EMA α = 1/8 para seguimiento determinista */
  uint32_t prev = fs->eba_ber[region];
  uint32_t next = (prev * 7u + ber_x1e6) / 8u;
  fs->eba_ber[region] = (uint16_t)((next > 0xFFFFu) ? 0xFFFFu : next);
  mfs_ecc_t cap = mfs_eba_select(&fs->hwv, region);
  if ((uint32_t)fs->eba_ber[region] > (uint32_t)eba_cap_x1e6[cap]) {
    fs->eba_violations++;
    mfs_hct_event(fs, MFS_EV_EBA,
                  ((uint32_t)region << 16) | fs->eba_ber[region]);
  }
}

bool mfs_eba_should_scrub(const mf_t *fs, uint32_t region) {
  if (!fs || region >= MFS_EBA_MAX_REGIONS)
    return false;
  mfs_ecc_t cap = mfs_eba_select(&fs->hwv, region);
  return (uint32_t)fs->eba_ber[region] > (uint32_t)eba_cap_x1e6[cap] / 2u;
}

uint16_t mfs_eba_capacity(mfs_ecc_t ecc) {
  return (ecc <= MFS_ECC_LDPC) ? eba_cap_x1e6[ecc] : 0u;
}

/* =====================================================================
 * RAS + Thermal Governor (§11.9)
 * ===================================================================== */
static uint16_t tg_retention_factor(int16_t temp_c) {
  /* tabla de perfil (§25 / MFS-TG-001): retención relativa por temperatura,
   * en centésimas respecto a la retención nominal (25 °C = 1,00×). */
  if (temp_c <= 0)
    return 200u; /* frío extremo: 2,00× */
  if (temp_c <= 25)
    return 100u; /* nominal: 1,00× */
  if (temp_c <= 55)
    return 100u;
  if (temp_c <= 85)
    return 50u;
  if (temp_c <= 105)
    return 25u;
  return 10u;
}

void mfs_tg_set_temp(mf_t *fs, int16_t temp_c) {
  if (!fs)
    return;
  fs->tg_temp_c = temp_c;
  if (temp_c >= 85)
    fs->tg_state = MFS_TG_HOT;
  else if (temp_c <= 0)
    fs->tg_state = MFS_TG_COLD;
  else
    fs->tg_state = MFS_TG_NOMINAL;
  mfs_hct_event(fs, MFS_EV_TG, (uint32_t)(uint16_t)temp_c);
}

uint8_t mfs_tg_scrub_urgency(const mf_t *fs, uint32_t age_s,
                             uint32_t retention_spec_s) {
  if (!fs)
    return 0u;
  uint16_t f = tg_retention_factor(fs->tg_temp_c); /* centésimas */
  uint32_t eff = (uint32_t)((uint64_t)retention_spec_s * f / 100u);
  if (eff == 0u)
    eff = 1u;
  if ((uint64_t)age_s * 2u >= (uint64_t)eff * 2u)
    return 3u; /* edad > retención */
  if ((uint64_t)age_s * 2u >= (uint64_t)eff)
    return 2u; /* edad > 0,5× */
  if (fs->tg_state != MFS_TG_NOMINAL)
    return 1u;
  return 0u;
}

mfs_st mfs_tg_step(mf_t *fs, uint32_t budget_us) {
  if (!fs)
    return MFS_EINVAL;
  if (fs->edp_level >= 4u)
    return MFS_EAGAIN; /* nunca en RT-A */
  if (!mfs_gc_rt_idle(fs))
    return MFS_EAGAIN; /* sólo ventanas idle (SPDR) */
  if (fs->eld_budget_mj && fs->eld_spent_mj > fs->eld_budget_mj) {
    fs->hct.tg_deferred++;
    return MFS_EENERGY;
  }
  uint32_t t0 = mfs_port_time_us();
  if (fs->tg_state == MFS_TG_HOT) {
    /* extremo térmico: aplazar programaciones no-RT (§11.9 MFS-TG-001) */
    fs->hct.tg_deferred++;
    return MFS_EAGAIN;
  }
  if (mfs_port_time_us() - t0 > budget_us)
    return MFS_ETIMEDOUT_BUDGET;
  fs->hct.scrub_done++;
  mfs_hct_event(fs, MFS_EV_TG, 0x100u | (uint32_t)fs->tg_state);
  return MFS_OK;
}

void mfs_tg_read_disturb(mf_t *fs, uint32_t block, uint32_t reads) {
  if (!fs || block >= MFS_MAX_ZONES)
    return;
  fs->tg_reads[block] += reads;
  if (fs->tg_reads[block] > fs->tg_disturb_threshold) {
    fs->tg_reads[block] = 0u;
    fs->hct.read_retry++;
    mfs_hct_event(fs, MFS_EV_TG, 0x200u | block);
  }
}

/* =====================================================================
 * ELM + PEP (§11.1) — modelo de vida determinista + refinamiento RT-C.
 * Pesos ELM fijos (0,5 / 0,3 / 0,2). PEP: 4 perceptrones de 32 entradas
 * cuantizadas a 8 bits, voto por mayoría, WCET < 5 µs; ELM prevalece.
 * ===================================================================== */

/* activación cuantizada: 0..255 → centiles 0..100 */
static uint8_t pep_act(int32_t acc) {
  if (acc <= 0)
    return 0u;
  if (acc >= 25500)
    return 100u;
  return (uint8_t)(acc / 255);
}

static int32_t pep_layer(const int8_t *w, const uint16_t *feat, uint32_t n) {
  int32_t acc = 0;
  for (uint32_t i = 0; i < n; i++)
    acc += (int32_t)w[i] * (int32_t)(feat[i] & 0xFFu);
  return acc;
}

uint16_t mfs_pep_score(const uint16_t feat[MFS_PEP_FEATURES]) {
  /* 4 perceptrones × 32 características (pesos ROM < 1 KB, §11.1) */
  static const int8_t W0[MFS_PEP_FEATURES] = {2, 2,  1, 1,  0, 0, 1, 1, 2, 1, 1,
                                              0, -1, 0, 1,  2, 1, 1, 0, 0, 1, 1,
                                              2, 1,  0, -1, 0, 1, 1, 2, 1, 1};
  static const int8_t W1[MFS_PEP_FEATURES] = {1, 1, 2, 2, 1, 0, 0, 1, 1,  2, 1,
                                              1, 0, 0, 1, 1, 2, 1, 0, -1, 1, 2,
                                              1, 0, 1, 0, 1, 2, 2, 1, 0,  0};
  static const int8_t W2[MFS_PEP_FEATURES] = {0, 1, 1, 2, 2, 1, 0, 0, 1, 1, 2,
                                              2, 1, 0, 0, 1, 1, 2, 2, 1, 0, 0,
                                              1, 1, 2, 1, 0, 0, 1, 1, 2, 1};
  static const int8_t W3[MFS_PEP_FEATURES] = {1, 0, 0, 1,  1, 2, 2, 1, 0, 0, 1,
                                              2, 2, 1, 0,  0, 1, 1, 2, 2, 1, 0,
                                              1, 2, 1, -1, 1, 1, 2, 2, 1, 0};
  const int8_t *W[4] = {W0, W1, W2, W3};
  uint8_t votes = 0u;
  for (int p = 0; p < 4; p++)
    if (pep_act(pep_layer(W[p], feat, MFS_PEP_FEATURES)) >= 50u)
      votes++;
  return (uint16_t)((votes >= 2u) ? 100u : 0u); /* voto por mayoría */
}

/* ELM: salud = 0,5·(1−PE/PE_max) + 0,3·(1−BER/BER_fallo) + 0,2·(1−tendencia) */
uint16_t mfs_elm_health(uint16_t pe, uint16_t pe_max, uint16_t ber,
                        uint16_t ber_fail, uint16_t trend) {
  if (pe_max == 0u)
    pe_max = 1u;
  if (ber_fail == 0u)
    ber_fail = 1u;
  if (trend > 1000u)
    trend = 1000u;
  uint32_t f1 = (pe_max > pe) ? ((uint32_t)(pe_max - pe) * 1000u / pe_max) : 0u;
  uint32_t f2 =
      (ber_fail > ber) ? ((uint32_t)(ber_fail - ber) * 1000u / ber_fail) : 0u;
  uint32_t f3 = 1000u - trend;
  return (uint16_t)((5u * f1 + 3u * f2 + 2u * f3) / 10u); /* 0..1000 */
}

/* RUL ≈ (BER_fallo − BER) / pendiente_EMA(BER); α = 1/32 (§11.1) */
uint32_t mfs_elm_rul(uint32_t ber, uint32_t ber_fail, uint32_t slope_x1000) {
  if (slope_x1000 == 0u)
    return 0xFFFFFFFFu; /* estable */
  if (ber >= ber_fail)
    return 0u;
  return (uint32_t)(((uint64_t)(ber_fail - ber) * 1000u) / slope_x1000);
}

void mfs_elm_ema(mf_t *fs, uint32_t ber) {
  if (!fs)
    return;
  /* EMA con α = 1/32 en punto fijo (Q16). determinista y sin libm */
  uint32_t prev = fs->elm_ber_ema_q16;
  uint32_t x_q16 = ber << 16;
  uint32_t next = prev + (x_q16 - prev) / 32u;
  fs->elm_ber_ema_q16 = next;
}

void mfs_elm_proactive_trigger(mf_t *fs, uint32_t zone, uint32_t rul,
                               uint32_t rul_mean) {
  if (!fs)
    return;
  /* disparo proactivo si RUL(b) < 0,2 × RUL_medio (§11.1) */
  if (rul_mean != 0u && rul * 5u < rul_mean) {
    fs->hct.elm_proactive++;
    mfs_hct_event(fs, MFS_EV_ELM, ((uint32_t)zone << 16) | (rul & 0xFFFFu));
  }
}

/* =====================================================================
 * WEP (§11.2) — Feistel 16 bits, 4 rondas, claves del UID; caché 16 en
 * Balanced+ (recálculo directo en modos bajos, coste RAM cero).
 * ===================================================================== */
uint16_t mfs_wep_feistel(uint16_t x, uint16_t k1, uint16_t k2) {
  /* F(x) = ((x·K1) ^ (x>>3))·K2  (aritmética mód 2^16) */
  uint16_t f = (uint16_t)(((uint16_t)(x * k1)) ^ (uint16_t)(x >> 3));
  return (uint16_t)(f * k2);
}

uint16_t mfs_wep_place(mf_t *fs, uint16_t logical) {
  if (!fs)
    return logical;
  uint16_t k1 = fs->wep_k1, k2 = fs->wep_k2;
  if (k1 == 0u && k2 == 0u)
    return logical; /* WEP deshabilitado */
  /* 4 rondas de Feistel balanceado de 8+8 bits */
  uint16_t l = (uint16_t)(logical >> 8), r = (uint16_t)(logical & 0xFFu);
  for (uint32_t round = 0; round < 4u; round++) {
    uint16_t f =
        (uint16_t)(mfs_wep_feistel((uint16_t)(r ^ (uint16_t)round), k1, k2) &
                   0xFFu);
    uint16_t nl = (uint16_t)(r & 0xFFu);
    uint16_t nr = (uint16_t)((l ^ f) & 0xFFu);
    l = nl;
    r = nr;
  }
  uint16_t out = (uint16_t)((l << 8) | r);
  if (fs->wep_cache_valid) {
    for (uint32_t i = 0; i < MFS_WEP_CACHE; i++) {
      if (fs->wep_cache[i].logical == logical)
        return fs->wep_cache[i].placed;
    }
    /* insertar con reemplazo determinista round-robin */
    fs->wep_cache[fs->wep_rr % MFS_WEP_CACHE].logical = logical;
    fs->wep_cache[fs->wep_rr % MFS_WEP_CACHE].placed = out;
    fs->wep_rr++;
  }
  return out;
}

void mfs_wep_init(mf_t *fs, const uint8_t uid[8]) {
  if (!fs)
    return;
  /* claves derivadas del UID (MFS-SEC-002: no persistidas) */
  uint16_t a = (uint16_t)((uid[0] << 8) | uid[1]);
  uint16_t b = (uint16_t)((uid[2] << 8) | uid[3]);
  uint16_t c = (uint16_t)((uid[4] << 8) | uid[5]);
  uint16_t d = (uint16_t)((uid[6] << 8) | uid[7]);
  fs->wep_k1 = (uint16_t)(a ^ c ^ 0x5A5Au);
  fs->wep_k2 = (uint16_t)(b ^ d ^ 0xA5A5u);
  if (fs->wep_k1 == 0u)
    fs->wep_k1 = 0x1111u;
  if (fs->wep_k2 == 0u)
    fs->wep_k2 = 0x2222u;
  fs->wep_cache_valid = mfs_mode_classic_ge(fs->mode, MFS_MODE_BALANCED);
  fs->wep_rr = 0u;
  memset(fs->wep_cache, 0, sizeof(fs->wep_cache));
}

/* =====================================================================
 * ZRP (§8.5) — Zone Rescue Parity: RS(16,15) sobre GF(2⁸) por grupo de 16
 * páginas; 1 página de paridad por grupo (+6,25 %), sólo zonas frías.
 * ===================================================================== */
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool gf_ready;

void mfs_gf_init(void) {
  if (gf_ready)
    return;
  uint16_t x = 1u;
  for (uint32_t i = 0; i < 255u; i++) {
    gf_exp[i] = (uint8_t)x;
    gf_log[x] = (uint8_t)i;
    x <<= 1;
    if (x & 0x100u)
      x ^= 0x11Du; /* polinomio primitivo */
  }
  for (uint32_t i = 255u; i < 512u; i++)
    gf_exp[i] = gf_exp[i - 255u];
  gf_log[0] = 0u;
  gf_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
  if (a == 0u || b == 0u)
    return 0u;
  return gf_exp[(uint32_t)gf_log[a] + gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b) {
  if (b == 0u)
    return 0u;
  if (a == 0u)
    return 0u;
  int d = (int)gf_log[a] - (int)gf_log[b];
  if (d < 0)
    d += 255;
  return gf_exp[d];
}

mfs_st mfs_zrp_encode(mf_t *fs, mfs_zone_t *z) {
  if (!fs || !z)
    return MFS_EINVAL;
  if (!fs->cfg->zrp_enable)
    return MFS_ENOTSUP; /* off por defecto */
  if (z->class_hot < 2u)
    return MFS_EINVAL; /* sólo cold/archive */
  if (z->total_pages < 17u)
    return MFS_ENOTSUP; /* 16 datos + paridad */
  mfs_gf_init();
  uint32_t pb = mfs_page_bytes(fs);
  static uint8_t parity[MFS_SCRATCH_MAX];
  memset(parity, 0, pb);
  uint8_t alpha = 1u;
  for (uint32_t p = 0; p < 16u; p++) {
    static uint8_t page[MFS_SCRATCH_MAX];
    mfs_st st =
        mfs_read(fs, z->start_addr + MFS_ZONEHDR_SIZE + p * pb, page, pb);
    if (st != MFS_OK)
      return st;
    for (uint32_t b = 0; b < pb; b++)
      parity[b] ^= gf_mul(page[b], alpha);
    alpha = gf_mul(alpha, 2u);
  }
  mfs_st st =
      mfs_write(fs, z->start_addr + MFS_ZONEHDR_SIZE + 16u * pb, parity, pb);
  if (st == MFS_OK) {
    z->zrp = 1u;
    fs->hct.zrp_encoded++;
    mfs_hct_event(fs, MFS_EV_ZRP, z->zone_id);
  }
  return st;
}

/* Reconstruye la página `idx` (0..15) del grupo usando la paridad (§12 nivel
 * 5). La reconstrucción se escribe en `dst_page` (relocalización): sobre NVM no
 * es posible reprogramar 0→1 en la página dañada, por lo que el rescate
 * consiste en regenerar el contenido y ubicarlo en una página virgen. */
mfs_st mfs_zrp_recover(mf_t *fs, mfs_zone_t *z, uint32_t idx,
                       uint32_t dst_page) {
  if (!fs || !z || idx >= 16u)
    return MFS_EINVAL;
  if (!z->zrp)
    return MFS_ENOTSUP;
  if (dst_page < 17u || dst_page >= z->total_pages)
    return MFS_EINVAL;
  mfs_gf_init();
  uint32_t pb = mfs_page_bytes(fs);
  static uint8_t parity[MFS_SCRATCH_MAX];
  static uint8_t page[MFS_SCRATCH_MAX];
  static uint8_t acc[MFS_SCRATCH_MAX];
  mfs_st st =
      mfs_read(fs, z->start_addr + MFS_ZONEHDR_SIZE + 16u * pb, parity, pb);
  if (st != MFS_OK)
    return st;
  memset(acc, 0, pb);
  uint8_t alpha = 1u;
  for (uint32_t p = 0; p < 16u; p++) {
    if (p == idx) {
      alpha = gf_mul(alpha, 2u);
      continue;
    }
    st = mfs_read(fs, z->start_addr + MFS_ZONEHDR_SIZE + p * pb, page, pb);
    if (st != MFS_OK)
      return st;
    for (uint32_t b = 0; b < pb; b++)
      acc[b] ^= gf_mul(page[b], alpha);
    alpha = gf_mul(alpha, 2u);
  }
  /* alpha_idx = 2^idx */
  uint8_t a_idx = 1u;
  for (uint32_t i = 0; i < idx; i++)
    a_idx = gf_mul(a_idx, 2u);
  for (uint32_t b = 0; b < pb; b++)
    page[b] = gf_div((uint8_t)(parity[b] ^ acc[b]), a_idx);
  st =
      mfs_write(fs, z->start_addr + MFS_ZONEHDR_SIZE + dst_page * pb, page, pb);
  if (st == MFS_OK) {
    z->zrp_dst = (uint8_t)dst_page; /* página relocalizada */
    fs->hct.zrp_recovered++;
    mfs_hct_event(fs, MFS_EV_ZRP, 0x8000u | (uint32_t)idx);
  }
  return st;
}

/* =====================================================================
 * Política combinada ELM + PEP para selección de víctima GC (§11.1)
 * Devuelve el índice de zona elegido; ELM es el baseline y prevalece ante
 * conflicto con PEP (evento registrado).
 * ===================================================================== */
int mfs_gc_select_victim_ftl(mf_t *fs, uint32_t *elm_out, uint32_t *pep_out) {
  if (!fs)
    return -1;
  int best_elm = -1;
  uint32_t best_elm_score = 0xFFFFFFFFu;
  int best_pep = -1;
  uint32_t best_pep_score = 0xFFFFFFFFu;
  uint32_t rul_sum = 0, rul_n = 0;
  for (uint32_t i = 0; i < fs->zone_cap; i++) {
    mfs_zone_t *z = &fs->zones[i];
    if (z->state != MFS_Z_FULL)
      continue;
    uint32_t pe = z->pe_cycles, pe_max = z->pe_max ? z->pe_max : 1u;
    uint32_t ber = z->ber_x1e6, ber_fail = 100000u; /* 10 % */
    uint32_t effort =
        mfs_elm_health((uint16_t)pe, (uint16_t)pe_max, (uint16_t)ber,
                       (uint16_t)ber_fail, (uint16_t)z->trend);
    uint32_t util = (uint32_t)z->valid_pages * 1000u /
                    (z->total_pages ? z->total_pages : 1u);
    /* coste estimado de reclamación: poco válido + mucha salud ⇒ barato */
    uint32_t score = util + (1000u - effort); /* menor = mejor */
    if (score < best_elm_score) {
      best_elm_score = score;
      best_elm = (int)i;
    }
    /* RUL y disparo proactivo */
    uint32_t slope = z->ber_slope_x1000;
    uint32_t rul = mfs_elm_rul(ber, ber_fail, slope);
    if (rul != 0xFFFFFFFFu) {
      rul_sum += (rul > 0xFFFFu ? 0xFFFFu : rul);
      rul_n++;
    }
    /* PEP sólo en RT-C sin rt_strict (§11.1) */
    if (mfs_mode_classic_ge(fs->mode, MFS_MODE_EXTENDED) &&
        !fs->cfg->rt_strict && fs->edp_level == 0u) {
      uint16_t feat[MFS_PEP_FEATURES];
      memset(feat, 0, sizeof(feat));
      feat[0] = (uint16_t)(pe & 0xFFu);
      feat[1] = (uint16_t)(ber & 0xFFu);
      feat[2] = (uint16_t)z->valid_pages;
      feat[3] = (uint16_t)z->total_pages;
      feat[4] = (uint16_t)(z->seq & 0xFFu);
      feat[5] = 100u; /* hotness cold */
      uint16_t ps = mfs_pep_score(feat);
      uint32_t pscore = score + (ps ? 0u : 200u);
      if (pscore < best_pep_score) {
        best_pep_score = pscore;
        best_pep = (int)i;
      }
    }
  }
  if (elm_out)
    *elm_out = (best_elm >= 0) ? best_elm_score : 0xFFFFFFFFu;
  if (pep_out)
    *pep_out = (best_pep >= 0) ? best_pep_score : 0xFFFFFFFFu;
  if (rul_n > 0u) {
    uint32_t rul_mean = rul_sum / rul_n;
    if (best_elm >= 0)
      mfs_elm_proactive_trigger(
          fs, fs->zones[best_elm].zone_id,
          mfs_elm_rul(fs->zones[best_elm].ber_x1e6, 100000u,
                      fs->zones[best_elm].ber_slope_x1000),
          rul_mean);
  }
  /* ELM prevalece ante conflicto; el desacuerdo de PEP se registra */
  if (best_pep >= 0 && best_pep != best_elm) {
    fs->hct.pep_overrides++;
    mfs_hct_event(fs, MFS_EV_PEP,
                  ((uint32_t)best_pep << 16) | (uint32_t)best_elm);
  }
  return best_elm;
}
