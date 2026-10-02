/* mfs_stm32_backend.c — cierre del backend activo.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mfs_stm32_backend.h"

/* Los descriptores de medio del port son ESTATICOS (sin heap) y cada
 * `*_prepare()` los reinicializa por completo:
 *   · iflash hace memset de su estado y recalcula la ventana uniforme;
 *   · mem vuelve a crear el driver de mfs_l2_8bit (que hace memset);
 *   · sd reinicializa el adaptador gestionado.
 * Por tanto no hay memoria que liberar. Lo unico que podria quedar "abierto" es
 * el periferico del HAL (SPI/I2C/OSPI/SD), y su propiedad es del BSP que lo
 * configuro con CubeMX: apagarlo aqui romperia a otros usuarios del mismo
 * periferico.
 *
 * La funcion existe para que el ciclo de vida del port (mount → deinit) sea
 * simetrico y para tener un punto unico donde anadir limpieza si en el futuro
 * algun backend reserva algo. */
void mfs_stm32_backend_close(void) {
  /* Intencionadamente vacio: ver el comentario de arriba. */
}
