/* mfs_wal.c — WAL+ (§9): registro anticipado con ventana BMT acotada,
 * tokens de commit T1/T0 con anillo A/B + contador termométrico (§8.4/§9.2),
 * checkpoint §9.5 con raíz Merkle BLAKE3 incremental, y replay acotado (§24.2).
 *
 * Invariantes normativas implementadas:
 *  MFS-TX-001: ninguna modificación de metadatos es visible antes del token T1.
 *  MFS-TX-002: el orden de escritura es WALENT(s) → … → T1(txid, crc, merkle).
 *  MFS-TX-003: tras corte, el montaje replay SOLO entradas con token vigente.
 *  MFS-TX-004: la ventana WAL en RAM está acotada por W (§25); overflow ⇒
 *             checkpoint forzoso o MFS_EBACKPRESSURE.
 *  MFS-HMT-001: si hay T0 (FRAM/MRAM), el token se escribe primero en T0 y
 *             después se espeja en flash (T1) para durabilidad de largo plazo.
 */
#include "mfs_internal.h"
#include <string.h>

/* ==== payload WALENT (orden determinista LE, sin struct on-flash) ====
 * [0..3] txid | [4..7] lba destino | [8..9] len | [10] kind | [11..] datos
 */
#define WALHDR 12u

/* ==== anillo de tokens A/B sobre TFC (§9.2, §8.4) ====
 * Layout T0/flash: 64 B = 16 slots × 4 B. Cada slot codifica:
 *   word = (txid_crc24 << 8) | (tfc_bit ? 0xE : 0x1)  — patrón inválido
 *   erased 0xFFFFFFFF ⇒ sin token. La validez real se comprueba releyendo
 *   el CRC del cuerpo T1 espejado; aquí el slot guarda txid XOR checksum.
 */
#define TOK_RING_BYTES (MFS_TOKEN_SLOTS * 4u) /* 64 B */

static uint32_t tok_slot_addr(mf_t *fs, uint32_t slot)
{
    /* región tokens: primer bloque reservado tras HWV (SB A/B + HWV = 1 KB) */
    return fs->hwv.base_reserved_off + TOK_RING_BYTES + slot * 4u;
}

/* ==== append WAL (§9.1) ==== */
mfs_st mfs_wal_append(mf_t *fs, uint32_t lba, uint8_t kind,
                      const uint8_t *pl, uint16_t len, uint32_t *ppage)
{
    if (!fs->mounted) return MFS_ENOTMOUNTED;
    if (len + WALHDR > mfs_page_bytes(fs)) return MFS_EINVAL;

    /* ventana BMT llena → checkpoint forzoso (MFS-TX-004) */
    if (fs->wal_count >= MFS_WAL_WINDOW_MAX) {
        mfs_st st = mfs_checkpoint_write(fs);
        if (st != MFS_OK) return MFS_EBACKPRESSURE;
    }

    /* zona WAL dedicada (hotness 0 = meta caliente) */
    if ((int)fs->zone_wal < 0 || !mfs_zone(fs, fs->zone_wal) ||
        mfs_zone(fs, fs->zone_wal)->state != MFS_Z_OPEN) {
        int z = mfs_zone_alloc_open(fs, 0u);
        if (z < 0) { mfs_gld_maybe_gc(fs); z = mfs_zone_alloc_open(fs, 0u); }
        if (z < 0) return MFS_ENOSPC;
        fs->zone_wal = (uint32_t)z;
    }

    uint8_t rec[MFS_CHUNK_EXTENDED];
    mfs_st32(rec,      fs->txid_cur);
    mfs_st32(rec + 4,  lba);
    mfs_st16(rec + 8,  len);
    rec[10] = kind;
    rec[11] = fs->sp_depth;             /* savepoint vigente (§9.6) */
    memcpy(rec + WALHDR, pl, len);
    /* relleno hasta página: patrón derivado del payload (evita 0xFF ambiguo) */
    for (uint32_t i = WALHDR + len; i < mfs_page_bytes(fs); i++) rec[i] = 0x5A;

    uint32_t pp = 0xFFFFFFFFu;
    mfs_st st = mfs_rec_write(fs, fs->zone_wal, lba, MFS_RT_WALENT,
                              (uint8_t)(fs->epoch & 0xFu), 0u, 0u, 0u,
                              rec, (uint16_t)(WALHDR + len), &pp);
    if (st == MFS_ENOSPC) { /* zona sellada a media escritura: reabrir */
        int z = mfs_zone_alloc_open(fs, 0u);
        if (z < 0) return MFS_ENOSPC;
        fs->zone_wal = (uint32_t)z;
        st = mfs_rec_write(fs, fs->zone_wal, lba, MFS_RT_WALENT,
                           (uint8_t)(fs->epoch & 0xFu), 0u, 0u, 0u,
                           rec, (uint16_t)(WALHDR + len), &pp);
    }
    if (st != MFS_OK) return st;

    uint16_t idx = (uint16_t)((fs->wal_head + fs->wal_count) % MFS_WAL_WINDOW_MAX);
    fs->wal[idx].txid = fs->txid_cur;
    fs->wal[idx].ppage = pp;
    fs->wal[idx].committed = fs->tx_open ? 0u : 1u; /* fuera de tx: autocommit */
    fs->wal[idx].valid = 1u;
    fs->wal_count++;
    if (ppage) *ppage = pp;
    return MFS_OK;
}

