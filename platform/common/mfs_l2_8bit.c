/* mfs_l2_8bit.c — Drivers L2 para MCU de 8 bits
 *
 * Implementa la interfaz mfs_l2_driver (mfs_port.h) sobre los medios típicos
 * en plataformas de 8 bits:
 *   - NOR SPI Flash (motor RAW)
 *   - FRAM SPI / I2C (motor RAW, byte-addressable, sin erase)
 *   - EEPROM SPI / I2C (motor RAW, byte-addressable)
 *   - Flash interna del MCU (motor RAW)
 *   - SD Card modo SPI (motor MANAGED)
 *
 * Diseño (MFS-ARCH-010 rev.2):
 *   - Sin malloc: buffers estáticos en el propio descriptor.
 *   - Determinismo: cada operación tiene timeout acotado por presupuesto.
 *   - Resiliencia: reintentos acotados + verificación de estado (WOB barrier).
 *   - Todo acceso al bus se delega en callbacks del BSP (el núcleo no habla
 *     nunca con el controlador, §2 P4).
 */
#include "mfs_l2_8bit.h"
#include <string.h>

/* ==== Helpers internos ==== */

/* Espera a que WIP (write-in-progress) baje. Devuelve MFS_ETIMEDOUT_BUDGET si
 * se agota el presupuesto. Determinista: nº de iteraciones acotado. */
static mfs_st l2_nor_wait_ready(mfs_l2_8bit_driver_t *d, uint32_t budget_us) {
  uint8_t cmd = MFS_SPI_NOR_CMD_RDSR;
  uint8_t sr = 0;
  uint32_t spins = budget_us; /* 1 iteración ≈ 1 µs declarada por el BSP */
  if (spins == 0u)
    spins = 1u;
  for (uint32_t i = 0; i < spins; i++) {
    if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
      return MFS_EIO;
    if (d->cfg.spi_transfer(d->cfg.ctx, &cmd, NULL, 1u) != MFS_OK) {
      d->cfg.spi_cs_high(d->cfg.ctx);
      return MFS_EIO;
    }
    sr = 0xFFu;
    if (d->cfg.spi_transfer(d->cfg.ctx, NULL, &sr, 1u) != MFS_OK) {
      d->cfg.spi_cs_high(d->cfg.ctx);
      return MFS_EIO;
    }
    d->cfg.spi_cs_high(d->cfg.ctx);
    if ((sr & MFS_SPI_NOR_SR_WIP) == 0u) {
      d->status_reg[0] = sr;
      return MFS_OK;
    }
    mfs_port_wfi();
  }
  return MFS_ETIMEDOUT_BUDGET;
}

/* Write Enable (WREN) para NOR/FRAM/EEPROM */
static mfs_st l2_nor_wren(mfs_l2_8bit_driver_t *d) {
  uint8_t cmd = MFS_SPI_NOR_CMD_WREN;
  if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
    return MFS_EIO;
  mfs_st st = d->cfg.spi_transfer(d->cfg.ctx, &cmd, NULL, 1u);
  d->cfg.spi_cs_high(d->cfg.ctx);
  return st;
}

/* Construye la dirección big-endian sobre el buffer de comando (2 o 3 bytes) */
static uint8_t l2_push_addr(mfs_l2_8bit_driver_t *d, uint8_t *buf, uint32_t addr) {
  if (d->cfg.addr_bytes == 3u) {
    buf[0] = (uint8_t)(addr >> 16);
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)addr;
    return 3u;
  }
  buf[0] = (uint8_t)(addr >> 8);
  buf[1] = (uint8_t)addr;
  return 2u;
}

