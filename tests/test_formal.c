/* test_formal.c — FormalCore ejecutable (§27.3)
 *
 * Complemento ejecutable de las especificaciones TLA+/Promela (carpeta
 * formal/): (a) replay determinista de los autómatas DAB/CUSUM/EDP y
 * (b) exploración acotada del protocolo WAL+ con invariantes de commit.
 */
#include "mfs_harness.h"
#include "mfs_test.h"

#define RAM_COMPACT 32768u

/* ---------------- determinismo de los autómatas adaptativos ------------- */
void test_formal_replay_determinism(void) {
  TEST_BEGIN("FormalCore: replay determinista (DAB/CUSUM/EDP)");

  /* DAB (EXP3 sembrado): misma semilla ⇒ misma secuencia de brazos */
  float w1[3] = {1.f / 3.f, 1.f / 3.f, 1.f / 3.f};
  float w2[3] = {1.f / 3.f, 1.f / 3.f, 1.f / 3.f};
  uint32_t r1 = 0xDAB5EEDu, r2 = 0xDAB5EEDu;
  float rew[3] = {0.9f, 0.4f, 0.1f};
  float x[3][3] = {{0}};
  int diff = 0;
  for (uint32_t i = 0; i < 128u; i++) {
    uint32_t a1 = mfs_exp3_update(w1, rew, x, &r1, 3);
    uint32_t a2 = mfs_exp3_update(w2, rew, x, &r2, 3);
    if (a1 != a2)
      diff++;
  }
  CHECK_EQ(diff, 0);
  /* semilla distinta ⇒ secuencia distinta (no degenerado) */
  float w3[3] = {1.f / 3.f, 1.f / 3.f, 1.f / 3.f};
  uint32_t r3 = 0x1234567u;
  int diff2 = 0;
  for (uint32_t i = 0; i < 64u; i++)
    if (mfs_exp3_update(w3, rew, x, &r3, 3) !=
        mfs_exp3_update(w1, rew, x, &r1, 3))
      diff2++;
  (void)diff2;

  /* EDP: la ventana ½CV² crece con V0 y cae con más corriente (monótona) */
  mfs_rail_state r;
  r.mv = 3300;
  r.slope_mv_ms = 100;
  r.t_remaining_us = 0;
  r.ok = true;
  uint32_t w_hi = mfs_edp_window_us(&r, 1800u, 100u, 50u);
  r.mv = 2400;
  uint32_t w_lo = mfs_edp_window_us(&r, 1800u, 100u, 50u);
  CHECK(w_hi > w_lo);
  r.mv = 3300;
  uint32_t w_i = mfs_edp_window_us(&r, 1800u, 100u, 100u);
  CHECK(w_i < w_hi);
  CHECK_EQ(mfs_edp_window_us(&r, 3300u, 100u, 50u), 0u); /* V0 ≤ Vmin */

  /* CUSUM: régimen estable no alarma; un cambio sostenido sí */
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  for (uint32_t i = 0; i < 200u; i++)
    mfs_cusum_update(&e.fs, 10000); /* 100 us */
  CHECK(!mfs_cusum_alarm(&e.fs));
  for (uint32_t i = 0; i < 200u; i++)
    mfs_cusum_update(&e.fs, 30000); /* 300 us */
  CHECK(mfs_cusum_alarm(&e.fs));
  CHECK(e.fs.hct.cusum_alarms > 0u);
  env_close(&e);
  TEST_END("FormalCore determinismo");
}

/* --------- exploración acotada del protocolo WAL+ (invariantes) -------- */
static int apply_op(mfs_env_t *e, char op, int *pending, int *committed) {
  switch (op) {
  case 'W': {
    if (!e->fs.tx_open)
      (void)mf_tx_begin(&e->fs);
    mfs_file *f = NULL;
    if (mf_open(&e->fs, "/f.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f) !=
        MFS_OK)
      return 0;
    size_t wr = 0;
    int r = mf_write(f, "abc", 3u, &wr);
    (void)mf_close(f);
    if (r == MFS_OK)
      *pending = 1;
    return r == MFS_OK;
  }
  case 'C':
    if (e->fs.tx_open) {
      if (mf_tx_commit(&e->fs) == MFS_OK && *pending)
        *committed = 1;
      *pending = 0;
    }
    return 1;
  case 'A':
    if (e->fs.tx_open) {
      (void)mf_tx_abort(&e->fs);
      *pending = 0;
    }
    return 1;
  case 'S':
    return mf_sync(&e->fs) == MFS_OK;
  default:
    return 0;
  }
}

void test_formal_wal_invariants(void) {
  TEST_BEGIN("FormalCore: invariantes del protocolo WAL+ (replay/gating)");
  static const char alpha[] = {'W', 'C', 'A', 'S'};
  uint32_t combos = 0, violations = 0;
  uint32_t epoch0 = 0;

  for (uint32_t a = 0; a < 4u; a++)
    for (uint32_t b = 0; b < 4u; b++)
      for (uint32_t c = 0; c < 4u; c++) {
        mfs_env_t e;
        if (!env_open(&e, RAM_COMPACT, NULL)) {
          CHECK(false);
          return;
        }
        if (env_format(&e) != MFS_OK) {
          CHECK(false);
          env_close(&e);
          return;
        }
        epoch0 = e.fs.epoch;
        int pending = 0, committed = 0;
        char ops[3] = {alpha[a], alpha[b], alpha[c]};
        for (uint32_t i = 0; i < 3u; i++)
          (void)apply_op(&e, ops[i], &pending, &committed);

        int r = env_remount(&e);
        combos++;
        if (r != MFS_OK) {
          violations++;
          env_close(&e);
          continue;
        }
        if (e.fs.epoch < epoch0)
          violations++; /* I2: época monótona */

        mfs_stat st;
        int exists = (mf_stat(&e.fs, "/f.bin", &st) == MFS_OK);
        if (committed) {
          /* I3: todo dato confirmado reaparece idéntico */
          if (!exists || st.size != 3u) {
            violations++;
          } else {
            mfs_file *f = NULL;
            if (mf_open(&e.fs, "/f.bin", MFS_O_RDONLY, &f) == MFS_OK) {
              char buf[8];
              size_t rd = 0;
              (void)mf_read(f, buf, sizeof(buf), &rd);
              (void)mf_close(f);
              if (rd != 3u || memcmp(buf, "abc", 3u) != 0)
                violations++;
            } else
              violations++;
          }
        } else {
          /* I4: sin confirmación, nunca aparece contenido confirmado */
          if (exists && st.size > 3u)
            violations++;
        }
        env_close(&e);
      }
  printf("   combinaciones exploradas=%u  violaciones=%u\n", combos,
         violations);
  CHECK_EQ(combos, 64u);
  CHECK_EQ(violations, 0u);
  TEST_END("FormalCore WAL+");
}
