/* stubs_backends.c — stubs SOLO DE ENLACE para los backends que no se prueban.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * `matrixfs_stm32.c` despacha por medio y por tanto REFERENCIA las cuatro
 * funciones `mfs_stm32_*_prepare()`. Este test solo ejerce la de la flash
 * interna (el backend mas delicado: RMW + ECC + ventana uniforme), asi que las
 * otras tres se aportan como stubs que devuelven MFS_ENOTSUP.
 *
 * NO se toca `mfs_stm32_iflash_prepare()`: es justo lo que se esta probando.
 *
 * `mfs_stm32_ospi.c`, `mfs_stm32_mem.c` y `mfs_stm32_sd.c` NO se compilan en
 * este binario: sus dependencias del HAL externo (OSPI/QSPI/SPI/I2C/SD,
 * decenas de constantes) son otro modelo entero; se prueban con shims propios
 * (estilo ESP-IDF) o no se prueban aqui. Los typedefs de esos perifericos
 * existen en el shim para que el port COMPILE si alguna vez se anaden a la
 * linea de comandos.
 */
#include "mfs_stm32_backend.h"

mfs_st mfs_stm32_ospi_prepare(const mfs_stm32_cfg *cfg,
                              mfs_embedded_flash_t *flash) {
  (void)cfg;
  (void)flash;
  return MFS_ENOTSUP;
}

mfs_st mfs_stm32_mem_prepare(const mfs_stm32_cfg *cfg, mfs_stm32_media_t media,
                             const mfs_l2_driver **out_l2,
                             const mfs_media_geom **out_geom) {
  (void)cfg;
  (void)media;
  (void)out_l2;
  (void)out_geom;
  return MFS_ENOTSUP;
}

mfs_st mfs_stm32_sd_prepare(const mfs_stm32_cfg *cfg,
                            const mfs_l2_driver **out_l2,
                            const mfs_media_geom **out_geom) {
  (void)cfg;
  (void)out_l2;
  (void)out_geom;
  return MFS_ENOTSUP;
}
