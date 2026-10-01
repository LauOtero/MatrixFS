/* test_pq.c — HKDF, PUF y firma post-cuántica LMS (§15) */
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

void test_pq_hkdf(void) {
  TEST_BEGIN("KAT HKDF-SHA256 (RFC 5869 caso 1)");
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  uint8_t salt[13];
  for (uint32_t i = 0; i < 13u; i++)
    salt[i] = (uint8_t)i;
  uint8_t info[10];
  for (uint32_t i = 0; i < 10u; i++)
    info[i] = (uint8_t)(0xf0u + i);
  uint8_t okm[42];
  mfs_hkdf_sha256_info(salt, 13u, ikm, 22u, info, 10u, okm, 42u);
  CHECK(hexeq(okm, 42u,
              "3cb25f25faacd57a90434f64d0362f2a"
              "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865"));
  /* determinismo y sensibilidad a la sal */
  uint8_t a[32], b[32];
  mfs_hkdf_sha256(salt, 13u, ikm, 22u, a, 32u);
  mfs_hkdf_sha256(salt, 13u, ikm, 22u, b, 32u);
  CHECK(memcmp(a, b, 32u) == 0);
  salt[0] ^= 0x01u;
  mfs_hkdf_sha256(salt, 13u, ikm, 22u, b, 32u);
  CHECK(memcmp(a, b, 32u) != 0);
  TEST_END("KAT HKDF");
}

void test_pq_puf(void) {
  TEST_BEGIN("PUF: enrolamiento, reproducción y fallback (§15 MFS-PUF-001)");
  mfs_env_t e;
  CHECK(env_open(&e, 32768u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  env_set_assets(&e, MFS_HWV2_PUF, 0u);

  /* SRAM modelada como código de repetición: los 3 bytes de cada triple
   * codifican el mismo bit ⇒ 1 error por triple es corregible (mayoría). */
  static uint8_t sram[768];
  for (uint32_t i = 0; i < 256u; i++) {
    uint8_t bit = (uint8_t)((i * 7u + 3u) & 1u);
    uint8_t v = bit ? 0xA5u : 0x5Au;
    sram[3u * i] = v;
    sram[3u * i + 1u] = v;
    sram[3u * i + 2u] = v;
  }
  uint8_t salt[16];
  for (uint32_t i = 0; i < 16u; i++)
    salt[i] = (uint8_t)(0xA0u + i);

  uint8_t k1[32], k2[32], k3[32];
  CHECK_EQ(mfs_puf_enroll(&e.fs, sram, sizeof(sram), salt, k1), MFS_OK);
  CHECK_EQ(e.fs.hct.puf_enrolls, 1u);
  CHECK_EQ(mfs_puf_reproduce(&e.fs, sram, sizeof(sram), k2), MFS_OK);
  CHECK(memcmp(k1, k2, 32u) == 0);

  /* ruido corregible: se altera 1 de las 3 muestras en 40 triples */
  uint8_t noisy[768];
  memcpy(noisy, sram, sizeof(noisy));
  for (uint32_t t = 0; t < 40u; t++)
    noisy[3u * t] ^= 0x01u;
  CHECK_EQ(mfs_puf_reproduce(&e.fs, noisy, sizeof(noisy), k3), MFS_OK);
  CHECK(memcmp(k1, k3, 32u) == 0); /* mayoría corrige el ruido */

  /* ruido no corregible: 2 de 3 muestras ⇒ la clave cambia (documentado) */
  uint8_t bad[768];
  memcpy(bad, sram, sizeof(bad));
  for (uint32_t t = 0; t < 40u; t++) {
    bad[3u * t] ^= 0x01u;
    bad[3u * t + 1u] ^= 0x01u;
  }
  CHECK_EQ(mfs_puf_reproduce(&e.fs, bad, sizeof(bad), k3), MFS_OK);
  CHECK(memcmp(k1, k3, 32u) != 0);

  /* fallback documentado: HKDF(UID+sal) con evento HCT */
  uint8_t uid[8] = {9, 8, 7, 6, 5, 4, 3, 2};
  uint32_t fb0 = e.fs.hct.puf_fallbacks;
  CHECK_EQ(mfs_puf_fallback_key(&e.fs, uid, sizeof(uid), k3), MFS_OK);
  CHECK(e.fs.hct.puf_fallbacks == fb0 + 1u);

  /* sin capacidad PUF declarada ⇒ MFS_EPUF */
  mfs_env_t e2;
  CHECK(env_open(&e2, 32768u, NULL));
  CHECK_EQ(env_format(&e2), MFS_OK);
  CHECK_EQ(mfs_puf_enroll(&e2.fs, sram, sizeof(sram), salt, k1), MFS_EPUF);
  env_close(&e2);
  env_close(&e);
  TEST_END("PUF");
}

void test_pq_lms(void) {
  TEST_BEGIN("LMS (SP 800-208): keygen, firma y verificación");
  static uint8_t seed[32];
  for (uint32_t i = 0; i < 32u; i++)
    seed[i] = (uint8_t)(i * 3u + 1u);
  mfs_lms_pub_t pub;
  CHECK_EQ(mfs_lms_keygen(seed, &pub, NULL), MFS_OK);
  CHECK(pub.type == 1u);

  static uint8_t sig[8192];
  uint32_t siglen = sizeof(sig);
  const char *msg = "MatrixFS Ultra ATLAS — secure boot anchor";
  CHECK_EQ(mfs_lms_sign(seed, 3u, (const uint8_t *)msg, (uint32_t)strlen(msg),
                        sig, &siglen),
           MFS_OK);
  CHECK(siglen > 0u);
  CHECK_EQ(mfs_lms_verify(&pub, (const uint8_t *)msg, (uint32_t)strlen(msg),
                          sig, siglen),
           MFS_OK);

  /* mensaje alterado ⇒ falla */
  CHECK_EQ(mfs_lms_verify(&pub, (const uint8_t *)"otro mensaje",
                          (uint32_t)strlen("otro mensaje"), sig, siglen),
           MFS_ESECURITY_STATE);
  /* firma alterada ⇒ falla */
  sig[siglen - 1u] ^= 0x01u;
  CHECK_EQ(mfs_lms_verify(&pub, (const uint8_t *)msg, (uint32_t)strlen(msg),
                          sig, siglen),
           MFS_ESECURITY_STATE);
  sig[siglen - 1u] ^= 0x01u;
  /* clave pública alterada ⇒ falla */
  mfs_lms_pub_t pub2 = pub;
  pub2.T1[0] ^= 0x01u;
  CHECK_EQ(mfs_lms_verify(&pub2, (const uint8_t *)msg, (uint32_t)strlen(msg),
                          sig, siglen),
           MFS_ESECURITY_STATE);

  /* hojas distintas del mismo árbol producen firmas distintas y válidas */
  uint32_t s2len = sizeof(sig);
  CHECK_EQ(mfs_lms_sign(seed, 7u, (const uint8_t *)msg, (uint32_t)strlen(msg),
                        sig, &s2len),
           MFS_OK);
  CHECK_EQ(mfs_lms_verify(&pub, (const uint8_t *)msg, (uint32_t)strlen(msg),
                          sig, s2len),
           MFS_OK);
  TEST_END("LMS");
}
