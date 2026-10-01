/* mfstool.c — herramienta de build MatrixFS «ATLAS» (§26)
 *
 *   mfstool plan                     Genera matrixfs_resources.h + certificado
 * RSC firmado mfstool check-map [mapfile]       MFS-RES-002: ausencia de ruta
 * al heap mfstool train-dict <corpus> [out] [bytes]   Entrena un diccionario
 * ODT mfstool sign-profile <perfil> <clave-hex64> [out]  Firma un perfil
 * (B3-keyed) mfstool verify-hwv <hwv.bin> [out.pin]     Valida un HWV de 64 B
 *   mfstool bench                    Banco reducido sobre vFlash (W1/WAF/NOR)
 *
 * Forma parte del pipeline obligatorio de release (§26):
 *   plan → build → check-map → KATs → vFlash (FIH) → bench → firmar
 * certificado.
 */
#include "mfs_internal.h"
#include "vflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

#define RSC_CERT_FILE "rsc_certificate.txt"
#define RSC_HDR_FILE "matrixfs_resources.h"

/* Clave de build del certificado RSC. NO es material de producción: sólo
 * autentica que el certificado y el árbol de fuentes son coherentes entre sí
 * (MFS-SEC-002 prohíbe comitear claves reales; esta es pública por diseño). */
static const uint8_t RSC_BUILD_KEY[32] = {
    0x4Du, 0x46u, 0x53u, 0x2Du, 0x52u, 0x53u, 0x43u, 0x2Du, 0x62u, 0x75u, 0x69u,
    0x6Cu, 0x64u, 0x2Du, 0x6Bu, 0x65u, 0x79u, 0x2Du, 0x41u, 0x54u, 0x4Cu, 0x41u,
    0x53u, 0x2Du, 0x30u, 0x31u, 0x2Du, 0x31u, 0x2Eu, 0x30u, 0x00u, 0x01u};

static const char *const MODE_NAMES[MFS_MODE_COUNT] = {
    "Ultra-Nano", "Nano", "Compact", "Balanced", "Extended", "8U", "8N", "8C"};

/* Sufijo del macro MFS_RAM_<sufijo> por modo (coincide con mfs_types.h) */
static const char *const MODE_MACROS[MFS_MODE_COUNT] = {
    "ULTRA_NANO", "NANO",       "COMPACT",   "BALANCED",
    "EXTENDED",   "8BIT_ULTRA", "8BIT_NANO", "8BIT_COMPACT"};

/* =====================================================================
 * plan — certificado de recursos estáticos (§7.1, §23.2, §26)
 * ===================================================================== */
static void hex32(const uint8_t in[32], char out[65]) {
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = d[in[i] >> 4];
    out[i * 2 + 1] = d[in[i] & 0x0Fu];
  }
  out[64] = '\0';
}

