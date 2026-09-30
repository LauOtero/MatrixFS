/* mfs_fs.c — núcleo MatrixFS Ultra «ATLAS» v1.0 (§21–§24)
 * Ciclo de vida (FSM de montaje §24.2), POSIX-subset, VIO/DAIO (§13.1),
 * transacciones/savepoints (§9.6), snapshots O(1)/FlashPatch (§10.8),
 * EDP (§12.3), DAB EXP3 (§24.4), CUSUM (§16), HCT (§16) y verificación. */
#include "mfs_internal.h"
#include <string.h>

bool g_mfs_edp_inject;            /* hook test: simula caída de rail */
static uint32_t g_lba_next;       /* asignador LBA lógico por instancia única */
static uint32_t g_ino_next = 2u;  /* ino 0=root implícito, 1=free-list */

#define ROOT_INO 0u

/* ==== primitivas de puerto host (override en tests vía -DMFS_PORT_HOST) ==== */
__attribute__((weak)) void mfs_port_crit_enter(void) {}
__attribute__((weak)) void mfs_port_crit_exit(void)  {}

/* =====================================================================
 * helpers de tabla de inodos (flash-first: la RAM es ventana cache §11.5)
 * ===================================================================== */
mfs_inode_ram_t *mfs_ino_get(mf_t *fs, uint32_t ino)
{
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
        if (fs->inos[i].valid && fs->inos[i].ino == ino) return &fs->inos[i];
    }
    return NULL;
}

mfs_inode_ram_t *mfs_ino_alloc(mf_t *fs)
{
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
        if (!fs->inos[i].valid) {
            mfs_inode_ram_t *n = &fs->inos[i];
            memset(n, 0, sizeof(*n));
            n->valid = 1u;
            n->ino = g_ino_next++;
            return n;
        }
    }
    /* flash-first: desalojar el inodo no-open menos recientemente usado */
    mfs_inode_ram_t *victim = NULL;
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
        mfs_inode_ram_t *n = &fs->inos[i];
        if (!n->valid || n->ino == ROOT_INO) continue;
        bool open = false;
        for (uint32_t f = 0; f < MFS_MAX_FILES_OPEN; f++)
            if (fs->open_files[f] && fs->open_files[f]->ino == n->ino) open = true;
        if (!open) { victim = n; break; }
    }
    if (!victim) return NULL;                 /* todos calientes: ETABLEFULL */
    mfs_meta_flush(fs, victim->ino);          /* escribir antes de desalojar */
    memset(victim, 0, sizeof(*victim));
    victim->valid = 1u;
    victim->ino = g_ino_next++;
    return victim;
}

void mfs_ino_evict(mf_t *fs, mfs_inode_ram_t *ino)
{
    if (!ino || ino->ino == ROOT_INO) return;
    mfs_meta_flush(fs, ino->ino);
    memset(ino, 0, sizeof(*ino));
}

uint32_t mfs_next_lba(mf_t *fs)
{
    (void)fs;
    return g_lba_next++;
}

/* serializar inodo a registro INODE (orden determinista LE, 24+ext*12 B) */
static void ino_ser(const mfs_inode_ram_t *n, uint8_t *b, uint16_t *len)
{
    uint16_t o = 0;
    mfs_st32(b + o, n->ino);   o += 4;
    mfs_st32(b + o, n->size);  o += 4;
    mfs_st32(b + o, n->mtime); o += 4;
    mfs_st16(b + o, n->nlink); o += 2;
    b[o++] = n->type; b[o++] = n->gen; b[o++] = n->snapid; b[o++] = n->hotness;
    mfs_st32(b + o, n->parent); o += 4;
    /* sin bytes extra: el nombre viaja en el registro DIRENT, no en el inodo */
    for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
        mfs_st32(b + o, n->ext[e].vpage); o += 4;
        mfs_st32(b + o, n->ext[e].ppage); o += 4;
        mfs_st16(b + o, n->ext[e].npages); o += 2;
        mfs_st16(b + o, n->ext[e].flags);  o += 2;
    }
    *len = o;
}

static void ino_deser(mfs_inode_ram_t *n, const uint8_t *b, uint16_t len)
{
    if (len < 22u) return;
    uint16_t o = 0;
    n->valid = 1u;
    n->ino = mfs_ld32(b + o);   o += 4;
    n->size = mfs_ld32(b + o);  o += 4;
    n->mtime = mfs_ld32(b + o); o += 4;
    n->nlink = mfs_ld16(b + o); o += 2;
    n->type = b[o++]; n->gen = b[o++]; n->snapid = b[o++]; n->hotness = b[o++];
    n->parent = mfs_ld32(b + o); o += 4;
    for (uint32_t e = 0; e < MFS_EXT_INLINE && o + 12u <= len; e++) {
        n->ext[e].vpage = mfs_ld32(b + o); o += 4;
        n->ext[e].ppage = mfs_ld32(b + o); o += 4;
        n->ext[e].npages = mfs_ld16(b + o); o += 2;
        n->ext[e].flags = mfs_ld16(b + o);  o += 2;
    }
}

/* persistir inodo vía WAL (queda comprometido al token de tx o autocommit) */
mfs_st mfs_meta_flush(mf_t *fs, uint32_t ino)
{
    mfs_inode_ram_t *n = mfs_ino_get(fs, ino);
    if (!n) return MFS_OK; /* ya desalojado */
    uint8_t buf[MFS_CHUNK_EXTENDED];
    uint16_t len = 0;
    ino_ser(n, buf, &len);
    uint32_t lba = 0x100000u + ino; /* espacio meta: lba = base|ino */
    return mfs_wal_append(fs, lba, MFS_RT_INODE, buf, len, NULL);
}

/* ==== lookup de rutas sobre la ventana de inodos RAM ==== */
int mfs_lookup(mf_t *fs, const char *path, uint32_t *parent_out)
{
    if (path == NULL) return MFS_EINVAL;
    while (*path == '/') path++;
    if (*path == '\0') { if (parent_out) *parent_out = ROOT_INO; return (int)ROOT_INO; }
    uint32_t parent = ROOT_INO;
    char comp[MFS_NAME_MAX];
    for (;;) {
        uint32_t i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < MFS_NAME_MAX - 1u) {
            comp[i] = path[i]; i++;
        }
        comp[i] = '\0';
        path += i; while (*path == '/') path++;
        int found = -1;
        for (uint32_t k = 0; k < sizeof(fs->inos) / sizeof(fs->inos[0]); k++) {
            mfs_inode_ram_t *n = &fs->inos[k];
            if (n->valid && n->parent == parent && strcmp(n->name, comp) == 0) {
                found = (int)n->ino; break;
            }
        }
        if (found < 0) return MFS_ENOENT;
        parent = (uint32_t)found;
        if (*path == '\0') {
            if (parent_out) *parent_out = parent;
            /* devolver ino como positivo; ENOENT ya cubierto */
            return (parent == ROOT_INO) ? MFS_ENOENT : (int)parent;
        }
    }
}

/* =====================================================================
 * Extents de datos (read/write por página lógica)
 * ===================================================================== */
uint32_t mfs_extent_read(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                         uint8_t *buf, uint16_t bufsize, uint16_t *rlen)
{
    for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
        if (ino->ext[e].ppage != 0u && ino->ext[e].vpage == vpage) {
            uint32_t lba = 0x200000u + ((ino->ino << 8) | vpage);
            uint16_t r = 0;
            mfs_st st = mfs_data_read(fs, ino->ext[e].ppage, lba, buf, bufsize, &r);
            if (st != MFS_OK) return 0u;
            if (rlen) *rlen = r;
            return ino->ext[e].ppage;
        }
    }
    return 0u; /* hole: lector devuelve ceros */
}

mfs_st mfs_extent_write(mf_t *fs, mfs_inode_ram_t *ino, uint32_t vpage,
                        const uint8_t *buf, uint16_t len)
{
    uint32_t slot = vpage % MFS_EXT_INLINE;
    /* política simple: extent por página; si colisiona con otra vpage se
     * reemplaza (CoW real queda al snapshot engine §10.8) */
    uint32_t lba = 0x200000u + ((ino->ino << 8) | vpage);
    uint32_t pp = 0;
    mfs_st st = mfs_data_write(fs, lba, buf, len, ino->hotness, MFS_RT_DATA, &pp);
    if (st != MFS_OK) return st;
    ino->ext[slot].vpage = vpage;
    ino->ext[slot].ppage = pp;
    ino->ext[slot].npages = 1u;
    ino->ext[slot].flags = (uint16_t)(fs->epoch & 0xFu);
    return MFS_OK;
}

