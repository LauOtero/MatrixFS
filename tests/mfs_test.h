/* mfs_test.h — framework mínimo de pruebas MatrixFS Ultra (§27) */
#ifndef MFS_TEST_H
#define MFS_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    g_checks++;                                                                \
    long long _a = (long long)(a), _b = (long long)(b);                        \
    if (_a != _b) {                                                            \
      g_failures++;                                                            \
      printf("  [FAIL] %s:%d  %s == %s  (%lld != %lld)\n", __FILE__, __LINE__, \
             #a, #b, _a, _b);                                                  \
    }                                                                          \
  } while (0)

#define TEST_BEGIN(name) printf("== %s\n", name)
#define TEST_END(name) printf("-- %s ok\n", name)

/* Suites (§27) */
void kat_crc32c(void);
void kat_sha256(void);
void kat_hmac_sha256(void);
void kat_blake3(void);
void kat_ascon_roundtrip(void);
void kat_chacha_roundtrip(void);
void kat_aesgcm_roundtrip(void);
void kat_suite_tamper(void);

void test_gate_earch(void);
void test_gate_notviable(void);
void test_gate_mode_matrix(void);

/* Perfiles por tecnología de memoria (MFS-CAP-001) */
void test_media_profiles(void);

void test_fs_basic(void);
void test_fs_multipage(void);
void test_fs_dirs(void);
void test_fs_persistence(void);
void test_fs_tx_savepoints(void);
void test_fs_snapshots(void);
void test_fs_edp(void);
void test_fs_verify_health(void);
void test_fs_daio(void);

void test_fih_powerloss(void);
void test_fih_torn_write(void);
void test_fih_nor_discipline(void);
void test_fih_bitflip(void);

/* Capa de integración VFS portable (§21, §22): misma ruta que FUSE/WinFsp */
void test_vfs_portable(void);

/* Fase 3–5 (§8.5, §11, §14.1, §15) */
void test_ftl_wom(void);
void test_ftl_slec(void);
void test_ftl_eba(void);
void test_ftl_tg(void);
void test_ftl_elm_pep(void);
void test_ftl_zrp(void);
void test_hmt_activation(void);
void test_hmt_metadata_in_t0(void);
void test_hmt_t0_failure_degrades(void);
void test_pq_hkdf(void);
void test_pq_puf(void);
void test_pq_lms(void);
void test_xio_xdam(void);
void test_xio_sdp(void);
void test_xio_cqe(void);

/* Extremos y formal (§27.2, §27.3, §27.4) */
void test_stress_churn(void);
void test_stress_crash_sweep(void);
void test_stress_metadata_storm(void);
void test_stress_crypto_matrix(void);
void test_stress_allocation_exhaustion(void);
void test_formal_replay_determinism(void);
void test_formal_wal_invariants(void);

void bench_run(void);
void bench_extreme_run(void);

#endif /* MFS_TEST_H */
