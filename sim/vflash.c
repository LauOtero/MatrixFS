/* vflash.c — simulador NOR SPI para host (§20.2 driver L2, §27.2 FIH)
 *
 * Reproduce la física mínima que el núcleo asume:
 *   · prog sólo aclara bits (1→0) y verifica por read-back (barrera WOB);
 *   · erase limpia un bloque entero y sólo actúa alineado;
 *   · el corte de energía (vf_crash) deja la imagen tal cual estaba: al
 *     re-alimentar (vf_recover) persiste, como un medio real.
 */
#include "vflash.h"

#include <stdlib.h>
#include <string.h>

/* ==== primitivas del driver (ctx = vflash_t *) ==== */

static bool vf_in_range(const vflash_t *vf, uint32_t addr, uint32_t len) {
  return (uint64_t)addr + (uint64_t)len <= (uint64_t)vf->size;
}

static mfs_st vf_do_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  vflash_t *vf = (vflash_t *)ctx;
  if (!vf || !dst || !vf_in_range(vf, addr, len))
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (vf->crashed)
    return MFS_EIO; /* medio sin alimentación */
  memcpy(dst, vf->img + addr, len);
  vf->n_read++;
  return MFS_OK;
}

static mfs_st vf_do_prog(void *ctx, uint32_t addr, const void *src,
                         uint32_t len) {
  vflash_t *vf = (vflash_t *)ctx;
  if (!vf || !src || !vf_in_range(vf, addr, len))
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (vf->crashed)
    return MFS_EIO;
  if (vf->fail_next_prog) { /* inyección de fallo de un solo uso */
    vf->fail_next_prog = false;
    return MFS_EIO;
  }
  const uint8_t *s = (const uint8_t *)src;
  /* Regla NOR: programar sólo puede poner bits a 0. Si se pretende subir un
   * 0→1 hay que borrar antes: es una violación de protocolo (no un fallo del
   * medio), y el simulador la contabiliza para el test de disciplina. */
  for (uint32_t i = 0; i < len; i++) {
    if ((uint8_t)(vf->img[addr + i] & s[i]) != s[i]) {
      vf->n_violations++;
      if (vf->n_violations == 1u)
        vf->viol_addr = addr + i;
      return MFS_EIO;
    }
  }
  for (uint32_t i = 0; i < len; i++)
    vf->img[addr + i] &= s[i];
  /* Barrera WOB (§20.3): el program no se da por bueno sin read-back. */
  if (memcmp(vf->img + addr, s, len) != 0)
    return MFS_ECORRUPT;
  vf->n_prog++;
  return MFS_OK;
}

static mfs_st vf_do_erase(void *ctx, uint32_t addr) {
  vflash_t *vf = (vflash_t *)ctx;
  if (!vf)
    return MFS_EINVAL;
  if (vf->crashed)
    return MFS_EIO;
  uint32_t eu = vf->erase_unit ? vf->erase_unit : VF_SECTOR;
  if ((addr % eu) != 0u || !vf_in_range(vf, addr, eu))
    return MFS_EINVAL;
  memset(vf->img + addr, 0xFFu, eu);
  uint32_t blk = addr / eu;
  if (blk < VF_MAX_BLOCKS) {
    if (vf->pe[blk] != 0xFFFFu)
      vf->pe[blk]++;
    if (vf->pe[blk] > vf->pe_max)
      vf->pe_max = vf->pe[blk];
  }
  vf->n_erase++;
  return MFS_OK;
}

static mfs_st vf_do_suspend(void *ctx) {
  (void)ctx;
  return MFS_OK;
}

static mfs_st vf_do_resume(void *ctx) {
  (void)ctx;
  return MFS_OK;
}

/* ==== API pública ==== */

bool vf_init(vflash_t *vf, uint32_t size, uint32_t erase_unit) {
  if (!vf || size == 0u)
    return false;
  memset(vf, 0, sizeof(*vf));
  vf->erase_unit = erase_unit ? erase_unit : VF_SECTOR;
  if ((vf->erase_unit & (vf->erase_unit - 1u)) != 0u || size < vf->erase_unit)
    return false;
  vf->size = size;
  vf->img = (uint8_t *)malloc(size);
  if (!vf->img)
    return false;
  memset(vf->img, 0xFFu, size); /* medio virgen */
  return true;
}

void vf_free(vflash_t *vf) {
  if (!vf)
    return;
  free(vf->img);
  vf->img = NULL;
  vf->size = 0u;
}

void vf_geom(const vflash_t *vf, mfs_media_geom *g) {
  if (!g)
    return;
  memset(g, 0, sizeof(*g));
  g->type = MFS_MEDIA_NOR_SPI;
  g->base_addr = 0u;
  g->size = vf ? vf->size : 0u;
  g->erase_unit = (vf && vf->erase_unit) ? vf->erase_unit : VF_SECTOR;
  g->program_granularity = 1u; /* NOR: programable byte a byte */
  g->page_size = 256u;
  g->oob_bytes = 0u;
  /* Presupuestos de datasheet NOR típicos (§25). */
  g->t_prog_max_us = 700u;
  g->t_erase_max_us = 45000u;
  g->t_read_max_us = 100u;
  g->t_suspend_max_us = 20u;
  g->flags0 = MFS_HWV0_SUSPEND_E | MFS_HWV0_SUSPEND_P;
  /* NOR es programable byte a byte ⇒ partial-page-program verificado (§11.4) */
  g->flags1 = MFS_HWV1_PPP;
  g->zones_per_block = 1u;
  g->zone_size = 0u; /* derivar: un bloque */
}

const mfs_l2_driver *vf_driver(vflash_t *vf) {
  if (!vf)
    return NULL;
  vf->drv.read = vf_do_read;
  vf->drv.prog = vf_do_prog;
  vf->drv.erase = vf_do_erase;
  vf->drv.suspend = vf_do_suspend;
  vf->drv.resume = vf_do_resume;
  vf->drv.t0_read = NULL;
  vf->drv.t0_prog = NULL;
  vf->drv.rail_ok = NULL;
  vf->drv.dma_read = NULL;
  vf->drv.dma_crc = NULL;
  vf->drv.ctx = vf;
  return &vf->drv;
}

void vf_crash(vflash_t *vf) {
  if (vf)
    vf->crashed = true;
}

void vf_recover(vflash_t *vf) {
  if (vf)
    vf->crashed = false;
}

void vf_fail_next_prog(vflash_t *vf, bool on) {
  if (vf)
    vf->fail_next_prog = on;
}

uint8_t *vf_raw(vflash_t *vf, uint32_t addr) {
  if (!vf || !vf->img || addr >= vf->size)
    return NULL;
  return &vf->img[addr];
}