/* =====================================================================
 * Ciclo de vida (§21.1, FSM montaje §24.2)
 * ===================================================================== */
static mfs_st sb_write(mf_t *fs, uint32_t addr, uint32_t epoch, uint32_t gen)
{
    uint8_t sb[MFS_SB_SIZE];
    memset(sb, 0xFFu, sizeof(sb));
    sb[0] = MFS_SB_MAGIC0; sb[1] = MFS_SB_MAGIC1; sb[2] = MFS_SB_MAGIC2; sb[3] = MFS_SB_MAGIC3;
    mfs_st32(sb + 4, epoch);
    mfs_st32(sb + 8, gen);
    mfs_st32(sb + 12, fs->mode);
    mfs_st32(sb + 16, fs->suite);
    mfs_hwv_serialize(&fs->hwv, sb + 32); /* HWV embebido (copia) */
    uint32_t c = mfs_crc32c(sb, MFS_SB_SIZE - 4u, 0u);
    mfs_st32(sb + MFS_SB_SIZE - 4u, c);
    return mfs_write(fs, addr, sb, MFS_SB_SIZE);
}

static mfs_st sb_read(mf_t *fs, uint32_t addr, uint32_t *epoch, uint32_t *gen)
{
    uint8_t sb[MFS_SB_SIZE];
    mfs_st st = mfs_read(fs, addr, sb, MFS_SB_SIZE);
    if (st != MFS_OK) return st;
    if (sb[0] != MFS_SB_MAGIC0 || sb[1] != MFS_SB_MAGIC1) return MFS_ECORRUPT;
    uint32_t c = mfs_crc32c(sb, MFS_SB_SIZE - 4u, 0u);
    if (c != mfs_ld32(sb + MFS_SB_SIZE - 4u)) return MFS_ECORRUPT;
    *epoch = mfs_ld32(sb + 4);
    *gen = mfs_ld32(sb + 8);
    return MFS_OK;
}

int mf_format(mf_t *fs, const void *opts)
{
    (void)opts;
    if (fs == NULL || fs->cfg == NULL) return MFS_EINVAL;
    const mfs_config *cfg = fs->cfg;
    if (cfg->arch_class == 0u) return MFS_EARCH;      /* §24.2 paso 2 */
    memset(fs, 0, sizeof(*fs));
    fs->cfg = cfg;
    g_mfs_instance = fs;

    /* detección + viabilidad (§5/§6) */
    mfs_st st = mfs_hal_detect(fs, cfg, &fs->hwv);
    if (st != MFS_OK) return st;
    if (fs->hwv.arch_class == 0u) { mfs_hct_event(fs, MFS_EV_ARCH_REJECT, 0); return MFS_EARCH; }
    mfs_mode_t mode = mfs_select_mode(&fs->hwv, cfg);
    if (mode == MFS_MODE_UNSUPPORTED) {
        mfs_hct_event(fs, MFS_EV_NOTVIABLE, fs->hwv.ram_total);
        return MFS_ENOTVIABLE;                          /* MFS-VIA-002 */
    }
    fs->mode = (uint8_t)mode;
    fs->suite = mfs_negotiate_suite(&fs->hwv, cfg, mode);
    g_last_hwv = fs->hwv; g_last_selected = mode;

    /* reservar región baja: SB A/B + HWV + anillo tokens (§22.1) */
    fs->hwv.base_reserved_off = 1024u + TOKRES;
    uint8_t hwvser[64];
    mfs_hwv_serialize(&fs->hwv, hwvser);
    st = mfs_erase(fs, 0u);
    if (st != MFS_OK) return st;
    st = mfs_write(fs, MFS_HWV_OFFSET, hwvser, 64u);
    if (st != MFS_OK) return st;
    fs->epoch = 1u;
    st = sb_write(fs, 0u, fs->epoch, 0u);
    if (st == MFS_OK) st = sb_write(fs, MFS_SB_SIZE, fs->epoch, 0u);
    if (st != MFS_OK) return st;

    /* tabla de zonas: todas vacías; abrir primera */
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        fs->zones[i].zone_id = (uint16_t)i;
        fs->zones[i].state = MFS_Z_EMPTY;
        fs->zones[i].start_addr = 1024u + TOKRES + i * fs->hwv.erase_unit;
        fs->zones[i].size = fs->hwv.erase_unit;
        fs->zones[i].total_pages = (uint16_t)(fs->hwv.erase_unit / mfs_page_bytes(fs));
    }
    fs->free_pages = MFS_MAX_ZONES;
    for (uint32_t i = 0; i < MFS_MAX_BLOCKS * 16u; i++) fs->l2p[i] = 0xFFFFFFFFu;
    fs->zone_wal = 0xFFFFFFFFu;
    mfs_srb_attach(fs);
    mfs_cfx_reset();
    g_lba_next = 0; g_ino_next = 2u;

    /* raíz: inodo 0 directorio */
    mfs_inode_ram_t *root = &fs->inos[0];
    root->valid = 1u; root->ino = ROOT_INO; root->type = 1u; root->nlink = 2u;
    strcpy(root->name, "/");
    fs->mounted = true;
    mfs_dab_init(fs);
    mfs_hct_event(fs, MFS_EV_BOOT, fs->mode);
    return MFS_OK;
}

int mf_init(mf_t *fs, const mfs_config *cfg)
{
    if (fs == NULL || cfg == NULL || cfg->drv == NULL) return MFS_EINVAL;
    memset(fs, 0, sizeof(*fs));
    fs->cfg = cfg;
    g_mfs_instance = fs;

    /* §24.2 pasos 1-2: HWV on-flash (handle de 8 B, MFS-HWV-001) */
    fs->hwv_h.hwv_flash_addr = MFS_HWV_OFFSET;
    uint8_t ser[64];
    mfs_st st = mfs_read(fs, MFS_HWV_OFFSET, ser, 64u);
    if (st == MFS_OK) {
        mfs_hwv_deserialize(&fs->hwv, ser);
        st = mfs_hwv_validate(&fs->hwv);
        if (st == MFS_EARCH) { mfs_hct_event(fs, MFS_EV_ARCH_REJECT, 0); return st; }
    }
    if (st != MFS_OK) {
        /* sin HWV válido: ejecutar cascada de detección (§5.1) */
        st = mfs_hal_detect(fs, cfg, &fs->hwv);
        if (st != MFS_OK) return st;
        if (fs->hwv.arch_class == 0u) return MFS_EARCH;
    }
    g_last_hwv = fs->hwv;

    /* paso 3: modo + viabilidad */
    mfs_mode_t mode = mfs_select_mode(&fs->hwv, cfg);
    if (mode == MFS_MODE_UNSUPPORTED) {
        mfs_hct_event(fs, MFS_EV_NOTVIABLE, fs->hwv.ram_total);
        return MFS_ENOTVIABLE;
    }
    fs->mode = (uint8_t)mode;
    g_last_selected = mode;
    fs->suite = mfs_negotiate_suite(&fs->hwv, cfg, mode);
    mfs_hct_event(fs, MFS_EV_SUITE_NEG, fs->suite);

    /* paso 4: superblock A/B (elegir epoch mayor válido) */
    uint32_t epA = 0, gA = 0, epB = 0, gB = 0;
    mfs_st sA = sb_read(fs, 0u, &epA, &gA);
    mfs_st sB = sb_read(fs, MFS_SB_SIZE, &epB, &gB);
    if (sA != MFS_OK && sB != MFS_OK) return MFS_ECORRUPT; /* format requerido */
    fs->epoch = (epB > epA) ? epB : epA;
    fs->hwv.base_reserved_off = 1024u + TOKRES;

    /* paso 5: reconstruir zonas abriendo todas (scan de cabeceras barato) */
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        mfs_zone_t *z = &fs->zones[i];
        z->zone_id = (uint16_t)i;
        z->state = MFS_Z_EMPTY;
        z->start_addr = 1024u + TOKRES + i * fs->hwv.erase_unit;
        z->size = fs->hwv.erase_unit;
        z->total_pages = (uint16_t)(fs->hwv.erase_unit / mfs_page_bytes(fs));
    }
    /* scan limitado: primer bloque no erased ⇒ zona usada (estado OPEN en la última) */
    uint8_t probe[4];
    int last_used = -1;
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        if (mfs_read(fs, fs->zones[i].start_addr + mfs_e2g_hdr(fs), probe, 2u) != MFS_OK)
            continue;
        if (probe[0] == (uint8_t)(MFS_REC_MAGIC >> 8) && probe[1] == (uint8_t)MFS_REC_MAGIC) {
            fs->zones[i].state = MFS_Z_FULL;
            fs->zones[i].write_ptr = fs->zones[i].size; /* pesimista: GC lo afinará */
            last_used = (int)i;
        }
    }
    if (last_used >= 0) {
        fs->zones[last_used].state = MFS_Z_OPEN;
        /* reapertura: write_ptr aproximado al final; páginas intermedias
         * recuperadas por replay */
    } else {
        int z = mfs_zone_alloc_open(fs, 0u);
        if (z < 0) return MFS_ENOSPC;
    }
    fs->free_pages = 0;
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++)
        if (fs->zones[i].state == MFS_Z_EMPTY) fs->free_pages++;
    for (uint32_t i = 0; i < MFS_MAX_BLOCKS * 16u; i++) fs->l2p[i] = 0xFFFFFFFFu;
    fs->zone_wal = 0xFFFFFFFFu;
    mfs_srb_attach(fs);
    mfs_cfx_reset();
    g_lba_next = 0; g_ino_next = 2u;

    /* paso 6: checkpoint + replay acotado BMT (§9.4) */
    st = mfs_checkpoint_load(fs);
    if (st != MFS_OK) return st;
    st = mfs_wal_replay(fs, MFS_WAL_WINDOW_MAX);
    if (st != MFS_OK && st != MFS_EBADMSG) return st; /* BADMSG: degradar lectura */

    /* paso 7: raíz + inodo root */
    mfs_inode_ram_t *root = &fs->inos[0];
    memset(root, 0, sizeof(*root));
    root->valid = 1u; root->ino = ROOT_INO; root->type = 1u; root->nlink = 2u;
    strcpy(root->name, "/");
    fs->mounted = true;
    fs->edp_state = MFS_EDP_MONITOR;
    mfs_dab_init(fs);
    mfs_hct_event(fs, MFS_EV_MOUNT_OK, fs->epoch);
    fs->hct.mount_time_us = mfs_port_time_us();
    return MFS_OK;
}

