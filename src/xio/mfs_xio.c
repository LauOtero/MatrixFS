/* mfs_xio.c — Aceleración por hardware y reglas de bus (§14.1, §11.5):
 *   - XDAM (MFS-XDAM-001): activos read-only accesibles por XIP con
 *     verificación E2G por muestreo (política 1/n) e invalidación por época.
 *   - SDP (MFS-SDP-001): captura sensor → DMA → POOL_SDP con CRC en tránsito
 *     → escritura O_RAW al WAL (la IRQ de completación se modela con
 *     mfs_sdp_feed, sin heap y determinista).
 *   - CQE (§11.5): cola HW de hasta 16 tareas mapeando iocbs DAIO; RT-A
 *     conserva escritura directa (sin reordenar).
 */
#include "mfs_internal.h"
#include <string.h>

/* ================================ XDAM =================================== */
#define XDAM_SAMPLE_EVERY 16u

static uint32_t xdam_crc_ref[MFS_XDAM_MAX];

void mfs_xdam_reset(mf_t *fs) {
  if (!fs)
    return;
  memset(fs->xdam, 0, sizeof(fs->xdam));
  memset(xdam_crc_ref, 0, sizeof(xdam_crc_ref));
  fs->xdam_epoch = 0u;
}

mfs_st mfs_xdam_map(mf_t *fs, uint32_t asset, uint32_t flash_addr,
                    uint32_t len) {
  if (!fs || asset >= MFS_XDAM_MAX || len == 0u)
    return MFS_EINVAL;
  if (!(fs->hwv.flags1 & MFS_HWV1_BYTE_ADDR) && !fs->cfg->drv->read)
    return MFS_EHW_UNSUPPORTED; /* XIP requiere lectura directa */
  /* CRC de referencia del activo (una única lectura de alta coste al mapear) */
  static uint8_t buf[MFS_CHUNK_EXTENDED];
  uint32_t done = 0u, crc = 0u;
  while (done < len) {
    uint32_t take = len - done;
    if (take > sizeof(buf))
      take = sizeof(buf);
    mfs_st st = mfs_read(fs, flash_addr + done, buf, take);
    if (st != MFS_OK)
      return st;
    crc = mfs_crc32c(buf, take, crc);
    done += take;
  }
  fs->xdam[asset].flash_addr = flash_addr;
  fs->xdam[asset].len = len;
  fs->xdam[asset].epoch = fs->xdam_epoch;
  fs->xdam[asset].samples = 0u;
  fs->xdam[asset].used = 1u;
  xdam_crc_ref[asset] = crc;
  mfs_hct_event(fs, MFS_EV_XDAM, asset);
  return MFS_OK;
}

mfs_st mfs_xdam_read(mf_t *fs, uint32_t asset, uint32_t off, void *dst,
                     uint32_t len) {
  if (!fs || !dst || asset >= MFS_XDAM_MAX)
    return MFS_EINVAL;
  mfs_xdam_ent_t *e = &fs->xdam[asset];
  if (!e->used)
    return MFS_ENOENT;
  if (e->epoch != fs->xdam_epoch) { /* invalidación por época obligatoria */
    e->used = 0u;
    fs->hct.xdam_invalid++;
    mfs_hct_event(fs, MFS_EV_XDAM, 0x8000u | asset);
    return MFS_ESTATE;
  }
  if (off + len > e->len)
    return MFS_EINVAL;
  e->samples++;
  /* verificación E2G por muestreo (política 1/n, auditable en HCT) */
  if ((e->samples % XDAM_SAMPLE_EVERY) == 0u) {
    static uint8_t buf[MFS_CHUNK_EXTENDED];
    uint32_t crc = 0u, done = 0u;
    while (done < e->len) {
      uint32_t take = e->len - done;
      if (take > sizeof(buf))
        take = sizeof(buf);
      mfs_st st = mfs_read(fs, e->flash_addr + done, buf, take);
      if (st != MFS_OK)
        return st;
      crc = mfs_crc32c(buf, take, crc);
      done += take;
    }
    fs->hct.xdam_samples++;
    if (crc != xdam_crc_ref[asset]) {
      e->used = 0u;
      fs->hct.xdam_invalid++;
      mfs_hct_event(fs, MFS_EV_XDAM, 0xC000u | asset);
      return MFS_ECORRUPT;
    }
  }
  return mfs_read(fs, e->flash_addr + off, dst, len);
}

void mfs_xdam_epoch_bump(mf_t *fs) {
  if (!fs)
    return;
  fs->xdam_epoch++;
  /* El cambio de época invalida lógicamente todos los activos XDAM; la
   * invalidación efectiva ocurre en el primer acceso (MFS-XDAM-001). */
  mfs_hct_event(fs, MFS_EV_XDAM, fs->epoch);
}

/* ================================= SDP =================================== */
#define SDP_POOL 4u

static uint8_t sdp_pool[SDP_POOL][MFS_CHUNK_EXTENDED];
static uint16_t sdp_len[SDP_POOL];
static uint32_t sdp_crc[SDP_POOL];
static uint8_t sdp_head, sdp_tail;

