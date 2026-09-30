/* mfs_internal.h — estructuras internas del núcleo (no forman API pública) */
#ifndef MATRIXFS_MFS_INTERNAL_H
#define MATRIXFS_MFS_INTERNAL_H

#include "matrixfs/matrixfs.h"

/* ==== Layout on-flash (§22) ==== */
#define MFS_SB_SIZE        256u   /* superblock A/B (§22.1) */
#define MFS_HWV_OFFSET     512u   /* tras SB A/B            */
#define MFS_HWV_SIZE       64u
#define MFS_ZONEHDR_SIZE   64u   /* cabecera de zona (§22.2) */
#define MFS_INO_S_SIZE     32u   /* INO-S UN/Nano (§22.3)    */
#define MFS_INO_L_SIZE     64u   /* INO-L Compact+           */
#define MFS_EXTENT_SIZE    12u   /* registro de extent (§22.4) */
#define MFS_E2G_NOR_HDR    20u   /* cabecera de registro NOR (§8.2) */
#define MFS_E2G_NAND_HDR   32u
#define MFS_TOKEN_T1_SIZE  32u   /* token commit T1 (§9.2)   */
#define MFS_TOKEN_T0_SIZE  16u   /* token commit T0          */
#define MFS_TOKEN_SLOTS    16u   /* anillo A/B con contador termométrico */

#define MFS_SB_MAGIC0 'M'
#define MFS_SB_MAGIC1 'F'
#define MFS_SB_MAGIC2 'S'
#define MFS_SB_MAGIC3 'U'
#define MFS_REC_MAGIC  0x4D5Au  /* 'MZ' magic+versión cabecera E2G */
#define MFS_ZONE_MAGIC 0x5A4Fu
#define MFS_TOK_MAGIC  0x544Fu /* "TO" */

/* Límites internos fijos (dimensionado estático, MFS-RES-001) */
#define MFS_MAX_FILES_OPEN   32u   /* Extended (§18.2)      */
#define MFS_MAX_SNAPS        64u   /* Extended              */
#define MFS_MAX_IOCB         16u   /* ring iocb Extended    */
#define MFS_MAX_ZONES       128u   /* tabla de zonas en RAM */
#define MFS_MAX_BLOCKS      256u   /* geometría por modo    */
#define MFS_DIR_DEPTH         8u
#define MFS_NAME_MAX         64u
#define MFS_PATH_MAX        256u
#define MFS_WAL_WINDOW_MAX  256u   /* §25 W rango 32..256   */
#define MFS_SAVEPOINT_DEPTH   4u   /* §9.6                  */
#define MFS_GL_DLIST_MAX     32u   /* AGCB+ dirty-list §25  */
#define MFS_CUSUM_RING       16u

/* Tipos de registro (§8.2 meta: tipo 4 bits) */
enum {
    MFS_RT_DATA = 0, MFS_RT_INODE = 1, MFS_RT_DIRENT = 2,
    MFS_RT_WALENT = 3, MFS_RT_CKPT = 4, MFS_RT_TOKEN = 5,
    MFS_RT_HCTAGG = 6, MFS_RT_ZRP = 7, MFS_RT_EPOCH = 8
};

/* Estados FSM de zona (§24.1) */
enum {
    MFS_Z_EMPTY = 0, MFS_Z_OPEN = 1, MFS_Z_FULL = 2,
    MFS_Z_RECLAIMING = 3, MFS_Z_QUARANTINE = 4
};

/* EDP states (§24.3) */
enum { MFS_EDP_MONITOR = 0, MFS_EDP_ARMED = 1, MFS_EDP_DRAIN = 2, MFS_EDP_DONE = 3 };
/* DAB states (§24.4) */
enum { MFS_DAB_FROZEN = 0, MFS_DAB_EXPLOIT = 1, MFS_DAB_EXPLORE = 2 };

/* ==== Descriptor de extent en RAM (12 B on-flash §22.4) ==== */
typedef struct {
    uint32_t vpage;   /* página lógica */
    uint32_t ppage;   /* página física */
    uint16_t npages;
    uint16_t flags;   /* gen(4b) snap(4b) shared cow zrp ... */
} mfs_extent_t;

