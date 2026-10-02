/* mfs_internal.h — estructuras internas del núcleo (no forman API pública) */
#ifndef MATRIXFS_MFS_INTERNAL_H
#define MATRIXFS_MFS_INTERNAL_H

#include "matrixfs/matrixfs.h"

/* Valor "ausente" del mapa L2P y centinelas de `mfs_extent_read`. El ppage 0 es
 * un destino VÁLIDO (zona 0, página 0): usarlo como "ausente" hacía que esas
 * páginas se leyeran como huecos (ceros). */
#define MFS_L2P_FREE 0xFFFFFFFFu
#define MFS_PPAGE_NONE MFS_L2P_FREE /* hueco: nunca escrita          */
#define MFS_PPAGE_ERROR 0xFFFFFFFEu /* viva pero ilegible (integridad) */

/* ==== Layout on-flash (§22) ==== */
#define MFS_SB_SIZE 256u    /* superblock A/B (§22.1) */
#define MFS_HWV_OFFSET 512u /* HWV @512 fijo (§8.1)   */
#define MFS_HWV_SIZE 64u
#define MFS_ZONEHDR_SIZE 64u /* cabecera de zona (§22.2) */
/* Región baja (alineada a sectores para respetar la semántica NOR: borrar
 * antes de reprogramar). Cada estructura crítica en su propio sector:
 *   sector 0            : SB A (@0) + HWV (@512)
 *   sector 1            : SB B (@erase_unit)
 *   sector 2            : anillo de tokens T1 (@2·erase_unit)
 *   sector 3..          : zonas ZLF
 * SB A y SB B en sectores distintos ⇒ el borrado/reprogramación de uno no
 * invalida al otro (crash-only §9.1). */
static inline uint32_t mfs_sb_a_off(void) { return 0u; }
static inline uint32_t mfs_sb_b_off(const mfs_hwv_t *h) {
  return h->erase_unit;
}
static inline uint32_t mfs_tok_off(const mfs_hwv_t *h) {
  return 2u * h->erase_unit;
}
static inline uint32_t mfs_zone_base(const mfs_hwv_t *h) {
  return 3u * h->erase_unit;
}
#define MFS_INO_S_SIZE 32u  /* INO-S UN/Nano (§22.3)    */
#define MFS_INO_L_SIZE 64u  /* INO-L Compact+           */
#define MFS_EXTENT_SIZE 12u /* registro de extent (§22.4) */
#define MFS_E2G_NOR_HDR 20u /* cabecera de registro NOR (§8.2) */
#define MFS_E2G_NAND_HDR 32u
#define MFS_TOKEN_T1_SIZE 32u /* token commit T1 (§9.2)   */
#define MFS_TOKEN_T0_SIZE 16u /* token commit T0          */
#define MFS_TOKEN_SLOTS 16u   /* anillo A/B con contador termométrico */

#define MFS_SB_MAGIC0 'M'
#define MFS_SB_MAGIC1 'F'
#define MFS_SB_MAGIC2 'S'
#define MFS_SB_MAGIC3 'U'
#define MFS_LABEL_MAX 16u     /* etiqueta de volumen (informática, §22.1) */
#define MFS_REC_MAGIC 0x4D5Au /* 'MZ' magic+versión cabecera E2G */
/* Bytes del magic ya enmascarados: al convertir el literal de 16 bits a 8 bits
 * MSVC avisa de truncamiento de constante (C4310) aunque el cast sea explícito.
 */
#define MFS_REC_MAGIC_HI ((uint8_t)((MFS_REC_MAGIC >> 8) & 0xFFu))
#define MFS_REC_MAGIC_LO ((uint8_t)(MFS_REC_MAGIC & 0xFFu))
#define MFS_ZONE_MAGIC 0x5A4Fu
#define MFS_TOK_MAGIC 0x544Fu /* "TO" */
#define MFS_TOK_MAGIC_HI ((uint8_t)((MFS_TOK_MAGIC >> 8) & 0xFFu))
#define MFS_TOK_MAGIC_LO ((uint8_t)(MFS_TOK_MAGIC & 0xFFu))

/* Límites internos fijos (dimensionado estático, MFS-RES-001).
 *
 * Se dimensionan según la familia de objetivo: un build para MCU de 8 bits
 * (MFS_ALLOW_8BIT_TARGET ⇒ MFS_IS_8BIT_TARGET) usa pools y scratch mínimos
 * para caber en el presupuesto de RAM (§18.2, modo 8-bit ≤ 2 KB), a costa de
 * un volumen/ventana más pequeños. El build de 16/32 bits mantiene los valores
 * normativos completos.
 *
 * Los tres límites que más RAM consumen (ficheros abiertos, ventana WAL y
 * scratch de página) son SOBRESCRIBIBLES en compilación: si el integrador los
 * define (p. ej. `-DMFS_SCRATCH_MAX=1024`), se respeta su valor. Así una
 * plataforma con SRAM escasa (ESP-IDF, STM32) puede recortar el `.bss`. */