mfs_st mfs_sdp_init(mf_t *fs, mf_t *target, uint16_t mtu) {
  if (!fs || !target)
    return MFS_EINVAL;
  uint32_t pb = mfs_page_bytes(target);
  if (mtu == 0u || mtu > pb)
    return MFS_EINVAL;
  fs->sdp_target = target;
  fs->sdp_mtu = mtu;
  fs->sdp_frames = 0u;
  fs->sdp_dropped = 0u;
  sdp_head = sdp_tail = 0u;
  mfs_hct_event(fs, MFS_EV_SDP, mtu);
  return MFS_OK;
}

/* IRQ de captura: DMA + CRC en tránsito → POOL_SDP → O_RAW al WAL */
mfs_st mfs_sdp_feed(mf_t *fs, const void *sample, uint16_t len) {
  if (!fs || !sample || !fs->sdp_target)
    return MFS_EINVAL;
  if (len == 0u || len > fs->sdp_mtu) {
    fs->sdp_dropped++;
    return MFS_EINVAL;
  }
  uint8_t next = (uint8_t)((sdp_head + 1u) % SDP_POOL);
  if (next == sdp_tail) { /* pool lleno: backpressure */
    fs->sdp_dropped++;
    mfs_hct_event(fs, MFS_EV_SDP, 0x8000u | fs->sdp_dropped);
    return MFS_EBACKPRESSURE;
  }
  mf_t *t = (mf_t *)fs->sdp_target;
  /* CRC en tránsito: se prefiere el acelerador DMA-CRC si está disponible */
  uint32_t crc = 0u;
  const mfs_l2_driver *d = t->cfg->drv;
  if (d && d->dma_crc) {
    if (d->dma_crc(d->ctx, (uint32_t)(uintptr_t)sample, len, &crc) != MFS_OK)
      crc = mfs_crc32c((const uint8_t *)sample, len, 0u);
  } else {
    crc = mfs_crc32c((const uint8_t *)sample, len, 0u);
  }
  memcpy(sdp_pool[sdp_head], sample, len);
  sdp_len[sdp_head] = len;
  sdp_crc[sdp_head] = crc;
  sdp_head = next;

  /* O_RAW al WAL: registro directo sin caché (E2G obligatorio) */
  uint32_t seq = fs->sdp_frames;
  mfs_st st = mfs_wal_append(
      t, 0x600000u + (seq & 0xFFFFu), MFS_RT_DATA,
      sdp_pool[(uint8_t)(sdp_head + SDP_POOL - 1u) % SDP_POOL], len, NULL);
  if (st != MFS_OK) {
    fs->sdp_dropped++;
    return st;
  }
  /* completada la escritura O_RAW, el slot del pool se libera */
  sdp_tail = sdp_head;
  fs->sdp_frames++;
  mfs_hct_event(fs, MFS_EV_SDP, fs->sdp_frames);
  return MFS_OK;
}

/* ================================= CQE =================================== */
mfs_st mfs_cqe_init(mf_t *fs, uint8_t depth) {
  if (!fs)
    return MFS_EINVAL;
  if (depth == 0u || depth > MFS_CQE_MAX)
    return MFS_EINVAL;
  if (!(fs->hwv.flags3 & MFS_HWV3_CQE))
    return MFS_EHW_UNSUPPORTED;
  memset(fs->cqe_q, 0, sizeof(fs->cqe_q));
  fs->cqe_depth = depth;
  fs->cqe_head = fs->cqe_tail = 0u;
  fs->cqe_completed = 0u;
  mfs_hct_event(fs, MFS_EV_CQE, depth);
  return MFS_OK;
}

mfs_st mfs_cqe_submit(mf_t *fs, mfs_iocb *cb) {
  if (!fs || !cb)
    return MFS_EINVAL;
  if (fs->cqe_depth == 0u)
    return MFS_ENOTSUP;
  uint8_t next = (uint8_t)((fs->cqe_head + 1u) % fs->cqe_depth);
  if (next == fs->cqe_tail)
    return MFS_EBACKPRESSURE; /* cola HW llena */
  fs->cqe_q[fs->cqe_head] = cb;
  cb->status = 0;
  fs->cqe_head = next;
  return MFS_OK;
}

int mfs_cqe_poll(mf_t *fs, mfs_iocb **done, int max) {
  if (!fs || fs->cqe_depth == 0u)
    return 0;
  int n = 0;
  while (fs->cqe_tail != fs->cqe_head && n < max) {
    mfs_iocb *cb = fs->cqe_q[fs->cqe_tail];
    fs->cqe_q[fs->cqe_tail] = NULL;
    fs->cqe_tail = (uint8_t)((fs->cqe_tail + 1u) % fs->cqe_depth);
    /* RT-A mantiene escritura directa (reliable write), sin reordenación */
    uint8_t cls = (uint8_t)(cb->class_flags & 3u);
    (void)cls;
    (void)mf_submit(fs, cb);
    (void)mf_poll(fs, NULL, 1, 1000u);
    fs->cqe_completed++;
    if (done)
      done[n] = cb;
    n++;
  }
  if (n)
    fs->hct.cqe_completed = fs->cqe_completed;
  return n;
}
