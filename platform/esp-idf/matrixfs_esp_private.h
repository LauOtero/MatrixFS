/* matrixfs_esp_private.h — superficie interna del componente ESP-IDF.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * NO forma parte de la API publica: solo la usan las unidades de traduccion del
 * propio componente (matrixfs_esp.c, matrixfs_esp_vfs.c).
 */
#ifndef MATRIXFS_ESP_PRIVATE_H
#define MATRIXFS_ESP_PRIVATE_H

#include <stdbool.h>

#include "matrixfs/matrixfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Asegura (creando si hace falta) el mutex recursivo del componente. Devuelve
 * false solo si no se pudo crear. Sin CONFIG_MATRIXFS_THREAD_SAFE devuelve
 * siempre true y no hay exclusion mutua. */
bool matrixfs_esp_mutex_ensure(void);

/* Traduce un error tipificado del nucleo al `errno` de newlib. Lo usan el VFS
 * y los wrappers POSIX. Nunca devuelve 0. */
int matrixfs_esp_errno(mfs_st st);

/* Volumen de trabajo del componente. Es el que declara la aplicacion en
 * matrixfs_esp_mount(); el VFS necesita saber si hay volumen montado. */
bool matrixfs_esp_is_mounted(void);

/* Instancia del nucleo del volumen montado, o NULL. La propiedad es del
 * llamador (el componente solo guarda el puntero para el VFS). */
mf_t *matrixfs_esp_fs(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_ESP_PRIVATE_H */