#if MFS_IS_8BIT_TARGET
#ifndef MFS_MAX_FILES_OPEN
#define MFS_MAX_FILES_OPEN 2u /* ficheros simultáneos   */
#endif
#define MFS_MAX_SNAPS 2u      /* snapshots O(1)         */
#define MFS_MAX_IOCB 2u       /* ring iocb              */
#define MFS_MAX_ZONES 16u     /* tabla de zonas en RAM  */
#define MFS_MAX_BLOCKS 32u    /* geometría por modo     */
#define MFS_DIR_DEPTH 3u
#define MFS_NAME_MAX 16u
#define MFS_PATH_MAX 48u
#ifndef MFS_WAL_WINDOW_MAX
#define MFS_WAL_WINDOW_MAX 16u /* §25 W: mínimo del rango 32..256 */
#endif
#define MFS_SAVEPOINT_DEPTH 1u /* §9.6                   */
#define MFS_GL_DLIST_MAX 4u    /* AGCB+ dirty-list §25   */
#define MFS_CUSUM_RING 4u
/* Scratch de una página lógica: cubre el mayor chunk 8-bit (256 B). */
#ifndef MFS_SCRATCH_MAX
#define MFS_SCRATCH_MAX MFS_CHUNK_8BIT_COMPACT
#endif
/* Ventana de inodos residentes (ficheros abiertos + directorios en curso) */
#define MFS_INODE_WINDOW 4u
#else
#ifndef MFS_MAX_FILES_OPEN
#define MFS_MAX_FILES_OPEN 32u     /* Extended (§18.2)      */
#endif
#define MFS_MAX_SNAPS 64u          /* Extended              */
#define MFS_MAX_IOCB 16u           /* ring iocb Extended    */
#define MFS_MAX_ZONES MFS_ZONE_MAX /* tabla de zonas en RAM (§8.1) */
#define MFS_MAX_BLOCKS 256u        /* geometría por modo    */
#define MFS_DIR_DEPTH 8u
#define MFS_NAME_MAX 64u
#define MFS_PATH_MAX 256u
#ifndef MFS_WAL_WINDOW_MAX
#define MFS_WAL_WINDOW_MAX 256u /* §25 W rango 32..256   */
#endif
#define MFS_SAVEPOINT_DEPTH 4u  /* §9.6                  */
#define MFS_GL_DLIST_MAX 32u    /* AGCB+ dirty-list §25  */
#define MFS_CUSUM_RING 16u
#ifndef MFS_SCRATCH_MAX
#define MFS_SCRATCH_MAX MFS_CHUNK_EXTENDED
#endif
#define MFS_INODE_WINDOW (MFS_MAX_FILES_OPEN + 16u)
#endif

/* ==== Fase 3–5: FTL Ultra 2, HMT, PQ, XDAM/SDP/CQE ==== */
#if MFS_IS_8BIT_TARGET
#define WOM_MAX_LANE 256u /* bytes de lane WOM-p (§11.4) */
#define MFS_PEP_FEATURES 32u
#define MFS_WEP_CACHE 1u
#define MFS_EBA_MAX_REGIONS 2u
#define MFS_TG_MAX_READS 2u
#else
#define WOM_MAX_LANE 4096u /* bytes de lane WOM-p (§11.4) */
#define MFS_PEP_FEATURES 32u
#define MFS_WEP_CACHE 16u
#define MFS_EBA_MAX_REGIONS 16u
#define MFS_TG_MAX_READS 4u
#endif

/* EBA (§11.8) */
typedef enum {
  MFS_ECC_NONE = 0,
  MFS_ECC_SECDED = 1,
  MFS_ECC_BCH4 = 2,
  MFS_ECC_BCH8 = 3,
  MFS_ECC_LDPC = 4
} mfs_ecc_t;

/* RAS+TG (§11.9) */
enum { MFS_TG_NOMINAL = 0, MFS_TG_COLD = 1, MFS_TG_HOT = 2 };

/* WOM-p (§11.4) */
typedef struct {
  uint32_t addr;
  uint16_t len;
  uint8_t gen;
  uint8_t max_gen;
  uint8_t live;
  uint8_t pad[3];
} mfs_wom_t;

/* WEP (§11.2) */
typedef struct {
  uint16_t logical;
  uint16_t placed;
} mfs_wep_ent_t;

/* HMT (§11.11) */
#define MFS_PPAGE_T0 0x80000000u /* marca de página en T0 */
#define MFS_HMT_T0_BUDGET 32768u /* mínimo para Balanced (§11.11) */

/* XDAM (§14.1) */
#if MFS_IS_8BIT_TARGET
#define MFS_XDAM_MAX 1u
#else
#define MFS_XDAM_MAX 8u
#endif
typedef struct {
  uint32_t flash_addr;
  uint32_t len;
  uint32_t epoch;
  uint32_t samples;
  uint8_t used;
  uint8_t pad[3];
} mfs_xdam_ent_t;

/* CQE (§11.5) */
#if MFS_IS_8BIT_TARGET
#define MFS_CQE_MAX 2u
#else
#define MFS_CQE_MAX 16u
#endif

/* PUF (§15) */
#if MFS_IS_8BIT_TARGET
#define MFS_PUF_HELPER 32u
#define MFS_PUF_SALT 8u
#else
#define MFS_PUF_HELPER 64u
#define MFS_PUF_SALT 16u
#endif

