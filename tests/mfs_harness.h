/* mfs_harness.h — utilidades de test sobre vFlash/vFRAM (blanco, incluye
 * interno) */
#ifndef MFS_HARNESS_H
#define MFS_HARNESS_H

#include "mfs_internal.h"
#include "vflash.h"
#include "vfram.h"

#define MFS_TEST_SIZE (1u << 20) /* 1 MiB, 256 sectores de 4 KB */
#define MFS_TEST_SECTOR 4096u
#define MFS_TEST_T0_SIZE (64u * 1024u)

typedef struct {
  vflash_t vf;
  mfs_media_geom g;
  vfram_t fr;
  mfs_media_geom g_t0;
  bool has_t0;
  mfs_config cfg;
  mf_t fs;
  bool formatted;
} mfs_env_t;

/* Crea el medio T1 y la configuración (sin montar). */
bool env_open(mfs_env_t *e, uint32_t ram_total, const uint8_t *key);
/* Igual, añadiendo un tier T0 byte-addressable (FRAM) de `t0_size` bytes. */
bool env_open_t0(mfs_env_t *e, uint32_t ram_total, const uint8_t *key,
                 uint32_t t0_size);
/* Formatea y monta. */
int env_format(mfs_env_t *e);
/* Desmonta y vuelve a montar (persistencia). */
int env_remount(mfs_env_t *e);
/* Desmonta y libera los medios. */
void env_close(mfs_env_t *e);
/* Marca capacidades de plataforma en el HWV (blanco: simula assets del HAL). */
void env_set_assets(mfs_env_t *e, uint8_t flags2, uint8_t flags3);

#endif /* MFS_HARNESS_H */
