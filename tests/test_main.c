/* test_main.c — runner de la suite MatrixFS Ultra (§27) */
#include "mfs_test.h"

int g_checks = 0;
int g_failures = 0;

int main(int argc, char **argv) {
  bool run_bench = false;
  bool run_extreme = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--bench") == 0)
      run_bench = true;
    if (strcmp(argv[i], "--extreme") == 0)
      run_extreme = true;
  }

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("MatrixFS Ultra «ATLAS» v1.0 — suite de verificación\n");
  printf("---------------------------------------------------\n");

  /* §27.1 KATs */
  kat_crc32c();
  kat_sha256();
  kat_hmac_sha256();
  kat_blake3();
  kat_aesgcm_roundtrip();
  kat_ascon_roundtrip();
  kat_chacha_roundtrip();
  kat_suite_tamper();

  /* §27.7 rechazo temprano */
  test_gate_earch();
  test_gate_notviable();
  test_gate_mode_matrix();

  /* MFS-CAP-001: perfiles por tecnología de memoria */
  test_media_profiles();

  /* Medios gestionados (SD/eMMC/UFS/SATA/NVMe) sobre el adaptador L2 */
  test_managed_media();

  /* Puerto RTOS genérico del núcleo (§20.2) */
  test_rtos_port();

  /* Funcional */
  test_fs_basic();
  test_fs_multipage();
  test_fs_dirs();
  test_fs_persistence();
  test_fs_tx_savepoints();
  test_fs_snapshots();
  test_fs_edp();
  test_fs_verify_health();
  test_fs_daio();

  /* §27.2 FIH */
  test_fih_powerloss();
  test_fih_torn_write();
  test_fih_nor_discipline();
  test_fih_bitflip();

  /* Capa de integración con el SO (§21, §22) */
  test_vfs_portable();

  /* Fase 3–5 */
  test_ftl_wom();
  test_ftl_slec();
  test_ftl_eba();
  test_ftl_tg();
  test_ftl_elm_pep();
  test_ftl_zrp();
  test_hmt_activation();
  test_hmt_metadata_in_t0();
  test_hmt_t0_failure_degrades();
  test_pq_hkdf();
  test_pq_puf();
  test_pq_lms();
  test_xio_xdam();
  test_xio_sdp();
  test_xio_cqe();

  /* Extremos y modelos ejecutables */
  test_stress_churn();
  test_stress_crash_sweep();
  test_stress_metadata_storm();
  test_stress_crypto_matrix();
  test_stress_allocation_exhaustion();
  test_formal_replay_determinism();
  test_formal_wal_invariants();

  if (run_bench)
    bench_run();
  if (run_extreme)
    bench_extreme_run();

  printf("---------------------------------------------------\n");
  printf("checks: %d   fallos: %d\n", g_checks, g_failures);
  return (g_failures == 0) ? 0 : 1;
}