/* Tipos de registro (§8.2 meta: tipo 4 bits) */
enum {
  MFS_RT_DATA = 0,
  MFS_RT_INODE = 1,
  MFS_RT_DIRENT = 2,
  MFS_RT_WALENT = 3,
  MFS_RT_CKPT = 4,
  MFS_RT_TOKEN = 5,
  MFS_RT_HCTAGG = 6,
  MFS_RT_ZRP = 7,
  MFS_RT_EPOCH = 8,
  MFS_RT_EXTENT = 9, /* reservado (ya no se emite: el LBA va en DATA) */
  MFS_RT_TXMARK = 10
};

/* Estados FSM de zona (§24.1) */
enum {
  MFS_Z_EMPTY = 0,
  MFS_Z_OPEN = 1,
  MFS_Z_FULL = 2,
  MFS_Z_RECLAIMING = 3,
  MFS_Z_QUARANTINE = 4
};

/* EDP states (§24.3) */
enum {
  MFS_EDP_MONITOR = 0,
  MFS_EDP_ARMED = 1,
  MFS_EDP_DRAIN = 2,
  MFS_EDP_DONE = 3
};
/* DAB states (§24.4) */
enum { MFS_DAB_FROZEN = 0, MFS_DAB_EXPLOIT = 1, MFS_DAB_EXPLORE = 2 };

/* ==== Descriptor de extent en RAM (12 B on-flash §22.4) ==== */
typedef struct {
  uint32_t vpage; /* página lógica */
  uint32_t ppage; /* página física */
  uint16_t npages;
  uint16_t flags; /* gen(4b) snap(4b) shared cow zrp ... */
} mfs_extent_t;

/* ==== Inodo en RAM ==== */
#if MFS_IS_8BIT_TARGET
#define MFS_EXT_INLINE 2u
#else
#define MFS_EXT_INLINE 4u
#endif
typedef struct {
  uint32_t ino;
  uint32_t size;
  uint32_t mtime;
  uint16_t nlink;
  uint8_t type; /* 0=file 1=dir */
  uint8_t gen;
  uint8_t snapid;
  uint8_t hotness;
  uint8_t valid;
  uint8_t next;
  mfs_extent_t ext[MFS_EXT_INLINE];
  uint32_t parent; /* dir padre para readdir/nombre */
  uint32_t uid;    /* propietario POSIX (§21.2) */
  uint32_t gid;    /* grupo POSIX               */
  uint16_t perm;   /* permisos (0777)           */
  uint16_t pad2;
  char name[MFS_NAME_MAX];
} mfs_inode_ram_t;

/* ==== Zona (ZLF) ==== */
typedef struct {
  uint16_t zone_id;
  uint8_t state;      /* FSM §24.1 */
  uint8_t class_hot;  /* hotness destino */
  uint32_t seq;       /* secuencia monótona por zona */
  uint32_t write_ptr; /* offset byte dentro de zona */
  uint32_t start_addr;
  uint32_t size;
  uint16_t valid_pages;
  uint16_t total_pages;
  uint8_t zrp;     /* flag ZRP activo */
  uint8_t zrp_dst; /* página destino de la última reconstrucción ZRP */
  /* telemetría FTL Ultra 2 (§11.1 ELM/PEP, §11.9 TG) */
  uint16_t pe_cycles;       /* ciclos P/E acumulados            */
  uint16_t pe_max;          /* límite de ciclos del perfil      */
  uint16_t ber_x1e6;        /* BER instantáneo (×10⁻⁶)          */
  uint16_t ber_slope_x1000; /* pendiente EMA de BER (×1000)   */
  uint8_t trend;            /* 0..1000 normalizado              */
  uint8_t slc;              /* zona en ventana SLC              */
} mfs_zone_t;

/* ==== WAL entry en RAM (ventana BMT) ==== */
typedef struct {
  uint32_t lba;      /* LBA lógico destino (E2G) */
  uint32_t zone_off; /* posición en zona WAL */
  uint16_t len;
  uint8_t kind; /* MFS_RT_* */
  uint8_t committed;
} mfs_wal_ent_t;

/* ==== Registro WAL en RAM (ventana BMT, §9/§25 W) ==== */
typedef struct {
  uint32_t txid;
  uint32_t ppage;    /* página física donde vive el registro WALENT */
  uint8_t committed; /* 0 = pendiente | sp_depth vigente | 1 = T1 */
  uint8_t valid;
} mfs_wrec_t;

