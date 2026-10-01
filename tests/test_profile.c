/* test_profile.c — perfiles por tecnología de memoria (MFS-CAP-001)
 *
 * Verifica que cada medio soportado declara una ficha coherente (unidad de
 * asignación, borrado, página, chunk, capacidades nativas) y que el núcleo
 * rechaza de forma tipificada un medio cuya capacidad exceda el máximo
 * certificado por su perfil.
 */
#include "mfs_harness.h"
#include "mfs_test.h"

/* 2 TB decimales: la cota que debe alcanzar todo medio gestionado de consumo.
 */
#define CAP_REQ (2000000000000ull)

static bool is_pow2(uint32_t v) { return v != 0u && (v & (v - 1u)) == 0u; }

/* El perfil de un medio declarado debe ser coherente consigo mismo. */
static void check_profile_sane(mfs_media_type_t t, const char *name) {
  const mfs_media_profile_t *p = mf_media_profile(t);
  CHECK(p != NULL);
  CHECK(p->name != NULL && p->name[0] != '\0');
  CHECK(p->spec != NULL && p->spec[0] != '\0');
  CHECK_EQ(p->type, t);
  CHECK(p->engine == MFS_ENGINE_RAW || p->engine == MFS_ENGINE_ZONED ||
        p->engine == MFS_ENGINE_MANAGED);
  CHECK(p->max_bytes > 0ull);
  CHECK(is_pow2(p->alloc_unit));
  CHECK(is_pow2(p->page_size));
  CHECK(is_pow2(p->seek_unit));
  CHECK(p->pgm_gran > 0u && p->pgm_gran <= p->page_size);
  CHECK(is_pow2(p->chunk));
  /* el borrado, si existe, es potencia de dos y no menor que la página */
  CHECK(p->erase_unit == 0u ||
        (is_pow2(p->erase_unit) && p->erase_unit >= p->page_size));
  /* La unidad de asignación agrupa páginas completas del medio. */
  CHECK(p->alloc_unit >= p->page_size);
  (void)name;
}

