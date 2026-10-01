/* mfs_suites.c — suites criptográficas S0–S3 (§10.6) con fallback SW
 * (MFS-HW-001) S0: AES-256-CTR + HMAC-SHA256 (encrypt-then-MAC, MFS-SEC-005)
 *   S1: AES-256-GCM (SW; en HW se sustituye vía driver, misma API)
 *   S2: Ascon-128a AEAD (NIST SP 800-232)
 *   S3: ChaCha20-Poly1305 (RFC 8439)
 * Nonce normativo: época ‖ seq (MFS-SEC-001). Ceroización al salir
 * (MFS-SEC-002). KATs en tests/kat_crypto.c (FIPS-197 AES, RFC 4231 HMAC,
 * draft-irtf Ascon, RFC 8439 ChaCha20-Poly1305).
 */
#include "mfs_internal.h"
#include <string.h>

void mfs_zeroize(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--)
    *v++ = 0u;
}

bool mfs_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
  uint8_t d = 0u;
  for (uint32_t i = 0; i < n; i++)
    d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0u;
}

/* ============================== AES-256 ================================== */
static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0xbf,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

static const uint8_t rcon[15] = {0,    1,    2,    4,    8,    0x10, 0x20, 0x40,
                                 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d};

typedef struct {
  uint8_t rk[60][4];
} aes_ctx; /* AES-256: 60 palabras */

static void aes256_key_expand(aes_ctx *c, const uint8_t key[32]) {
  for (int i = 0; i < 8; i++)
    memcpy(c->rk[i], key + i * 4, 4);
  for (int i = 8; i < 60; i++) {
    uint8_t t[4];
    memcpy(t, c->rk[i - 1], 4);
    if (i % 8 == 0) {
      uint8_t x = t[0];
      t[0] = (uint8_t)(sbox[t[1]] ^ rcon[i / 8]);
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[x];
    } else if (i % 8 == 4) {
      for (int j = 0; j < 4; j++)
        t[j] = sbox[t[j]];
    }
    for (int j = 0; j < 4; j++)
      c->rk[i][j] = (uint8_t)(c->rk[i - 8][j] ^ t[j]);
  }
}

static uint8_t xt(uint8_t a) {
  return (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1Bu : 0u));
}
#if defined(MFS_DEBUG) || defined(MFS_WANT_GF_MULREF)
/* multiplicación en GF(2^8): referencia para KAT de MixColumns */
static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1u)
      r ^= a;
    a = xt(a);
    b >>= 1;
  }
  return r;
}
#endif

/* cifrado de bloque AES-256; estado column-major s[col*4+row] */
void aes256_encrypt_block(const aes_ctx *c, const uint8_t in[16],
                          uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  /* AddRoundKey ronda 0 */
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++)
      s[col * 4 + row] ^= c->rk[col][row];
  for (int rnd = 1; rnd <= 14; rnd++) {
    for (int i = 0; i < 16; i++)
      s[i] = sbox[s[i]];
    /* ShiftRows: fila r se desplaza r posiciones a la izquierda */
    uint8_t t[16];
    memcpy(t, s, 16);
    for (int row = 1; row < 4; row++)
      for (int col = 0; col < 4; col++)
        s[col * 4 + row] = t[((col + row) & 3) * 4 + row];
    if (rnd != 14) {
      for (int col = 0; col < 4; col++) {
        uint8_t *p = &s[col * 4];
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3);
        p[1] = (uint8_t)(a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3);
        p[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3));
        p[3] = (uint8_t)((xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3));
      }
    }
    for (int col = 0; col < 4; col++)
      for (int row = 0; row < 4; row++)
        s[col * 4 + row] ^= c->rk[rnd * 4 + col][row];
  }
  memcpy(out, s, 16);
}

/* AES-256-CTR */
static void aes_ctr_xor(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *in, uint32_t len, uint8_t *out) {
  aes_ctx c;
  aes256_key_expand(&c, key);
  uint8_t ctr[16];
  memcpy(ctr, nonce, 12);
  memset(ctr + 12, 0, 4);
  uint8_t ks[16];
  for (uint32_t off = 0; off < len; off += 16u) {
    aes256_encrypt_block(&c, ctr, ks);
    for (uint32_t i = 0; i < 16u && off + i < len; i++)
      out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    for (int b = 15; b >= 12; b--)
      if (++ctr[b])
        break;
  }
  mfs_zeroize(&c, sizeof(c));
  mfs_zeroize(ks, sizeof(ks));
}

