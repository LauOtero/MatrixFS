/* test_stm32.c — verificacion en host del port STM32Cube (sin HAL real).
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Se compila y ejecuta con -DMFS_STM32_HOST_TEST=1 contra el nucleo REAL y el
 * modelo de flash de test/shims. Se ejecuta TRES veces con distinta unidad de
 * programacion y ECC para cubrir el rango de familias:
 *
 *   -DMFS_STM32_HOST_PGM_UNIT=4  -DMFS_STM32_HOST_ECC=0   (F4/F7)
 *   -DMFS_STM32_HOST_PGM_UNIT=8  -DMFS_STM32_HOST_ECC=1   (L4/G4/G0/WB)
 *   -DMFS_STM32_HOST_PGM_UNIT=16 -DMFS_STM32_HOST_ECC=1   (H7/H5/U5)
 *
 * Lo que se prueba, y por que importa:
 *   1) mapa de sectores: expansion, contaje, validacion y RECHAZO de tablas
 *      malas (una tabla mal escrita borraria el sector equivocado);
 *   2) ventana uniforme con sectores NO uniformes (16/64/128 KB), incluido el
 *      desempate por unidad mas pequena a igual capacidad;
 *   3) ciclo completo formatear → montar → escribir → releer por la API publica
 *      del port, sobre la region de sectores de 128 KB;
 *   4) el INVARIANTE de ECC: el backend nunca pide al HAL reprogramar una unidad
 *      no virgen con datos distintos, acepta el no-op si el dato ya esta
 *      contenido y devuelve MFS_EIO en cualquier otro caso;
 *   5) granularidad de unidad y rechazo tipificado de configuraciones invalidas.
 */
#include <stdio.h>
#include <string.h>

#include "mfs_internal.h" /* mf_t */
#include "mfs_test.h"
#include "matrixfs_stm32.h"
#include "mfs_stm32_backend.h"
#include "mfs_stm32_flash_map.h"
#include "stm32_hal_shim.h"

int g_checks = 0;
int g_failures = 0;

/* Region del dispositivo que se usa para montar: los SIETE sectores de 128 KB
 * contiguos (0x08020000..0x08100000). El layout del nucleo reserva 3 unidades
 * logicas (SB A, SB B y anillo de tokens: src/mfs_internal.h:27-36), de modo que
 * hacen falta >= 4 unidades para tener al menos una zona; con 7 se obtienen 4
 * zonas y 512 KB de capacidad util. */
#define REGION_ADDR 0x08020000u
#define REGION_SIZE (7u * 128u * 1024u)

/* Familia equivalente a la tabla del modelo, para probar la expansion: un F4 de
 * 1 MB de un solo banco (4x16 KB + 1x64 KB + 7x128 KB). */
static const mfs_stm32_sector_run_t F4_RUNS[] = {
    {4u, 16u * 1024u},
    {1u, 64u * 1024u},
    {7u, 128u * 1024u},
};
static const mfs_stm32_family_t F4_1MB = {
    "STM32F4 (1 MB, un banco)", 0x08000000u, 1u << 20, F4_RUNS, 3u};

/* ------------------------------------------------------------------ */
static void test_flash_map(void) {
  TEST_BEGIN("mapa de sectores: expansion, contaje y validacion");

  CHECK_EQ(mfs_stm32_flash_count(&F4_1MB, 0u), 12u);
  /* Hasta 128 KB entran 4 sectores de 16 KB + 1 de 64 KB. */
  CHECK_EQ(mfs_stm32_flash_count(&F4_1MB, 128u * 1024u), 5u);
  CHECK_EQ(mfs_stm32_flash_count(&F4_1MB, 8u * 1024u), 0u);

  /* Expansion completa: direcciones contiguas y ascendentes, y debe coincidir
   * con la tabla que usa el modelo (que es la que se le pasa al port). */
  mfs_stm32_sector_t out[32];
  uint32_t n = 0u;
  CHECK_EQ(mfs_stm32_flash_expand(&F4_1MB, out, 32u, 0u, &n), MFS_OK);
  CHECK_EQ(n, 12u);
  CHECK_EQ(n, shim_sector_count);
  uint32_t expect = 0x08000000u;
  uint32_t total = 0u;
  for (uint32_t i = 0; i < n; i++) {
    CHECK_EQ(out[i].addr, expect);
    CHECK(out[i].size > 0u);
    expect += out[i].size;
    total += out[i].size;
    if (i < shim_sector_count) {
      CHECK_EQ(out[i].addr, shim_sectors[i].addr);
      CHECK_EQ(out[i].size, shim_sectors[i].size);
    }
  }
  CHECK_EQ(total, 1u << 20);
  CHECK_EQ(expect, 0x08000000u + (1u << 20));

  /* Capacidad insuficiente: tipificada, no truncada en silencio. */
  CHECK_EQ(mfs_stm32_flash_expand(&F4_1MB, out, 4u, 0u, &n), MFS_EOVERFLOW);
  CHECK_EQ(mfs_stm32_flash_expand(&F4_1MB, out, 32u, 8u * 1024u, &n), MFS_OK);
  CHECK_EQ(n, 0u);

  /* La tabla del modelo cubre [base, base+size) de forma contigua. */
  CHECK(mfs_stm32_flash_map_validate(shim_sectors, shim_sector_count,
                                     0x08000000u, shim_flash_size()));
  CHECK(!mfs_stm32_flash_map_validate(NULL, shim_sector_count, 0x08000000u,
                                      shim_flash_size()));
  CHECK(!mfs_stm32_flash_map_validate(shim_sectors, 0u, 0x08000000u,
                                      shim_flash_size()));
  /* La tabla se queda corta: no llega al final de la flash. */
  CHECK(!mfs_stm32_flash_map_validate(shim_sectors, shim_sector_count - 1u,
                                      0x08000000u, shim_flash_size()));
  /* La tabla declara mas flash de la que hay. */
  CHECK(!mfs_stm32_flash_map_validate(shim_sectors, shim_sector_count,
                                      0x08000000u, shim_flash_size() + 4096u));
  {
    /* HUECO: el segundo sector no empieza donde termina el primero. */
    mfs_sector_t bad[3] = {{0x08000000u, 16u * 1024u},
                           {0x08008000u, 16u * 1024u},
                           {0x0800C000u, 16u * 1024u}};
    CHECK(!mfs_stm32_flash_map_validate(bad, 3u, 0x08000000u, 64u * 1024u));
  }
  {
    /* TAMANO EQUIVOCADO: el segundo sector declarado mas grande de lo que
     * queda, de modo que la tabla se sale del final de la flash declarada. */
    mfs_sector_t bad[2] = {{0x08000000u, 64u * 1024u},
                           {0x08010000u, 64u * 1024u}};
    CHECK(!mfs_stm32_flash_map_validate(bad, 2u, 0x08000000u, 96u * 1024u));
  }
  {
    /* Tamano 0 y desbordamiento de la ultima direccion. */
    mfs_sector_t bad0[1] = {{0x08000000u, 0u}};
    CHECK(!mfs_stm32_flash_map_validate(bad0, 1u, 0x08000000u, 4096u));
    mfs_sector_t badovf[1] = {{0xFFFFFF00u, 0x200u}};
    CHECK(!mfs_stm32_flash_map_validate(badovf, 1u, 0xFFFFFF00u, 0x200u));
  }

  TEST_END("mapa de sectores");
}

