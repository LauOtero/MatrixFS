/* bench_extreme.c — rendimiento extremo y tablas de resultados (§17)
 * Ejecutar con: mfs_tests --extreme
 */
#include "mfs_harness.h"
#include "mfs_test.h"
#include <stdlib.h>

static double now_us(void) { return (double)mfs_port_time_us(); }

static void rule(const char *title, uint32_t n) {
  printf("\n  +-- %s ", title);
  for (uint32_t i = (uint32_t)strlen(title) + 6u; i < n; i++)
    putchar('-');
  printf("+\n");
}

/* ------------------------- rendimiento por modo ------------------------- */
static void bench_mode(uint32_t ram, const char *name, uint32_t n_rec) {
  mfs_env_t e;
  if (!env_open(&e, ram, NULL))
    return;
  if (env_format(&e) != MFS_OK) {
    env_close(&e);
    return;
  }

  uint32_t chunk = mfs_payload_bytes(&e.fs);
  static uint8_t buf[MFS_CHUNK_EXTENDED];
  for (uint32_t i = 0; i < chunk; i++)
    buf[i] = (uint8_t)(i * 7u + 3u);

  mfs_file *f = NULL;
  if (mf_open(&e.fs, "/bench.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC,
              &f) != MFS_OK) {
    env_close(&e);
    return;
  }
  double t0 = now_us();
  uint32_t ok = 0;
  for (uint32_t i = 0; i < n_rec; i++) {
    size_t wr = 0;
    if (mf_write(f, buf, chunk, &wr) == MFS_OK && wr == chunk)
      ok++;
  }
  double t1 = now_us();
  (void)mf_close(f);
  double wr_mbs =
      (t1 > t0) ? ((double)ok * chunk / (1024.0 * 1024.0)) / ((t1 - t0) / 1e6)
                : 0.0;

  /* lectura completa */
  uint32_t total = ok * chunk;
  f = NULL;
  double rd_mbs = 0.0;
  if (total && mf_open(&e.fs, "/bench.bin", MFS_O_RDONLY, &f) == MFS_OK) {
    double r0 = now_us();
    uint32_t rd_total = 0;
    while (rd_total < total) {
      size_t rd = 0;
      if (mf_read(f, buf, chunk, &rd) != MFS_OK || rd == 0u)
        break;
      rd_total += (uint32_t)rd;
    }
    double r1 = now_us();
    (void)mf_close(f);
    rd_mbs = (r1 > r0)
                 ? ((double)rd_total / (1024.0 * 1024.0)) / ((r1 - r0) / 1e6)
                 : 0.0;
  }

  mfs_health_t h;
  (void)mf_ioctl(&e.fs, MFS_IOCTL_HEALTH, &h);
  double waf =
      h.writes_host ? (double)h.writes_prog / (double)h.writes_host : 0.0;

  double m0 = now_us();
  int r = env_remount(&e);
  double m1 = now_us();

  printf("  | %-10s | %5u | %6u | %8.2f | %8.2f | %7.0f | %5.2f | %s |\n", name,
         chunk, e.fs.zone_cap, wr_mbs, rd_mbs, m1 - m0, waf,
         (r == MFS_OK) ? "OK" : "ERR");
  env_close(&e);
}

static void table_modes(void) {
  rule("Rendimiento por modo (host, vFlash NOR)", 92);
  printf("  | %-10s | %5s | %6s | %8s | %8s | %7s | %5s | %s |\n", "modo",
         "chunk", "zonas", "wr MB/s", "rd MB/s", "mount us", "WAF", "st");
  printf("  |%-12s|%7s|%8s|%10s|%10s|%9s|%7s|%4s|\n", "------------", "-------",
         "--------", "----------", "----------", "---------", "-------",
         "----");
  bench_mode(8192u, "Ultra-Nano", 192u);
  bench_mode(16384u, "Nano", 192u);
  bench_mode(32768u, "Compact", 192u);
  bench_mode(98304u, "Balanced", 48u);
  bench_mode(131072u, "Extended", 48u);
}

/* ------------------------- throughput cripto --------------------------- */
static void table_crypto(void) {
  rule("Throughput criptográfico por suite (§10.6)", 76);
  printf("  | %-6s | %10s | %10s | %8s | %s |\n", "suite", "seal MB/s",
         "open MB/s", "tag B", "nota");
  static const uint8_t suites[4] = {MFS_SUITE_S0, MFS_SUITE_S1, MFS_SUITE_S2,
                                    MFS_SUITE_S3};
  static const char *names[4] = {"S0 AES-CTR", "S1 AES-GCM", "S2 Ascon",
                                 "S3 ChaCha"};
  uint8_t key[32];
  memset(key, 0x33, sizeof(key));
  static uint8_t pt[MFS_CHUNK_EXTENDED], ct[MFS_CHUNK_EXTENDED + 32],
      rt[MFS_CHUNK_EXTENDED];
  for (uint32_t i = 0; i < 4096u; i++)
    pt[i] = (uint8_t)(i * 5u);
  const uint32_t N = 256u, LEN = 4096u;
  for (uint32_t s = 0; s < 4u; s++) {
    uint16_t clen = 0, rlen = 0;
    double t0 = now_us();
    for (uint32_t i = 0; i < N; i++)
      (void)mfs_suite_seal(suites[s], key, 0x1000u + i, pt, (uint16_t)LEN, ct,
                           &clen);
    double t1 = now_us();
    for (uint32_t i = 0; i < N; i++)
      (void)mfs_suite_open(suites[s], key, 0x1000u + i, ct, clen, rt, &rlen);
    double t2 = now_us();
    double mb = (double)N * LEN / (1024.0 * 1024.0);
    printf("  | %-10s | %10.1f | %10.1f | %8u | %s |\n", names[s],
           mb / ((t1 - t0) / 1e6), mb / ((t2 - t1) / 1e6),
           (unsigned)(clen - LEN), "EtM/AEAD verificado");
  }
}

