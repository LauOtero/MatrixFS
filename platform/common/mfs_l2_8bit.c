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

/* ¿Es un medio I2C? (FRAM/EEPROM sobre I2C) */
static bool l2_is_i2c(const mfs_l2_8bit_cfg_t *c) {
  return c->type == MFS_L2_8BIT_I2C_FRAM || c->type == MFS_L2_8BIT_I2C_EEPROM;
}

/* Nº de bytes de la word address I2C según la capacidad del dispositivo:
 * 1 B hasta 256 B (24C01/02), 2 B hasta 64 KiB, 3 B por encima. */
static uint16_t l2_i2c_addr_bytes(const mfs_l2_8bit_cfg_t *c) {
  if (c->total_size <= 0x100u)
    return 1u;
  if (c->total_size <= 0x10000u)
    return 2u;
  return 3u;
}

/* Espera a que WIP (write-in-progress) baje. Solo para medios SPI con registro
 * de estado (NOR/EEPROM). Devuelve MFS_ETIMEDOUT_BUDGET si se agota el
 * presupuesto. Determinista: nº de iteraciones acotado. */
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
static uint8_t l2_push_addr(mfs_l2_8bit_driver_t *d, uint8_t *buf,
                            uint32_t addr) {
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
  /* Cota sin desbordamiento de 32 bits (addr+len podía envolver). */
  if ((uint64_t)addr + (uint64_t)len > (uint64_t)d->cfg.total_size)
    return MFS_EINVAL;

  if (d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH) {
    if (!d->cfg.iflash_read)
      return MFS_ENOTSUP;
    return d->cfg.iflash_read(d->cfg.ctx, addr, (uint8_t *)dst, len);
  }

  if (l2_is_i2c(&d->cfg)) {
    if (!d->cfg.i2c_mem_read)
      return MFS_ENOTSUP;
    return d->cfg.i2c_mem_read(d->cfg.ctx, d->cfg.i2c_addr, addr,
                               l2_i2c_addr_bytes(&d->cfg), (uint8_t *)dst, len);
  }

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
  /* La lectura continua SÍ puede cruzar fronteras de página sin problema; es
   * la programación (Page Program) la que no puede (ver l2_prog). */
  if (st == MFS_OK)
    st = d->cfg.spi_transfer(d->cfg.ctx, NULL, (uint8_t *)dst, len);
  d->cfg.spi_cs_high(d->cfg.ctx);
  return st;
}

/* ==== prog ====
 *
 * Un *Page Program* NOR **no puede cruzar** la frontera de una página: si la
 * orden lleva más bytes que los que caben hasta el final de la página, el
 * dispositivo ENVUELVE al principio de la misma y el dato se corrompe **sin
 * devolver error**. El núcleo pide programaciones de hasta `chunk_size`
 * (4096 B, src/core/mfs_zone.c) desde direcciones no necesariamente alineadas
 * a página, así que trocear aquí es obligatorio, no una optimización.
 * La EEPROM tiene la misma restricción (*page write*); la FRAM no.
 *
 * Además, el *write enable latch* se limpia al completar cada programación, de
 * modo que hace falta un WREN **por trozo**, no uno por llamada.
 */