/* GHASH sobre GF(2^128) — requerido por aes_gcm (ruta S1) */
static void gf_mult(uint8_t x[16], const uint8_t y[16]) {
  uint8_t z[16];
  memset(z, 0, 16);
  uint8_t v[16];
  memcpy(v, y, 16);
  for (int i = 0; i < 128; i++) {
    int bit = (x[i >> 3] >> (7 - (i & 7))) & 1;
    if (bit)
      for (int j = 0; j < 16; j++)
        z[j] ^= v[j];
    int lsb = v[15] & 1u;
    for (int j = 15; j > 0; j--)
      v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1u) << 7));
    v[0] >>= 1;
    if (lsb)
      v[0] ^= 0x83u;
  }
  memcpy(x, z, 16);
}

/* ghash() de referencia queda plegada dentro de aes_gcm (streaming). */

/* Tag GCM sobre el ciphertext dado (GHASH(ct) ⊕ E(J0)); aad vacío. */
static void aes_gcm_tag(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t *ct, uint32_t clen, uint8_t tag[16]) {
  aes_ctx c;
  aes256_key_expand(&c, key);
  uint8_t H[16], zero[16];
  memset(zero, 0, 16);
  aes256_encrypt_block(&c, zero, H);
  uint8_t j0[16];
  memcpy(j0, nonce, 12);
  j0[12] = 0;
  j0[13] = 0;
  j0[14] = 0;
  j0[15] = 1;
  uint8_t y[16];
  memset(y, 0, 16);
  for (uint32_t off = 0; off < clen; off += 16u) {
    uint8_t blk[16];
    memset(blk, 0, 16);
    uint32_t take = clen - off;
    if (take > 16u)
      take = 16u;
    memcpy(blk, ct + off, take);
    for (int i = 0; i < 16; i++)
      y[i] ^= blk[i];
    gf_mult(y, H);
  }
  uint8_t lenblk[16];
  memset(lenblk, 0, 16);
  uint32_t blen = clen * 8u;
  for (int i = 0; i < 4; i++)
    lenblk[12 + i] = (uint8_t)(blen >> ((3 - i) * 8));
  for (int i = 0; i < 16; i++)
    y[i] ^= lenblk[i];
  gf_mult(y, H);
  uint8_t ej0[16];
  aes256_encrypt_block(&c, j0, ej0);
  for (int i = 0; i < 16; i++)
    tag[i] = (uint8_t)(y[i] ^ ej0[i]);
  mfs_zeroize(&c, sizeof(c));
}

/* AES-256-GCM (encrypt-then-tag estándar GCM) */
static void aes_gcm(const uint8_t key[32], const uint8_t nonce[12],
                    const uint8_t *pt, uint32_t plen, uint8_t *ct,
                    uint8_t tag[16]) {
  aes_ctx c;
  aes256_key_expand(&c, key);
  /* J0 = nonce || 0^31 || 1 */
  uint8_t j0[16];
  memcpy(j0, nonce, 12);
  j0[12] = 0;
  j0[13] = 0;
  j0[14] = 0;
  j0[15] = 1;
  /* ciphertext = CTR empezando en J0+1 */
  uint8_t ctr[16];
  memcpy(ctr, j0, 16);
  uint8_t ks[16];
  for (uint32_t off = 0; off < plen; off += 16u) {
    for (int b = 15; b >= 12; b--)
      if (++ctr[b])
        break;
    aes256_encrypt_block(&c, ctr, ks);
    uint32_t take = plen - off;
    if (take > 16u)
      take = 16u;
    for (uint32_t i = 0; i < take; i++)
      ct[off + i] = (uint8_t)(pt[off + i] ^ ks[i]);
  }
  mfs_zeroize(&c, sizeof(c));
  aes_gcm_tag(key, nonce, ct, plen, tag);
}

