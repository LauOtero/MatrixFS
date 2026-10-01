/* test_ftl2.c — FTL Ultra 2 (§11): WOM-p, SLEC, EBA, RAS/TG, ELM/PEP, WEP, ZRP
 */
#include "mfs_harness.h"
#include "mfs_test.h"

#define RAM_ULTRA_NANO 8192u /* chunk 128 B ⇒ 31 páginas/zona: permite ZRP */
#define RAM_COMPACT 32768u

static void fill(uint8_t *b, uint32_t n, uint8_t seed) {
  for (uint32_t i = 0; i < n; i++)
    b[i] = (uint8_t)(seed + i * 7u);
}

/* ------------------------------- WOM-p -------------------------------- */
void test_ftl_wom(void) {
  TEST_BEGIN("WOM-p: reescritura program-only multigeneración (§11.4)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* región virgen dentro de la zona más alta */
  uint32_t stride = (uint32_t)e.fs.zone_blocks * e.fs.hwv.erase_unit;
  uint32_t addr = mfs_zone_base(&e.fs.hwv) + 120u * stride;

  mfs_wom_t w;
  CHECK_EQ(mfs_wom_begin(&e.fs, addr, 64u, 2u, &w), MFS_OK);

  uint8_t p0[8], p1[8], out[8];
  fill(p0, 8u, 0x11);
  fill(p1, 8u, 0x22);
  CHECK_EQ(mfs_wom_program(&e.fs, &w, p0, 64u), MFS_OK);
  CHECK_EQ(w.gen, 1u);
  CHECK_EQ(mfs_wom_read(&e.fs, &w, out, 64u), MFS_OK);
  CHECK(memcmp(out, p0, 8u) == 0);

  CHECK_EQ(mfs_wom_program(&e.fs, &w, p1, 64u), MFS_OK);
  CHECK_EQ(w.gen, 2u);
  CHECK_EQ(mfs_wom_read(&e.fs, &w, out, 64u), MFS_OK);
  CHECK(memcmp(out, p1, 8u) == 0);

  CHECK(mfs_wom_exhausted(&w));
  CHECK_EQ(mfs_wom_program(&e.fs, &w, p1, 64u), MFS_EBACKPRESSURE);
  CHECK_EQ(e.vf.n_violations, 0u); /* nunca reprograma celdas sin borrar */
  env_close(&e);
  TEST_END("WOM-p");
}

/* ------------------------------- SLEC --------------------------------- */
void test_ftl_slec(void) {
  TEST_BEGIN("SLEC: ventana SLC y plegado bajo ELD (§11.7)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* con NOR no aplica (sólo NAND multinivel sin T0) */
  CHECK(!mfs_slec_enabled(&e.fs));
  CHECK_EQ(mfs_slec_enter(&e.fs, 0u), MFS_ENOTSUP);

  /* política: se simula medio NAND y páginas SLC pendientes */
  e.fs.hwv.media_type = MFS_MEDIA_NAND_RAW;
  CHECK(mfs_slec_enabled(&e.fs));
  CHECK_EQ(mfs_slec_enter(&e.fs, 0u), MFS_OK);
  e.fs.slec_pages = 4u;
  CHECK_EQ(mfs_slec_fold(&e.fs, 1000u), MFS_OK);
  CHECK_EQ(e.fs.slec_pages, 3u); /* ≤1 bloque por ventana */
  CHECK_EQ(e.fs.hct.slec_folded, 1u);

  /* ELD agotado ⇒ diferido (MFS-EENERGY) */
  e.fs.eld_budget_mj = 10u;
  e.fs.eld_spent_mj = 999u;
  CHECK_EQ(mfs_slec_fold(&e.fs, 1000u), MFS_EENERGY);
  env_close(&e);
  TEST_END("SLEC");
}

/* -------------------------------- EBA --------------------------------- */
void test_ftl_eba(void) {
  TEST_BEGIN("EBA: ECC adaptativa por región (§11.8)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  mfs_hwv_t h = e.fs.hwv;
  h.flags0 = 0;
  h.oob_bytes = 0;
  CHECK_EQ((int)mfs_eba_select(&h, 0u), (int)MFS_ECC_SECDED);
  h.oob_bytes = 32u;
  CHECK_EQ((int)mfs_eba_select(&h, 0u), (int)MFS_ECC_BCH4);
  h.oob_bytes = 128u;
  CHECK_EQ((int)mfs_eba_select(&h, 0u), (int)MFS_ECC_BCH8);
  h.flags0 = MFS_HWV0_ECC_ON_DIE;
  CHECK_EQ((int)mfs_eba_select(&h, 0u), (int)MFS_ECC_LDPC);

  /* regla BER < capacidad/2 */
  mfs_eba_report(&e.fs, 0u, 100u); /* muy por debajo */
  CHECK(!mfs_eba_should_scrub(&e.fs, 0u));
  CHECK_EQ(e.fs.eba_violations, 0u);
  mfs_eba_report(&e.fs, 0u, 60000u); /* por encima de SECDED */
  CHECK(mfs_eba_should_scrub(&e.fs, 0u));
  CHECK(e.fs.eba_violations > 0u);
  env_close(&e);
  TEST_END("EBA");
}

/* ------------------------------ RAS + TG ------------------------------ */
void test_ftl_tg(void) {
  TEST_BEGIN("RAS + Thermal Governor (§11.9)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_COMPACT, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);

  /* retención efectiva según temperatura: urgencia por edad */
  mfs_tg_set_temp(&e.fs, 25);
  CHECK_EQ((int)e.fs.tg_state, (int)MFS_TG_NOMINAL);
  CHECK_EQ(mfs_tg_scrub_urgency(&e.fs, 100u, 1000u), 0u);
  CHECK_EQ(mfs_tg_scrub_urgency(&e.fs, 600u, 1000u), 2u);  /* > 0,5× */
  CHECK_EQ(mfs_tg_scrub_urgency(&e.fs, 1100u, 1000u), 3u); /* > retención */

  /* a 105 °C la retención se reduce ⇒ misma edad dispara urgencia */
  mfs_tg_set_temp(&e.fs, 105);
  CHECK_EQ((int)e.fs.tg_state, (int)MFS_TG_HOT);
  CHECK(mfs_tg_scrub_urgency(&e.fs, 300u, 1000u) >= 2u);

  /* extremo térmico ⇒ mantenimiento no-RT aplazado */
  uint32_t before = e.fs.hct.tg_deferred;
  CHECK_EQ(mfs_tg_step(&e.fs, 500u), MFS_EAGAIN);
  CHECK(e.fs.hct.tg_deferred > before);

  /* en nominal, el scrub avanza en ventana idle */
  mfs_tg_set_temp(&e.fs, 25);
  uint32_t sc = e.fs.hct.scrub_done;
  CHECK_EQ(mfs_tg_step(&e.fs, 500u), MFS_OK);
  CHECK(e.fs.hct.scrub_done == sc + 1u);

  /* ELD agotado ⇒ MFS_EENERGY */
  e.fs.eld_budget_mj = 1u;
  e.fs.eld_spent_mj = 100u;
  CHECK_EQ(mfs_tg_step(&e.fs, 500u), MFS_EENERGY);

  /* read-disturb */
  for (uint32_t i = 0; i < 40u; i++)
    mfs_tg_read_disturb(&e.fs, 1u, 100u);
  CHECK(e.fs.hct.read_retry > 0u);
  env_close(&e);
  TEST_END("RAS + TG");
}

/* ------------------------------ ELM + PEP ----------------------------- */
void test_ftl_elm_pep(void) {
  TEST_BEGIN("ELM + PEP: salud, RUL y refinamiento RT-C (§11.1)");
  /* salud: bloque nuevo ⇒ 1000; agotado ⇒ 0 */
  CHECK_EQ(mfs_elm_health(0u, 1000u, 0u, 1000u, 0u), 1000u);
  CHECK_EQ(mfs_elm_health(1000u, 1000u, 1000u, 1000u, 1000u), 0u);
  uint16_t h_mid = mfs_elm_health(500u, 1000u, 500u, 1000u, 500u);
  CHECK(h_mid > 400u && h_mid < 600u);

  /* RUL decrece con BER y satura sin pendiente */
  CHECK_EQ(mfs_elm_rul(100u, 1000u, 10u), 90000u);
  CHECK(mfs_elm_rul(100u, 1000u, 100u) < mfs_elm_rul(100u, 1000u, 10u));
  CHECK_EQ(mfs_elm_rul(500u, 1000u, 0u), 0xFFFFFFFFu);

  /* PEP determinista y acotado */
  uint16_t f1[MFS_PEP_FEATURES], f2[MFS_PEP_FEATURES];
  memset(f1, 0, sizeof(f1));
  memset(f2, 0, sizeof(f2));
  f1[0] = 200u;
  f1[1] = 50u;
  f2[0] = 255u;
  f2[1] = 255u;
  CHECK_EQ(mfs_pep_score(f1), mfs_pep_score(f1)); /* replay determinista */
  CHECK(mfs_pep_score(f1) <= 100u);

  /* WEP: permutación determinista sobre 16 bits y caché en Balanced */
  mfs_env_t e;
  CHECK(env_open(&e, 98304u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  uint8_t uid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  mfs_wep_init(&e.fs, uid);
  uint8_t seen[256];
  memset(seen, 0, sizeof(seen));
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < 256u; i++) {
    uint16_t p = mfs_wep_place(&e.fs, (uint16_t)i);
    uint8_t bucket = (uint8_t)((p >> 8) & 0xFFu);
    if (!seen[bucket]) {
      seen[bucket] = 1u;
      distinct++;
    }
    CHECK_EQ(mfs_wep_place(&e.fs, (uint16_t)i), p); /* estable (caché) */
  }
  CHECK(distinct > 16u); /* alta dispersión */

  /* integración con GC: elige una zona FULL válida */
  for (uint32_t i = 0; i < 4u; i++) {
    e.fs.zones[i].state = MFS_Z_FULL;
    e.fs.zones[i].valid_pages = (uint16_t)(i + 1u);
    e.fs.zones[i].total_pages = 7u;
    e.fs.zones[i].pe_cycles = (uint16_t)(100u * i);
  }
  int v = mfs_gc_select_victim_ftl(&e.fs, NULL, NULL);
  CHECK(v >= 0 && v < 4);
  env_close(&e);
  TEST_END("ELM + PEP");
}

/* -------------------------------- ZRP --------------------------------- */
void test_ftl_zrp(void) {
  TEST_BEGIN("ZRP: paridad RS(16,15) y reconstrucción (§8.5)");
  mfs_env_t e;
  CHECK(env_open(&e, RAM_ULTRA_NANO, NULL)); /* 31 páginas/zona */
  e.cfg.zrp_enable = true;
  CHECK_EQ(env_format(&e), MFS_OK);

  mfs_zone_t *z = &e.fs.zones[0];
  uint32_t pb = mfs_page_bytes(&e.fs); /* 128 B */
  CHECK(pb * 17u + MFS_ZONEHDR_SIZE <= z->size);

  /* escribir 16 páginas patrón (ZRP opera a nivel de página cruda) */
  for (uint32_t p = 0; p < 16u; p++) {
    uint8_t page[MFS_CHUNK_EXTENDED];
    fill(page, pb, (uint8_t)(p * 13u + 1u));
    CHECK_EQ(
        mfs_write(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + p * pb, page, pb),
        MFS_OK);
  }
  z->state = MFS_Z_OPEN;
  z->class_hot = 3u;
  z->total_pages = 31u;
  CHECK_EQ(mfs_zrp_encode(&e.fs, z), MFS_OK);
  CHECK_EQ(e.fs.hct.zrp_encoded, 1u);

  /* recuperar el patrón original de la página 3 para comparar */
  uint8_t want[MFS_CHUNK_EXTENDED];
  fill(want, pb, (uint8_t)(3u * 13u + 1u));

  /* degradar la página 3 (NOR: sólo se aclaran bits) */
  uint8_t zeros[MFS_CHUNK_EXTENDED];
  memset(zeros, 0x00u, pb);
  CHECK_EQ(
      mfs_write(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + 3u * pb, zeros, pb),
      MFS_OK);

  uint8_t got[MFS_CHUNK_EXTENDED];
  CHECK_EQ(mfs_read(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + 3u * pb, got, pb),
           MFS_OK);
  CHECK(memcmp(got, want, pb) != 0); /* está dañada */

  /* el rescate relocaliza la página reconstruida en una página virgen
   * (sobre NVM no se puede reprogramar 0→1 en la página dañada) */
  CHECK_EQ(mfs_zrp_recover(&e.fs, z, 3u, 20u), MFS_OK);
  CHECK_EQ(e.fs.hct.zrp_recovered, 1u);
  CHECK_EQ((int)z->zrp_dst, 20);
  CHECK_EQ(
      mfs_read(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + 20u * pb, got, pb),
      MFS_OK);
  CHECK(memcmp(got, want, pb) == 0); /* reconstruida */

  /* con ZRP deshabilitado no se genera paridad */
  e.cfg.zrp_enable = false;
  mfs_zone_t *z2 = &e.fs.zones[1];
  CHECK_EQ(mfs_zrp_encode(&e.fs, z2), MFS_ENOTSUP);
  env_close(&e);
  TEST_END("ZRP");
}
