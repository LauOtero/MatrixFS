/* matrixfs.h — MatrixFS Ultra «ATLAS» v1.0 — API pública completa (§21) */
#ifndef MATRIXFS_MATRIXFS_H
#define MATRIXFS_MATRIXFS_H

#include "mfs_types.h"
#include "mfs_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFS_VERSION_MAJOR 1
#define MFS_VERSION_MINOR 0
#define MFS_VERSION_EDITION_ATLAS 1

/* ==== Ciclo de vida (§21.1) ==== */
int  mf_init(mf_t *fs, const mfs_config *cfg);   /* monta; FSM §24.2, ≤12 ms @512MB */
int  mf_format(mf_t *fs, const void *opts);      /* opts may be NULL (defaults)     */
int  mf_sync(mf_t *fs);
int  mf_deinit(mf_t *fs);                       /* ceroización + sync final (§15 SEC-002) */

/* ==== POSIX-subset ==== */
int  mf_open(mf_t *fs, const char *path, uint32_t flags, mfs_file **f);
int  mf_close(mfs_file *f);
int  mf_read(mfs_file *f, void *buf, size_t len, size_t *rd);
int  mf_write(mfs_file *f, const void *buf, size_t len, size_t *wr);
int  mf_seek(mfs_file *f, int64_t off, int whence);
int  mf_tell(mfs_file *f, uint64_t *pos);
int  mf_stat(mf_t *fs, const char *path, mfs_stat *st);
int  mf_unlink(mf_t *fs, const char *path);
int  mf_rename(mf_t *fs, const char *from, const char *to);
int  mf_mkdir(mf_t *fs, const char *path);
int  mf_truncate(mfs_file *f, uint64_t size);

typedef struct {
    char     name[64];          /* nombre truncado a 63 chars + NUL */
    mfs_stat st;
} mfs_dirent;

int  mf_opendir(mf_t *fs, const char *path, mfs_dir **d);
int  mf_readdir(mfs_dir *d, mfs_dirent *de);
int  mf_closedir(mfs_dir *d);

/* ==== VIO y DAIO (§13.1) ==== */
int  mf_readv(mfs_file *f, const mfs_iovec *v, int n);
int  mf_writev(mfs_file *f, const mfs_iovec *v, int n);

typedef struct {
    uint32_t op;             /* MFS_AREAD / MFS_AWRITE */
    uint32_t deadline_us;
    uint16_t len;
    uint16_t class_flags;    /* (mfs_rt_class_t | flags) */
    void    *buf;            /* puntero a mfs_file* para ops de archivo */
    int32_t  status;         /* completado: mfs_st; pendiente: 0        */
    uint32_t file_off;
} mfs_iocb;

#define MFS_AREAD  1u
#define MFS_AWRITE 2u

int  mf_submit(mf_t *fs, mfs_iocb *cb);   /* ISR-safe (§21.3); nunca bloquea */
int  mf_poll(mf_t *fs, mfs_iocb **done, int max, uint32_t timeout_us);

/* ==== Transacciones (§9) ==== */
int  mf_tx_begin(mf_t *fs);
int  mf_tx_commit(mf_t *fs);
int  mf_tx_abort(mf_t *fs);

/* Savepoints anidados hasta profundidad del modo (§9.6) */
int  mf_sp_create(mf_t *fs, mfs_sp *sp);
int  mf_sp_rollback(mfs_sp sp);
int  mf_sp_release(mfs_sp sp);

/* ==== Snapshots y OTA (§10.8, MFS-Snap O(1)) ==== */
int  mf_snap_create(mf_t *fs, mfs_snap_id *id);
int  mf_snap_delete(mf_t *fs, mfs_snap_id id);
int  mf_snap_revert(mf_t *fs, mfs_snap_id id);   /* < 1 ms: re-fijar raíz */
int  mf_fpt_begin(mf_t *fs, mfs_fpt *h);
int  mf_fpt_apply(mfs_fpt h, const void *delta, size_t len); /* streaming */
int  mf_fpt_activate(mfs_fpt h);                /* swap de raíz ART + época */
int  mf_fpt_rollback(mfs_fpt h);                /* instantáneo, sin copia    */

/* ==== Salud y verificación ==== */
int  mf_ioctl(mf_t *fs, uint32_t cmd, void *arg);
int  mf_export_health(mf_t *fs, void *buf, size_t len);  /* CBOR+COSE_Sign1 */
int  mf_verify(mf_t *fs, mfs_verify_level lvl);

/* Estructura de salud exportada por MFS_IOCTL_HEALTH (§16.1) */
typedef struct {
    uint8_t  mode;
    uint8_t  suite;
    uint8_t  edp_state;      /* MONITOR/ARMED/DRAIN/DONE (§24.3) */
    uint8_t  dab_state;      /* FREEZE/EXPLOIT/EXPLORE           */
    uint16_t debt_gld;       /* deuda GLD actual                 */
    uint16_t free_pages;
    uint32_t epoch;
    uint64_t seq;
    uint32_t writes_prog;    /* páginas programadas (WAF numerador) */
    uint32_t writes_host;    /* páginas escritas por host (denominador) */
    uint32_t gc_relocated;
    uint32_t scrub_done;
    uint32_t read_retry;
    uint32_t quarantined_blocks;
    uint32_t e2g_failures;
    uint32_t crc_errors;
    uint32_t power_events;   /* cortes detectados en montaje */
    uint32_t edp_drains;
    uint32_t energy_mj_total;/* J/Op acumulada (miliJ)       */
    uint32_t ops_count;
    uint16_t temp_c;         /* última lectura TG            */
    uint16_t health_min;     /* salud mínima de bloque (0-1000) */
    uint32_t mount_time_us;  /* último montaje (BMT)         */
    uint32_t p999_commit_us; /* medido por HCT               */
    uint32_t snapshots_active;
    uint32_t fpt_events;
    uint32_t cusum_alarms;
    uint32_t dab_changes;
} mfs_health_t;

/* Estado interno expuesto solo para tests/formal (no API normativas) */
const mfs_hwv_t *mf_last_hwv(void);       /* último HWV analizado por viabilidad */
mfs_mode_t       mf_last_selected(void);  /* modo seleccionado o UNSUPPORTED     */

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_MATRIXFS_H */
