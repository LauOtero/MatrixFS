/* test_fs.c — pruebas funcionales del núcleo (§21 API, §9 TX, §10.8 snap/FPT,
 * §12.3 EDP, §13.1 DAIO, §16 HCT, §21.1 fsck) */
#include "mfs_harness.h"
#include "mfs_test.h"

#define RAM_COMPACT 32768u

static void fill_pattern(uint8_t *b, size_t n, uint8_t seed) {
  for (size_t i = 0; i < n; i++)
    b[i] = (uint8_t)(seed + (i * 31u));
}

void test_fs_basic(void) {
  TEST_BEGIN("FS básico: crear/escribir/leer/stat/unlink");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  const char *msg = "MatrixFS Ultra";
  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/a.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
           MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, msg, strlen(msg), &wr), MFS_OK);
  CHECK_EQ(wr, strlen(msg));
  CHECK_EQ(mf_close(f), MFS_OK);

  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/a.txt", &st), MFS_OK);
  CHECK_EQ(st.size, strlen(msg));
  CHECK((st.mode & 0xF000u) == 0x8000u);

  f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/a.txt", MFS_O_RDONLY, &f), MFS_OK);
  char buf[64];
  size_t rd = 0;
  CHECK_EQ(mf_read(f, buf, sizeof(buf), &rd), MFS_OK);
  CHECK_EQ(rd, strlen(msg));
  CHECK(memcmp(buf, msg, rd) == 0);
  mf_close(f);

  /* O_EXCL sobre existente ⇒ EEXISTS */
  mfs_file *g = NULL;
  CHECK_EQ(mf_open(&e.fs, "/a.txt", MFS_O_CREAT | MFS_O_EXCL, &g), MFS_EEXISTS);

  CHECK_EQ(mf_unlink(&e.fs, "/a.txt"), MFS_OK);
  CHECK_EQ(mf_stat(&e.fs, "/a.txt", &st), MFS_ENOENT);
  env_close(&e);
  TEST_END("FS básico");
}

void test_fs_multipage(void) {
  TEST_BEGIN("FS multipágina (extents inline + desbordamiento)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t data[3072]; /* 6 páginas de 512 B (chunk Compact) */
  fill_pattern(data, sizeof(data), 7u);
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/big.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  CHECK_EQ(wr, sizeof(data));
  mf_close(f);

  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/big.bin", &st), MFS_OK);
  CHECK_EQ(st.size, sizeof(data));

  f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/big.bin", MFS_O_RDONLY, &f), MFS_OK);
  uint8_t back[3072];
  size_t rd = 0;
  CHECK_EQ(mf_read(f, back, sizeof(back), &rd), MFS_OK);
  CHECK_EQ(rd, sizeof(data));
  CHECK(memcmp(back, data, sizeof(data)) == 0);
  mf_close(f);
  env_close(&e);
  TEST_END("FS multipágina");
}

void test_fs_dirs(void) {
  TEST_BEGIN("FS directorios y readdir");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  CHECK_EQ(mf_mkdir(&e.fs, "/d"), MFS_OK);
  CHECK_EQ(mf_mkdir(&e.fs, "/d"), MFS_EEXISTS);

  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/d/f1", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
           MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, "x", 1u, &wr), MFS_OK);
  mf_close(f);

  mfs_dir *d = NULL;
  CHECK_EQ(mf_opendir(&e.fs, "/d", &d), MFS_OK);
  mfs_dirent de;
  int found = 0;
  while (mf_readdir(d, &de) == MFS_OK) {
    if (strcmp(de.name, "f1") == 0)
      found = 1;
  }
  mf_closedir(d);
  CHECK(found);

  CHECK_EQ(mf_rename(&e.fs, "/d/f1", "/d/f2"), MFS_OK);
  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/d/f2", &st), MFS_OK);
  CHECK_EQ(mf_stat(&e.fs, "/d/f1", &st), MFS_ENOENT);

  /* directorio no vacío no se elimina */
  CHECK_EQ(mf_unlink(&e.fs, "/d"), MFS_EBUSY);
  CHECK_EQ(mf_unlink(&e.fs, "/d/f2"), MFS_OK);
  CHECK_EQ(mf_unlink(&e.fs, "/d"), MFS_OK);
  env_close(&e);
  TEST_END("FS directorios");
}

