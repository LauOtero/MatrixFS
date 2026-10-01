/* mfs_harness.c — implementación del entorno de test sobre vFlash/vFRAM */
#include "mfs_harness.h"
#include <string.h>

bool env_open(mfs_env_t *e, uint32_t ram_total, const uint8_t *key) {
  memset(e, 0, sizeof(*e));
  if (!vf_init(&e->vf, MFS_TEST_SIZE, MFS_TEST_SECTOR))
    return false;
  vf_geom(&e->vf, &e->g);
  e->cfg.drv = vf_driver(&e->vf);
  e->cfg.geom = &e->g;
  e->cfg.ram_total = ram_total;
  e->cfg.arch_class = 2u;                    /* 32-bit */
  e->cfg.forced_mode = MFS_MODE_UNSUPPORTED; /* auto (§6.2) */
  e->cfg.key = key;
  return true;
}

bool env_open_t0(mfs_env_t *e, uint32_t ram_total, const uint8_t *key,
                 uint32_t t0_size) {
  if (!env_open(e, ram_total, key))
    return false;
  if (!vfram_init(&e->fr, t0_size)) {
    vf_free(&e->vf);
    return false;
  }
  vfram_geom(&e->fr, &e->g_t0);
  e->cfg.drv_t0 = vfram_driver(&e->fr);
  e->cfg.geom_t0 = &e->g_t0;
  e->has_t0 = true;
  return true;
}

int env_format(mfs_env_t *e) {
  e->fs.cfg = &e->cfg; /* blanco: mf_format lee la config de la instancia */
  int r = mf_format(&e->fs, NULL);
  if (r == MFS_OK)
    e->formatted = true;
  return r;
}

int env_remount(mfs_env_t *e) {
  mf_deinit(&e->fs);
  e->fs.cfg = &e->cfg;
  return mf_init(&e->fs, &e->cfg);
}

void env_close(mfs_env_t *e) {
  if (e->formatted)
    mf_deinit(&e->fs);
  vf_free(&e->vf);
  if (e->has_t0)
    vfram_free(&e->fr);
}

void env_set_assets(mfs_env_t *e, uint8_t flags2, uint8_t flags3) {
  e->fs.hwv.flags2 |= flags2;
  e->fs.hwv.flags3 |= flags3;
}
