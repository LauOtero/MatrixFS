/* mfs_stm32_sd.c — SD / eMMC como medio de MatrixFS sobre HAL_SD / HAL_MMC.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Un SD/eMMC es un medio GESTIONADO: el propio dispositivo resuelve el mapeo
 * fisico, el desgaste y la reubicacion. Reutiliza el adaptador
 * platform/common/mfs_l2_managed.c, que presenta la API de sectores como un
 * `mfs_l2_driver` de MatrixFS, hace el RMW alineado a sector con un *bounce
 * buffer* y emite TRIM/UNMAP cuando el dispositivo lo soporta. Aqui solo se
 * aportan los 2-3 callbacks de sector sobre el HAL, mas el buffer de RMW.
 *
 * CACHE (Cortex-M7, familias H7): el HAL de SDMMC con DMA limpia/invalida la
 * D-cache en sus variantes _DMA, pero el *bounce buffer* y los buffers de la
 * aplicacion deben estar en memoria coherente (MPU). El README lo documenta como
 * requisito del BSP; el port no puede garantizarlo desde aqui.
 */
#include "mfs_stm32_backend.h"

#include <string.h>

#include "mfs_stm32_hal.h"
#include "mfs_l2_managed.h"

#ifndef MFS_STM32_SD_TIMEOUT_MS
#define MFS_STM32_SD_TIMEOUT_MS 5000u
#endif

/* Buffer de RMW del adaptador gestionado: un sector como minimo. Se declara
 * alineado a 4 B (el DMA del SDMMC lo prefiere) y estatico (sin heap). */
#define MFS_STM32_SD_BOUNCE 512u
static uint32_t s_bounce_word[MFS_STM32_SD_BOUNCE / 4u];

/* =====================================================================
 * Callbacks de sector (HAL_SD / HAL_MMC)
 *
 * Diferencias que se aislan aqui:
 *   · HAL_MMC_* toma `uint32_t *` y HAL_SD_* toma `uint8_t *` para los datos;
 *   · HAL_*_Erase toma un RANGO DE DIRECCIONES (StartAddr, EndAddr), no
 *     (lba, numero de bloques): hay que convertir;
 *   · la constante de estado "transferencia lista" cambia de nombre.
 * ===================================================================== */
#if defined(MFS_STM32_USE_MMC)
#define MFS_SD_HANDLE_T MMC_HandleTypeDef
#define MFS_SD_READY HAL_MMC_CARD_TRANSFER
#define MFS_SD_READ(h, lba, n, p)                                              \
  HAL_MMC_ReadBlocks((h), (uint32_t *)(p), (uint32_t)(lba), (uint32_t)(n),     \
                     MFS_STM32_SD_TIMEOUT_MS)
#define MFS_SD_WRITE(h, lba, n, p)                                             \
  HAL_MMC_WriteBlocks((h), (uint32_t *)(p), (uint32_t)(lba), (uint32_t)(n),    \
                      MFS_STM32_SD_TIMEOUT_MS)
#define MFS_SD_GETSTATE(h) HAL_MMC_GetCardState(h)
#define MFS_SD_ERASE(h, lba, n)                                                \
  HAL_MMC_Erase((h), (uint32_t)(lba), (uint32_t)((lba) + (n) - 1u))
#else
#define MFS_SD_HANDLE_T SD_HandleTypeDef
#define MFS_SD_READY HAL_SD_CARD_TRANSFER
#define MFS_SD_READ(h, lba, n, p)                                              \
  HAL_SD_ReadBlocks((h), (uint8_t *)(p), (uint32_t)(lba), (uint32_t)(n),       \
                    MFS_STM32_SD_TIMEOUT_MS)
#define MFS_SD_WRITE(h, lba, n, p)                                             \
  HAL_SD_WriteBlocks((h), (uint8_t *)(p), (uint32_t)(lba), (uint32_t)(n),      \
                     MFS_STM32_SD_TIMEOUT_MS)
#define MFS_SD_GETSTATE(h) HAL_SD_GetCardState(h)
#define MFS_SD_ERASE(h, lba, n)                                                \
  HAL_SD_Erase((h), (uint32_t)(lba), (uint32_t)((lba) + (n) - 1u))
#endif

static mfs_st sd_read_sectors(void *ctx, uint32_t lba, uint32_t count,
                              void *dst) {
  MFS_SD_HANDLE_T *h = (MFS_SD_HANDLE_T *)ctx;
  if (!h || !dst || count == 0u)
    return MFS_EINVAL;
  if (MFS_SD_READ(h, lba, count, dst) != HAL_OK)
    return MFS_EIO;
  /* Se comprueba el estado para no devolver datos de una lectura rechazada. */
  return (MFS_SD_GETSTATE(h) == MFS_SD_READY) ? MFS_OK : MFS_EIO;
}

