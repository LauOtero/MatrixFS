/* mfs_stm32_hal.h — capa de adaptacion al HAL de STM32Cube.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TODA la dependencia del HAL de la flash interna vive aqui. El motivo es
 * concreto: entre familias y versiones de CubeMX cambian
 *   · el nombre de la cabecera del dispositivo (stm32f4xx_hal.h, ...),
 *   · la FIRMA de HAL_FLASH_Program (uint64_t en F4/F7/L4/H7; uint32_t en
 *     versiones antiguas de F1/F0),
 *   · los campos de FLASH_EraseInitTypeDef (Page vs Sector, Banks, NbPages) y
 *     la seleccion de banco,
 *   · los nombres de las constantes FLASH_TYPEPROGRAM_*.
 * Aislarlos aqui hace que un cambio de familia o de version del HAL se resuelva
 * en UN fichero, y permite compilar y probar el resto del port en host contra
 * un modelo fiel (platform/stm32cube/test/shims).
 */
#ifndef MFS_STM32_HAL_H
#define MFS_STM32_HAL_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs/mfs_types.h"

/* `mfs_sector_t` lo usa el prototipo de mfs_stm32_hal_flash_set_sector_map().
 * En el build de host lo aportaba el shim de la flash interna; en destino hay
 * que incluirlo AQUI, o los TU que incluyen esta cabecera primero (p. ej.
 * mfs_stm32_hal.c y mfs_stm32_port.c) no compilan. */
#include "mfs_sectors.h"

/* ==== Seleccion de la cabecera del dispositivo ==== */
#if defined(MFS_STM32_HOST_TEST)
/* Build de host: se usa el modelo de flash de platform/stm32cube/test/shims. */
#include "stm32_hal_shim.h"
#elif defined(STM32F0xx)
#include "stm32f0xx_hal.h"
#elif defined(STM32F1xx)
#include "stm32f1xx_hal.h"
#elif defined(STM32F2xx)
#include "stm32f2xx_hal.h"
#elif defined(STM32F3xx)
#include "stm32f3xx_hal.h"
#elif defined(STM32F4xx)
#include "stm32f4xx_hal.h"
#elif defined(STM32F7xx)
#include "stm32f7xx_hal.h"
#elif defined(STM32G0xx)
#include "stm32g0xx_hal.h"
#elif defined(STM32G4xx)
#include "stm32g4xx_hal.h"
#elif defined(STM32H5xx)
#include "stm32h5xx_hal.h"
#elif defined(STM32H7xx)
#include "stm32h7xx_hal.h"
#elif defined(STM32L0xx)
#include "stm32l0xx_hal.h"
#elif defined(STM32L1xx)
#include "stm32l1xx_hal.h"
#elif defined(STM32L4xx)
#include "stm32l4xx_hal.h"
#elif defined(STM32L5xx)
#include "stm32l5xx_hal.h"
#elif defined(STM32U5xx)
#include "stm32u5xx_hal.h"
#elif defined(STM32WBxx)
#include "stm32wbxx_hal.h"
#elif defined(STM32WLxx)
#include "stm32wlxx_hal.h"
#else
#error                                                                         \
    "MatrixFS: familia STM32 no reconocida. Define el macro del dispositivo (p. ej. STM32F4xx) o MFS_STM32_HOST_TEST. Anade la cabecera HAL correspondiente a mfs_stm32_hal.h."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Direccion base de la flash interna (0x08000000 en todas las familias) ==
 */
#define MFS_STM32_FLASH_BASE_ADDR 0x08000000u

/* ==== Envoltorios de la flash interna ==== */

/* Tamano total de la flash del dispositivo en bytes. En destino se toma del
 * registro de tamano del module (FLASH_SIZE) o del macro del CMSIS; en host,
 * del modelo. */
uint32_t mfs_stm32_hal_flash_size(void);