/* ==== Inodo en RAM ==== */
#define MFS_EXT_INLINE 4u
typedef struct {
    uint32_t ino;
    uint32_t size;
    uint32_t mtime;
    uint16_t nlink;
    uint8_t  type;      /* 0=file 1=dir */
    uint8_t  gen;
    uint8_t  snapid;
    uint8_t  hotness;
    uint8_t  valid;
    uint8_t  next;
    mfs_extent_t ext[MFS_EXT_INLINE];
    uint32_t parent;    /* dir padre para readdir/nombre */
    char     name[MFS_NAME_MAX];
} mfs_inode_ram_t;

/* ==== Zona (ZLF) ==== */
typedef struct {
    uint16_t zone_id;
    uint8_t  state;         /* FSM §24.1 */
    uint8_t  class_hot;     /* hotness destino */
    uint32_t seq;           /* secuencia monótona por zona */
    uint32_t write_ptr;     /* offset byte dentro de zona */
    uint32_t start_addr;
    uint32_t size;
    uint16_t valid_pages;
    uint16_t total_pages;
    uint8_t  zrp;           /* flag ZRP activo */
    uint8_t  pad;
} mfs_zone_t;

/* ==== WAL entry en RAM (ventana BMT) ==== */
typedef struct {
    uint32_t lba;      /* LBA lógico destino (E2G) */
    uint32_t zone_off; /* posición en zona WAL */
    uint16_t len;
    uint8_t  kind;     /* MFS_RT_* */
    uint8_t  committed;
} mfs_wal_ent_t;

/* ==== Registro WAL en RAM (ventana BMT, §9/§25 W) ==== */
typedef struct {
    uint32_t txid;
    uint32_t ppage;     /* página física donde vive el registro WALENT */
    uint8_t  committed; /* 0 = pendiente | sp_depth vigente | 1 = T1 */
    uint8_t  valid;
} mfs_wrec_t;

/* ==== Núcleo mf_t (§23.1 — 96 B packed conceptualmente; tablas aparte) ==== */
struct mfs_fs {
    const mfs_config *cfg;
    mfs_hwv_handle_t  hwv_h;          /* 8 B */
    mfs_hwv_t         hwv;            /* copia de trabajo solo init/format */
    uint32_t epoch;
    uint64_t seq;
    uint8_t  mode;
    uint8_t  suite;
    uint8_t  flags;
    uint16_t debt_gld;                /* GLD (§9.4) */
    uint8_t  edp_state;
    uint8_t  edp_level;               /* §12.3 niveles 0..5 */
    uint8_t  dab_state;
    uint8_t  merkle_root[8];          /* raíz checkpoint truncada (§9.5) */
    void    *ovl[4];                  /* unions según modo (§7.2) */
    /* raíces ART vivas, ventana WAL, contadores críticos: */
    uint32_t root_art;                /* ppage raíz metadatos */
    uint16_t wal_head, wal_count;
    mfs_wrec_t wal[MFS_WAL_WINDOW_MAX];   /* ventana BMT (§9.4/§25 W) */
    uint32_t txid_next, txid_cur;
    uint32_t sp_marks[MFS_SAVEPOINT_DEPTH + 1u]; /* wal_count al crear sp */
    uint32_t zone_wal;                /* zona WAL activa           */
    uint32_t free_pages;
    bool     mounted;
    bool     tx_open;
    uint8_t  sp_depth;
    /* tablas estáticas (pools, sin heap) */
    mfs_inode_ram_t  inos[MFS_MAX_FILES_OPEN + 16u]; /* ventana flash-first */
    mfs_zone_t       zones[MFS_MAX_ZONES];
    uint32_t         l2p[MFS_MAX_BLOCKS * 16u];      /* hash L2P simplificado */
    mfs_file        *open_files[MFS_MAX_FILES_OPEN];
    mfs_snap_id      snaps[MFS_MAX_SNAPS];
    uint32_t         snap_roots[MFS_MAX_SNAPS];
    uint8_t          snap_count;
    mfs_iocb        *ring[MFS_MAX_IOCB];
    uint8_t          ring_head, ring_tail;
    /* HCT agregados (§16, modos bajos: a flash en sync/unmount) */
    mfs_health_t     hct;
    /* DAB/CUSUM/ELD estado (§24.4) */
    uint32_t         dab_rng;
    float            w_exp3[3];        /* pesos EXP3 (§24.4) */
    uint16_t         tau[3];           /* τ_A/τ_B/τ_C activos */
    uint16_t         d_max;
    uint16_t         cusum_pos, cusum_neg;
    int16_t          cusum_last_mean_x100;
    uint32_t         eld_budget_mj;    /* presupuesto energía ventana */
    uint32_t         eld_spent_mj;
    uint32_t         ops_since_dab;
    /* Token slots A/B (§9.2) */
    uint32_t         tok_seq_a, tok_seq_b;
    uint8_t          tok_toggle;
    /* buffer SRB único (§10.1) apunta a ovl o estático */
    uint8_t         *srb;
    uint16_t         srb_size;
    uint8_t          err_last;
};