static int cmd_plan(void) {
  printf("mfstool plan — certificado de recursos estáticos (RSC)\n");

  /* --- 1. Cabecera con static_assert por modo (§23.2) --- */
  FILE *h = fopen(RSC_HDR_FILE, "w");
  if (!h) {
    fprintf(stderr, "no se pudo crear matrixfs_resources.h\n");
    return 1;
  }
  fprintf(
      h,
      "/* matrixfs_resources.h — GENERADO por mfstool plan (no editar) */\n");
  fprintf(h, "#ifndef MATRIXFS_RESOURCES_H\n#define MATRIXFS_RESOURCES_H\n\n");
  fprintf(h, "#include <assert.h>\n");
  fprintf(h, "#include \"matrixfs/mfs_types.h\"\n\n");
  fprintf(h, "#ifndef MFS_STATIC_ASSERT\n#define MFS_STATIC_ASSERT(c, m) "
             "_Static_assert(c, m)\n#endif\n\n");
  fprintf(h, "/* Presupuesto RAM normativo por modo (§18.2) */\n");
  for (int m = 0; m < (int)MFS_MODE_COUNT; m++) {
    fprintf(h, "#define MFS_RAM_%s %uu\n", MODE_MACROS[m],
            (unsigned)mfs_limits[m].ram_total);
  }
  fprintf(h, "\n/* Huella real del núcleo (MFS-RES-001: sin heap) */\n");
  fprintf(h, "#define MFS_SIZEOF_MF_T %uu\n", (unsigned)sizeof(mf_t));
  fprintf(h, "#define MFS_SIZEOF_INODE %uu\n",
          (unsigned)sizeof(mfs_inode_ram_t));
  fprintf(h, "#define MFS_SIZEOF_ZONE %uu\n", (unsigned)sizeof(mfs_zone_t));
  fprintf(
      h,
      "\nMFS_STATIC_ASSERT(sizeof(mf_t) <= MFS_RAM_EXTENDED,\n"
      "                  \"mf_t excede el presupuesto RAM de Extended\");\n");
  fprintf(h, "\n#endif /* MATRIXFS_RESOURCES_H */\n");
  fclose(h);
  printf("  escrito %s\n", RSC_HDR_FILE);

  /* --- 2. Cuerpo del certificado --- */
  char body[2048];
  int n = snprintf(body, sizeof(body),
                   "MatrixFS «ATLAS» v1.0 — certificado RSC\n"
                   "generador: mfstool plan\n"
                   "modos cubiertos: %d\n",
                   (int)MFS_MODE_COUNT);
  for (int m = 0; m < (int)MFS_MODE_COUNT; m++) {
    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "  %-11s ram=%u B stack=%u B chunk=%u B max_file=%u KiB\n",
                  MODE_NAMES[m], (unsigned)mfs_limits[m].ram_total,
                  (unsigned)mfs_limits[m].own_stack,
                  (unsigned)mfs_limits[m].chunk_size,
                  (unsigned)mfs_limits[m].max_file_size_kb);
  }
  n +=
      snprintf(body + n, sizeof(body) - (size_t)n,
               "huella: mf_t=%u B inodo=%u B zona=%u B\n"
               "cotas globales (§17.2): montaje<=12ms p99.9 RT-A<=2.5ms "
               "corrupcion=0\n"
               "suites: S0 AES-256-CTR+HMAC | S1 AES-256-GCM | "
               "S2 Ascon-128a | S3 ChaCha20-Poly1305\n"
               "fallback: software para todo acelerador ausente (MFS-HW-001)\n",
               (unsigned)sizeof(mf_t), (unsigned)sizeof(mfs_inode_ram_t),
               (unsigned)sizeof(mfs_zone_t));

  uint8_t mac32[32];
  mfs_b3_256(RSC_BUILD_KEY, 32u, (const uint8_t *)body, (uint32_t)n, mac32);
  char hex[65];
  hex32(mac32, hex);

  FILE *c = fopen(RSC_CERT_FILE, "w");
  if (!c) {
    fprintf(stderr, "no se pudo crear %s\n", RSC_CERT_FILE);
    return 1;
  }
  fwrite(body, 1u, (size_t)n, c);
  fprintf(c, "MAC RSC (BLAKE3-keyed): %s\n", hex);
  fclose(c);
  printf("  escrito %s (firmado)\n", RSC_CERT_FILE);
  printf("  modos cubiertos: %d ; cotas: montaje<=12ms, p99.9 RT-A<=2.5ms, "
         "corrupcion=0\n",
         (int)MFS_MODE_COUNT);
  return 0;
}

/* =====================================================================
 * check-map — MFS-RES-002: ninguna ruta al heap en el núcleo
 * ===================================================================== */
static const char *const FORBIDDEN[] = {"malloc", "calloc", "realloc", "strdup",
                                        "alloca"};

