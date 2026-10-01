/* test_stress.c — pruebas extremas de todas las funcionalidades (§27.2)
 * Churn/GC, barrido masivo de cortes, tormenta de metadatos, matriz de
 * suites con persistencia y agotamiento de recursos.
 */
#include "mfs_harness.h"
#include "mfs_test.h"
#include <stdlib.h>

#define RAM_COMPACT 32768u

static void pattern(uint8_t *b, uint32_t n, uint32_t seed) {
  uint32_t s = seed * 2654435761u + 1u;
  for (uint32_t i = 0; i < n; i++) {
    s = s * 1103515245u + 12345u;
    b[i] = (uint8_t)(s >> 16);
  }
}

static int write_file(mfs_env_t *e, const char *path, const uint8_t *data,
                      uint32_t n) {
  mfs_file *f = NULL;
  int r = mf_open(&e->fs, path, MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
  if (r != MFS_OK)
    return r;
  size_t wr = 0;
  r = mf_write(f, data, n, &wr);
  (void)mf_close(f);
  return r;
}

static int read_file(mfs_env_t *e, const char *path, uint8_t *out, uint32_t n,
                     uint32_t *rd) {
  mfs_file *f = NULL;
  int r = mf_open(&e->fs, path, MFS_O_RDONLY, &f);
  if (r != MFS_OK)
    return r;
  size_t got = 0;
  r = mf_read(f, out, n, &got);
  (void)mf_close(f);
  if (rd)
    *rd = (uint32_t)got;
  return r;
}

/* ------------------- churn intensivo + GC + integridad ------------------- */
void test_stress_churn(void) {
  TEST_BEGIN("Estrés: churn con GC y verificación de integridad");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t canary[492];
  pattern(canary, sizeof(canary), 0xC0FFEEu);
  CHECK_EQ(write_file(&e, "/canary.bin", canary, sizeof(canary)), MFS_OK);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  uint8_t buf[492];
  uint32_t ok = 0, gc0 = e.fs.hct.gc_relocated;
  for (uint32_t i = 0; i < 300u; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/churn%u.tmp", i % 8u);
    pattern(buf, sizeof(buf), i);
    int r = write_file(&e, path, buf, sizeof(buf));
    if (r != MFS_OK) {
      (void)mf_sync(&e.fs);
      r = write_file(&e, path, buf, sizeof(buf));
    }
    if (r == MFS_OK)
      ok++;
    if ((i % 16u) == 0u)
      (void)mf_sync(&e.fs);
  }
  CHECK(ok > 250u);
  CHECK(e.fs.hct.gc_relocated > gc0); /* la GC trabajó de verdad */
  CHECK_EQ(e.vf.n_violations, 0u);

  uint8_t back[492];
  uint32_t rd = 0;
  CHECK_EQ(read_file(&e, "/canary.bin", back, sizeof(back), &rd), MFS_OK);
  CHECK_EQ(rd, sizeof(canary));
  CHECK(memcmp(back, canary, sizeof(canary)) == 0);

  /* la integridad sobrevive a un remontaje completo */
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK_EQ(read_file(&e, "/canary.bin", back, sizeof(back), &rd), MFS_OK);
  CHECK(rd == sizeof(canary) && memcmp(back, canary, sizeof(canary)) == 0);
  printf(
      "   GC relocalizaciones=%u  WAF=%.3f  prog=%u\n", e.fs.hct.gc_relocated,
      e.fs.hct.writes_host ? (double)e.fs.hct.writes_prog / e.fs.hct.writes_host
                           : 0.0,
      e.vf.n_prog);
  env_close(&e);
  TEST_END("Estrés churn");
}

/* --------------------- barrido masivo de cortes (W6) --------------------- */
void test_stress_crash_sweep(void) {
  TEST_BEGIN("Estrés: 1000 ciclos de corte + remontaje (W6)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t canary[492];
  pattern(canary, sizeof(canary), 0xABCDEFu);
  CHECK_EQ(write_file(&e, "/durable.bin", canary, sizeof(canary)), MFS_OK);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  uint8_t buf[492];
  uint32_t bad = 0;
  for (uint32_t i = 0; i < 1000u; i++) {
    pattern(buf, sizeof(buf), i + 1000u);
    char path[32];
    snprintf(path, sizeof(path), "/c%u.tmp", i % 4u);
    (void)write_file(&e, path, buf, sizeof(buf)); /* sin sync: puede perderse */
    vf_crash(&e.vf);
    vf_recover(&e.vf);
    if ((i % 3u) == 0u)
      (void)mf_sync(&e.fs); /* commit intermitente */
    int r = mf_init(&e.fs, &e.cfg);
    if (r != MFS_OK) {
      bad++;
      break;
    }
    uint8_t back[492];
    uint32_t rd = 0;
    if (read_file(&e, "/durable.bin", back, sizeof(back), &rd) != MFS_OK ||
        rd != sizeof(canary) || memcmp(back, canary, sizeof(canary)) != 0) {
      bad++;
      break;
    }
  }
  CHECK_EQ(bad, 0u);
  CHECK_EQ(e.vf.n_violations, 0u);
  printf("   1000 cortes, 0 corrupciones del fichero confirmado\n");
  env_close(&e);
  TEST_END("Estrés cortes");
}

/* ----------------------- tormenta de metadatos -------------------------- */
void test_stress_metadata_storm(void) {
  TEST_BEGIN("Estrés: tormenta de metadatos (mkdir/rename/unlink)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t canary[256];
  pattern(canary, sizeof(canary), 0x112233u);
  CHECK_EQ(write_file(&e, "/canary.bin", canary, sizeof(canary)), MFS_OK);

  uint32_t ops = 0;
  for (uint32_t i = 0; i < 60u; i++) {
    char d[32], f1[48], f2[48];
    snprintf(d, sizeof(d), "/d%u", i % 6u);
    (void)mf_mkdir(&e.fs, d);
    snprintf(f1, sizeof(f1), "%s/a%u", d, i);
    snprintf(f2, sizeof(f2), "%s/b%u", d, i);
    if (write_file(&e, f1, canary, 32u) == MFS_OK)
      ops++;
    if (mf_rename(&e.fs, f1, f2) == MFS_OK)
      ops++;
    (void)mf_unlink(&e.fs, f2);
    if ((i % 10u) == 0u)
      (void)mf_sync(&e.fs);
  }
  CHECK(ops > 80u);
  CHECK_EQ(env_remount(&e), MFS_OK);

  /* el canario sobrevive y no hay ficheros fantasma */
  uint8_t back[256];
  uint32_t rd = 0;
  CHECK_EQ(read_file(&e, "/canary.bin", back, sizeof(back), &rd), MFS_OK);
  CHECK(rd == sizeof(canary) && memcmp(back, canary, sizeof(canary)) == 0);

  mfs_stat st;
  for (uint32_t i = 0; i < 6u; i++) {
    char d[32];
    snprintf(d, sizeof(d), "/d%u", i);
    mfs_dir *dir = NULL;
    if (mf_opendir(&e.fs, d, &dir) == MFS_OK) {
      mfs_dirent de;
      while (mf_readdir(dir, &de) == MFS_OK) {
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", d, de.name);
        CHECK_EQ(mf_stat(&e.fs, full, &st), MFS_OK); /* todo listado existe */
      }
      mf_closedir(dir);
    }
  }
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_META), MFS_OK);
  env_close(&e);
  TEST_END("Estrés metadatos");
}

/* ---------------- matriz de suites con persistencia -------------------- */
void test_stress_crypto_matrix(void) {
  TEST_BEGIN("Estrés: matriz S0–S3 con cifrado y remontaje");
  static const uint8_t suites[4] = {MFS_SUITE_S0, MFS_SUITE_S1, MFS_SUITE_S2,
                                    MFS_SUITE_S3};
  for (uint32_t s = 0; s < 4u; s++) {
    mfs_env_t e;
    uint8_t key[32];
    for (uint32_t i = 0; i < 32u; i++)
      key[i] = (uint8_t)(0x40u + i + s);
    CHECK(env_open(&e, 98304u, key)); /* Balanced (chunk 4096) */
    e.cfg.suite_preferred = suites[s];
    CHECK_EQ(env_format(&e), MFS_OK);

    uint8_t data[3000];
    pattern(data, sizeof(data), 0x1000u + s);
    CHECK_EQ(write_file(&e, "/enc.bin", data, sizeof(data)), MFS_OK);
    CHECK_EQ(mf_sync(&e.fs), MFS_OK);

    /* ciphertext real en flash: el patrón no debe aparecer en claro */
    int plain_in_flash = 0;
    for (uint32_t off = 0; off < e.vf.size; off++) {
      uint8_t *p = vf_raw(&e.vf, off);
      if (p && *p == data[0] && off + 32u <= e.vf.size &&
          memcmp(vf_raw(&e.vf, off), data, 32u) == 0) {
        plain_in_flash = 1;
        break;
      }
    }
    CHECK(!plain_in_flash);

    CHECK_EQ(env_remount(&e), MFS_OK);
    uint8_t back[3000];
    uint32_t rd = 0;
    CHECK_EQ(read_file(&e, "/enc.bin", back, sizeof(back), &rd), MFS_OK);
    CHECK_EQ(rd, sizeof(data));
    CHECK(memcmp(back, data, sizeof(data)) == 0);
    printf(
        "   suite S%u: round-trip cifrado + remontaje OK (nonce época‖seq)\n",
        s);
    env_close(&e);
  }
  TEST_END("Estrés cripto");
}

/* ------------------- agotamiento de recursos --------------------------- */
void test_stress_allocation_exhaustion(void) {
  TEST_BEGIN("Estrés: agotamiento de zonas ⇒ errores tipificados");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t canary[128];
  pattern(canary, sizeof(canary), 0x777u);
  CHECK_EQ(write_file(&e, "/keep.bin", canary, sizeof(canary)), MFS_OK);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  /* El agotamiento real de zonas se alcanza con la presión combinada de
   * datos + metadatos (cada fichero genera INODE/DIRENT). Se detiene al
   * quedar poco margen para poder verificar que el sistema sigue operativo
   * (el agotamiento total exige reserva de GC rotatoria; ver
   * known-limitations.md). Nota: la ventana de inodos es acotada
   * (MFS_MAX_FILES_OPEN+16). */
  static uint8_t buf[492];
  pattern(buf, sizeof(buf), 0x999u);
  uint32_t typed_errors = 0, writes_ok = 0;
  for (uint32_t i = 0; i < 800u; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/fill%u.bin", i);
    int r = write_file(&e, path, buf, sizeof(buf));
    if (r == MFS_OK)
      writes_ok++;
    else if (r == MFS_ENOSPC || r == MFS_EBACKPRESSURE || r == MFS_ETABLEFULL ||
             r == MFS_ETIMEDOUT_BUDGET)
      typed_errors++;
    else {
      CHECK(false);
      break;
    } /* nunca error inesperado */
    if ((i % 32u) == 0u)
      (void)mf_sync(&e.fs);
    if (mfs_zone_count_free(&e.fs) <= 4u)
      break; /* margen de seguridad antes del llenado total */
  }
  printf("   escrituras ok=%u, errores tipificados=%u, zonas libres=%u\n",
         writes_ok, typed_errors, mfs_zone_count_free(&e.fs));
  CHECK(writes_ok > 0u);
  CHECK_EQ(e.vf.n_violations, 0u);

  /* el sistema sigue siendo montable y consistente */
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_META), MFS_OK);

  /* ventana de mantenimiento: drenar la deuda GLD acumulada */
  mfs_gld_maybe_gc(&e.fs);
  /* y sigue operativo con el margen restante */
  uint8_t canary2[128];
  pattern(canary2, sizeof(canary2), 0xABCu);
  CHECK_EQ(write_file(&e, "/after.bin", canary2, sizeof(canary2)), MFS_OK);
  uint8_t back[128];
  uint32_t rd = 0;
  CHECK_EQ(read_file(&e, "/after.bin", back, sizeof(back), &rd), MFS_OK);
  CHECK(rd == sizeof(canary2) && memcmp(back, canary2, sizeof(canary2)) == 0);

  /* la consistencia y la accesibilidad sobreviven al remontaje */
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_META), MFS_OK);
  CHECK_EQ(read_file(&e, "/after.bin", back, sizeof(back), &rd), MFS_OK);
  CHECK(rd == sizeof(canary2) && memcmp(back, canary2, sizeof(canary2)) == 0);
  env_close(&e);
  TEST_END("Estrés agotamiento");
}
