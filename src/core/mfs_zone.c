/* mfs_zone.c — ZLF log-structured (§11.6, FSM §24.1), registro E2G (§8.2),
 * AGCB+ con GLD (§9.4/§11.2) y backpressure (§7.4). */
#include "mfs_internal.h"
#include <string.h>

#define HDR_NOR  MFS_E2G_NOR_HDR   /* magic2 ver1 type1 flags1 dict1 lba4 gen1 snap1 hot1 zrp1 len2 crc4 = 20 */
#define HDR_NAND MFS_E2G_NAND_HDR

uint16_t mfs_e2g_hdr(mf_t *fs)
{
    uint32_t t = fs->hwv.media_type;
    return (t == MFS_MEDIA_NAND_RAW || t == MFS_MEDIA_NAND_ONFI ||
            t == MFS_MEDIA_ZNS_NAND) ? HDR_NAND : HDR_NOR;
}

/* ==== geometría derivada del modo (§18.1 chunk 128..4096) ==== */
uint32_t mfs_page_bytes(mf_t *fs)
{
    return mfs_limits[fs->mode].chunk_size;
}

static uint32_t pages_per_zone(mf_t *fs)
{
    uint32_t zu = fs->hwv.erase_unit;
    if (!zu) zu = 4096u;
    uint32_t pb = mfs_page_bytes(fs);
    return zu / pb;
}

mfs_st mfs_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len)
{
    const mfs_l2_driver *d = fs->cfg->drv;
    if (!d || !d->read) return MFS_EIO;
    mfs_port_crit_enter();
    mfs_st st = d->read(d->ctx, addr, dst, len);
    mfs_port_crit_exit();
    return st;
}

mfs_st mfs_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len)
{
    const mfs_l2_driver *d = fs->cfg->drv;
    if (!d || !d->prog) return MFS_EIO;
    /* WOB barrier (§20.3): prog retorna SOLO tras verificación de estado.
     * El driver L2 garantiza esto; aquí verificamos read-back en modo debug. */
    mfs_st st = d->prog(d->ctx, addr, src, len);
#ifdef MFS_DEBUG
    if (st == MFS_OK) {
        uint8_t rb[128];
        if (len <= 128u && d->read(d->ctx, addr, rb, len) == MFS_OK) {
            if (memcmp(rb, src, len) != 0) { mfs_hct_event(fs, MFS_EV_E2G_FAIL, addr); return MFS_ECORRUPT; }
        }
    }
#endif
    if (st == MFS_OK) {
        fs->hct.writes_prog++;
        fs->eld_spent_mj += mfs_energy_prog_mj(&fs->hwv, len);
    }
    return st;
}

mfs_st mfs_erase(mf_t *fs, uint32_t addr)
{
    const mfs_l2_driver *d = fs->cfg->drv;
    if (!d || !d->erase) return MFS_EIO;
    return d->erase(d->ctx, addr);
}

bool mfs_has_t0(mf_t *fs)
{
    return fs->cfg->drv_t0 && fs->hwv.t0_size > 0u;
}

mfs_st mfs_t0_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len)
{
    if (!mfs_has_t0(fs)) return MFS_ENOTSUP;
    return fs->cfg->drv_t0->read(fs->cfg->drv_t0->ctx, addr, dst, len);
}

mfs_st mfs_t0_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len)
{
    if (!mfs_has_t0(fs)) return MFS_ENOTSUP;
    return fs->cfg->drv_t0->prog(fs->cfg->drv_t0->ctx, addr, src, len);
}

/* ==== construcción de cabecera E2G (§8.2) ==== */
static void build_hdr(uint8_t *h, uint16_t hs, uint8_t kind, uint8_t flags,
                      uint8_t dictid, uint32_t lba, uint8_t gen, uint8_t snapid,
                      uint8_t hotness, bool zrp, uint16_t plen)
{
    h[0] = (uint8_t)(MFS_REC_MAGIC >> 8); h[1] = (uint8_t)MFS_REC_MAGIC;
    h[2] = 1u; /* versión layout */
    h[3] = (uint8_t)((kind << 4) | (flags & 0x0Fu));
    h[4] = dictid;
    mfs_st32(h + 5, lba);
    h[9] = gen; h[10] = snapid; h[11] = hotness;
    h[12] = zrp ? 1u : 0u;
    mfs_st16(h + 13, plen);
    /* NAND añade 12 B extra: plane/die/cycle hints — relleno determinista */
    if (hs >= MFS_E2G_NAND_HDR) { memset(h + 15, 0, hs - 20u); }
    /* CRC al final */
    mfs_st32(h + hs - 4u, 0u);
}