/* ------------------------------------------------------------------ */
static void test_uniform_window(void) {
  TEST_BEGIN("ventana uniforme con sectores no uniformes");

  mfs_uniform_window_t w;

  /* Region que abarca SOLO los sectores de 128 KB: la unidad debe ser 128 KB
   * (con 7 unidades contiguas) y la capacidad, min(MFS_ZONE_MAX, count)*unit. */
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       REGION_ADDR, REGION_SIZE, 1024u,
                                       &w) == MFS_OK);
  CHECK_EQ(w.base, REGION_ADDR);
  CHECK_EQ(w.unit, 128u * 1024u);
  CHECK_EQ(w.phys_sector, 128u * 1024u);
  CHECK_EQ(w.count, 7u);
  CHECK_EQ(w.size, REGION_SIZE);
  CHECK_EQ(w.dropped_bytes, 0u);
  CHECK_EQ(w.capacity_bytes, 7u * 128u * 1024u);
  CHECK_EQ(w.capacity_bytes,
           (w.count > MFS_ZONE_MAX ? MFS_ZONE_MAX : w.count) * w.unit);

  /* Region con sectores de 16 KB: 4 x 16 KB (capacidad 64 KB) empata con el
   * sector de 64 KB (capacidad 64 KB), y el desempate documentado en
   * platform/common/mfs_sectors.h:71-75 elige la unidad MAS PEQUENA. */
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       0x08000000u, 0x20000u, 1024u,
                                       &w) == MFS_OK);
  CHECK_EQ(w.base, 0x08000000u);
  CHECK_EQ(w.unit, 16u * 1024u);
  CHECK_EQ(w.phys_sector, 16u * 1024u);
  CHECK_EQ(w.count, 4u);
  CHECK_EQ(w.capacity_bytes, 64u * 1024u);
  /* Los 64 KB del sector grande quedan FUERA: hay que reportarlo, no ocultarlo. */
  CHECK_EQ(w.dropped_bytes, 64u * 1024u);

  /* Region que cruza un cambio de tamano: usa la parte uniforme y declara lo
   * descartado. */
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       0x08010000u, 0x20000u, 1024u,
                                       &w) == MFS_OK);
  CHECK_EQ(w.unit, 64u * 1024u);
  CHECK_EQ(w.count, 1u);
  CHECK(w.dropped_bytes > 0u);

  /* Fuera del dispositivo: no hay ventana (tipificado). */
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       0x20000000u, 4096u, 1024u,
                                       &w) == MFS_ENOENT);
  /* Argumentos invalidos. */
  CHECK(mfs_stm32_flash_uniform_window(NULL, shim_sector_count, 0x08000000u,
                                       4096u, 1024u, &w) == MFS_EINVAL);
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       0x08000000u, 0u, 1024u,
                                       &w) == MFS_EINVAL);

  /* Paginas de 128 B (STM32L0): el nucleo exige erase_unit >= 1024 B, asi que
   * el helper AGRUPA 8 paginas en una unidad de 1024 B. */
  {
    static mfs_sector_t small[64];
    for (uint32_t i = 0; i < 64u; i++) {
      small[i].addr = 0x08000000u + i * 128u;
      small[i].size = 128u;
    }
    CHECK(mfs_stm32_flash_uniform_window(small, 64u, 0x08000000u, 64u * 128u,
                                         1024u, &w) == MFS_OK);
    CHECK_EQ(w.unit, 1024u);
    CHECK_EQ(w.phys_sector, 128u);
    CHECK_EQ(w.count, 8u);
    CHECK_EQ(w.capacity_bytes, 8u * 1024u);
  }

  /* Paginas de 2 KB (L4/G4/G0): el sector ya cumple el minimo, de modo que la
   * unidad logica es el propio sector. */
  {
    static mfs_sector_t mid[64];
    for (uint32_t i = 0; i < 64u; i++) {
      mid[i].addr = 0x08000000u + i * 2048u;
      mid[i].size = 2048u;
    }
    CHECK(mfs_stm32_flash_uniform_window(mid, 64u, 0x08000000u, 64u * 2048u,
                                         1024u, &w) == MFS_OK);
    CHECK_EQ(w.unit, 2048u);
    CHECK_EQ(w.count, 64u);
    CHECK_EQ(w.capacity_bytes,
             (w.count > MFS_ZONE_MAX ? MFS_ZONE_MAX : w.count) * w.unit);
  }

  TEST_END("ventana uniforme");
}

