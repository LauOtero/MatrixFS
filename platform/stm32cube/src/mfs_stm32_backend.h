/* mfs_stm32_backend.h — contrato interno entre el port y los backends de medio.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * NO es API publica. Cada backend traduce un periferico del HAL a UNA de las dos
 * vias que el port sabe montar:
 *
 *   A) REGION PLANA  → mfs_embedded_flash_t  (read/prog/erase + geometria).
 *      La usan los medios "tipo NOR": flash interna, NOR externa OSPI y SPI.
 *      El port delega en mfs_embedded_mount()/mfs_embedded_format().
 *
 *   B) DRIVER L2     → mfs_l2_driver + mfs_media_geom.
 *      La usan los medios que YA aportan un driver de dispositivo:
 *      mfs_l2_8bit (FRAM/EEPROM) y mfs_l2_managed (SD/eMMC). El port construye
 *      la mfs_config con mfs_embedded_bind() y llama a mf_init()/mf_format().
 *
 * Todo el estado es ESTATICO (sin heap): cada backend guarda su descriptor en
 * una estructura de fichero y el port mantiene el puntero mientras el volumen
 * este montado.
 */
#ifndef MFS_STM32_BACKEND_H
#define MFS_STM32_BACKEND_H

#include "matrixfs_stm32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Via A: region plana ==== */

/* Flash interna del MCU. Valida la region reservada contra el mapa de sectores
 * de la familia, calcula la ventana uniforme, elige `erase_unit`, implementa
 * programacion con RMW respetando la unidad del dispositivo y la ECC, y borra
 * el tramo completo [addr, addr+erase_unit). */
mfs_st mfs_stm32_iflash_prepare(const mfs_stm32_cfg *cfg,
                                mfs_embedded_flash_t *flash);

/* NOR externa sobre OSPI/QUADSPI. Autodetecta geometria por JEDEC RDID si
 * `cfg->total_size == 0`; programa con troceado en la frontera de pagina. */
mfs_st mfs_stm32_ospi_prepare(const mfs_stm32_cfg *cfg,
                              mfs_embedded_flash_t *flash);

/* ==== Via B: driver L2 ==== */

/* NOR externa SPI, FRAM y EEPROM (SPI o I2C) sobre platform/common/mfs_l2_8bit.
 * `media` debe ser MFS_STM32_MEDIA_SPI_NOR, MFS_STM32_MEDIA_FRAM o
 * MFS_STM32_MEDIA_EEPROM: cada uno tiene geometria, pagina de escritura y
 * semantica de borrado distintas (la EEPROM tiene *page write*; la FRAM no). */
mfs_st mfs_stm32_mem_prepare(const mfs_stm32_cfg *cfg, mfs_stm32_media_t media,
                             const mfs_l2_driver **out_l2,
                             const mfs_media_geom **out_geom);

/* SD / eMMC sobre platform/common/mfs_l2_managed (el dispositivo trae su FTL:
 * el nucleo no lo duplica). */
mfs_st mfs_stm32_sd_prepare(const mfs_stm32_cfg *cfg,
                            const mfs_l2_driver **out_l2,
                            const mfs_media_geom **out_geom);

/* Libera el descriptor del backend activo (no toca el medio). */
void mfs_stm32_backend_close(void);

#ifdef __cplusplus
}
#endif
#endif /* MFS_STM32_BACKEND_H */