struct mfs_file {
    uint32_t ino;
    uint64_t pos;
    uint32_t size;
    uint16_t flags;
    uint8_t  ref;
    uint8_t  pad;
    uint32_t dirty_hint;   /* extent actual */
    mf_t    *fs;
};

struct mfs_dir {
    mf_t *fs;
    uint32_t parent_ino;
    uint16_t idx;
    uint8_t  used;
};

/* ==== Sub-sistemas internos ==== */
/* crc32c / blake3 / crypto suites / lz4 / gear-cdc / fsst / exp3 / thermometer */
uint32_t mfs_crc32c(const uint8_t *buf, uint32_t len, uint32_t seed);
void     mfs_b3_256(const uint8_t *key, uint32_t keylen,
                    const uint8_t *in, uint32_t inlen, uint8_t out[32]);
void     mfs_b3_mac_trunc(const uint8_t key[32], const uint8_t *msg, uint32_t len,
                          uint8_t mac[8]);
void     mfs_b3_merkle_root(const uint8_t *leaves, uint32_t nleaf, uint32_t leaf_len,
                            uint8_t root_out[32]);

/* ==== SHA-256 streaming sin heap (FIPS 180-4) ==== */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t buflen;
} mfs_sha256_ctx;
void mfs_sha256_init(mfs_sha256_ctx *c);
void mfs_sha256_update(mfs_sha256_ctx *c, const uint8_t *in, uint32_t len);
void mfs_sha256_final(mfs_sha256_ctx *c, uint8_t out[32]);
void mfs_hmac_sha256(const uint8_t *key, uint32_t klen,
                     const uint8_t *msg, uint32_t mlen, uint8_t out[32]);

/* aprox. determinista de e^x en punto flotante simple (sin libm, §20.1 P1) */
static inline float expf_fast(float x)
{
    if (x > 10.0f) x = 10.0f;
    if (x < -10.0f) x = -10.0f;
    /* e^x ≈ (1 + x/1024)^1024 — 10 elevaciones al cuadrado, error < 0.05 % */
    float t = 1.0f + x / 1024.0f;
    for (int i = 0; i < 10; i++) t *= t;
    return t;
}
/* suites S0-S3 (AEAD cifrar/descifrar autenticado) src==dst permitido */
mfs_st   mfs_suite_seal(uint8_t suite, const uint8_t key[32],
                        uint64_t epoch_seq_nonce,
                        const uint8_t *in, uint16_t len,
                        uint8_t *out, uint16_t *olen /* in: len+16 tag */);
mfs_st   mfs_suite_open(uint8_t suite, const uint8_t key[32],
                        uint64_t epoch_seq_nonce,
                        const uint8_t *in, uint16_t clen,
                        uint8_t *out, uint16_t *olen);
uint16_t mfs_lz4_compress(const uint8_t *in, uint16_t ilen, uint8_t *out, uint16_t ospace);
uint16_t mfs_lz4_decompress(const uint8_t *in, uint16_t ilen, uint8_t *out, uint16_t ospace);
uint32_t mfs_gear_hash_init(void);
bool     mfs_gear_boundary(uint32_t *state, uint8_t b, uint32_t mask);
uint16_t mfs_fsst_encode(const uint8_t *in, uint16_t ilen, uint8_t *out, uint16_t ospace);
uint16_t mfs_fsst_decode(const uint8_t *in, uint16_t ilen, uint8_t *out, uint16_t ospace);
/* EXP3 determinista sembrado (§24.4): replay-determinista */
uint32_t mfs_exp3_update(float w[3], const float r[3], const float x[3][3],
                         uint32_t *rng, int n_arms);