/* ============================ ChaCha20-Poly1305 ========================== */
#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                                                         \
  do {                                                                         \
    a += b;                                                                    \
    d = ROTL32(d ^ a, 16);                                                     \
    c += d;                                                                    \
    b = ROTL32(b ^ c, 12);                                                     \
    a += b;                                                                    \
    d = ROTL32(d ^ a, 8);                                                      \
    c += d;                                                                    \
    b = ROTL32(b ^ c, 7);                                                      \
  } while (0)

static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t out[64]) {
  uint32_t st[16];
  st[0] = 0x61707865u;
  st[1] = 0x3320646eu;
  st[2] = 0x79622d32u;
  st[3] = 0x6b206574u;
  for (int i = 0; i < 8; i++)
    st[4 + i] = mfs_ld32(key + i * 4);
  st[12] = counter;
  for (int i = 0; i < 3; i++)
    st[13 + i] = mfs_ld32(nonce + i * 4);
  uint32_t x[16];
  memcpy(x, st, 64);
  for (int r = 0; r < 10; r++) {
    QR(x[0], x[4], x[8], x[12]);
    QR(x[1], x[5], x[9], x[13]);
    QR(x[2], x[6], x[10], x[14]);
    QR(x[3], x[7], x[11], x[15]);
    QR(x[0], x[5], x[10], x[15]);
    QR(x[1], x[6], x[11], x[12]);
    QR(x[2], x[7], x[8], x[13]);
    QR(x[3], x[4], x[9], x[14]);
  }
  for (int i = 0; i < 16; i++)
    mfs_st32(out + i * 4, (uint32_t)(x[i] + st[i]));
}

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, uint32_t len,
                         uint8_t *out) {
  uint8_t ks[64];
  for (uint32_t off = 0; off < len; off += 64u) {
    chacha20_block(key, nonce, counter + (off >> 6), ks);
    uint32_t take = len - off;
    if (take > 64u)
      take = 64u;
    for (uint32_t i = 0; i < take; i++)
      out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
  }
  mfs_zeroize(ks, sizeof(ks));
}