/* Desbloquea/bloquea el controlador de flash. */
mfs_st mfs_stm32_hal_flash_unlock(void);
mfs_st mfs_stm32_hal_flash_lock(void);

/* Lee `len` bytes de `addr` de la flash interna.
 *
 * En destino la flash interna esta mapeada en el espacio de direcciones (XIP),
 * de modo que basta un memcpy; pero se expone como funcion para que:
 *   · el nucleo no dependa de que la region sea accesible por puntero, y
 *   · el backend se pueda PROBAR en host, donde la flash simulada vive en RAM y
 *     una direccion 0x0800xxxx *no* es valida (era un fallo de acceso real).
 * El llamante (backend) ya valida el rango. */
mfs_st mfs_stm32_hal_flash_read(uint32_t addr, void *dst, uint32_t len);

/* Programa `unit` bytes (2/4/8 o 16) en `addr`, que debe estar alineada a
 * `unit`.
 *
 * IMPORTANTE: recibe un PUNTERO, no un uint64_t. La razon es que una unidad de
 * 16 bytes (quadword de H5/H7/U5) NO CABE en el `uint64_t Data` de
 * `HAL_FLASH_Program`, y volcarla en un uint64_t seria un desbordamiento de
 * buffer (lo detecto el modelo de host con -Werror=array-bounds). Aqui no se
 * trunca nunca: o se programa la unidad completa o se devuelve error. */
mfs_st mfs_stm32_hal_flash_program_bytes(uint32_t addr, const uint8_t *bytes,
                                         uint32_t unit);

/* Gancho para las familias de unidad de 16 bytes. En H5/H7/U5 el HAL recibe
 * solo 64 bits, de modo que la quadword completa exige una secuencia especifica
 * de la familia (registros de datos de la flash). Esta funcion es DEBIL: por
 * defecto devuelve MFS_ENOTSUP, de modo que el port **rechaza** programar de 16
 * en 16 bytes en vez de truncar en silencio. El integrador de esas familias
 * debe implementarla (ver README §Flash interna). */
mfs_st mfs_stm32_hal_flash_program_quadword(uint32_t addr,
                                            const uint8_t bytes[16]);

/* Instala la tabla de sectores del dispositivo. OBLIGATORIO antes de borrar:
 * el indice del sector no se puede deducir dividiendo por el tamano cuando los
 * sectores NO son uniformes (F1/F4/F7/L0/U5...), y un indice erroneo hace que
 * HAL_FLASHEx_Erase borre OTRO sector. Lo llama mfs_stm32_iflash_prepare(). */
void mfs_stm32_hal_flash_set_sector_map(const mfs_sector_t *map, uint32_t n);

/* Borra el sector que CONTIENE `addr` (buscado en la tabla instalada), cuyo
 * tamano es `size`. Absorbe las diferencias de FLASH_EraseInitTypeDef y la
 * seleccion de banco. Devuelve MFS_EINVAL si no hay tabla instalada. */
mfs_st mfs_stm32_hal_flash_erase_sector(uint32_t addr, uint32_t size);

/* Programa un BLOQUE CONTIGUO de `len` bytes en `addr` (alineado a `unit`),
 * con un UNICO ciclo unlock/program/lock. Esto evita la latencia del bus por
 * desbloquear/bloquear el controlador de flash por cada unidad de programacion
 * (antes: una escritura de 64 B en un F4 hacia 16 ciclos unlock/lock). El
 * bloque debe ser multiplo de `unit` y `addr` debe estar alineado a `unit`. */
mfs_st mfs_stm32_hal_flash_program_block(uint32_t addr, const uint8_t *bytes,
                                         uint32_t len, uint32_t unit);

/* Cierto en familias con un solo banco de flash: programar o borrar detiene la
 * busqueda de instrucciones durante toda la operacion (penalizacion RWW). */
bool mfs_stm32_hal_flash_single_bank(void);

#ifdef __cplusplus
}
#endif
#endif /* MFS_STM32_HAL_H */