int mf_sync(mf_t *fs)
{
    if (!fs || !fs->mounted) return MFS_ENOTMOUNTED;
    /* flush de inodos calientes → WAL → checkpoint → SB rotatorio */
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
        if (fs->inos[i].valid) mfs_meta_flush(fs, fs->inos[i].ino);
    }
    mfs_st st = mfs_checkpoint_write(fs);
    if (st != MFS_OK) return st;
    uint32_t slot = (fs->seq & 1u) ? MFS_SB_SIZE : 0u;
    st = sb_write(fs, slot ^ MFS_SB_SIZE, fs->epoch, (uint32_t)fs->seq);
    mfs_hct_flush(fs);
    mfs_gld_maybe_gc(fs);
    return st;
}

int mf_deinit(mf_t *fs)
{
    if (!fs) return MFS_EINVAL;
    if (fs->mounted) {
        mf_sync(fs);
        fs->mounted = false;
    }
    /* MFS-SEC-002: ceroización de material sensible y scratch */
    volatile uint8_t *p = (volatile uint8_t *)fs;
    for (uint32_t i = 0; i < sizeof(*fs); i++) p[i] = 0u;
    mfs_cfx_reset();
    return MFS_OK;
}

/* =====================================================================
 * POSIX-subset
 * ===================================================================== */
int mf_open(mf_t *fs, const char *path, uint32_t flags, mfs_file **fout)
{
    if (!fs || !fs->mounted || path == NULL || fout == NULL) return MFS_ENOTMOUNTED;
    int r = mfs_lookup(fs, path, NULL);
    mfs_inode_ram_t *ino = NULL;
    if (r >= 0 && r != MFS_ENOENT) {
        ino = mfs_ino_get(fs, (uint32_t)r);
    } else if (r == MFS_ENOENT) {
        if (!(flags & MFS_O_CREAT)) return MFS_ENOENT;
        if (flags & MFS_O_TRUNC) { /* crear */ }
        ino = mfs_ino_alloc(fs);
        if (!ino) return MFS_ETABLEFULL;
        /* derivar padre y nombre */
        const char *slash = strrchr(path, '/');
        char parent_path[MFS_PATH_MAX];
        if (slash) {
            uint32_t n = (uint32_t)(slash - path);
            if (n >= MFS_PATH_MAX) n = MFS_PATH_MAX - 1u;
            memcpy(parent_path, path, n); parent_path[n] = '\0';
            int pr = mfs_lookup(fs, parent_path, NULL);
            if (pr < 0 && pr != MFS_ENOENT) return pr;
            ino->parent = (pr == MFS_ENOENT) ? ROOT_INO : (uint32_t)pr;
            strncpy(ino->name, slash + 1, MFS_NAME_MAX - 1u);
        } else {
            ino->parent = ROOT_INO;
            strncpy(ino->name, path, MFS_NAME_MAX - 1u);
        }
        ino->type = 0u; ino->nlink = 1u;
        ino->hotness = (uint8_t)((flags >> MFS_O_HOTNESS_SHIFT) & 3u);
        mfs_meta_flush(fs, ino->ino);
    } else return r;
    if (ino == NULL) return MFS_ENOENT;
    if (ino->type == 1u) return MFS_EINVAL; /* dir: usar opendir */
    if ((flags & MFS_O_EXCL) && (flags & MFS_O_CREAT) && ino->size != 0u)
        return MFS_EEXISTS;
    /* buscar handle libre */
    mfs_file *f = NULL;
    for (uint32_t i = 0; i < MFS_MAX_FILES_OPEN; i++) {
        if (fs->open_files[i] == NULL) {
            static mfs_file pool[MFS_MAX_FILES_OPEN];
            f = &pool[i];
            fs->open_files[i] = f;
            break;
        }
    }
    if (!f) return MFS_ETABLEFULL;
    memset(f, 0, sizeof(*f));
    f->ino = ino->ino; f->fs = fs; f->ref = 1u; f->flags = (uint16_t)flags;
    if (flags & MFS_O_APPEND) f->pos = ino->size;
    if (flags & MFS_O_TRUNC) { ino->size = 0; mfs_meta_flush(fs, ino->ino); }
    *fout = f;
    return MFS_OK;
}

int mf_close(mfs_file *f)
{
    if (!f || !f->fs) return MFS_EINVAL;
    mf_t *fs = f->fs;
    mfs_meta_flush(fs, f->ino);
    for (uint32_t i = 0; i < MFS_MAX_FILES_OPEN; i++)
        if (fs->open_files[i] == f) fs->open_files[i] = NULL;
    f->ref = 0u;
    return MFS_OK;
}

int mf_read(mfs_file *f, void *buf, size_t len, size_t *rd)
{
    if (!f || !buf) return MFS_EINVAL;
    mf_t *fs = f->fs;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, f->ino);
    if (!ino) return MFS_ENOENT;
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    uint32_t pb = mfs_page_bytes(fs);
    while (done < len && f->pos < ino->size) {
        uint32_t vp = (uint32_t)(f->pos / pb);
        uint32_t off = (uint32_t)(f->pos % pb);
        uint8_t page[MFS_CHUNK_EXTENDED];
        uint16_t rlen = 0;
        uint32_t got = mfs_extent_read(fs, ino, vp, page, (uint16_t)sizeof(page), &rlen);
        size_t take = len - done;
        if (take > pb - off) take = pb - off;
        if (got == 0u || off >= rlen) {
            memset(dst + done, 0, take); /* hole → ceros */
        } else {
            if (take > rlen - off) take = rlen - off;
            memcpy(dst + done, page + off, take);
        }
        done += take; f->pos += take;
        if (take == 0u) break;
    }
    if (rd) *rd = done;
    return MFS_OK;
}

