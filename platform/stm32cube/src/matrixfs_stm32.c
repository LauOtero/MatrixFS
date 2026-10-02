/* matrixfs_stm32.c — port de MatrixFS a STM32Cube: API publica y despacho.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Este fichero NO sabe nada del HAL: elige el backend segun `cfg->media`,
 * construye la configuracion del nucleo por la via que corresponda (region plana
 * o driver L2) y expone el ciclo de vida y el diagnostico.
 *
 * ESTADO ESTATICO (sin heap). El port guarda:
 *   · una COPIA de la configuracion (para que el llamador no tenga que
 *     mantenerla viva, salvo `key` y `sectors`, que se copian/punteran segun el
 *     caso y se documenta);
 *   · el descriptor de la region plana o el driver L2;
 *   · la instancia del nucleo (la aporta el llamador) para el diagnostico.
 */
#include "matrixfs_stm32.h"

#include <stdio.h>
#include <string.h>

#include "mfs_stm32_backend.h"
#include "mfs_stm32_hal.h"
/* mf_t es OPACO en include/matrixfs: el port necesita s->cfg y s->hwv,
 * de modo que incluye la cabecera interna del nucleo, igual que hacen
 * platform/common/mfs_vfs.c y platform/embedded/mfs_embedded.c. El componente
 * publica src/ entre sus include dirs precisamente por esto. */
#include "mfs_internal.h"

#ifndef MFS_STM32_LAST_ERR_LEN
#define MFS_STM32_LAST_ERR_LEN 160u
#endif

/* =====================================================================
 * Estado estatico
 * ===================================================================== */
static mfs_stm32_cfg s_cfg;              /* copia de la configuracion activa */
static mfs_embedded_flash_t s_flash;     /* via A: region plana              */
static mfs_config s_core_cfg;            /* via B: config del nucleo         */
static mfs_embedded_opts s_opts;         /* opciones traducidas              */
static mf_t *s_fs = NULL;                /* volumen montado (propiedad ajena) */
static mfs_hwv_t s_hwv;                  /* HWV del ultimo montaje           */
static bool s_have_hwv = false;
static uint32_t s_capacity = 0u;
static bool s_capacity_wasteful = false;
static uint8_t s_key_copy[32];           /* copia de la clave (32 B)         */
static bool s_key_present = false;
static char s_last_err[MFS_STM32_LAST_ERR_LEN];

static mfs_stm32_lock_fn s_lock = NULL;
static mfs_stm32_lock_fn s_unlock = NULL;

/* =====================================================================
 * Utilidades
 * ===================================================================== */

static void stm32_set_ok(void) { snprintf(s_last_err, sizeof(s_last_err), "ok"); }

static mfs_st stm32_fail(mfs_st st, const char *msg) {
  snprintf(s_last_err, sizeof(s_last_err), "%s: %s", msg, mfs_ststr(st));
  return st;
}

/* Traduce la configuracion del port a las opciones de la capa embebida. */
static void stm32_build_opts(mfs_stm32_cfg *c) {
  mfs_embedded_opts_default(&s_opts);
  s_opts.ram_total = c->ram_total;
  s_opts.forced_mode = c->forced_mode;
  s_opts.suite_preferred = c->suite_preferred;
  s_opts.bus_speed_hz = c->bus_speed_hz;
  s_opts.uid = c->uid;
  s_opts.gid = c->gid;
  if (c->file_perm)
    s_opts.file_perm = c->file_perm;
  if (c->dir_perm)
    s_opts.dir_perm = c->dir_perm;
  s_opts.allow_convergent = c->allow_convergent;
  s_opts.dedup_enable = c->dedup_enable;
  s_opts.cdc_enable = c->cdc_enable;
  s_opts.zrp_enable = c->zrp_enable;
  s_opts.dab_enable = c->dab_enable;
  s_opts.format_if_needed = c->format_if_needed;
  s_opts.key = NULL;
  if (c->key) {
    /* Se copia la clave: el llamador no tiene por que mantenerla viva. */
    memcpy(s_key_copy, c->key, sizeof(s_key_copy));
    s_key_present = true;
    s_opts.key = s_key_copy;
  } else {
    s_key_present = false;
  }
}

/* Calcula el diagnostico de capacidad a partir del HWV resuelto. El nucleo
 * direcciona MFS_ZONE_MAX zonas de `erase_unit` tras los 3 sectores reservados
 * (SB A, SB B y anillo de tokens: src/mfs_internal.h). */
