/* kat_crypto.c — Known Answer Tests criptográficos (§27.1)
 *
 * Vectores oficiales: CRC-32C, SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 4231),
 * BLAKE3 (MFS-B3-001) y round-trip autenticado de las suites S0–S3 (§10.6).
 */
#include "mfs_harness.h"
#include "mfs_test.h"

static bool hexeq(const uint8_t *b, uint32_t n, const char *hex) {
  for (uint32_t i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1)
      return false;
    if ((uint8_t)v != b[i])
      return false;
  }
  return true;
}

void kat_crc32c(void) {
  TEST_BEGIN("KAT CRC-32C (Castagnoli)");
  const char *s = "123456789";
  CHECK_EQ(mfs_crc32c((const uint8_t *)s, 9u, 0u), 0xE3069283u);
  CHECK_EQ(mfs_crc32c((const uint8_t *)s, 0u, 0u), 0u);
  /* la semilla cambia el resultado (encadenado por registro E2G) */
  CHECK(mfs_crc32c((const uint8_t *)s, 9u, 0xFFFFFFFFu) !=
        mfs_crc32c((const uint8_t *)s, 9u, 0u));
  /* sensibilidad a un bit */
  uint8_t t[9];
  memcpy(t, s, 9u);
  t[0] ^= 0x01u;
  CHECK(mfs_crc32c(t, 9u, 0u) != mfs_crc32c((const uint8_t *)s, 9u, 0u));
  TEST_END("KAT CRC-32C");
}