/* ==== Núcleo mf_t (§23.1 — 96 B packed conceptualmente; tablas aparte) ==== */
struct mfs_fs {
  const mfs_config *cfg;
  mfs_hwv_handle_t hwv_h; /* 8 B */
  mfs_hwv_t hwv;          /* copia de trabajo solo init/format */
  uint32_t epoch;
  uint64_t seq;
  uint8_t mode;
  uint8_t suite;
  uint8_t flags;
  char label[MFS_LABEL_MAX]; /* etiqueta de volumen (§22.1, informativa) */
  uint16_t debt_gld;         /* GLD (§9.4) */
  uint8_t edp_state;
  uint8_t edp_level; /* §12.3 niveles 0..5 */
  uint8_t dab_state;
  uint8_t merkle_root[8]; /* raíz checkpoint truncada (§9.5) */
  void *ovl[4];           /* unions según modo (§7.2) */
  /* raíces ART vivas, ventana WAL, contadores críticos: */
  uint32_t root_art; /* ppage raíz metadatos */
  uint16_t wal_head, wal_count;
  mfs_wrec_t wal[MFS_WAL_WINDOW_MAX]; /* ventana BMT (§9.4/§25 W) */
  uint32_t txid_next, txid_cur;
  uint32_t sp_marks[MFS_SAVEPOINT_DEPTH + 1u]; /* wal_count al crear sp */
  uint32_t zone_wal;                           /* zona WAL activa           */
  uint32_t free_pages;
  uint32_t zone_cap;   /* nº de zonas usables según geometría */
  uint8_t zone_blocks; /* 1..4 bloques por zona (§8.1)         */
  bool mounted;
  bool tx_open;
  uint8_t sp_depth;
  /* tablas estáticas (pools, sin heap) */
  mfs_inode_ram_t inos[MFS_INODE_WINDOW]; /* ventana flash-first */
  mfs_zone_t zones[MFS_MAX_ZONES];
  mfs_file *open_files[MFS_MAX_FILES_OPEN];
  mfs_snap_id snaps[MFS_MAX_SNAPS];
  uint32_t snap_roots[MFS_MAX_SNAPS];
  uint8_t snap_count;
  mfs_iocb *ring[MFS_MAX_IOCB];
  uint8_t ring_head, ring_tail;
  /* HCT agregados (§16, modos bajos: a flash en sync/unmount) */
  mfs_health_t hct;
  /* DAB/CUSUM/ELD estado (§24.4) */
  uint32_t dab_rng;
  float w_exp3[3]; /* pesos EXP3 (§24.4) */
  uint16_t tau[3]; /* τ_A/τ_B/τ_C activos */
  uint16_t d_max;
  uint16_t cusum_pos, cusum_neg;
  int16_t cusum_last_mean_x100;
  uint8_t cusum_init;
  uint32_t eld_budget_mj; /* presupuesto energía ventana */
  uint32_t eld_spent_mj;
  uint32_t ops_since_dab;
  /* Token slots A/B (§9.2) */
  uint32_t tok_seq_a, tok_seq_b;
  uint8_t tok_toggle;
  /* buffer SRB único (§10.1) apunta a ovl o estático */
  uint8_t *srb;
  uint16_t srb_size;
  uint8_t err_last;
  /* ==== Fase 3–5 ==== */
  /* SLEC (§11.7) */
  uint16_t slec_zone;
  uint16_t slec_pages;
  /* EBA (§11.8) */
  uint16_t eba_ber[MFS_EBA_MAX_REGIONS];
  uint32_t eba_violations;
  /* RAS + Thermal Governor (§11.9) */
  int16_t tg_temp_c;
  uint8_t tg_state;
  uint8_t tg_pad;
  uint32_t tg_reads[MFS_MAX_ZONES];
  uint32_t tg_disturb_threshold;
  /* ELM (§11.1) */
  uint32_t elm_ber_ema_q16;
  /* WEP (§11.2) */
  uint16_t wep_k1, wep_k2;
  uint32_t wep_rr;
  bool wep_cache_valid;
  mfs_wep_ent_t wep_cache[MFS_WEP_CACHE];
  /* HMT (§11.11) */
  uint8_t hmt_active;
  uint8_t hmt_pad[3];
  uint32_t hmt_t0_off; /* puntero de append en T0 */
  uint32_t hmt_t0_seq; /* secuencia monótona T0   */
  /* PUF / PQ (§15) */
  uint8_t puf_enrolled;
  uint8_t puf_helper[MFS_PUF_HELPER];
  uint8_t puf_salt[MFS_PUF_SALT];
  /* XDAM (§14.1) */
  mfs_xdam_ent_t xdam[MFS_XDAM_MAX];
  uint8_t xdam_epoch;
  /* SDP (§14.1) */
  void *sdp_target; /* mf_t* destino del WAL */
  uint16_t sdp_mtu;
  uint16_t sdp_pad;
  uint32_t sdp_frames;
  uint32_t sdp_dropped;
  /* CQE (§11.5) */
  uint8_t cqe_depth;
  uint8_t cqe_head;
  uint8_t cqe_tail;
  uint8_t cqe_pad;
  uint32_t cqe_completed;
  mfs_iocb *cqe_q[MFS_CQE_MAX];
  /* WOM-p: espejo monotónico de época (§15 MFS-SEC-003) */
  mfs_wom_t epoch_wom;
  uint32_t wom_epoch_seen;
};

struct mfs_file {
  uint32_t ino;
  uint64_t pos;
  uint32_t size;
  uint16_t flags;
  uint8_t ref;
  uint8_t pad;
  uint32_t dirty_hint; /* extent actual */
  mf_t *fs;
};

struct mfs_dir {
  mf_t *fs;
  uint32_t parent_ino;
  uint16_t idx;
  uint8_t used;
};

/* ==== Sub-sistemas internos ==== */
/* crc32c / blake3 / crypto suites / lz4 / gear-cdc / fsst / exp3 / thermometer
 */
uint32_t mfs_crc32c(const uint8_t *buf, uint32_t len, uint32_t seed);
void mfs_b3_256(const uint8_t *key, uint32_t keylen, const uint8_t *in,
                uint32_t inlen, uint8_t out[32]);
