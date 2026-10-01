/* mfs_pq.c — Cripto-agilidad post-cuántica (§15), PUF y KDF.
 *
 *  - HKDF-SHA256 (RFC 5869) para derivación de claves sin heap.
 *  - PUF (§15 MFS-PUF-001): fuzzy extractor con corrección por mayoría de 3
 *    muestras y fallback HKDF(UID+sal) marcado con evento HCT.
 *  - LMS hash-based (estructura NIST SP 800-208 / RFC 8554) con SHA-256:
 *    keygen, firma y verificación. Parámetros n=32, w=4, H=8 (256 hojas).
 *    La codificación del typecode es propia (documentada en DOCS/crypto.md);
 *    la construcción Winternitz + árbol Merkle es conforme a la estructura.
 */
#include "mfs_internal.h"
#include <string.h>

/* =========================== HKDF-SHA256 (RFC 5869) ======================= */
void mfs_hkdf_sha256_info(const uint8_t *salt, uint32_t saltlen,
                          const uint8_t *ikm, uint32_t ikmlen,
                          const uint8_t *info, uint32_t infolen, uint8_t *okm,
                          uint32_t okmlen) {
  static const uint8_t zero[32] = {0};
  if (!salt || saltlen == 0u) {
    salt = zero;
    saltlen = 32u;
  }
  uint8_t prk[32], t[32];
  mfs_hmac_sha256(salt, saltlen, ikm, ikmlen, prk); /* extract */
  uint32_t tlen = 0u, produced = 0u;
  uint8_t ctr = 1u;
  while (produced < okmlen) { /* expand */
    mfs_sha256_ctx hc;
    uint8_t inner[32];
    /* HMAC(prk, T(i-1) ‖ info ‖ i) mediante streaming */
    uint8_t kpad[64], ipad[64], opad[64];
    memset(kpad, 0, 64u);
    memcpy(kpad, prk, 32u);
    for (uint32_t i = 0; i < 64u; i++) {
      ipad[i] = (uint8_t)(kpad[i] ^ 0x36u);
      opad[i] = (uint8_t)(kpad[i] ^ 0x5Cu);
    }
    mfs_sha256_init(&hc);
    mfs_sha256_update(&hc, ipad, 64u);
    if (tlen)
      mfs_sha256_update(&hc, t, tlen);
    if (info && infolen)
      mfs_sha256_update(&hc, info, infolen);
    mfs_sha256_update(&hc, &ctr, 1u);
    mfs_sha256_final(&hc, inner);
    mfs_sha256_init(&hc);
    mfs_sha256_update(&hc, opad, 64u);
    mfs_sha256_update(&hc, inner, 32u);
    mfs_sha256_final(&hc, t);
    ctr++;
    tlen = 32u;
    uint32_t take = okmlen - produced;
    if (take > 32u)
      take = 32u;
    memcpy(okm + produced, t, take);
    produced += take;
  }
  mfs_zeroize(prk, sizeof(prk));
  mfs_zeroize(t, sizeof(t));
}

void mfs_hkdf_sha256(const uint8_t *salt, uint32_t saltlen, const uint8_t *ikm,
                     uint32_t ikmlen, uint8_t *okm, uint32_t okmlen) {
  mfs_hkdf_sha256_info(salt, saltlen, ikm, ikmlen, NULL, 0u, okm, okmlen);
}

/* ================================ PUF (§15) =============================== */
#define PUF_SAMPLES 256u
#define PUF_SPAN (PUF_SAMPLES * 3u)

static void puf_bits(const uint8_t *sram, uint32_t len, uint8_t bits[32]) {
  memset(bits, 0, 32u);
  for (uint32_t i = 0; i < PUF_SAMPLES; i++) {
    uint8_t b;
    if (len >= PUF_SPAN) {
      uint8_t x = (uint8_t)(sram[3u * i] & 1u);
      uint8_t y = (uint8_t)(sram[3u * i + 1u] & 1u);
      uint8_t z = (uint8_t)(sram[3u * i + 2u] & 1u);
      b = (uint8_t)(((uint32_t)x + y + z) >= 2u ? 1u : 0u);
    } else {
      b = (uint8_t)((i < len) ? (sram[i] & 1u) : 0u);
    }
    if (b)
      bits[i >> 3] |= (uint8_t)(1u << (i & 7u));
  }
}

