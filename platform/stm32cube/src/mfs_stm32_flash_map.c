/* mfs_stm32_flash_map.c — mapa de sectores por familia STM32.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sin heap y sin dependencias del HAL: compila en host (para los tests) y en
 * destino. Las tablas usan la representacion por TRAMOS para que sean legibles
 * y verificables a simple vista.
 */
#include "mfs_stm32_flash_map.h"

#include <string.h>

#define MFS_STM32_FLASH_BASE 0x08000000u

/* Se compilan las tablas de TODAS las familias y solo se usa la que selecciona
 * el macro del dispositivo (en el build de host no se usa ninguna). El atributo
 * evita el aviso -Wunused-const-variable sin tener que condicionar la
 * compilacion tabla por tabla, que haria el fichero ilegible. */
#if defined(__GNUC__) || defined(__clang__)
#define MFS_STM32_TABLE __attribute__((unused))
#else
#define MFS_STM32_TABLE
#endif

/* =====================================================================
 * Tablas por familia (representacion por tramos).
 *
 * Cada tabla describe una VARIANTE COMPLETA de la flash. La variante se elige
 * con MFS_STM32_IFLASH_SIZE (definida en mfs_stm32_conf.h); si no se define, se
 * usa el tamano por defecto de la familia.
 * ===================================================================== */

/* --- STM32F4, 1 MB: 2 bancos de 8 sectores (4×16K + 1×64K + 3×128K) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F4_1M_RUNS[] = {
    {4u, 16u * 1024u},
    {1u, 64u * 1024u},
    {7u, 128u * 1024u},
};

/* --- STM32F4, 2 MB: 2 bancos de 12 sectores (4×16K + 1×64K + 7×128K) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F4_2M_RUNS[] = {
    {4u, 16u * 1024u},
    {1u, 64u * 1024u},
    {7u, 128u * 1024u},
    {4u, 16u * 1024u},
    {1u, 64u * 1024u},
    {7u, 128u * 1024u},
};

/* --- STM32F7, 1 MB: 2 bancos de 8 sectores (4×32K + 1×128K + 3×256K) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F7_1M_RUNS[] = {
    {4u, 32u * 1024u},
    {1u, 128u * 1024u},
    {3u, 256u * 1024u},
    {4u, 32u * 1024u},
    {1u, 128u * 1024u},
    {3u, 256u * 1024u},
};

/* --- STM32F7, 2 MB: 2 bancos de 12 sectores (4×32K + 1×128K + 7×256K) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F7_2M_RUNS[] = {
    {4u, 32u * 1024u},
    {1u, 128u * 1024u},
    {7u, 256u * 1024u},
    {4u, 32u * 1024u},
    {1u, 128u * 1024u},
    {7u, 256u * 1024u},
};

/* --- STM32F1, densidad media (<= 128 KB): 128 paginas de 1 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F1_MD_RUNS[] = {{128u, 1024u}};

/* --- STM32F1, densidad alta (256/512 KB): paginas de 2 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F1_HD_RUNS[] = {{256u, 2048u}};

/* --- STM32F0 (F03x/F07x, <= 256 KB): paginas de 1 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F0_RUNS[] = {{256u, 1024u}};

/* --- STM32F3: paginas de 2 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t F3_RUNS[] = {{256u, 2048u}};

/* --- STM32L0/L1: paginas de 128 B (L0) y 256 B (L1) ---
 * OJO: por debajo del minimo de 1024 B que exige el nucleo
 * (src/core/mfs_hal.c fuerza erase_unit >= 1024). El helper de ventana
 * uniforme AGRUPA estas paginas (32 × 128 B = 4 KB; 4 × 256 B = 1 KB), que es
 * justo lo que hace falta. */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t L0_RUNS[] = {{1536u, 128u}}; /* 192 KB */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t L1_RUNS[] = {{2048u, 256u}}; /* 512 KB */