static mfs_st sd_write_sectors(void *ctx, uint32_t lba, uint32_t count,
                               const void *src) {
  MFS_SD_HANDLE_T *h = (MFS_SD_HANDLE_T *)ctx;
  if (!h || !src || count == 0u)
    return MFS_EINVAL;
  if (MFS_SD_WRITE(h, lba, count, src) != HAL_OK)
    return MFS_EIO;
  /* BARRERA WOB (§20.3): no retornar hasta que el dispositivo haya terminado su
   * programacion interna. */
  return (MFS_SD_GETSTATE(h) == MFS_SD_READY) ? MFS_OK : MFS_EIO;
}

static mfs_st sd_trim_sectors(void *ctx, uint32_t lba, uint32_t count) {
  MFS_SD_HANDLE_T *h = (MFS_SD_HANDLE_T *)ctx;
  if (!h || count == 0u)
    return MFS_EINVAL;
  /* Si la tarjeta no soporta ERASE, se devuelve ENOTSUP y el adaptador no
   * anuncia la capacidad (MFS-HW-001: no declarar lo que no se ejecuta). */
  if (MFS_SD_ERASE(h, lba, count) != HAL_OK)
    return MFS_ENOTSUP;
  return MFS_OK;
}

static mfs_st sd_flush(void *ctx) {
  MFS_SD_HANDLE_T *h = (MFS_SD_HANDLE_T *)ctx;
  if (!h)
    return MFS_EINVAL;
  return (MFS_SD_GETSTATE(h) == HAL_SD_CARD_TRANSFER) ? MFS_OK : MFS_EIO;
}

/* =====================================================================
 * Preparacion
 * ===================================================================== */
static mfs_managed_dev_t s_dev;
static mfs_managed_l2_t s_l2;

mfs_st mfs_stm32_sd_prepare(const mfs_stm32_cfg *cfg,
                            const mfs_l2_driver **out_l2,
                            const mfs_media_geom **out_geom) {
  if (!cfg || !out_l2 || !out_geom || !cfg->sd)
    return MFS_EINVAL;

  MFS_SD_HANDLE_T *h = (MFS_SD_HANDLE_T *)cfg->sd;

  memset(&s_dev, 0, sizeof(s_dev));
  s_dev.read_sectors = sd_read_sectors;
  s_dev.write_sectors = sd_write_sectors;
  s_dev.trim_sectors = sd_trim_sectors;
  s_dev.flush = sd_flush;
  s_dev.ctx = h;
  s_dev.bounce = (uint8_t *)s_bounce_word;
  s_dev.bounce_len = (uint32_t)sizeof(s_bounce_word);
  s_dev.name = "stm32-sdmmc";

  /* Geometria: la declarada manda; lo que falte se deriva del propio
   * dispositivo (CubeMX debe haber completado la inicializacion antes). */
  uint16_t ssz = cfg->sd_sector_size;
  uint32_t count = cfg->sd_sector_count;
  if (ssz == 0u || count == 0u) {
#if defined(MFS_STM32_USE_MMC)
    if (ssz == 0u)
      ssz = (uint16_t)h->MmcCard.LogBlockSize;
    if (count == 0u)
      count = (uint32_t)h->MmcCard.LogBlockNbr;
#else
    if (ssz == 0u)
      ssz = (uint16_t)h->SdCard.LogBlockSize;
    if (count == 0u)
      count = (uint32_t)h->SdCard.LogBlockNbr;
#endif
  }
  /* El adaptador gestionado exige sector potencia de dos >= 256. */
  if (count == 0u || ssz < 256u)
    return MFS_EINVAL;
  if (s_dev.bounce_len < ssz)
    return MFS_EOVERFLOW; /* el bounce buffer debe cubrir un sector */
  s_dev.sector_size = ssz;
  s_dev.sector_count = count;

  s_dev.t_read_max_us = cfg->t_read_max_us ? cfg->t_read_max_us : 1000u;
  s_dev.t_write_max_us = cfg->t_prog_max_us ? cfg->t_prog_max_us : 5000u;
  s_dev.t_trim_max_us = cfg->t_erase_max_us ? cfg->t_erase_max_us : 100000u;

  /* El perfil lo fija el tipo de medio: SD o eMMC. Ambos son MANAGED. */
  mfs_media_type_t media = MFS_MEDIA_SD;
#if defined(MFS_STM32_USE_MMC)
  media = MFS_MEDIA_EMMC;
#endif

  mfs_st st = mfs_managed_l2_init(&s_l2, &s_dev, media);
  if (st != MFS_OK)
    return st;

  *out_l2 = &s_l2.drv;
  *out_geom = &s_l2.geom;
  return MFS_OK;
}