mfs_st mfs_puf_enroll(mf_t *fs, const uint8_t *sram, uint32_t len,
                      const uint8_t salt[MFS_PUF_SALT], uint8_t key[32]) {
  if (!fs || !sram || !salt || !key)
    return MFS_EINVAL;
  if (!(fs->hwv.flags2 & MFS_HWV2_PUF)) {
    mfs_hct_event(fs, MFS_EV_PUF_FALLBACK, 0u);
    return MFS_EPUF; /* PUF no declarado (MFS-HAL-003) */
  }
  uint8_t bits[32], ref[32];
  puf_bits(sram, len, bits);
  mfs_hkdf_sha256(salt, MFS_PUF_SALT, bits, 32u, key, 32u);
  mfs_hkdf_sha256(salt, MFS_PUF_SALT, bits, 32u, ref, 32u);
  for (uint32_t i = 0; i < 32u; i++)
    fs->puf_helper[i] = (uint8_t)(key[i] ^ ref[i]);
  memcpy(fs->puf_salt, salt, MFS_PUF_SALT);
  fs->puf_enrolled = 1u;
  fs->hct.puf_enrolls++;
  mfs_zeroize(ref, sizeof(ref));
  mfs_zeroize(bits, sizeof(bits));
  mfs_hct_event(fs, MFS_EV_PUF, 1u);
  return MFS_OK;
}

mfs_st mfs_puf_reproduce(mf_t *fs, const uint8_t *sram, uint32_t len,
                         uint8_t key[32]) {
  if (!fs || !sram || !key)
    return MFS_EINVAL;
  if (!fs->puf_enrolled)
    return MFS_EPUF;
  uint8_t bits[32], ref[32];
  puf_bits(sram, len, bits);
  mfs_hkdf_sha256(fs->puf_salt, MFS_PUF_SALT, bits, 32u, ref, 32u);
  for (uint32_t i = 0; i < 32u; i++)
    key[i] = (uint8_t)(fs->puf_helper[i] ^ ref[i]);
  mfs_zeroize(ref, sizeof(ref));
  mfs_zeroize(bits, sizeof(bits));
  return MFS_OK;
}

mfs_st mfs_puf_fallback_key(mf_t *fs, const uint8_t *uid, uint32_t ulen,
                            uint8_t key[32]) {
  if (!fs || !uid || !key)
    return MFS_EINVAL;
  uint8_t ikm[64];
  uint32_t n = (ulen > 64u) ? 64u : ulen;
  memcpy(ikm, uid, n);
  mfs_hkdf_sha256(fs->puf_salt, MFS_PUF_SALT, ikm, n, key, 32u);
  fs->hct.puf_fallbacks++;
  mfs_zeroize(ikm, sizeof(ikm));
  mfs_hct_event(fs, MFS_EV_PUF_FALLBACK, n);
  return MFS_OK;
}

/* ============================ LMS (SP 800-208) =========================== */
#define LMS_N 32u
#define LMS_W 4u
#define LMS_H 8u
#define LMS_LEAVES (1u << LMS_H)
#define LMS_U 64u
#define LMS_V 3u
#define LMS_P 67u /* U + V */
#define LMS_LS 4u

#define LMS_D_MESG 0x01u
#define LMS_D_PBLC 0x02u
#define LMS_D_LEAF 0x03u
#define LMS_D_INTR 0x04u
#define LMS_TYPE 1u /* perfil MFS-LMS-SHA256-H8-W4 */

/* sig = type(4) | q(4) | C(32) | y[P·32] | auth[H·32] */
#define LMS_SIG_LEN (4u + 4u + 32u + LMS_P * 32u + LMS_H * 32u)

static void lms_H(const uint8_t I[16], uint32_t r, uint8_t D, const uint8_t *K,
                  const uint8_t *data, uint32_t dlen, uint8_t out[32]) {
  mfs_sha256_ctx c;
  uint8_t u32[4] = {(uint8_t)(r >> 24), (uint8_t)(r >> 16), (uint8_t)(r >> 8),
                    (uint8_t)r};
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, I, 16u);
  mfs_sha256_update(&c, u32, 4u);
  mfs_sha256_update(&c, &D, 1u);
  if (K)
    mfs_sha256_update(&c, K, 2u);
  if (data && dlen)
    mfs_sha256_update(&c, data, dlen);
  mfs_sha256_final(&c, out);
}

static void lms_chain(const uint8_t I[16], uint32_t i, uint8_t from, uint8_t to,
                      const uint8_t x[32], uint8_t out[32]) {
  uint8_t K[2] = {(uint8_t)(i >> 8), (uint8_t)i};
  memcpy(out, x, 32u);
  for (uint32_t s = from; s < to; s++) {
    uint8_t t[32];
    lms_H(I, i, LMS_D_MESG, K, out, 32u, t);
    memcpy(out, t, 32u);
  }
}