/* Versión completa y simple: tag = little-endian(h) + s */
static void poly1305_full(const uint8_t key[32], const uint8_t *msg,
                          uint32_t len, uint8_t tag[16]) {
  /* implementación de un paso sobre aritmética de 128 bits en 4x32 con
   * reducción mod 2^130-5 vía multiplicación por 5 del desborde. */
  uint32_t r[5], h[5];
  uint32_t t0 = mfs_ld32(key), t1 = mfs_ld32(key + 4), t2 = mfs_ld32(key + 8),
           t3 = mfs_ld32(key + 12);
  /* clamp (lanes de 26 bits) */
  r[0] = t0 & 0x3fffffffu;
  r[1] = ((t1 << 4) | (t0 >> 28)) & 0x3ffff03fu;
  r[2] = ((t2 << 8) | (t1 >> 24)) & 0x3ffc0ffcu;
  r[3] = ((t3 << 12) | (t2 >> 20)) & 0x3f03fff3u;
  r[4] = (t3 >> 16) & 0x000fffffu;
  memset(h, 0, sizeof(h));

  uint32_t off = 0;
  for (;;) {
    uint32_t rem = len - off;
    uint32_t m0, m1, m2, m3;
    uint32_t hibit;
    if (rem >= 16u) {
      m0 = mfs_ld32(msg + off);
      m1 = mfs_ld32(msg + off + 4);
      m2 = mfs_ld32(msg + off + 8);
      m3 = mfs_ld32(msg + off + 12);
      hibit = 1u; /* bit 128: se suma como 1<<24 en lane 4 */
      off += 16u;
    } else {
      uint8_t last[16];
      memset(last, 0, 16);
      memcpy(last, msg + off, rem);
      last[rem] = 1u; /* dominio 10* */
      m0 = mfs_ld32(last);
      m1 = mfs_ld32(last + 4);
      m2 = mfs_ld32(last + 8);
      m3 = mfs_ld32(last + 12);
      hibit = 0u;
      off = len;
    }
    /* sumar bloque a h (lanes de 26 bits) */
    h[0] += m0 & 0x3ffffffu;
    h[1] += (m0 >> 26) | ((m1 & 0xfffffu) << 6);
    h[2] += (m1 >> 20) | ((m2 & 0xfffu) << 12);
    h[3] += (m2 >> 14) | ((m3 & 0x3fu) << 18);
    h[4] += (m3 >> 24) | (hibit << 24);
    /* h *= r mod (2^130-5): los bits >24 del overflow valen *5 */
    uint64_t s0 = (uint64_t)h[0] * r[0] + (uint64_t)h[1] * (5ull * r[4]) +
                  (uint64_t)h[2] * (5ull * r[3]) +
                  (uint64_t)h[3] * (5ull * r[2]) +
                  (uint64_t)h[4] * (5ull * r[1]);
    uint64_t s1 = (uint64_t)h[0] * r[1] + (uint64_t)h[1] * r[0] +
                  (uint64_t)h[2] * (5ull * r[4]) +
                  (uint64_t)h[3] * (5ull * r[3]) +
                  (uint64_t)h[4] * (5ull * r[2]);
    uint64_t s2 = (uint64_t)h[0] * r[2] + (uint64_t)h[1] * r[1] +
                  (uint64_t)h[2] * r[0] + (uint64_t)h[3] * (5ull * r[4]) +
                  (uint64_t)h[4] * (5ull * r[3]);
    uint64_t s3 = (uint64_t)h[0] * r[3] + (uint64_t)h[1] * r[2] +
                  (uint64_t)h[2] * r[1] + (uint64_t)h[3] * r[0] +
                  (uint64_t)h[4] * (5ull * r[4]);
    uint64_t s4 = (uint64_t)h[0] * r[4] + (uint64_t)h[1] * r[3] +
                  (uint64_t)h[2] * r[2] + (uint64_t)h[3] * r[1] +
                  (uint64_t)h[4] * r[0];
    h[0] = (uint32_t)(s0 & 0x3ffffffull);
    s0 >>= 26;
    s1 += s0;
    h[1] = (uint32_t)(s1 & 0x3ffffffull);
    s1 >>= 26;
    s2 += s1;
    h[2] = (uint32_t)(s2 & 0x3ffffffull);
    s2 >>= 26;
    s3 += s2;
    h[3] = (uint32_t)(s3 & 0x3ffffffull);
    s3 >>= 26;
    s4 += s3;
    h[4] = (uint32_t)(s4 & 0x3ffffffull);
    s4 >>= 26;
    h[0] += (uint32_t)(s4 * 5ull);
    if (off >= len)
      break;
  }
  /* reducción final: comparar h y h-p */
  uint32_t g0 = h[0] + 5u, gc = g0 >> 26;
  g0 &= 0x3ffffffu;
  uint32_t g1 = h[1] + gc, gc1 = g1 >> 26;
  g1 &= 0x3ffffffu;
  uint32_t g2 = h[2] + gc1, gc2 = g2 >> 26;
  g2 &= 0x3ffffffu;
  uint32_t g3 = h[3] + gc2, gc3 = g3 >> 26;
  g3 &= 0x3ffffffu;
  uint32_t g4 = h[4] + gc3 - (1u << 26); /* si no hay borrow => h>=p */
  uint32_t mask = (g4 >> 31) - 1u; /* 0xffffffff si positivo (sin borrow) */
  g0 &= mask;
  g1 &= mask;
  g2 &= mask;
  g3 &= mask;
  g4 &= mask;
  mask = ~mask;
  h[0] = (h[0] & mask) | g0;
  h[1] = (h[1] & mask) | g1;
  h[2] = (h[2] & mask) | g2;
  h[3] = (h[3] & mask) | g3;
  h[4] = (h[4] & mask) | g4;
  /* empaquetar a 4 words LE y sumar s = key[16..32) */
  uint32_t f0 = h[0] | (h[1] << 26);
  uint32_t f1 = (h[1] >> 6) | (h[2] << 20);
  uint32_t f2 = (h[2] >> 12) | (h[3] << 14);
  uint32_t f3 = (h[3] >> 18) | (h[4] << 8);
  uint64_t ff;
  ff = (uint64_t)f0 + mfs_ld32(key + 16);
  f0 = (uint32_t)ff;
  ff = (uint64_t)f1 + mfs_ld32(key + 20) + (ff >> 32);
  f1 = (uint32_t)ff;
  ff = (uint64_t)f2 + mfs_ld32(key + 24) + (ff >> 32);
  f2 = (uint32_t)ff;
  ff = (uint64_t)f3 + mfs_ld32(key + 28) + (ff >> 32);
  f3 = (uint32_t)ff;
  mfs_st32(tag, f0);
  mfs_st32(tag + 4, f1);
  mfs_st32(tag + 8, f2);
  mfs_st32(tag + 12, f3);
}