static void stm32_capacity_from_hwv(void) {
  uint32_t eu = s_hwv.erase_unit;
  uint32_t total = s_hwv.media_size;
  if (eu == 0u || total < 4u * eu) {
    s_capacity = 0u;
    s_capacity_wasteful = false;
    return;
  }
  uint64_t usable = (uint64_t)(total / eu - 3u) * eu;
  uint64_t cap = (uint64_t)MFS_ZONE_MAX * eu;
  s_capacity = (uint32_t)(usable < cap ? usable : cap);
  s_capacity_wasteful = (usable > cap);
}

/* =====================================================================
 * Montaje / formateo
 * ===================================================================== */

/* Comprueba la configuracion y normaliza los valores por defecto. */
static mfs_st stm32_check_cfg(const mfs_stm32_cfg *cfg) {
  if (!cfg)
    return MFS_EINVAL;
  switch (cfg->media) {
  case MFS_STM32_MEDIA_IFLASH:
    if (cfg->region_addr == 0u || cfg->region_size == 0u)
      return MFS_EINVAL;
    break;
  case MFS_STM32_MEDIA_OSPI_NOR:
    if (!cfg->ospi)
      return MFS_EINVAL;
    break;
  case MFS_STM32_MEDIA_SPI_NOR:
  case MFS_STM32_MEDIA_FRAM:
  case MFS_STM32_MEDIA_EEPROM:
    /* SPI o I2C segun cfg->bus. */
    if (cfg->bus == MFS_STM32_BUS_I2C) {
      if (!cfg->i2c)
        return MFS_EINVAL;
    } else if (!cfg->spi) {
      return MFS_EINVAL;
    }
    /* FRAM/EEPROM no tienen geometria detectable fiable: es obligatoria. */
    if ((cfg->media == MFS_STM32_MEDIA_FRAM ||
         cfg->media == MFS_STM32_MEDIA_EEPROM) &&
        cfg->total_size == 0u)
      return MFS_EINVAL;
    break;
  case MFS_STM32_MEDIA_SDMMC:
    if (!cfg->sd)
      return MFS_EINVAL;
    break;
  default:
    return MFS_EINVAL;
  }
  /* El nucleo no es viable con menos RAM declarada de la cuenta. No se impone un
   * minimo (el planificador devolvera MFS_ENOTVIABLE), pero 0 significa
   * "no declarado" y provoca un modo poco realista. */
  if (cfg->ram_total == 0u)
    return MFS_EINVAL;
  return MFS_OK;
}

/* Prepara el medio. Devuelve si hay que usar la via L2 (driver propio). */
static mfs_st stm32_prepare_media(const mfs_stm32_cfg *c, bool *use_l2) {
  *use_l2 = false;
  mfs_st st;
  switch (c->media) {
  case MFS_STM32_MEDIA_IFLASH:
    return mfs_stm32_iflash_prepare(c, &s_flash);
  case MFS_STM32_MEDIA_OSPI_NOR:
    return mfs_stm32_ospi_prepare(c, &s_flash);
  case MFS_STM32_MEDIA_SPI_NOR:
  case MFS_STM32_MEDIA_FRAM:
  case MFS_STM32_MEDIA_EEPROM: {
    /* Los tres usan platform/common/mfs_l2_8bit.c; el tipo concreto lo
     * selecciona el propio medio (geometria y semantica de borrado distintas:
     * la EEPROM tiene page write y la FRAM no tiene borrado). */
    const mfs_l2_driver *l2 = NULL;
    const mfs_media_geom *g = NULL;
    st = mfs_stm32_mem_prepare(c, c->media, &l2, &g);
    if (st != MFS_OK)
      return st;
    *use_l2 = true;
    return mfs_embedded_bind(&s_core_cfg, l2, g, &s_opts);
  }
  case MFS_STM32_MEDIA_SDMMC: {
    const mfs_l2_driver *l2 = NULL;
    const mfs_media_geom *g = NULL;
    st = mfs_stm32_sd_prepare(c, &l2, &g);
    if (st != MFS_OK)
      return st;
    *use_l2 = true;
    return mfs_embedded_bind(&s_core_cfg, l2, g, &s_opts);
  }
  default:
    return MFS_EINVAL;
  }
}

