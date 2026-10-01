/* mfs_zone.c — ZLF log-structured (§11.6, FSM §24.1), cabecera de zona (§22.2),
 * registro E2G (§8.2), mapa L2P, AGCB+ con GLD (§9.4/§11.2) y backpressure
 * (§7.4). */
#include "mfs_internal.h"
#include <string.h>

#define HDR_NOR MFS_E2G_NOR_HDR   /* 20 B */
#define HDR_NAND MFS_E2G_NAND_HDR /* 32 B */

/* Zona víctima del pase de GC en curso (0xFFFFFFFF = ninguna). Evita que la
 * relocalización contabilice como deuda la copia que se está reclamando. */
static uint32_t g_gc_victim = 0xFFFFFFFFu;

/* Guarda de reentrancia: impide que la GC invocada desde el asignador vuelva
 * a dispararse a sí misma. */
static bool g_in_gc = false;

/* Reserva de zonas para que la GC tenga siempre destino: la última zona de la
 * ventana queda excluida de la asignación normal y sólo la GC puede usarla;
 * al reclamar una víctima vuelve a quedar libre para la asignación normal. */
#define MFS_GC_RESERVE 1u

/* Límite de zonas asignables: se excluye la reserva de GC salvo cuando es la
 * propia GC quien asigna (destino de relocalización). */
static uint32_t zone_cap(const mf_t *fs);

static uint32_t zone_alloc_limit(const mf_t *fs) {
  uint32_t cap = zone_cap(fs);
  if (!g_in_gc && cap > MFS_GC_RESERVE)
    cap -= MFS_GC_RESERVE;
  return cap;
}

/* Zonas EMPTY asignables por llamantes normales (excluye la reserva de GC).
 * Es la magnitud que gobierna el disparo de la GC por reserva. */
static uint32_t zone_count_normal_free(const mf_t *fs) {
  uint32_t lim = zone_cap(fs);
  if (lim > MFS_GC_RESERVE)
    lim -= MFS_GC_RESERVE;
  uint32_t n = 0u;
  for (uint32_t i = 0; i < lim; i++)
    if (fs->zones[i].state == MFS_Z_EMPTY)
      n++;
  return n;
}

/* =====================================================================
 * L2P — mapa lógico→físico (tabla estática, sin heap, MFS-RES-001).
 * Clave: LBA lógico completo (datos = 0x200000|(ino<<8|vpage), meta =
 * 0x100000|ino, dirent = 0x300000|ino, WALENT = lba destino). Valor: ppage =
 * (zone<<16)|idx.
 * ===================================================================== */
/* Número de entradas (potencia de dos). El valor por defecto (4096 ≈ 32 KiB)
 * cubre el perfil embebido; los front-ends de host lo amplían por build
 * (-DMFS_L2P_SLOTS=N) para dimensionar el mapa al volumen declarado sin usar
 * heap (MFS-RES-001). */
#ifndef MFS_L2P_SLOTS
#define MFS_L2P_SLOTS 4096u
#endif
#if (MFS_L2P_SLOTS & (MFS_L2P_SLOTS - 1u)) != 0u
#error "MFS_L2P_SLOTS debe ser potencia de dos"
#endif
#define MFS_L2P_TOMB 0xFFFFFFFEu /* entrada retirada: la cadena continúa */
typedef struct {
  uint32_t lba;
  uint32_t ppage;
} l2p_ent_t;
static l2p_ent_t g_l2p[MFS_L2P_SLOTS];

/* Mezcla multiplicativa + plegado XOR: usa bits altos y bajos del LBA, de modo
 * que la distribución es uniforme para cualquier tamaño de tabla (el plegado
 * evita que con tablas > 4096 sólo se alcanzara el primer tramo). */
static uint32_t l2p_slot(uint32_t lba) {
  uint32_t h = lba * 2654435761u;
  h ^= h >> 15;
  return h & (MFS_L2P_SLOTS - 1u);
}

void mfs_l2p_reset(void) {
  for (uint32_t i = 0; i < MFS_L2P_SLOTS; i++) {
    g_l2p[i].lba = MFS_L2P_FREE;
    g_l2p[i].ppage = MFS_L2P_FREE;
  }
}

