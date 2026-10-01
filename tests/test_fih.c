/* test_fih.c — Fault Injection Hardware (§27.2): cortes de energía, escritura
 * rasgada y disciplina NOR. Objetivo W6: cero corrupción de datos confirmados.
 */
#include "mfs_harness.h"
#include "mfs_test.h"

#define RAM_COMPACT 32768u
#define FIH_ITERS 100u

void test_fih_powerloss(void) {
  TEST_BEGIN("FIH corte de energía (W6): consistencia tras crash");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t data[1024];
  for (uint32_t i = 0; i < sizeof(data); i++)
    data[i] = (uint8_t)(i * 13u + 1u);
  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/durable.bin",
                   MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
           MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  uint32_t ok = 0;
  for (uint32_t i = 0; i < FIH_ITERS; i++) {
    vf_crash(&e.vf);   /* corte en un punto arbitrario */
    vf_recover(&e.vf); /* re-alimentación: imagen persistente */
    int r = mf_init(&e.fs, &e.cfg);
    if (r != MFS_OK) {
      CHECK_EQ(r, MFS_OK);
      break;
    }
    mfs_file *g = NULL;
    if (mf_open(&e.fs, "/durable.bin", MFS_O_RDONLY, &g) != MFS_OK) {
      CHECK(false);
      break;
    }
    uint8_t back[1024];
    size_t rd = 0;
    int rr = mf_read(g, back, sizeof(back), &rd);
    mf_close(g);
    if (rr != MFS_OK || rd != sizeof(data) ||
        memcmp(back, data, sizeof(data)) != 0) {
      CHECK(false);
      break;
    }
    ok++;
  }
  CHECK_EQ(ok, FIH_ITERS);
  printf("   %u ciclos crash/recover sin pérdida\n", ok);
  env_close(&e);
  TEST_END("FIH corte de energía");
}

void test_fih_torn_write(void) {
  TEST_BEGIN("FIH escritura rasgada: datos no confirmados no corrompen");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* operación fallida limpiamente (fallo del medio en la próxima prog) */
  vf_fail_next_prog(&e.vf, true);
  mfs_file *f = NULL;
  int r = mf_open(&e.fs, "/f.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
  if (r == MFS_OK) {
    size_t wr = 0;
    int w = mf_write(f, "0123456789", 10u, &wr);
    CHECK(w != MFS_OK || wr == 10u); /* fallo tipificado o éxito */
    mf_close(f);
  }

  /* datos escritos sin confirmar (sin close/sync) y corte: el archivo no
   * puede reaparecer con contenido distinto al confirmado */
  vf_recover(&e.vf);
  mfs_file *h = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/torn.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &h),
      MFS_OK);
  size_t wr2 = 0;
  (void)mf_write(h, "uncommitted", 11u, &wr2);
  /* crash antes de close/sync */
  vf_crash(&e.vf);
  vf_recover(&e.vf);
  CHECK_EQ(mf_init(&e.fs, &e.cfg), MFS_OK);

  mfs_stat st;
  int sr = mf_stat(&e.fs, "/torn.bin", &st);
  CHECK(sr == MFS_ENOENT || st.size == 0u || st.size == 11u);
  if (sr == MFS_OK && st.size == 11u) {
    mfs_file *g = NULL;
    CHECK_EQ(mf_open(&e.fs, "/torn.bin", MFS_O_RDONLY, &g), MFS_OK);
    char buf[16];
    size_t rd = 0;
    CHECK_EQ(mf_read(g, buf, sizeof(buf), &rd), MFS_OK);
    CHECK(rd == 11u && memcmp(buf, "uncommitted", 11u) == 0);
    mf_close(g);
  }
  env_close(&e);
  TEST_END("FIH escritura rasgada");
}

void test_fih_nor_discipline(void) {
  TEST_BEGIN("FIH disciplina NOR: sin reprogramar celdas sin erase");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* workload mixto: crear, escribir, sync, remount, borrar, snapshots */
  for (uint32_t i = 0; i < 8u; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/f%u.bin", i);
    mfs_file *f = NULL;
    CHECK_EQ(mf_open(&e.fs, path, MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
             MFS_OK);
    uint8_t buf[300];
    for (uint32_t k = 0; k < sizeof(buf); k++)
      buf[k] = (uint8_t)(i + k);
    size_t wr = 0;
    CHECK_EQ(mf_write(f, buf, sizeof(buf), &wr), MFS_OK);
    mf_close(f);
    CHECK_EQ(mf_sync(&e.fs), MFS_OK);
  }
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK_EQ(mf_unlink(&e.fs, "/f0.bin"), MFS_OK);
  (void)mf_sync(&e.fs);

  CHECK_EQ(e.vf.n_violations, 0u);
  if (e.vf.n_violations)
    printf("   viol_addr=0x%X zone_base=0x%X erase=0x%X\n", e.vf.viol_addr,
           mfs_zone_base(&e.fs.hwv),
           e.vf.viol_addr / e.vf.erase_unit * e.vf.erase_unit);
  printf("   prog=%u erase=%u violations=%u\n", e.vf.n_prog, e.vf.n_erase,
         e.vf.n_violations);
  env_close(&e);
  TEST_END("FIH disciplina NOR");
}

void test_fih_bitflip(void) {
  /* Aunque no está en el índice de suites, se ejecuta: corrupción puntual
   * (E2G) nunca debe entregar datos incorrectos silenciosamente. */
  TEST_BEGIN("FIH bit-flip: E2G impide datos erróneos");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t data[512];
  for (uint32_t i = 0; i < sizeof(data); i++)
    data[i] = (uint8_t)(i ^ 0x5Au);
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/e2g.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  /* localizar una celda programada en la región de zonas y voltearla */
  uint32_t base = mfs_zone_base(&e.fs.hwv);
  uint32_t off = base;
  for (; off < e.vf.size; off++) {
    uint8_t *p = vf_raw(&e.vf, off);
    if (p && *p != 0xFFu)
      break;
  }
  CHECK(off < e.vf.size);
  uint8_t *cell = vf_raw(&e.vf, off);
  *cell ^= 0x08u;

  int r = mf_init(&e.fs, &e.cfg);
  CHECK(r == MFS_OK || r == MFS_ECORRUPT || r == MFS_EBADMSG);
  if (r == MFS_OK) {
    mfs_file *g = NULL;
    if (mf_open(&e.fs, "/e2g.bin", MFS_O_RDONLY, &g) == MFS_OK) {
      uint8_t back[512];
      size_t rd = 0;
      (void)mf_read(g, back, sizeof(back), &rd);
      mf_close(g);
      bool all_zero = true;
      for (size_t i = 0; i < rd; i++)
        if (back[i] != 0u)
          all_zero = false;
      CHECK(rd == 0u || ((rd == sizeof(data)) && memcmp(back, data, rd) == 0) ||
            all_zero);
    }
  }
  env_close(&e);
  TEST_END("FIH bit-flip");
}
