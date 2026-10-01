/* test_gate.c — Puertas de arquitectura y viabilidad (§27.7)
 *
 * Verifica que el núcleo falla de forma explícita y tipificada en lugar de
 * degradar silenciosamente (MFS-ARCH-010 rev.2, MFS-VIA-001/002).
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
  case MFS_MODE_8BIT_ULTRA:
    return "8-bit Ultra";
  case MFS_MODE_8BIT_NANO:
    return "8-bit Nano";
  case MFS_MODE_8BIT_COMPACT:
    return "8-bit Compact";
  default:
    return "unsupported";
  }
}

void test_gate_earch(void) {
  TEST_BEGIN("Soporte de arquitecturas 8/16/32-bit (MFS-ARCH-010 rev.2)");
  mfs_env_t e;

  /* 1) 8-bit (arch_class 0) es ahora una arquitectura válida: se formatea y el
   *    núcleo elige un modo 8-bit dedicado para el presupuesto declarado. */
  CHECK(env_open(&e, 8192u, NULL));
  e.cfg.arch_class = 0u;
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK_EQ(e.fs.mode, MFS_MODE_8BIT_NANO);

  /* 2) un HWV persistido con arch_class 0 valida correctamente (round-trip) */
  mfs_hwv_t h;
  memset(&h, 0, sizeof(h));
  h.magic[0] = 'M';
  h.magic[1] = 'H';
  h.magic[2] = 'W';
  h.magic[3] = 'V';
  h.arch_class = 0u;
  h.erase_unit = 4096u;
  h.program_granularity = 1u;
  h.ram_total = 8192u;
  {
    uint8_t ser[64];
    mfs_hwv_serialize(&h, ser);
    mfs_hwv_deserialize(&h, ser);
  }
  CHECK_EQ(mfs_hwv_validate(&h), MFS_OK);

  /* 3) 64-bit (arch_class 3) también es válido: round-trip y validación OK */
  h.arch_class = 3u;
  {
    uint8_t ser2[64];
    mfs_hwv_serialize(&h, ser2);
    mfs_hwv_deserialize(&h, ser2);
  }
  CHECK_EQ(mfs_hwv_validate(&h), MFS_OK);

  /* 4) una clase de arquitectura desconocida (> 3) sí se rechaza */
  h.arch_class = 4u;
  CHECK_EQ(mfs_hwv_validate(&h), MFS_EARCH);

  /* 5) sin arquitectura declarada la API ni siquiera arranca */
  CHECK_EQ(mf_init(&e.fs, NULL), MFS_EINVAL);

  env_close(&e);
  TEST_END("Soporte de arquitecturas");
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

  /* Autodetección de arquitectura: MFS_ARCH_AUTO se resuelve a la clase del
   * objetivo (32/64 bits en host) y el núcleo elige un modo clásico. */
  CHECK(env_open(&e, 262144u, NULL));
  e.cfg.arch_class = (uint8_t)MFS_ARCH_AUTO;
  CHECK_EQ(env_format(&e), MFS_OK);
  CHECK(mfs_mode_is_classic((mfs_mode_t)e.fs.mode));
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
  hwv.arch_class = 2u;     /* esta matriz cubre los modos 16/32-bit */

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

  /* Los cinco modos clásicos tienen presupuesto normativo creciente y positivo.
   * Los modos 8-bit forman una familia aparte (RAM ≤ 2 KB) y se comprueban por
   * separado más abajo. */
  bool creciente = true;
  for (int m = MFS_MODE_ULTRA_NANO + 1; m <= (int)MFS_MODE_EXTENDED; m++) {
    if (mfs_limits[m].ram_total <= mfs_limits[m - 1].ram_total)
      creciente = false;
  }
  CHECK(creciente);

  /* Familia 8-bit: presupuesto creciente y acotado al techo de 2 KB. */
  bool creciente8 = true;
  for (int m = MFS_MODE_8BIT_ULTRA + 1; m <= (int)MFS_MODE_8BIT_COMPACT; m++) {
    if (mfs_limits[m].ram_total <= mfs_limits[m - 1].ram_total)
      creciente8 = false;
  }
  CHECK(creciente8);
  CHECK(mfs_limits[MFS_MODE_8BIT_COMPACT].ram_total <= MFS_RAM_8BIT_COMPACT);

  /* Con arch_class 0 el selector prefiere los modos 8-bit dedicados. */
  hwv.arch_class = 0u;
  hwv.ram_total = 8192u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_8BIT_NANO);
  hwv.ram_total = 16384u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_8BIT_COMPACT);
  hwv.arch_class = 2u; /* restaurar para las comprobaciones siguientes */
  printf("   %s(1K) -> unsupported | 8K -> %s | 16K -> %s | 32K -> %s | "
         "128K+ -> %s\n",
         "", mode_name(MFS_MODE_ULTRA_NANO), mode_name(MFS_MODE_NANO),
         mode_name(MFS_MODE_COMPACT), mode_name(MFS_MODE_EXTENDED));

  /* 64-bit (arch_class 3): usa la familia clásica, con Extended como techo. */
  hwv.arch_class = (uint8_t)MFS_ARCH_64BIT;
  hwv.ram_total = 262144u;
  CHECK_EQ(mfs_select_mode(&hwv, &cfg), MFS_MODE_EXTENDED);

  /* Autodetección de arquitectura y capacidades de aceleración HW. */
  {
    mfs_arch_info_t ai;
    CHECK_EQ(mfs_arch_detect(&ai), MFS_OK);
    CHECK(ai.arch_class <= (uint8_t)MFS_ARCH_64BIT);
    CHECK(ai.bits == 8u || ai.bits == 16u || ai.bits == 32u || ai.bits == 64u);
    CHECK(ai.name != NULL);
    /* Todo objetivo de 32/64 bits con CAS declara ATOMICS; en 8 bits no. */
    if (MFS_IS_8BIT_TARGET)
      CHECK((ai.hwaccel & MFS_HWACCEL_ATOMICS) == 0u);
    else
      CHECK((ai.hwaccel & MFS_HWACCEL_ATOMICS) != 0u);

    /* KAT CRC-32C (ruta activa: instrucción HW si existe, si no tabla). */
    const char *kat = "123456789";
    uint32_t c = mfs_crc32c((const uint8_t *)kat, 9u, 0u);
    CHECK_EQ(c, 0xE3069283u);
    /* Si hay ruta acelerada, debe coincidir con la de referencia. */
    if (mfs_arch_crc32c_hw_available()) {
      uint32_t hw = mfs_crc32c_hw((const uint8_t *)kat, 9u, 0u);
      CHECK_EQ(hw, c);
      CHECK((ai.hwaccel & MFS_HWACCEL_CRC32C) != 0u);
    }
  }

  /* Los modos compactos y superiores negocian suite; Ultra-Nano no (§10.6). */
  cfg.key = NULL;
  CHECK_EQ(mfs_negotiate_suite(&hwv, &cfg, MFS_MODE_ULTRA_NANO),
           (uint8_t)MFS_SUITE_NONE);
  CHECK_EQ(mfs_negotiate_suite(&hwv, &cfg, MFS_MODE_COMPACT),
           (uint8_t)MFS_SUITE_NONE); /* sin clave ⇒ sin cifrado */
  TEST_END("Matriz de viabilidad");
}