mfs_st mfs_l2p_put(uint32_t lba, uint32_t ppage) {
  uint32_t s = l2p_slot(lba);
  l2p_ent_t *tomb = NULL;
  for (uint32_t n = 0; n < MFS_L2P_SLOTS; n++) {
    l2p_ent_t *e = &g_l2p[(s + n) & (MFS_L2P_SLOTS - 1u)];
    if (e->lba == lba) {
      e->ppage = ppage;
      return MFS_OK;
    }
    if (e->lba == MFS_L2P_TOMB) {
      if (!tomb)
        tomb = e; /* reutilizable si la clave no está en la tabla */
      continue;
    }
    if (e->lba == MFS_L2P_FREE) {
      l2p_ent_t *dst = tomb ? tomb : e;
      dst->lba = lba;
      dst->ppage = ppage;
      return MFS_OK;
    }
  }
  if (tomb) { /* tabla sin huecos libres: se recicla una lápida */
    tomb->lba = lba;
    tomb->ppage = ppage;
    return MFS_OK;
  }
  return MFS_ETABLEFULL;
}

uint32_t mfs_l2p_get(uint32_t lba) {
  uint32_t s = l2p_slot(lba);
  for (uint32_t n = 0; n < MFS_L2P_SLOTS; n++) {
    l2p_ent_t *e = &g_l2p[(s + n) & (MFS_L2P_SLOTS - 1u)];
    if (e->lba == lba)
      return e->ppage;
    if (e->lba == MFS_L2P_FREE)
      return MFS_L2P_FREE;
  }
  return MFS_L2P_FREE;
}

/* Retira el LBA (truncado/borrado). La entrada queda como lápida: el sondeo
 * lineal de las demás claves que colisionan con ella debe continuar (vaciar el
 * hueco truncaría sus cadenas y haría perder páginas vivas). */
void mfs_l2p_drop(uint32_t lba) {
  uint32_t s = l2p_slot(lba);
  for (uint32_t n = 0; n < MFS_L2P_SLOTS; n++) {
    l2p_ent_t *e = &g_l2p[(s + n) & (MFS_L2P_SLOTS - 1u)];
    if (e->lba == lba) {
      e->lba = MFS_L2P_TOMB;
      e->ppage = MFS_L2P_FREE;
      return;
    }
    if (e->lba == MFS_L2P_FREE)
      return;
  }
}

/* Entradas totales del mapa (capacidad de direccionamiento del volumen). */
uint32_t mfs_l2p_capacity(void) { return MFS_L2P_SLOTS; }

/* Entradas ocupadas (lápidas incluidas: siguen consumiendo hueco de sondeo). */
uint32_t mfs_l2p_used(void) {
  uint32_t n = 0u;
  for (uint32_t i = 0; i < MFS_L2P_SLOTS; i++)
    if (g_l2p[i].lba != MFS_L2P_FREE)
      n++;
  return n;
}

/* Descarta las entradas cuyo destino vive en la zona indicada (tras erase). */
static void l2p_drop_zone(uint32_t zone) {
  for (uint32_t i = 0; i < MFS_L2P_SLOTS; i++) {
    if (g_l2p[i].lba != MFS_L2P_FREE && g_l2p[i].lba != MFS_L2P_TOMB &&
        (g_l2p[i].ppage >> 16) == zone) {
      g_l2p[i].lba = MFS_L2P_TOMB;
      g_l2p[i].ppage = MFS_L2P_FREE;
    }
  }
}

/* ==== geometría derivada del modo (§18.1 chunk 128..4096) ==== */
uint16_t mfs_e2g_hdr(mf_t *fs) {
  uint32_t t = fs->hwv.media_type;
  return (t == MFS_MEDIA_NAND_RAW || t == MFS_MEDIA_NAND_ONFI ||
          t == MFS_MEDIA_ZNS_NAND)
             ? HDR_NAND
             : HDR_NOR;
}

uint32_t mfs_page_bytes(mf_t *fs) { return mfs_limits[fs->mode].chunk_size; }

/* Capacidad útil de payload por registro = chunk − cabecera E2G (y margen del
 * tag AEAD si hay suite activa). Es la granularidad de página lógica de datos.
 */
uint32_t mfs_payload_bytes(mf_t *fs) {
  uint32_t pb = mfs_page_bytes(fs);
  uint32_t hs = mfs_e2g_hdr(fs);
  uint32_t tag =
      (fs->suite != (uint8_t)MFS_SUITE_NONE && fs->cfg->key) ? 16u : 0u;
  if (pb <= hs + tag)
    return 0u;
  return pb - hs - tag;
}

mfs_st mfs_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len) {
  const mfs_l2_driver *d = fs->cfg->drv;
  if (!d || !d->read)
    return MFS_EIO;
  mfs_port_crit_enter();
  mfs_st st = d->read(d->ctx, addr, dst, len);
  mfs_port_crit_exit();
  return st;
}

mfs_st mfs_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len) {
  const mfs_l2_driver *d = fs->cfg->drv;
  if (!d || !d->prog)
    return MFS_EIO;
  mfs_st st = d->prog(d->ctx, addr, src, len); /* WOB barrier (§20.3) */
  if (st == MFS_OK) {
    fs->hct.writes_prog++;
    fs->eld_spent_mj += mfs_energy_prog_mj(&fs->hwv, len);
  }
  return st;
}

