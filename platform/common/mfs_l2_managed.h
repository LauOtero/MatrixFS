/* mfs_l2_managed.h — driver L2 genérico para medios GESTIONADOS (MFS-CAP-001).
 *
 * Los medios gestionados (SD, eMMC, UFS, USB, NVMe, SATA) ya traen su propio
 * FTL: el dispositivo resuelve mapeo físico, desgaste y reubicación. Este
 * adaptador presenta la API de "sectores" del BSP/SDK como un `mfs_l2_driver`
 * de MatrixFS, de modo que el núcleo asigna por clúster y **no duplica** el
 * trabajo del controlador (§3.2, MFS-CAP-001).
 *
 * El adaptador:
 *   - traduce byte-offset ⇄ LBA y hace RMW alineado a sector (el medio sólo
 *     admite escritura de sector completo);
 *   - emite TRIM/UNMAP en `erase()` cuando el dispositivo lo expone (en un
 *     medio gestionado no hay borrado previo obligatorio);
 *   - no reserva memoria: el *bounce buffer* para RMW lo aporta el integrador.
 *
 * El integrador sólo implementa 2-3 callbacks de sector sobre la API nativa
 * (p. ej. `esp_partition_*`, `sdmmc_read_sectors`, `nvme_read`, `ufs_read`).
 */
#ifndef MFS_L2_MANAGED_H
#define MFS_L2_MANAGED_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs/mfs_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== API de dispositivo que aporta el BSP/SDK ==== */
typedef struct mfs_managed_dev {
  /* Lectura/escritura de `count` sectores desde el LBA `lba` (0-based). */
  mfs_st (*read_sectors)(void *ctx, uint32_t lba, uint32_t count, void *dst);
  mfs_st (*write_sectors)(void *ctx, uint32_t lba, uint32_t count,
                          const void *src);
  /* Discard/UNMAP/TRIM de `count` sectores. NULL ⇒ no soportado. */
  mfs_st (*trim_sectors)(void *ctx, uint32_t lba, uint32_t count);
  /* Vuelca la caché del dispositivo (fsync). NULL ⇒ no aplica. */
  mfs_st (*flush)(void *ctx);
  void *ctx;

  uint16_t sector_size;  /* 512 o 4096; potencia de dos ≥ 256 */
  uint32_t sector_count; /* capacidad en sectores */

  uint32_t t_read_max_us;
  uint32_t t_write_max_us;
  uint32_t t_trim_max_us;

  /* Buffer de RMW: obligatorio, ≥ sector_size (lo aporta el integrador). */
  uint8_t *bounce;
  uint32_t bounce_len;

  const char *name; /* informativo (diagnóstico) */
} mfs_managed_dev_t;

/* ==== Adaptador listo para mfs_config ==== */
typedef struct mfs_managed_l2 {
  mfs_l2_driver drv;   /* driver L2 a registrar en mfs_config.drv */
  mfs_media_geom geom; /* geometría derivada del dispositivo       */
  mfs_managed_dev_t *dev;
} mfs_managed_l2_t;

/* Prepara el adaptador sobre `dev`. `media` debe ser un medio gestionado
 * (mf_media_profile(media)->engine == MFS_ENGINE_MANAGED). */
mfs_st mfs_managed_l2_init(mfs_managed_l2_t *l2, mfs_managed_dev_t *dev,
                           mfs_media_type_t media);

#ifdef __cplusplus
}
#endif
#endif /* MFS_L2_MANAGED_H */