static uint32_t hdr_crc(const uint8_t *h, uint16_t hs)
{
    return mfs_ld32(h + hs - 4u);
}

/* ==== asignación de zona abierta por clase de hotness ==== */
int mfs_zone_alloc_open(mf_t *fs, uint8_t hotness)
{
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        mfs_zone_t *z = &fs->zones[i];
        if (z->state == MFS_Z_OPEN && z->class_hot == hotness &&
            z->write_ptr + mfs_page_bytes(fs) <= z->size) {
            return (int)i;
        }
    }
    /* abrir nueva zona vacía */
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        mfs_zone_t *z = &fs->zones[i];
        if (z->state == MFS_Z_EMPTY) {
            z->zone_id = (uint16_t)i;
            z->state = MFS_Z_OPEN;
            z->class_hot = hotness;
            z->seq = fs->seq++;
            z->start_addr = i * fs->hwv.erase_unit;
            z->size = fs->hwv.erase_unit;
            z->write_ptr = mfs_e2g_hdr(fs); /* saltar cabecera de zona */
            z->total_pages = (uint16_t)pages_per_zone(fs);
            z->valid_pages = 0;
            z->zrp = 0;
            return (int)i;
        }
    }
    return -1; /* sin zonas libres → GC/backpressure caller */
}

mfs_zone_t *mfs_zone(mf_t *fs, uint32_t id)
{
    if (id >= MFS_MAX_ZONES) return NULL;
    return &fs->zones[id];
}

mfs_st mfs_zone_seal(mf_t *fs, uint32_t id)
{
    mfs_zone_t *z = mfs_zone(fs, id);
    if (!z || z->state != MFS_Z_OPEN) return MFS_ESTATE;
    z->state = MFS_Z_FULL;
    return MFS_OK;
}

/* escribir una página completa (cabecera + payload + padding erased-safe) */
mfs_st mfs_rec_write(mf_t *fs, uint32_t zone, uint32_t lba, uint8_t kind,
                     uint8_t gen, uint8_t snapid, uint8_t hotness, uint8_t dictid,
                     const uint8_t *payload, uint16_t plen, uint32_t *ppage_out)
{
    mfs_zone_t *z = mfs_zone(fs, zone);
    if (!z || z->state != MFS_Z_OPEN) return MFS_ESTATE;
    uint16_t hs = mfs_e2g_hdr(fs);
    uint32_t pb = mfs_page_bytes(fs);
    if (plen + hs > pb) return MFS_EINVAL;
    if (z->write_ptr + pb > z->size) {
        mfs_zone_seal(fs, zone);
        return MFS_ENOSPC;
    }
    static uint8_t buf[MFS_CHUNK_EXTENDED]; /* scratch de 1 sola entrada: núcleo single-core (§13.4) */
    memset(buf, 0xFFu, pb); /* NOR erased state */
    build_hdr(buf, hs, kind, 0u, dictid, lba, gen, snapid, hotness, z->zrp != 0u, plen);
    memcpy(buf + hs, payload, plen);
    /* CRC sobre cabecera(con 0)+payload */
    uint32_t crc = mfs_crc32c(buf, (uint32_t)(hs - 4u), 0u);
    crc = mfs_crc32c(payload, plen, crc);
    mfs_st32(buf + hs - 4u, crc);
    /* cifrado AEAD por registro si suite activa (S0-S3): cifra payload in situ */
    if (fs->suite != (uint8_t)MFS_SUITE_NONE && fs->cfg->key) {
        uint16_t olen = 0;
        uint64_t nonce = ((uint64_t)fs->epoch << 32) | (uint64_t)(z->seq + z->write_ptr / pb);
        mfs_st st = mfs_suite_seal(fs->suite, fs->cfg->key, nonce,
                                   buf + hs, plen, buf + hs, &olen);
        if (st != MFS_OK) return st;
        if (olen + hs > pb) return MFS_ENOSPC;
        /* recalcular CRC sobre el ciphertext sellado */
        crc = mfs_crc32c(buf, (uint32_t)(hs - 4u), 0u);
        crc = mfs_crc32c(buf + hs, olen, crc);
        mfs_st32(buf + hs - 4u, crc);
        mfs_st16(buf + 13, olen);
        plen = olen;
    }
    uint32_t addr = z->start_addr + z->write_ptr;
    uint32_t ppage = (zone << 16) | (z->write_ptr / pb);
    mfs_st st = mfs_write(fs, addr, buf, pb);
    if (st != MFS_OK) {
        if (st == MFS_ECORRUPT) { /* falla verificación post-program */
            z->state = MFS_Z_QUARANTINE;
            fs->hct.quarantined_blocks++;
            mfs_hct_event(fs, MFS_EV_QUARANTINE, addr);
        }
        return st;
    }
    fs->hct.writes_host++;
    /* L2P: este registro pasa a ser la versión vigente del LBA */
    if (lba < MFS_MAX_BLOCKS * 16u) {
        if (fs->l2p[lba] != 0xFFFFFFFFu) {
            /* obsoleto: deuda GLD de 1 página */
            mfs_gld_add(fs, 1u, kind != MFS_RT_DATA);
        }
        fs->l2p[lba] = ppage;
    }
    if (ppage_out) *ppage_out = ppage;
    z->write_ptr += pb;
    z->valid_pages++;
    fs->seq++;
    return MFS_OK;
}

