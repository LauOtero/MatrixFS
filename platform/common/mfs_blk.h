/* mfs_blk.h — driver L2 sobre un medio de bloques real o una imagen.
 *
 * Abstrae el acceso al medio (fichero imagen, /dev/sdX, /dev/mtdN,
 * /dev/nvmeXnY,
 * \\.\PhysicalDriveN, \\.\X:) aplicando la semántica NOR/NAND de MatrixFS:
 *   - `prog`  sólo aclara bits (1→0) en NOR; en medios con FTL (SD/eMMC/USB) el
 *             propio dispositivo lo garantiza.
 *   - `erase` devuelve el bloque a 0xFF (o usa MEMERASE si es un MTD en Linux).
 *
 * Limitación conocida: el núcleo direcciona con 32 bits, por lo que sólo se
 * exponen los primeros 4 GiB del medio (ver DOCS/known-limitations.md).
 */
#ifndef MFS_BLK_H
#define MFS_BLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matrixfs/mfs_port.h"

/* Cota de la ruta del dispositivo (compartida con la capa VFS, que copia el
 * nombre del medio para mostrarlo en /proc/mounts y en el Explorador). */
#define MFS_BLK_PATH_MAX 512u

typedef struct mfs_blk mfs_blk;

typedef struct {
  mfs_media_type_t media;       /* por defecto MFS_MEDIA_NOR_SPI */
  uint32_t erase_unit;          /* 0 ⇒ autodetectar, si no 4096 B */
  uint32_t program_granularity; /* 0 ⇒ 1 (NOR programable por byte) */
  uint16_t page_size;           /* 0 ⇒ 256 B */
  uint16_t oob_bytes;           /* 0 ⇒ ninguno */
  uint32_t size_limit;          /* 0 ⇒ todo el medio (máx. 4 GiB) */
} mfs_blk_opts;

void mfs_blk_opts_default(mfs_blk_opts *o);

/* Abre el medio. `readonly` prohíbe prog/erase (montaje de sólo lectura).
 * Devuelve NULL y un mensaje en `err` si falla. */
mfs_blk *mfs_blk_open(const char *path, bool readonly, const mfs_blk_opts *opts,
                      char *err, size_t errlen);
void mfs_blk_close(mfs_blk *b);

/* Driver L2 para registrar en mfs_config.drv (válido mientras viva `b`). */
const mfs_l2_driver *mfs_blk_driver(mfs_blk *b);
/* Geometría efectiva (tras autodetección y límites). */
void mfs_blk_geom(const mfs_blk *b, mfs_media_geom *g);
/* Vuelca las escrituras pendientes a la capa física (fsync/FlushFileBuffers).
 */
int mfs_blk_flush(mfs_blk *b);
/* Borrado de bajo nivel de todo el medio (0xFF); usado antes de formatear. */
int mfs_blk_erase_all(mfs_blk *b);

uint64_t mfs_blk_size(const mfs_blk *b);
bool mfs_blk_is_readonly(const mfs_blk *b);
const char *mfs_blk_path(const mfs_blk *b);

#endif /* MFS_BLK_H */