mfs_st mfs_erase(mf_t *fs, uint32_t addr) {
  const mfs_l2_driver *d = fs->cfg->drv;
  if (!d || !d->erase)
    return MFS_EIO;
  return d->erase(d->ctx, addr);
}

bool mfs_has_t0(mf_t *fs) { return fs->cfg->drv_t0 && fs->hwv.t0_size > 0u; }

mfs_st mfs_t0_read(mf_t *fs, uint32_t addr, void *dst, uint32_t len) {
  if (!mfs_has_t0(fs))
    return MFS_ENOTSUP;
  return fs->cfg->drv_t0->read(fs->cfg->drv_t0->ctx, addr, dst, len);
}

mfs_st mfs_t0_write(mf_t *fs, uint32_t addr, const void *src, uint32_t len) {
  if (!mfs_has_t0(fs))
    return MFS_ENOTSUP;
  return fs->cfg->drv_t0->prog(fs->cfg->drv_t0->ctx, addr, src, len);
}

/* =====================================================================
 * Cabecera de zona (§22.2) — 64 B, programada al abrir la zona
 * ===================================================================== */
mfs_st mfs_zonehdr_write(mf_t *fs, mfs_zone_t *z) {
  uint8_t h[MFS_ZONEHDR_SIZE];
  memset(h, 0xFFu, sizeof(h));
  mfs_st16(h + 0, (uint16_t)MFS_ZONE_MAGIC);
  mfs_st16(h + 2, z->zone_id);
  h[4] = z->state;
  mfs_st32(h + 5, z->seq);
  h[9] = 0u; /* gen + WOM-gen (reservado) */
  mfs_st16(h + 10, 0u);
  mfs_st16(h + 12, (uint16_t)z->class_hot);
  mfs_st16(h + 14, z->valid_pages);
  mfs_st32(h + 16, mfs_crc32c(h, 16u, 0u));
  return mfs_write(fs, z->start_addr, h, MFS_ZONEHDR_SIZE);
}

mfs_st mfs_zonehdr_read(mf_t *fs, mfs_zone_t *z) {
  uint8_t h[MFS_ZONEHDR_SIZE];
  mfs_st st = mfs_read(fs, z->start_addr, h, sizeof(h));
  if (st != MFS_OK)
    return st;
  if (mfs_ld16(h) != MFS_ZONE_MAGIC)
    return MFS_EBADMSG;
  if (mfs_crc32c(h, 16u, 0u) != mfs_ld32(h + 16))
    return MFS_EBADMSG;
  z->zone_id = mfs_ld16(h + 2);
  z->state = h[4];
  z->seq = mfs_ld32(h + 5);
  z->class_hot = (uint8_t)(mfs_ld16(h + 12) & 0xFFu);
  z->valid_pages = mfs_ld16(h + 14);
  return MFS_OK;
}

/* ==== asignación de zona abierta por clase de hotness ==== */
static uint32_t zone_cap(const mf_t *fs) {
  return fs->zone_cap ? fs->zone_cap : MFS_MAX_ZONES;
}

int mfs_zone_alloc_open(mf_t *fs, uint8_t hotness) {
  uint32_t cap = zone_alloc_limit(fs);
  /* Reserva GC: antes de quedarse sin zonas asignables se reclama, de forma
   * que la GC siempre dispone de una zona destino donde relocalizar. */
  if (!g_in_gc && zone_count_normal_free(fs) == 0u)
    mfs_gc_force(fs);
  for (uint32_t i = 0; i < cap; i++) {
    mfs_zone_t *z = &fs->zones[i];
    if (z->state != MFS_Z_OPEN || z->class_hot != hotness)
      continue;
    if (z->write_ptr + mfs_page_bytes(fs) <= z->size)
      return (int)i;
    /* sin espacio para otra página: sellar para que la GC pueda reclamarla
     * (si se abandonara en estado OPEN quedaría invisible e irrecuperable) */
    (void)mfs_zone_seal(fs, i);
  }
  for (uint32_t i = 0; i < cap; i++) {
    mfs_zone_t *z = &fs->zones[i];
    if (z->state == MFS_Z_EMPTY) {
      z->zone_id = (uint16_t)i;
      z->state = MFS_Z_OPEN;
      z->class_hot = hotness;
      z->seq = (uint32_t)fs->seq++;
      uint32_t stride =
          (fs->zone_blocks ? fs->zone_blocks : 1u) * fs->hwv.erase_unit;
      z->start_addr = mfs_zone_base(&fs->hwv) + i * stride;
      z->size = stride;
      z->write_ptr = MFS_ZONEHDR_SIZE;
      z->total_pages =
          (uint16_t)((z->size - MFS_ZONEHDR_SIZE) / mfs_page_bytes(fs));
      z->valid_pages = 0u;
      z->zrp = 0u;
      if (mfs_zonehdr_write(fs, z) != MFS_OK) {
        z->state = MFS_Z_EMPTY;
        return -1;
      }
      if (fs->free_pages > 0u)
        fs->free_pages--;
      return (int)i;
    }
  }
  return -1; /* sin zonas libres → GC/backpressure caller */
}