static mfs_st l2_prog(void *ctx, uint32_t addr, const void *src, uint32_t len) {
  mfs_l2_8bit_driver_t *d = (mfs_l2_8bit_driver_t *)ctx;
  if (!d || !d->initialized || !src || len == 0u)
    return MFS_EINVAL;
  if ((uint64_t)addr + (uint64_t)len > (uint64_t)d->cfg.total_size)
    return MFS_EINVAL;
  if (d->cfg.type == MFS_L2_8BIT_SD_SPI)
    return MFS_ENOTSUP; /* SD-SPI no se gestiona por esta ruta */

  const uint8_t *sp = (const uint8_t *)src;

  /* ---- Flash interna del MCU: el backend del BSP hace el RMW y respeta su
   * unidad de programación y ECC (iflash_write). ---- */
  if (d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH) {
    if (!d->cfg.iflash_write)
      return MFS_ENOTSUP;
    return d->cfg.iflash_write(d->cfg.ctx, addr, sp, len);
  }

  /* ---- I2C: FRAM/EEPROM ---- */
  if (l2_is_i2c(&d->cfg)) {
    uint16_t ab = l2_i2c_addr_bytes(&d->cfg);
    const uint8_t *i2c_w = sp;
    for (uint32_t done = 0u; done < len;) {
      uint32_t cur = addr + done;
      uint32_t room = len - done;
      /* EEPROM: no cruzar la página de escritura del dispositivo. */
      if (d->cfg.type == MFS_L2_8BIT_I2C_EEPROM && d->cfg.page_size != 0u) {
        uint32_t left = d->cfg.page_size - (cur % d->cfg.page_size);
        if (room > left)
          room = left;
      }
      if (!d->cfg.i2c_mem_write)
        return MFS_ENOTSUP;
      mfs_st st = d->cfg.i2c_mem_write(d->cfg.ctx, d->cfg.i2c_addr, cur, ab,
                                       i2c_w + done, room);
      if (st != MFS_OK)
        return st;
      done += room;
    }
    return MFS_OK;
  }

  /* ---- SPI: NOR / EEPROM / flash interna / FRAM ---- */
  bool paged = (d->cfg.type == MFS_L2_8BIT_SPI_NOR ||
                d->cfg.type == MFS_L2_8BIT_SPI_EEPROM ||
                d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH);
  uint32_t page = (paged && d->cfg.page_size) ? d->cfg.page_size : len;

  for (uint32_t done = 0u; done < len;) {
    uint32_t cur = addr + done;
    uint32_t room = page - (cur % page);
    uint32_t n = len - done;
    if (n > room)
      n = room;

    /* El latch de escritura se limpia tras cada programación: WREN por trozo.
     */
    mfs_st st = l2_nor_wren(d);
    if (st != MFS_OK)
      return st;

    uint8_t hdr[4];
    uint8_t hn = 1u;
    hdr[0] = (d->cfg.type == MFS_L2_8BIT_SPI_FRAM) ? MFS_FRAM_CMD_WRITE
                                                   : MFS_SPI_NOR_CMD_PP;
    hn = (uint8_t)(hn + l2_push_addr(d, &hdr[hn], cur));

    if (d->cfg.spi_cs_low(d->cfg.ctx) != MFS_OK)
      return MFS_EIO;
    st = d->cfg.spi_transfer(d->cfg.ctx, hdr, NULL, hn);
    if (st == MFS_OK)
      st = d->cfg.spi_transfer(d->cfg.ctx, sp + done, NULL, n);
    d->cfg.spi_cs_high(d->cfg.ctx);
    if (st != MFS_OK)
      return st;

    /* Barrera WOB (§20.3): no retornar hasta que el dispositivo confirmó.
     * Solo NOR/EEPROM SPI exponen WIP; la FRAM confirma con el propio ACK. */
    if (d->cfg.type == MFS_L2_8BIT_SPI_NOR) {
      st = l2_nor_wait_ready(d, d->cfg.t_prog_max_us);
      if (st != MFS_OK)
        return st;
    }
    done += n;
  }
  return MFS_OK;
}

/* ==== erase (sector NOR) ====
 *
 * El comando de borrado se ELIGE por el tamaño declarado. Antes se usaba SE
 * (4 KiB) para cualquier `erase_size < 65536`, de modo que declarar 32 KiB
 * borraba solo 4 KiB mientras el núcleo creía haber borrado 32 KiB: corrupción
 * silenciosa. Un `erase_size` que no corresponda a un comando estándar se
 * rechaza en vez de aproximarse.
 */
