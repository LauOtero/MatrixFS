/* test_gate.c — Rechazo temprano (§27.7): MFS_EARCH y MFS_ENOTVIABLE
 *
 * Verifica que el núcleo falla de forma explícita y tipificada en lugar de
 * degradar silenciosamente (MFS-ARCH-010, MFS-VIA-001/002).
 */
#include "mfs_harness.h"
#include "mfs_test.h"

static const char *mode_name(mfs_mode_t m) {
  switch (m) {
  case MFS_MODE_ULTRA_NANO:
    return "Ultra-Nano";
  case MFS_MODE_NANO:
    return "Nano";
  case MFS_MODE_COMPACT:
    return "Compact";
  case MFS_MODE_BALANCED:
    return "Balanced";
  case MFS_MODE_EXTENDED:
    return "Extended";
  default:
    return "unsupported";
  }
}

void test_gate_earch(void) {
  TEST_BEGIN("Rechazo temprano: arquitecturas de 8 bits (MFS-ARCH-010)");
  mfs_env_t e;
  CHECK(env_open(&e, 262144u, NULL));
  e.cfg.arch_class = 0u; /* HWV declara arch_class = 0 */

  /* 1) el montaje se rechaza con MFS_EARCH (§24.2 paso 2) */
  CHECK_EQ(mf_init(&e.fs, &e.cfg), MFS_EARCH);

  /* 2) un HWV persistido con arch_class 0 también se rechaza al validar */
  mfs_hwv_t h;
  memset(&h, 0, sizeof(h));
  h.magic[0] = 'M';
  h.magic[1] = 'H';
  h.magic[2] = 'W';
  h.magic[3] = 'V';
  h.arch_class = 0u;
  CHECK_EQ(mfs_hwv_validate(&h), MFS_EARCH);

  /* 3) arch_class > 2 tampoco es una arquitectura soportada */
  h.arch_class = 3u;
  CHECK_EQ(mfs_hwv_validate(&h), MFS_EARCH);

  /* 4) sin arquitectura declarada la API ni siquiera arranca */
  CHECK_EQ(mf_init(&e.fs, NULL), MFS_EINVAL);

  env_close(&e);
  TEST_END("Rechazo MFS_EARCH");
}

void test_gate_notviable(void) {
  TEST_BEGIN("Rechazo temprano: RAM insuficiente (MFS-VIA-002)");
  mfs_env_t e;

  /* Presupuesto imposible: el reparto §6.1 (fw 50 %, pila 15 %, perif 8 %,
   * margen 10 %) no deja los 720 B de Ultra-Nano. */
  CHECK(env_open(&e, 1024u, NULL));
  CHECK_EQ(mf_init(&e.fs, &e.cfg), MFS_ENOTVIABLE);

  /* La puerta no depende del orden de llamadas: formatear tampoco es viable */
  CHECK_EQ(env_format(&e), MFS_ENOTVIABLE);
  env_close(&e);

  /* Presupuesto suficiente: formatea y selecciona un modo declarable */
  CHECK(env_open(&e, 32768u, NULL));
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(e.fs.mode, MFS_MODE_COMPACT);
  env_close(&e);

  /* El manifiesto fijado manda sobre el cálculo automático (§6.1 fase 10) */
  CHECK(env_open(&e, 262144u, NULL));
  e.cfg.forced_mode = MFS_MODE_NANO;
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(e.fs.mode, MFS_MODE_NANO);
  env_close(&e);
  TEST_END("Rechazo MFS_ENOTVIABLE");
}

void test_gate_mode_matrix(void) {
  TEST_BEGIN("Matriz de viabilidad por modo (§6.3)");
  mfs_hwv_t hwv;
  mfs_config cfg;
  memset(&hwv, 0, sizeof(hwv));
  memset(&cfg, 0, sizeof(cfg));
  hwv.mode_forced = 0xFFu; /* auto */

  /* El modo seleccionado nunca retrocede al aumentar el presupuesto de RAM. */
  const uint32_t rams[] = {1024u,  4096u,   8192u,   16384u,  32768u,
                           65536u, 131072u, 262144u, 1048576u};
  mfs_mode_t prev = MFS_MODE_UNSUPPORTED;
  bool monotone = true;
  for (uint32_t i = 0; i < sizeof(rams) / sizeof(rams[0]); i++) {
    hwv.ram_total = rams[i];
    mfs_mode_t m = mfs_select_mode(&hwv, &cfg);
    if (m != MFS_MODE_UNSUPPORTED && prev != MFS_MODE_UNSUPPORTED && m < prev)
      monotone = false;
    if (m != MFS_MODE_UNSUPPORTED)
      prev = m;
  }
  CHECK(monotone);

  /* Cotas concretas verificadas a mano con el reparto §6.1. */
  hwv.ram_total = 1024u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_UNSUPPORTED);
  hwv.ram_total = 8192u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_ULTRA_NANO);
  hwv.ram_total = 16384u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_NANO);
  hwv.ram_total = 32768u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_COMPACT);
  hwv.ram_total = 131072u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_EXTENDED);
  hwv.ram_total = 262144u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_EXTENDED);

  /* Los cinco modos tienen presupuesto normativo creciente y positivo. */
  bool creciente = true;
  for (int m = 1; m < (int)MFS_MODE_COUNT; m++) {
    if (mfs_limits[m].ram_total <= mfs_limits[m - 1].ram_total)
      creciente = false;
  }
  CHECK(creciente);
  printf("   %s(1K) -> unsupported | 8K -> %s | 16K -> %s | 32K -> %s | "
         "128K+ -> %s\n",
         "", mode_name(MFS_MODE_ULTRA_NANO), mode_name(MFS_MODE_NANO),
         mode_name(MFS_MODE_COMPACT), mode_name(MFS_MODE_EXTENDED));

  /* Los modos compactos y superiores negocian suite; Ultra-Nano no (§10.6). */
  cfg.key = NULL;
  CHECK_EQ(mfs_negotiate_suite(&hwv, &cfg, MFS_MODE_ULTRA_NANO),
           (uint8_t)MFS_SUITE_NONE);
  CHECK_EQ(mfs_negotiate_suite(&hwv, &cfg, MFS_MODE_COMPACT),
           (uint8_t)MFS_SUITE_NONE); /* sin clave ⇒ sin cifrado */
  TEST_END("Matriz de viabilidad");
}
