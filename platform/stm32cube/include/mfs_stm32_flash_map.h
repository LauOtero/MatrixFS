/* mfs_stm32_flash_map.h — mapa de sectores por familia STM32.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * La flash interna de un STM32 NO es uniforme en todas las familias: el F4
 * mezcla sectores de 16, 64 y 128 KB; el F7 de 32, 128 y 256 KB; el L0 usa
 * paginas de 128 B. El nucleo de MatrixFS, en cambio, exige un unico
 * `erase_unit` para toda la region (dimensiona SB B, el anillo de tokens y cada
 * zona: ver src/mfs_internal.h).
 *
 * Este modulo resuelve esa tension en dos pasos:
 *   1) EXPANDE la tabla compacta de la familia (lista de tramos) a una lista de
 *      sectores con direccion y tamano;
 *   2) busca la mayor VENTANA UNIFORME utilizable (delegando en
 *      platform/common/mfs_sectors.c).
 *
 * SEGURIDAD: `mfs_stm32_flash_map_validate()` comprueba que la tabla cubre
 * [base, base+size) de forma contigua, ordenada y sin huecos. Una tabla mal
 * escrita se detecta en el arranque en vez de provocar borrados del sector
 * equivocado. Las tablas de abajo reproducen la documentacion de ST; aun asi,
 * la via SIEMPRE correcta es que el integrador aporte su propia tabla
 * (`mfs_stm32_cfg.sectors`), tomada de su Reference Manual.
 */
#ifndef MFS_STM32_FLASH_MAP_H
#define MFS_STM32_FLASH_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs_stm32.h"      /* mfs_stm32_sector_t */
#include "mfs_sectors.h"         /* mfs_uniform_window_t */
#include "matrixfs/mfs_types.h"  /* mfs_st */

#ifdef __cplusplus
extern "C" {
#endif

/* Tramo compacto: `count` sectores consecutivos de `size` bytes. */
typedef struct {
  uint32_t count;
  uint32_t size;
} mfs_stm32_sector_run_t;

/* Descripcion de la flash de una familia. */
typedef struct {
  const char *name;    /* p. ej. "STM32F4 (1 MB)"                     */
  uint32_t flash_base; /* direccion base de la flash (0x08000000)      */
  uint32_t flash_size; /* bytes totales de la variante                */
  const mfs_stm32_sector_run_t *runs;
  uint32_t run_count;
} mfs_stm32_family_t;

/* Familia detectada en tiempo de compilacion a partir de las macros del CMSIS
 * (STM32F4xx, STM32H7xx, ...). Devuelve NULL si no hay tabla para esa familia:
 * en ese caso el integrador DEBE aportar `mfs_stm32_cfg.sectors`. */
const mfs_stm32_family_t *mfs_stm32_flash_family(void);

/* Expande `fam->runs` en `out` (hasta `cap` sectores) deteniendose al alcanzar
 * `limit_size` bytes o el final de la tabla. `out_n` recibe el numero de
 * sectores escritos. Devuelve MFS_OK, MFS_EINVAL o MFS_EOVERFLOW si `cap` es
 * insuficiente para la parte de la tabla que se quiere usar. */
mfs_st mfs_stm32_flash_expand(const mfs_stm32_family_t *fam,
                              mfs_stm32_sector_t *out, uint32_t cap,
                              uint32_t limit_size, uint32_t *out_n);

/* Numero de sectores que produce la expansion hasta `limit_size`. */
uint32_t mfs_stm32_flash_count(const mfs_stm32_family_t *fam,
                               uint32_t limit_size);

/* Valida que los `n` sectores cubran [base, base+size) contiguamente, en orden
 * ascendente y con tamanos > 0. Devuelve true si el mapa es utilizable. */
bool mfs_stm32_flash_map_validate(const mfs_stm32_sector_t *s, uint32_t n,
                                  uint32_t base, uint32_t size);

/* Busca la mayor ventana uniforme de [addr, addr+size) usando la tabla `s`.
 * `min_unit` debe ser >= 1024 (el nucleo fuerza ese minimo). Devuelve MFS_OK y
 * rellena `out`, o MFS_ENOENT si no hay ventana utilizable. */
mfs_st mfs_stm32_flash_uniform_window(const mfs_stm32_sector_t *s, uint32_t n,
                                      uint32_t addr, uint32_t size,
                                      uint32_t min_unit,
                                      mfs_uniform_window_t *out);

#ifdef __cplusplus
}
#endif
#endif /* MFS_STM32_FLASH_MAP_H */