/* ==== token de commit (§9.2) ====
 * T1 (flash, 32 B): magic 'TO' | ver | txid | n_entries | crc_cuerpo |
 *                   merkle_root(32→trunc 8) | tfc_bits(4) | epoch | pad | crc32c
 * T0 (HMT, 16 B): magic | txid | crc_cuerpo | tfc(4) — escritura < 1 µs.
 */
mfs_st mfs_token_commit(mf_t *fs, uint32_t txid, uint32_t crc)
{
    uint8_t t1[MFS_TOKEN_T1_SIZE];
    memset(t1, 0xFFu, sizeof(t1));
    t1[0] = (uint8_t)(MFS_TOK_MAGIC >> 8); t1[1] = (uint8_t)MFS_TOK_MAGIC;
    t1[2] = 1u; t1[3] = 0u; /* version */
    mfs_st32(t1 + 4, txid);
    uint32_t n = 0;
    for (uint32_t i = 0; i < fs->wal_count; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        if (fs->wal[j].valid && fs->wal[j].txid == txid) n++;
    }
    mfs_st32(t1 + 8, n);
    mfs_st32(t1 + 12, crc);
    /* raíz Merkle del cuerpo de la transacción (§9.5) */
    memcpy(t1 + 16, fs->merkle_root, 8u);
    /* porteo del contador termométrico: 1 bit por intervalo de 16 commits */
    uint8_t tfcbuf[4];
    mfs_st16(tfcbuf, (uint16_t)(fs->tok_seq_a & 0xFFFFu));
    mfs_st16(tfcbuf + 2, (uint16_t)(fs->tok_seq_b & 0xFFFFu));
    memcpy(t1 + 24, tfcbuf, 4u);
    mfs_st32(t1 + 28, fs->epoch);
    uint32_t c = mfs_crc32c(t1, sizeof(t1) - 4u, 0u);
    mfs_st32(t1 + sizeof(t1) - 4u, c);

    /* 1) HMT: escribir ANTES en T0 si existe (MFS-HMT-001, latencia mínima) */
    if (mfs_has_t0(fs)) {
        uint8_t t0[MFS_TOKEN_T0_SIZE];
        memset(t0, 0xFFu, sizeof(t0));
        t0[0] = t1[0]; t0[1] = t1[1]; t0[2] = 1u; t0[3] = 0u;
        mfs_st32(t0 + 4, txid);
        mfs_st32(t0 + 8, crc);
        memcpy(t0 + 12, tfcbuf, 4u);
        uint32_t a = fs->tok_toggle ? fs->tok_seq_b : fs->tok_seq_a;
        mfs_st st0 = mfs_t0_write(fs, tok_slot_addr(fs, a % MFS_TOKEN_SLOTS),
                                  t0, sizeof(t0));
        if (st0 != MFS_OK) return st0;
    }

    /* 2) Espejo T1 en flash dentro de la zona WAL (append-only) */
    int z = mfs_zone_alloc_open(fs, 0u);
    if (z < 0) return MFS_ENOSPC;
    /* el token NO pasa por E2G (no es dato de usuario): escritura cruda
     * al write_ptr de la zona, manteniendo coherencia de índice de página */
    mfs_zone_t *zz = mfs_zone(fs, (uint32_t)z);
    if (zz->write_ptr + MFS_TOKEN_T1_SIZE > zz->size) return MFS_ENOSPC;
    mfs_st st = mfs_write(fs, zz->start_addr + zz->write_ptr, t1, sizeof(t1));
    if (st != MFS_OK) return st;
    zz->write_ptr += mfs_page_bytes(fs); /* avance a granularidad de página */

    /* 3) avanzar anillo A/B + TFC (carry cada 16 commits, §8.4) */
    if (fs->tok_toggle) fs->tok_seq_b++; else fs->tok_seq_a++;
    fs->tok_toggle ^= 1u;
    if ((fs->tok_seq_a % 16u) == 0u || (fs->tok_seq_b % 16u) == 0u) {
        /* carry termométrico: marca evento de fatiga de tokens */
        mfs_hct_event(fs, MFS_EV_CONSERVATIVE, fs->tok_seq_a + fs->tok_seq_b);
    }

    /* 4) marcar entradas como comprometidas en la ventana RAM */
    for (uint32_t i = 0; i < fs->wal_count; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        if (fs->wal[j].valid && fs->wal[j].txid == txid)
            fs->wal[j].committed = 1u;
    }
    fs->tx_open = false;
    fs->sp_depth = 0u;
    return MFS_OK;
}