/* --- STM32L4/L4+/L5: paginas de 2 KB (hasta 512 KB) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t L4_RUNS[] = {{256u, 2048u}};

/* --- STM32G0: paginas de 2 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t G0_RUNS[] = {{64u, 2048u}};

/* --- STM32G4: paginas de 2 KB (hasta 512 KB) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t G4_RUNS[] = {{256u, 2048u}};

/* --- STM32H5/U5: paginas de 8 KB (2 MB) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t H5_RUNS[] = {{256u, 8192u}};
MFS_STM32_TABLE static const mfs_stm32_sector_run_t U5_RUNS[] = {{256u, 8192u}};

/* --- STM32H7: 2 bancos de 8 sectores de 128 KB (uniforme) --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t H7_2M_RUNS[] = {{16u, 128u * 1024u}};

/* --- STM32WB/WL: paginas de 4 KB --- */
MFS_STM32_TABLE static const mfs_stm32_sector_run_t WB_RUNS[] = {{256u, 4096u}};
MFS_STM32_TABLE static const mfs_stm32_sector_run_t WL_RUNS[] = {{64u, 2048u}};

#define DESC(nm, sz, runs)                                                     \
  { nm, MFS_STM32_FLASH_BASE, sz, runs, (uint32_t)(sizeof(runs) / sizeof((runs)[0])) }

/* Tamano de la variante: el integrador puede forzarlo con MFS_STM32_IFLASH_SIZE
 * (por ejemplo desde CubeMX). Si no, se usa el maximo tipico de la familia.
 *
 * OJO: se evalua en directivas #if, asi que NO puede llevar un cast: el
 * preprocesador no conoce tipos y falla con 'missing binary operator before
 * token'. */
#ifdef MFS_STM32_IFLASH_SIZE
#define MFS_STM32_VARIANT_SIZE (MFS_STM32_IFLASH_SIZE)
#else
#define MFS_STM32_VARIANT_SIZE 0 /* 0 = tamano por defecto de la familia */
#endif

#if defined(STM32F4xx)
#if MFS_STM32_VARIANT_SIZE == 0 || MFS_STM32_VARIANT_SIZE > (1024 * 1024)
static const mfs_stm32_family_t FAMILY = DESC("STM32F4 (2 MB)", 2u * 1024u * 1024u,
                                               F4_2M_RUNS);
#else
static const mfs_stm32_family_t FAMILY = DESC("STM32F4 (1 MB)", 1u * 1024u * 1024u,
                                               F4_1M_RUNS);
#endif
#elif defined(STM32F7xx)
#if MFS_STM32_VARIANT_SIZE == 0 || MFS_STM32_VARIANT_SIZE > (1024 * 1024)
static const mfs_stm32_family_t FAMILY = DESC("STM32F7 (2 MB)", 2u * 1024u * 1024u,
                                               F7_2M_RUNS);
#else
static const mfs_stm32_family_t FAMILY = DESC("STM32F7 (1 MB)", 1u * 1024u * 1024u,
                                               F7_1M_RUNS);
#endif
#elif defined(STM32F1xx)
#if MFS_STM32_VARIANT_SIZE != 0 && MFS_STM32_VARIANT_SIZE <= (128 * 1024)
static const mfs_stm32_family_t FAMILY = DESC("STM32F1 densidad media", 128u * 1024u,
                                               F1_MD_RUNS);
#else
static const mfs_stm32_family_t FAMILY = DESC("STM32F1 densidad alta", 512u * 1024u,
                                               F1_HD_RUNS);
#endif
#elif defined(STM32F0xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32F0", 256u * 1024u, F0_RUNS);
#elif defined(STM32F3xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32F3", 512u * 1024u, F3_RUNS);
#elif defined(STM32L0xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32L0", 192u * 1024u, L0_RUNS);
#elif defined(STM32L1xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32L1", 512u * 1024u, L1_RUNS);
#elif defined(STM32L4xx) || defined(STM32L5xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32L4/L5", 512u * 1024u, L4_RUNS);
#elif defined(STM32G0xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32G0", 128u * 1024u, G0_RUNS);
#elif defined(STM32G4xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32G4", 512u * 1024u, G4_RUNS);
#elif defined(STM32H5xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32H5", 2u * 1024u * 1024u, H5_RUNS);
#elif defined(STM32U5xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32U5", 2u * 1024u * 1024u, U5_RUNS);
#elif defined(STM32H7xx)
static const mfs_stm32_family_t FAMILY = DESC("STM32H7", 2u * 1024u * 1024u,
                                               H7_2M_RUNS);