mfs_zone_t *mfs_zone(mf_t *fs, uint32_t id) {
  if (id >= MFS_MAX_ZONES)
    return NULL;
  return &fs->zones[id];
}

mfs_st mfs_zone_seal(mf_t *fs, uint32_t id) {
  mfs_zone_t *z = mfs_zone(fs, id);
  if (!z || z->state != MFS_Z_OPEN)
    return MFS_ESTATE;
  z->state = MFS_Z_FULL; /* el estado se recalcula en el siguiente montaje */
  /* ZRP (§8.5): paridad RS(16,15) en zonas frías al sellar (off por defecto) */
  if (fs->cfg->zrp_enable && z->class_hot >= 2u && z->total_pages >= 17u)
    (void)mfs_zrp_encode(fs, z);
  return MFS_OK;
}

/* Borra todos los bloques que componen la zona (1..4, §8.1). */
mfs_st mfs_zone_erase(mf_t *fs, mfs_zone_t *z) {
  uint32_t nb = fs->zone_blocks ? fs->zone_blocks : 1u;
  for (uint32_t k = 0; k < nb; k++) {
    mfs_st st = mfs_erase(fs, z->start_addr + k * fs->hwv.erase_unit);
    if (st != MFS_OK)
      return st;
  }
  if (z->pe_cycles < 0xFFFFu)
    z->pe_cycles++; /* desgaste §11.1 */
  return MFS_OK;
}

/* Ventana idle para mantenimiento (SPDR §13.1): no hay trabajo RT-A pendiente
 * ni el sistema está en drenaje de emergencia. */
bool mfs_gc_rt_idle(const mf_t *fs) {
  if (!fs)
    return false;
  if (fs->edp_level >= 4u)
    return false;
  uint32_t pending_rt = 0;
  for (uint8_t i = 0; i < MFS_MAX_IOCB; i++) {
    const mfs_iocb *cb = fs->ring[i];
    if (cb && (cb->class_flags & 3u) == MFS_RT_A)
      pending_rt++;
  }
  return pending_rt == 0u;
}

uint32_t mfs_zone_count_free(mf_t *fs) {
  uint32_t n = 0, cap = zone_cap(fs);
  for (uint32_t i = 0; i < cap; i++)
    if (fs->zones[i].state == MFS_Z_EMPTY)
      n++;
  return n;
}

/* ==== escaneo de zonas al montar (FSM §24.2 paso 5) ==== */
mfs_st mfs_scan_zones(mf_t *fs) {
  uint32_t base = mfs_zone_base(&fs->hwv);
  uint32_t pb = mfs_page_bytes(fs);
  uint32_t stride =
      (fs->zone_blocks ? fs->zone_blocks : 1u) * fs->hwv.erase_unit;
  for (uint32_t i = 0; i < MFS_MAX_ZONES; i++) {
    mfs_zone_t *z = &fs->zones[i];
    memset(z, 0, sizeof(*z));
    z->zone_id = (uint16_t)i;
    z->state = MFS_Z_EMPTY;
    z->start_addr = base + i * stride;
    z->size = stride;
    z->total_pages = (uint16_t)((z->size - MFS_ZONEHDR_SIZE) / pb);
    if (i >= fs->zone_cap) {
      z->state = MFS_Z_QUARANTINE;
      continue;
    }
    /* ¿sector con cabecera de zona válida? */
    if (mfs_zonehdr_read(fs, z) != MFS_OK) {
      z->state = MFS_Z_EMPTY;
      continue;
    }
    /* recorrer páginas hasta el primer hueco erased (escritura secuencial) */
    uint32_t off = MFS_ZONEHDR_SIZE;
    uint16_t cnt = 0;
    while (off + pb <= z->size) {
      uint8_t m[2];
      if (mfs_read(fs, z->start_addr + off, m, 2u) != MFS_OK)
        break;
      if (m[0] != MFS_REC_MAGIC_HI || m[1] != MFS_REC_MAGIC_LO)
        break;
      off += pb;
      cnt++;
    }
    z->write_ptr = off;
    z->valid_pages = cnt;
    z->state = (off + pb <= z->size) ? MFS_Z_OPEN : MFS_Z_FULL;
  }
  fs->free_pages = mfs_zone_count_free(fs);
  return MFS_OK;
}

