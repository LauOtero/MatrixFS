/* bench.c — banco de rendimiento reducido (§17 MFS-Bench v2)
 * W1 (append-log), W3 (random write) y coste de montaje. Reporta p50/p99/p99.9
 * de latencia de operación y WAF observado por HCT. */
#include "mfs_harness.h"
#include "mfs_test.h"
#include <stdlib.h>

#define RAM_BENCH 32768u /* Compact: chunk 512 B, 7 páginas/zona */

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

static void pct(const char *name, uint32_t *v, uint32_t n) {
  if (n == 0u)
    return;
  qsort(v, n, sizeof(v[0]), cmp_u32);
  printf("   %-22s p50=%u us  p99=%u us  p99.9=%u us\n", name, v[n / 2u],
         v[(n * 99u) / 100u], v[(n * 999u) / 1000u]);
}

void bench_run(void) {
  TEST_BEGIN("MFS-Bench v2 (reducido)");
  mfs_env_t e;
  if (!env_open(&e, RAM_BENCH, NULL)) {
    CHECK(false);
    return;
  }
  CHECK_EQ(env_format(&e), MFS_OK);

  const uint32_t chunk = mfs_payload_bytes(&e.fs);
  static uint8_t buf[MFS_CHUNK_EXTENDED];
  for (uint32_t i = 0; i < chunk; i++)
    buf[i] = (uint8_t)(i * 3u);

  /* ---- W1: append-log secuencial ---- */
  const uint32_t N1 = 96u;
  static uint32_t lat1[256];
  mfs_file *f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/w1.log", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  uint32_t t0 = mfs_port_time_us();
  uint32_t ok = 0;
  for (uint32_t i = 0; i < N1; i++) {
    uint32_t a = mfs_port_time_us();
    size_t wr = 0;
    int r = mf_write(f, buf, chunk, &wr);
    uint32_t b = mfs_port_time_us();
    if (r == MFS_OK && wr == chunk)
      ok++;
    lat1[i] = b - a;
  }
  uint32_t t1 = mfs_port_time_us();
  mf_close(f);
  double secs = (double)(t1 - t0) / 1e6;
  double mbps =
      (secs > 0.0) ? ((double)ok * chunk / (1024.0 * 1024.0)) / secs : 0.0;
  printf("   W1 append %u×%u B: %.2f MB/s (%u ok, %.1f ms)\n", N1, chunk, mbps,
         ok, secs * 1000.0);
  pct("W1 latencia", lat1, N1);
  CHECK(ok > 0u);

  /* ---- W3: random write sobre un archivo ---- */
  const uint32_t N3 = 96u;
  static uint32_t lat3[256];
  f = NULL;
  CHECK_EQ(
      mf_open(&e.fs, "/w3.dat", MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f),
      MFS_OK);
  uint32_t seed = 0x1234ABCDu;
  uint32_t ok3 = 0;
  for (uint32_t i = 0; i < N3; i++) {
    seed = seed * 1103515245u + 12345u;
    uint32_t off = (seed >> 8) % chunk; /* dentro de la 1ª página */
    uint32_t a = mfs_port_time_us();
    int r = mf_seek(f, off, MFS_SEEK_SET);
    size_t wr = 0;
    if (r == MFS_OK)
      r = mf_write(f, buf, chunk / 2u, &wr);
    lat3[i] = mfs_port_time_us() - a;
    if (r == MFS_OK && wr == chunk / 2u)
      ok3++;
  }
  mf_close(f);
  printf("   W3 random %u B: %u/%u ok\n", chunk / 2u, ok3, N3);
  pct("W3 latencia", lat3, N3);
  CHECK(ok3 > 0u);

  /* ---- WAF y J/Op observados por HCT ---- */
  mfs_health_t h;
  CHECK_EQ(mf_ioctl(&e.fs, MFS_IOCTL_HEALTH, &h), MFS_OK);
  double waf = (h.writes_host != 0u)
                   ? ((double)h.writes_prog / (double)h.writes_host)
                   : 0.0;
  printf("   HCT: prog=%u host=%u  WAF=%.3f  GC-reloc=%u  deuda=%u\n",
         h.writes_prog, h.writes_host, waf, h.gc_relocated, h.debt_gld);

  /* ---- coste de montaje (§17.2: ≤ 12 ms) ---- */
  uint32_t m0 = mfs_port_time_us();
  CHECK_EQ(env_remount(&e), MFS_OK);
  uint32_t m1 = mfs_port_time_us();
  printf("   Montaje: %u us\n", m1 - m0);
  CHECK((m1 - m0) < 12000u);

  env_close(&e);
  TEST_END("MFS-Bench v2 (reducido)");
}