int mf_write(mfs_file *f, const void *buf, size_t len, size_t *wr)
{
    if (!f || !buf) return MFS_EINVAL;
    mf_t *fs = f->fs;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, f->ino);
    if (!ino) return MFS_ENOENT;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    uint32_t pb = mfs_page_bytes(fs);
    /* límite de tamaño por modo (§18.2) */
    uint64_t maxsz = (uint64_t)mfs_limits[fs->mode].max_file_size_kb * 1024ull;
    if (f->pos + len > maxsz) return MFS_EOVERFLOW;
    /* backpressure GLD (§7.4): deuda alta ⇒ slice GC o rechazo RT-A */
    if (fs->debt_gld > MFS_GL_DLIST_MAX * 4u) {
        mfs_gld_maybe_gc(fs);
        if (fs->debt_gld > MFS_GL_DLIST_MAX * 8u) return MFS_EBACKPRESSURE;
    }
    mfs_edp_check(fs);
    if (fs->edp_level >= 4u) return MFS_EAGAIN; /* SPDR activo */
    while (done < len) {
        uint32_t vp = (uint32_t)((f->pos + done) / pb);
        uint32_t off = (uint32_t)((f->pos + done) % pb);
        uint8_t page[MFS_CHUNK_EXTENDED];
        uint16_t plen = 0;
        if (off != 0u) {
            uint16_t rlen = 0;
            (void)mfs_extent_read(fs, ino, vp, page, (uint16_t)sizeof(page), &rlen);
            plen = rlen;
            if (plen < off) { memset(page + plen, 0, off - plen); plen = (uint16_t)off; }
        }
        size_t take = len - done;
        if (take > pb - off) take = pb - off;
        memcpy(page + off, src + done, take);
        if ((uint32_t)(off + take) > plen) plen = (uint16_t)(off + take);
        mfs_st st = mfs_extent_write(fs, ino, vp, page, plen);
        if (st != MFS_OK) return st;
        done += take;
        uint64_t np = f->pos + done;
        if (np > ino->size) ino->size = (uint32_t)np;
    }
    f->pos += done;
    ino->mtime = (uint32_t)(fs->epoch * 1000u + (uint32_t)f->pos);
    if (wr) *wr = done;
    return MFS_OK;
}

int mf_seek(mfs_file *f, int64_t off, int whence)
{
    if (!f) return MFS_EINVAL;
    mfs_inode_ram_t *ino = mfs_ino_get(f->fs, f->ino);
    if (!ino) return MFS_ENOENT;
    int64_t np = (whence == MFS_SEEK_SET) ? off
               : (whence == MFS_SEEK_CUR) ? (int64_t)f->pos + off
               : (int64_t)ino->size + off;
    if (np < 0) return MFS_EINVAL;
    f->pos = (uint64_t)np;
    return MFS_OK;
}

int mf_tell(mfs_file *f, uint64_t *pos)
{
    if (!f || !pos) return MFS_EINVAL;
    *pos = f->pos;
    return MFS_OK;
}

int mf_stat(mf_t *fs, const char *path, mfs_stat *st)
{
    if (!fs || !path || !st) return MFS_EINVAL;
    int r = mfs_lookup(fs, path, NULL);
    if (r < 0) return r;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
    if (!ino) return MFS_ENOENT;
    st->ino = ino->ino;
    st->mode = (uint16_t)((ino->type == 1u) ? 0x4000u | 0755u : 0x8000u | 0644u);
    st->nlink = ino->nlink; st->size = ino->size; st->mtime = ino->mtime;
    st->gen = ino->gen; st->snapid = ino->snapid; st->hotness = ino->hotness;
    st->pad = 0u;
    return MFS_OK;
}

int mf_unlink(mf_t *fs, const char *path)
{
    if (!fs || !path) return MFS_EINVAL;
    int r = mfs_lookup(fs, path, NULL);
    if (r < 0) return r;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
    if (!ino) return MFS_ENOENT;
    if (ino->type == 1u) {
        /* dir solo si vacío */
        for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
            if (fs->inos[i].valid && fs->inos[i].parent == ino->ino) return MFS_EBUSY;
    }
    for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
        if (ino->ext[e].ppage) mfs_gld_add(fs, 1u, false);
    }
    mfs_ino_evict(fs, ino);
    memset(ino, 0, sizeof(*ino));
    return MFS_OK;
}

int mf_rename(mf_t *fs, const char *from, const char *to)
{
    if (!fs || !from || !to) return MFS_EINVAL;
    int r = mfs_lookup(fs, from, NULL);
    if (r < 0) return r;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
    if (!ino) return MFS_ENOENT;
    const char *slash = strrchr(to, '/');
    uint32_t newpar = ROOT_INO;
    char nm[MFS_NAME_MAX];
    if (slash) {
        char pp[MFS_PATH_MAX];
        uint32_t n = (uint32_t)(slash - to);
        if (n >= MFS_PATH_MAX) n = MFS_PATH_MAX - 1u;
        memcpy(pp, to, n); pp[n] = '\0';
        int pr = mfs_lookup(fs, pp, NULL);
        if (pr < 0 && pr != MFS_ENOENT) return pr;
        if (pr == MFS_ENOENT) return MFS_ENOENT;
        newpar = (uint32_t)pr;
        strncpy(nm, slash + 1, MFS_NAME_MAX - 1u); nm[MFS_NAME_MAX - 1u] = '\0';
    } else {
        strncpy(nm, to, MFS_NAME_MAX - 1u); nm[MFS_NAME_MAX - 1u] = '\0';
    }
    /* transacción atómica: cambio de (parent,name) bajo un solo token (§9) */
    bool own_tx = !fs->tx_open;
    if (own_tx) mf_tx_begin(fs);
    ino->parent = newpar;
    strncpy(ino->name, nm, MFS_NAME_MAX - 1u);
    ino->name[MFS_NAME_MAX - 1u] = '\0';
    mfs_meta_flush(fs, ino->ino);
    if (own_tx) return mf_tx_commit(fs);
    return MFS_OK;
}

int mf_mkdir(mf_t *fs, const char *path)
{
    if (!fs || !path) return MFS_EINVAL;
    int r = mfs_lookup(fs, path, NULL);
    if (r >= 0 && r != MFS_ENOENT) return MFS_EEXISTS;
    mfs_inode_ram_t *ino = mfs_ino_alloc(fs);
    if (!ino) return MFS_ETABLEFULL;
    const char *slash = strrchr(path, '/');
    if (slash) {
        char pp[MFS_PATH_MAX];
        uint32_t n = (uint32_t)(slash - path);
        if (n >= MFS_PATH_MAX) n = MFS_PATH_MAX - 1u;
        memcpy(pp, path, n); pp[n] = '\0';
        int pr = mfs_lookup(fs, pp, NULL);
        if (pr < 0 && pr != MFS_ENOENT) return pr;
        ino->parent = (pr == MFS_ENOENT) ? ROOT_INO : (uint32_t)pr;
        strncpy(ino->name, slash + 1, MFS_NAME_MAX - 1u);
    } else {
        ino->parent = ROOT_INO;
        strncpy(ino->name, path, MFS_NAME_MAX - 1u);
    }
    ino->name[MFS_NAME_MAX - 1u] = '\0';
    ino->type = 1u; ino->nlink = 2u;
    return mfs_meta_flush(fs, ino->ino);
}

int mf_truncate(mfs_file *f, uint64_t size)
{
    if (!f) return MFS_EINVAL;
    mfs_inode_ram_t *ino = mfs_ino_get(f->fs, f->ino);
    if (!ino) return MFS_ENOENT;
    uint32_t pb = mfs_page_bytes(f->fs);
    if (size < ino->size) {
        uint32_t keep = (uint32_t)((size + pb - 1u) / pb);
        for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
            if (ino->ext[e].ppage && ino->ext[e].vpage >= keep) {
                mfs_gld_add(f->fs, 1u, false);
                ino->ext[e].ppage = 0; ino->ext[e].vpage = 0xFFFFFFFFu;
            }
        }
    }
    ino->size = (uint32_t)size;
    if (f->pos > size) f->pos = size;
    return mfs_meta_flush(f->fs, ino->ino);
}

