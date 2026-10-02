/* matrixfs_esp_vfs.h — registro POSIX (VFS) del volumen MatrixFS en ESP-IDF.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Permite usar la API POSIX de newlib (fopen/fread/fwrite/stat/opendir/...)
 * sobre un volumen MatrixFS ya montado, igual que hacen SPIFFS y FAT con
 * esp_vfs_register(). El VFS NO monta el volumen: hay que llamar antes a
 * matrixfs_esp_mount() (o matrixfs_esp_mount_default()).
 *
 * Compilacion: este fichero solo entra en el build con
 * CONFIG_MATRIXFS_VFS_ENABLE=y.
 *
 * ADVERTENCIA DE VERSION: `esp_vfs_t` cambio de nombres de campo entre IDF v4
 * (open/read/write) y v5 (open_p/read_p/write_p, con ESP_VFS_FLAG_DEFAULT).
 * Esta implementacion usa la forma de IDF v5.x, que es la que exige
 * idf_component.yml. Si se compila contra otra version, ajustar SOLO
 * matrixfs_esp_vfs.c.
 */
#ifndef MATRIXFS_ESP_VFS_H
#define MATRIXFS_ESP_VFS_H

#include "matrixfs/matrixfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registra el volumen montado en el VFS bajo CONFIG_MATRIXFS_VFS_MOUNT_POINT.
 *
 * Devuelve MFS_OK, MFS_ENOTMOUNTED si no hay volumen montado, MFS_EBUSY si ya
 * estaba registrado, o MFS_EIO si el registro en el VFS falla.
 * Idempotente en el sentido de que un segundo registro devuelve MFS_EBUSY.
 */
mfs_st matrixfs_esp_vfs_register(void);

/* Desregistra del VFS y libera la tabla de descriptores. Cierra los ficheros
 * que quedaran abiertos. NO desmonta el volumen (usar mf_deinit() para eso).
 */
mfs_st matrixfs_esp_vfs_unregister(void);

/* Punto de montaje efectivo (nunca NULL). */
const char *matrixfs_esp_vfs_mount_point(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_ESP_VFS_H */