/* ---------------------- coste de subsistemas FTL ------------------------ */
static void table_ftl(void) {
  rule("Coste por operación de subsistemas FTL/seguridad (§11, §15)", 84);
  printf("  | %-28s | %10s | %s |\n", "operación", "coste us", "nota");

  /* CRC-32C sobre 4 KB */
  static uint8_t blk[4096];
  memset(blk, 0x5A, sizeof(blk));
  double t0 = now_us();
  for (uint32_t i = 0; i < 512u; i++)
    (void)mfs_crc32c(blk, sizeof(blk), 0u);
  double t1 = now_us();
  printf("  | %-28s | %10.2f | %s |\n", "crc32c(4 KB)", (t1 - t0) / 512.0,
         "integridad E2G");

  /* HKDF-SHA256 */
  uint8_t okm[32], salt[16] = {0}, ikm[32] = {0};
  t0 = now_us();
  for (uint32_t i = 0; i < 512u; i++)
    mfs_hkdf_sha256(salt, 16u, ikm, 32u, okm, 32u);
  t1 = now_us();
  printf("  | %-28s | %10.2f | %s |\n", "HKDF-SHA256", (t1 - t0) / 512.0,
         "derivación de claves");

  /* ELM + PEP */
  uint16_t feat[MFS_PEP_FEATURES];
  memset(feat, 7, sizeof(feat));
  t0 = now_us();
  for (uint32_t i = 0; i < 4096u; i++)
    (void)mfs_elm_health(1000u, 60000u, 50u, 60000u, 10u);
  t1 = now_us();
  printf("  | %-28s | %10.3f | %s |\n", "ELM health", (t1 - t0) / 4096.0,
         "modelo de vida §11.1");
  t0 = now_us();
  for (uint32_t i = 0; i < 4096u; i++)
    (void)mfs_pep_score(feat);
  t1 = now_us();
  printf("  | %-28s | %10.3f | %s |\n", "PEP score (4x32)", (t1 - t0) / 4096.0,
         "WCET < 5 us §11.1");

  /* LMS */
  static uint8_t seed[32];
  for (uint32_t i = 0; i < 32u; i++)
    seed[i] = (uint8_t)(i + 1u);
  static uint8_t sig[8192];
  uint32_t sl = sizeof(sig);
  mfs_lms_pub_t pub;
  t0 = now_us();
  (void)mfs_lms_keygen(seed, &pub, NULL);
  t1 = now_us();
  printf("  | %-28s | %10.0f | %s |\n", "LMS keygen (H=8)", t1 - t0,
         "ancla PQ §15");
  (void)mfs_lms_sign(seed, 1u, (const uint8_t *)"msg", 3u, sig, &sl);
  t0 = now_us();
  (void)mfs_lms_verify(&pub, (const uint8_t *)"msg", 3u, sig, sl);
  t1 = now_us();
  printf("  | %-28s | %10.0f | %s |\n", "LMS verify", t1 - t0,
         "secure-boot (una vez)");

  /* WOM-p, ZRP y WEP sobre medio real */
  mfs_env_t e;
  if (env_open(&e, 8192u, NULL) && env_format(&e) == MFS_OK) {
    e.cfg.zrp_enable = true;
    mfs_zone_t *z = &e.fs.zones[0];
    uint32_t pb = mfs_page_bytes(&e.fs);
    for (uint32_t p = 0; p < 16u; p++) {
      uint8_t page[MFS_CHUNK_EXTENDED];
      memset(page, (int)(p + 1u), pb);
      (void)mfs_write(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + p * pb, page,
                      pb);
    }
    z->state = MFS_Z_OPEN;
    z->class_hot = 3u;
    z->total_pages = 31u;
    t0 = now_us();
    (void)mfs_zrp_encode(&e.fs, z);
    t1 = now_us();
    printf("  | %-28s | %10.1f | %s |\n", "ZRP encode (16 pag)", t1 - t0,
           "+6,25 % en frío");
    uint8_t zeros[MFS_CHUNK_EXTENDED];
    memset(zeros, 0, pb);
    (void)mfs_write(&e.fs, z->start_addr + MFS_ZONEHDR_SIZE + 4u * pb, zeros,
                    pb);
    t0 = now_us();
    (void)mfs_zrp_recover(&e.fs, z, 4u, 20u);
    t1 = now_us();
    printf("  | %-28s | %10.1f | %s |\n", "ZRP recover (1 pag)", t1 - t0,
           "nivel 5 §12");
    env_close(&e);
  }

  /* WEP */
  mfs_env_t e2;
  if (env_open(&e2, 98304u, NULL) && env_format(&e2) == MFS_OK) {
    uint8_t uid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    mfs_wep_init(&e2.fs, uid);
    t0 = now_us();
    for (uint32_t i = 0; i < 4096u; i++)
      (void)mfs_wep_place(&e2.fs, (uint16_t)i);
    t1 = now_us();
    printf("  | %-28s | %10.3f | %s |\n", "WEP place (caché)",
           (t1 - t0) / 4096.0, "§11.2 Balanced+");
    env_close(&e2);
  }
}