/* TFC termométrico (§8.4): 1 bit por intervalo de 16 ciclos */
uint32_t mfs_tfc_count(const uint8_t *bits, uint32_t nbytes);
bool     mfs_tfc_will_carry(const uint8_t *bits, uint32_t nbytes);
void     mfs_tfc_increment(uint8_t *bits, uint32_t nbytes);

/* HAL/viabilidad (§5,§6) */
mfs_mode_t mfs_select_mode(const mfs_hwv_t *hwv, const mfs_config *cfg);
mfs_st     mfs_hal_detect(mf_t *fs, const mfs_config *cfg, mfs_hwv_t *out);
/* Bus read-only (§14.2 MFS-BUS-001..004) */
uint32_t   mfs_bus_measure_hz(const mfs_l2_driver *drv, void *ctx);

/* Flash helpers */
mfs_st mfs_read (mf_t *fs, uint32_t addr, void *dst, uint32_t len);
mfs_st mfs_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len); /* WOB barrier */
mfs_st mfs_erase(mf_t *fs, uint32_t addr);
mfs_st mfs_t0_read (mf_t *fs, uint32_t addr, void *dst, uint32_t len);
mfs_st mfs_t0_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len);
bool   mfs_has_t0(mf_t *fs);
bool   mfs_edp_capable(mf_t *fs);

/* Pipeline de datos por página (§10.1–§10.4, src/core/mfs_compact.c) */
mfs_st mfs_data_write(mf_t *fs, uint32_t lba, const uint8_t *pl, uint16_t len,
                      uint8_t hotness, uint8_t kind, uint32_t *ppage_out);
mfs_st mfs_data_read(mf_t *fs, uint32_t ppage, uint32_t lba,
                     uint8_t *buf, uint16_t bufsize, uint16_t *rlen);

/* Suite negociada (§10.6, src/core/mfs_hal.c) */
uint8_t mfs_negotiate_suite(const mfs_hwv_t *hwv, const mfs_config *cfg, mfs_mode_t mode);

/* SRB y CFX (§10.1/§10.4, src/core/mfs_compact.c) */
void     mfs_srb_attach(mf_t *fs);
void     mfs_cfx_reset(void);
int      mfs_cfx_lookup(uint64_t h64, uint32_t *ppage);
void     mfs_cfx_insert(uint64_t h64, uint32_t ppage);

/* E2G registro (§8.2) */
mfs_st mfs_rec_write(mf_t *fs, uint32_t zone, uint32_t lba, uint8_t kind,
                     uint8_t gen, uint8_t snapid, uint8_t hotness, uint8_t dictid,
                     const uint8_t *payload, uint16_t plen, uint32_t *ppage_out);
mfs_st mfs_rec_read (mf_t *fs, uint32_t ppage, uint32_t expect_lba, uint8_t expect_gen,
                     uint8_t *kind, uint8_t *gen, uint8_t *snapid,
                     uint8_t *buf, uint16_t bufsize, uint16_t *rlen);

/* Zonas/ZLF (§11.6, FSM §24.1) */
uint32_t mfs_page_bytes(mf_t *fs);           /* chunk del modo activo   */
uint16_t mfs_e2g_hdr(mf_t *fs);              /* cabecera E2G NOR/NAND   */
mfs_zone_t *mfs_zone(mf_t *fs, uint32_t id);
int   mfs_zone_alloc_open(mf_t *fs, uint8_t hotness);
mfs_st mfs_zone_seal(mf_t *fs, uint32_t id);
mfs_st mfs_gc_slice(mf_t *fs, uint32_t budget_us);   /* AGCB+ sobre GLD */
void   mfs_gld_add(mf_t *fs, uint32_t pages, bool meta);