/* Devuelve true si `line` contiene un símbolo prohibido como identificador. */
static bool line_has_heap(const char *line, const char **which) {
  for (size_t f = 0; f < sizeof(FORBIDDEN) / sizeof(FORBIDDEN[0]); f++) {
    const char *p = line;
    while ((p = strstr(p, FORBIDDEN[f])) != NULL) {
      bool lok =
          (p == line) ||
          !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') ||
            (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_');
      char next = p[strlen(FORBIDDEN[f])];
      bool rok =
          !((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
            (next >= '0' && next <= '9') || next == '_');
      if (lok && rok) {
        *which = FORBIDDEN[f];
        return true;
      }
      p += strlen(FORBIDDEN[f]);
    }
  }
  return false;
}

static int check_file(const char *path, unsigned *nfiles, unsigned *nbad) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0; /* el fichero puede no existir en un árbol parcial */
  (*nfiles)++;
  char line[1024];
  unsigned ln = 0;
  int bad_here = 0;
  while (fgets(line, (int)sizeof(line), f)) {
    ln++;
    const char *w = NULL;
    if (line_has_heap(line, &w)) {
      printf("  [FAIL] %s:%u usa '%s'\n", path, ln, w);
      (*nbad)++;
      bad_here = 1;
    }
  }
  fclose(f);
  return bad_here;
}

/* Recorre un directorio y aplica check_file a los .c/.h que contenga. */
static void check_dir(const char *dir, unsigned *nfiles, unsigned *nbad) {
  char path[512];
#if defined(_WIN32)
  struct _finddata_t fd;
  char pattern[512];
  snprintf(pattern, sizeof(pattern), "%s/*", dir);
  intptr_t h = _findfirst(pattern, &fd);
  if (h == -1)
    return;
  do {
    if (fd.attrib & _A_SUBDIR)
      continue;
    size_t l = strlen(fd.name);
    if (l < 2u || (strcmp(fd.name + l - 2, ".c") != 0 &&
                   strcmp(fd.name + l - 2, ".h") != 0))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
    (void)check_file(path, nfiles, nbad);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t l = strlen(e->d_name);
    if (l < 2u || (strcmp(e->d_name + l - 2, ".c") != 0 &&
                   strcmp(e->d_name + l - 2, ".h") != 0))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    (void)check_file(path, nfiles, nbad);
  }
  closedir(d);
#endif
}

static int cmd_check_map(const char *mapfile) {
  printf("mfstool check-map — MFS-RES-002: ausencia de ruta al heap\n");
  unsigned nfiles = 0, nbad = 0;
  check_dir("src/core", &nfiles, &nbad);
  check_dir("src/crypto", &nfiles, &nbad);
  if (nbad == 0u)
    printf("  escaneados %u fichero(s) de src/core y src/crypto\n", nfiles);
  if (mapfile) {
    unsigned mf = 0;
    (void)check_file(mapfile, &mf, &nbad);
    printf("  mapa de enlazado: %s (%u fichero(s))\n", mapfile, mf);
  }
  if (nbad != 0u) {
    printf("  FAIL: %u referencia(s) a memoria dinámica (MFS-RES-001)\n", nbad);
    return 1;
  }
  /* Fail-closed: un escaneo que no encuentra fuentes no demuestra nada. El
   * comando debe ejecutarse desde la raíz del proyecto. */
  if (nfiles == 0u) {
    printf("  FAIL: no se ha escaneado ningún fichero (ejecute desde la raíz "
           "del proyecto)\n");
    return 1;
  }
  printf("  PASS: sin asignación dinámica en el núcleo (MFS-RES-001/002)\n");
  return 0;
}

/* =====================================================================
 * train-dict — diccionario ODT de bigramas frecuentes (§10.1)
 * ===================================================================== */
static int cmd_train_dict(const char *corpus, const char *out,
                          uint32_t budget) {
  FILE *f = fopen(corpus, "rb");
  if (!f) {
    fprintf(stderr, "train-dict: no se puede abrir '%s'\n", corpus);
    return 1;
  }
  uint32_t *freq = (uint32_t *)calloc(65536u, sizeof(uint32_t));
  if (!freq) {
    fclose(f);
    fprintf(stderr, "train-dict: sin memoria para la tabla de bigramas\n");
    return 1;
  }
  int prev = -1;
  int ch;
  while ((ch = fgetc(f)) != EOF) {
    if (prev >= 0)
      freq[((uint32_t)prev << 8) | (uint32_t)ch]++;
    prev = ch;
  }
  fclose(f);

  if (out == NULL)
    out = "matrixfs.dict";
  if (budget == 0u)
    budget = 4096u;
  uint32_t max_entries = (budget > 14u) ? (budget - 14u) / 4u : 0u;

  FILE *d = fopen(out, "wb");
  if (!d) {
    free(freq);
    fprintf(stderr, "train-dict: no se puede crear '%s'\n", out);
    return 1;
  }
  uint8_t hdr[14];
  memcpy(hdr, "MFOD", 4u);
  mfs_st16(hdr + 4, 1u); /* versión */
  mfs_st16(hdr + 6, 4u); /* tamaño de entrada: [a b freq u16] */
  mfs_st16(hdr + 8, 0u);
  mfs_st32(hdr + 10, 0u); /* nº de entradas (se reescribe al final) */
  fwrite(hdr, 1u, sizeof(hdr), d);

  uint32_t written = 0;
  for (uint32_t a = 0; a < 256u && written < max_entries; a++) {
    for (uint32_t b = 0; b < 256u && written < max_entries; b++) {
      uint32_t v = freq[(a << 8) | b];
      if (v == 0u)
        continue;
      uint8_t ent[4];
      ent[0] = (uint8_t)a;
      ent[1] = (uint8_t)b;
      mfs_st16(ent + 2, (uint16_t)((v > 0xFFFFu) ? 0xFFFFu : v));
      fwrite(ent, 1u, sizeof(ent), d);
      written++;
    }
  }
  free(freq);
  fseek(d, 10L, SEEK_SET);
  uint8_t cnt[4];
  mfs_st32(cnt, written);
  fwrite(cnt, 1u, sizeof(cnt), d);
  fclose(d);
  printf("train-dict: %u bigrama(s) -> %s (%u bytes)\n", written, out,
         written * 4u + 14u);
  return 0;
}

/* =====================================================================
 * sign-profile — firma BLAKE3-keyed (§15, MFS-B3-001)
 * ===================================================================== */
static int cmd_sign_profile(const char *profile, const char *keyhex,
                            const char *out) {
  if (strlen(keyhex) != 64u) {
    fprintf(stderr, "sign-profile: la clave debe ser hex de 64 caracteres\n");
    return 1;
  }
  uint8_t key[32];
  for (int i = 0; i < 32; i++) {
    unsigned v = 0u;
    if (sscanf(keyhex + i * 2, "%2x", &v) != 1) {
      fprintf(stderr, "sign-profile: clave hex inválida\n");
      return 1;
    }
    key[i] = (uint8_t)v;
  }
  FILE *f = fopen(profile, "rb");
  if (!f) {
    fprintf(stderr, "sign-profile: no se puede abrir '%s'\n", profile);
    return 1;
  }
  fseek(f, 0L, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0L, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return 1;
  }
  uint8_t *buf = (uint8_t *)malloc((size_t)sz + 32u);
  if (!buf) {
    fclose(f);
    fprintf(stderr, "sign-profile: sin memoria\n");
    return 1;
  }
  size_t rd = fread(buf, 1u, (size_t)sz, f);
  fclose(f);
  uint8_t mac[32];
  mfs_b3_256(key, 32u, buf, (uint32_t)rd, mac);
  memcpy(buf + rd, mac, 32u); /* append de 32 B de MAC */
  if (out == NULL)
    out = profile;
  FILE *o = fopen(out, "wb");
  if (!o) {
    free(buf);
    fprintf(stderr, "sign-profile: no se puede crear '%s'\n", out);
    return 1;
  }
  fwrite(buf, 1u, rd + 32u, o);
  fclose(o);
  free(buf);
  char hex[65];
  hex32(mac, hex);
  printf("sign-profile: %s firmado (%zu + 32 B) MAC=%s\n", out, rd, hex);
  return 0;
}

/* =====================================================================
 * verify-hwv — valida un HWV de 64 B (§5.2)
 * ===================================================================== */
static int cmd_verify_hwv(const char *bin, const char *pin) {
  FILE *f = fopen(bin, "rb");
  if (!f) {
    fprintf(stderr, "verify-hwv: no se puede abrir '%s'\n", bin);
    return 1;
  }
  uint8_t raw[64];
  size_t rd = fread(raw, 1u, sizeof(raw), f);
  fclose(f);
  if (rd != sizeof(raw)) {
    fprintf(stderr, "verify-hwv: se esperaban 64 B y hay %zu\n", rd);
    return 1;
  }
  mfs_hwv_t h;
  mfs_hwv_deserialize(&h, raw);
  mfs_st st = mfs_hwv_validate(&h);
  printf("verify-hwv: %s -> %s\n", bin, mfs_ststr(st));
  if (st != MFS_OK)
    return 1;
  printf("  arch_class=%u mode_forced=%u erase_unit=%u media_size=%u "
         "t0_size=%u\n",
         (unsigned)h.arch_class, (unsigned)h.mode_forced,
         (unsigned)h.erase_unit, (unsigned)h.media_size, (unsigned)h.t0_size);
  if (pin) {
    FILE *p = fopen(pin, "wb");
    if (!p) {
      fprintf(stderr, "verify-hwv: no se puede crear '%s'\n", pin);
      return 1;
    }
    /* "Pin" de producción: HWV + su CRC-32C como sello independiente. */
    uint8_t seal[68];
    memcpy(seal, raw, 64u);
    mfs_st32(seal + 64, mfs_crc32c(raw, 64u, 0u));
    fwrite(seal, 1u, sizeof(seal), p);
    fclose(p);
    printf("  pinneado en %s (64 B + CRC-32C)\n", pin);
  }
  return 0;
}

/* =====================================================================
 * bench — banco reducido sobre vFlash (§17.1)
 * ===================================================================== */
#define BENCH_SIZE (1u << 20)
#define BENCH_RECS 256u
#define BENCH_PAY 246u

static int cmd_bench(void) {
  printf("mfstool bench — MFS-Bench reducido sobre vFlash (§17)\n");
  vflash_t vf;
  if (!vf_init(&vf, BENCH_SIZE, VF_SECTOR)) {
    fprintf(stderr, "bench: no se pudo inicializar vFlash\n");
    return 1;
  }
  const mfs_l2_driver *d = vf_driver(&vf);
  uint8_t page[512];
  memset(page, 0xA5u, sizeof(page));

  uint32_t t0 = mfs_port_time_us();
  uint32_t ok = 0u;
  for (uint32_t r = 0; r < BENCH_RECS; r++) {
    uint32_t addr = 4096u + r * 512u; /* bloque 1 en adelante */
    if (addr + sizeof(page) > BENCH_SIZE)
      break;
    if (d->prog(d->ctx, addr, page, sizeof(page)) == MFS_OK)
      ok++;
  }
  uint32_t t1 = mfs_port_time_us();
  double secs = (double)(t1 - t0) / 1e6;
  double mbps =
      (secs > 0.0)
          ? ((double)ok * (double)sizeof(page) / (1024.0 * 1024.0)) / secs
          : 0.0;

  /* lectura de comprobación (W1 append + verificación) */
  uint8_t back[512];
  uint32_t rok = 0u;
  for (uint32_t r = 0; r < ok; r++) {
    if (d->read(d->ctx, 4096u + r * 512u, back, sizeof(back)) == MFS_OK &&
        memcmp(back, page, sizeof(page)) == 0)
      rok++;
  }
  printf("  W1 append   : %u/%u registros de %u B (%.2f MB/s escritura)\n", ok,
         BENCH_RECS, (unsigned)sizeof(page), mbps);
  printf("  relectura   : %u/%u idénticos\n", rok, ok);
  printf("  NOR viols   : %u   prog=%u erase=%u\n", vf.n_violations, vf.n_prog,
         vf.n_erase);
  vf_free(&vf);
  if (ok != BENCH_RECS || rok != ok || vf.n_violations != 0u) {
    printf("  FAIL: el banco reducido no cumple las invariantes\n");
    return 1;
  }
  printf("  PASS\n");
  return 0;
}

/* =====================================================================
 * profiles — perfiles por tecnología de memoria (MFS-CAP-001)
 * ===================================================================== */
static const char *cap_str(uint64_t b, char *buf, size_t n) {
  static const char *const u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024.0 && i < 5) {
    v /= 1024.0;
    i++;
  }
  snprintf(buf, n, "%.4g %s", v, u[i]);
  return buf;
}