void mfs_b3_mac_trunc(const uint8_t key[32], const uint8_t *msg, uint32_t len,
                      uint8_t mac[8]);
void mfs_b3_merkle_root(const uint8_t *leaves, uint32_t nleaf,
                        uint32_t leaf_len, uint8_t root_out[32]);

/* ==== SHA-256 streaming sin heap (FIPS 180-4) ==== */
typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  uint32_t buflen;
} mfs_sha256_ctx;
void mfs_sha256_init(mfs_sha256_ctx *c);
void mfs_sha256_update(mfs_sha256_ctx *c, const uint8_t *in, uint32_t len);
void mfs_sha256_final(mfs_sha256_ctx *c, uint8_t out[32]);
void mfs_hmac_sha256(const uint8_t *key, uint32_t klen, const uint8_t *msg,
                     uint32_t mlen, uint8_t out[32]);

/* aprox. determinista de e^x en punto flotante simple (sin libm, §20.1 P1) */
static inline float expf_fast(float x) {
  if (x > 10.0f)
    x = 10.0f;
  if (x < -10.0f)
    x = -10.0f;
  /* e^x ≈ (1 + x/1024)^1024 — 10 elevaciones al cuadrado, error < 0.05 % */
  float t = 1.0f + x / 1024.0f;
  for (int i = 0; i < 10; i++)
    t *= t;
  return t;
}
/* suites S0-S3 (AEAD cifrar/descifrar autenticado) src==dst permitido */
void mfs_zeroize(void *p, size_t n); /* MFS-SEC-002 */
bool mfs_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n);
mfs_st mfs_suite_seal(uint8_t suite, const uint8_t key[32],
                      uint64_t epoch_seq_nonce, const uint8_t *in, uint16_t len,
                      uint8_t *out, uint16_t *olen /* in: len+16 tag */);
mfs_st mfs_suite_open(uint8_t suite, const uint8_t key[32],
                      uint64_t epoch_seq_nonce, const uint8_t *in,
                      uint16_t clen, uint8_t *out, uint16_t *olen);
uint16_t mfs_lz4_compress(const uint8_t *in, uint16_t ilen, uint8_t *out,
                          uint16_t ospace);
uint16_t mfs_lz4_decompress(const uint8_t *in, uint16_t ilen, uint8_t *out,
                            uint16_t ospace);
uint32_t mfs_gear_hash_init(void);
bool mfs_gear_boundary(uint32_t *state, uint8_t b, uint32_t mask);
uint16_t mfs_fsst_encode(const uint8_t *in, uint16_t ilen, uint8_t *out,
                         uint16_t ospace);
uint16_t mfs_fsst_decode(const uint8_t *in, uint16_t ilen, uint8_t *out,
                         uint16_t ospace);
/* EXP3 determinista sembrado (§24.4): replay-determinista */
uint32_t mfs_exp3_update(float w[3], const float r[3], const float x[3][3],
                         uint32_t *rng, int n_arms);
/* TFC termométrico (§8.4): 1 bit por intervalo de 16 ciclos */
uint32_t mfs_tfc_count(const uint8_t *bits, uint32_t nbytes);
bool mfs_tfc_will_carry(const uint8_t *bits, uint32_t nbytes);
void mfs_tfc_increment(uint8_t *bits, uint32_t nbytes);

/* HAL/viabilidad (§5,§6) */
mfs_mode_t mfs_select_mode(const mfs_hwv_t *hwv, const mfs_config *cfg);
mfs_st mfs_hal_detect(mf_t *fs, const mfs_config *cfg, mfs_hwv_t *out);
/* Bus read-only (§14.2 MFS-BUS-001..004) */
uint32_t mfs_bus_measure_hz(const mfs_l2_driver *drv, void *ctx);

/* Arquitectura y aceleración HW (§3.1 MFS-ARCH-010 rev.3) — mfs_arch.c */
mfs_st mfs_arch_detect(mfs_arch_info_t *out);
mfs_st mfs_arch_adapt_config(const mfs_arch_info_t *info, mfs_config *cfg);
const mfs_arch_info_t *mf_arch_last(void); /* último análisis (diagnóstico) */
bool mfs_arch_crc32c_hw_available(void);   /* CRC-32C por instrucción */
uint32_t mfs_crc32c_hw(const uint8_t *buf, uint32_t len, uint32_t seed);

/* Flash helpers */
mfs_st mfs_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len);
mfs_st mfs_write(mf_t *fs, uint32_t addr, const void *src,
                 uint32_t len); /* WOB barrier */
mfs_st mfs_erase(mf_t *fs, uint32_t addr);
mfs_st mfs_t0_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len);
mfs_st mfs_t0_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len);
bool mfs_has_t0(mf_t *fs);
bool mfs_edp_capable(mf_t *fs);

/* Pipeline de datos por página (§10.1–§10.4, src/core/mfs_compact.c) */
mfs_st mfs_data_write(mf_t *fs, uint32_t lba, const uint8_t *pl, uint16_t len,
                      uint8_t hotness, uint8_t kind, uint32_t *ppage_out);
mfs_st mfs_data_read(mf_t *fs, uint32_t ppage, uint32_t lba, uint8_t *buf,
                     uint16_t bufsize, uint16_t *rlen);

