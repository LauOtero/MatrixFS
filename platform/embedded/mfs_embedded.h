/* mfs_embedded.h — capa de integración embebida (32-bit MCU) para MatrixFS.
 *
 * Punto único de acoplamiento entre el núcleo (sin dependencias de plataforma)
 * y los SDK de MCU. La usan los envoltorios de:
 *   · Arduino (ESP32, ESP8266, RP2040…)   → platform/arduino/
 *   · ESP-IDF (esp_partition)             → platform/esp-idf/
 *   · PlatformIO (proyecto)               → platform/platformio/
 *   · MicroPython (bdev)                  → platform/micropython/
 *
 * El BSP/SDK sólo aporta tres primitivas sobre una región de flash plana
 * (read/prog/erase) con la semántica NOR de MatrixFS (§2 P4, §20.3): el núcleo
 * nunca habla directamente con el controlador.
 */
#ifndef MFS_EMBEDDED_H
#define MFS_EMBEDDED_H

#include "matrixfs/matrixfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Región de flash y callbacks del SDK ==== */
typedef struct mfs_embedded_flash {
  /* Callbacks del SDK. `addr` es ABSOLUTO en el mapa del MCU. */
  mfs_st (*read)(void *ctx, uint32_t addr, void *dst, uint32_t len);
  /* prog debe retornar SÓLO tras verificar el estado (barrera WOB, §20.3). */
  mfs_st (*prog)(void *ctx, uint32_t addr, const void *src, uint32_t len);
  /* erase borra el bloque alineado que contiene `addr` (0 si no aplica). */
  mfs_st (*erase)(void *ctx, uint32_t addr);
  void *ctx;

  /* Geometría de la región (partición o rango reservado). */
  uint32_t base_addr;  /* dirección física inicial (p. ej. 0x100000)   */
  uint32_t size;       /* bytes totales de la región                    */
  uint32_t erase_unit; /* bloque borrable mínimo (NOR: 4096)            */
  uint16_t page_size;  /* página de programación (NOR: 256)             */
  uint32_t t_prog_max_us;
  uint32_t t_erase_max_us;
  uint32_t t_read_max_us;
  bool no_erase; /* true en FRAM/MRAM/EEPROM (sin borrado por bloque) */

  /* Rellenados por mfs_embedded_setup(); no los edite a mano. */
  mfs_l2_driver drv;
  mfs_media_geom geom;
  mfs_config cfg;
  bool ready;
} mfs_embedded_flash_t;

/* ==== Opciones de integración ==== */
typedef struct {
  uint32_t ram_total;          /* RAM real reservada al FS (bytes)      */
  uint8_t arch_class;          /* 1=16-bit, 2=32-bit (defecto 2)        */
  mfs_mode_t forced_mode;      /* MFS_MODE_UNSUPPORTED = automático     */
  const uint8_t *key;          /* 32 B o NULL (sin cifrado)             */
  uint8_t suite_preferred;     /* mfs_suite_t o 0xFF = negociar         */
  uint32_t bus_speed_hz;       /* 0 ⇒ no declarar (no se mide en MCU)   */
  uint32_t uid, gid;           /* propietario de nodos nuevos           */
  uint16_t file_perm, dir_perm;/* permisos por defecto                  */
  bool allow_convergent, dedup_enable, cdc_enable, zrp_enable, dab_enable;
  bool format_if_needed;       /* formatea si no hay volumen válido     */
} mfs_embedded_opts;

void mfs_embedded_opts_default(mfs_embedded_opts *o);

/* Rellena drv+geom+cfg a partir de la región de flash y las opciones.
 * Idempotente; no toca el medio. Devuelve MFS_OK o error tipificado. */
mfs_st mfs_embedded_setup(mfs_embedded_flash_t *flash,
                          const mfs_embedded_opts *o);

/* Monta el volumen (mf_init). Si `o->format_if_needed` y el volumen no es
 * válido, formatea y reintenta una vez. `fs` debe sobrevivir al montaje. */
mfs_st mfs_embedded_mount(mf_t *fs, mfs_embedded_flash_t *flash,
                          const mfs_embedded_opts *o);

/* Formatea el volumen (borra SB/HWV/zonas y crea la raíz). */
mfs_st mfs_embedded_format(mf_t *fs, mfs_embedded_flash_t *flash,
                           const mfs_embedded_opts *o);

#ifdef __cplusplus
}
#endif
#endif /* MFS_EMBEDDED_H */