/* ==== construcción de cabecera E2G (§8.2) ==== */
static void build_hdr(uint8_t *h, uint16_t hs, uint8_t kind, uint8_t flags,
                      uint8_t dictid, uint32_t lba, uint8_t gen, uint8_t snapid,
                      uint8_t hotness, bool zrp, uint16_t plen) {
  h[0] = MFS_REC_MAGIC_HI;
  h[1] = MFS_REC_MAGIC_LO;
  h[2] = 1u; /* versión layout */
  h[3] = (uint8_t)((kind << 4) | (flags & 0x0Fu));
  h[4] = dictid;
  mfs_st32(h + 5, lba);
  h[9] = gen;
  h[10] = snapid;
  h[11] = hotness;
  h[12] = zrp ? 1u : 0u;
  mfs_st16(h + 13, plen);
  if (hs > 20u)
    memset(h + 15, 0, (size_t)(hs - 20u)); /* extensión NAND */
  mfs_st32(h + hs - 4u, 0u);
}

static uint32_t hdr_crc(const uint8_t *h, uint16_t hs) {
  return mfs_ld32(h + hs - 4u);
}

/* escribir una página completa (cabecera + payload + padding erased-safe) */
mfs_st mfs_rec_write(mf_t *fs, uint32_t zone, uint32_t lba, uint8_t kind,
                     uint8_t gen, uint8_t snapid, uint8_t hotness,
                     uint8_t dictid, const uint8_t *payload, uint16_t plen,
                     uint32_t *ppage_out) {
  mfs_zone_t *z = mfs_zone(fs, zone);
  if (!z || z->state != MFS_Z_OPEN)
    return MFS_ESTATE;
  uint16_t hs = mfs_e2g_hdr(fs);
  uint32_t pb = mfs_page_bytes(fs);
  if ((uint32_t)plen + (uint32_t)hs > pb)
    return MFS_EINVAL;
  if (z->write_ptr + pb > z->size) {
    mfs_zone_seal(fs, zone);
    return MFS_ENOSPC;
  }
  static uint8_t buf[MFS_CHUNK_EXTENDED];
  memset(buf, 0xFFu, pb);
  build_hdr(buf, hs, kind, 0u, dictid, lba, gen, snapid, hotness, z->zrp != 0u,
            plen);
  memcpy(buf + hs, payload, plen);
  uint32_t crc = mfs_crc32c(buf, (uint32_t)(hs - 4u), 0u);
  crc = mfs_crc32c(payload, plen, crc);
  mfs_st32(buf + hs - 4u, crc);

  /* cifrado AEAD por registro si suite activa (S0–S3), MFS-SEC-005 */
  if (fs->suite != (uint8_t)MFS_SUITE_NONE && fs->cfg->key) {
    uint16_t olen = 0;
    uint64_t nonce =
        ((uint64_t)fs->epoch << 32) |
        (uint64_t)(z->seq + (z->write_ptr - MFS_ZONEHDR_SIZE) / pb);
    mfs_st st = mfs_suite_seal(fs->suite, fs->cfg->key, nonce, buf + hs, plen,
                               buf + hs, &olen);
    if (st != MFS_OK)
      return st;
    if ((uint32_t)olen + hs > pb)
      return MFS_ENOSPC;
    /* el campo de longitud forma parte de la cabecera cubierta por el CRC:
     * debe fijarse ANTES de recalcularlo */
    mfs_st16(buf + 13, olen);
    crc = mfs_crc32c(buf, (uint32_t)(hs - 4u), 0u);
    crc = mfs_crc32c(buf + hs, olen, crc);
    mfs_st32(buf + hs - 4u, crc);
    plen = olen;
  }

  uint32_t addr = z->start_addr + z->write_ptr;
  uint32_t ppage = (zone << 16) | ((z->write_ptr - MFS_ZONEHDR_SIZE) / pb);
  mfs_st st = mfs_write(fs, addr, buf, pb);
  if (st != MFS_OK) {
    if (st == MFS_ECORRUPT) {
      z->state = MFS_Z_QUARANTINE;
      fs->hct.quarantined_blocks++;
      mfs_hct_event(fs, MFS_EV_QUARANTINE, addr);
    }
    return st;
  }
  fs->hct.writes_host++;
  if (kind == MFS_RT_DATA) { /* WAF: sólo datos de usuario cuentan como host */
  }

  /* L2P: este registro pasa a ser la versión vigente del LBA */
  uint32_t prev = mfs_l2p_get(lba);
  /* La copia superada no genera deuda si vive en la zona que la GC está
   * reclamando en este mismo pase (su deuda ya se salda página a página). */
  if (prev != MFS_L2P_FREE && prev != ppage && (prev >> 16) != g_gc_victim)
    mfs_gld_add(fs, 1u, kind != MFS_RT_DATA);
  st = mfs_l2p_put(lba, ppage);
  if (st != MFS_OK)
    return st;
  if (ppage_out)
    *ppage_out = ppage;
  z->write_ptr += pb;
  z->valid_pages++;
  fs->seq++;
  return MFS_OK;
}

