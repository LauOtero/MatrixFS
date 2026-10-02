/* matrixfs_esp.h — API publica del componente ESP-IDF para MatrixFS.
 *
 * Copyright 2026 MatrixFS contributors
 *
 * Licencia Apache, Version 2.0 (la "Licencia");
 * no puede usar este fichero salvo en cumplimiento de la Licencia.
 * Puede obtener una copia en:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Salvo que la ley aplicable exija lo contrario o se acuerde por escrito, el
 * software distribuido bajo la Licencia se distribuye "TAL CUAL", SIN GARANTIAS
 * NI CONDICIONES DE NINGUN TIPO, ni expresas ni implicitas. Consulte la
 * Licencia para conocer el permiso y las limitaciones especificas.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Este componente enlaza el nucleo de MatrixFS (sin dependencias de plataforma)
 * con el SDK de ESP-IDF:
 *   - aporta las primitivas obligatorias del puerto (mfs_port_*, ver el .c);
 *   - traduce los callbacks read/prog/erase a la API esp_partition;
 *   - monta/formatea el volumen usando la capa embebida compartida;
 *   - opcionalmente lo registra en el VFS (matrixfs_esp_vfs.c).
 *
 * La instancia del nucleo (`mf_t`) es OPACA en include/matrixfs: para declarar
 * una instancia incluye "mfs_internal.h" (directorio src/) tal y como hace
 * platform/common/mfs_vfs.c. El componente expone src/ en sus include dirs.
 *
 * NOTA: el nucleo de MatrixFS es de instancia unica (tablas estaticas, sin
 * heap). Este componente aloja el descriptor de flash en estado estatico, por
 * lo que solo puede haber un volumen MatrixFS montado a la vez.
 *
 * CONCURRENCIA. El nucleo NO es reentrante. Con CONFIG_MATRIXFS_THREAD_SAFE
 * (por defecto) el componente serializa el montaje/formateo y la capa VFS, pero
 * las llamadas DIRECTAS al nucleo (mf_open/mf_read/mf_write/...) no pasan por
 * aqui: si el volumen se usa desde mas de una tarea, la aplicacion debe
 * agruparlas con matrixfs_esp_lock()/matrixfs_esp_unlock(). Ver README.
 */
#ifndef MATRIXFS_ESP_H
#define MATRIXFS_ESP_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs/matrixfs.h"
#include "mfs_embedded.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monta el volumen MatrixFS alojado en la particion `partition_label`.
 *
 * - `partition_label`: etiqueta de la particion de tipo data; si es NULL o
 *   cadena vacia se usa CONFIG_MATRIXFS_PARTITION_LABEL.
 * - `out_fs`: instancia del nucleo aportada por el llamador; debe sobrevivir
 *   mientras el volumen permanezca montado.
 * - `opts`: opciones de integracion. Si es NULL se usan los valores por
 *   defecto de mfs_embedded_opts_default() (sin formateo automatico).
 *
 * Devuelve MFS_OK o un error tipificado (mfs_st). Con `opts->format_if_needed`
 * y un volumen ausente o corrupto, formatea y reintenta una vez.
 */
mfs_st matrixfs_esp_mount(const char *partition_label, mf_t *out_fs,
                          const mfs_embedded_opts *opts);

/* Formatea el volumen MatrixFS de la particion `partition_label`.
 *
 * Destruye el contenido previo (superblocks, cabeceras y zonas). `fs` es la
 * instancia del nucleo aportada por el llamador (queda SIN montar). El resto de
 * parametros siguen la misma convencion que matrixfs_esp_mount().
 */
mfs_st matrixfs_esp_format(const char *partition_label, mf_t *fs,
                           const mfs_embedded_opts *opts);

/* Monta el volumen usando la configuracion compilada (Kconfig):
 *   CONFIG_MATRIXFS_PARTITION_LABEL, CONFIG_MATRIXFS_RAM_BUDGET,
 *   CONFIG_MATRIXFS_FORMAT_IF_NEEDED, los modos MATRIXFS_FORCE_MODE_* y, si
 *   esta habilitado, la clave CONFIG_MATRIXFS_CRYPTO_KEY_HEX.
 */
mfs_st matrixfs_esp_mount_default(mf_t *out_fs);

/* Ultimo mensaje de error legible del componente (nunca NULL). Valido hasta la
 * siguiente llamada al componente desde la misma tarea. Con el mutex activo, el
 * buffer es por tarea; sin el, no es thread-safe.
 */
const char *matrixfs_esp_last_error(void);

/* ==== Diagnostico de capacidad ==== */

/* Capacidad util REAL del volumen montado, en bytes. No es el tamano de la
 * particion: el nucleo direcciona como maximo MFS_ZONE_MAX zonas de
 * `erase_unit` bytes, de modo que una particion mayor NO se aprovecha entera.
 * Devuelve 0 si no hay volumen preparado. */
uint32_t matrixfs_esp_capacity_bytes(void);

/* Devuelve true si la particion es mayor que la capacidad util y por tanto hay
 * espacio que no se aprovechara (el montaje lo avisa por ESP_LOGW). */
bool matrixfs_esp_capacity_wasteful(void);

/* ==== Serializacion (CONFIG_MATRIXFS_THREAD_SAFE) ==== */

/* Toma/suelta el mutex recursivo del componente. Necesario para agrupar varias
 * llamadas DIRECTAS al nucleo (p. ej. mf_stat + mf_open) cuando el volumen se
 * usa desde mas de una tarea. Son recursivos: se pueden anidar.
 *
 * Sin CONFIG_MATRIXFS_THREAD_SAFE son no-op: la aplicacion es responsable de
 * la exclusion mutua. */
void matrixfs_esp_lock(void);
void matrixfs_esp_unlock(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_ESP_H */
