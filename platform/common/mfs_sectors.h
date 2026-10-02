/* mfs_sectors.h — MatrixFS: aritmética de sectores no uniformes.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Varias familias de MCU tienen sectores de borrado de TAMAÑO DISTINTO dentro
 * del mismo dispositivo (STM32F4: 16 KB ×4, 64 KB ×1, 128 KB ×3; STM32F1:
 * 1/2/4/…/128 KB; STM32L0: 128 B). El núcleo de MatrixFS, en cambio, asume un
 * único `erase_unit` para toda la región porque lo usa para dimensionar a la vez
 * SB B, el anillo de tokens y cada zona (ver src/mfs_internal.h).
 *
 * Este módulo resuelve la tensión sin tocar el núcleo: busca la mayor VENTANA
 * UNIFORME utilizable, es decir un tramo contiguo de sectores que se pueden
 * agrupar en unidades lógicas de igual tamaño. La unidad lógica debe ser
 * múltiplo exacto del sector físico (para que `erase(addr)` borre siempre
 * sectores completos) y abarcar un número entero de ellos.
 *
 * El integrador declara `erase_unit = window.unit` y su backend de borrado
 * elimina TODOS los sectores del dispositivo que cubren
 * [addr, addr + unit) — el núcleo solo borra en múltiplos de `unit`.
 */
#ifndef MFS_SECTORS_H
#define MFS_SECTORS_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs/mfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Un sector/página de borrado del dispositivo, en direcciones ABSOLUTAS. */
typedef struct {
  uint32_t addr; /* dirección de inicio del sector */
  uint32_t size; /* bytes del sector (> 0)          */
} mfs_sector_t;

/* Resultado de la búsqueda de ventana uniforme. */
typedef struct {
  uint32_t base;      /* inicio absoluto de la ventana                  */
  uint32_t size;      /* bytes utilizables de la ventana (= count·unit)  */
  uint32_t unit;      /* erase_unit lógico (múltiplo del sector físico)  */
  uint32_t phys_sector; /* tamaño del sector físico agrupado            */
  uint32_t count;     /* número de unidades lógicas de la ventana        */
  /* Capacidad direccionable real: el núcleo limita a MFS_ZONE_MAX zonas, así
   * que una ventana mayor no se aprovecha entera. Es el dato que el integrador
   * debe reportar al usuario. */
  uint32_t capacity_bytes;
  /* Bytes de [from, from+span) que quedan fuera de la ventana (descartados).
   * El integrador NO debe ocultarlo: una región que cruza un cambio de tamaño
   * de sector pierde la parte no uniforme. */
  uint32_t dropped_bytes;
} mfs_uniform_window_t;

/* Índice del sector que contiene `addr`, o -1 si `addr` no está cubierto. */
int32_t mfs_sector_index_of(const mfs_sector_t *map, uint32_t map_len,
                            uint32_t addr);

/* ¿Está [addr, addr+len) cubierto por sectores del mapa y empieza `addr` en
 * frontera de sector? (Requisito para que un borrado por tramo sea correcto.) */
bool mfs_sector_range_covered(const mfs_sector_t *map, uint32_t map_len,
                              uint32_t addr, uint32_t len);

/* Busca la mayor ventana uniforme contenida en [from, from+span).
 *
 *   min_unit   unidad lógica mínima aceptable (el núcleo fuerza >= 1024 B;
 *              pasar 1024). Si el sector físico es menor, se agrupan varios.
 *   max_zones  número máximo de zonas del núcleo (MFS_ZONE_MAX). Se usa para
 *              calcular `capacity_bytes` y para elegir entre ventanas.
 *
 * Criterio de elección: maximiza `min(max_zones, count) · unit`; a igualdad,
 * prefiere la unidad lógica MÁS PEQUEÑA (menos coste de borrado y mejor
 * granularidad de reclamación).
 *
 * Devuelve MFS_OK y rellena `out`, MFS_EINVAL si los argumentos son inválidos,
 * o MFS_ENOENT si no hay ninguna ventana válida en el rango pedido.
 */
mfs_st mfs_sector_uniform_window(const mfs_sector_t *map, uint32_t map_len,
                                 uint32_t from, uint32_t span,
                                 uint32_t min_unit, uint32_t max_zones,
                                 mfs_uniform_window_t *out);

#ifdef __cplusplus
}
#endif
#endif /* MFS_SECTORS_H */