/* WAL+/token (§9) */
mfs_st mfs_token_commit(mf_t *fs, uint32_t txid, uint32_t crc);
mfs_st mfs_wal_append(mf_t *fs, uint32_t lba, uint8_t kind,
                      const uint8_t *pl, uint16_t len, uint32_t *ppage);
mfs_st mfs_wal_replay(mf_t *fs, uint32_t window);    /* BMT acotado */

/* ==== helpers compartidos core (definidos en mfs_fs.c) ==== */
#define TOKRES 256u                          /* región anillo de tokens    */
uint32_t mfs_next_lba(mf_t *fs);             /* asignador LBA lógico       */
void     mfs_dab_init(mf_t *fs);             /* §24.4 EXP3 sembrado        */
void     mfs_dab_feedback(mf_t *fs, uint8_t arm, uint32_t latency_us);
uint8_t  mfs_dab_select_arm(mf_t *fs);       /* brazo por pesos w          */
float    mfs_dab_reward_latency(uint32_t us);/* r ∈ [0,1]                  */
mfs_st   mfs_snap_persist(mf_t *fs);         /* tabla de snaps a WAL (§10.8) */
extern mf_t *g_mfs_instance;                 /* instancia única (sp/fpt)   */
void     mfs_cusum_update(mf_t *fs, int16_t x_x100); /* §16 CUSUM        */
bool     mfs_cusum_alarm(const mf_t *fs);
void     mfs_edp_check(mf_t *fs);                /* §12.3 niveles 1-5    */
void     mfs_edp_drain(mf_t *fs);
int      mfs_lookup(mf_t *fs, const char *path, uint32_t *parent_out);
mfs_inode_ram_t *mfs_ino_get(mf_t *fs, uint32_t ino);
mfs_inode_ram_t *mfs_ino_alloc(mf_t *fs);
void     mfs_ino_evict(mf_t *fs, mfs_inode_ram_t *ino);
mfs_st   mfs_meta_flush(mf_t *fs, uint32_t ino); /* persistir inodo vía WAL */
uint32_t mfs_extent_read(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                         uint8_t *buf, uint16_t bufsize, uint16_t *rlen);
mfs_st   mfs_extent_write(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                          const uint8_t *buf, uint16_t len);
uint32_t mfs_zone_count_free(mf_t *fs);
void     mfs_gld_maybe_gc(mf_t *fs);             /* umbral deuda → slice */
extern bool g_mfs_edp_inject;                    /* hook de test vFlash  */

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
    MFS_EV_BOOT = 1, MFS_EV_POWER_LOSS, MFS_EV_EDP_DRAIN, MFS_EV_QUARANTINE,
    MFS_EV_E2G_FAIL, MFS_EV_SUITE_NEG, MFS_EV_DAB_CHANGE, MFS_EV_CUSUM_ALARM,
    MFS_EV_ELD_DEFER, MFS_EV_FPT, MFS_EV_SNAP, MFS_EV_PUF_FALLBACK,
    MFS_EV_NOTVIABLE, MFS_EV_ARCH_REJECT, MFS_EV_CONSERVATIVE, MFS_EV_MOUNT_OK
};

/* CBOR+COSE exportación (§15 Export) */
uint32_t mfs_cbor_health(const mfs_health_t *h, uint8_t *out, uint32_t cap);

/* Energy model (§13.3 ELD, §12.3 EDP: E_cap = ½·C·(V0²−Vmin²)) */
uint32_t mfs_energy_prog_mj(const mfs_hwv_t *hwv, uint32_t bytes);
uint32_t mfs_edp_window_us(const mfs_rail_state *r, uint16_t vmin_mv,
                           uint32_t cap_uf, uint32_t i_ma);

/* asserts de fase overlays (RSC §7.2, MFS-RES-003) */
#if defined(MFS_DEBUG)
#  define MFS_PHASE_ASSERT(cond) do { if (!(cond)) { mfs_assert_fail(__LINE__); } } while (0)
void mfs_assert_fail(int line);
#else
#  define MFS_PHASE_ASSERT(c) ((void)(c))
#endif

extern mf_t *g_mfs_instance; /* única instancia estática en targets pequeños */
extern mfs_hwv_t g_last_hwv;
extern mfs_mode_t g_last_selected;

#endif /* MATRIXFS_MFS_INTERNAL_H */