void test_media_profiles(void) {
  TEST_BEGIN(
      "Perfiles por tecnología de memoria y cota de capacidad (MFS-CAP-001)");

  /* 1. Toda tecnología soportada tiene ficha coherente ----------------------
   */
  for (unsigned i = 1; i < (unsigned)MFS_MEDIA_COUNT; i++)
    check_profile_sane((mfs_media_type_t)i,
                       mf_media_profile((mfs_media_type_t)i)->name);

  /* 2. Determinismo: la consulta es estable byte a byte -------------------- */
  const mfs_media_profile_t *a = mf_media_profile(MFS_MEDIA_SD);
  const mfs_media_profile_t *b = mf_media_profile(MFS_MEDIA_SD);
  CHECK(a == b);
  CHECK_EQ(memcmp(a, b, sizeof(*a)), 0);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_SD)->alloc_unit, 4u << 20);

  /* 3. El medio decide el motor -------------------------------------------- */
  CHECK_EQ(mf_media_profile(MFS_MEDIA_NOR_SPI)->engine, MFS_ENGINE_RAW);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_NAND_RAW)->engine, MFS_ENGINE_RAW);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_NAND_ONFI)->engine, MFS_ENGINE_RAW);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_FRAM)->engine, MFS_ENGINE_RAW);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_EEPROM)->engine, MFS_ENGINE_RAW);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_ZNS_NAND)->engine, MFS_ENGINE_ZONED);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_SD)->engine, MFS_ENGINE_MANAGED);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_EMMC)->engine, MFS_ENGINE_MANAGED);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_USB)->engine, MFS_ENGINE_MANAGED);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_NVME)->engine, MFS_ENGINE_MANAGED);
  CHECK(strcmp(mf_engine_name(MFS_ENGINE_MANAGED), "MANAGED") == 0);
  CHECK(strcmp(mf_engine_name(MFS_ENGINE_RAW), "RAW") == 0);
  CHECK(strcmp(mf_engine_name(MFS_ENGINE_ZONED), "ZONED") == 0);

  /* 4. Cota de capacidad exigida ------------------------------------------- */
  CHECK(mf_media_profile(MFS_MEDIA_SD)->max_bytes >= CAP_REQ);
  CHECK(mf_media_profile(MFS_MEDIA_EMMC)->max_bytes >= CAP_REQ);
  CHECK(mf_media_profile(MFS_MEDIA_USB)->max_bytes >= CAP_REQ);
  CHECK(mf_media_profile(MFS_MEDIA_NVME)->max_bytes >= CAP_REQ);
  /* El motor RAW sigue acotado a 4 GiB: es su límite vigente, no el del silicio
   */
  CHECK(mf_media_profile(MFS_MEDIA_NOR_SPI)->max_bytes <=
        4ull * 1024ull * 1024ull * 1024ull);
  CHECK(mf_media_profile(MFS_MEDIA_NAND_RAW)->max_bytes <=
        4ull * 1024ull * 1024ull * 1024ull);

  /* 5. Capacidades nativas declaradas -------------------------------------- */
  const mfs_media_profile_t *sd = mf_media_profile(MFS_MEDIA_SD);
  CHECK((sd->flags & MFS_PROF_TRIM) != 0u);      /* discard por AU        */
  CHECK((sd->flags & MFS_PROF_REMOVABLE) != 0u); /* medio extraíble       */
  CHECK_EQ(sd->seek_unit, 512u);                 /* direccionamiento 512 B */
  CHECK_EQ(sd->alloc_unit, sd->erase_unit);      /* borrado == AU         */
  const mfs_media_profile_t *em = mf_media_profile(MFS_MEDIA_EMMC);
  CHECK_EQ(em->erase_unit, 512u << 10); /* erase group 512 KiB   */
  CHECK_EQ(em->alloc_unit, 4u << 20);   /* AU_SIZE 4 MB          */
  CHECK((em->flags & MFS_PROF_CQE) != 0u);
  CHECK((em->flags & MFS_PROF_TRIM) != 0u);
  const mfs_media_profile_t *nv = mf_media_profile(MFS_MEDIA_NVME);
  CHECK((nv->flags & MFS_PROF_CQE) != 0u);
  CHECK((nv->flags & MFS_PROF_ECC_ON_DIE) != 0u);
  CHECK((mf_media_profile(MFS_MEDIA_NOR_SPI)->flags & MFS_PROF_PPP) != 0u);
  /* Medios sin borrado: FRAM/MRAM/EEPROM (byte-addressable) */
  CHECK_EQ(mf_media_profile(MFS_MEDIA_FRAM)->erase_unit, 0u);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_MRAM)->erase_unit, 0u);
  CHECK_EQ(mf_media_profile(MFS_MEDIA_EEPROM)->erase_unit, 0u);
  CHECK((mf_media_profile(MFS_MEDIA_FRAM)->flags & MFS_PROF_NO_ERASE) != 0u);

  /* 6. Validación de capacidad (MFS-CAP-001) ------------------------------- */
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_SD, CAP_REQ), MFS_OK);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_SD, sd->max_bytes), MFS_OK);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_SD, sd->max_bytes + 1ull),
           MFS_ENOTVIABLE);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_EMMC, 64ull << 30), MFS_OK);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_NVME, 8ull << 40), MFS_OK);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_NVME, (8ull << 40) + 1ull),
           MFS_ENOTVIABLE);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_FRAM, 32ull << 20), MFS_ENOTVIABLE);
  CHECK_EQ(mfs_profile_check(MFS_MEDIA_NONE, 4096ull), MFS_EINVAL);
  CHECK_EQ(mfs_profile_check((mfs_media_type_t)200, 4096ull), MFS_EINVAL);

  /* 7. Integración con el montaje: el HAL aplica la cota ------------------- */
  {
    mfs_env_t e;
    CHECK(env_open(&e, 262144u, NULL));
    /* Un volumen NOR de 1 MiB se formatea con normalidad... */
    CHECK_EQ(env_format(&e), MFS_OK);
    /* ...pero declarar 32 MiB de FRAM (por encima de su perfil) se rechaza. */
    e.g.type = MFS_MEDIA_FRAM;
    e.g.size = 32u << 20;
    CHECK_EQ(env_remount(&e), MFS_ENOTVIABLE);
    e.g.type = MFS_MEDIA_NOR_SPI;
    e.g.size = MFS_TEST_SIZE;
    env_close(&e);
  }

  /* 8. El tier T0 también se valida contra su perfil ----------------------- */
  {
    mfs_env_t e;
    CHECK(
        env_open_t0(&e, 262144u, NULL, 32u << 20)); /* 32 MiB > FRAM (16 MiB) */
    CHECK_EQ(env_format(&e), MFS_ENOTVIABLE);
    env_close(&e);
  }

  TEST_END("Perfiles por tecnología de memoria");
}