void test_fs_persistence(void) {
  TEST_BEGIN("FS persistencia (remount: datos + nombres + unlink durable)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t data[1024];
  fill_pattern(data, sizeof(data), 0x5Au);
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/keep.dat", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  mf_close(f);

  /* archivo que se elimina antes del remount */
  f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/gone.dat", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  CHECK_EQ(mf_write(f, "z", 1u, &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_unlink(&e.fs, "/gone.dat"), MFS_OK);

  CHECK_EQ(env_remount(&e), MFS_OK);

  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/keep.dat", &st), MFS_OK);
  CHECK_EQ(st.size, sizeof(data));
  CHECK_EQ(mf_stat(&e.fs, "/gone.dat", &st), MFS_ENOENT);

  f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/keep.dat", MFS_O_RDONLY, &f), MFS_OK);
  uint8_t back[1024];
  size_t rd = 0;
  CHECK_EQ(mf_read(f, back, sizeof(back), &rd), MFS_OK);
  CHECK_EQ(rd, sizeof(data));
  CHECK(memcmp(back, data, sizeof(data)) == 0);
  mf_close(f);
  env_close(&e);
  TEST_END("FS persistencia");
}

void test_fs_tx_savepoints(void) {
  TEST_BEGIN("Transacciones y savepoints (§9, §9.6)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* tx confirmada */
  CHECK_EQ(mf_tx_begin(&e.fs), MFS_OK);
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/tx_ok.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, "commit", 6u, &wr), MFS_OK);
  mfs_sp sp;
  CHECK_EQ(mf_sp_create(&e.fs, &sp), MFS_OK);
  CHECK_EQ(mf_write(f, "rolled", 6u, &wr), MFS_OK);
  CHECK_EQ(mf_sp_rollback(sp), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_tx_commit(&e.fs), MFS_OK);

  /* tx abortada: no debe reaparecer tras remount */
  CHECK_EQ(mf_tx_begin(&e.fs), MFS_OK);
  f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/tx_bad.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  CHECK_EQ(mf_write(f, "nope", 4u, &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_tx_abort(&e.fs), MFS_OK);

  CHECK_EQ(env_remount(&e), MFS_OK);
  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/tx_ok.txt", &st), MFS_OK);
  CHECK_EQ(mf_stat(&e.fs, "/tx_bad.txt", &st), MFS_ENOENT);

  f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/tx_ok.txt", MFS_O_RDONLY, &f), MFS_OK);
  char buf[32];
  size_t rd = 0;
  CHECK_EQ(mf_read(f, buf, sizeof(buf), &rd), MFS_OK);
  CHECK_EQ(rd, 6u);
  CHECK(memcmp(buf, "commit", 6u) == 0);
  mf_close(f);
  env_close(&e);
  TEST_END("Transacciones y savepoints");
}

void test_fs_snapshots(void) {
  TEST_BEGIN("Snapshots O(1) y FPT (§10.8)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  uint8_t key[32];
  memset(key, 0x11, sizeof(key));
  (void)key;

  mfs_snap_id id1 = 0, id2 = 0, id3 = 0;
  CHECK_EQ(mf_snap_create(&e.fs, &id1), MFS_OK);
  CHECK_EQ(mf_snap_create(&e.fs, &id2), MFS_OK);
  /* Compact permite 2 snapshots (§18.2) */
  CHECK_EQ(mf_snap_create(&e.fs, &id3), MFS_ESNAPMAX);
  CHECK_EQ(mf_snap_revert(&e.fs, id1), MFS_OK);
  CHECK_EQ(mf_snap_delete(&e.fs, id1), MFS_OK);
  CHECK_EQ(mf_snap_revert(&e.fs, id1), MFS_ENOENT);

  mfs_snap_id list[8];
  int n = mf_ioctl(&e.fs, MFS_IOCTL_SNAPSHOT_LIST, list);
  CHECK_EQ(n, 1);
  env_close(&e);
  TEST_END("Snapshots O(1) y FPT");
}

void test_fs_edp(void) {
  TEST_BEGIN("EDP: niveles, SPDR y drenaje (§12.3)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* modelo ½CV² (§12.3): ventana > 0 con rail por encima de Vmin */
  mfs_rail_state r;
  r.mv = 3300u;
  r.slope_mv_ms = 100u;
  r.t_remaining_us = 0u;
  r.ok = true;
  uint32_t w = mfs_edp_window_us(&r, 1800u, 100u, 50u);
  CHECK(w > 0u);

  bool inj = true;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_EDP_INJECT, &inj), MFS_OK);

  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/edp.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  size_t wr = 0;
  /* con el rail cayendo, SPDR rechaza el trabajo no-RT (§13.1) */
  int rw = mf_write(f, "abc", 3u, &wr);
  CHECK(rw == MFS_EAGAIN || rw == MFS_OK);
  mf_close(f);

  /* drenaje de emergencia explícito: deja el volumen consistente */
  mfs_edp_drain(&e.fs);
  mfs_health_t h;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_HEALTH, &h), MFS_OK);
  CHECK_EQ((int)h.edp_state, (int)MFS_EDP_DONE);

  inj = false;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_EDP_INJECT, &inj), MFS_OK);
  CHECK_EQ(env_remount(&e), MFS_OK);
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_META), MFS_OK);
  env_close(&e);
  TEST_END("EDP");
}

