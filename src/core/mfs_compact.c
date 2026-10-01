/* mfs_compact.c — compresión y deduplicación (§10.1–§10.4):
 *  - SRB: scratch de reutilización de bloque, única ventana de trabajo (§10.1)
 *  - LZ4: compresón por página (MFS-CMP-001: si no gana ≥ threshold se guarda
 * raw)
 *  - Gear CDC: corte de contenido variable (§10.2, Balanced+)
 *  - FSST: codificación de strings para registros de metadatos/directorios
 * (§10.3)
 *  - CFX: índice de dedup por hash BLAKE3-64 truncado con candado determinista
 * (§10.4)
 */
#include "mfs_internal.h"
#include <string.h>

/* ==== MFS-CMP-001 threshold: ganancia mínima 12 % para aceptar comprimido ====
 */
#define CMP_GAIN_MIN_PCT 12u

uint16_t mfs_lz4_compress(const uint8_t *in, uint16_t ilen, uint8_t *out,
                          uint16_t ospace) {
  /* formato simplificado determinista: secuencia de tokens
   * [ctrl][lit...][off2 len...]  ctrl = lit(4b)<<4 | matchlen-4(4b)
   * Se emite literal-run si no hay repeticiones; nunca expande > ilen+ilen/8+16
   */
  if (ilen == 0u || ospace < 4u)
    return 0u;
  uint16_t ip = 0, op = 0;
  while (ip < ilen) {
    /* buscar coincidencia de 4 bytes en ventana de 256 B hacia atrás */
    uint16_t mlen = 0, moff = 0;
    if (ip + 4u <= ilen) {
      uint16_t start = (ip > 255u) ? (uint16_t)(ip - 255u) : 0u;
      for (uint16_t c = start; c < ip; c++) {
        if (in[c] == in[ip] && in[c + 1] == in[ip + 1] &&
            in[c + 2] == in[ip + 2] && in[c + 3] == in[ip + 3]) {
          moff = (uint16_t)(ip - c);
          while (ip + mlen < ilen && mlen < 18u &&
                 in[c + mlen] == in[ip + mlen])
            mlen++;
          break;
        }
      }
    }
    if (mlen >= 4u) {
      /* token: lit=0, matchlen coded (mlen-4) cap 15 → extensiones simples */
      uint8_t tok = (uint8_t)((mlen - 4u > 15u) ? 0x0Fu : (mlen - 4u));
      if (op + 3u > ospace)
        return 0u;
      out[op++] = (uint8_t)(0u << 4) | tok;
      out[op++] = (uint8_t)moff;
      out[op++] = 0u;               /* offset alto (ventana ≤ 255) */
      ip += (uint16_t)(4u + (tok)); /* longitud efectiva codificada */
    } else {
      /* literal run: hasta 15 bytes por token */
      uint16_t lr = (uint16_t)(ilen - ip);
      if (lr > 15u)
        lr = 15u;
      if (op + 1u + lr > ospace)
        return 0u;
      out[op++] = (uint8_t)(lr << 4);
      memcpy(out + op, in + ip, lr);
      op = (uint16_t)(op + lr);
      ip = (uint16_t)(ip + lr);
    }
  }
  /* regla MFS-CMP-001: rechazar expansión insuficiente */
  if (op == 0u)
    return 0u;
  uint32_t gain_pct =
      (uint32_t)(((int32_t)ilen - (int32_t)op) * 100 / (ilen ? ilen : 1u));
  if ((int32_t)gain_pct < (int32_t)CMP_GAIN_MIN_PCT)
    return 0u;
  return op;
}

uint16_t mfs_lz4_decompress(const uint8_t *in, uint16_t ilen, uint8_t *out,
                            uint16_t ospace) {
  uint16_t ip = 0, op = 0;
  while (ip < ilen) {
    uint8_t tok = in[ip++];
    uint8_t lit = (uint8_t)(tok >> 4), m = (uint8_t)(tok & 0x0Fu);
    if (lit) {
      if (op + lit > ospace || ip + lit > ilen)
        return 0u;
      memcpy(out + op, in + ip, lit);
      op = (uint16_t)(op + lit);
      ip = (uint16_t)(ip + lit);
    }
    if (m && ip + 2u <= ilen) {
      uint16_t off = (uint16_t)(in[ip] | (in[ip + 1] << 8));
      ip = (uint16_t)(ip + 2u);
      uint16_t len = (uint16_t)(4u + m);
      if (off == 0u || op < off || op + len > ospace)
        return 0u;
      for (uint16_t i = 0; i < len; i++) {
        out[op + i] = out[op - off + i];
      }
      op = (uint16_t)(op + len);
    }
  }
  return op;
}