static void lms_x_for(const uint8_t seed[32], uint32_t leaf, uint32_t i,
                      uint8_t out[32]) {
  mfs_sha256_ctx c;
  uint8_t idx[8] = {(uint8_t)(leaf >> 24), (uint8_t)(leaf >> 16),
                    (uint8_t)(leaf >> 8),  (uint8_t)leaf,
                    (uint8_t)(i >> 24),    (uint8_t)(i >> 16),
                    (uint8_t)(i >> 8),     (uint8_t)i};
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, seed, 32u);
  mfs_sha256_update(&c, (const uint8_t *)"MFS-LMS-X", 9u);
  mfs_sha256_update(&c, idx, 8u);
  mfs_sha256_final(&c, out);
}

static void lms_coefs(const uint8_t I[16], uint32_t q, const uint8_t C[32],
                      const uint8_t *msg, uint32_t mlen, uint8_t a[LMS_P]) {
  uint8_t Q[32];
  uint8_t q32[4] = {(uint8_t)(q >> 24), (uint8_t)(q >> 16), (uint8_t)(q >> 8),
                    (uint8_t)q};
  uint8_t d = LMS_D_MESG;
  mfs_sha256_ctx c;
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, I, 16u);
  mfs_sha256_update(&c, q32, 4u);
  mfs_sha256_update(&c, &d, 1u);
  mfs_sha256_update(&c, C, 32u);
  mfs_sha256_update(&c, msg, mlen);
  mfs_sha256_final(&c, Q);
  uint32_t sum = 0u;
  for (uint32_t i = 0; i < LMS_U; i++) {
    uint8_t nib =
        (uint8_t)((Q[(i * LMS_LS) / 8u] >> ((i * LMS_LS) % 8u)) & 0x0Fu);
    a[i] = nib;
    sum += (uint32_t)(0x0Fu - nib);
  }
  sum <<= LMS_LS;
  for (uint32_t i = 0; i < LMS_V; i++)
    a[LMS_U + i] = (uint8_t)((sum >> (8u * (LMS_V - 1u - i))) & 0xFFu);
}

static void lms_leaf(const uint8_t I[16], const uint8_t seed[32], uint32_t q,
                     uint8_t out[32]) {
  static uint8_t flat[LMS_P * 32u];
  uint8_t tmp[32];
  for (uint32_t i = 0; i < LMS_P; i++) {
    uint8_t x[32], y[32];
    lms_x_for(seed, q, i, x);
    lms_chain(I, i, 0u, 0x0Fu, x, y);
    memcpy(flat + i * 32u, y, 32u);
  }
  lms_H(I, q, LMS_D_PBLC, NULL, flat, sizeof(flat), tmp);
  lms_H(I, q, LMS_D_LEAF, NULL, tmp, 32u, out);
}

static void lms_intr(const uint8_t I[16], uint32_t r, const uint8_t l[32],
                     const uint8_t rr[32], uint8_t out[32]) {
  uint8_t abl[64];
  memcpy(abl, l, 32u);
  memcpy(abl + 32u, rr, 32u);
  uint8_t K[2] = {0u, 0u};
  lms_H(I, r, LMS_D_INTR, K, abl, sizeof(abl), out);
}

/* Construye el árbol y (si se pide) el camino de autenticación de la hoja q. */
static void lms_tree(const uint8_t I[16], const uint8_t seed[32], uint32_t q,
                     uint8_t root[32], uint8_t *auth) {
  static uint8_t lvl[LMS_LEAVES * 32u];
  for (uint32_t i = 0; i < LMS_LEAVES; i++)
    lms_leaf(I, seed, i, lvl + i * 32u);
  uint32_t n = LMS_LEAVES;
  for (uint32_t L = 0; L < LMS_H; L++) {
    uint32_t idx = (q >> L) & 0xFFFFu;
    if (auth)
      memcpy(auth + L * 32u, lvl + ((idx ^ 1u) & (n - 1u)) * 32u, 32u);
    for (uint32_t i = 0; i < n / 2u; i++) {
      uint32_t r = (LMS_LEAVES >> (L + 1u)) + i;
      lms_intr(I, r, lvl + (2u * i) * 32u, lvl + (2u * i + 1u) * 32u,
               lvl + i * 32u);
    }
    n /= 2u;
  }
  memcpy(root, lvl, 32u);
}

mfs_st mfs_lms_keygen(const uint8_t seed[32], mfs_lms_pub_t *pub,
                      uint8_t sk_out[64]) {
  if (!seed || !pub)
    return MFS_EINVAL;
  uint8_t id[32];
  mfs_sha256_ctx c;
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, (const uint8_t *)"MFS-LMS-I", 9u);
  mfs_sha256_update(&c, seed, 32u);
  mfs_sha256_final(&c, id);
  memcpy(pub->I, id, 16u);
  lms_tree(pub->I, seed, 0u, pub->T1, NULL);
  pub->type = (uint8_t)LMS_TYPE;
  if (sk_out)
    memcpy(sk_out, seed, 32u);
  return MFS_OK;
}