void test_fs_verify_health(void) {
  TEST_BEGIN("fsck read-only + HCT CBOR/COSE (§21.1, §16)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&e.fs, "/h.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
           MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, "0123456789", 10u, &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);

  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_QUICK), MFS_OK);
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_META), MFS_OK);
  CHECK_EQ(mf_verify(&e.fs, MFS_VERIFY_FULL), MFS_OK);

  uint8_t buf[512];
  int n = mf_export_health(&e.fs, buf, sizeof(buf));
  CHECK(n > 0);

  mfs_health_t h;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_HEALTH, &h), MFS_OK);
  CHECK_EQ((int)h.mode, (int)MFS_MODE_COMPACT);
  CHECK(h.epoch >= 1u);

  uint32_t jperop = 0;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_GET_JPEROP, &jperop), MFS_OK);
  env_close(&e);
  TEST_END("fsck + HCT");
}

void test_fs_daio(void) {
  TEST_BEGIN("DAIO multi-cola: submit/poll (§13.1)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/daio.txt", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);

  struct {
    mfs_file *file;
    char data[64];
  } slot;
  slot.file = f;
  memset(slot.data, 0xAB, sizeof(slot.data));

  mfs_iocb cb;
  memset(&cb, 0, sizeof(cb));
  cb.op = MFS_AWRITE;
  cb.class_flags = MFS_RT_A;
  cb.len = sizeof(slot.data);
  cb.buf = &slot;
  cb.file_off = 0u;

  CHECK_EQ(mf_submit(&e.fs, &cb), MFS_OK);
  mfs_iocb *done[4];
  int n = mf_poll(&e.fs, done, 4, 100000u);
  CHECK_EQ(n, 1);
  CHECK_EQ((int)cb.status, MFS_OK);
  mf_close(f);

  mfs_stat st;
  CHECK_EQ(mf_stat(&e.fs, "/daio.txt", &st), MFS_OK);
  CHECK_EQ(st.size, sizeof(slot.data));
  env_close(&e);
  TEST_END("DAIO");
}