/* leer un registro validando LBA+gen (E2G §12 niveles 4-5) */
mfs_st mfs_rec_read(mf_t *fs, uint32_t ppage, uint32_t expect_lba,
                    uint8_t expect_gen, uint8_t *kind, uint8_t *gen,
                    uint8_t *snapid, uint8_t *dictid, uint8_t *buf,
                    uint16_t bufsize, uint16_t *rlen) {
  uint32_t zone = ppage >> 16;
  uint32_t idx = ppage & 0xFFFFu;
  mfs_zone_t *z = mfs_zone(fs, zone);
  if (!z)
    return MFS_EINVAL;
  uint16_t hs = mfs_e2g_hdr(fs);
  uint32_t pb = mfs_page_bytes(fs);
  uint32_t addr = z->start_addr + MFS_ZONEHDR_SIZE + idx * pb;
  static uint8_t tmp[MFS_CHUNK_EXTENDED];
  if (pb > sizeof(tmp))
    return MFS_EINVAL;
  mfs_st st = mfs_read(fs, addr, tmp, pb);
  if (st != MFS_OK)
    return st;
  if (tmp[0] != MFS_REC_MAGIC_HI || tmp[1] != MFS_REC_MAGIC_LO)
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
  if (expect_gen != 0xFFu && g != expect_gen)
    return MFS_EBADMSG;
  if (kind)
    *kind = (uint8_t)(tmp[3] >> 4);
  if (gen)
    *gen = g;
  if (snapid)
    *snapid = tmp[10];
  if (dictid)
    *dictid = tmp[4];
  if (fs->suite != (uint8_t)MFS_SUITE_NONE && fs->cfg->key) {
    uint16_t olen = 0;
    uint64_t nonce = ((uint64_t)fs->epoch << 32) | (uint64_t)(z->seq + idx);
    st = mfs_suite_open(fs->suite, fs->cfg->key, nonce, tmp + hs, plen, buf,
                        &olen);
    if (st == MFS_ESECURITY_STATE) {
      fs->hct.e2g_failures++;
      mfs_hct_event(fs, MFS_EV_E2G_FAIL, addr);
      return st;
    }
    if (st != MFS_OK)
      return st;
    if (rlen)
      *rlen = olen;
    return MFS_OK;
  }
  if (plen > bufsize)
    return MFS_EOVERFLOW;
  memcpy(buf, tmp + hs, plen);
  if (rlen)
    *rlen = plen;
  return MFS_OK;
}

/* ==== GLD deuda + backpressure (§9.4, §7.4) ==== */
void mfs_gld_add(mf_t *fs, uint32_t pages, bool meta) {
  uint32_t w = meta ? 2u : 1u;
  uint32_t d = (uint32_t)fs->debt_gld + pages * w;
  fs->debt_gld = (d > 0xFFFFu) ? 0xFFFFu : (uint16_t)d;
}

/* selección victim por eficiencia (menor utilidad viva) — AGCB+ §11.2.
 * El baseline determinista es ELM (§11.1); PEP sólo refina en RT-C. */
static int pick_victim(mf_t *fs) {
  uint32_t cap = zone_cap(fs);
  for (uint32_t i = 0; i < cap; i++)
    if (fs->zones[i].state == MFS_Z_FULL && fs->zones[i].valid_pages == 0u)
      return (int)i; /* liberación directa */
  int v = mfs_gc_select_victim_ftl(fs, NULL, NULL);
  if (v >= 0)
    return v;
  /* reserva: selección por utilidad si FTL no aporta candidato */
  int best = -1;
  float bestscore = -1.0f;
  for (uint32_t i = 0; i < cap; i++) {
    mfs_zone_t *z = &fs->zones[i];
    if (z->state != MFS_Z_FULL)
      continue;
    float util =
        (float)z->valid_pages / (float)(z->total_pages ? z->total_pages : 1u);
    float score = 1.0f - util;
    if (score > bestscore) {
      bestscore = score;
      best = (int)i;
    }
  }
  return best;
}

/* Reajuste de la deuda GLD: cada página procesada por la GC (viva o
 * obsoleta) salda la deuda que generó al ser superada (§9.4). Sin esto la
 * deuda sólo crecería (las páginas obsoletas nunca se relocalizan). */