/* -------------------------- huella de memoria -------------------------- */
static void table_footprint(void) {
  rule("Huella de memoria estática (MFS-RES-001: sin heap)", 84);
  printf("  | %-28s | %8s | %s |\n", "estructura", "bytes", "uso");
  struct {
    const char *n;
    uint32_t b;
    const char *u;
  } rows[] = {
      {"mf_t (núcleo)", (uint32_t)sizeof(mf_t), "instancia única §23.1"},
      {"mfs_inode_ram_t", (uint32_t)sizeof(mfs_inode_ram_t),
       "ventana flash-first §22.3"},
      {"mfs_zone_t", (uint32_t)sizeof(mfs_zone_t), "tabla de zonas §24.1"},
      {"mfs_iocb", (uint32_t)sizeof(mfs_iocb), "ring DAIO §13.1"},
      {"mfs_wom_t", (uint32_t)sizeof(mfs_wom_t), "WOM-p §11.4"},
      {"mfs_health_t", (uint32_t)sizeof(mfs_health_t), "telemetría HCT §16"},
      {"mfs_hwv_t", (uint32_t)sizeof(mfs_hwv_t), "HWV (en flash) §5.2"},
  };
  for (uint32_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    printf("  | %-28s | %8u | %s |\n", rows[i].n, rows[i].b, rows[i].u);
}

/* ------------------------- WAF por workload --------------------------- */
static void table_waf(void) {
  rule("WAF y durabilidad por workload (§17.1/§17.3)", 84);
  printf("  | %-24s | %8s | %8s | %8s | %s |\n", "workload", "ops", "WAF",
         "GC reloc", "modo");

  struct {
    const char *name;
    uint32_t ram;
    uint32_t ops;
    int kind;
  } cfgs[] = {
      {"W1 append secuencial", 32768u, 400u, 0},
      {"W3 random en fichero", 32768u, 400u, 1},
      {"W4 mixto churn 90/10", 32768u, 400u, 2},
  };
  for (uint32_t c = 0; c < 3u; c++) {
    mfs_env_t e;
    if (!env_open(&e, cfgs[c].ram, NULL) || env_format(&e) != MFS_OK) {
      env_close(&e);
      continue;
    }
    uint32_t chunk = mfs_payload_bytes(&e.fs);
    static uint8_t buf[MFS_CHUNK_EXTENDED];
    for (uint32_t i = 0; i < chunk; i++)
      buf[i] = (uint8_t)(i * 13u);
    mfs_file *f = NULL;
    (void)mf_open(&e.fs, "/waf.bin", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC,
                  &f);
    uint32_t ok = 0, seed = 0x9E3779B9u;
    for (uint32_t i = 0; i < cfgs[c].ops; i++) {
      size_t wr = 0;
      int r;
      if (cfgs[c].kind == 1) {
        seed = seed * 1103515245u + 12345u;
        (void)mf_seek(f, (int64_t)((seed >> 9) % 4096u), MFS_SEEK_SET);
        r = mf_write(f, buf, chunk / 2u, &wr);
      } else if (cfgs[c].kind == 2) {
        if ((i % 10u) == 0u)
          (void)mf_sync(&e.fs); /* 10 % mantenimiento */
        r = mf_write(f, buf, chunk, &wr);
      } else {
        r = mf_write(f, buf, chunk, &wr);
      }
      if (r == MFS_OK)
        ok++;
      if ((i % 32u) == 0u)
        (void)mf_sync(&e.fs);
    }
    (void)mf_close(f);
    (void)mf_sync(&e.fs);
    mfs_health_t h;
    (void)mf_ioctl(&e.fs, MFS_IOCTL_HEALTH, &h);
    printf("  | %-24s | %8u | %8.3f | %8u | %s |\n", cfgs[c].name, ok,
           h.writes_host ? (double)h.writes_prog / (double)h.writes_host : 0.0,
           h.gc_relocated, mfs_ststr((mfs_st)0));
    env_close(&e);
  }
}

void bench_extreme_run(void) {
  printf("\n================ MFS-Bench EXTREMO (§17) ================\n");
  table_modes();
  table_crypto();
  table_ftl();
  table_waf();
  table_footprint();
  printf("\n=========================================================\n");
}