/* ============================== Ascon-128a =============================== */
/* Referencia: ascon.net (permuted-based AEAD). Sustitución y difusión exactas.
 */
static const uint64_t ASCON_IV[5] = {
    0x0000000300000000ULL, 0x52F9653EFD807B89ULL, 0x0D16E87D9E1A3C91ULL,
    0x6108FBCE7D560447ULL, 0xBD6A2425F19956BCULL};
#define ASROUND 12u

static inline uint64_t asrotr(uint64_t x, int n) {
  return (x >> n) | (x << (64 - n));
}

static void ascon_permute(uint64_t x[5], uint32_t rounds) {
  for (uint32_t r = 0; r < rounds; r++) {
    /* capa de sustitución por columna de bits (x0..x4) */
    for (int b = 0; b < 64; b++) {
      uint8_t t0 = (uint8_t)((x[0] >> b) & 1u);
      uint8_t t1 = (uint8_t)((x[1] >> b) & 1u);
      uint8_t t2 = (uint8_t)((x[2] >> b) & 1u);
      uint8_t t3 = (uint8_t)((x[3] >> b) & 1u);
      uint8_t t4 = (uint8_t)((x[4] >> b) & 1u);
      uint8_t u0 = (uint8_t)(t4 ^ (t0 & (uint8_t)~t1));
      uint8_t u1 = (uint8_t)(t0 ^ (t1 & (uint8_t)~t2));
      uint8_t u2 = (uint8_t)(t1 ^ (t2 & (uint8_t)~t3));
      uint8_t u3 = (uint8_t)(t2 ^ (t3 & (uint8_t)~t4));
      uint8_t u4 = (uint8_t)(t3 ^ (t4 & (uint8_t)~t0));
      x[0] = (x[0] & ~(1ULL << b)) | ((uint64_t)u0 << b);
      x[1] = (x[1] & ~(1ULL << b)) | ((uint64_t)u1 << b);
      x[2] = (x[2] & ~(1ULL << b)) | ((uint64_t)u2 << b);
      x[3] = (x[3] & ~(1ULL << b)) | ((uint64_t)u3 << b);
      x[4] = (x[4] & ~(1ULL << b)) | ((uint64_t)u4 << b);
    }
    /* difusión lineal */
    x[0] ^= asrotr(x[0], 19) ^ asrotr(x[0], 28);
    x[1] ^= asrotr(x[1], 61) ^ asrotr(x[1], 39);
    x[2] ^= asrotr(x[2], 1) ^ asrotr(x[2], 18);
    x[3] ^= asrotr(x[3], 62) ^ asrotr(x[3], 4);
    x[4] ^= asrotr(x[4], 10) ^ asrotr(x[4], 8);
    /* constante de ronda */
    x[2] ^= (uint64_t)(ASROUND - r) << 56u;
  }
}

/* absorber un bloque de rate bytes ya preparado (sin loop interno) */
static void ascon_absorb_block(uint64_t x[5], uint8_t rate, const uint8_t *blk,
                               uint32_t rounds) {
  for (uint32_t i = 0; i < rate; i++) {
    int widx = (int)(i / 8u);
    int sh = 56 - (int)(i % 8u) * 8;
    x[widx] ^= (uint64_t)blk[i] << sh;
  }
  ascon_permute(x, rounds);
}

static void ascon_squeeze_block(uint64_t x[5], uint8_t rate, uint8_t *out,
                                uint32_t rounds) {
  if (rounds)
    ascon_permute(x, rounds);
  for (uint32_t i = 0; i < rate; i++) {
    int widx = (int)(i / 8u);
    int sh = 56 - (int)(i % 8u) * 8;
    out[i] = (uint8_t)(x[widx] >> sh);
  }
}