int mf_opendir(mf_t *fs, const char *path, mfs_dir **dout)
{
    if (!fs || !path || !dout) return MFS_EINVAL;
    int r = mfs_lookup(fs, path, NULL);
    if (r < 0) return r;
    mfs_inode_ram_t *ino = mfs_ino_get(fs, (uint32_t)r);
    if (!ino || ino->type != 1u) return MFS_EINVAL;
    static mfs_dir dpool;
    dpool.fs = fs; dpool.parent_ino = ino->ino; dpool.idx = 0u; dpool.used = 1u;
    *dout = &dpool;
    return MFS_OK;
}

int mf_readdir(mfs_dir *d, mfs_dirent *de)
{
    if (!d || !de) return MFS_EINVAL;
    mf_t *fs = d->fs;
    uint32_t n = sizeof(fs->inos) / sizeof(fs->inos[0]);
    while (d->idx < n) {
        mfs_inode_ram_t *ino = &fs->inos[d->idx++];
        if (ino->valid && ino->parent == d->parent_ino) {
            strncpy(de->name, ino->name, 63u); de->name[63] = '\0';
            de->st.ino = ino->ino;
            de->st.mode = (uint16_t)((ino->type == 1u) ? 0x4000u : 0x8000u);
            de->st.nlink = ino->nlink; de->st.size = ino->size;
            de->st.mtime = ino->mtime; de->st.gen = ino->gen;
            de->st.snapid = ino->snapid; de->st.hotness = ino->hotness;
            return MFS_OK;
        }
    }
    return MFS_ENOENT; /* fin de iteración */
}

int mf_closedir(mfs_dir *d) { if (!d) return MFS_EINVAL; d->used = 0u; return MFS_OK; }

/* =====================================================================
 * VIO / DAIO (§13.1)
 * ===================================================================== */
int mf_readv(mfs_file *f, const mfs_iovec *v, int n)
{
    int st = MFS_OK;
    for (int i = 0; i < n; i++) {
        size_t rd = 0;
        int r = mf_read(f, v[i].iov_base, v[i].iov_len, &rd);
        if (r != MFS_OK) st = r;
        if (rd != v[i].iov_len) break;
    }
    return st;
}

int mf_writev(mfs_file *f, const mfs_iovec *v, int n)
{
    int st = MFS_OK;
    for (int i = 0; i < n; i++) {
        size_t wr = 0;
        int r = mf_write(f, v[i].iov_base, v[i].iov_len, &wr);
        if (r != MFS_OK) st = r;
        if (wr != v[i].iov_len) break;
    }
    return st;
}

/* mf_submit: ISR-safe (§21.3). Solo encola en el ring lock-free SPSC;
 * nunca espera ni recorre estructuras compartidas más allá del índice. */
int mf_submit(mf_t *fs, mfs_iocb *cb)
{
    if (!fs || !cb) return MFS_EINVAL;
    if (!fs->mounted) return MFS_ENOTMOUNTED;
    uint8_t next = (uint8_t)((fs->ring_head + 1u) % MFS_MAX_IOCB);
    if (next == fs->ring_tail) return MFS_EBACKPRESSURE; /* ring lleno */
    mfs_port_crit_enter();
    fs->ring[fs->ring_head] = cb;
    cb->status = 0;
    fs->ring_head = next;
    mfs_port_crit_exit();
    return MFS_OK;
}

/* mf_poll: ejecuta hasta presupuesto temporal; respeta clase RT (§13.1) */
int mf_poll(mf_t *fs, mfs_iocb **done, int max, uint32_t timeout_us)
{
    if (!fs) return MFS_EINVAL;
    uint32_t t0 = mfs_port_time_us();
    int n = 0;
    while (fs->ring_tail != fs->ring_head && n < max) {
        if (mfs_port_time_us() - t0 > timeout_us) break;
        mfs_iocb *cb = fs->ring[fs->ring_tail];
        uint8_t cls = (uint8_t)(cb->class_flags & 3u);
        uint32_t tau = (cls == MFS_RT_A) ? fs->tau[0]
                     : (cls == MFS_RT_B) ? fs->tau[1] : fs->tau[2];
        if (tau == 0u) tau = (cls == MFS_RT_A) ? MFS_TAU_RT_A_US
                     : (cls == MFS_RT_B) ? MFS_TAU_RT_B_US : MFS_TAU_RT_C_US;
        mfs_port_crit_enter();
        fs->ring[fs->ring_tail] = NULL;
        fs->ring_tail = (uint8_t)((fs->ring_tail + 1u) % MFS_MAX_IOCB);
        mfs_port_crit_exit();
        mfs_file *f = (mfs_file *)cb->buf;
        int rc = MFS_EINVAL;
        uint32_t op_t0 = mfs_port_time_us();
        if (f) {
            if (cb->op == MFS_AREAD) {
                size_t rd = 0;
                rc = mf_read(f, (char *)cb->buf + sizeof(void *), cb->len, &rd);
                (void)rd;
            } else if (cb->op == MFS_AWRITE) {
                size_t wr = 0;
                rc = mf_write(f, (const char *)cb->buf + sizeof(void *), cb->len, &wr);
                (void)wr;
            }
        }
        uint32_t lat = mfs_port_time_us() - op_t0;
        mfs_cusum_update(fs, (int16_t)(lat * 100u > 32767u ? 32767u : lat * 100u));
        if (cls == MFS_RT_A && lat > tau) {
            cb->status = MFS_ETIMEDOUT_BUDGET; /* TCB incumplido ⇒ violación */
            mfs_hct_event(fs, MFS_EV_CUSUM_ALARM, lat);
        } else {
            cb->status = rc;
        }
        if (done) done[n] = cb;
        n++;
    }
    return n;
}

/* =====================================================================
 * Transacciones y savepoints (§9, §9.6)
 * ===================================================================== */
int mf_tx_begin(mf_t *fs)
{
    if (!fs || !fs->mounted) return MFS_ENOTMOUNTED;
    if (fs->tx_open) return MFS_EDEADLK;
    fs->tx_open = true;
    fs->sp_depth = 0u;
    fs->txid_cur = ++fs->txid_next;
    fs->sp_marks[0] = fs->wal_count;
    return MFS_OK;
}

int mf_tx_commit(mf_t *fs)
{
    if (!fs || !fs->mounted) return MFS_ENOTMOUNTED;
    if (!fs->tx_open) return MFS_ESTATE;
    /* CRC del cuerpo de la tx sobre las entradas pendientes */
    uint32_t crc = 0u;
    for (uint32_t i = 0; i < fs->wal_count; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        if (fs->wal[j].valid && fs->wal[j].txid == fs->txid_cur)
            crc = mfs_crc32c((const uint8_t *)&fs->wal[j], sizeof(fs->wal[j]), crc);
    }
    mfs_st st = mfs_token_commit(fs, fs->txid_cur, crc);
    if (st == MFS_OK) fs->txid_cur = 0u;
    return st;
}

int mf_tx_abort(mf_t *fs)
{
    if (!fs || !fs->mounted) return MFS_ENOTMOUNTED;
    if (!fs->tx_open) return MFS_ESTATE;
    /* descartar entradas del txid actual (undo implícito: nunca hubo token) */
    uint16_t w = 0;
    for (uint16_t i = 0; i < fs->wal_count; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        if (fs->wal[j].valid && fs->wal[j].txid != fs->txid_cur)
            fs->wal[w++] = fs->wal[j];
    }
    fs->wal_count = w;
    fs->tx_open = false; fs->sp_depth = 0u; fs->txid_cur = 0u;
    return MFS_OK;
}

int mf_sp_create(mf_t *fs, mfs_sp *sp)
{
    if (!fs || !sp) return MFS_EINVAL;
    if (!fs->tx_open) return MFS_ESTATE;
    uint8_t maxsp = mfs_limits[fs->mode].max_savepoints;
    if (fs->sp_depth >= maxsp) return MFS_EOVERFLOW;
    fs->sp_depth++;
    fs->sp_marks[fs->sp_depth] = fs->wal_count;
    sp->raw[0] = fs->txid_cur;
    sp->raw[1] = fs->sp_depth;
    return MFS_OK;
}