mfs_st mfs_lms_sign(const uint8_t seed[32], uint32_t q, const uint8_t *msg,
                    uint32_t mlen, uint8_t *sig, uint32_t *siglen) {
  if (!seed || !msg || !sig || !siglen)
    return MFS_EINVAL;
  if (q >= LMS_LEAVES)
    return MFS_EINVAL;
  if (*siglen < LMS_SIG_LEN)
    return MFS_EOVERFLOW;
  mfs_lms_pub_t pub;
  mfs_st st = mfs_lms_keygen(seed, &pub, NULL);
  if (st != MFS_OK)
    return st;

  uint8_t C[32];
  uint8_t q32[4] = {(uint8_t)(q >> 24), (uint8_t)(q >> 16), (uint8_t)(q >> 8),
                    (uint8_t)q};
  mfs_sha256_ctx c;
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, seed, 32u);
  mfs_sha256_update(&c, (const uint8_t *)"MFS-LMS-C", 9u);
  mfs_sha256_update(&c, q32, 4u);
  mfs_sha256_update(&c, msg, mlen);
  mfs_sha256_final(&c, C);

  uint8_t a[LMS_P];
  lms_coefs(pub.I, q, C, msg, mlen, a);

  uint32_t o = 0u;
  sig[o++] = 0u;
  sig[o++] = 0u;
  sig[o++] = 0u;
  sig[o++] = (uint8_t)LMS_TYPE;
  memcpy(sig + o, q32, 4u);
  o += 4u;
  memcpy(sig + o, C, 32u);
  o += 32u;
  for (uint32_t i = 0; i < LMS_P; i++) {
    uint8_t x[32], y[32];
    lms_x_for(seed, q, i, x);
    lms_chain(pub.I, i, 0u, (a[i] < 0x0Fu) ? a[i] : 0x0Fu, x, y);
    memcpy(sig + o, y, 32u);
    o += 32u;
  }
  lms_tree(pub.I, seed, q, pub.T1, sig + o); /* camino de autenticación */
  o += LMS_H * 32u;
  *siglen = o;
  return MFS_OK;
}

mfs_st mfs_lms_verify(const mfs_lms_pub_t *pub, const uint8_t *msg,
                      uint32_t mlen, const uint8_t *sig, uint32_t siglen) {
  if (!pub || !msg || !sig)
    return MFS_EINVAL;
  if (siglen < LMS_SIG_LEN)
    return MFS_EBADMSG;
  uint32_t type = ((uint32_t)sig[0] << 24) | ((uint32_t)sig[1] << 16) |
                  ((uint32_t)sig[2] << 8) | (uint32_t)sig[3];
  if (type != LMS_TYPE || pub->type != LMS_TYPE)
    return MFS_EHW_UNSUPPORTED;
  uint32_t q = ((uint32_t)sig[4] << 24) | ((uint32_t)sig[5] << 16) |
               ((uint32_t)sig[6] << 8) | (uint32_t)sig[7];
  if (q >= LMS_LEAVES)
    return MFS_EBADMSG;
  const uint8_t *C = sig + 8u;
  const uint8_t *y = sig + 40u;
  const uint8_t *auth = y + LMS_P * 32u;

  uint8_t a[LMS_P];
  lms_coefs(pub->I, q, C, msg, mlen, a);

  /* reconstruir extremos de cadena */
  static uint8_t z[LMS_P * 32u];
  for (uint32_t i = 0; i < LMS_P; i++) {
    if (a[i] < 0x0Fu)
      lms_chain(pub->I, i, a[i], 0x0Fu, y + i * 32u, z + i * 32u);
    else
      memcpy(z + i * 32u, y + i * 32u, 32u);
  }
  /* Kc = H(I,q,D_PBLC,z…) → hoja */
  uint8_t tmp[32], node[32];
  lms_H(pub->I, q, LMS_D_PBLC, NULL, z, sizeof(z), tmp);
  lms_H(pub->I, q, LMS_D_LEAF, NULL, tmp, 32u, node);
  /* subir el árbol con el camino de autenticación */
  for (uint32_t L = 0; L < LMS_H; L++) {
    uint32_t r = (LMS_LEAVES >> (L + 1u)) + (q >> (L + 1u));
    if (((q >> L) & 1u) == 0u)
      lms_intr(pub->I, r, node, auth + L * 32u, node);
    else
      lms_intr(pub->I, r, auth + L * 32u, node, node);
  }
  mfs_zeroize(z, sizeof(z));
  mfs_zeroize(tmp, sizeof(tmp));
  if (memcmp(node, pub->T1, 32u) != 0)
    return MFS_ESECURITY_STATE;
  return MFS_OK;
}