/* Acepta bytes decimales con sufijo k/M/G/T (potencias de 1000, como el
 * etiquetado comercial de los medios) o Ki/Mi/Gi/Ti (potencias de 1024). */
static uint64_t parse_capacity(const char *s) {
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (end && end[0] != '\0') {
    bool bin = (end[1] == 'i' || end[1] == 'I');
    unsigned long long mul = 1000ull;
    switch (end[0]) {
    case 'k':
    case 'K':
      mul = 1000ull;
      break;
    case 'm':
    case 'M':
      mul = 1000ull * 1000ull;
      break;
    case 'g':
    case 'G':
      mul = 1000ull * 1000ull * 1000ull;
      break;
    case 't':
    case 'T':
      mul = 1000ull * 1000ull * 1000ull * 1000ull;
      break;
    default:
      mul = 1ull;
      break;
    }
    if (bin)
      mul = (mul / 1000ull) * 1024ull;
    v *= mul;
  }
  return (uint64_t)v;
}

static bool name_ieq(const char *a, const char *b) {
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
    char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
    if (ca != cb)
      return false;
  }
  return *a == '\0' && *b == '\0';
}

static int cmd_profiles(int argc, char **argv) {
  printf(
      "mfstool profiles — perfiles por tecnología de memoria (MFS-CAP-001)\n");
  if (argc > 3 && strcmp(argv[2], "--check") == 0) {
    const char *want = argv[3];
    uint64_t bytes = (argc > 4) ? parse_capacity(argv[4]) : 0ull;
    for (unsigned i = 0; i < (unsigned)MFS_MEDIA_COUNT; i++) {
      const mfs_media_profile_t *p = mf_media_profile((mfs_media_type_t)i);
      if (!name_ieq(p->name, want))
        continue;
      mfs_st st = mfs_profile_check((mfs_media_type_t)i, bytes);
      printf("  %s: %llu B (máx. %llu B) → %s\n", p->name,
             (unsigned long long)bytes, (unsigned long long)p->max_bytes,
             mfs_ststr(st));
      return (st == MFS_OK) ? 0 : 1;
    }
    printf("  medio desconocido: %s (use el nombre del perfil)\n", want);
    return 2;
  }

  printf("  %-9s %-7s %-10s %-9s %-9s %-8s %-6s %s\n", "medio", "motor", "máx.",
         "clúster", "borrado", "página", "chunk", "especificación");
  for (unsigned i = 1; i < (unsigned)MFS_MEDIA_COUNT; i++) {
    const mfs_media_profile_t *p = mf_media_profile((mfs_media_type_t)i);
    char mb[24], ab[24], eb[24];
    printf("  %-9s %-7s %-10s %-9s %-9s %-8u %-6u %s\n", p->name,
           mf_engine_name(p->engine),
           p->max_bytes ? cap_str(p->max_bytes, mb, sizeof(mb)) : "—",
           p->alloc_unit ? cap_str(p->alloc_unit, ab, sizeof(ab)) : "—",
           p->erase_unit ? cap_str(p->erase_unit, eb, sizeof(eb)) : "—",
           (unsigned)p->page_size, (unsigned)p->chunk, p->spec);
  }
  printf("  (--check <medio> <capacidad>  valida una capacidad contra su "
         "perfil)\n");
  return 0;
}