static void gld_relax(mf_t *fs, uint32_t n) {
  if (fs->debt_gld > n)
    fs->debt_gld = (uint16_t)(fs->debt_gld - n);
  else
    fs->debt_gld = 0u;
}

/* Liberación directa de una zona FULL sin ningún registro vivo: no requiere
 * relocalización, así que permite progresar incluso sin zona destino (caso
 * de emergencia cuando el medio está lleno de registros obsoletos). */
static bool gc_free_dead_zone(mf_t *fs) {
  uint32_t cap = zone_cap(fs);
  if (cap > MFS_GC_RESERVE)
    cap -= MFS_GC_RESERVE; /* la reserva no es asignable: liberar otra */
  uint32_t pb = mfs_page_bytes(fs);
  uint16_t hs = mfs_e2g_hdr(fs);
  for (uint32_t zi = 0; zi < cap; zi++) {
    mfs_zone_t *z = &fs->zones[zi];
    if (z->state != MFS_Z_FULL)
      continue;
    bool live = false;
    for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
      uint8_t raw[HDR_NAND];
      if (mfs_read(fs, z->start_addr + off, raw, hs) != MFS_OK)
        continue;
      if (raw[0] != MFS_REC_MAGIC_HI || raw[1] != MFS_REC_MAGIC_LO)
        continue;
      uint32_t ppage = (zi << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
      if (mfs_l2p_get(mfs_ld32(raw + 5)) == ppage) {
        live = true;
        break;
      }
    }
    if (live)
      continue;
    gld_relax(fs, (z->valid_pages != 0u) ? z->valid_pages : 1u);
    if (mfs_zone_erase(fs, z) != MFS_OK)
      continue;
    l2p_drop_zone(zi);
    z->state = MFS_Z_EMPTY;
    z->write_ptr = 0u;
    z->valid_pages = 0u;
    z->seq = 0u;
    fs->free_pages++;
    return true;
  }
  return false;
}

/* Nº de páginas de la zona que el L2P sigue considerando vigentes. Se usa como
 * invariante antes de borrar una zona: si alguna sobrevive a la relocalización,
 * el erase la perdería. */
static uint32_t zone_live_pages(mf_t *fs, const mfs_zone_t *z) {
  uint32_t pb = mfs_page_bytes(fs);
  uint16_t hs = mfs_e2g_hdr(fs);
  uint32_t live = 0u;
  for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
    uint8_t raw[HDR_NAND];
    if (mfs_read(fs, z->start_addr + off, raw, hs) != MFS_OK)
      continue;
    if (raw[0] != MFS_REC_MAGIC_HI || raw[1] != MFS_REC_MAGIC_LO)
      continue;
    uint32_t ppage =
        ((uint32_t)z->zone_id << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
    if (mfs_l2p_get(mfs_ld32(raw + 5)) == ppage)
      live++;
  }
  return live;
}