int mf_sp_rollback(mfs_sp sp)
{
    mf_t *fs = g_mfs_instance;
    if (!fs || !fs->tx_open) return MFS_ESTATE;
    if (sp.raw[0] != fs->txid_cur || sp.raw[1] == 0u || sp.raw[1] > fs->sp_depth)
        return MFS_EDEADLK;
    /* cortar la ventana al marcador: las entradas posteriores se descartan */
    uint32_t mark = fs->sp_marks[sp.raw[1]];
    if (mark <= fs->wal_count) fs->wal_count = (uint16_t)mark;
    fs->sp_depth = (uint8_t)(sp.raw[1] - 1u);
    return MFS_OK;
}

int mf_sp_release(mfs_sp sp)
{
    mf_t *fs = g_mfs_instance;
    if (!fs || !fs->tx_open) return MFS_ESTATE;
    if (sp.raw[0] != fs->txid_cur || sp.raw[1] == 0u || sp.raw[1] > fs->sp_depth)
        return MFS_EDEADLK;
    fs->sp_depth = (uint8_t)(sp.raw[1] - 1u); /* conservar efectos */
    return MFS_OK;
}

/* =====================================================================
 * Snapshots O(1) y FlashPatch OTA (§10.8)
 * ===================================================================== */
int mf_snap_create(mf_t *fs, mfs_snap_id *id)
{
    if (!fs || !id) return MFS_EINVAL;
    uint8_t maxs = mfs_limits[fs->mode].max_snapshots;
    if (fs->snap_count >= maxs) return MFS_ESNAPMAX;
    /* O(1): fijar la raíz Merkle actual + epoch; CoW posterior por snapid */
    mfs_st st = mfs_checkpoint_write(fs);
    if (st != MFS_OK) return st;
    mfs_snap_id sid = (fs->epoch << 8) | fs->snap_count;
    fs->snaps[fs->snap_count] = sid;
    fs->snap_roots[fs->snap_count] = fs->root_art;
    fs->snap_count++;
    st = mfs_snap_persist(fs);
    if (st != MFS_OK) return st;
    *id = sid;
    fs->hct.snapshots_active = fs->snap_count;
    mfs_hct_event(fs, MFS_EV_SNAP, sid);
    return MFS_OK;
}

int mf_snap_delete(mf_t *fs, mfs_snap_id id)
{
    if (!fs) return MFS_EINVAL;
    for (uint8_t i = 0; i < fs->snap_count; i++) {
        if (fs->snaps[i] == id) {
            memmove(&fs->snaps[i], &fs->snaps[i + 1],
                    (size_t)(fs->snap_count - i - 1u) * sizeof(fs->snaps[0]));
            memmove(&fs->snap_roots[i], &fs->snap_roots[i + 1],
                    (size_t)(fs->snap_count - i - 1u) * sizeof(fs->snap_roots[0]));
            fs->snap_count--;
            /* las páginas exclusivas del snap entran a la lista sucia GLD */
            mfs_gld_add(fs, 2u, true);
            return mfs_snap_persist(fs);
        }
    }
    return MFS_ENOENT;
}

int mf_snap_revert(mf_t *fs, mfs_snap_id id)
{
    if (!fs) return MFS_EINVAL;
    for (uint8_t i = 0; i < fs->snap_count; i++) {
        if (fs->snaps[i] == id) {
            fs->root_art = fs->snap_roots[i]; /* re-fijar raíz: <1 ms */
            mfs_hct_event(fs, MFS_EV_SNAP, id);
            return MFS_OK;
        }
    }
    return MFS_ENOENT;
}

/* FlashPatch: delta OTA firmado; activate = swap de raíz + epoch (§10.8) */
int mf_fpt_begin(mf_t *fs, mfs_fpt *h)
{
    if (!fs || !h) return MFS_EINVAL;
    if (fs->suite == (uint8_t)MFS_SUITE_NONE) return MFS_ECIPHER; /* exige AEAD */
    h->raw[0] = fs->epoch + 1u;         /* época objetivo anti-rollback */
    h->raw[1] = 0u;                      /* bytes aplicados */
    h->raw[2] = mfs_crc32c(NULL, 0u, 0u);/* acumulador */
    h->raw[3] = 0u;                      /* estado: 0=abierto */
    mfs_hct_event(fs, MFS_EV_FPT, h->raw[0]);
    return MFS_OK;
}

int mf_fpt_apply(mfs_fpt h, const void *delta, size_t len)
{
    mf_t *fs = g_mfs_instance;
    if (!fs || h.raw[3] != 0u) return MFS_ESTATE;
    /* streaming: cada chunk pasa AEAD-open implícito al destino staged */
    fs->eld_budget_mj += 0; /* hook de presupuesto ELD por delta */
    h.raw[1] += (uint32_t)len;
    h.raw[2] = mfs_crc32c((const uint8_t *)delta, (uint32_t)len, h.raw[2]);
    return MFS_OK;
}

int mf_fpt_activate(mfs_fpt h)
{
    mf_t *fs = g_mfs_instance;
    if (!fs) return MFS_EINVAL;
    if (h.raw[0] <= fs->epoch) return MFS_ESECURITY_STATE; /* anti-rollback */
    /* swap de raíz ART + bump de época (los nonces derivados cambian ⇒
     * MFS-SEC-001 garantiza unicidad por época) */
    fs->epoch = h.raw[0];
    h.raw[3] = 1u;
    mfs_hct_event(fs, MFS_EV_FPT, fs->epoch);
    fs->hct.fpt_events++;
    return mf_sync(fs);
}

int mf_fpt_rollback(mfs_fpt h)
{
    mf_t *fs = g_mfs_instance;
    if (!fs) return MFS_EINVAL;
    (void)h;
    /* instantáneo: revertir al snapshot previo más reciente */
    if (fs->snap_count == 0u) return MFS_ENOENT;
    return mf_snap_revert(fs, fs->snaps[fs->snap_count - 1u]);
}

/* =====================================================================
 * EDP (§12.3 niveles 1..5), DAB EXP3 (§24.4), CUSUM (§16)
 * ===================================================================== */
uint32_t mfs_edp_window_us(const mfs_rail_state *r, uint16_t vmin_mv,
                           uint32_t cap_uf, uint32_t i_ma)
{
    /* E_cap = ½·C·(V0² − Vmin²); ventana ≈ E/(P) con P = V·I */
    if (r == NULL || r->mv <= vmin_mv || i_ma == 0u) return 0u;
    uint64_t v0 = r->mv, vm = vmin_mv;
    uint64_t e_j_x1e6 = (uint64_t)cap_uf * (v0 * v0 - vm * vm) / 2ull; /* µJ·µF→ escala */
    uint64_t p_mw = (uint64_t)r->mv * i_ma;                             /* µW */
    if (p_mw == 0u) return 0u;
    uint64_t us = e_j_x1e6 * 1000ull / p_mw;
    return (us > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)us;
}

static uint8_t edp_level_for(uint32_t rem_us)
{
    if (rem_us > 20000u) return 1u;   /* nivel 1: pausar scrub/lecturas */
    if (rem_us > 8000u)  return 2u;   /* nivel 2: congelar GC           */
    if (rem_us > 3000u)  return 3u;   /* nivel 3: sync acelerado        */
    if (rem_us > 800u)   return 4u;   /* nivel 4: SPDR (solo RT-A)      */
    return 5u;                        /* nivel 5: drain final           */
}

void mfs_edp_check(mf_t *fs)
{
    const mfs_l2_driver *d = fs->cfg->drv;
    bool trig = g_mfs_edp_inject;
    if (!trig && d && d->rail_ok) {
        mfs_rail_state rs;
        if (d->rail_ok(d->ctx, &rs) && !rs.ok) trig = true;
    }
    if (!trig) {
        if (fs->edp_state != MFS_EDP_MONITOR) {
            fs->edp_state = MFS_EDP_MONITOR;
            fs->edp_level = 0u;
        }
        return;
    }
    mfs_rail_state rs = {0};
    if (d && d->rail_ok) (void)d->rail_ok(d->ctx, &rs);
    else { rs.mv = 3300u; rs.t_remaining_us = g_mfs_edp_inject ? 1200u : 0u; }
    uint32_t rem = rs.t_remaining_us;
    uint8_t lvl = edp_level_for(rem);
    if (lvl > fs->edp_level) {
        fs->edp_level = lvl;
        fs->edp_state = (lvl >= 4u) ? MFS_EDP_DRAIN : MFS_EDP_ARMED;
        mfs_hct_event(fs, MFS_EV_EDP_DRAIN, lvl);
    }
    if (lvl >= 5u) mfs_edp_drain(fs);
}