/* =====================================================================
 * main
 * ===================================================================== */
static void usage(void) {
  printf(
      "mfstool — herramienta de build MatrixFS «ATLAS» (§26)\n"
      "  plan                     genera matrixfs_resources.h + RSC firmado\n"
      "  check-map [mapfile]      MFS-RES-002: ausencia de ruta al heap\n"
      "  profiles [--check M B]   perfiles por medio y cota de capacidad\n"
      "  train-dict <corpus> [out] [bytes]   diccionario ODT (bigramas)\n"
      "  sign-profile <perfil> <clave-hex64> [out]   firma B3-keyed\n"
      "  verify-hwv <hwv.bin> [out.pin]      valida un HWV de 64 B\n"
      "  bench                    banco reducido sobre vFlash\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const char *cmd = argv[1];
  if (strcmp(cmd, "plan") == 0)
    return cmd_plan();
  if (strcmp(cmd, "check-map") == 0)
    return cmd_check_map(argc > 2 ? argv[2] : NULL);
  if (strcmp(cmd, "profiles") == 0)
    return cmd_profiles(argc, argv);
  if (strcmp(cmd, "train-dict") == 0) {
    if (argc < 3) {
      usage();
      return 2;
    }
    uint32_t budget = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 0u;
    return cmd_train_dict(argv[2], argc > 3 ? argv[3] : NULL, budget);
  }
  if (strcmp(cmd, "sign-profile") == 0) {
    if (argc < 4) {
      usage();
      return 2;
    }
    return cmd_sign_profile(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
  }
  if (strcmp(cmd, "verify-hwv") == 0) {
    if (argc < 3) {
      usage();
      return 2;
    }
    return cmd_verify_hwv(argv[2], argc > 3 ? argv[3] : NULL);
  }
  if (strcmp(cmd, "bench") == 0)
    return cmd_bench();
  usage();
  return 2;
}
