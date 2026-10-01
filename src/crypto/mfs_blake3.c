/* mfs_blake3.c — BLAKE3-256: compresión, hashing de un chunk y modo árbol
 * (§10.5) Implementación conforme al reference spec (7 rondas, permutación
 * estándar). KAT verificados contra BLAKE3 vectors oficiales en
 * tests/kat_crypto.c. MFS-B3-001: integridad de datos, fingerprints de dedup y
 * Merkle global.
 */
#include "mfs_internal.h"
#include <string.h>

#define BLOCK_LEN 64u
#define CHUNK_LEN 1024u

#define CHUNK_START 1u
#define CHUNK_END 2u
#define PARENT 4u
#define ROOT 8u
#define KEYED_HASH 16u

static const uint32_t IV[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u,
                               0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu,
                               0x1F83D9ABu, 0x5BE0CD19u};

static inline uint32_t load_le32(const uint8_t *b) { return mfs_ld32(b); }
static inline void store_le32(uint8_t *b, uint32_t v) { mfs_st32(b, v); }
static inline uint32_t rotr32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

/* Permutación del mensaje entre rondas (reference impl), aplicada acumulativa
 */
static void permute_msg(uint32_t m[16]) {
  const uint8_t p[16] = {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8};
  uint32_t t[16];
  memcpy(t, m, sizeof(t));
  for (int i = 0; i < 16; i++)
    m[i] = t[p[i]];
}

static void b3_compress(const uint32_t cv_in[8], const uint8_t block[BLOCK_LEN],
                        uint64_t counter, uint32_t blen, uint32_t flags,
                        uint32_t out[16]) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = load_le32(block + i * 4);

  uint32_t s[16];
  memcpy(s, cv_in, 32);
  s[8] = IV[0];
  s[9] = IV[1];
  s[10] = IV[2];
  s[11] = IV[3];
  s[12] = (uint32_t)counter;
  s[13] = (uint32_t)(counter >> 32);
  s[14] = blen;
  s[15] = flags;

#define G(a, b, c, d, x, y)                                                    \
  do {                                                                         \
    s[a] = s[a] + s[b] + (x);                                                  \
    s[d] = rotr32(s[d] ^ s[a], 16);                                            \
    s[c] = s[c] + s[d];                                                        \
    s[b] = rotr32(s[b] ^ s[c], 12);                                            \
    s[a] = s[a] + s[b] + (y);                                                  \
    s[d] = rotr32(s[d] ^ s[a], 8);                                             \
    s[c] = s[c] + s[d];                                                        \
    s[b] = rotr32(s[b] ^ s[c], 7);                                             \
  } while (0)

  for (int r = 0; r < 7; r++) {
    if (r > 0)
      permute_msg(m);
    G(0, 4, 8, 12, m[0], m[1]);
    G(1, 5, 9, 13, m[2], m[3]);
    G(2, 6, 10, 14, m[4], m[5]);
    G(3, 7, 11, 15, m[6], m[7]);
    G(0, 5, 10, 15, m[8], m[9]);
    G(1, 6, 11, 12, m[10], m[11]);
    G(2, 7, 8, 13, m[12], m[13]);
    G(3, 4, 9, 14, m[14], m[15]);
  }
#undef G

  /* salida BLAKE3: out[0..7] = v[i]^v[i+8]; out[8..15] = v[i+8]^cv[i] */
  for (int i = 0; i < 8; i++)
    out[i] = s[i] ^ s[i + 8];
  for (int i = 0; i < 8; i++)
    out[i + 8] = s[i + 8] ^ cv_in[i];
}

/* Hash de un chunk (<= 1024 B) con flags; sale CV[0..7] */
static void chunk_cv(const uint8_t *in, size_t len, uint64_t counter,
                     const uint32_t key[8], uint32_t flags,
                     uint32_t cv_out[8]) {
  uint32_t cv[8];
  memcpy(cv, key, 32);
  size_t off = 0;
  uint32_t cs = CHUNK_START;
  if (len == 0u) {
    uint8_t blk[BLOCK_LEN];
    memset(blk, 0, BLOCK_LEN);
    uint32_t w[16];
    b3_compress(cv, blk, counter, 0, flags | CHUNK_START | CHUNK_END, w);
    memcpy(cv_out, w, 32);
    return;
  }
  while (off < len) {
    uint8_t blk[BLOCK_LEN];
    memset(blk, 0, BLOCK_LEN);
    size_t take = len - off;
    if (take > BLOCK_LEN)
      take = BLOCK_LEN;
    memcpy(blk, in + off, take);
    int last = (off + take == len);
    uint32_t f = flags | cs | (last ? CHUNK_END : 0u);
    uint32_t w[16];
    b3_compress(cv, blk, counter, (uint32_t)take, f, w);
    memcpy(cv, w, 32);
    counter++;
    cs = 0;
    off += take;
  }
  memcpy(cv_out, cv, 32);
}