/* Suite negociada (§10.6, src/core/mfs_hal.c) */
uint8_t mfs_negotiate_suite(const mfs_hwv_t *hwv, const mfs_config *cfg,
                            mfs_mode_t mode);

/* SRB y CFX (§10.1/§10.4, src/core/mfs_compact.c) */
void mfs_srb_attach(mf_t *fs);
void mfs_cfx_reset(void);
int mfs_cfx_lookup(uint64_t h64, uint32_t *ppage);
void mfs_cfx_insert(uint64_t h64, uint32_t ppage);

/* E2G registro (§8.2) */
mfs_st mfs_rec_write(mf_t *fs, uint32_t zone, uint32_t lba, uint8_t kind,
                     uint8_t gen, uint8_t snapid, uint8_t hotness,
                     uint8_t dictid, const uint8_t *payload, uint16_t plen,
                     uint32_t *ppage_out);
mfs_st mfs_rec_read(mf_t *fs, uint32_t ppage, uint32_t expect_lba,
                    uint8_t expect_gen, uint8_t *kind, uint8_t *gen,
                    uint8_t *snapid, uint8_t *dictid, uint8_t *buf,
                    uint16_t bufsize, uint16_t *rlen);

/* Zonas/ZLF (§11.6, FSM §24.1) */
uint32_t mfs_page_bytes(mf_t *fs);    /* chunk del modo activo   */
uint32_t mfs_payload_bytes(mf_t *fs); /* capacidad útil por registro */
uint16_t mfs_e2g_hdr(mf_t *fs);       /* cabecera E2G NOR/NAND   */
mfs_zone_t *mfs_zone(mf_t *fs, uint32_t id);
int mfs_zone_alloc_open(mf_t *fs, uint8_t hotness);
mfs_st mfs_zone_seal(mf_t *fs, uint32_t id);
mfs_st mfs_zone_erase(mf_t *fs, mfs_zone_t *z);    /* borra todos sus bloques */
mfs_st mfs_gc_slice(mf_t *fs, uint32_t budget_us); /* AGCB+ sobre GLD */
void mfs_gc_force(mf_t *fs); /* reclamación forzosa si no hay zona libre */
void mfs_gld_add(mf_t *fs, uint32_t pages, bool meta);
uint32_t mfs_zone_count_free(mf_t *fs);
void mfs_gld_maybe_gc(mf_t *fs); /* umbral → slice  */

/* Cabecera de zona (§22.2) + recuperación de metadatos al montar */
mfs_st mfs_zonehdr_write(mf_t *fs, mfs_zone_t *z);
mfs_st mfs_zonehdr_read(mf_t *fs, mfs_zone_t *z);
mfs_st mfs_scan_zones(mf_t *fs);       /* FSM montaje paso 5 */
mfs_st mfs_recover_metadata(mf_t *fs); /* inodos + dirents */

/* L2P estático (mapa lba lógica → página física) */
void mfs_l2p_reset(void);
mfs_st mfs_l2p_put(uint32_t lba, uint32_t ppage);
uint32_t mfs_l2p_get(uint32_t lba); /* 0xFFFFFFFF si ausente   */
void mfs_l2p_drop(uint32_t lba);    /* retira el mapeo del LBA */
uint32_t mfs_l2p_capacity(void);    /* entradas totales        */
uint32_t mfs_l2p_used(void);        /* entradas ocupadas       */

/* WAL+/token (§9) */
mfs_st mfs_token_commit(mf_t *fs, uint32_t txid, uint32_t crc);
mfs_st mfs_wal_append(mf_t *fs, uint32_t lba, uint8_t kind, const uint8_t *pl,
                      uint16_t len, uint32_t *ppage);
mfs_st mfs_wal_replay(mf_t *fs, uint32_t window); /* BMT acotado */
bool mfs_tok_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE]);

/* ==== helpers compartidos core (definidos en mfs_fs.c) ==== */
uint32_t mfs_next_lba(mf_t *fs); /* asignador LBA lógico       */
void mfs_dab_init(mf_t *fs);     /* §24.4 EXP3 sembrado        */
void mfs_dab_feedback(mf_t *fs, uint8_t arm, uint32_t latency_us);
uint8_t mfs_dab_select_arm(mf_t *fs);      /* brazo por pesos w          */
float mfs_dab_reward_latency(uint32_t us); /* r ∈ [0,1]                  */
mfs_st mfs_snap_persist(mf_t *fs);         /* tabla de snaps a WAL (§10.8) */
extern mf_t *g_mfs_instance;               /* instancia única (sp/fpt)   */
void mfs_cusum_update(mf_t *fs, int16_t x_x100); /* §16 CUSUM        */
bool mfs_cusum_alarm(const mf_t *fs);
void mfs_edp_check(mf_t *fs); /* §12.3 niveles 1-5    */
void mfs_edp_drain(mf_t *fs);
int mfs_lookup(mf_t *fs, const char *path, uint32_t *parent_out);
mfs_inode_ram_t *mfs_ino_get(mf_t *fs, uint32_t ino);
mfs_inode_ram_t *mfs_ino_alloc(mf_t *fs);
void mfs_ino_evict(mf_t *fs, mfs_inode_ram_t *ino);
mfs_st mfs_meta_flush(mf_t *fs, uint32_t ino); /* persistir inodo vía WAL */
uint32_t mfs_extent_read(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                         uint8_t *buf, uint16_t bufsize, uint16_t *rlen);
mfs_st mfs_extent_write(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                        const uint8_t *buf, uint16_t len);
/* Páginas lógicas de un tamaño de fichero y retirada de su mapeo L2P */
uint32_t mfs_vpages_of(mf_t *fs, uint32_t size);
void mfs_extent_drop_range(mf_t *fs, uint32_t ino, uint32_t from_vpage,
                           uint32_t to_vpage);
