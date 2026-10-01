/* vblk_sim.c — implementación del simulador de dispositivo gestionado. */
#include "vblk_sim.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *vb_at(vblk_t *b, uint32_t lba) {
  return b->img + (size_t)lba * b->sector_size;
}

static mfs_st vb_read(void *ctx, uint32_t lba, uint32_t count, void *dst) {
  vblk_t *b = (vblk_t *)ctx;
  if (b->crashed)
    return MFS_EIO;
  if ((uint64_t)lba + count > b->sectors)
    return MFS_EINVAL;
  b->n_read += count;
  memcpy(dst, vb_at(b, lba), (size_t)count * b->sector_size);
  return MFS_OK;
}

static mfs_st vb_write(void *ctx, uint32_t lba, uint32_t count,
                       const void *src) {
  vblk_t *b = (vblk_t *)ctx;
  if (b->crashed)
    return MFS_EIO;
  if (b->fail_next_write) {
    b->fail_next_write = false;
    return MFS_EIO;
  }
  if ((uint64_t)lba + count > b->sectors)
    return MFS_EINVAL;
  b->n_write += count;
  memcpy(vb_at(b, lba), src, (size_t)count * b->sector_size);
  return MFS_OK;
}

static mfs_st vb_trim(void *ctx, uint32_t lba, uint32_t count) {
  vblk_t *b = (vblk_t *)ctx;
  if (b->crashed)
    return MFS_EIO;
  if ((uint64_t)lba + count > b->sectors)
    return MFS_EINVAL;
  b->n_trim++;
  b->trim_sectors += count;
  /* En un SSD/UFS real el TRIM no garantiza ceros; el FTL decide. Aquí sólo se
   * contabiliza (no se altera el contenido, como haría un dispositivo real). */
  return MFS_OK;
}

static mfs_st vb_flush(void *ctx) {
  vblk_t *b = (vblk_t *)ctx;
  return b->crashed ? MFS_EIO : MFS_OK;
}

bool vblk_init(vblk_t *b, uint32_t sectors, uint32_t sector_size) {
  size_t bytes;
  if (!b || sectors == 0u || sector_size < 256u)
    return false;
  memset(b, 0, sizeof(*b));
  bytes = (size_t)sectors * sector_size;
  b->img = (uint8_t *)malloc(bytes);
  if (!b->img)
    return false;
  memset(b->img, 0xFFu, bytes);
  b->sectors = sectors;
  b->sector_size = (uint16_t)sector_size;
  return true;
}

void vblk_free(vblk_t *b) {
  if (b && b->img) {
    free(b->img);
    b->img = NULL;
  }
}

void vblk_attach(vblk_t *b, mfs_managed_dev_t *dev) {
  memset(dev, 0, sizeof(*dev));
  dev->read_sectors = vb_read;
  dev->write_sectors = vb_write;
  dev->trim_sectors = vb_trim;
  dev->flush = vb_flush;
  dev->ctx = b;
  dev->sector_size = b->sector_size;
  dev->sector_count = b->sectors;
  dev->t_read_max_us = 100u;
  dev->t_write_max_us = 2000u;
  dev->t_trim_max_us = 10000u;
  dev->name = "vblk (simulado, gestionado)";
}