/* leer el token vigente más reciente desde la zona WAL (búsqueda binaria
 * simplificada: escaneo acotado desde el final de la zona) */
static bool tok_read_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE])
{
    for (int zi = (int)MFS_MAX_ZONES - 1; zi >= 0; zi--) {
        mfs_zone_t *z = &fs->zones[zi];
        if (z->state == MFS_Z_EMPTY || z->state == MFS_Z_QUARANTINE) continue;
        uint32_t pb = mfs_page_bytes(fs);
        for (uint32_t off = z->write_ptr; off >= mfs_e2g_hdr(fs) + MFS_TOKEN_T1_SIZE; ) {
            off -= pb;
            uint8_t t1[MFS_TOKEN_T1_SIZE];
            if (mfs_read(fs, z->start_addr + off, t1, sizeof(t1)) != MFS_OK)
                continue;
            if (t1[0] != (uint8_t)(MFS_TOK_MAGIC >> 8)) continue;
            uint32_t c = mfs_crc32c(t1, sizeof(t1) - 4u, 0u);
            if (c != mfs_ld32(t1 + sizeof(t1) - 4u)) continue;
            memcpy(out, t1, sizeof(t1));
            return true;
        }
    }
    return false;
}

/* versión expuesta a fsck/full (§21.1): último token T1 válido */
bool mfs_tok_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE]);
bool mfs_tok_latest(mf_t *fs, uint8_t out[MFS_TOKEN_T1_SIZE])
{
    return tok_read_latest(fs, out);
}

/* ==== replay acotado BMT (§9.4, FSM §24.2 paso 5) ====
 * Estrategia: localizar último token T1 vigente; todas las WALENT con
 * txid == token.txid se aplican (redo); las de otros txid pendientes se
 * descartan (undo implícito: nunca llegaron a destino). Coste ≤ window·(read+apply).
 */
mfs_st mfs_wal_replay(mf_t *fs, uint32_t window)
{
    if (window > MFS_WAL_WINDOW_MAX) window = MFS_WAL_WINDOW_MAX;
    uint8_t t1[MFS_TOKEN_T1_SIZE];
    if (!tok_read_latest(fs, t1)) {
        /* sin token: FS limpia o primera vez — no hay nada que replayar */
        return MFS_OK;
    }
    uint32_t txid = mfs_ld32(t1 + 4);
    uint32_t body_crc = mfs_ld32(t1 + 12);
    uint32_t n = mfs_ld32(t1 + 8);
    if (n == 0u) return MFS_OK;
    if (n > window) return MFS_ECORRUPT; /* invariante T1.n <= W */

    fs->hct.power_events++; /* llegamos aquí tras un montaje no limpio */
    mfs_hct_event(fs, MFS_EV_POWER_LOSS, txid);

    /* escanear registros WALENT de la zona WAL buscando txid objetivo */
    uint32_t applied = 0, acc = 0xFFFFFFFFu;
    for (uint32_t zi = 0; zi < MFS_MAX_ZONES && applied < n; zi++) {
        mfs_zone_t *z = &fs->zones[zi];
        if (z->state == MFS_Z_EMPTY || z->state == MFS_Z_QUARANTINE) continue;
        uint32_t pb = mfs_page_bytes(fs);
        for (uint32_t off = mfs_e2g_hdr(fs); off + pb <= z->write_ptr; off += pb) {
            uint8_t kind; uint8_t gen, snap; uint16_t rlen;
            static uint8_t buf[MFS_CHUNK_EXTENDED];
            mfs_st st = mfs_rec_read(fs, (zi << 16) | (off / pb), 0xFFFFFFFFu,
                                     0xFFu, &kind, &gen, &snap, buf,
                                     (uint16_t)sizeof(buf), &rlen);
            if (st != MFS_OK || kind != MFS_RT_WALENT) continue;
            if (rlen < WALHDR) continue;
            uint32_t wtx = mfs_ld32(buf);
            if (wtx != txid) continue;
            uint32_t lba = mfs_ld32(buf + 4);
            uint16_t dlen = mfs_ld16(buf + 8);
            uint8_t dk = buf[10];
            if ((uint32_t)(WALHDR + dlen) > rlen) continue;
            /* redo: aplicar la entrada al LBA destino (re-escritura idempotente) */
            int tz = mfs_zone_alloc_open(fs, dk == MFS_RT_DATA ? 1u : 0u);
            if (tz < 0) return MFS_ENOSPC;
            uint32_t pp2;
            st = mfs_rec_write(fs, (uint32_t)tz, lba, dk, gen, snap, 0u, 0u,
                               buf + WALHDR, dlen, &pp2);
            if (st != MFS_OK) return st;
            acc = mfs_crc32c(buf + WALHDR, dlen, acc);
            applied++;
        }
    }
    if (applied != n) return MFS_ECORRUPT; /* token anuncia más entradas de las halladas */
    if (body_crc != 0u && acc != body_crc) {
        fs->hct.crc_errors++;
        return MFS_EBADMSG;
    }
    /* limpiar ventana RAM: todo lo replayado ya está en destino */
    fs->wal_head = 0; fs->wal_count = 0;
    memset(fs->wal, 0, sizeof(fs->wal));
    return MFS_OK;
}