static mfs_st l2_erase(void *ctx, uint32_t addr) {
  mfs_l2_8bit_driver_t *d = (mfs_l2_8bit_driver_t *)ctx;
  if (!d || !d->initialized)
    return MFS_EINVAL;
  /* Flash interna: la borra el backend del BSP (una unidad de borrado). Se
   * comprueba ANTES del atajo `erase_size == 0` porque, si el integrador
   * olvidara declararlo, el atajo haría creer al núcleo que se borró. */
  if (d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH) {
    if (d->cfg.erase_size == 0u)
      return MFS_EINVAL;
    if (!d->cfg.iflash_erase_page)
      return MFS_ENOTSUP;
    if (addr % d->cfg.erase_size != 0u)
      return MFS_EINVAL;
    return d->cfg.iflash_erase_page(d->cfg.ctx, addr);
  }
  /* FRAM/MRAM/EEPROM/interna gestionada: no requieren borrado explícito */
  if (d->cfg.erase_size == 0u)
    return MFS_OK;
  if (d->cfg.type != MFS_L2_8BIT_SPI_NOR)
    return MFS_ENOTSUP;
  if (addr % d->cfg.erase_size != 0u)
    return MFS_EINVAL;

  uint8_t cmd;
  switch (d->cfg.erase_size) {
  case 4096u:
    cmd = MFS_SPI_NOR_CMD_SE;
    break;
  case 32768u:
    cmd = MFS_SPI_NOR_CMD_BE32;
    break;
  case 65536u:
    cmd = MFS_SPI_NOR_CMD_BE64;
    break;
  default:
    return MFS_ENOTSUP; /* sin comando estándar para este tamaño */
  }

  mfs_st st = l2_nor_wren(d);
  if (st != MFS_OK)
    return st;

  uint8_t hdr[4];
  hdr[0] = cmd;
  uint8_t n = (uint8_t)(1u + l2_push_addr(d, &hdr[1], addr));
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

  /* Propiedades declaradas por el integrador (no deducibles del tipo). */
  if (drv->cfg.ecc_on_die)
    geom->flags0 |= MFS_HWV0_ECC_ON_DIE;

  /* `erase_unit` dimensiona el layout reservado del núcleo y debe ser >= 1024
   * (src/core/mfs_hal.c). Un medio sin borrado (FRAM/MRAM/EEPROM) sigue
   * necesitando una unidad de ZONA: se usa 4096 salvo que el integrador indique
   * otra cosa, en vez de dejar el valor a 0. */
  uint32_t eu = drv->cfg.erase_size ? drv->cfg.erase_size : 4096u;

  geom->base_addr = 0u;
  geom->size = drv->cfg.total_size;
  geom->erase_unit = eu;
  geom->program_granularity = drv->cfg.pgm_gran ? drv->cfg.pgm_gran : 1u;
  geom->page_size = (uint16_t)(drv->cfg.page_size ? drv->cfg.page_size : 256u);
  geom->oob_bytes = 0u;
  geom->t_prog_max_us = drv->cfg.t_prog_max_us;
  geom->t_erase_max_us = drv->cfg.t_erase_max_us;
  geom->t_read_max_us = drv->cfg.t_read_max_us;
  geom->t_suspend_max_us = 0u;
  geom->zones_per_block = 1u;
  geom->zone_size = eu;
  return MFS_OK;
}