/* GC slice acotado por presupuesto temporal (§11.2 AGCB+) */
mfs_st mfs_gc_slice(mf_t *fs, uint32_t budget_us) {
  uint32_t t0 = mfs_port_time_us();
  /* Sin zona libre: primero se intenta liberar una zona totalmente obsoleta,
   * que no necesita destino de relocalización. */
  if (mfs_zone_count_free(fs) == 0u && gc_free_dead_zone(fs))
    return MFS_OK;
  int v = pick_victim(fs);
  if (v < 0)
    return MFS_OK;
  mfs_zone_t *z = &fs->zones[v];
  uint32_t pb = mfs_page_bytes(fs);
  uint16_t hs = mfs_e2g_hdr(fs);
  static uint8_t payload[MFS_CHUNK_EXTENDED];
  mfs_st ret = MFS_OK;
  g_gc_victim = (uint32_t)v;
  for (uint32_t off = MFS_ZONEHDR_SIZE; off + pb <= z->write_ptr; off += pb) {
    if (mfs_port_time_us() - t0 > budget_us) {
      ret = MFS_ETIMEDOUT_BUDGET;
      goto done;
    }
    uint32_t ppage = ((uint32_t)v << 16) | ((off - MFS_ZONEHDR_SIZE) / pb);
    uint8_t raw[HDR_NAND];
    mfs_st sth = mfs_read(fs, z->start_addr + off, raw, hs);
    if (sth != MFS_OK || raw[0] != MFS_REC_MAGIC_HI)
      continue;
    gld_relax(fs, 1u); /* página de la víctima: su deuda queda saldada */
    uint32_t lba = mfs_ld32(raw + 5);
    if (mfs_l2p_get(lba) != ppage)
      continue; /* versión obsoleta */
    uint8_t k, g, s, dict;
    uint16_t rlen = 0;
    sth = mfs_rec_read(fs, ppage, lba, 0xFFu, &k, &g, &s, &dict, payload,
                       (uint16_t)sizeof(payload), &rlen);
    if (sth != MFS_OK)
      continue; /* registro ilegible: se descarta */
    /* `mfs_rec_read` entrega el payload tal cual está en el medio (incluido el
     * flag `dict` de compresión), por lo que la relocalización es una copia
     * literal del registro: no hay que re-aplicar el pipeline. */
    int nz = mfs_zone_alloc_open(fs, z->class_hot);
    if (nz < 0) {
      ret = MFS_ENOSPC;
      goto done;
    }
    mfs_st st2 = mfs_rec_write(fs, (uint32_t)nz, lba, k, g, s, z->class_hot,
                               dict, payload, rlen, NULL);
    if (st2 != MFS_OK) {
      ret = st2;
      goto done;
    }
    if (z->valid_pages > 0u)
      z->valid_pages--;
    fs->hct.gc_relocated++;
  }
  /* Borrar la víctima y limpiar su rastro en L2P. Antes de programar el erase
   * se comprueba que no quede ninguna página viva sin relocalizar: borrarlas
   * las perdería silenciosamente (el L2P quedaría apuntando a 0xFF). */
  if (zone_live_pages(fs, z) != 0u) {
    ret = MFS_ESTATE;
    goto done;
  }
  {
    mfs_st est = mfs_zone_erase(fs, z);
    if (est != MFS_OK) {
      z->state = MFS_Z_QUARANTINE;
      fs->hct.quarantined_blocks++;
      mfs_hct_event(fs, MFS_EV_QUARANTINE, z->start_addr);
      ret = est;
      goto done;
    }
  }
  l2p_drop_zone((uint32_t)v);
  z->state = MFS_Z_EMPTY;
  z->write_ptr = 0;
  z->valid_pages = 0;
  z->seq = 0;
  fs->free_pages++;
done:
  g_gc_victim = 0xFFFFFFFFu;
  return ret;
}

/* Reclamación forzosa: se invoca cuando no hay zona libre asignable,
 * independientemente de la heurística de deuda GLD (que se reinicia tras un
 * montaje, de modo que sin esto un medio lleno quedaría inescribible). */
void mfs_gc_force(mf_t *fs) {
  if (!fs || g_in_gc)
    return;
  if (fs->eld_budget_mj != 0u && fs->eld_spent_mj > fs->eld_budget_mj)
    return; /* MFS-ELD-001 */
  g_in_gc = true;
  for (uint32_t i = 0; i < 4u; i++) {
    if (zone_count_normal_free(fs) > 0u)
      break;
    uint32_t free_before = zone_count_normal_free(fs);
    uint32_t rc_before = fs->hct.gc_relocated;
    (void)mfs_gc_slice(fs, 3000u);
    if (zone_count_normal_free(fs) == free_before &&
        fs->hct.gc_relocated == rc_before)
      break; /* sin progreso */
  }
  g_in_gc = false;
}

/* Disparador de GC por deuda GLD con puerta ELD (§9.4/§13.3) */
void mfs_gld_maybe_gc(mf_t *fs) {
  if (!fs || g_in_gc)
    return;
  if (fs->debt_gld < fs->d_max)
    return;
  if (fs->eld_budget_mj != 0u && fs->eld_spent_mj > fs->eld_budget_mj) {
    mfs_hct_event(fs, MFS_EV_ELD_DEFER, fs->debt_gld);
    return; /* MFS-ELD-001: mantenimiento diferido con MFS_EENERGY visible */
  }
  /* Nota: no se exige zona libre; la selección de víctima puede liberar
   * directamente zonas sin páginas vivas, y la relocalización queda acotada
   * por el presupuesto temporal si no hay destino disponible. */
  /* Drenaje acotado: se itera hasta bajar de D_max o agotar el cupo de
   * slices / dejar de progresar (§9.4 AGCB+). */
  g_in_gc = true;
  for (uint32_t i = 0; i < 16u && fs->debt_gld >= fs->d_max; i++) {
    uint16_t debt_before = fs->debt_gld;
    uint32_t free_before = mfs_zone_count_free(fs);
    (void)mfs_gc_slice(fs, 500u);
    if (fs->debt_gld == debt_before && mfs_zone_count_free(fs) == free_before)
      break; /* sin progreso: no insistir */
  }
  g_in_gc = false;
}

/* modelo energético para ELD (§13.3): P·t aproximado por página programada */
uint32_t mfs_energy_prog_mj(const mfs_hwv_t *hwv, uint32_t bytes) {
  uint32_t pages = bytes / 256u + 1u;
  uint32_t mj = pages * (hwv->t_prog_max_us ? hwv->t_prog_max_us : 700u) / 100u;
  return mj;
}