/* ==== checkpoint §9.5 ====
 * Serializa la ventana WAL pendiente, calcula la raíz Merkle BLAKE3 del
 * cuerpo (hash por entrada + par-fold ascendente) y emite token sintético
 * ckpt. Tras checkpoint, wal_count puede reiniciarse (las páginas destino
 * ya están fijadas en l2p).
 */
mfs_st mfs_checkpoint_write(mf_t *fs)
{
    if (fs->wal_count == 0u) return MFS_OK;
    /* árbol Merkle incremental (§9.5): leaves = h(entry_i) */
    uint8_t lvl[64][32];
    uint32_t cnt = fs->wal_count;
    if (cnt > 64u) cnt = 64u; /* plegado por tramos: W grande se agrupa */
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        mfs_b3_256(NULL, 0u, (const uint8_t *)&fs->wal[j],
                   sizeof(fs->wal[j]), lvl[i]);
    }
    while (cnt > 1u) {
        uint32_t next = (cnt + 1u) / 2u;
        for (uint32_t i = 0; i + 1u < cnt; i += 2u) {
            uint8_t pair[64];
            memcpy(pair, lvl[i], 32u); memcpy(pair + 32, lvl[i + 1], 32u);
            mfs_b3_256(NULL, 0u, pair, 64u, lvl[i / 2u]);
        }
        if (cnt & 1u) memcpy(lvl[next - 1u], lvl[cnt - 1u], 32u);
        cnt = next;
    }
    memcpy(fs->merkle_root, lvl[0], 8u); /* truncación normativa 64 bits */

    /* token sintético de checkpoint (txid = 0xCHECKPOINT) */
    uint32_t saved_tx = fs->txid_cur;
    fs->txid_cur = 0xCECE0000u ^ (uint32_t)fs->seq;
    mfs_st st = mfs_token_commit(fs, fs->txid_cur, 0u);
    fs->txid_cur = saved_tx;
    if (st != MFS_OK) return st;

    fs->hct.scrub_done++;
    mfs_hct_event(fs, MFS_EV_MOUNT_OK, fs->wal_count);
    /* compactar ventana: conservar solo entradas no comprometidas */
    uint16_t w = 0;
    for (uint16_t i = 0; i < fs->wal_count; i++) {
        uint32_t j = (fs->wal_head + i) % MFS_WAL_WINDOW_MAX;
        if (fs->wal[j].valid && fs->wal[j].committed != 1u) {
            fs->wal[w++] = fs->wal[j];
        }
    }
    fs->wal_head = 0; fs->wal_count = w;
    return MFS_OK;
}

/* cargar estado tras montaje: encontrar token y validar raíz */
mfs_st mfs_checkpoint_load(mf_t *fs)
{
    uint8_t t1[MFS_TOKEN_T1_SIZE];
    if (!tok_read_latest(fs, t1)) return MFS_OK; /* volumen virgen */
    memcpy(fs->merkle_root, t1 + 16, 8u);
    fs->epoch = mfs_ld32(t1 + 28);
    return MFS_OK;
}
