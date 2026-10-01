/* vfram.h — simulador del tier T0 byte-addressable para host (§11.11, §27)
 *
 * Modela FRAM/MRAM/EEPROM: no hay borrado por bloque ni regla 1→0; cada byte
 * se reprograma in-place y la escritura sólo está limitada por el número de
 * ciclos. Se usa como segundo medio del tiering heterogéneo (HMT) en los
 * tests, con el MISMO contrato de driver L2 que vFlash.
 */
#ifndef MATRIXFS_SIM_VFRAM_H
#define MATRIXFS_SIM_VFRAM_H

#include "matrixfs/matrixfs.h"

typedef struct {
  uint8_t *img;  /* imagen persistente (byte-addressable)        */
  uint32_t size; /* bytes totales                                */
  bool crashed;  /* true ⇒ toda op devuelve MFS_EIO              */
  /* telemetría */
  uint32_t n_read, n_prog;
  uint32_t n_writes[256]; /* escrituras por bloque de 256 B (desgaste)  */
  mfs_l2_driver drv;      /* driver L2 asociado (ctx = este)              */
} vfram_t;

/* Inicializa con imagen de `size` bytes a 0xFF (estado virgen). */
bool vfram_init(vfram_t *fr, uint32_t size);
void vfram_free(vfram_t *fr);

/* Rellena mfs_media_geom para un medio byte-addressable (FRAM). */
void vfram_geom(const vfram_t *fr, mfs_media_geom *g);

/* Driver L2 del tier T0 (ctx = vfram_t*). No tiene `erase`. */
const mfs_l2_driver *vfram_driver(vfram_t *fr);

/* Corte/re-alimentación del tier (persistencia entre montajes). */
void vfram_crash(vfram_t *fr);
void vfram_recover(vfram_t *fr);

/* Acceso directo para asserts de bajo nivel. */
uint8_t *vfram_raw(vfram_t *fr, uint32_t addr);

#endif /* MATRIXFS_SIM_VFRAM_H */