/* leer un registro validando LBA+gen (E2G nivel 4-5, §12 niveles 4/5) */
mfs_st mfs_rec_read(mf_t *fs, uint32_t ppage, uint32_t expect_lba, uint8_t expect_gen,
                    uint8_t *kind, uint8_t *gen, uint8_t *snapid,
                    uint8_t *buf, uint16_t bufsize, uint16_t *rlen)
{
    uint32_t zone = ppage >> 16;
    uint32_t idx = ppage & 0xFFFFu;
    mfs_zone_t *z = mfs_zone(fs, zone);
    if (!z) return MFS_EINVAL;
    uint16_t hs = mfs_e2g_hdr(fs);
    uint32_t pb = mfs_page_bytes(fs);
    uint32_t addr = z->start_addr + idx * pb;
    uint8_t tmp[MFS_CHUNK_EXTENDED];
    if (pb > sizeof(tmp)) return MFS_EINVAL;
    mfs_st st = mfs_read(fs, addr, tmp, pb);
    if (st != MFS_OK) return st;
    if (tmp[0] != (uint8_t)(MFS_REC_MAGIC >> 8) || tmp[1] != (uint8_t)MFS_REC_MAGIC)
        return MFS_EBADMSG;
    uint16_t plen = mfs_ld16(tmp + 13);
    uint32_t crc = hdr_crc(tmp, hs);
    uint32_t calc = mfs_crc32c(tmp, (uint32_t)(hs - 4u), 0u);
    calc = mfs_crc32c(tmp + hs, plen, calc);
    if (calc != crc) {
        fs->hct.crc_errors++;
        mfs_hct_event(fs, MFS_EV_E2G_FAIL, addr);
        return MFS_EBADMSG;
    }
    uint32_t lba = mfs_ld32(tmp + 5);
    if (expect_lba != 0xFFFFFFFFu && lba != expect_lba) {
        fs->hct.e2g_failures++;
        mfs_hct_event(fs, MFS_EV_E2G_FAIL, lba);
        return MFS_EBADMSG;
    }
    uint8_t g = tmp[9];
    if (expect_gen != 0xFFu && g != expect_gen) return MFS_EBADMSG;
    if (kind) *kind = (uint8_t)(tmp[3] >> 4);
    if (gen) *gen = g;
    if (snapid) *snapid = tmp[10];
    /* descifrar si aplica: nonce reproducido = epoch<<32 | seq_de_escritura.
     * El seq de escritura se recupera determinísticamente: base_seq de la
     * zona + índice de página (las páginas se escriben consecutivas). */
    if (fs->suite != (uint8_t)MFS_SUITE_NONE && fs->cfg->key) {
        uint16_t olen = 0;
        uint64_t nonce = ((uint64_t)fs->epoch << 32) | (uint64_t)(z->seq + idx);
        st = mfs_suite_open(fs->suite, fs->cfg->key, nonce, tmp + hs, plen, buf, &olen);
        if (st == MFS_ESECURITY_STATE) {
            fs->hct.e2g_failures++;
            mfs_hct_event(fs, MFS_EV_E2G_FAIL, addr);
            return st;
        }
        if (st != MFS_OK) return st;
        if (rlen) *rlen = olen;
        return MFS_OK;
    }
    if (plen > bufsize) return MFS_EOVERFLOW;
    memcpy(buf, tmp + hs, plen);
    if (rlen) *rlen = plen;
    return MFS_OK;
}

/* ==== GLD deuda + backpressure (§9.4, §7.4) ==== */
void mfs_gld_add(mf_t *fs, uint32_t pages, bool meta)
{
    uint32_t w = meta ? 2u : 1u;              /* meta cuenta x2 (§25 meta_weight) */
    uint32_t d = fs->debt_gld + pages * w;
    fs->debt_gld = (d > 0xFFFFu) ? 0xFFFFu : (uint16_t)d;
}