void kat_sha256(void) {
  TEST_BEGIN("KAT SHA-256 (FIPS 180-4)");
  mfs_sha256_ctx c;
  uint8_t out[32];

  mfs_sha256_init(&c);
  mfs_sha256_update(&c, (const uint8_t *)"abc", 3u);
  mfs_sha256_final(&c, out);
  CHECK(hexeq(out, 32u,
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad"));

  /* mensaje vacío */
  mfs_sha256_init(&c);
  mfs_sha256_final(&c, out);
  CHECK(hexeq(out, 32u,
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855"));

  /* streaming por trozos == de una vez (bloques + cola) */
  uint8_t msg[200];
  for (uint32_t i = 0; i < sizeof(msg); i++)
    msg[i] = (uint8_t)(i * 7u + 5u);
  uint8_t whole[32], chunked[32];
  mfs_sha256_init(&c);
  mfs_sha256_update(&c, msg, (uint32_t)sizeof(msg));
  mfs_sha256_final(&c, whole);

  mfs_sha256_init(&c);
  uint32_t off = 0;
  const uint32_t steps[3] = {1u, 63u, 64u};
  uint32_t si = 0;
  while (off < sizeof(msg)) {
    uint32_t n = steps[si % 3u];
    if (off + n > sizeof(msg))
      n = (uint32_t)sizeof(msg) - off;
    mfs_sha256_update(&c, msg + off, n);
    off += n;
    si++;
  }
  mfs_sha256_final(&c, chunked);
  CHECK(memcmp(whole, chunked, 32u) == 0);
  TEST_END("KAT SHA-256");
}

void kat_hmac_sha256(void) {
  TEST_BEGIN("KAT HMAC-SHA256 (RFC 4231 caso 1)");
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t mac[32];
  mfs_hmac_sha256(key, (uint32_t)sizeof(key), (const uint8_t *)data,
                  (uint32_t)strlen(data), mac);
  CHECK(hexeq(mac, 32u,
              "b0344c61d8db38535ca8afceaf0bf12b"
              "881dc200c9833da726e9376c2e32cff7"));
  /* determinismo y sensibilidad a la clave */
  uint8_t mac2[32];
  mfs_hmac_sha256(key, (uint32_t)sizeof(key), (const uint8_t *)data,
                  (uint32_t)strlen(data), mac2);
  CHECK(memcmp(mac, mac2, 32u) == 0);
  key[0] ^= 0x01u;
  mfs_hmac_sha256(key, (uint32_t)sizeof(key), (const uint8_t *)data,
                  (uint32_t)strlen(data), mac2);
  CHECK(memcmp(mac, mac2, 32u) != 0);
  TEST_END("KAT HMAC-SHA256");
}

void kat_blake3(void) {
  TEST_BEGIN("KAT BLAKE3-256 (MFS-B3-001)");
  uint8_t out[32];
  /* vector oficial: entrada vacía */
  mfs_b3_256(NULL, 0u, NULL, 0u, out);
  CHECK(hexeq(out, 32u,
              "af1349b9f5f9a1a6a0404dea36dcc949"
              "9bcb25c9adc112b7cc9a93cae41f3262"));
  /* determinismo */
  uint8_t out2[32];
  mfs_b3_256(NULL, 0u, NULL, 0u, out2);
  CHECK(memcmp(out, out2, 32u) == 0);
  /* keyed ≠ unkeyed, y el modo keyed es determinista */
  uint8_t key[32];
  for (uint32_t i = 0; i < 32u; i++)
    key[i] = (uint8_t)(0x11u * (i & 0x0Fu));
  uint8_t k1[32], k2[32];
  mfs_b3_256(key, 32u, (const uint8_t *)"abc", 3u, k1);
  mfs_b3_256(key, 32u, (const uint8_t *)"abc", 3u, k2);
  CHECK(memcmp(k1, k2, 32u) == 0);
  uint8_t u1[32];
  mfs_b3_256(NULL, 0u, (const uint8_t *)"abc", 3u, u1);
  CHECK(memcmp(k1, u1, 32u) != 0);
  /* MAC truncado (token T1): 8 B y estable */
  uint8_t m1[8], m2[8];
  mfs_b3_mac_trunc(key, (const uint8_t *)"token", 5u, m1);
  mfs_b3_mac_trunc(key, (const uint8_t *)"token", 5u, m2);
  CHECK(memcmp(m1, m2, 8u) == 0);
  CHECK(memcmp(m1, k1, 8u) != 0);
  TEST_END("KAT BLAKE3");
}

/* Round-trip autenticado de una suite: seal/open + tamper del tag y del
 * criptograma (ninguno debe abrir). */
static void suite_roundtrip(const char *name, uint8_t suite) {
  uint8_t key[32];
  for (uint32_t i = 0; i < 32u; i++)
    key[i] = (uint8_t)(i * 5u + 1u);
  uint8_t pt[64];
  for (uint32_t i = 0; i < sizeof(pt); i++)
    pt[i] = (uint8_t)(i ^ 0x3Cu);
  uint8_t ct[sizeof(pt) + 16u];
  uint8_t rt[sizeof(pt) + 16u];

  uint16_t olen = (uint16_t)sizeof(ct);
  CHECK_EQ(mfs_suite_seal(suite, key, 0x0123456789ABCDEFull, pt,
                          (uint16_t)sizeof(pt), ct, &olen),
           MFS_OK);
  CHECK_EQ(olen, (uint16_t)(sizeof(pt) + 16u));
  /* determinista: el nonce época‖seq no reutiliza aleatoriedad (§MFS-SEC-001) */
  uint8_t ct2[sizeof(pt) + 16u];
  uint16_t olen2 = (uint16_t)sizeof(ct2);
  CHECK_EQ(mfs_suite_seal(suite, key, 0x0123456789ABCDEFull, pt,
                          (uint16_t)sizeof(pt), ct2, &olen2),
           MFS_OK);
  CHECK(memcmp(ct, ct2, olen) == 0);

  uint16_t rtlen = (uint16_t)sizeof(rt);
  CHECK_EQ(mfs_suite_open(suite, key, 0x0123456789ABCDEFull, ct, olen, rt,
                          &rtlen),
           MFS_OK);
  CHECK_EQ(rtlen, (uint16_t)sizeof(pt));
  CHECK(memcmp(rt, pt, sizeof(pt)) == 0);

  /* ciphertext alterado ⇒ no abre */
  uint8_t bad[sizeof(pt) + 16u];
  memcpy(bad, ct, olen);
  bad[0] ^= 0x01u;
  rtlen = (uint16_t)sizeof(rt);
  CHECK_EQ(mfs_suite_open(suite, key, 0x0123456789ABCDEFull, bad, olen, rt,
                          &rtlen),
           MFS_ESECURITY_STATE);

  /* tag alterado ⇒ no abre */
  memcpy(bad, ct, olen);
  bad[olen - 1u] ^= 0x01u;
  rtlen = (uint16_t)sizeof(rt);
  CHECK_EQ(mfs_suite_open(suite, key, 0x0123456789ABCDEFull, bad, olen, rt,
                          &rtlen),
           MFS_ESECURITY_STATE);

  /* nonce distinto ⇒ no abre (el nonce está autenticado) */
  rtlen = (uint16_t)sizeof(rt);
  CHECK_EQ(mfs_suite_open(suite, key, 0x0123456789ABCDEFull + 1u, ct, olen, rt,
                          &rtlen),
           MFS_ESECURITY_STATE);

  printf("   suite %s: round-trip OK (tag 16 B, nonce autenticado)\n", name);
}

void kat_aesgcm_roundtrip(void) {
  TEST_BEGIN("AEAD S1 AES-256-GCM: round-trip y tamper");
  suite_roundtrip("S1", (uint8_t)MFS_SUITE_S1);
  TEST_END("AEAD S1");
}

void kat_ascon_roundtrip(void) {
  TEST_BEGIN("AEAD S2 Ascon-128a: round-trip y tamper");
  suite_roundtrip("S2", (uint8_t)MFS_SUITE_S2);
  TEST_END("AEAD S2");
}

void kat_chacha_roundtrip(void) {
  TEST_BEGIN("AEAD S3 ChaCha20-Poly1305: round-trip y tamper");
  suite_roundtrip("S3", (uint8_t)MFS_SUITE_S3);
  TEST_END("AEAD S3");
}

void kat_suite_tamper(void) {
  TEST_BEGIN("Suites: el estado autenticado no se puede falsear");
  uint8_t key[32];
  memset(key, 0x5Au, sizeof(key));
  uint8_t pt[32];
  memset(pt, 0xC3u, sizeof(pt));

  /* una clave distinta no abre el mensaje de otra (S0 y S3) */
  const uint8_t suites[2] = {(uint8_t)MFS_SUITE_S0, (uint8_t)MFS_SUITE_S3};
  for (uint32_t s = 0; s < 2u; s++) {
    uint8_t ct[sizeof(pt) + 16u];
    uint16_t olen = (uint16_t)sizeof(ct);
    CHECK_EQ(mfs_suite_seal(suites[s], key, 42ull, pt, (uint16_t)sizeof(pt),
                            ct, &olen),
             MFS_OK);
    uint8_t other[32];
    memset(other, 0xA5u, sizeof(other));
    uint8_t rt[sizeof(pt) + 16u];
    uint16_t rtlen = (uint16_t)sizeof(rt);
    CHECK_EQ(mfs_suite_open(suites[s], other, 42ull, ct, olen, rt, &rtlen),
             MFS_ESECURITY_STATE);
  }
  /* comparación en tiempo constante (MFS-SEC-001/002) */
  uint8_t a[8] = {0}, b[8] = {0};
  CHECK(mfs_ct_equal(a, b, 8u));
  b[7] = 1u;
  CHECK(!mfs_ct_equal(a, b, 8u));
  TEST_END("Tamper de suites");
}