/* ------------------------------------------------------------------ */
/* Restriccion de LAYOUT (no es un fallo del port, es el precio del layout):
 * el nucleo reserva 3 unidades logicas antes de la primera zona
 * (src/mfs_internal.h:27-36) y `zones_init_table()` calcula
 * zona_cap = (media_size - 3*erase_unit) / zona (src/core/mfs_fs.c:132-153).
 * Con la ventana de 3 x 128 KB quedan 0 zonas: el port monta (los metadatos
 * caben) pero la capacidad util es 0 y la primera escritura devuelve ENOSPC.
 * Se comprueba que el fallo es TIPIFICADO (nunca corrupcion silenciosa) y queda
 * documentado por que `test_mount_write_read` necesita la ventana completa de
 * 7 x 128 KB. */
static void test_layout_constraint(void) {
  TEST_BEGIN("region de 3 x 128 KB: capacidad 0 y ENOSPC tipificado");

  shim_flash_reset();
  static mf_t fs;
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = 0x080A0000u;             /* ultimos 3 sectores de 128 KB */
  c.region_size = 3u * 128u * 1024u;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  c.ram_total = 32768u;

  /* La ventana es correcta (128 KB)... */
  mfs_uniform_window_t w;
  CHECK(mfs_stm32_flash_uniform_window(shim_sectors, shim_sector_count,
                                       c.region_addr, c.region_size, 1024u,
                                       &w) == MFS_OK);
  CHECK_EQ(w.unit, 128u * 1024u);
  CHECK_EQ(w.count, 3u);
  /* ...pero el volumen resultante NO es utilizable: con 3 unidades logicas y 3
   * reservadas (SB A, SB B, anillo de tokens) quedan 0 zonas. El nucleo lo
   * gestiona de forma TIPIFICADA — monta y devuelve ENOSPC en la primera
   * escritura, en vez de corromper — pero la capacidad util es 0, asi que esta
   * configuracion no sirve para un volumen real. Por eso `test_mount_write_read`
   * usa la ventana completa de 7 x 128 KB. */
  mfs_embedded_flash_t fl;
  CHECK_EQ(mfs_stm32_iflash_prepare(&c, &fl), MFS_OK);
  CHECK_EQ(fl.size, 3u * 128u * 1024u);
  CHECK_EQ(fl.erase_unit, 128u * 1024u);

  CHECK_EQ(matrixfs_stm32_format(&fs, &c), MFS_OK);
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_OK);
  CHECK_EQ(matrixfs_stm32_capacity_bytes(), 0u);
  CHECK(!matrixfs_stm32_capacity_wasteful());
  CHECK(matrixfs_stm32_last_error() != NULL);

  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&fs, "/x", MFS_O_RDWR | MFS_O_CREAT, &f), MFS_OK);
  size_t wr = 0u;
  CHECK_EQ(mf_write(f, "hola", 4u, &wr), MFS_ENOSPC);
  if (f)
    (void)mf_close(f);

  TEST_END("restriccion de layout");
}

/* ------------------------------------------------------------------ */
static void test_mount_write_read(void) {
  TEST_BEGIN("ciclo completo: formatear, montar, escribir y releer");

  shim_flash_reset();
  CHECK(shim_flash_ready());

  static mf_t fs;
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = REGION_ADDR;
  c.region_size = REGION_SIZE;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  c.ram_total = 32768u;
  c.forced_mode = MFS_MODE_UNSUPPORTED;

  CHECK_EQ(matrixfs_stm32_format(&fs, &c), MFS_OK);
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_OK);

  /* Capacidad util: 7 unidades - 3 reservadas = 4 zonas x 128 KB = 512 KB, por
   * debajo de MFS_ZONE_MAX*unit, asi que no se desperdicia region. */
  CHECK_EQ(matrixfs_stm32_capacity_bytes(), 4u * 128u * 1024u);
  CHECK(!matrixfs_stm32_capacity_wasteful());
  CHECK(matrixfs_stm32_last_error() != NULL);

  const mfs_hwv_t *h = matrixfs_stm32_hwv();
  CHECK(h != NULL);
  if (h) {
    CHECK_EQ(h->erase_unit, 128u * 1024u);
    CHECK_EQ(h->program_granularity, (uint32_t)MFS_STM32_HOST_PGM_UNIT);
    CHECK_EQ(h->media_size, REGION_SIZE);
#if MFS_STM32_HOST_ECC
    /* Declarar ECC en el HWV NO es cosmetico: el nucleo elige la clase ECC mas
     * tolerante (src/ftl/mfs_ftl2.c: mfs_eba_select). */
    CHECK((h->flags0 & MFS_HWV0_ECC_ON_DIE) != 0u);
#else
    CHECK((h->flags0 & MFS_HWV0_ECC_ON_DIE) == 0u);
#endif
  }

  /* Fichero mayor que una pagina NOR (256 B) y que el chunk del modo elegido. */
  static uint8_t buf[3000];
  for (size_t i = 0; i < sizeof(buf); i++)
    buf[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);

  mfs_file *f = NULL;
  CHECK_EQ(mf_open(&fs, "/datos.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC,
                   &f), MFS_OK);
  size_t wr = 0u;
  CHECK_EQ(mf_write(f, buf, sizeof(buf), &wr), MFS_OK);
  CHECK_EQ(wr, sizeof(buf));
  CHECK_EQ(mf_close(f), MFS_OK);
  CHECK_EQ(mf_sync(&fs), MFS_OK);

  static uint8_t got[3000];
  memset(got, 0, sizeof(got));
  CHECK_EQ(mf_open(&fs, "/datos.bin", MFS_O_RDONLY, &f), MFS_OK);
  size_t rd = 0u;
  CHECK_EQ(mf_read(f, got, sizeof(got), &rd), MFS_OK);
  CHECK_EQ(mf_close(f), MFS_OK);
  CHECK_EQ(rd, sizeof(buf));
  CHECK(memcmp(got, buf, sizeof(buf)) == 0);

  CHECK_EQ(mf_mkdir(&fs, "/dir"), MFS_OK);
  mfs_stat stt;
  CHECK_EQ(mf_stat(&fs, "/datos.bin", &stt), MFS_OK);
  CHECK_EQ(stt.size, sizeof(buf));

  /* INVARIANTE: en todo el ciclo el backend NUNCA pidio al HAL reprogramar una
   * unidad no virgen (ni siquiera con el dato ya contenido). */
  CHECK(!shim_flash_was_double_programmed());
  CHECK(shim_flash_program_count() > 0u);
  CHECK(shim_flash_erase_count() > 0u);

  /* STM-04: el port programa SIEMPRE con el controlador desbloqueado: el lote
   * unlock/program/lock de mfs_stm32_hal_flash_program_block() lo garantiza. El
   * modelo cuenta cualquier programacion con LOCK=1, que aqui debe ser CERO. En
   * silicio programar bloqueado perderia la escritura en silencio. */
  CHECK_EQ(shim_flash_program_while_locked_count(), 0u);

  CHECK_EQ(matrixfs_stm32_deinit(&fs), MFS_OK);

  TEST_END("ciclo completo");
}