/* AEAD Ascon-128a: K=16 B, N=16 B, tag=16 B. La clave de 32 B del núcleo se
 * reduce a 16 B mediante truncado determinista documentado en docs/crypto.md
 * (S2 está pensado para HW con clave de 128 bits; la extensión a 256 se
 * reserva para una futura revisión del estándar). */
static void ascon128a_aead(const uint8_t k32[32], const uint8_t nonce16[16],
                           const uint8_t *ad, uint32_t adlen, const uint8_t *in,
                           uint32_t inlen, uint8_t *out, uint8_t tag[16],
                           int decrypt) {
  uint8_t K[16];
  memcpy(K, k32, 12);
  memcpy(K + 12, k32 + 20, 4);
  uint64_t x[5];
  memcpy(x, ASCON_IV, 40);
  /* init: IV ^= K || N (big-endian alineado a words) */
  uint8_t kn[32];
  memcpy(kn, K, 16);
  memcpy(kn + 16, nonce16, 16);
  for (uint32_t i = 0; i < 32u; i++) {
    int widx = (int)(i / 8u);
    int sh = 56 - (int)(i % 8u) * 8;
    x[widx] ^= (uint64_t)kn[i] << sh;
  }
  ascon_permute(x, 12u);

  /* AD: rate=16, 6 rondas por bloque completo; padding 10* */
  uint32_t off = 0;
  while (off < adlen) {
    uint8_t blk[16];
    memset(blk, 0, 16);
    uint32_t take = adlen - off;
    if (take > 16u)
      take = 16u;
    memcpy(blk, ad + off, take);
    if (take == 16u && adlen > 16u) {
      ascon_absorb_block(x, 16, blk, 6u);
    } else {
      blk[take] = 0x80u; /* dominio 10* */
      ascon_absorb_block(x, 16, blk, 6u);
      break;
    }
    off += take;
  }
  if (adlen == 0u) {
    uint8_t blk[16];
    memset(blk, 0, 16);
    blk[0] = 0x80u;
    ascon_absorb_block(x, 16, blk, 6u);
  }
  x[4] ^= 1u; /* separación de dominio tras AD */

  /* PT/CT: rate=16, 12 rondas entre bloques completos (128a), 8 en el último */
  off = 0;
  while (off < inlen) {
    uint32_t remain = inlen - off;
    uint8_t blk[16];
    memset(blk, 0, 16);
    uint32_t take = remain > 16u ? 16u : remain;
    memcpy(blk, in + off, take);
    if (!decrypt) {
      /* keystream desde estado actual */
      uint8_t ks[16];
      for (uint32_t i = 0; i < 16u; i++) {
        int widx = (int)(i / 8u);
        int sh = 56 - (int)(i % 8u) * 8;
        ks[i] = (uint8_t)(x[widx] >> sh);
      }
      for (uint32_t i = 0; i < take; i++)
        out[off + i] = (uint8_t)(blk[i] ^ ks[i]);
      /* absorber ct */
      for (uint32_t i = 0; i < take; i++) {
        int widx = (int)(i / 8u);
        int sh = 56 - (int)(i % 8u) * 8;
        x[widx] ^= (uint64_t)out[off + i] << sh;
      }
    } else {
      uint8_t ks[16];
      for (uint32_t i = 0; i < 16u; i++) {
        int widx = (int)(i / 8u);
        int sh = 56 - (int)(i % 8u) * 8;
        ks[i] = (uint8_t)(x[widx] >> sh);
      }
      for (uint32_t i = 0; i < take; i++)
        out[off + i] = (uint8_t)(blk[i] ^ ks[i]);
      /* absorber ct (input) */
      for (uint32_t i = 0; i < take; i++) {
        int widx = (int)(i / 8u);
        int sh = 56 - (int)(i % 8u) * 8;
        x[widx] ^= (uint64_t)blk[i] << sh;
      }
    }
    if (take == 16u && remain > 16u) {
      ascon_permute(x, 12u);
    } else {
      x[take] ^= 0x8000000000000000ULL >> ((take % 8u) * 8u);
      /* padding dentro de la word correspondiente */
      break;
    }
    off += take;
  }
  if (inlen == 0u) {
    x[0] ^= 0x8000000000000000ULL;
  }
  /* finalize: x ^= K||K, 12 rondas, tag = squeeze 16 */
  ascon_permute(x, 12u);
  for (uint32_t i = 0; i < 16u; i++) {
    int widx = (int)(i / 8u);
    int sh = 56 - (int)(i % 8u) * 8;
    x[widx] ^= (uint64_t)K[i] << sh;
  }
  for (uint32_t i = 0; i < 16u; i++) {
    int widx = (int)((i + 16u) / 8u);
    int sh = 56 - (int)(i % 8u) * 8;
    x[widx] ^= (uint64_t)K[i] << sh;
  }
  ascon_permute(x, 12u);
  ascon_squeeze_block(x, 16, tag, 0u);
  mfs_zeroize(x, sizeof(x));
  mfs_zeroize(K, sizeof(K));
}