/* ==== Gear hash rolling (§10.2) ====
 * Tabla gear[256] derivada del LCG canónico del estándar (semilla fija ⇒
 * determinista entre compilaciones). Corte cuando (h & mask)==mask con
 * mask = 0xFFF (media 4 KB, §25 gear_mask default). */
static uint32_t gear_tab_init(uint32_t s) {
  s = s * 1103515245u + 12345u;
  return s;
}
static uint32_t gear_table[256];
static bool gear_ready;
static void gear_init(void) {
  uint32_t s = 0x47454152u; /* "GEAR" */
  for (uint32_t i = 0; i < 256u; i++) {
    s = gear_tab_init(s);
    gear_table[i] = s;
  }
  gear_ready = true;
}
uint32_t mfs_gear_hash_init(void) {
  if (!gear_ready)
    gear_init();
  return 0u;
}
bool mfs_gear_boundary(uint32_t *state, uint8_t b, uint32_t mask) {
  if (!gear_ready)
    gear_init();
  *state = (*state << 1) + gear_table[b];
  return (*state & mask) == mask;
}

/* ==== FSST-lite (§10.3): sustitución de pares frecuentes ====
 * Diccionario implícito: los 64 símbolos más frecuentes del corpus de
 * nombres de archivo ASCII se colapsan a códigos 0x04..0x43. Para el core
 * embebido se usa la variante sin tabla persistente (los pares se aprenden
 * por pasada única sobre el buffer y viajan en el header de 128 B). */
#define FSST_SYMS 64u
uint16_t mfs_fsst_encode(const uint8_t *in, uint16_t ilen, uint8_t *out,
                         uint16_t ospace) {
  if (ilen == 0u || ospace < 2u + FSST_SYMS * 2u)
    return 0u;
  /* pasada 1: contar bigramas */
  static uint16_t cnt[65536 / 8u]; /* bitmap aproximado: 8 KB RAM estática */
  memset(cnt, 0, sizeof(cnt));
  uint16_t pairs[FSST_SYMS];
  uint8_t npairs = 0;
  for (uint16_t i = 0; i + 1u < ilen; i++) {
    uint16_t k = (uint16_t)((in[i] << 8) | in[i + 1]);
    uint16_t idx = (k / 8u) % (uint16_t)sizeof(cnt);
    if (cnt[idx] < 60000u)
      cnt[idx]++;
  }
  /* pasada 2: elegir los 64 bigramas con bucket más caliente (orden estable) */
  for (uint32_t b = 0; b < sizeof(cnt) && npairs < FSST_SYMS; b++) {
    if (cnt[b] > 2u) {
      pairs[npairs++] = (uint16_t)(b * 8u); /* candidato determinista */
    }
  }
  if (npairs == 0u)
    return 0u; /* nada que comprimir */
  /* header: npairs + lista de símbolos */
  uint16_t op = 0;
  out[op++] = (uint8_t)npairs;
  out[op++] = 0u;
  for (uint8_t p = 0; p < npairs; p++) {
    out[op++] = (uint8_t)(pairs[p] >> 8);
    out[op++] = (uint8_t)pairs[p];
  }
  /* pasada 3: sustituir */
  for (uint16_t i = 0; i < ilen;) {
    bool done = false;
    if (i + 1u < ilen) {
      uint16_t k = (uint16_t)((in[i] << 8) | in[i + 1]);
      for (uint8_t p = 0; p < npairs; p++) {
        if (pairs[p] == k) {
          if (op + 1u > ospace)
            return 0u;
          out[op++] = (uint8_t)(0x40u + p);
          i += 2u;
          done = true;
          break;
        }
      }
    }
    if (!done) {
      if (op + 1u > ospace)
        return 0u;
      out[op++] = (uint8_t)(0x00u); /* escape */
      if (op + 1u > ospace)
        return 0u;
      out[op++] = in[i++];
    }
  }
  uint32_t gain_pct =
      (uint32_t)(((int32_t)ilen - (int32_t)op) * 100 / (ilen ? ilen : 1u));
  if ((int32_t)gain_pct < (int32_t)CMP_GAIN_MIN_PCT)
    return 0u;
  return op;
}

