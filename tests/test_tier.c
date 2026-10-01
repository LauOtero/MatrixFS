/* test_tier.c — Tiering heterogéneo T0/T1 (§11.11, §27)
 *
 * T0 = FRAM/MRAM byte-addressable (vFRAM); T1 = NOR (vFlash). El espejo de
 * metadatos calientes en T0 es *best-effort*: si T0 falla, el sistema degrada
 * a mono-medio de forma declarada (nunca silenciosa).
 */
#include "mfs_harness.h"
#include "mfs_test.h"

#define TIER_RAM 262144u /* Extended: HMT exige ≥ 32 KiB en T0 */

/* Recorre el log T0 y devuelve cuántos registros válidos ('M','0' + CRC-32C)
 * encuentra, junto con los tipos vistos. */
static uint32_t hmt_scan_t0(const vfram_t *fr, uint32_t budget, bool *inode,
                            bool *dirent) {
  *inode = false;
  *dirent = false;
  uint32_t off = 0, n = 0;
  while (off + 20u <= budget) {
    const uint8_t *h = vfram_raw((vfram_t *)fr, off);
    if (!h)
      break;
    if (h[0] != 'M' || h[1] != '0')
      break;
    uint16_t len = mfs_ld16(h + 12);
    if (off + 20u + len > budget)
      break;
    uint32_t crc = mfs_crc32c(h, 14u, 0u);
    const uint8_t *pl = vfram_raw((vfram_t *)fr, off + 20u);
    if (len && pl)
      crc = mfs_crc32c(pl, len, crc);
    if (crc == mfs_ld32(h + 14)) {
      n++;
      if (h[3] == MFS_RT_INODE)
        *inode = true;
      if (h[3] == MFS_RT_DIRENT)
        *dirent = true;
    }
    off += 20u + len;
  }
  return n;
}

void test_hmt_activation(void) {
  TEST_BEGIN("HMT: activación del tier T0 (§11.11)");
  mfs_env_t e;

  /* 1) Sin T0 la función no aplica: se declara ENOTSUP, no se finge nada. */
  CHECK(env_open(&e, TIER_RAM, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK(!mfs_hmt_active(&e.fs));
  CHECK_EQ(mfs_hmt_init(&e.fs), MFS_ENOTSUP);
  CHECK(!mfs_hmt_active(&e.fs));
  env_close(&e);

  /* 2) T0 presente y con presupuesto: activo tras el montaje. */
  CHECK(env_open_t0(&e, TIER_RAM, NULL, MFS_TEST_T0_SIZE));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(e.fs.mode, MFS_MODE_EXTENDED);
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK(mfs_hmt_active(&e.fs));
  CHECK_EQ(e.fs.hwv.t0_size, MFS_TEST_T0_SIZE);
  CHECK(e.fs.hwv.t0_write_ns > 0u);
  env_close(&e);

  /* 3) T0 demasiado pequeño para el modo: ENOTVIABLE explícito. */
  CHECK(env_open_t0(&e, TIER_RAM, NULL, 4096u));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(mfs_hmt_init(&e.fs), MFS_ENOTVIABLE);
  CHECK(!mfs_hmt_active(&e.fs));
  env_close(&e);
  TEST_END("HMT activación");
}

void test_hmt_metadata_in_t0(void) {
  TEST_BEGIN("HMT: espejo de metadatos en T0 y recuperación (§11.11)");
  mfs_env_t e;
  CHECK(env_open_t0(&e, TIER_RAM, NULL, MFS_TEST_T0_SIZE));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(env_remount(&e), MFS_OK); /* mf_init activa el tier */
  CHECK(mfs_hmt_active(&e.fs));

  /* crear metadatos calientes: inodo + entrada de directorio */
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/hmt.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, "tier-0", 6u, &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  /* el T0 debe contener registros válidos de tipo INODE y DIRENT */
  CHECK(e.fs.hmt_t0_off > 0u);
  bool inode = false, dirent = false;
  uint32_t recs = hmt_scan_t0(&e.fr, MFS_TEST_T0_SIZE, &inode, &dirent);
  CHECK(recs > 0u);
  CHECK(inode);
  CHECK(dirent);

  /* el escaneo de T0 es reproducible y no rompe el sistema de archivos */
  CHECK_EQ(mfs_hmt_scan(&e.fs), MFS_OK);
  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/hmt.txt", &st), MFS_OK);
  CHECK_EQ(st.size, 6u);
  env_close(&e);
  TEST_END("HMT metadatos en T0");
}

void test_hmt_t0_failure_degrades(void) {
  TEST_BEGIN("HMT: fallo de T0 ⇒ degradación a mono-medio");
  mfs_env_t e;
  CHECK(env_open_t0(&e, TIER_RAM, NULL, MFS_TEST_T0_SIZE));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK(mfs_hmt_active(&e.fs));

  /* T0 deja de responder: el espejo falla y el tier se desactiva, pero la
   * escritura durable en T1 debe completarse igualmente. */
  vfram_crash(&e.fr);
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/deg.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, "durable-en-t1", 13u, &wr), MFS_OK);
  mf_close(f);
  CHECK(!mfs_hmt_active(&e.fs)); /* degradación declarada, no silenciosa */

  /* el dato confirmado sigue accesible */
  mfs_file *g = NULL;
  CHECK_EQ(mf_open(&e.fs, "/deg.txt", MFS_O_RDONLY, &g), MFS_OK);
  char buf[16];
  size_t rd = 0;
  CHECK_EQ(mf_read(g, buf, sizeof(buf), &rd), MFS_OK);
  CHECK(rd == 13u && memcmp(buf, "durable-en-t1", 13u) == 0);
  mf_close(g);

  /* T0 vuelve: el tier se reactiva en el siguiente montaje */
  vfram_recover(&e.fr);
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK(mfs_hmt_active(&e.fs));
  mfs_stat st2;
  CHECK_EQ(mf_stat(&e.fs, "/deg.txt", &st2), MFS_OK);
  CHECK_EQ(st2.size, 13u);
  env_close(&e);
  TEST_END("HMT degradación");
}
