/* idf_shim.h — superficie del shim de ESP-IDF para las pruebas de host.
 *
 * Declara:
 *   1) las funciones internas del shim (reloj, log, FreeRTOS, flash);
 *   2) los ayudantes de DESPACHO del VFS, que imitan lo que hace newlib/IDF:
 *      localizan el punto de montaje registrado por
 *      matrixfs_esp_vfs_register() y llaman al callback del componente con la
 *      ruta YA SIN ese prefijo (igual que esp_vfs_register_fd_range en IDF).
 *
 * Tambien declara una copia de contabilidad para el test: granularidad de
 * programacion observada en esp_partition_write, que es la evidencia de que el
 * componente alinea sus escrituras a 16 B (bloque de cifrado) y no a 4 B.
 */
#ifndef MATRIXFS_TEST_IDF_SHIM_H
#define MATRIXFS_TEST_IDF_SHIM_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Contabilidad de escrituras de la particion simulada ==== */

/* Rellena el medio con 0xFF, apaga el cifrado y limpia las estadisticas (lo
 * aporta esp_partition_fake.c). */
void shim_partition_reset(void);
uint8_t *shim_partition_raw(void);

/* Menor granularidad de alineacion observada en las escrituras aceptadas:
 * 16 = todas alineadas a 16 B, 4 = hubo alguna alineada a 4 pero no a 16,
 * 1 = hubo alguna desalineada. UINT32_MAX si no se observo ninguna. */
uint32_t shim_partition_write_min_gran(void);
/* Escrituras aceptadas desde el ultimo reset de estadisticas. */
uint32_t shim_partition_write_count(void);
/* Escrituras rechazadas por la restriccion de alineacion del cifrado. */
uint32_t shim_partition_align_rejects(void);
/* Pone a cero el contador de escrituras y deja la granularidad observada al
 * maximo (sin datos). */
void shim_partition_reset_stats(void);

/* ==== Despacho POSIX tipo IDF (busca el montaje y quita el prefijo) ==== */

/* Handle opaco de directorio que devuelve `opendir_p` (en el componente es su
 * propia ranura; en IDF seria un DIR* de newlib). */
typedef void *shim_dir_handle_t;

int shim_vfs_open(const char *path, int flags, int mode);
ssize_t shim_vfs_read(int fd, void *buf, size_t n);
ssize_t shim_vfs_write(int fd, const void *buf, size_t n);
off_t shim_vfs_lseek(int fd, off_t off, int whence);
int shim_vfs_close(int fd);
int shim_vfs_fstat(int fd, struct stat *st);
int shim_vfs_fsync(int fd);
int shim_vfs_stat(const char *path, struct stat *st);
int shim_vfs_unlink(const char *path);
int shim_vfs_rename(const char *from, const char *to);
int shim_vfs_mkdir(const char *path, mode_t mode);
DIR *shim_vfs_opendir(const char *path);
struct dirent *shim_vfs_readdir(DIR *d);
int shim_vfs_closedir(DIR *d);
/* true si `path` cae bajo el punto de montaje registrado actualmente. */
bool shim_vfs_path_is_mounted(const char *path);
/* Numero de montajes VFS registrados en el shim (0 si no hay ninguno). */
int shim_vfs_mount_count(void);
/* Version de IDF simulada (1 = perfil 5.x, 2 = perfil 6.x). */
int shim_idf_profile(void);

/* ==== API de v6 con el tipo `esp_vfs_fs_ops_t` ====
 * Solo se declara en el perfil v6; en v5.5 el tipo no existe. Los prototipos
 * reales (con `esp_vfs_fs_ops_t`) se declaran en idf_shim.c, despues de
 * esp_vfs.h, que es quien define el tipo. */

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_IDF_SHIM_H */