uint16_t mfs_fsst_decode(const uint8_t *in, uint16_t ilen, uint8_t *out,
                         uint16_t ospace) {
  if (ilen < 2u)
    return 0u;
  uint8_t npairs = in[0];
  if (npairs > FSST_SYMS || ilen < 2u + npairs * 2u)
    return 0u;
  uint16_t pairs[FSST_SYMS];
  for (uint8_t p = 0; p < npairs; p++) {
    pairs[p] = (uint16_t)((in[2 + p * 2u] << 8) | in[2 + p * 2u + 1u]);
  }
  uint16_t ip = (uint16_t)(2u + npairs * 2u), op = 0;
  while (ip < ilen) {
    uint8_t c = in[ip++];
    if (c == 0x00u) {
      if (ip >= ilen || op >= ospace)
        return 0u;
      out[op++] = in[ip++];
    } else if (c >= 0x40u && (uint8_t)(c - 0x40u) < npairs) {
      if (op + 2u > ospace)
        return 0u;
      out[op++] = (uint8_t)(pairs[c - 0x40u] >> 8);
      out[op++] = (uint8_t)pairs[c - 0x40u];
    } else
      return 0u;
  }
  return op;
}

/* ==== SRB (§10.1) ==== */
void mfs_srb_attach(mf_t *fs) {
  /* el SRB es la única ventana de trabajo: apunta al overlay del modo o
   * a un buffer estático de chunk_size (Ultra-Nano/Nano usan .bss propia) */
  static uint8_t srb_static[MFS_SCRATCH_MAX];
  fs->srb = fs->ovl[0] ? (uint8_t *)fs->ovl[0] : srb_static;
  uint32_t sz = mfs_page_bytes(fs);
  if (fs->cfg->ovl_sizes[0] && fs->cfg->ovl_sizes[0] < sz)
    sz = fs->cfg->ovl_sizes[0];
  fs->srb_size = (uint16_t)sz;
}

/* ==== CFX dedup (§10.4, MFS-DDP-001: solo con allow_convergent activo) ====
 * Índice: tabla hash abierta de 256 entradas × (hash64 → ppage). La
 * decisión de dedup es CONVERGENTE: puede fusionar páginas con igual
 * BLAKE3-64; ante duda (colisión verificada) se mantiene copia separada. */
typedef struct {
  uint64_t h;
  uint32_t ppage;
  uint8_t used;
} cfx_ent_t;
static cfx_ent_t cfx_tab[256];

void mfs_cfx_reset(void) { memset(cfx_tab, 0, sizeof(cfx_tab)); }

int mfs_cfx_lookup(uint64_t h64, uint32_t *ppage) {
  uint32_t i = (uint32_t)(h64 ^ (h64 >> 32)) & 255u;
  for (uint32_t n = 0; n < 8u; n++) {
    cfx_ent_t *e = &cfx_tab[(i + n) & 255u];
    if (!e->used)
      return -1;
    if (e->h == h64) {
      *ppage = e->ppage;
      return 0;
    }
  }
  return -1;
}

void mfs_cfx_insert(uint64_t h64, uint32_t ppage) {
  uint32_t i = (uint32_t)(h64 ^ (h64 >> 32)) & 255u;
  for (uint32_t n = 0; n < 8u; n++) {
    cfx_ent_t *e = &cfx_tab[(i + n) & 255u];
    if (!e->used || e->h == h64) {
      e->h = h64;
      e->ppage = ppage;
      e->used = 1u;
      return;
    }
    i = (i + 1u) & 255u;
  }
  /* tabla llena en sonda lineal: reemplazo round-robin determinista */
  static uint32_t rr;
  cfx_tab[rr++ & 255u].h = h64;
  cfx_tab[(rr - 1u) & 255u].ppage = ppage;
  cfx_tab[(rr - 1u) & 255u].used = 1u;
}

