/* mfs_port.h — MatrixFS «ATLAS» v1.0 — contrato de puerto (§20.2/§20.3)
 *
 * Todo integrador DEBE implementar las funciones obligatorias. Las opcionales
 * se registran en mfs_config; si faltan, aplica el fallback software con la
 * misma API (MFS-HW-001).
 */
#ifndef MATRIXFS_MFS_PORT_H
#define MATRIXFS_MFS_PORT_H

#include "mfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Obligatorio: primitivas del puerto (§20.2) ==== */
void mfs_port_crit_enter(void); /* sección crítica, latencia <= 1 µs (§20.3) */
void mfs_port_crit_exit(void);
uint32_t mfs_port_cycles(void); /* contador de ciclos libre-corriente         */
uint32_t mfs_port_time_us(void); /* tiempo monotónico en µs */
void mfs_port_wfi(void); /* idle durante cuantos ESP (§13.2)           */

/* ==== Driver L2 (registrado en mfs_config) ==== */
typedef struct mfs_l2_driver {
  mfs_st (*read)(void *ctx, uint32_t addr, void *dst, uint32_t len);
  /* prog retorna SOLO tras verificación de estado (barrera WOB, §20.3) */
  mfs_st (*prog)(void *ctx, uint32_t addr, const void *src, uint32_t len);
  mfs_st (*erase)(void *ctx, uint32_t addr); /* erase de bloque        */
  mfs_st (*suspend)(void *ctx); /* NULL si !flags0.suspend_e                 */
  mfs_st (*resume)(void *ctx);
  /* T0 (HMT, solo si hay FRAM/MRAM/EEPROM secundario): */
  mfs_st (*t0_read)(void *ctx, uint32_t addr, void *dst, uint32_t len);
  mfs_st (*t0_prog)(void *ctx, uint32_t addr, const void *src, uint32_t len);
  /* EDP (solo si adc_rail && supercap, MFS-HAL-003): */
  bool (*rail_ok)(void *ctx, mfs_rail_state *out);
  /* Opcionales: DMA/CRC HW, crypto HW (MFS-HW-001 exige fallback SW) */
  mfs_st (*dma_read)(void *ctx, uint32_t addr, void *dst, uint32_t len);
  mfs_st (*dma_crc)(void *ctx, uint32_t addr, uint32_t len, uint32_t *crc_out);
  void *ctx;
} mfs_l2_driver;

/* ==== Geometría declarada por el driver (fase 1-7 de detección §5.1) ==== */
typedef struct {
  mfs_media_type_t type;
  uint32_t base_addr;
  uint32_t size;                /* bytes totales                              */
  uint32_t erase_unit;          /* bytes por bloque eraseable                 */
  uint32_t program_granularity; /* bytes mínimos por programación             */
  uint16_t page_size;           /* página lógica                              */
  uint16_t oob_bytes;
  uint32_t t_prog_max_us; /* presupuestos de datasheet T_max (§2 P5)    */
  uint32_t t_erase_max_us;
  uint32_t t_read_max_us;
  uint32_t t_suspend_max_us;
  uint8_t flags0, flags1;  /* MFS_HWV0_* / MFS_HWV1_*                    */
  uint8_t zones_per_block; /* ZLF: zona = 1-4 bloques                    */
  uint32_t zone_size;      /* 0 => derivar (1 bloque)                    */
} mfs_media_geom;

/* ==== HWV persistido en flash (§5.2) — 64 B, serializado LE ==== */
typedef struct {
  /* magic[4] == 'M','H','W','V' */
  uint8_t magic[4];
  uint16_t hw_version;
  uint8_t arch_class;  /* 1=16-bit, 2=32-bit; 0 ⇒ rechazo MFS_EARCH */
  uint8_t mode_forced; /* modo fijado por manifiesto o 0xFF=auto     */
  uint32_t ram_total;
  uint32_t bus_speed_hz; /* min(MCU, flash) — solo lectura (§14.2)     */
  uint32_t t_prog_max_us, t_erase_max_us, t_read_max_us, t_suspend_max_us;
  uint32_t program_granularity, erase_unit;
  uint32_t base_reserved_off; /* offset tras SB A/B + HWV: inicio zona tokens */
  uint16_t oob_bytes;
  uint8_t flags0; /* suspend_e, suspend_p, multi_plane, ecc_on_die */
  uint8_t flags1; /* byte_addr, managed, ppp, slc, asym, se        */
  uint8_t flags2; /* puf, adc_rail, supercap, trustzone            */
  uint8_t flags3; /* crypto_hw, ascon_hw, b3_hw, cqe, zns, dma_crc */
  uint32_t t0_size, t0_write_ns;
  uint32_t profile_id; /* perfil firmado                                 */
  uint32_t media_type; /* extensión: mfs_media_type_t del medio T1       */
  uint32_t media_size;
  uint32_t crc; /* CRC-32C del cuerpo (sin este campo)            */
} mfs_hwv_t;