/* ========================= API unificada de suites ======================= */
#define TAG_LEN 16u

mfs_st mfs_suite_seal(uint8_t suite, const uint8_t key[32],
                      uint64_t epoch_seq_nonce, const uint8_t *in, uint16_t len,
                      uint8_t *out, uint16_t *olen) {
  if (suite == 0xFFu) { /* Ultra-Nano sin suites: CRC-only path (caller) */
    if (out != in)
      memmove(out, in, len);
    *olen = len;
    return MFS_OK;
  }
  uint8_t nonce[16];
  mfs_st64(nonce, epoch_seq_nonce); /* época‖seq (MFS-SEC-001) */
  mfs_st64(nonce + 8, epoch_seq_nonce ^ 0xA5A5A5A5A5A5A5A5ULL);
  switch (suite) {
  case MFS_SUITE_S0: {
    uint8_t mac[32]; /* HMAC-SHA256 completo (32 B) */
    uint8_t ctr_nonce[12];
    memcpy(ctr_nonce, nonce, 12);
    aes_ctr_xor(key, ctr_nonce, in, len, out);
    /* EtM: MAC sobre nonce‖ciphertext (el nonce queda autenticado, MFS-SEC-005)
     */
    uint8_t macin[MFS_SCRATCH_MAX + 16u];
    if ((uint32_t)len + 16u > sizeof(macin))
      return MFS_EINVAL;
    memcpy(macin, nonce, 16u);
    memcpy(macin + 16u, out, len);
    mfs_hmac_sha256(key, 32u, macin, (uint32_t)len + 16u, mac);
    memcpy(out + len, mac, TAG_LEN); /* tag = trunc_128(HMAC) */
    *olen = (uint16_t)(len + TAG_LEN);
    mfs_zeroize(mac, sizeof(mac));
    mfs_zeroize(macin, sizeof(macin));
    return MFS_OK;
  }
  case MFS_SUITE_S1: {
    uint8_t gnonce[12];
    memcpy(gnonce, nonce, 12);
    aes_gcm(key, gnonce, in, len, out, out + len);
    *olen = (uint16_t)(len + TAG_LEN);
    return MFS_OK;
  }
  case MFS_SUITE_S2: {
    ascon128a_aead(key, nonce, NULL, 0u, in, len, out, out + len, 0);
    *olen = (uint16_t)(len + TAG_LEN);
    return MFS_OK;
  }
  case MFS_SUITE_S3: {
    uint8_t polykey[32], cn[12];
    memcpy(cn, nonce + 4, 12);
    /* clave Poly1305 = primeras 32 B del keystream block 0 (RFC 8439) */
    uint8_t blk0[64];
    chacha20_block(key, cn, 0u, blk0);
    memcpy(polykey, blk0, 32);
    chacha20_xor(key, cn, 1u, in, len, out);
    poly1305_full(polykey, out, len, out + len);
    *olen = (uint16_t)(len + TAG_LEN);
    mfs_zeroize(blk0, sizeof(blk0));
    mfs_zeroize(polykey, sizeof(polykey));
    return MFS_OK;
  }
  default:
    return MFS_ECIPHER;
  }
}