/* ------------------------------------------------------------------ */
/* Escritura ANCHA: la unidad completa del dispositivo, con un patron distinto
 * en cada byte. Es la prueba que distingue un RMW correcto de uno que trunca
 * (el caso de la quadword de 16 B de H5/H7/U5, que no cabe en un uint64_t: el
 * port la delega en mfs_stm32_hal_flash_program_quadword()). Ademas, lo que hay
 * DESPUES del tramo escrito debe seguir virgen: el RMW no puede inventar datos
 * fuera de lo pedido. */
static void test_wide_write(void) {
  TEST_BEGIN("escritura ancha (unidad completa) y no invencion de datos");

  shim_flash_reset();

  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = REGION_ADDR;
  c.region_size = REGION_SIZE;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  c.ram_total = 32768u;

  static mfs_embedded_flash_t flash;
  CHECK_EQ(mfs_stm32_iflash_prepare(&c, &flash), MFS_OK);

  const uint32_t unit = (uint32_t)MFS_STM32_HOST_PGM_UNIT;
  const uint32_t addr = flash.base_addr;
  CHECK_EQ(flash.erase(flash.ctx, addr), MFS_OK);

  uint8_t src[16];
  for (uint32_t i = 0; i < sizeof(src); i++)
    src[i] = (uint8_t)(0xA0u + i); /* 16 bytes distintos: nada de patrones */

  CHECK_EQ(flash.prog(flash.ctx, addr, src, sizeof(src)), MFS_OK);

  uint8_t back[16];
  memset(back, 0, sizeof(back));
  CHECK_EQ(flash.read(flash.ctx, addr, back, sizeof(back)), MFS_OK);
  CHECK(memcmp(back, src, sizeof(src)) == 0);

  /* El byte siguiente (ya en la unidad contigua) debe seguir a 0xFF. */
  uint8_t tail[16];
  memset(tail, 0, sizeof(tail));
  CHECK_EQ(flash.read(flash.ctx, addr + (uint32_t)sizeof(src), tail, unit),
           MFS_OK);
  for (uint32_t i = 0; i < unit; i++)
    CHECK_EQ(tail[i], 0xFFu);

  /* Repetir el mismo bloque: no-op (el backend no debe reprogramar la unidad). */
  const uint32_t calls = shim_flash_program_count();
  CHECK_EQ(flash.prog(flash.ctx, addr, src, sizeof(src)), MFS_OK);
  CHECK_EQ(shim_flash_program_count(), calls);

  TEST_END("escritura ancha");
}

/* ------------------------------------------------------------------ */
/* Borrado por TRAMO con sectores fisicos mas pequenos que la unidad logica.
 *
 * El nucleo solo borra en multiplos de `erase_unit`; el backend debe borrar
 * TODOS los sectores fisicos que cubren [addr, addr+erase_unit)
 * (mfs_stm32_iflash.c:224-255). Con la tabla tipo F4 (128 KB) la unidad logica
 * coincide con UN sector fisico, de modo que el bucle interior no se ejercita:
 * aqui se usa una tabla de 512 B por sector, donde el minimo de 1024 B que
 * impone el nucleo obliga a agrupar DOS sectores por unidad logica. Un solo
 * erase debe borrar los dos, y no tocar los vecinos. */