/* selección victim por eficiencia estimada (greedy+coste de salud §11.2) */
static int pick_victim(mf_t *fs)
{
    int best = -1; float bestscore = -1.0f;
    for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
        mfs_zone_t *z = &fs->zones[i];
        if (z->state != MFS_Z_FULL) continue;
        if (z->valid_pages == 0u) { /* zona totalmente obsoleta: liberación directa */
            return (int)i;
        }
        float util = (float)z->valid_pages / (float)(z->total_pages ? z->total_pages : 1u);
        float score = 1.0f - util;   /* menor utilidad = mejor */
        if (score > bestscore) { bestscore = score; best = (int)i; }
    }
    return best;
}

/* GC slice acotado por presupuesto temporal (§11.2 AGCB+) */
mfs_st mfs_gc_slice(mf_t *fs, uint32_t budget_us)
{
    uint32_t t0 = mfs_port_time_us();
    int v = pick_victim(fs);
    if (v < 0) return MFS_OK;
    mfs_zone_t *z = &fs->zones[v];
    uint32_t pb = mfs_page_bytes(fs);
    uint8_t payload[MFS_CHUNK_EXTENDED];
    for (uint32_t off = mfs_e2g_hdr(fs); off + pb <= z->write_ptr; off += pb) {
        if (mfs_port_time_us() - t0 > budget_us) return MFS_ETIMEDOUT_BUDGET;
        uint32_t ppage = ((uint32_t)v << 16) | (off / pb);
        uint8_t k; uint8_t g, s; uint16_t rlen;
        /* leer cabecera en crudo para conocer el LBA del registro */
        uint8_t raw[24];
        mfs_st sth = mfs_read(fs, z->start_addr + off, raw, mfs_e2g_hdr(fs));
        if (sth != MFS_OK || raw[0] != (uint8_t)(MFS_REC_MAGIC >> 8)) continue;
        uint32_t lba = mfs_ld32(raw + 5);
        /* ¿es la última versión de ese LBA? si no, es obsoleto: no relocar */
        bool live = true;
        for (uint32_t zz = 0; zz < MFS_MAX_ZONES && live; zz++) {
            mfs_zone_t *o = &fs->zones[zz];
            if (o->state == MFS_Z_EMPTY || o->state == MFS_Z_QUARANTINE) continue;
            if (o->seq >= z->seq) continue; /* zonas más jóvenes pueden contener copia */
        }
        /* búsqueda directa en zonas OPEN/FULL posteriores: comprobar último
         * mapeo conocido vía tabla l2p[] */
        if (lba < MFS_MAX_BLOCKS * 16u && fs->l2p[lba] != ppage) live = false;
        if (!live) continue;
        sth = mfs_rec_read(fs, ppage, lba, 0xFFu, &k, &g, &s,
                           payload, (uint16_t)sizeof(payload), &rlen);
        if (sth == MFS_EBADMSG) continue; /* registro muerto: no relocar */
        /* reubicar en zona abierta de su clase */
        int nz = mfs_zone_alloc_open(fs, z->class_hot);
        if (nz < 0) return MFS_ENOSPC;
        mfs_rec_write(fs, (uint32_t)nz, lba, k, g, s, z->class_hot, 0u,
                      payload, rlen, NULL);
        z->valid_pages--;
        fs->hct.gc_relocated++;
        if (fs->debt_gld > 0u) fs->debt_gld--;
    }
    /* borrar zona víctima completa */
    mfs_st est = mfs_erase(fs, z->start_addr);
    if (est != MFS_OK) {
        z->state = MFS_Z_QUARANTINE;
        fs->hct.quarantined_blocks++;
        mfs_hct_event(fs, MFS_EV_QUARANTINE, z->start_addr);
        return est;
    }
    z->state = MFS_Z_EMPTY;
    z->write_ptr = 0; z->valid_pages = 0; z->seq = 0;
    fs->free_pages++;
    return MFS_OK;
}

/* modelo energético simple para ELD (§13.3): P·t aproximado por bytes */
uint32_t mfs_energy_prog_mj(const mfs_hwv_t *hwv, uint32_t bytes)
{
    /* 1 mA @3.3 V ≈ 3.3 mW; t_prog por página (256 B) ⇒ mJ/página */
    uint32_t per_kb_mv = 3300u / 1000u; /* factor demo determinista */
    (void)per_kb_mv;
    uint32_t pages = bytes / 256u + 1u;
    uint32_t mj = pages * (hwv->t_prog_max_us ? hwv->t_prog_max_us : 700u) / 100u;
    return mj;
}
