/* mfs_sectors.c — MatrixFS: aritmética de sectores no uniformes.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sin heap, sin dependencias de plataforma. Determinista: mismo mapa y mismo
 * rango producen siempre la misma ventana.
 */
#include "mfs_sectors.h"

#include <string.h>

int32_t mfs_sector_index_of(const mfs_sector_t *map, uint32_t map_len,
                            uint32_t addr) {
  if (!map)
    return -1;
  for (uint32_t i = 0; i < map_len; i++) {
    uint32_t s = map[i].addr;
    uint32_t sz = map[i].size;
    if (sz == 0u)
      continue;
    uint32_t e = s + sz;
    if (e < s) /* desbordamiento: sector inválido */
      continue;
    if (addr >= s && addr < e)
      return (int32_t)i;
  }
  return -1;
}

bool mfs_sector_range_covered(const mfs_sector_t *map, uint32_t map_len,
                              uint32_t addr, uint32_t len) {
  if (!map || len == 0u)
    return false;
  uint32_t end = addr + len;
  if (end < addr) /* desbordamiento */
    return false;

  uint32_t cur = addr;
  while (cur < end) {
    int32_t i = mfs_sector_index_of(map, map_len, cur);
    if (i < 0)
      return false;
    /* El tramo debe empezar en frontera de sector; si no, un borrado "del
     * bloque que contiene addr" no borraría exactamente lo pedido. */
    if (map[i].addr != cur)
      return false;
    uint32_t next = map[i].addr + map[i].size;
    if (next <= cur)
      return false;
    cur = next;
  }
  return cur == end;
}

mfs_st mfs_sector_uniform_window(const mfs_sector_t *map, uint32_t map_len,
                                 uint32_t from, uint32_t span,
                                 uint32_t min_unit, uint32_t max_zones,
                                 mfs_uniform_window_t *out) {
  if (!map || map_len == 0u || !out || span == 0u || min_unit == 0u ||
      max_zones == 0u)
    return MFS_EINVAL;

  uint32_t limit = from + span;
  if (limit < from) /* desbordamiento */
    return MFS_EINVAL;

  bool found = false;
  uint32_t best_cap = 0u;
  mfs_uniform_window_t best;
  memset(&best, 0, sizeof(best));

  /* Cada sector se prueba como INICIO de ventana. El mapa debe estar ordenado
   * por `addr` ascendente (es como lo generan las tablas por familia). */
  for (uint32_t i = 0; i < map_len; i++) {
    uint32_t u = map[i].size;
    if (u == 0u)
      continue;
    uint32_t s = map[i].addr;
    if (s < from || s >= limit)
      continue;

    /* Unidad lógica = menor múltiplo de `u` que alcanza min_unit. Así el
     * erase_unit del núcleo siempre cubre sectores físicos COMPLETOS, que es lo
     * que hace correcto el borrado por tramo (p. ej. L0: 128 B → 4096 B = 32
     * sectores; F1: 1 KB → 4096 B = 4 sectores). */
    uint32_t k = (min_unit + u - 1u) / u;
    if (k == 0u || k > (0xFFFFFFFFu / u))
      continue;
    uint32_t unit = u * k;

    /* Tramo contiguo de sectores de tamaño exactamente `u`, dentro del rango. */
    uint32_t run_end = s;
    for (uint32_t j = i; j < map_len; j++) {
      if (map[j].size != u || map[j].addr != run_end)
        break;
      uint32_t e = map[j].addr + map[j].size;
      if (e < map[j].addr || e > limit)
        break;
      run_end = e;
    }
    if (run_end <= s)
      continue;

    uint32_t count = (run_end - s) / unit; /* unidades lógicas enteras */
    if (count == 0u)
      continue;
    uint32_t win_bytes = count * unit;
    /* Capacidad direccionable real: el núcleo limita a MFS_ZONE_MAX zonas. */
    uint32_t cap = (count > max_zones ? max_zones : count) * unit;

    if (!found || cap > best_cap || (cap == best_cap && unit < best.unit)) {
      found = true;
      best_cap = cap;
      best.base = s;
      best.size = win_bytes;
      best.unit = unit;
      best.phys_sector = u;
      best.count = count;
      best.capacity_bytes = cap;
      best.dropped_bytes = span - win_bytes;
    }
  }

  if (!found)
    return MFS_ENOENT;
  *out = best;
  return MFS_OK;
}