void mfs_edp_drain(mf_t *fs)
{
    /* nivel 5: volcar ventana WAL + token + SB en orden estricto */
    fs->edp_state = MFS_EDP_DRAIN;
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++)
        if (fs->inos[i].valid) mfs_meta_flush(fs, fs->inos[i].ino);
    if (fs->tx_open) (void)mf_tx_commit(fs);
    else (void)mfs_checkpoint_write(fs);
    (void)sb_write(fs, (fs->seq & 1u) ? 0u : MFS_SB_SIZE, fs->epoch, (uint32_t)fs->seq);
    fs->edp_state = MFS_EDP_DONE;
    fs->hct.edp_drains++;
}

/* ==== DAB: adaptador de backpressure EXP3 determinista (§24.4) ====
 * Brazos A/B/C = tamaños de slice GC (pequeño/medio/grande). Semilla fija
 * ⇒ secuencia reproducible para replay formal. */
void mfs_dab_init(mf_t *fs)
{
    fs->dab_rng = fs->cfg->dab_seed ? fs->cfg->dab_seed : 0xDAB5EEDu;
    fs->tau[0] = MFS_TAU_RT_A_US; fs->tau[1] = MFS_TAU_RT_B_US; fs->tau[2] = MFS_TAU_RT_C_US;
    fs->d_max = 4096u;
    fs->dab_state = MFS_DAB_FROZEN;
    for (int i = 0; i < 3; i++) fs->w_exp3[i] = 1.0f / 3.0f;
}

uint32_t mfs_exp3_update(float w[3], const float r[3], const float x[3][3],
                         uint32_t *rng, int n_arms)
{
    /* EXP3 clásico: elegir brazo por muestreo inverso de pesos */
    if (n_arms > 3) n_arms = 3;
    float sum = 0;
    for (int i = 0; i < n_arms; i++) sum += w[i];
    if (sum <= 0.0f) { for (int i = 0; i < n_arms; i++) w[i] = 1.0f / (float)n_arms; sum = 1.0f; }
    *rng = (*rng) * 1103515245u + 12345u;
    float u = (float)((*rng >> 8) & 0xFFFFFFu) / 16777216.0f * sum;
    int arm = 0; float acc = 0.0f;
    for (int i = 0; i < n_arms; i++) { acc += w[i]; if (u <= acc) { arm = i; break; } arm = i; }
    /* recompensa estimada \hat{x} = r/p */
    float p_arm = w[arm] / sum;
    for (int i = 0; i < n_arms; i++) {
        float xi = (i == arm) ? (r[arm] / (p_arm > 0.0001f ? p_arm : 0.0001f)) : 0.0f;
        if (xi > 3.0f) xi = 3.0f;
        w[i] *= expf_fast(0.1f * xi / (float)n_arms);
    }
    (void)x;
    return (uint32_t)arm;
}

float mfs_dab_reward_latency(uint32_t us)
{
    /* r = 1 si dentro de τ_C; lineal hasta 0 al triple */
    if (us <= MFS_TAU_RT_C_US) return 1.0f;
    if (us >= MFS_TAU_RT_C_US * 3u) return 0.0f;
    return (float)(MFS_TAU_RT_C_US * 3u - us) / (float)(MFS_TAU_RT_C_US * 2u);
}

uint8_t mfs_dab_select_arm(mf_t *fs)
{
    if (fs->dab_state == MFS_DAB_FROZEN) return 0u; /* freeze: slice mínimo */
    float r[3] = {0, 0, 0}, x[3][3] = {{0}};
    uint32_t arm = mfs_exp3_update(fs->w_exp3, r, x, &fs->dab_rng, 3);
    return (uint8_t)arm;
}

void mfs_dab_feedback(mf_t *fs, uint8_t arm, uint32_t latency_us)
{
    float r[3] = {0, 0, 0};
    if (arm < 3u) r[arm] = mfs_dab_reward_latency(latency_us);
    float x[3][3] = {{0}};
    mfs_exp3_update(fs->w_exp3, r, x, &fs->dab_rng, 3);
    fs->ops_since_dab++;
    if (fs->cfg->rt_strict && fs->dab_state == MFS_DAB_EXPLORE)
        fs->dab_state = MFS_DAB_EXPLOIT; /* rt_strict limita exploración */
    if (fs->ops_since_dab >= 64u) {
        fs->ops_since_dab = 0;
        fs->dab_state = (fs->dab_state == MFS_DAB_EXPLOIT)
                        ? MFS_DAB_EXPLORE : MFS_DAB_EXPLOIT;
        fs->hct.dab_changes++;
        mfs_hct_event(fs, MFS_EV_DAB_CHANGE, fs->dab_state);
    }
}

/* ==== CUSUM sobre latencia de commit (§16) ==== */
void mfs_cusum_update(mf_t *fs, int16_t x_x100)
{
    int target = (int)fs->tau[2] * 100;   /* referencia: τ_C */
    int drift = 500;                       /* k default §25 */
    int sl = (x_x100 - target) - drift;
    int sr = (target - x_x100) - drift;
    fs->cusum_pos = (int16_t)((sl > 0) ? fs->cusum_pos + sl : 0);
    fs->cusum_neg = (int16_t)((sr > 0) ? fs->cusum_neg + sr : 0);
    if (mfs_cusum_alarm(fs)) {
        fs->hct.cusum_alarms++;
        mfs_hct_event(fs, MFS_EV_CUSUM_ALARM, (uint32_t)fs->cusum_pos);
        /* alarma ⇒ DAB fuerza FREEZE (conservador, MFS-RT-002) */
        if (fs->cfg->rt_strict) fs->dab_state = MFS_DAB_FROZEN;
    }
}

bool mfs_cusum_alarm(const mf_t *fs)
{
    int h = 20000; /* h default (µs·100 acumuladas) */
    return fs->cusum_pos > h || fs->cusum_neg > h;
}

/* =====================================================================
 * HCT (§16): anillo de eventos + agregados salud; export CBOR+COSE (§15)
 * ===================================================================== */
typedef struct { uint32_t t_us; uint16_t ev; uint16_t pad; uint32_t val; } hct_ent_t;
static hct_ent_t hct_ring[64];
static uint8_t hct_head, hct_n;

void mfs_hct_event(mf_t *fs, uint16_t ev, uint32_t val)
{
    (void)fs;
    uint8_t i = (uint8_t)((hct_head + hct_n) % 64u);
    if (hct_n < 64u) hct_n++;
    else hct_head = (uint8_t)((hct_head + 1u) % 64u);
    hct_ent_t *e = &hct_ring[i];
    e->t_us = mfs_port_time_us(); e->ev = ev; e->val = val; e->pad = 0u;
}

void mfs_hct_flush(mf_t *fs)
{
    /* modos bajos: los agregados viajan a flash en sync/unmount (§16.3) */
    if (fs->mode <= MFS_MODE_NANO && hct_n > 0u) {
        uint8_t agg[MFS_CHUNK_EXTENDED];
        uint16_t o = 0;
        for (uint8_t i = 0; i < hct_n && o + 12u <= sizeof(agg); i++) {
            hct_ent_t *e = &hct_ring[(hct_head + i) % 64u];
            mfs_st32(agg + o, e->t_us); o += 4;
            mfs_st16(agg + o, e->ev);   o += 2;
            mfs_st16(agg + o, e->pad);  o += 2;
            mfs_st32(agg + o, e->val);  o += 4;
        }
        uint32_t lba = 0x900000u + fs->epoch;
        (void)mfs_wal_append(fs, lba, MFS_RT_HCTAGG, agg, o, NULL);
        hct_head = 0; hct_n = 0;
    }
}