uint32_t mfs_zone_count_free(mf_t *fs);
void mfs_gld_maybe_gc(mf_t *fs); /* umbral deuda → slice */
extern bool g_mfs_edp_inject;    /* hook de test vFlash  */

/* Checkpoint (§9.5) */
mfs_st mfs_checkpoint_write(mf_t *fs);
mfs_st mfs_checkpoint_load(mf_t *fs);

/* Snapshots/FPT (§10.8) */
mfs_st mfs_snap_persist(mf_t *fs);

/* HCT (§16) */
void mfs_hct_event(mf_t *fs, uint16_t ev, uint32_t val);
void mfs_hct_flush(mf_t *fs);
/* Codigo evento HCT */
enum {
  MFS_EV_BOOT = 1,
  MFS_EV_POWER_LOSS,
  MFS_EV_EDP_DRAIN,
  MFS_EV_QUARANTINE,
  MFS_EV_E2G_FAIL,
  MFS_EV_SUITE_NEG,
  MFS_EV_DAB_CHANGE,
  MFS_EV_CUSUM_ALARM,
  MFS_EV_ELD_DEFER,
  MFS_EV_FPT,
  MFS_EV_SNAP,
  MFS_EV_PUF_FALLBACK,
  MFS_EV_NOTVIABLE,
  MFS_EV_ARCH_REJECT,
  MFS_EV_CONSERVATIVE,
  MFS_EV_MOUNT_OK,
  /* Fase 3–5 */
  MFS_EV_WOM_PROG,
  MFS_EV_SLEC,
  MFS_EV_EBA,
  MFS_EV_TG,
  MFS_EV_ELM,
  MFS_EV_PEP,
  MFS_EV_ZRP,
  MFS_EV_HMT,
  MFS_EV_PUF,
  MFS_EV_PQ,
  MFS_EV_XDAM,
  MFS_EV_SDP,
  MFS_EV_CQE
};

/* CBOR+COSE exportación (§15 Export) */
uint32_t mfs_cbor_health(const mfs_health_t *h, uint8_t *out, uint32_t cap);

/* Energy model (§13.3 ELD, §12.3 EDP: E_cap = ½·C·(V0²−Vmin²)) */
uint32_t mfs_energy_prog_mj(const mfs_hwv_t *hwv, uint32_t bytes);
uint32_t mfs_edp_window_us(const mfs_rail_state *r, uint16_t vmin_mv,
                           uint32_t cap_uf, uint32_t i_ma);

/* asserts de fase overlays (RSC §7.2, MFS-RES-003) */
#if defined(MFS_DEBUG)
#define MFS_PHASE_ASSERT(cond)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      mfs_assert_fail(__LINE__);                                               \
    }                                                                          \
  } while (0)
void mfs_assert_fail(int line);
#else
#define MFS_PHASE_ASSERT(c) ((void)(c))
#endif

extern mf_t *g_mfs_instance; /* única instancia estática en targets pequeños */
extern mfs_hwv_t g_last_hwv;
extern mfs_mode_t g_last_selected;

/* ==== FTL Ultra 2 (§11) — src/ftl/mfs_ftl2.c ==== */
/* WOM-p (§11.4) */
mfs_st mfs_wom_begin(mf_t *fs, uint32_t addr, uint16_t lane_bytes,
                     uint8_t max_gen, mfs_wom_t *w);
mfs_st mfs_wom_program(mf_t *fs, mfs_wom_t *w, const uint8_t *bits,
                       uint16_t nbits);
mfs_st mfs_wom_read(mf_t *fs, const mfs_wom_t *w, uint8_t *bits,
                    uint16_t nbits);
bool mfs_wom_exhausted(const mfs_wom_t *w);
/* SLEC (§11.7) */
bool mfs_slec_enabled(mf_t *fs);
mfs_st mfs_slec_enter(mf_t *fs, uint32_t zone);
mfs_st mfs_slec_fold(mf_t *fs, uint32_t budget_us);
/* EBA (§11.8) */
mfs_ecc_t mfs_eba_select(const mfs_hwv_t *hwv, uint32_t region);
void mfs_eba_report(mf_t *fs, uint32_t region, uint16_t ber_x1e6);
bool mfs_eba_should_scrub(const mf_t *fs, uint32_t region);
uint16_t mfs_eba_capacity(mfs_ecc_t ecc);
/* RAS + TG (§11.9) */
void mfs_tg_set_temp(mf_t *fs, int16_t temp_c);
uint8_t mfs_tg_scrub_urgency(const mf_t *fs, uint32_t age_s,
                             uint32_t retention_spec_s);
