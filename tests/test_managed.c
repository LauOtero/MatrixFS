/* test_managed.c — medio gestionado (SD/eMMC/UFS/SATA/NVMe) sobre el adaptador
 * L2 genérico `mfs_l2_managed` (§3.2, §18, §20.2, MFS-CAP-001).
 *
 * Verifica: rechazo de medios no gestionados, geometría derivada, montaje y
 * formato, E/S con escritura NO alineada a sector (RMW), emisión de TRIM y
 * persistencia tras remontaje.
 */
#include "mfs_harness.h"
#include "mfs_test.h"
#include "vblk_sim.h"

#include <string.h>

void test_managed_media(void) {
  TEST_BEGIN("Medio gestionado (eMMC/UFS/SATA/NVMe): adaptador L2, RMW y TRIM");
  static vblk_t b;
  CHECK(vblk_init(&b, 4096u, 512u)); /* 2 MiB con sectores de 512 B */

  static mfs_managed_dev_t dev;
  static uint8_t bounce[4096];
  vblk_attach(&b, &dev);
  dev.bounce = bounce;
  dev.bounce_len = (uint32_t)sizeof(bounce);

  static mfs_managed_l2_t l2;
  /* Un medio RAW (NOR/NAND) no se sirve con este adaptador: su FTL es del
   * núcleo (motor RAW). */
  CHECK_EQ(mfs_managed_l2_init(&l2, &dev, MFS_MEDIA_NOR_SPI), MFS_EINVAL);
  /* Un medio gestionado sí, y con la geometría derivada del dispositivo. */
  CHECK_EQ(mfs_managed_l2_init(&l2, &dev, MFS_MEDIA_EMMC), MFS_OK);
  CHECK_EQ((l2.geom.flags1 & MFS_HWV1_MANAGED), MFS_HWV1_MANAGED);
  CHECK_EQ((l2.geom.flags0 & MFS_HWV0_ECC_ON_DIE), MFS_HWV0_ECC_ON_DIE);
  CHECK_EQ(l2.geom.size, 4096u * 512u);
  CHECK_EQ(l2.geom.erase_unit, 4096u);
  CHECK(l2.drv.read != NULL && l2.drv.prog != NULL && l2.drv.erase != NULL);

  static mfs_config cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.drv = &l2.drv;
  cfg.geom = &l2.geom;
  cfg.ram_total = 32768u;
  cfg.arch_class = 2u; /* 32-bit */
  cfg.forced_mode = MFS_MODE_UNSUPPORTED;
  cfg.suite_preferred = 0xFFu;

  static mf_t fs;
  fs.cfg = &cfg;
  CHECK_EQ(mf_format(&fs, NULL), MFS_OK);
  CHECK(b.n_trim > 0u); /* el formato emite TRIM al liberar zonas */

  /* E/S: 700 B no es múltiplo del sector ⇒ el adaptador hace RMW */
  uint8_t data[700];
  size_t i;
  for (i = 0; i < sizeof(data); i++)
    data[i] = (uint8_t)(i * 31u + 7u);
  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&fs, "/blk.bin", MFS_O_CREAT | MFS_O_RDWR, &f), MFS_OK);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  CHECK_EQ(wr, sizeof(data));
  CHECK_EQ(mf_close(f), MFS_OK);
  mf_deinit(&fs);

  /* Persistencia: remontaje sobre el mismo dispositivo */
  fs.cfg = &cfg;
  CHECK_EQ(mf_init(&fs, &cfg), MFS_OK);
  mfs_stat st;
  CHECK_EQ(mf_stat(&fs, "/blk.bin", &st), MFS_OK);
  CHECK_EQ(st.size, (uint32_t)sizeof(data));
  uint8_t got[700];
  memset(got, 0, sizeof(got));
  CHECK_EQ(mf_open(&fs, "/blk.bin", MFS_O_RDONLY, &f), MFS_OK);
  size_t rd = 0;
  CHECK_EQ(mf_read(f, got, sizeof(got), &rd), MFS_OK);
  CHECK_EQ(rd, sizeof(data));
  CHECK(memcmp(got, data, sizeof(data)) == 0);
  CHECK_EQ(mf_close(f), MFS_OK);
  mf_deinit(&fs);

  /* Fallo de medio tipificado: no corrompe, devuelve error */
  vblk_attach(&b, &dev);
  dev.bounce = bounce;
  dev.bounce_len = (uint32_t)sizeof(bounce);
  CHECK_EQ(mfs_managed_l2_init(&l2, &dev, MFS_MEDIA_NVME), MFS_OK);
  b.crashed = true;
  CHECK_EQ(l2.drv.read(&l2, 0u, got, 512u), MFS_EIO);
  b.crashed = false;

  vblk_free(&b);
  TEST_END("Medio gestionado");
}