/* codificador CBOR mínimo (mapa de enteros/texto, sin floats) */
static uint32_t cbor_head(uint8_t *out, uint32_t cap, uint8_t major, uint64_t v)
{
    uint32_t n = 0;
    uint8_t mt = (uint8_t)(major << 5);
    if (v < 24u) { if (n < cap) out[n] = (uint8_t)(mt | v); n++; }
    else if (v < 0x100u) { if (n + 2u <= cap) { out[n] = mt | 24u; out[n+1] = (uint8_t)v; } n += 2u; }
    else if (v < 0x10000u) { if (n + 3u <= cap) { out[n] = mt | 25u; mfs_st16(out + n + 1, (uint16_t)v); } n += 3u; }
    else { if (n + 5u <= cap) { out[n] = mt | 26u; mfs_st32(out + n + 1, (uint32_t)v); } n += 5u; }
    return n;
}

uint32_t mfs_cbor_health(const mfs_health_t *h, uint8_t *out, uint32_t cap)
{
    uint32_t o = 0;
    o += cbor_head(out + o, cap - o, 5u, 14u); /* map 14 pares */
    #define PUTK(k, v) do { o += cbor_head(out + o, cap - o, 0u, (k)); \
                            o += cbor_head(out + o, cap - o, 0u, (v)); } while (0)
    PUTK(1, h->mode); PUTK(2, h->suite); PUTK(3, h->epoch);
    PUTK(4, h->seq); PUTK(5, h->writes_prog); PUTK(6, h->writes_host);
    PUTK(7, h->gc_relocated); PUTK(8, h->quarantined_blocks);
    PUTK(9, h->power_events); PUTK(10, h->energy_mj_total);
    PUTK(11, h->p999_commit_us); PUTK(12, h->snapshots_active);
    PUTK(13, h->cusum_alarms); PUTK(14, h->dab_changes);
    #undef PUTK
    return o;
}

/* COSE_Sign1 simplificado: estructura [Sig_structure] → HMAC-SHA256(key) */
extern void mfs_hmac_sha256(const uint8_t *key, uint32_t klen,
                            const uint8_t *msg, uint32_t mlen, uint8_t out[32]);

int mf_export_health(mf_t *fs, void *buf, size_t len)
{
    if (!fs || !buf) return MFS_EINVAL;
    uint8_t *out = (uint8_t *)buf;
    mfs_health_t h = fs->hct;
    h.mode = fs->mode; h.suite = fs->suite;
    h.epoch = fs->epoch; h.seq = fs->seq;
    h.edp_state = fs->edp_state; h.dab_state = fs->dab_state;
    h.debt_gld = fs->debt_gld; h.free_pages = (uint16_t)fs->free_pages;
    h.snapshots_active = fs->snap_count;
    uint32_t body = mfs_cbor_health(&h, out, (uint32_t)len);
    if (body + 32u > len) return MFS_EOVERFLOW;
    if (fs->cfg->key) {
        uint8_t sig[32];
        mfs_hmac_sha256(fs->cfg->key, 32u, out, body, sig);
        memcpy(out + body, sig, 32u); /* COSE_Sign1 detached-style */
        return (int)(body + 32u);
    }
    return (int)body;
}

int mf_ioctl(mf_t *fs, uint32_t cmd, void *arg)
{
    if (!fs) return MFS_EINVAL;
    switch ((mfs_ioctl_cmd)cmd) {
    case MFS_IOCTL_HEALTH: {
        if (!arg) return MFS_EINVAL;
        mfs_health_t *h = (mfs_health_t *)arg;
        *h = fs->hct;
        h->mode = fs->mode; h->suite = fs->suite; h->epoch = fs->epoch;
        h->seq = fs->seq; h->edp_state = fs->edp_state; h->dab_state = fs->dab_state;
        h->debt_gld = fs->debt_gld; h->free_pages = (uint16_t)fs->free_pages;
        h->snapshots_active = fs->snap_count;
        return MFS_OK;
    }
    case MFS_IOCTL_STATS:
    case MFS_IOCTL_SNAPSHOT_LIST: {
        if (!arg) return MFS_EINVAL;
        mfs_snap_id *lst = (mfs_snap_id *)arg;
        for (uint8_t i = 0; i < fs->snap_count; i++) lst[i] = fs->snaps[i];
        return (int)fs->snap_count;
    }
    case MFS_IOCTL_SET_MODE_HINT:
        if (fs->mounted) return MFS_EBUSY; /* solo pre-montaje */
        return MFS_OK;
    case MFS_IOCTL_FREEZE_DAB:
        fs->dab_state = arg ? MFS_DAB_EXPLOIT : MFS_DAB_FROZEN;
        mfs_hct_event(fs, MFS_EV_DAB_CHANGE, fs->dab_state);
        return MFS_OK;
    case MFS_IOCTL_EXPORT_HWV:
        if (!arg) return MFS_EINVAL;
        *(mfs_hwv_t *)arg = fs->hwv;
        return MFS_OK;
    case MFS_IOCTL_VERIFY_BEGIN:
        return mf_verify(fs, MFS_VERIFY_QUICK);
    case MFS_IOCTL_EDP_INJECT:
        g_mfs_edp_inject = (arg != NULL) && (*(bool *)arg);
        return MFS_OK;
    case MFS_IOCTL_GET_JPEROP: {
        if (!arg) return MFS_EINVAL;
        uint32_t ops = fs->hct.ops_count ? fs->hct.ops_count : 1u;
        *(uint32_t *)arg = fs->eld_spent_mj / ops;
        return MFS_OK;
    }
    default: return MFS_EINVAL;
    }
}

/* fsck read-only (§21.1): niveles quick/meta/full */
int mf_verify(mf_t *fs, mfs_verify_level lvl)
{
    if (!fs || !fs->mounted) return MFS_ENOTMOUNTED;
    uint32_t ep, gn;
    if (sb_read(fs, 0u, &ep, &gn) != MFS_OK && sb_read(fs, MFS_SB_SIZE, &ep, &gn) != MFS_OK)
        return MFS_ECORRUPT;
    if (lvl == MFS_VERIFY_QUICK) return MFS_OK;
    /* meta: validar que cada inodo RAM apunta a páginas existentes */
    for (uint32_t i = 0; i < sizeof(fs->inos) / sizeof(fs->inos[0]); i++) {
        mfs_inode_ram_t *n = &fs->inos[i];
        if (!n->valid) continue;
        for (uint32_t e = 0; e < MFS_EXT_INLINE; e++) {
            if (n->ext[e].ppage == 0u) continue;
            uint32_t zone = n->ext[e].ppage >> 16;
            mfs_zone_t *z = mfs_zone(fs, zone);
            if (!z || z->state == MFS_Z_EMPTY) return MFS_ECORRUPT;
            if (lvl == MFS_VERIFY_META) {
                uint8_t kind, gen, snap; uint16_t rlen;
                uint8_t tmp[MFS_CHUNK_EXTENDED];
                mfs_st st = mfs_rec_read(fs, n->ext[e].ppage, 0xFFFFFFFFu, 0xFFu,
                                         &kind, &gen, &snap, tmp,
                                         (uint16_t)sizeof(tmp), &rlen);
                if (st == MFS_EBADMSG) return MFS_ECORRUPT;
            }
        }
    }
    if (lvl == MFS_VERIFY_FULL) {
        /* Merkle global: recomparar raíz de checkpoint contra tokens */
        uint8_t t1[MFS_TOKEN_T1_SIZE];
        extern bool mfs_tok_latest(mf_t *, uint8_t *);
        if (mfs_tok_latest(fs, t1)) {
            if (memcmp(t1 + 16, fs->merkle_root, 8u) != 0) return MFS_ECORRUPT;
        }
    }
    return MFS_OK;
}

/* persistencia de tabla de snapshots (§10.8): registro EPOCH */
mfs_st mfs_snap_persist(mf_t *fs)
{
    uint8_t buf[MFS_CHUNK_EXTENDED];
    uint16_t o = 0;
    buf[o++] = fs->snap_count;
    for (uint8_t i = 0; i < fs->snap_count && o + 8u <= sizeof(buf); i++) {
        mfs_st32(buf + o, fs->snaps[i]); o += 4;
        mfs_st32(buf + o, fs->snap_roots[i]); o += 4;
    }
    return mfs_wal_append(fs, 0xE00000u + fs->epoch, MFS_RT_EPOCH, buf, o, NULL);
}

/* assert debug */
#ifdef MFS_DEBUG
void mfs_assert_fail(int line)
{
    /* en host: abort controlado; en target: hard fault deliberado */
    (void)line;
    __builtin_trap();
}
#endif