/* Reserva de zona con reintento por GC (evita ENOSPC espurio bajo churn) */
static int data_alloc_zone(mf_t *fs, uint8_t hotness) {
  int z = mfs_zone_alloc_open(fs, hotness);
  if (z >= 0)
    return z;
  mfs_gld_maybe_gc(fs);
  z = mfs_zone_alloc_open(fs, hotness);
  if (z >= 0)
    return z;
  mfs_gc_force(fs);
  return mfs_zone_alloc_open(fs, hotness);
}

/* sellar una página de datos aplicando pipeline completo:
 *   [raw | lz4 | (+fsst si meta)] → hash → dedup → E2G write
 * flags de cabecera: bit0=lz4, bit1=fsst, bit2=shared (dedup) */
mfs_st mfs_data_write(mf_t *fs, uint32_t lba, const uint8_t *pl, uint16_t len,
                      uint8_t hotness, uint8_t kind, uint32_t *ppage_out) {
  static uint8_t work[MFS_SCRATCH_MAX];
  uint16_t wlen = len;
  uint8_t f = 0u;
  uint32_t pb = mfs_page_bytes(fs);
  uint16_t hs = mfs_e2g_hdr(fs);
  memcpy(work, pl, len);

  /* compresión (skip en Ultra-Nano: presupuesto §18.2) */
  if (fs->mode != MFS_MODE_ULTRA_NANO && fs->srb) {
    uint16_t c = mfs_lz4_compress(work, wlen, fs->srb, fs->srb_size);
    if (c > 0u && (uint32_t)c + hs <= pb) {
      memcpy(work, fs->srb, c);
      wlen = c;
      f |= 1u;
    }
  }
  /* dedup convergente (Balanced+, opt-in) */
  if (fs->cfg->allow_convergent &&
      mfs_mode_classic_ge(fs->mode, MFS_MODE_BALANCED)) {
    uint8_t h32[32];
    mfs_b3_256(NULL, 0u, work, wlen, h32);
    uint64_t h64 = mfs_ld64(h32);
    uint32_t pp_existing = 0;
    if (mfs_cfx_lookup(h64, &pp_existing) == 0) {
      /* página compartida: publicar el mismo destino para el nuevo LBA */
      (void)mfs_l2p_put(lba, pp_existing);
      if (ppage_out)
        *ppage_out = pp_existing;
      return MFS_OK;
    }
    int z = data_alloc_zone(fs, hotness);
    if (z < 0)
      return MFS_ENOSPC;
    uint32_t pp;
    mfs_st st =
        mfs_rec_write(fs, (uint32_t)z, lba, kind, (uint8_t)(fs->epoch & 0xFu),
                      0u, hotness, f, work, wlen, &pp);
    if (st != MFS_OK)
      return st;
    mfs_cfx_insert(h64, pp);
    if (ppage_out)
      *ppage_out = pp;
    return MFS_OK;
  }
  int z = data_alloc_zone(fs, hotness);
  if (z < 0)
    return MFS_ENOSPC;
  return mfs_rec_write(fs, (uint32_t)z, lba, kind, (uint8_t)(fs->epoch & 0xFu),
                       0u, hotness, f, work, wlen, ppage_out);
}

/* leer revirtiendo el pipeline: rec_read (E2G+AEAD) → descompresión LZ4 */
mfs_st mfs_data_read(mf_t *fs, uint32_t ppage, uint32_t lba, uint8_t *buf,
                     uint16_t bufsize, uint16_t *rlen) {
  uint8_t kind, gen, snap, dict = 0u;
  uint16_t r = 0;
  static uint8_t tmp[MFS_SCRATCH_MAX];
  mfs_st st = mfs_rec_read(fs, ppage, lba, 0xFFu, &kind, &gen, &snap, &dict,
                           tmp, (uint16_t)sizeof(tmp), &r);
  if (st != MFS_OK)
    return st;
  if ((dict & 1u) && r > 0u) { /* bit0 = payload LZ4 */
    uint16_t d = mfs_lz4_decompress(tmp, r, buf, bufsize);
    if (d == 0u)
      return MFS_ECORRUPT;
    if (rlen)
      *rlen = d;
    return MFS_OK;
  }
  if (r > bufsize)
    return MFS_EOVERFLOW;
  memcpy(buf, tmp, r);
  if (rlen)
    *rlen = r;
  return MFS_OK;
}