mfs_st mfs_tg_step(mf_t *fs, uint32_t budget_us);
void mfs_tg_read_disturb(mf_t *fs, uint32_t block, uint32_t reads);
/* ELM + PEP (§11.1) */
uint16_t mfs_elm_health(uint16_t pe, uint16_t pe_max, uint16_t ber,
                        uint16_t ber_fail, uint16_t trend);
uint32_t mfs_elm_rul(uint32_t ber, uint32_t ber_fail, uint32_t slope_x1000);
uint16_t mfs_pep_score(const uint16_t feat[MFS_PEP_FEATURES]);
void mfs_elm_ema(mf_t *fs, uint32_t ber);
void mfs_elm_proactive_trigger(mf_t *fs, uint32_t zone, uint32_t rul,
                               uint32_t rul_mean);
int mfs_gc_select_victim_ftl(mf_t *fs, uint32_t *elm_out, uint32_t *pep_out);
/* WEP (§11.2) */
uint16_t mfs_wep_place(mf_t *fs, uint16_t logical);
uint16_t mfs_wep_feistel(uint16_t x, uint16_t k1, uint16_t k2);
void mfs_wep_init(mf_t *fs, const uint8_t uid[8]);
/* ZRP (§8.5) */
void mfs_gf_init(void);
mfs_st mfs_zrp_encode(mf_t *fs, mfs_zone_t *z);
mfs_st mfs_zrp_recover(mf_t *fs, mfs_zone_t *z, uint32_t idx,
                       uint32_t dst_page);
/* Ventana idle para mantenimiento (§13.1 SPDR) */
bool mfs_gc_rt_idle(const mf_t *fs);

/* ==== HMT (§11.11) — src/tier/mfs_hmt.c ==== */
/* Aplicadores de registro compartidos con la recuperación (mfs_fs.c) */
void mfs_ino_deser(mfs_inode_ram_t *n, const uint8_t *b, uint16_t len);
mfs_st mfs_inode_apply(mf_t *fs, const uint8_t *pl, uint16_t len,
                       uint16_t *max_ino);
void mfs_dirent_apply(mf_t *fs, const uint8_t *pl, uint16_t len);
void mfs_epoch_apply(mf_t *fs, const uint8_t *pl, uint16_t len);

bool mfs_hmt_active(const mf_t *fs);
mfs_st mfs_hmt_init(mf_t *fs);
mfs_st mfs_hmt_t0_write(mf_t *fs, uint8_t kind, uint32_t lba,
                        const uint8_t *data, uint16_t len, uint32_t *t0_addr);
mfs_st mfs_hmt_scan(mf_t *fs);

/* ==== PUF / PK / PQ (§15) — src/sec/mfs_pq.c ==== */
void mfs_hkdf_sha256(const uint8_t *salt, uint32_t saltlen, const uint8_t *ikm,
                     uint32_t ikmlen, uint8_t *okm, uint32_t okmlen);
void mfs_hkdf_sha256_info(const uint8_t *salt, uint32_t saltlen,
                          const uint8_t *ikm, uint32_t ikmlen,
                          const uint8_t *info, uint32_t infolen, uint8_t *okm,
                          uint32_t okmlen);
mfs_st mfs_puf_enroll(mf_t *fs, const uint8_t *sram, uint32_t len,
                      const uint8_t salt[MFS_PUF_SALT], uint8_t key[32]);
mfs_st mfs_puf_reproduce(mf_t *fs, const uint8_t *sram, uint32_t len,
                         uint8_t key[32]);
mfs_st mfs_puf_fallback_key(mf_t *fs, const uint8_t *uid, uint32_t ulen,
                            uint8_t key[32]);
/* LMS/SP 800-208 (verificación + generación para tests) */
typedef struct {
  uint8_t I[16];
  uint8_t T1[32];
  uint8_t type;
} mfs_lms_pub_t;
mfs_st mfs_lms_verify(const mfs_lms_pub_t *pub, const uint8_t *msg,
                      uint32_t mlen, const uint8_t *sig, uint32_t siglen);
mfs_st mfs_lms_keygen(const uint8_t seed[32], mfs_lms_pub_t *pub,
                      uint8_t sk_out[64]);
mfs_st mfs_lms_sign(const uint8_t seed[32], uint32_t q, const uint8_t *msg,
                    uint32_t mlen, uint8_t *sig, uint32_t *siglen);

/* ==== XDAM / SDP / CQE (§14.1, §11.5) — src/xio/mfs_xio.c ==== */
mfs_st mfs_xdam_map(mf_t *fs, uint32_t asset, uint32_t flash_addr,
                    uint32_t len);
mfs_st mfs_xdam_read(mf_t *fs, uint32_t asset, uint32_t off, void *dst,
                     uint32_t len);
void mfs_xdam_epoch_bump(mf_t *fs);
void mfs_xdam_reset(mf_t *fs);
mfs_st mfs_sdp_init(mf_t *fs, mf_t *target, uint16_t mtu);
mfs_st mfs_sdp_feed(mf_t *fs, const void *sample, uint16_t len);
mfs_st mfs_cqe_init(mf_t *fs, uint8_t depth);
mfs_st mfs_cqe_submit(mf_t *fs, mfs_iocb *cb);
int mfs_cqe_poll(mf_t *fs, mfs_iocb **done, int max);

#endif /* MATRIXFS_MFS_INTERNAL_H */
