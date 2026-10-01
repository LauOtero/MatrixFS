/* mfs_l2_managed.c — adaptador L2 para medios gestionados (SD/eMMC/UFS/SATA/
 * NVMe/USB). Traduce la API de sectores del BSP al contrato `mfs_l2_driver`
 * (§20.2/§20.3) con RMW alineado y TRIM opcional. Sin heap.
 */
#include "mfs_l2_managed.h"

#include <string.h>

/* Dirección máxima direccionable por el núcleo (32 bits, ver
 * DOCS/known-limitations.md). Un dispositivo mayor se usa hasta este límite. */
#define MG_ADDR_MAX 0xFFFFFFFFu

static uint32_t mg_bytes(const mfs_managed_dev_t *d) {
  uint64_t b = (uint64_t)d->sector_size * (uint64_t)d->sector_count;
  return (b > (uint64_t)MG_ADDR_MAX) ? MG_ADDR_MAX : (uint32_t)b;
}

static mfs_st mg_read(void *c, uint32_t addr, void *dst, uint32_t len) {
  mfs_managed_l2_t *l2 = (mfs_managed_l2_t *)c;
  mfs_managed_dev_t *d = l2->dev;
  uint32_t ss = d->sector_size;
  if (len == 0u)
    return MFS_OK;
  if ((uint64_t)addr + len > (uint64_t)mg_bytes(d))
    return MFS_EINVAL;
  if ((addr % ss) == 0u && (len % ss) == 0u)
    return d->read_sectors(d->ctx, addr / ss, len / ss, dst);
  /* RMW de lectura: trae los sectores que cubren [addr, addr+len) al bounce */
  {
    uint32_t first = addr / ss;
    uint32_t last = (addr + len - 1u) / ss;
    uint32_t nsec = last - first + 1u;
    if (nsec * ss > d->bounce_len)
      return MFS_EINVAL;
    mfs_st st = d->read_sectors(d->ctx, first, nsec, d->bounce);
    if (st != MFS_OK)
      return st;
    memcpy(dst, d->bounce + (addr - first * ss), len);
  }
  return MFS_OK;
}

static mfs_st mg_prog(void *c, uint32_t addr, const void *src, uint32_t len) {
  mfs_managed_l2_t *l2 = (mfs_managed_l2_t *)c;
  mfs_managed_dev_t *d = l2->dev;
  uint32_t ss = d->sector_size;
  const uint8_t *p = (const uint8_t *)src;
  uint32_t off = addr;
  uint32_t left = len;
  if (len == 0u)
    return MFS_OK;
  if ((uint64_t)addr + len > (uint64_t)mg_bytes(d))
    return MFS_EINVAL;

  while (left > 0u) {
    uint32_t s = off / ss;
    uint32_t in = off % ss;
    uint32_t take = ss - in;
    if (take > left)
      take = left;
    if (in == 0u && take == ss) {
      mfs_st st = d->write_sectors(d->ctx, s, 1u, p);
      if (st != MFS_OK)
        return st;
    } else {
      /* RMW: el medio sólo admite escritura de sector completo */
      if (ss > d->bounce_len)
        return MFS_EINVAL;
      mfs_st st = d->read_sectors(d->ctx, s, 1u, d->bounce);
      if (st != MFS_OK)
        return st;
      memcpy(d->bounce + in, p, take);
      st = d->write_sectors(d->ctx, s, 1u, d->bounce);
      if (st != MFS_OK)
        return st;
    }
    off += take;
    p += take;
    left -= take;
  }
  if (d->flush)
    return d->flush(d->ctx);
  return MFS_OK;
}

static mfs_st mg_erase(void *c, uint32_t addr) {
  mfs_managed_l2_t *l2 = (mfs_managed_l2_t *)c;
  mfs_managed_dev_t *d = l2->dev;
  uint32_t ss = d->sector_size;
  uint32_t eu = l2->geom.erase_unit;
  /* En un medio gestionado no hay borrado previo obligatorio: si el
   * dispositivo expone discard, se emite TRIM como pista de reciclado. */
  if (!d->trim_sectors)
    return MFS_OK;
  if (eu == 0u || (eu % ss) != 0u)
    return MFS_EINVAL;
  if ((addr % ss) != 0u)
    return MFS_EINVAL;
  if ((uint64_t)addr + eu > (uint64_t)mg_bytes(d))
    return MFS_EINVAL;
  return d->trim_sectors(d->ctx, addr / ss, eu / ss);
}

mfs_st mfs_managed_l2_init(mfs_managed_l2_t *l2, mfs_managed_dev_t *dev,
                           mfs_media_type_t media) {
  if (!l2 || !dev)
    return MFS_EINVAL;
  if (!dev->read_sectors || !dev->write_sectors)
    return MFS_EINVAL;
  if (dev->sector_size < 256u ||
      (dev->sector_size & (uint16_t)(dev->sector_size - 1u)) != 0u)
    return MFS_EINVAL; /* debe ser potencia de dos */
  if (dev->sector_count == 0u)
    return MFS_EINVAL;
  if (!dev->bounce || dev->bounce_len < dev->sector_size)
    return MFS_EINVAL; /* el RMW necesita el buffer del integrador */
  if (mf_media_profile(media)->engine != MFS_ENGINE_MANAGED)
    return MFS_EINVAL; /* este adaptador sólo sirve a medios gestionados */

  {
    mfs_st cap = mfs_profile_check(media, mg_bytes(dev));
    if (cap != MFS_OK)
      return cap; /* MFS-CAP-001: no direccionar fuera de la cota certificada */
  }

  memset(&l2->drv, 0, sizeof(l2->drv));
  memset(&l2->geom, 0, sizeof(l2->geom));
  l2->dev = dev;

  l2->drv.read = mg_read;
  l2->drv.prog = mg_prog;
  l2->drv.erase = mg_erase;
  l2->drv.ctx = l2;

  l2->geom.type = media;
  l2->geom.base_addr = 0u;
  l2->geom.size = mg_bytes(dev);
  {
    /* El layout reserva sectores completos para SB/HWV/tokens (≥ 1 KiB). Se
     * usa 4 KiB o el tamaño de sector si es mayor, siempre múltiplo de él. */
    uint32_t ss = dev->sector_size;
    uint32_t eu = (ss >= 4096u) ? ss : 4096u;
    if ((eu % ss) != 0u)
      eu = ((eu / ss) + 1u) * ss;
    l2->geom.erase_unit = eu;
    l2->geom.zone_size = eu;
    l2->geom.page_size = (uint16_t)ss;
  }
  l2->geom.program_granularity = 1u; /* el adaptador hace RMW a sector */
  l2->geom.oob_bytes = 0u;
  l2->geom.t_prog_max_us = dev->t_write_max_us;
  l2->geom.t_erase_max_us = dev->t_trim_max_us;
  l2->geom.t_read_max_us = dev->t_read_max_us;
  l2->geom.flags0 = MFS_HWV0_ECC_ON_DIE; /* el FTL del medio ya protege */
  l2->geom.flags1 = MFS_HWV1_MANAGED;
  l2->geom.zones_per_block = 1u;
  return MFS_OK;
}