static void test_multisector_erase(void) {
  TEST_BEGIN("borrado por tramo: erase_unit = 2 sectores fisicos");

  shim_flash_reset();
  /* 2048 x 512 B = 1 MiB (cubre la flash completa, como exige prepare). */
  static mfs_sector_t tiny[2048];
  for (uint32_t i = 0; i < 2048u; i++) {
    tiny[i].addr = 0x08000000u + i * 512u;
    tiny[i].size = 512u;
  }
  shim_flash_set_sectors(tiny, 2048u);

  /* Region de 64 KB en la mitad alta, alineada a 512 B. */
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = 0x08040000u;
  c.region_size = 64u * 1024u;
  c.sectors = tiny;
  c.sector_count = 2048u;
  c.ram_total = 32768u;

  static mfs_embedded_flash_t flash;
  CHECK_EQ(mfs_stm32_iflash_prepare(&c, &flash), MFS_OK);
  /* El minimo de 1024 B del nucleo agrupa 2 sectores de 512 B. */
  CHECK_EQ(flash.erase_unit, 1024u);
  CHECK_EQ(flash.program_granularity, (uint32_t)MFS_STM32_HOST_PGM_UNIT);
  CHECK_EQ(flash.base_addr, 0x08040000u);

  const uint32_t base = flash.base_addr;

  /* Marca en la unidad ANTERIOR y en la SIGUIENTE a la que se va a borrar. */
  const uint8_t mark = 0x5Au;
  CHECK_EQ(flash.prog(flash.ctx, base + 1024u, &mark, 1u), MFS_OK);
  CHECK_EQ(flash.prog(flash.ctx, base + 3072u, &mark, 1u), MFS_OK);

  const uint32_t erases_before = shim_flash_erase_count();

  /* Un solo erase logico: debe borrar 1024 B = 2 sectores fisicos de 512 B. */
  CHECK_EQ(flash.erase(flash.ctx, base + 2048u), MFS_OK);
  CHECK_EQ(shim_flash_erase_count(), erases_before + 2u);

  uint8_t buf[1024];
  memset(buf, 0xAA, sizeof(buf));
  CHECK_EQ(flash.read(flash.ctx, base + 2048u, buf, sizeof(buf)), MFS_OK);
  {
    bool all_ff = true;
    for (uint32_t i = 0; i < sizeof(buf); i++)
      if (buf[i] != 0xFFu)
        all_ff = false;
    CHECK(all_ff); /* los DOS sectores fisicos borrados, enteros */
  }

  /* Los vecinos siguen intactos: el tramo no puede borrar de mas. */
  uint8_t nb[4];
  memset(nb, 0, sizeof(nb));
  CHECK_EQ(flash.read(flash.ctx, base + 1024u, nb, 1u), MFS_OK);
  CHECK_EQ(nb[0], mark);
  memset(nb, 0, sizeof(nb));
  CHECK_EQ(flash.read(flash.ctx, base + 3072u, nb, 1u), MFS_OK);
  CHECK_EQ(nb[0], mark);

  /* Direccion que NO empieza en frontera de sector fisico: EINVAL, no se borra
   * un tramo "a medias" (mfs_stm32_iflash.c:243-247). */
  CHECK_EQ(flash.erase(flash.ctx, base + 256u), MFS_EINVAL);

  /* OBSERVACION (no es un fallo): `if_erase` valida que el tramo empiece en
   * frontera de SECTOR FISICO, no en multiplo de `erase_unit`. Con esta tabla,
   * base+512 es frontera de sector de 512 B, de modo que se acepta y se borran
   * los 2 sectores que cubren [base+512, base+1536). El nucleo solo llama con
   * multiplos de erase_unit (nunca este caso), pero conviene dejarlo por
   * escrito: un integrador que llame al callback a mano puede borrar media
   * unidad logica. */
  CHECK_EQ(flash.prog(flash.ctx, base, &mark, 1u), MFS_OK);
  const uint32_t erases_before2 = shim_flash_erase_count();
  CHECK_EQ(flash.erase(flash.ctx, base + 512u), MFS_OK);
  CHECK_EQ(shim_flash_erase_count(), erases_before2 + 2u);
  {
    uint8_t half[512];
    memset(half, 0xAA, sizeof(half));
    CHECK_EQ(flash.read(flash.ctx, base, half, sizeof(half)), MFS_OK);
    CHECK_EQ(half[0], mark);      /* el sector 0 NO se toca */
    CHECK_EQ(half[256], 0xFFu);   /* el resto del sector 0 sigue virgen */
    memset(half, 0xAA, sizeof(half));
    CHECK_EQ(flash.read(flash.ctx, base + 512u, half, sizeof(half)), MFS_OK);
    bool lo_ff = true;
    for (uint32_t i = 0; i < sizeof(half); i++)
      if (half[i] != 0xFFu)
        lo_ff = false;
    CHECK(lo_ff);
  }

  /* La capa HAL comprueba ademas que el tamano declarado es el del sector. */
  CHECK_EQ(mfs_stm32_hal_flash_erase_sector(base + 2048u, 4096u), MFS_EINVAL);

  TEST_END("borrado por tramo");
}

/* ------------------------------------------------------------------ */
/* HALLAZGO (bug de destino, no ejecutable en host): el numero de sector que
 * calcula mfs_stm32_hal.c:169 es
 *
 *     sector = (addr - MFS_STM32_FLASH_BASE_ADDR) / size;
 *
 * lo que solo coincide con el numero de sector del HAL si TODOS los sectores
 * miden lo mismo (H7/U5: 128 KB u 8 KB; L4/G4: 2 KB). En las familias NO
 * uniformes (F4/F7) el cociente no es el indice: para el sector de 128 KB en
 * 0x08020000 vale 1, cuando el indice correcto es 5. Este test lo demuestra
 * sobre el modelo (que resuelve el indice contra la tabla, como el silicio). */
static void test_erase_sector_index(void) {
  TEST_BEGIN("indice de sector: formula del port vs tabla del dispositivo");

  shim_flash_reset();
  uint8_t *raw = shim_flash_raw();
  CHECK(raw != NULL);
  if (!raw) {
    TEST_END("indice de sector");
    return;
  }
  /* Marca en el sector 1 (16 KB, 0x08004000) y en el 5 (128 KB, 0x08020000). */
  raw[0x04000] = 0x11u;
  raw[0x20000] = 0x55u;

  const uint32_t addr = 0x08020000u;
  const uint32_t size = 128u * 1024u;
  const uint32_t by_formula = (addr - 0x08000000u) / size;
  const int32_t correct = mfs_sector_index_of(shim_sectors, shim_sector_count, addr);

  CHECK_EQ(by_formula, 1u);  /* lo que enviaria el port al HAL */
  CHECK_EQ(correct, 5);      /* el sector que de verdad contiene `addr` */
  CHECK(by_formula != (uint32_t)correct);

  /* Consecuencia en silicio: el HAL borra el sector que le piden POR INDICE. */
  FLASH_EraseInitTypeDef ei;
  memset(&ei, 0, sizeof(ei));
  ei.TypeErase = FLASH_TYPEERASE_SECTORS;
  ei.Sector = by_formula;
  ei.NbSectors = 1u;
  uint32_t err = 0u;
  CHECK_EQ(HAL_FLASHEx_Erase(&ei, &err), HAL_OK);
  CHECK_EQ(raw[0x04000], 0xFFu); /* se borro el sector EQUIVOCADO */
  CHECK_EQ(raw[0x20000], 0x55u); /* el sector pedido ni se toco */

  /* Con el indice correcto se borra el sector pedido. */
  ei.Sector = (uint32_t)correct;
  CHECK_EQ(HAL_FLASHEx_Erase(&ei, &err), HAL_OK);
  CHECK_EQ(raw[0x20000], 0xFFu);

  TEST_END("indice de sector");
}