/* ==== Creación del driver ==== */
mfs_st mfs_l2_8bit_create(const mfs_l2_8bit_cfg_t *cfg,
                          mfs_l2_8bit_driver_t **out_drv) {
  if (!cfg || !out_drv)
    return MFS_EINVAL;

  /* Validación POR TIPO. Antes se exigían los callbacks SPI incondicionalmente,
   * de modo que una configuración I2C (FRAM/EEPROM) o de flash interna eran
   * imposibles de crear aunque sus callbacks estuvieran presentes. */
  switch (cfg->type) {
  case MFS_L2_8BIT_SPI_NOR:
  case MFS_L2_8BIT_SPI_FRAM:
  case MFS_L2_8BIT_SPI_EEPROM:
    if (!cfg->spi_cs_low || !cfg->spi_cs_high || !cfg->spi_transfer)
      return MFS_EINVAL;
    break;
  case MFS_L2_8BIT_I2C_FRAM:
  case MFS_L2_8BIT_I2C_EEPROM:
    if (!cfg->i2c_mem_read || !cfg->i2c_mem_write)
      return MFS_EINVAL;
    break;
  case MFS_L2_8BIT_INTERNAL_FLASH:
    if (!cfg->iflash_read || !cfg->iflash_write)
      return MFS_EINVAL;
    break;
  case MFS_L2_8BIT_SD_SPI:
    return MFS_ENOTSUP; /* usar mfs_l2_managed para medios gestionados */
  default:
    return MFS_EINVAL;
  }

  /* Descriptor estático (sin heap): el integrador provee el almacenamiento. */
  static mfs_l2_8bit_driver_t s_drv; /* instancia única para targets pequeños */
  mfs_l2_8bit_driver_t *d = &s_drv;
  memset(d, 0, sizeof(*d));
  d->cfg = *cfg;

  /* Normalizaciones de geometría */
  if (d->cfg.page_size == 0u)
    d->cfg.page_size = 256u;
  if (d->cfg.type == MFS_L2_8BIT_SPI_NOR && d->cfg.erase_size == 0u)
    d->cfg.erase_size = 4096u;
  if (d->cfg.pgm_gran == 0u)
    d->cfg.pgm_gran = 1u; /* NOR/FRAM/EEPROM son byte-programables */
  if (d->cfg.type == MFS_L2_8BIT_I2C_FRAM && d->cfg.page_size == 256u)
    d->cfg.page_size = 1u; /* la FRAM I2C no tiene página de escritura */
  /* La FRAM I2C (MB85RSxx) direcciona por debajo de 64 KiB con 2 bytes; el
   * tamaño real lo declara el integrador en `total_size`. */

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

  /* Autodetección de tamaño (solo SPI: JEDEC RDID) */
  if (d->cfg.total_size == 0u) {
    if (d->cfg.type == MFS_L2_8BIT_I2C_FRAM ||
        d->cfg.type == MFS_L2_8BIT_I2C_EEPROM ||
        d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH)
      return MFS_EINVAL; /* hay que declarar total_size */
    mfs_l2_8bit_type_t t;
    uint32_t sz = 0u;
    mfs_st st = mfs_l2_8bit_probe_spi(&d->cfg, &t, &sz);
    if (st != MFS_OK)
      return st;
    d->cfg.total_size = sz;
  }

  /* Init del bus/dispositivo (si el BSP lo provee) */
  if (d->cfg.spi_init && !l2_is_i2c(&d->cfg) &&
      d->cfg.type != MFS_L2_8BIT_INTERNAL_FLASH) {
    mfs_st st = d->cfg.spi_init(d->cfg.ctx);
    if (st != MFS_OK)
      return st;
  }
  if (d->cfg.i2c_init && l2_is_i2c(&d->cfg)) {
    mfs_st st = d->cfg.i2c_init(d->cfg.ctx);
    if (st != MFS_OK)
      return st;
  }
  if (d->cfg.iflash_init && d->cfg.type == MFS_L2_8BIT_INTERNAL_FLASH) {
    mfs_st st = d->cfg.iflash_init(d->cfg.ctx);
    if (st != MFS_OK)
      return st;
  }

  /* Modo de direccionamiento SPI según el tamaño REAL, ya conocido tras la
   * autodetección JEDEC (antes se calculaba ANTES de sondear, con total_size=0,
   * de modo que todo dispositivo quedaba direccionado con 2 bytes): 16 bits
   * hasta 64 KiB y 24 bits hasta 16 MiB. Por encima de 16 MiB haría falta
   * dirección de 32 bits + EN4B, que este driver no implementa todavía: se
   * rechaza en vez de truncar la dirección. */
  if (d->cfg.addr_bytes == 0u && !l2_is_i2c(&d->cfg) &&
      d->cfg.type != MFS_L2_8BIT_INTERNAL_FLASH) {
    if (d->cfg.total_size > 0x1000000u)
      return MFS_ENOTSUP;
    d->cfg.addr_bytes = (d->cfg.total_size <= 0x10000u) ? 2u : 3u;
    d->cfg.addr_width = (d->cfg.addr_bytes == 2u) ? 16u : 24u;
  }

  d->initialized = true;
  *out_drv = d;
  return MFS_OK;
}

void mfs_l2_8bit_destroy(mfs_l2_8bit_driver_t *drv) {
  if (!drv)
    return;
  if (drv->cfg.spi_deinit && !l2_is_i2c(&drv->cfg) &&
      drv->cfg.type != MFS_L2_8BIT_INTERNAL_FLASH)
    drv->cfg.spi_deinit(drv->cfg.ctx);
  if (drv->cfg.i2c_deinit && l2_is_i2c(&drv->cfg))
    drv->cfg.i2c_deinit(drv->cfg.ctx);
  drv->initialized = false;
}

/* ==== Autodetección JEDEC (RDID 0x9F) ====
 * Lee 3 bytes de ID: [manufacturer, memory_type, capacity].
 * La capacidad se codifica como 2^n bytes (JEDEC standard). */
mfs_st mfs_l2_8bit_probe_spi(const mfs_l2_8bit_cfg_t *cfg,
                             mfs_l2_8bit_type_t *out_type, uint32_t *out_size) {
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