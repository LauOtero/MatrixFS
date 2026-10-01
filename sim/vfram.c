/* vfram.c — simulador del tier T0 byte-addressable (§11.11 HMT, §27)
 *
 * A diferencia de vFlash (NOR), aquí programar SOBRESCRIBE el byte: no hay
 * regla 1→0 ni operación de borrado. Es el modelo de FRAM/MRAM/EEPROM.
 */
#include "vfram.h"

#include <stdlib.h>
#include <string.h>

/* ==== primitivas del driver (ctx = vfram_t *) ==== */

static bool vfr_in_range(const vfram_t *fr, uint32_t addr, uint32_t len) {
  return (uint64_t)addr + (uint64_t)len <= (uint64_t)fr->size;
}

static mfs_st vfr_do_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  vfram_t *fr = (vfram_t *)ctx;
  if (!fr || !dst || !vfr_in_range(fr, addr, len))
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (fr->crashed)
    return MFS_EIO;
  memcpy(dst, fr->img + addr, len);
  fr->n_read++;
  return MFS_OK;
}

static mfs_st vfr_do_prog(void *ctx, uint32_t addr, const void *src,
                          uint32_t len) {
  vfram_t *fr = (vfram_t *)ctx;
  if (!fr || !src || !vfr_in_range(fr, addr, len))
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (fr->crashed)
    return MFS_EIO;
  memcpy(fr->img + addr, src, len); /* reescritura directa, sin borrado */
  fr->n_prog++;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t blk = (addr + i) / 256u;
    if (blk < 256u)
      fr->n_writes[blk]++;
  }
  return MFS_OK;
}

static mfs_st vfr_do_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  (void)addr;
  /* Un medio byte-addressable no tiene borrado por bloque: el núcleo no debe
   * invocarlo nunca para T0. */
  return MFS_EINVAL;
}

/* ==== API pública ==== */

bool vfram_init(vfram_t *fr, uint32_t size) {
  if (!fr || size == 0u)
    return false;
  memset(fr, 0, sizeof(*fr));
  fr->size = size;
  fr->img = (uint8_t *)malloc(size);
  if (!fr->img)
    return false;
  memset(fr->img, 0xFFu, size);
  return true;
}

void vfram_free(vfram_t *fr) {
  if (!fr)
    return;
  free(fr->img);
  fr->img = NULL;
  fr->size = 0u;
}

void vfram_geom(const vfram_t *fr, mfs_media_geom *g) {
  if (!g)
    return;
  memset(g, 0, sizeof(*g));
  g->type = MFS_MEDIA_FRAM;
  g->base_addr = 0u;
  g->size = fr ? fr->size : 0u;
  g->erase_unit = 0u; /* sin borrado */
  g->program_granularity = 1u;
  g->page_size = 1u; /* byte-addressable */
  g->oob_bytes = 0u;
  g->t_prog_max_us = 1u; /* ~1 µs (el campo está en µs; FRAM real ~150 ns) */
  g->t_erase_max_us = 0u;
  g->t_read_max_us = 1u;
  g->t_suspend_max_us = 0u;
  g->flags0 = 0u;
  g->flags1 = MFS_HWV1_BYTE_ADDR | MFS_HWV1_ASYM;
  g->zones_per_block = 0u;
  g->zone_size = 0u;
}

const mfs_l2_driver *vfram_driver(vfram_t *fr) {
  if (!fr)
    return NULL;
  fr->drv.read = vfr_do_read;
  fr->drv.prog = vfr_do_prog;
  fr->drv.erase = vfr_do_erase;
  fr->drv.suspend = NULL;
  fr->drv.resume = NULL;
  fr->drv.t0_read = NULL;
  fr->drv.t0_prog = NULL;
  fr->drv.rail_ok = NULL;
  fr->drv.dma_read = NULL;
  fr->drv.dma_crc = NULL;
  fr->drv.ctx = fr;
  return &fr->drv;
}

void vfram_crash(vfram_t *fr) {
  if (fr)
    fr->crashed = true;
}

void vfram_recover(vfram_t *fr) {
  if (fr)
    fr->crashed = false;
}

uint8_t *vfram_raw(vfram_t *fr, uint32_t addr) {
  if (!fr || !fr->img || addr >= fr->size)
    return NULL;
  return &fr->img[addr];
}
