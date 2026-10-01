/* mfs_embedded.c — capa de integración embebida (32-bit MCU).
 *
 * Adapta los callbacks del SDK (región de flash plana) al contrato de driver L2
 * de MatrixFS (§20.2/§20.3) y construye la mfs_config de montaje. Sin heap: el
 * estado vive en el mfs_embedded_flash_t que aporta el integrador.
 */
#include "mfs_embedded.h"

#include <string.h>

#include "mfs_internal.h" /* struct mfs_fs (fs->cfg para mf_format) */

/* ==== Adaptación de callbacks (direcciones absolutas del MCU) ==== */
static mfs_st emb_read(void *c, uint32_t addr, void *dst, uint32_t len) {
  mfs_embedded_flash_t *f = (mfs_embedded_flash_t *)c;
  if (len == 0u)
    return MFS_OK;
  if ((uint64_t)addr + len > f->size)
    return MFS_EINVAL;
  return f->read(f->ctx, f->base_addr + addr, dst, len);
}

static mfs_st emb_prog(void *c, uint32_t addr, const void *src, uint32_t len) {
  mfs_embedded_flash_t *f = (mfs_embedded_flash_t *)c;
  if (len == 0u)
    return MFS_OK;
  if ((uint64_t)addr + len > f->size)
    return MFS_EINVAL;
  return f->prog(f->ctx, f->base_addr + addr, src, len);
}

static mfs_st emb_erase(void *c, uint32_t addr) {
  mfs_embedded_flash_t *f = (mfs_embedded_flash_t *)c;
  if (f->no_erase)
    return MFS_OK; /* FRAM/MRAM/EEPROM: no hay borrado por bloque */
  if (addr >= f->size)
    return MFS_EINVAL;
  return f->erase(f->ctx, f->base_addr + addr);
}

/* ==== Opciones por defecto ==== */
void mfs_embedded_opts_default(mfs_embedded_opts *o) {
  if (!o)
    return;
  memset(o, 0, sizeof(*o));
  o->ram_total = 32768u;      /* 32 KB: cubre Balanced sin sobre-declarar */
  o->arch_class = 2u;         /* ESP32/ESP8266/RP2040/STM32 son 32-bit    */
  o->forced_mode = MFS_MODE_UNSUPPORTED;
  o->suite_preferred = 0xFFu; /* negociar                                 */
  o->file_perm = MFS_DEFAULT_FILE_MODE;
  o->dir_perm = MFS_DEFAULT_DIR_MODE;
  o->format_if_needed = false;
}

/* ==== setup: drv + geom + cfg ==== */
mfs_st mfs_embedded_setup(mfs_embedded_flash_t *flash,
                          const mfs_embedded_opts *o) {
  if (!flash || !flash->read || !flash->prog)
    return MFS_EINVAL;
  if (flash->size == 0u || flash->erase_unit == 0u)
    return MFS_EINVAL;
  if (!flash->no_erase && !flash->erase)
    return MFS_EINVAL;

  mfs_embedded_opts def;
  if (!o) {
    mfs_embedded_opts_default(&def);
    o = &def;
  }

  /* Geometría normalizada */
  if (flash->page_size == 0u)
    flash->page_size = 256u;
  if (flash->t_prog_max_us == 0u)
    flash->t_prog_max_us = 700u;
  if (flash->t_erase_max_us == 0u)
    flash->t_erase_max_us = 45000u;
  if (flash->t_read_max_us == 0u)
    flash->t_read_max_us = 100u;

  memset(&flash->drv, 0, sizeof(flash->drv));
  flash->drv.read = emb_read;
  flash->drv.prog = emb_prog;
  flash->drv.erase = flash->no_erase ? NULL : emb_erase;
  flash->drv.ctx = flash;

  memset(&flash->geom, 0, sizeof(flash->geom));
  flash->geom.type = MFS_MEDIA_NOR_SPI;
  flash->geom.base_addr = 0u;
  flash->geom.size = flash->size;
  flash->geom.erase_unit = flash->erase_unit;
  flash->geom.program_granularity = 1u; /* NOR programable byte a byte */
  flash->geom.page_size = flash->page_size;
  flash->geom.oob_bytes = 0u;
  flash->geom.t_prog_max_us = flash->t_prog_max_us;
  flash->geom.t_erase_max_us = flash->t_erase_max_us;
  flash->geom.t_read_max_us = flash->t_read_max_us;
  flash->geom.flags1 = flash->no_erase ? MFS_HWV1_BYTE_ADDR : 0u;
  flash->geom.zones_per_block = 1u;
  flash->geom.zone_size = flash->erase_unit;

  memset(&flash->cfg, 0, sizeof(flash->cfg));
  flash->cfg.drv = &flash->drv;
  flash->cfg.geom = &flash->geom;
  flash->cfg.arch_class = o->arch_class ? o->arch_class : 2u;
  flash->cfg.forced_mode = o->forced_mode;
  flash->cfg.ram_total = o->ram_total;
  flash->cfg.key = o->key;
  flash->cfg.suite_preferred = o->suite_preferred;
  flash->cfg.bus_speed_hz = o->bus_speed_hz;
  flash->cfg.allow_convergent = o->allow_convergent;
  flash->cfg.dedup_enable = o->dedup_enable;
  flash->cfg.cdc_enable = o->cdc_enable;
  flash->cfg.zrp_enable = o->zrp_enable;
  flash->cfg.dab_enable = o->dab_enable;
  flash->cfg.default_uid = o->uid;
  flash->cfg.default_gid = o->gid;
  flash->cfg.default_file_perm =
      o->file_perm ? o->file_perm : MFS_DEFAULT_FILE_MODE;
  flash->cfg.default_dir_perm =
      o->dir_perm ? o->dir_perm : MFS_DEFAULT_DIR_MODE;

  flash->ready = true;
  return MFS_OK;
}

/* ==== format ==== */
mfs_st mfs_embedded_format(mf_t *fs, mfs_embedded_flash_t *flash,
                           const mfs_embedded_opts *o) {
  if (!fs || !flash)
    return MFS_EINVAL;
  mfs_st st = mfs_embedded_setup(flash, o);
  if (st != MFS_OK)
    return st;
  /* mf_format limpia la instancia y exige fs->cfg ya fijado. */
  fs->cfg = &flash->cfg;
  return (mfs_st)mf_format(fs, NULL);
}

/* ==== mount (con formateo opcional) ==== */
mfs_st mfs_embedded_mount(mf_t *fs, mfs_embedded_flash_t *flash,
                          const mfs_embedded_opts *o) {
  if (!fs || !flash)
    return MFS_EINVAL;
  mfs_embedded_opts def;
  if (!o) {
    mfs_embedded_opts_default(&def);
    o = &def;
  }
  mfs_st st = mfs_embedded_setup(flash, o);
  if (st != MFS_OK)
    return st;

  fs->cfg = &flash->cfg;
  st = (mfs_st)mf_init(fs, &flash->cfg);
  if (st == MFS_OK)
    return MFS_OK;

  /* Volumen ausente o corrupto ⇒ formatear si se pidió (§24.2). */
  if (o->format_if_needed &&
      (st == MFS_ECORRUPT || st == MFS_EIO || st == MFS_EBADMSG)) {
    st = mfs_embedded_format(fs, flash, o);
    if (st != MFS_OK)
      return st;
    return (mfs_st)mf_init(fs, &flash->cfg);
  }
  return st;
}