/* ------------------------------------------------------------------ */
static void test_program_unit_invariant(void) {
  TEST_BEGIN("invariante de unidad/ECC en el RMW del backend");

  shim_flash_reset();
  CHECK(shim_flash_ready());

  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = REGION_ADDR;
  c.region_size = REGION_SIZE;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  c.ram_total = 32768u;

  /* Se prepara el descriptor directamente para poder llamar a sus callbacks. */
  static mfs_embedded_flash_t flash;
  CHECK_EQ(mfs_stm32_iflash_prepare(&c, &flash), MFS_OK);
  CHECK(flash.read != NULL && flash.prog != NULL && flash.erase != NULL);
  CHECK_EQ(flash.erase_unit, 128u * 1024u);
  CHECK_EQ(flash.program_granularity, (uint32_t)MFS_STM32_HOST_PGM_UNIT);

  const uint32_t unit = (uint32_t)MFS_STM32_HOST_PGM_UNIT;
  const uint32_t base = flash.base_addr;

  /* A = patron con bits a 0 de sobra; C ⊂ A (programable por NOR sin borrar);
   * B no esta contenido en A (exigiria borrar el sector). */
  const uint8_t A[4] = {0x11u, 0x22u, 0x33u, 0x44u};
  const uint8_t B[4] = {0xAAu, 0xBBu, 0xCCu, 0xDDu};
  const uint8_t C[4] = {0x01u, 0x22u, 0x33u, 0x44u};

  CHECK_EQ(flash.erase(flash.ctx, base), MFS_OK);
  shim_flash_clear_flags();

  /* 1) Programacion sobre unidad virgen: camino normal (RMW de la unidad). */
  CHECK_EQ(flash.prog(flash.ctx, base, A, sizeof(A)), MFS_OK);
  const uint32_t calls_1 = shim_flash_program_count();
  CHECK(calls_1 >= 1u);

  uint8_t back[16];
  memset(back, 0, sizeof(back));
  CHECK_EQ(flash.read(flash.ctx, base, back, sizeof(A)), MFS_OK);
  CHECK(memcmp(back, A, sizeof(A)) == 0);

  /* 2) Datos DISTINTOS sobre la misma unidad: imposible sin borrar el sector ⇒
   *    MFS_EIO, sin llegar al HAL (por eso el modelo no marca doble
   *    programacion) y sin tocar el contenido. */
  CHECK_EQ(flash.prog(flash.ctx, base, B, sizeof(B)), MFS_EIO);
  CHECK_EQ(shim_flash_program_count(), calls_1);
  CHECK(!shim_flash_was_double_programmed());
  memset(back, 0, sizeof(back));
  CHECK_EQ(flash.read(flash.ctx, base, back, sizeof(A)), MFS_OK);
  CHECK(memcmp(back, A, sizeof(A)) == 0);

  /* 3) El MISMO dato: no-op, no se reprograma (seria PROGERR/ECCC con ECC). */
  CHECK_EQ(flash.prog(flash.ctx, base, A, sizeof(A)), MFS_OK);
  CHECK_EQ(shim_flash_program_count(), calls_1);

  /* 4) Dato CONTENIDO en el actual (C ⊂ A): tambien no-op y MFS_OK. */
  CHECK_EQ(flash.prog(flash.ctx, base, C, sizeof(C)), MFS_OK);
  CHECK_EQ(shim_flash_program_count(), calls_1);
  CHECK(!shim_flash_was_double_programmed());

  /* 5) Escritura PARCIAL (< unidad) sobre unidad virgen: RMW de la unidad
   *    completa y el resto de la unidad sigue a 0xFF (el backend no debe
   *    inventarse datos en los bytes que no se pidieron). */
  CHECK_EQ(flash.erase(flash.ctx, base + flash.erase_unit), MFS_OK);
  const uint8_t part[4] = {0xAAu, 0x55u, 0x00u, 0xFFu};
  CHECK_EQ(flash.prog(flash.ctx, base + flash.erase_unit, part, sizeof(part)),
           MFS_OK);
  memset(back, 0, sizeof(back));
  CHECK_EQ(flash.read(flash.ctx, base + flash.erase_unit, back, sizeof(part)),
           MFS_OK);
  CHECK(memcmp(back, part, sizeof(part)) == 0);
  if (unit > (uint32_t)sizeof(part)) {
    uint32_t rest = unit - (uint32_t)sizeof(part);
    uint8_t tail[16];
    memset(tail, 0, sizeof(tail));
    CHECK_EQ(flash.read(flash.ctx, base + flash.erase_unit +
                                       (uint32_t)sizeof(part),
                        tail, rest), MFS_OK);
    for (uint32_t i = 0; i < rest; i++)
      CHECK_EQ(tail[i], 0xFFu);
  }

  /* 6) Escritura que CRUZA la frontera de unidad: dos unidades, ambas virgenes. */
  CHECK_EQ(flash.erase(flash.ctx, base + 2u * flash.erase_unit), MFS_OK);
  const uint8_t cross[4] = {0x01u, 0x02u, 0x03u, 0x04u};
  const uint32_t cross_addr = base + 2u * flash.erase_unit + unit - 2u;
  CHECK_EQ(flash.prog(flash.ctx, cross_addr, cross, sizeof(cross)), MFS_OK);
  memset(back, 0, sizeof(back));
  CHECK_EQ(flash.read(flash.ctx, cross_addr, back, sizeof(cross)), MFS_OK);
  CHECK(memcmp(back, cross, sizeof(cross)) == 0);

  /* 7) Borrado: la misma programacion vuelve a ser valida tras borrar. */
  CHECK_EQ(flash.erase(flash.ctx, base), MFS_OK);
  CHECK_EQ(flash.prog(flash.ctx, base, B, sizeof(B)), MFS_OK);

  /* 8) Argumentos fuera de la region y longitudes nulas: tipificados, sin
   *    acceso a memoria fuera del medio. */
  CHECK_EQ(flash.prog(flash.ctx, 0x20000000u, A, sizeof(A)), MFS_EINVAL);
  CHECK_EQ(flash.prog(flash.ctx, base, A, 0u), MFS_EINVAL);
  CHECK_EQ(flash.prog(flash.ctx, base, NULL, sizeof(A)), MFS_EINVAL);
  CHECK_EQ(flash.read(flash.ctx, 0x20000000u, back, sizeof(A)), MFS_EINVAL);
  CHECK_EQ(flash.read(flash.ctx, base, NULL, sizeof(A)), MFS_EINVAL);
  CHECK_EQ(flash.erase(flash.ctx, 0x20000000u), MFS_EINVAL);
  /* Borrado que no empieza en frontera de la unidad logica. */
  CHECK_EQ(flash.erase(flash.ctx, base + 512u), MFS_EINVAL);

  /* 9) El HAL/MODELO si enforza PROGERR: se le pide directamente reprogramar la
   *    unidad ya escrita (el backend nunca llega aqui). Con ECC la unidad no
   *    virgen da error y marca la bandera; sin ECC vale la semantica NOR
   *    (los bits solo van de 1 a 0), que es la diferencia entre familias. */
  shim_flash_clear_flags();
  /* E NO esta contenido en B (B & E != E): exige BORRAR el sector. Se pasa un
   * buffer DEL TAMANO DE LA UNIDAD (como hace el backend): los bytes que no se
   * piden van a 0xFF, que es su valor actual y no cambia nada. */
  const uint8_t E[4] = {0xFFu, 0x0Fu, 0xF0u, 0x00u};
  uint8_t uni[16];
  memset(uni, 0xFFu, sizeof(uni));
  memcpy(uni, E, sizeof(E));
  mfs_st hal_st = mfs_stm32_hal_flash_program_bytes(base, uni, unit);
  /* Bytes realmente cubiertos por UNA programacion del dispositivo. */
  const uint32_t unit_n = unit < (uint32_t)sizeof(E) ? unit : (uint32_t)sizeof(E);
#if MFS_STM32_HOST_ECC
  CHECK_EQ(hal_st, MFS_EIO);
  CHECK(shim_flash_was_double_programmed());
#else
  CHECK_EQ(hal_st, MFS_OK);
  CHECK(!shim_flash_was_double_programmed());
  {
    uint8_t cur_after[4];
    memset(cur_after, 0, sizeof(cur_after));
    CHECK_EQ(flash.read(flash.ctx, base, cur_after, sizeof(cur_after)), MFS_OK);
    for (uint32_t i = 0; i < unit_n; i++)
      CHECK_EQ(cur_after[i], (uint8_t)(B[i] & E[i])); /* NOR: old & data */
    for (uint32_t i = unit_n; i < sizeof(E); i++)
      CHECK_EQ(cur_after[i], B[i]); /* fuera de la unidad: intacto */
  }
#endif

  /* 10) El modelo no es "prohibir todo": si el dato pedido YA ESTA CONTENIDO en
   *     la unidad, reprogramarla es un no-op ACEPTADO (con ECC, reescribir la
   *     unidad daria PROGERR; sin ECC, la semantica NOR escribiria old & data,
   *     que es justo el valor contenido). Es la diferencia entre familias que
   *     explica por que el backend NO puede limitarse a "leer-modificar-
   *     escribir". */
  {
    const uint8_t sub[4] = {(uint8_t)(B[0] & 0x0Fu), (uint8_t)(B[1] & 0x0Fu),
                            (uint8_t)(B[2] & 0x0Fu), (uint8_t)(B[3] & 0x0Fu)};
    /* Precondicion: la unidad contiene B (el paso 9 pudo cambiar otros bits). */
    CHECK_EQ(flash.erase(flash.ctx, base), MFS_OK);
    CHECK_EQ(flash.prog(flash.ctx, base, B, sizeof(B)), MFS_OK);
    shim_flash_clear_flags();

    uint8_t uni2[16];
    memset(uni2, 0xFFu, sizeof(uni2));
    memcpy(uni2, sub, sizeof(sub));
    CHECK_EQ(mfs_stm32_hal_flash_program_bytes(base, uni2, unit), MFS_OK);
    uint8_t after[4];
    memset(after, 0, sizeof(after));
    CHECK_EQ(flash.read(flash.ctx, base, after, sizeof(after)), MFS_OK);
    uint8_t expect[4];
    memcpy(expect, B, sizeof(expect));
    for (uint32_t i = 0; i < unit_n; i++)
      expect[i] = (uint8_t)(B[i] & sub[i]);
#if MFS_STM32_HOST_ECC
    /* No se programo nada: la unidad conserva B. */
    CHECK(memcmp(after, B, sizeof(B)) == 0);
    CHECK(shim_flash_was_double_programmed()); /* se INTENTO, y se tolero */
#else
    /* NOR: los bits solo van de 1 a 0 en los bytes que cubre la unidad. */
    CHECK(memcmp(after, expect, sizeof(expect)) == 0);
    CHECK(!shim_flash_was_double_programmed());
#endif
  }

  TEST_END("invariante de unidad/ECC");
}