mfs_st matrixfs_stm32_mount(mf_t *fs, const mfs_stm32_cfg *cfg) {
  if (!fs)
    return stm32_fail(MFS_EINVAL, "fs es NULL");
  mfs_st st = stm32_check_cfg(cfg);
  if (st != MFS_OK)
    return stm32_fail(st, "configuracion invalida");

  matrixfs_stm32_lock();
  s_cfg = *cfg; /* copia: el llamador puede liberar la suya */
  stm32_build_opts(&s_cfg);
  s_fs = NULL;
  s_have_hwv = false;

  bool use_l2 = false;
  st = stm32_prepare_media(&s_cfg, &use_l2);
  if (st != MFS_OK) {
    matrixfs_stm32_unlock();
    return stm32_fail(st, "medio no preparado");
  }

  if (use_l2) {
    fs->cfg = &s_core_cfg;
    st = (mfs_st)mf_init(fs, &s_core_cfg);
    if (st != MFS_OK && s_opts.format_if_needed &&
        (st == MFS_ECORRUPT || st == MFS_EIO || st == MFS_EBADMSG)) {
      fs->cfg = &s_core_cfg;
      st = (mfs_st)mf_format(fs, NULL);
      if (st == MFS_OK)
        st = (mfs_st)mf_init(fs, &s_core_cfg);
    }
  } else {
    st = mfs_embedded_mount(fs, &s_flash, &s_opts);
  }

  if (st != MFS_OK) {
    matrixfs_stm32_unlock();
    return stm32_fail(st, "montaje fallido");
  }

  s_fs = fs;
  s_hwv = fs->hwv;
  s_have_hwv = true;
  stm32_capacity_from_hwv();
  stm32_set_ok();
  matrixfs_stm32_unlock();
  return MFS_OK;
}

mfs_st matrixfs_stm32_format(mf_t *fs, const mfs_stm32_cfg *cfg) {
  if (!fs)
    return stm32_fail(MFS_EINVAL, "fs es NULL");
  mfs_st st = stm32_check_cfg(cfg);
  if (st != MFS_OK)
    return stm32_fail(st, "configuracion invalida");

  matrixfs_stm32_lock();
  s_cfg = *cfg;
  stm32_build_opts(&s_cfg);
  s_fs = NULL;
  s_have_hwv = false;

  bool use_l2 = false;
  st = stm32_prepare_media(&s_cfg, &use_l2);
  if (st != MFS_OK) {
    matrixfs_stm32_unlock();
    return stm32_fail(st, "medio no preparado");
  }

  if (use_l2) {
    fs->cfg = &s_core_cfg;
    st = (mfs_st)mf_format(fs, NULL);
  } else {
    st = mfs_embedded_format(fs, &s_flash, &s_opts);
  }

  if (st != MFS_OK) {
    matrixfs_stm32_unlock();
    return stm32_fail(st, "formateo fallido");
  }
  /* Formatear deja el volumen SIN montar. */
  stm32_set_ok();
  matrixfs_stm32_unlock();
  return MFS_OK;
}

mfs_st matrixfs_stm32_deinit(mf_t *fs) {
  if (!fs)
    return MFS_EINVAL;
  mfs_st st = (mfs_st)mf_deinit(fs);
  mfs_stm32_backend_close();
  s_fs = NULL;
  s_have_hwv = false;
  s_capacity = 0u;
  s_capacity_wasteful = false;
  return st;
}

/* =====================================================================
 * Diagnostico
 * ===================================================================== */

const char *matrixfs_stm32_last_error(void) {
  return s_last_err[0] ? s_last_err : "ok";
}

uint32_t matrixfs_stm32_capacity_bytes(void) { return s_capacity; }

bool matrixfs_stm32_capacity_wasteful(void) { return s_capacity_wasteful; }

const mfs_hwv_t *matrixfs_stm32_hwv(void) { return s_have_hwv ? &s_hwv : NULL; }

const char *matrixfs_stm32_media_name(mfs_stm32_media_t m) {
  switch (m) {
  case MFS_STM32_MEDIA_IFLASH:
    return "flash interna (HAL_FLASH)";
  case MFS_STM32_MEDIA_OSPI_NOR:
    return "NOR externa OSPI/QUADSPI";
  case MFS_STM32_MEDIA_SPI_NOR:
    return "NOR externa SPI";
  case MFS_STM32_MEDIA_FRAM:
    return "FRAM SPI/I2C";
  case MFS_STM32_MEDIA_EEPROM:
    return "EEPROM SPI/I2C";
  case MFS_STM32_MEDIA_SDMMC:
    return "SD/eMMC (HAL_SD)";
  default:
    return "desconocido";
  }
}