static void parent_cv(const uint32_t left[8], const uint32_t right[8],
                      const uint32_t key[8], uint32_t flags, uint32_t out[8]) {
  uint8_t blk[BLOCK_LEN];
  for (int i = 0; i < 8; i++)
    store_le32(blk + i * 4, left[i]);
  for (int i = 0; i < 8; i++)
    store_le32(blk + 32 + i * 4, right[i]);
  uint32_t w[16];
  b3_compress(key, blk, 0, BLOCK_LEN, flags | PARENT, w);
  memcpy(out, w, 32);
}

/* Tamaño del subtree más grande: potencia de 2 * CHUNK_LEN estrictamente menor
 * que len */
static size_t largest_chunk_power(size_t len) {
  size_t node = CHUNK_LEN;
  while (node * 2u < len)
    node *= 2u;
  return node;
}

static void tree_cv(const uint8_t *in, size_t len, uint64_t counter,
                    const uint32_t key[8], uint32_t flags, uint32_t out[8]) {
  if (len <= CHUNK_LEN) {
    chunk_cv(in, len, counter, key, flags, out);
    return;
  }
  size_t split = largest_chunk_power(len);
  size_t left_chunks = split / CHUNK_LEN; /* contador del subárbol derecho */
  uint32_t left[8], right[8];
  tree_cv(in, split, counter, key, flags & ~ROOT, left);
  tree_cv(in + split, len - split, counter + left_chunks, key, flags & ~ROOT,
          right);
  parent_cv(left, right, key, flags, out);
}

void mfs_b3_256(const uint8_t *key, uint32_t keylen, const uint8_t *in,
                uint32_t inlen, uint8_t out[32]) {
  uint32_t k[8];
  uint32_t flags = 0;
  if (key != NULL && keylen > 0u) {
    flags = KEYED_HASH;
    if (keylen == 32u) {
      for (int i = 0; i < 8; i++)
        k[i] = load_le32(key + i * 4);
    } else {
      uint8_t padded[64];
      memset(padded, 0, 64);
      memcpy(padded, key, (keylen < 64u) ? keylen : 64u);
      uint32_t kk[8];
      tree_cv(padded, 64, 0, IV, 0, kk);
      memcpy(k, kk, 32);
    }
  } else {
    memcpy(k, IV, 32);
  }
  uint32_t root[8];
  tree_cv((in != NULL) ? in : (const uint8_t *)"", inlen, 0, k, flags | ROOT,
          root);
  for (int i = 0; i < 8; i++)
    store_le32(out + i * 4, root[i]);
}

/* MAC truncado para token (§9.2): B3-keyed, primeros 8 bytes */
void mfs_b3_mac_trunc(const uint8_t key[32], const uint8_t *msg, uint32_t len,
                      uint8_t mac[8]) {
  uint8_t h[32];
  mfs_b3_256(key, 32u, msg, len, h);
  memcpy(mac, h, 8u);
}

/* Árbol Merkle sobre hashes de hoja (§9.5 checkpoint incremental).
 * leaves: concatenación de nleaf bloques de leaf_len bytes. */
void mfs_b3_merkle_root(const uint8_t *leaves, uint32_t nleaf,
                        uint32_t leaf_len, uint8_t root_out[32]) {
  if (nleaf == 0u) {
    mfs_b3_256(NULL, 0, NULL, 0, root_out);
    return;
  }
  static uint8_t lvl[64][32];
  uint32_t cur = (nleaf > 64u) ? 64u : nleaf;
  for (uint32_t i = 0; i < cur; i++) {
    mfs_b3_256(NULL, 0, leaves + (size_t)i * leaf_len, leaf_len, lvl[i]);
  }
  while (cur > 1u) {
    uint32_t nxt = (cur + 1u) / 2u;
    for (uint32_t i = 0; i < nxt; i++) {
      uint8_t pair[64];
      memcpy(pair, lvl[2u * i], 32);
      memcpy(pair + 32, lvl[(2u * i + 1u < cur) ? 2u * i + 1u : 2u * i], 32);
      mfs_b3_256(NULL, 0, pair, 64u, lvl[i]);
    }
    cur = nxt;
  }
  memcpy(root_out, lvl[0], 32);
}
