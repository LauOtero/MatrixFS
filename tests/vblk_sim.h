/* vblk_sim.h — simulador host de un dispositivo de bloques GESTIONADO.
 *
 * Modela un medio con FTL propio (SD/eMMC/UFS/SATA/NVMe): la unidad de
 * direccionamiento es el sector (512 B o 4 KB), la escritura es de sector
 * completo (no hay "program" byte a byte ni estado borrado 0xFF), y opcional-
 * mente expone TRIM/UNMAP. Sirve para validar el adaptador
 * `platform/common/mfs_l2_managed.c` en la suite de host (§27).
 */
#ifndef MFS_SIM_VBLK_H
#define MFS_SIM_VBLK_H

#include <stdbool.h>
#include <stdint.h>

#include "mfs_l2_managed.h"

typedef struct {
  uint8_t *img;        /* imagen persistente del dispositivo (host: malloc) */
  uint32_t sectors;    /* capacidad en sectores                           */
  uint16_t sector_size;
  bool crashed;        /* true ⇒ toda operación devuelve MFS_EIO         */
  bool fail_next_write;/* inyección de fallo de un solo uso               */
  /* telemetría para los asserts del test */
  uint32_t n_read, n_write, n_trim;
  uint32_t trim_sectors;
} vblk_t;

/* Crea el dispositivo (imagen a 0xFF, como un medio nuevo). */
bool vblk_init(vblk_t *b, uint32_t sectors, uint32_t sector_size);
void vblk_free(vblk_t *b);

/* Rellena la API de dispositivo para `mfs_managed_l2_init()` (el llamante
 * asigna `bounce`/`bounce_len` después). */
void vblk_attach(vblk_t *b, mfs_managed_dev_t *dev);

#endif /* MFS_SIM_VBLK_H */