mfs_st mfs_suite_open(uint8_t suite, const uint8_t key[32],
                      uint64_t epoch_seq_nonce, const uint8_t *in,
                      uint16_t clen, uint8_t *out, uint16_t *olen) {
  if (clen < TAG_LEN && suite != 0xFFu)
    return MFS_EBADMSG;
  if (suite == 0xFFu) {
    if (out != in)
      memmove(out, in, clen);
    *olen = clen;
    return MFS_OK;
  }
  uint16_t len = (uint16_t)(clen - TAG_LEN);
  uint8_t nonce[16];
  mfs_st64(nonce, epoch_seq_nonce);
  mfs_st64(nonce + 8, epoch_seq_nonce ^ 0xA5A5A5A5A5A5A5A5ULL);
  switch (suite) {
  case MFS_SUITE_S0: {
    uint8_t want[32]; /* HMAC completo; se comparan 16 B */
    uint8_t ctr_nonce[12];
    memcpy(ctr_nonce, nonce, 12);
    uint8_t macin[MFS_SCRATCH_MAX + 16u];
    if ((uint32_t)len + 16u > sizeof(macin))
      return MFS_EINVAL;
    memcpy(macin, nonce, 16u);
    memcpy(macin + 16u, in, len);
    mfs_hmac_sha256(key, 32u, macin, (uint32_t)len + 16u, want);
    bool ok = mfs_ct_equal(want, in + len, TAG_LEN);
    mfs_zeroize(want, sizeof(want));
    mfs_zeroize(macin, sizeof(macin));
    if (!ok)
      return MFS_ESECURITY_STATE;
    aes_ctr_xor(key, ctr_nonce, in, len, out);
    *olen = len;
    return MFS_OK;
  }
  case MFS_SUITE_S1: {
    /* verificar el tag GCM sobre el ciphertext ANTES de descifrar
     * (MFS-SEC-005), luego descifrar en 'out' */
    uint8_t gnonce[12];
    memcpy(gnonce, nonce, 12);
    uint8_t tag[16];
    aes_gcm_tag(key, gnonce, in, len, tag);
    if (!mfs_ct_equal(tag, in + len, TAG_LEN))
      return MFS_ESECURITY_STATE;
    uint8_t dummy[16];
    aes_gcm(key, gnonce, in, len, out, dummy); /* out = plaintext */
    *olen = len;
    return MFS_OK;
  }
  case MFS_SUITE_S2: {
    uint8_t tag[16];
    ascon128a_aead(key, nonce, NULL, 0u, in, len, out, tag, 1);
    if (!mfs_ct_equal(tag, in + len, 16u))
      return MFS_ESECURITY_STATE;
    *olen = len;
    return MFS_OK;
  }
  case MFS_SUITE_S3: {
    uint8_t blk0[64], polykey[32], cn[12];
    memcpy(cn, nonce + 4, 12);
    chacha20_block(key, cn, 0u, blk0);
    memcpy(polykey, blk0, 32);
    uint8_t want[16];
    poly1305_full(polykey, in, len, want);
    if (!mfs_ct_equal(want, in + len, 16u))
      return MFS_ESECURITY_STATE;
    chacha20_xor(key, cn, 1u, in, len, out);
    *olen = len;
    mfs_zeroize(blk0, sizeof(blk0));
    return MFS_OK;
  }
  default:
    return MFS_ECIPHER;
  }
}

/* MAC de token (§9.2): B3-keyed truncado o Poly1305 según suite */
void mfs_token_mac(uint8_t suite, const uint8_t key[32], const uint8_t *body,
                   uint32_t blen, uint8_t mac[8]) {
  if (suite == MFS_SUITE_S3) {
    uint8_t tag[16];
    uint8_t blk0[64], polykey[32], cn[12];
    memset(cn, 0, 12);
    memcpy(cn, body, (blen < 12u) ? blen : 12u);
    chacha20_block(key, cn, 0u, blk0);
    memcpy(polykey, blk0, 32);
    poly1305_full(polykey, body, blen, tag);
    memcpy(mac, tag, 8);
    mfs_zeroize(blk0, sizeof(blk0));
  } else {
    mfs_b3_mac_trunc(key, body, blen, mac);
  }
}