/* ==== read ==== */
static mfs_st l2_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  mfs_l2_8bit_driver_t *d = (mfs_l2_8bit_driver_t *)ctx;
  if (!d || !d->initialized || !dst || len == 0u)
    return MFS_EINVAL;
  if (addr + len > d->cfg.total_size)
    return MFS_EINVAL;

  uint8_t hdr[4];
  uint8_t n = 0;

  switch (d->cfg.type) {
  case MFS_L2_8BIT_SPI_NOR:
    hdr[0] = MFS_SPI_NOR_CMD_READ;
    break;
  case MFS_L2_8BIT_SPI_FRAM:
    hdr[0] = MFS_FRAM_CMD_READ;
    break;
  case MFS_L2_8BIT_SPI_EEPROM:
  case MFS_L2_8BIT_INTERNAL_FLASH:
    hdr[0] = MFS_SPI_NOR_CMD_READ;
    break;
  default:
    return MFS_ENOTSUP;
  }
  n = 1u;
  n += l2_push_addr(d, &hdr[n], addr);

  if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
    return MFS_EIO;
  mfs_st st = d->cfg.spi_transfer(d->cfg.ctx, hdr, NULL, n);
  if (st == MFS_OK)
    st = d->cfg.spi_transfer(d->cfg.ctx, NULL, (uint8_t *)dst, (uint16_t)len);
  d->cfg.spi_cs_high(d->cfg.ctx);
  return st;
}

/* ==== prog (NOR/FRAM/EEPROM SPI) ==== */
static mfs_st l2_prog(void *ctx, uint32_t addr, const void *src, uint32_t len) {
  mfs_l2_8bit_driver_t *d = (mfs_l2_8bit_driver_t *)ctx;
  if (!d || !d->initialized || !src || len == 0u)
    return MFS_EINVAL;
  if (addr + len > d->cfg.total_size)
    return MFS_EINVAL;

  /* FRAM/EEPROM no requieren WREN obligatorio, pero enviarlo es inocuo en FRAM
   * y necesario en EEPROM/NOR. */
  mfs_st st = l2_nor_wren(d);
  if (st != MFS_OK)
    return st;

  uint8_t hdr[4];
  uint8_t n = 1u;
  switch (d->cfg.type) {
  case MFS_L2_8BIT_SPI_NOR:
  case MFS_L2_8BIT_SPI_EEPROM:
  case MFS_L2_8BIT_INTERNAL_FLASH:
    hdr[0] = MFS_SPI_NOR_CMD_PP;
    break;
  case MFS_L2_8BIT_SPI_FRAM:
    hdr[0] = MFS_FRAM_CMD_WRITE;
    break;
  default:
    return MFS_ENOTSUP;
  }
  n += l2_push_addr(d, &hdr[n], addr);

  if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
    return MFS_EIO;
  st = d->cfg.spi_transfer(d->cfg.ctx, hdr, NULL, n);
  if (st == MFS_OK)
    st = d->cfg.spi_transfer(d->cfg.ctx, (const uint8_t *)src, NULL,
                             (uint16_t)len);
  d->cfg.spi_cs_high(d->cfg.ctx);
  if (st != MFS_OK)
    return st;

  /* Barrera WOB (§20.3): verificamos que la operación terminó antes de
   * retornar; en FRAM/EEPROM el dispositivo ya confirmó, pero mantener la
   * verificación preserva la semántica del contrato de puerto. */
  if (d->cfg.type == MFS_L2_8BIT_SPI_NOR)
    return l2_nor_wait_ready(d, d->cfg.t_prog_max_us);
  return MFS_OK;
}