#elif defined(STM32WBxx)
static const mfs_stm32_family_t FAMILY = DESC("STM32WB", 1u * 1024u * 1024u, WB_RUNS);
#elif defined(STM32WLxx)
static const mfs_stm32_family_t FAMILY = DESC("STM32WL", 128u * 1024u, WL_RUNS);
#else
/* Familia no tabulada (o compilacion en host): el integrador DEBE aportar su
 * propia tabla en mfs_stm32_cfg.sectors. No se inventa ninguna geometria. */
static const mfs_stm32_family_t FAMILY = {"(sin tabla)", MFS_STM32_FLASH_BASE, 0u,
                                          NULL, 0u};
#endif

const mfs_stm32_family_t *mfs_stm32_flash_family(void) {
  if (FAMILY.runs == NULL || FAMILY.run_count == 0u)
    return NULL;
  return &FAMILY;
}

uint32_t mfs_stm32_flash_count(const mfs_stm32_family_t *fam,
                               uint32_t limit_size) {
  if (!fam || !fam->runs)
    return 0u;
  uint32_t limit = (limit_size == 0u || limit_size > fam->flash_size)
                       ? fam->flash_size
                       : limit_size;
  uint32_t total = 0u;
  uint32_t n = 0u;
  for (uint32_t r = 0; r < fam->run_count && total < limit; r++) {
    for (uint32_t i = 0; i < fam->runs[r].count; i++) {
      if (total + fam->runs[r].size > limit)
        return n; /* el tramo no cabe entero: se corta aqui */
      total += fam->runs[r].size;
      n++;
    }
  }
  return n;
}

mfs_st mfs_stm32_flash_expand(const mfs_stm32_family_t *fam,
                              mfs_stm32_sector_t *out, uint32_t cap,
                              uint32_t limit_size, uint32_t *out_n) {
  if (!fam || !out || !out_n || !fam->runs)
    return MFS_EINVAL;
  uint32_t limit = (limit_size == 0u || limit_size > fam->flash_size)
                       ? fam->flash_size
                       : limit_size;
  uint32_t addr = fam->flash_base;
  uint32_t total = 0u;
  uint32_t n = 0u;
  for (uint32_t r = 0; r < fam->run_count && total < limit; r++) {
    for (uint32_t i = 0; i < fam->runs[r].count; i++) {
      uint32_t sz = fam->runs[r].size;
      if (sz == 0u)
        return MFS_EINVAL;
      if (total + sz > limit)
        goto done; /* no cabe entero */
      if (n >= cap)
        return MFS_EOVERFLOW; /* hace falta mas capacidad de la prevista */
      out[n].addr = addr;
      out[n].size = sz;
      n++;
      addr += sz;
      total += sz;
    }
  }
done:
  *out_n = n;
  return MFS_OK;
}

bool mfs_stm32_flash_map_validate(const mfs_stm32_sector_t *s, uint32_t n,
                                  uint32_t base, uint32_t size) {
  if (!s || n == 0u || size == 0u)
    return false;
  uint32_t cur = base;
  uint32_t end = base + size;
  if (end < base)
    return false;
  for (uint32_t i = 0; i < n; i++) {
    if (s[i].size == 0u)
      return false;
    if (s[i].addr != cur)
      return false; /* hueco, solape o desorden */
    uint32_t next = s[i].addr + s[i].size;
    if (next < s[i].addr || next > end)
      return false;
    cur = next;
    if (cur == end)
      return (i + 1u == n) ? true : false; /* sobra tabla despues del final */
  }
  return cur == end;
}

mfs_st mfs_stm32_flash_uniform_window(const mfs_stm32_sector_t *s, uint32_t n,
                                      uint32_t addr, uint32_t size,
                                      uint32_t min_unit,
                                      mfs_uniform_window_t *out) {
  if (!s || !out || n == 0u || size == 0u)
    return MFS_EINVAL;
  if (min_unit < 1024u)
    min_unit = 1024u; /* minimo que impone el nucleo (mfs_hal.c) */

  /* Toda la aritmetica (agrupacion de sectores pequenos, capacidad segun
   * MFS_ZONE_MAX y desempate) vive en platform/common/mfs_sectors.c, donde
   * tiene tests unitarios propios. Aqui solo se fija la politica. */
  return mfs_sector_uniform_window(s, n, addr, size, min_unit,
                                   (uint32_t)MFS_ZONE_MAX, out);
}