/* ------------------------------------------------------------------ */
static void test_misconfiguration(void) {
  TEST_BEGIN("configuracion invalida rechazada con error tipificado");

  shim_flash_reset();
  static mf_t fs;
  mfs_stm32_cfg c;

  /* Sin region reservada. */
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.ram_total = 32768u;
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);

  /* Sin presupuesto de RAM declarado. */
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = REGION_ADDR;
  c.region_size = REGION_SIZE;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);

  /* Tabla de sectores que NO valida (hueco): no se monta sobre una geometria
   * sin verificar, porque un borrado iria al sector equivocado. */
  {
    static mfs_sector_t bad[12];
    memcpy(bad, shim_sectors, sizeof(bad));
    bad[6].addr += 0x1000u; /* hueco + solape */
    memset(&c, 0, sizeof(c));
    c.media = MFS_STM32_MEDIA_IFLASH;
    c.region_addr = REGION_ADDR;
    c.region_size = REGION_SIZE;
    c.sectors = bad;
    c.sector_count = 12u;
    c.ram_total = 32768u;
    CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);
    CHECK_EQ(matrixfs_stm32_format(&fs, &c), MFS_EINVAL);
  }

  /* Region fuera del dispositivo: la ventana uniforme no existe. */
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = 0x20000000u;
  c.region_size = 0x1000u;
  c.sectors = shim_sectors;
  c.sector_count = shim_sector_count;
  c.ram_total = 32768u;
  CHECK(matrixfs_stm32_mount(&fs, &c) != MFS_OK);

  /* Medio no soportado / handles ausentes: la validacion es previa al backend,
   * de modo que ni siquiera se toca el periferico. */
  memset(&c, 0, sizeof(c));
  c.media = (mfs_stm32_media_t)99;
  c.ram_total = 32768u;
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);

  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_OSPI_NOR;
  c.ospi = NULL;
  c.ram_total = 32768u;
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);

  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_FRAM;
  c.bus = MFS_STM32_BUS_SPI;
  c.spi = (void *)0x1u; /* handle cualquiera: el stub devuelve ENOTSUP */
  c.ram_total = 32768u;
  c.total_size = 0u; /* FRAM sin geometria declarada: no hay autodeteccion */
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);

  /* Punteros nulos. */
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = REGION_ADDR;
  c.region_size = REGION_SIZE;
  c.ram_total = 32768u;
  CHECK_EQ(matrixfs_stm32_mount(NULL, &c), MFS_EINVAL);
  CHECK_EQ(matrixfs_stm32_mount(&fs, NULL), MFS_EINVAL);
  CHECK_EQ(matrixfs_stm32_format(NULL, &c), MFS_EINVAL);
  CHECK_EQ(matrixfs_stm32_deinit(NULL), MFS_EINVAL);

  TEST_END("configuracion invalida");
}