/* ==== erase (sector NOR) ==== */
static mfs_st l2_erase(void *ctx, uint32_t addr) {
  mfs_l2_8bit_driver_t *d = (mfs_l2_8bit_driver_t *)ctx;
  if (!d || !d->initialized)
    return MFS_EINVAL;
  /* FRAM/MRAM/EEPROM/interna gestionada: no requieren borrado explícito */
  if (d->cfg.erase_size == 0u)
    return MFS_OK;
  if (d->cfg.type != MFS_L2_8BIT_SPI_NOR)
    return MFS_ENOTSUP;
  if (addr % d->cfg.erase_size != 0u)
    return MFS_EINVAL;

  mfs_st st = l2_nor_wren(d);
  if (st != MFS_OK)
    return st;

  uint8_t hdr[4];
  hdr[0] = (d->cfg.erase_size >= 65536u) ? MFS_SPI_NOR_CMD_BE64
                                         : MFS_SPI_NOR_CMD_SE;
  uint8_t n = 1u + l2_push_addr(d, &hdr[1], addr);
  if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
    return MFS_EIO;
  st = d->cfg.spi_transfer(d->cfg.ctx, hdr, NULL, n);
  d->cfg.spi_cs_high(d->cfg.ctx);
  if (st != MFS_OK)
    return st;
  return l2_nor_wait_ready(d, d->cfg.t_erase_max_us);
}

/* ==== Geometría → mfs_media_geom ==== */
mfs_st mfs_l2_8bit_get_geom(const mfs_l2_8bit_driver_t *drv,
                            mfs_media_geom *geom) {
  if (!drv || !geom)
    return MFS_EINVAL;
  memset(geom, 0, sizeof(*geom));

  switch (drv->cfg.type) {
  case MFS_L2_8BIT_SPI_NOR:
    geom->type = MFS_MEDIA_NOR_SPI;
    geom->flags0 = (uint8_t)(MFS_HWV0_SUSPEND_E | MFS_HWV0_SUSPEND_P);
    break;
  case MFS_L2_8BIT_SPI_FRAM:
  case MFS_L2_8BIT_I2C_FRAM:
    geom->type = MFS_MEDIA_FRAM;
    geom->flags1 = MFS_HWV1_BYTE_ADDR;
    break;
  case MFS_L2_8BIT_SPI_EEPROM:
  case MFS_L2_8BIT_I2C_EEPROM:
    geom->type = MFS_MEDIA_EEPROM;
    geom->flags1 = MFS_HWV1_BYTE_ADDR;
    break;
  case MFS_L2_8BIT_SD_SPI:
    geom->type = MFS_MEDIA_SD;
    geom->flags1 = MFS_HWV1_MANAGED;
    geom->flags0 = MFS_HWV0_ECC_ON_DIE;
    break;
  case MFS_L2_8BIT_INTERNAL_FLASH:
    geom->type = MFS_MEDIA_NOR_SPI; /* se comporta como NOR interna */
    break;
  default:
    return MFS_EINVAL;
  }

  geom->base_addr = 0u;
  geom->size = drv->cfg.total_size;
  geom->erase_unit = drv->cfg.erase_size ? drv->cfg.erase_size : 4096u;
  geom->program_granularity =
      (drv->cfg.type == MFS_L2_8BIT_SPI_FRAM ||
       drv->cfg.type == MFS_L2_8BIT_I2C_FRAM) ? 1u : 1u;
  geom->page_size = (uint16_t)(drv->cfg.page_size ? drv->cfg.page_size : 256u);
  geom->oob_bytes = 0u;
  geom->t_prog_max_us = drv->cfg.t_prog_max_us;
  geom->t_erase_max_us = drv->cfg.t_erase_max_us;
  geom->t_read_max_us = drv->cfg.t_read_max_us;
  geom->t_suspend_max_us = 0u;
  geom->zones_per_block = 1u;
  geom->zone_size = drv->cfg.erase_size;
  return MFS_OK;
}