/* Handle de 8 B que reside en RAM (MFS-HWV-001: vector vive en flash) */
typedef struct {
  uint32_t hwv_flash_addr;
  uint32_t hwv_crc;
} mfs_hwv_handle_t;

/* Serialización por accesores del puerto (§20.4): formato on-flash LE */
void mfs_hwv_serialize(const mfs_hwv_t *h, uint8_t out[64]);
void mfs_hwv_deserialize(mfs_hwv_t *h, const uint8_t in[64]);
mfs_st
mfs_hwv_validate(const mfs_hwv_t *h); /* magic+crc+arch_class (§24.2 p2) */

/* ==== Accesores de serialización (§20.4) — únicos permitidos on-flash ==== */
static inline uint16_t mfs_ld16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t mfs_ld32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline uint64_t mfs_ld64(const uint8_t *p) {
  return (uint64_t)mfs_ld32(p) | ((uint64_t)mfs_ld32(p + 4) << 32);
}
static inline void mfs_st16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline void mfs_st32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static inline void mfs_st64(uint8_t *p, uint64_t v) {
  mfs_st32(p, (uint32_t)v);
  mfs_st32(p + 4, (uint32_t)(v >> 32));
}

/* ==== Configuración de instancia (§21.1 mf_init) ==== */
typedef struct {
  const mfs_l2_driver *drv; /* driver L2 principal (T1)              */
  const mfs_media_geom
      *geom; /* geometría declarada; NULL ⇒ autodetección vía port_ops */
  const mfs_l2_driver *drv_t0; /* HMT: medio T0 (FRAM/MRAM/EEPROM)      */
  const mfs_media_geom *geom_t0;
  /* Presupuesto de viabilidad (§6.1): valores ABSOLUTOS en bytes.
   * Si son 0, se usan los porcentajes default del estándar. */
  uint32_t ram_total;        /* RAM física real del MCU               */
  uint32_t ram_firmware_min; /* default: 50 % de ram_total            */
  uint32_t ram_stack_min;    /* default: 15 %                         */
  uint32_t ram_peripherals;  /* default: 8 %                          */
  uint32_t ram_margin;       /* default: 10 % (mínimo normativo)      */
  mfs_mode_t forced_mode;    /* MFS_MODE_UNSUPPORTED (0xFF) ⇒ auto    */
  uint8_t arch_class;        /* 1=16b, 2=32b; 0 ⇒ MFS_EARCH (§24.2)   */
  uint8_t suite_preferred;   /* mfs_suite_t; 0xFF = negociar (§10.6)  */
  uint32_t bus_speed_hz;     /* 0 ⇒ medir read-only (MFS-BUS-001)     */
  uint32_t profile_id;
  const uint8_t *key;    /* 32 B; NULL ⇒ sin cifrado (S_NONE)     */
  bool rt_strict;        /* §13.4                                 */
  bool allow_convergent; /* MFS_OPT_CONVERGENT (§10.3)            */
  bool zrp_enable;       /* §8.5, off por defecto                 */
  bool dedup_enable;     /* CFX, Balanced+                        */
  bool cdc_enable;       /* Gear CDC, Balanced+                   */
  bool dab_enable;       /* autómata EXP3 (§24.4)                 */
  uint32_t dab_seed;     /* replay-determinista (P12)             */
  /* Propietario y permisos POSIX por defecto de los nodos nuevos (§21.2) */
  uint32_t default_uid;
  uint32_t default_gid;
  uint16_t default_file_perm; /* p. ej. 0644 */
  uint16_t default_dir_perm;  /* p. ej. 0755 */
  /* Overlays opcionales aportados por el integrador (RSC); NULL ⇒ internos */
  void *ovl_a, *ovl_b, *ovl_c, *ovl_d;
  uint32_t ovl_sizes[4];
} mfs_config;

/* Operaciones de autodetección HAL (cascada §5.1) delegadas al BSP.
 * Opcional: si cfg->geom está presente se usa directamente. */
typedef struct mfs_hal_ops {
  /* fase 1-3: JEDEC 0x9F / SFDP 0x5A / CFI 0x98 */
  mfs_st (*jedec_id)(uint8_t out[4]);
  mfs_st (*sfdp_parse)(mfs_media_geom *g);
  /* fase 8: assets de plataforma */
  mfs_st (*assets)(uint8_t *flags2, uint8_t *flags3);
  /* fase 9: prueba activa limitada (bloque defectuoso) */
  mfs_st (*bad_block_probe)(uint32_t blk, bool *bad);
} mfs_hal_ops;

void mfs_set_hal_ops(const mfs_hal_ops *ops);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_MFS_PORT_H */
