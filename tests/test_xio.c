/* test_xio.c — E/S acelerada (§14.1, §11.5): XDAM, SDP y CQE
 *
 * Los tres subsistemas comparten la misma exigencia normativa: si el
 * acelerador no existe, el sistema lo declara (MFS_EHW_UNSUPPORTED) y nunca
 * entrega datos sin verificar.
 */
#include "mfs_harness.h"
#include "mfs_test.h"

void test_xio_xdam(void) {
  TEST_BEGIN("XDAM: activo XIP con muestreo E2G e invalidación por época");
  mfs_env_t e;
  CHECK(env_open(&e, 32768u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  mfs_xdam_reset(&e.fs);
  CHECK_EQ(e.fs.xdam_epoch, 0u);

  /* Región al final del medio: fuera de todas las zonas ZLF ⇒ erased. */
  uint32_t addr = e.vf.size - 64u;
  uint8_t buf[64];
  CHECK_EQ(mfs_xdam_map(&e.fs, 0u, addr, (uint32_t)sizeof(buf)), MFS_OK);
  CHECK_EQ(e.fs.xdam[0].used, 1u);
  CHECK_EQ(e.fs.xdam[0].len, (uint32_t)sizeof(buf));

  CHECK_EQ(mfs_xdam_read(&e.fs, 0u, 0u, buf, (uint32_t)sizeof(buf)), MFS_OK);
  bool erased = true;
  for (uint32_t i = 0; i < sizeof(buf); i++)
    if (buf[i] != 0xFFu)
      erased = false;
  CHECK(erased);

  /* argumentos inválidos y activos no mapeados */
  CHECK_EQ(mfs_xdam_read(&e.fs, 1u, 0u, buf, 8u), MFS_ENOENT);
  CHECK_EQ(mfs_xdam_map(&e.fs, MFS_XDAM_MAX, addr, 8u), MFS_EINVAL);
  CHECK_EQ(mfs_xdam_map(&e.fs, 0u, addr, 0u), MFS_EINVAL);
  CHECK_EQ(mfs_xdam_read(&e.fs, 0u, 60u, buf, 64u), MFS_EINVAL);

  /* el cambio de época invalida lógicamente; el primer acceso lo materializa */
  mfs_xdam_epoch_bump(&e.fs);
  CHECK_EQ(mfs_xdam_read(&e.fs, 0u, 0u, buf, 8u), MFS_ESTATE);
  CHECK_EQ(e.fs.xdam[0].used, 0u);

  /* corrupción del medio detectada en el muestreo 1/16 (nunca dato erróneo) */
  CHECK_EQ(mfs_xdam_map(&e.fs, 0u, addr, (uint32_t)sizeof(buf)), MFS_OK);
  uint8_t *cell = vf_raw(&e.vf, addr);
  CHECK(cell != NULL);
  if (cell)
    *cell ^= 0x01u;
  int rc = MFS_OK;
  for (uint32_t i = 0; i < 16u; i++)
    rc = mfs_xdam_read(&e.fs, 0u, 0u, buf, (uint32_t)sizeof(buf));
  CHECK_EQ(rc, MFS_ECORRUPT);
  CHECK_EQ(e.fs.xdam[0].used, 0u);
  CHECK_EQ(mfs_xdam_read(&e.fs, 0u, 0u, buf, 8u), MFS_ENOENT);
  env_close(&e);
  TEST_END("XDAM");
}

void test_xio_sdp(void) {
  TEST_BEGIN("SDP: sensor → DMA/CRC → pool → O_RAW al WAL");
  mfs_env_t e;
  CHECK(env_open(&e, 32768u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* MTU inválido o destino ausente ⇒ EINVAL explícito */
  CHECK_EQ(mfs_sdp_init(&e.fs, &e.fs, 0u), MFS_EINVAL);
  CHECK_EQ(mfs_sdp_init(&e.fs, NULL, 16u), MFS_EINVAL);

  CHECK_EQ(mfs_sdp_init(&e.fs, &e.fs, 64u), MFS_OK);
  CHECK_EQ(e.fs.sdp_mtu, 64u);
  CHECK_EQ(e.fs.sdp_frames, 0u);
  CHECK_EQ(e.fs.sdp_dropped, 0u);

  uint8_t sample[64];
  for (uint32_t i = 0; i < sizeof(sample); i++)
    sample[i] = (uint8_t)(i * 3u + 1u);

  CHECK_EQ(mfs_sdp_feed(&e.fs, sample, 64u), MFS_OK);
  CHECK_EQ(e.fs.sdp_frames, 1u);
  CHECK_EQ(e.fs.sdp_dropped, 0u);

  /* trama mayor que el MTU: se descarta y se contabiliza */
  CHECK_EQ(mfs_sdp_feed(&e.fs, sample, 65u), MFS_EINVAL);
  CHECK_EQ(e.fs.sdp_dropped, 1u);
  CHECK_EQ(mfs_sdp_feed(&e.fs, NULL, 8u), MFS_EINVAL);

  /* ráfaga de capturas: el pool se libera tras cada O_RAW */
  for (uint32_t i = 0; i < 12u; i++)
    CHECK_EQ(mfs_sdp_feed(&e.fs, sample, 32u), MFS_OK);
  CHECK_EQ(e.fs.sdp_frames, 13u);
  CHECK_EQ(e.fs.sdp_dropped, 1u);
  env_close(&e);
  TEST_END("SDP");
}

void test_xio_cqe(void) {
  TEST_BEGIN("CQE: cola HW de DAIO (RT-A sin reordenar)");
  mfs_env_t e;
  CHECK(env_open(&e, 32768u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* sin capacidad declarada el acelerador no se finge */
  CHECK_EQ(mfs_cqe_init(&e.fs, 4u), MFS_EHW_UNSUPPORTED);
  env_set_assets(&e, 0u, MFS_HWV3_CQE);
  CHECK_EQ(mfs_cqe_init(&e.fs, 4u), MFS_OK);
  CHECK_EQ(e.fs.cqe_depth, 4u);

  /* profundidades fuera de rango */
  CHECK_EQ(mfs_cqe_init(&e.fs, 0u), MFS_EINVAL);
  CHECK_EQ(mfs_cqe_init(&e.fs, (uint8_t)(MFS_CQE_MAX + 1u)), MFS_EINVAL);
  CHECK_EQ(mfs_cqe_init(&e.fs, 4u), MFS_OK);

  /* fichero de trabajo con contenido conocido */
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/cqe.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  uint8_t data[64];
  for (uint32_t i = 0; i < sizeof(data); i++)
    data[i] = (uint8_t)(0xA0u + i);
  size_t wr = 0;
  CHECK_EQ(mf_write(f, data, sizeof(data), &wr), MFS_OK);
  mf_close(f);
  CHECK_EQ(mf_sync(&e.fs), MFS_OK);
  CHECK_EQ(mf_open(&e.fs, "/cqe.bin", MFS_O_RDONLY, &f), MFS_OK);

  /* El contrato DAIO es cb->buf → { mfs_file *f; uint8_t datos[]; }: el
   * descriptor va delante del búfer de datos. */
  struct io_buf {
    mfs_file *f;
    uint8_t data[64];
  } b0, b1, b2;
  memset(&b0, 0, sizeof(b0));
  memset(&b1, 0, sizeof(b1));
  memset(&b2, 0, sizeof(b2));
  b0.f = b1.f = b2.f = f;

  mfs_iocb cb[3];
  memset(cb, 0, sizeof(cb));
  void *bufs[3];
  bufs[0] = &b0;
  bufs[1] = &b1;
  bufs[2] = &b2;
  for (uint32_t i = 0; i < 3u; i++) {
    cb[i].op = MFS_AREAD;
    cb[i].buf = bufs[i];
    cb[i].len = 32u;
    cb[i].file_off = 0u;
    cb[i].class_flags = (uint16_t)MFS_RT_C;
  }
  CHECK_EQ(mfs_cqe_submit(&e.fs, &cb[0]), MFS_OK);
  CHECK_EQ(mfs_cqe_submit(&e.fs, &cb[1]), MFS_OK);
  CHECK_EQ(mfs_cqe_submit(&e.fs, &cb[2]), MFS_OK);
  CHECK_EQ(mfs_cqe_submit(&e.fs, &cb[0]), MFS_EBACKPRESSURE);

  /* el drenaje completa todo lo pendiente y contabiliza en HCT */
  mfs_iocb *done[4];
  CHECK_EQ(mfs_cqe_poll(&e.fs, done, 4), 3);
  CHECK_EQ(e.fs.cqe_completed, 3u);
  CHECK_EQ(e.fs.hct.cqe_completed, 3u);
  CHECK_EQ(mfs_cqe_poll(&e.fs, done, 4), 0);

  /* los datos leídos son los del fichero */
  CHECK(memcmp(b0.data, data, 32u) == 0);
  CHECK(memcmp(b1.data, data, 32u) == 0);
  CHECK(memcmp(b2.data, data, 32u) == 0);

  mf_close(f);
  env_close(&e);
  TEST_END("CQE");
}
