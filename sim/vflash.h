/* vflash.h — simulador NOR SPI para host (§20.2 driver L2, tests §27)
 *
 * Modelo físico verificado:
 *   - estado borrado = 0xFF por byte;
 *   - prog solo puede aclarar bits (1→0); intentar escribir sobre un 0 sin
 *     erase previo ⇒ MFS_EIO (violación de protocolo, como en NOR real);
 *   - erase aclara todo un bloque (erase_unit) alineado;
 *   - post-program read-back verification (barrera WOB §20.3): si el buffer
 *     no coincide tras prog ⇒ MFS_ECORRUPT y el bloque pasa a cuarentena
 *     (stub: se reporta el error);
 *   - contadores de P/E por bloque para HCT/desgaste;
 *   - "power loss": vf_crash() congela la imagen; las escrituras posteriores
 *     fallan hasta vf_recover(), que reactiva la MISMA imagen (persistencia
 *     entre montajes, como un SPI-flash real alimentado de nuevo).
 */
#ifndef MATRIXFS_SIM_VFLASH_H
#define MATRIXFS_SIM_VFLASH_H

#include "matrixfs/matrixfs.h"

#define VF_SECTOR   4096u                  /* unidad de erase mínima NOR (§25) */
#define VF_MAX_BLOCKS 256u                 /* hasta 1 MiB de medio simulado    */

typedef struct {
    uint8_t  *img;                 /* imagen persistente del medio        */
    uint32_t  size;                /* bytes totales                       */
    uint32_t  erase_unit;          /* bytes por bloque                    */
    bool      crashed;             /* true ⇒ toda op devuelve MFS_EIO     */
    /* telemetría para asserts de test */
    uint32_t  n_read, n_prog, n_erase, n_violations;
    uint16_t  pe_max;              /* máximos ciclos P/E de un bloque     */
    uint16_t  pe[VF_MAX_BLOCKS];   /* contador por bloque                 */
} vflash_t;

/* Inicializa con imagen de `size` bytes (erased 0xFF). `size` múltiplo de
 * VF_SECTOR. Devuelve false si falla la asignación. */
bool vf_init(vflash_t *vf, uint32_t size, uint32_t erase_unit);
void vf_free(vflash_t *vf);

/* Rellena mfs_media_geom para NOR SPI con esta geometría. */
void vf_geom(const vflash_t *vf, mfs_media_geom *g);

/* Driver L2 conectado al dispositivo (ctx = vflash_t*). */
const mfs_l2_driver *vf_driver(vflash_t *vf);

/* Eventos de energía (§12.3 hooks de test): */
void vf_crash(vflash_t *vf);   /* corte: escrituras/lecturas fallan       */
void vf_recover(vflash_t *vf); /* re-alimentación: imagen persiste        */

/* Inyección selectiva de fallo en la próxima operación (1 uso): */
void vf_fail_next_prog(vflash_t *vf, bool on);

/* Acceso directo para asserts de bajo nivel (post-mortem forense): */
uint8_t *vf_raw(vflash_t *vf, uint32_t addr);

#endif /* MATRIXFS_SIM_VFLASH_H */