/* ------------------------------------------------------------------ */
static int g_lock_calls = 0;
static int g_unlock_calls = 0;
static void test_lock_hook(void) { g_lock_calls++; }
static void test_unlock_hook(void) { g_unlock_calls++; }

static void test_misc(void) {
  TEST_BEGIN("diagnostico, nombres de medio y hooks de bloqueo");

  for (int m = 0; m <= (int)MFS_STM32_MEDIA_SDMMC; m++)
    CHECK(matrixfs_stm32_media_name((mfs_stm32_media_t)m) != NULL);
  CHECK(matrixfs_stm32_media_name((mfs_stm32_media_t)99) != NULL);
  CHECK(matrixfs_stm32_last_error() != NULL);

  /* Sin hooks, lock/unlock son no-op. */
  matrixfs_stm32_set_lock_hooks(NULL, NULL);
  matrixfs_stm32_lock();
  matrixfs_stm32_unlock();
  CHECK_EQ(g_lock_calls, 0);
  CHECK_EQ(g_unlock_calls, 0);

  /* Con hooks, se invocan en el montaje. */
  matrixfs_stm32_set_lock_hooks(test_lock_hook, test_unlock_hook);
  static mf_t fs;
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));
  c.media = MFS_STM32_MEDIA_IFLASH;
  c.region_addr = 0x08000000u;
  c.ram_total = 32768u; /* region_addr sin tabla ⇒ EINVAL tras validar cfg */
  CHECK_EQ(matrixfs_stm32_mount(&fs, &c), MFS_EINVAL);
  (void)matrixfs_stm32_mount(&fs, &c);
  matrixfs_stm32_set_lock_hooks(NULL, NULL);

  TEST_END("diagnostico");
}

/* ------------------------------------------------------------------ */
int main(void) {
  /* Sin buffer: si un test provoca un fallo de acceso, la fase en curso no debe
   * perderse al redirigir la salida. */
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("== MatrixFS port STM32Cube (host) ==\n");
  printf("   unidad de programacion = %u B, ECC = %s\n",
         (unsigned)MFS_STM32_HOST_PGM_UNIT, MFS_STM32_HOST_ECC ? "si" : "no");

  shim_flash_reset();
  if (!shim_flash_ready()) {
    printf("  [ABORT] el modelo de flash no tiene imagen\n");
    return 2;
  }
  /* Informativo: si el host concede 0x08000000, el modelo sirve tambien para un
   * backend que lea con puntero directo (XIP). El port actual lee por la capa
   * HAL, de modo que no es un requisito. */
  printf("   imagen del modelo en %s\n",
         shim_flash_at_flash_base() ? "0x08000000" : "RAM estatica");

  test_flash_map();
  test_uniform_window();
  test_layout_constraint();
  test_mount_write_read();
  test_wide_write();
  test_multisector_erase();
  test_erase_sector_index();
  test_program_unit_invariant();
  test_misconfiguration();
  test_misc();

  printf("---------------------------------------------------\n");
  printf("checks: %d   fallos: %d\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