/* =====================================================================
 * Sincronizacion
 * ===================================================================== */

void matrixfs_stm32_set_lock_hooks(mfs_stm32_lock_fn lock,
                                   mfs_stm32_lock_fn unlock) {
  s_lock = lock;
  s_unlock = unlock;
}

void matrixfs_stm32_lock(void) {
  if (s_lock)
    s_lock();
}

void matrixfs_stm32_unlock(void) {
  if (s_unlock)
    s_unlock();
}

/* =====================================================================
 * Montaje con la configuracion compilada (mfs_stm32_conf.h)
 * ===================================================================== */
#if defined(MFS_STM32_USE_CONF)
#include "mfs_stm32_conf.h"

mfs_st matrixfs_stm32_mount_default(mf_t *fs) {
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = (mfs_stm32_media_t)MFS_STM32_MEDIA;
  c.ram_total = (uint32_t)MFS_STM32_RAM_BUDGET;
  c.forced_mode = (mfs_mode_t)MFS_STM32_FORCED_MODE;
  c.format_if_needed = (MFS_STM32_FORMAT_IF_NEEDED != 0);
#ifdef MFS_STM32_SUITE_PREFERRED
  c.suite_preferred = (uint8_t)MFS_STM32_SUITE_PREFERRED;
#else
  c.suite_preferred = 0xFFu;
#endif

  /* Region de la flash interna. */
#ifdef MFS_STM32_IFLASH_ADDR
  c.region_addr = (uint32_t)MFS_STM32_IFLASH_ADDR;
#endif
#ifdef MFS_STM32_IFLASH_REGION_SIZE
  c.region_size = (uint32_t)MFS_STM32_IFLASH_REGION_SIZE;
#endif
#ifdef MFS_STM32_IFLASH_SECTORS
  c.sectors = MFS_STM32_IFLASH_SECTORS;
  c.sector_count = (uint32_t)MFS_STM32_IFLASH_SECTOR_COUNT;
#endif
#ifdef MFS_STM32_KEY
  c.key = (const uint8_t *)MFS_STM32_KEY;
#endif

  /* Handles de los perifericos: los macros de mfs_stm32_conf.h evaluan al
   * PUNTERO del handle que declara CubeMX (ver la plantilla). */
#if MFS_STM32_MEDIA == MFS_STM32_MEDIA_OSPI_NOR
#if defined(MFS_STM32_QSPI_HANDLE) && !defined(STM32H7xx) && !defined(STM32U5xx) && \
    !defined(STM32L5xx)
  c.ospi = (void *)MFS_STM32_QSPI_HANDLE;
#else
  c.ospi = (void *)MFS_STM32_OSPI_HANDLE;
#endif
#ifdef MFS_STM32_OSPI_TOTAL_SIZE
  c.total_size = (uint32_t)MFS_STM32_OSPI_TOTAL_SIZE;
#endif
#ifdef MFS_STM32_OSPI_MEMORY_MAPPED
  c.memory_mapped = (MFS_STM32_OSPI_MEMORY_MAPPED != 0);
#endif
#elif MFS_STM32_MEDIA == MFS_STM32_MEDIA_SPI_NOR
  c.spi = (void *)MFS_STM32_SPI_HANDLE;
  c.bus = MFS_STM32_BUS_SPI;
#elif MFS_STM32_MEDIA == MFS_STM32_MEDIA_FRAM ||                               \
    MFS_STM32_MEDIA == MFS_STM32_MEDIA_EEPROM
#if MFS_STM32_MEM_BUS_I2C
  c.bus = MFS_STM32_BUS_I2C;
  c.i2c = (void *)MFS_STM32_I2C_HANDLE;
  c.i2c_addr = (uint8_t)MFS_STM32_I2C_ADDR;
#else
  c.bus = MFS_STM32_BUS_SPI;
  c.spi = (void *)MFS_STM32_SPI_HANDLE;
#endif
  c.total_size = (uint32_t)MFS_STM32_MEM_TOTAL_SIZE;
  c.page_size = (uint32_t)MFS_STM32_MEM_PAGE_SIZE;
  c.erase_unit = (uint32_t)MFS_STM32_MEM_ERASE_UNIT;
#elif MFS_STM32_MEDIA == MFS_STM32_MEDIA_SDMMC
  c.sd = (void *)MFS_STM32_SD_HANDLE;
#endif

  return matrixfs_stm32_mount(fs, &c);
}
#endif /* MFS_STM32_USE_CONF */