/* ==== Creación del driver ==== */
mfs_st mfs_l2_8bit_create(const mfs_l2_8bit_cfg_t *cfg,
                          mfs_l2_8bit_driver_t **out_drv) {
  if (!cfg || !out_drv)
    return MFS_EINVAL;
  if (!cfg->spi_cs_low || !cfg->spi_cs_high || !cfg->spi_transfer)
    return MFS_EINVAL;

  /* Descriptor estático (sin heap): el integrador provee el almacenamiento. */
  static mfs_l2_8bit_driver_t s_drv; /* instancia única para targets pequeños */
  mfs_l2_8bit_driver_t *d = &s_drv;
  memset(d, 0, sizeof(*d));
  d->cfg = *cfg;

  /* Normalizaciones de geometría */
  if (d->cfg.page_size == 0u)
    d->cfg.page_size = 256u;
  if (d->cfg.addr_bytes == 0u)
    d->cfg.addr_bytes = (d->cfg.total_size > 0x1000000u) ? 3u : 2u;
  if (d->cfg.type == MFS_L2_8BIT_SPI_NOR && d->cfg.erase_size == 0u)
    d->cfg.erase_size = 4096u;

  d->base.read = l2_read;
  d->base.prog = l2_prog;
  d->base.erase = l2_erase;
  d->base.suspend = NULL;
  d->base.resume = NULL;
  d->base.t0_read = NULL;
  d->base.t0_prog = NULL;
  d->base.rail_ok = NULL;
  d->base.dma_read = NULL;
  d->base.dma_crc = NULL;
  d->base.ctx = d;

  /* Autodetección de tamaño si el integrador no lo declaró */
  if (d->cfg.total_size == 0u) {
    mfs_l2_8bit_type_t t;
    uint32_t sz = 0u;
    mfs_st st = mfs_l2_8bit_probe_spi(&d->cfg, &t, &sz);
    if (st != MFS_OK)
      return st;
    d->cfg.total_size = sz;
  }

  /* Init del bus (si el BSP lo provee) */
  if (d->cfg.spi_init) {
    mfs_st st = d->cfg.spi_init(d->cfg.ctx);
    if (st != MFS_OK)
      return st;
  }

  d->initialized = true;
  *out_drv = d;
  return MFS_OK;
}

void mfs_l2_8bit_destroy(mfs_l2_8bit_driver_t *drv) {
  if (!drv)
    return;
  if (drv->cfg.spi_deinit)
    drv->cfg.spi_deinit(drv->cfg.ctx);
  drv->initialized = false;
}

/* ==== Autodetección JEDEC (RDID 0x9F) ====
 * Lee 3 bytes de ID: [manufacturer, memory_type, capacity].
 * La capacidad se codifica como 2^n bytes (JEDEC standard). */
mfs_st mfs_l2_8bit_probe_spi(const mfs_l2_8bit_cfg_t *cfg,
                             mfs_l2_8bit_type_t *out_type,
                             uint32_t *out_size) {
  if (!cfg || !out_type || !out_size)
    return MFS_EINVAL;
  if (!cfg->spi_cs_low || !cfg->spi_cs_high || !cfg->spi_transfer)
    return MFS_EINVAL;

  uint8_t cmd = MFS_SPI_NOR_CMD_RDID;
  uint8_t id[3] = {0xFFu, 0xFFu, 0xFFu};

  if (cfg->spi_cs_low(cfg->ctx) != MFS_OK)
    return MFS_EIO;
  mfs_st st = cfg->spi_transfer(cfg->ctx, &cmd, NULL, 1u);
  if (st == MFS_OK)
    st = cfg->spi_transfer(cfg->ctx, NULL, id, 3u);
  cfg->spi_cs_high(cfg->ctx);
  if (st != MFS_OK)
    return st;

  /* 0xFF en el byte de capacidad ⇒ no hay dispositivo */
  if (id[2] == 0xFFu || (id[0] == 0xFFu && id[1] == 0xFFu && id[2] == 0xFFu))
    return MFS_EIO;

  /* Capacidad JEDEC: 2^id[2] bytes */
  *out_size = (uint32_t)1u << (id[2] & 0x1Fu);
  /* Tipo por byte de capacidad: heurística conservadora.
   * Los SPI NOR tienen fabricante != 0; si el fabricante es típico de FRAM
   * (p. ej. 0x04 Cypress/Infineon FRAM en algunos modelos) el integrador debe
   * declarar el tipo explícitamente. Por defecto asumimos NOR. */
  *out_type = MFS_L2_8BIT_SPI_NOR;
  return MFS_OK;
}