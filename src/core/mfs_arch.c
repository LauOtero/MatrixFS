/* mfs_arch.c — modelo de arquitectura e integración de aceleración HW.
 *
 * Forma parte del núcleo (MFS-ARCH-010 rev. 3): clasifica el objetivo en
 * 8/16/32/64 bits, detecta las capacidades de aceleración por hardware y
 * autoconfigura la instancia (clase, RAM, suite y flags). Es código puro C sin
 * dependencias de plataforma: la detección se apoya en macros del compilador y
 * en *builtins* cuando están disponibles, y degrada siempre a la ruta software
 * (MFS-HW-001).
 *
 * El motor CRC-32C acelerado vive aquí porque es común a todos los objetivos
 * que exponen la instrucción (x86 SSE4.2 y ARMv8 CRC32 usan el mismo polinomio
 * de Castagnoli que el núcleo): resultado idéntico bit a bit al de la tabla.
 */
#include "mfs_internal.h"

#include <string.h>

/* ==== CRC-32C por instrucción ==========================================
 * x86 SSE4.2 (instrucción CRC32) y ARMv8 CRC32 comparten el polinomio de
 * Castagnoli con la tabla del núcleo ⇒ resultado idéntico bit a bit.
 * En GCC/Clang se usa atributo de función `target` para no exigir flags
 * globales; la disponibilidad se decide en runtime. */
#if defined(__x86_64__) || defined(__i386__)
#define MFS_HWCRC_X86 1
#else
#define MFS_HWCRC_X86 0
#endif

#if defined(__ARM_FEATURE_CRC32)
#define MFS_HWCRC_ARM 1
#include <arm_acle.h>
#else
#define MFS_HWCRC_ARM 0
#endif

#if MFS_HWCRC_X86
#if defined(_MSC_VER)
#include <intrin.h>
#define MFS_CRC_ATTR
#else
#include <nmmintrin.h>
#define MFS_CRC_ATTR __attribute__((target("sse4.2")))
#endif

MFS_CRC_ATTR
static uint32_t crc32c_inst(const uint8_t *buf, uint32_t len, uint32_t c) {
  uint32_t i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  for (; i + 8u <= len; i += 8u) {
    uint64_t v;
    memcpy(&v, buf + i, 8u);
    c = (uint32_t)_mm_crc32_u64((unsigned long long)c, v);
  }
#else
  for (; i + 4u <= len; i += 4u) {
    uint32_t v;
    memcpy(&v, buf + i, 4u);
    c = _mm_crc32_u32(c, v);
  }
#endif
  for (; i < len; i++)
    c = _mm_crc32_u8(c, buf[i]);
  return c;
}
#elif MFS_HWCRC_ARM
static uint32_t crc32c_inst(const uint8_t *buf, uint32_t len, uint32_t c) {
  uint32_t i = 0;
  for (; i + 8u <= len; i += 8u) {
    uint64_t v;
    memcpy(&v, buf + i, 8u);
    c = __crc32cd(c, (uint32_t)v);
    c = __crc32cd(c, (uint32_t)(v >> 32));
  }
  for (; i < len; i++)
    c = __crc32cb(c, buf[i]);
  return c;
}
#endif /* MFS_HWCRC_X86 || MFS_HWCRC_ARM */

/* Detección de CPU para SSE4.2 (bit ECX[20] de CPUID.1) */
static bool crc32c_x86_supported(void) {
#if MFS_HWCRC_X86 && (defined(__GNUC__) || defined(__clang__))
  static int cached = -1;
  if (cached < 0) {
    __builtin_cpu_init();
    cached = __builtin_cpu_supports("sse4.2") ? 1 : 0;
  }
  return cached != 0;
#elif MFS_HWCRC_X86 && defined(_MSC_VER)
  static int cached = -1;
  if (cached < 0) {
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    cached = ((unsigned)regs[2] & (1u << 20)) ? 1 : 0; /* ECX[20] = SSE4.2 */
  }
  return cached != 0;
#else
  return false;
#endif
}

bool mfs_arch_crc32c_hw_available(void) {
#if MFS_HWCRC_X86
  return crc32c_x86_supported();
#elif MFS_HWCRC_ARM
  return true; /* __ARM_FEATURE_CRC32 ⇒ extensión compilada y disponible */
#else
  return false;
#endif
}

uint32_t mfs_crc32c_hw(const uint8_t *buf, uint32_t len, uint32_t seed) {
#if MFS_HWCRC_X86 || MFS_HWCRC_ARM
  return ~crc32c_inst(buf, len, ~seed);
#else
  /* Sin ruta acelerada: no se llama nunca (available() ⇒ false). Se devuelve
   * el valor semilla sin transformar para no introducir una ruta silenciosa. */
  (void)buf;
  (void)len;
  return seed;
#endif
}

/* ==== Detección de capacidades ==== */
static uint16_t arch_detect_hwaccel(void) {
  uint16_t f = MFS_HWACCEL_NONE;

  /* CRC-32C por instrucción (misma polinómica que el núcleo) */
  if (mfs_arch_crc32c_hw_available())
    f |= MFS_HWACCEL_CRC32C;

  /* Aceleradores criptográficos por macros de compilador */
#if defined(__AES__) || defined(__ARM_FEATURE_CRYPTO) ||                       \
    defined(__ARM_FEATURE_AES)
  f |= MFS_HWACCEL_AES;
#endif
#if defined(__SHA__) || defined(__ARM_FEATURE_CRYPTO) ||                       \
    defined(__ARM_FEATURE_SHA2)
  f |= MFS_HWACCEL_SHA256;
#endif
#if defined(__PCLMUL__)
  f |= MFS_HWACCEL_CLMUL;
#endif
#if defined(__SSE2__) || defined(__AVX2__) || defined(__ARM_NEON) ||           \
    defined(__ARM_NEON__) || defined(__riscv_v)
  f |= MFS_HWACCEL_SIMD;
#endif
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4) || defined(__cplusplus) ||     \
    defined(_WIN32)
  /* En MCU de 8 bits no hay CAS: se excluye explícitamente. */
  if (!MFS_IS_8BIT_TARGET)
    f |= MFS_HWACCEL_ATOMICS;
#endif

  /* Refinado en runtime en x86 (el mismo binario puede correr en CPU sin la
   * extensión): se limpian los bits que la CPU no soporte. */
#if MFS_HWCRC_X86 && (defined(__GNUC__) || defined(__clang__)) &&              \
    !defined(__cplusplus)
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("sse4.2"))
    f &= (uint16_t)~MFS_HWACCEL_CRC32C;
  if (!__builtin_cpu_supports("aes"))
    f &= (uint16_t)~MFS_HWACCEL_AES;
  if (!__builtin_cpu_supports("sha"))
    f &= (uint16_t)~MFS_HWACCEL_SHA256;
  if (!__builtin_cpu_supports("pclmul"))
    f &= (uint16_t)~MFS_HWACCEL_CLMUL;
  if (!__builtin_cpu_supports("avx2") && !__builtin_cpu_supports("sse2"))
    f &= (uint16_t)~MFS_HWACCEL_SIMD;
  if (__builtin_cpu_supports("rdrnd"))
    f |= MFS_HWACCEL_RNG;
#endif

  return f;
}

/* Valores por defecto conservadores por arquitectura (sólo para MCU de 8 bits,
 * donde el integrador suele no declarar geometría). En 16/32/64 bits el
 * presupuesto de RAM lo declara el integrador y se deja a 0 (desconocido). */
static void arch_defaults_by_class(uint8_t cls, mfs_arch_info_t *o) {
#if MFS_IS_8BIT_TARGET
  (void)cls;
  o->ram_total = 2048u;
  o->flash_size = 32768u;
  o->eeprom_size = 1024u;
  o->f_cpu_hz = 16000000u;
#else
  (void)cls;
  o->ram_total = 0u;
  o->flash_size = 0u;
  o->eeprom_size = 0u;
  o->f_cpu_hz = 0u;
#endif
}

static const char *arch_name_by_class(uint8_t cls) {
  switch (cls) {
  case MFS_ARCH_8BIT:
    return "8-bit MCU";
  case MFS_ARCH_16BIT:
    return "16-bit MCU";
  case MFS_ARCH_32BIT:
    return "32-bit MCU";
  case MFS_ARCH_64BIT:
    return "64-bit";
  default:
    return "unknown";
  }
}

mfs_st mfs_arch_detect(mfs_arch_info_t *out) {
  if (!out)
    return MFS_EINVAL;
  memset(out, 0, sizeof(*out));

  out->arch_class = (uint8_t)MFS_ARCH_CLASS_DEFAULT;
  switch (out->arch_class) {
  case MFS_ARCH_8BIT:
    out->bits = 8u;
    break;
  case MFS_ARCH_16BIT:
    out->bits = 16u;
    break;
  case MFS_ARCH_32BIT:
    out->bits = 32u;
    break;
  default:
    out->bits = 64u;
    break;
  }

  arch_defaults_by_class(out->arch_class, out);
  out->hwaccel = arch_detect_hwaccel();
  out->name = arch_name_by_class(out->arch_class);
  return MFS_OK;
}

/* ==== Autoadaptación de la configuración ==== */
mfs_st mfs_arch_adapt_config(const mfs_arch_info_t *info, mfs_config *cfg) {
  if (!info || !cfg)
    return MFS_EINVAL;

  /* Clase: autodetectar sólo si no se forzó explícitamente */
  if (cfg->arch_class == (uint8_t)MFS_ARCH_AUTO)
    cfg->arch_class = info->arch_class;

  /* RAM: respetar la declarada; si es 0 (y el MCU es de 8 bits) usar la cota */
  if (cfg->ram_total == 0u && info->arch_class == MFS_ARCH_8BIT)
    cfg->ram_total = info->ram_total;

  /* Velocidad de bus: si no se declara, derivar de F_CPU en 8-bit (SPI/I2C a
   * F_CPU/2 como cota conservadora). En 32/64 bits la mide el HAL (§14.2). */
  if (cfg->bus_speed_hz == 0u && info->f_cpu_hz != 0u &&
      info->arch_class == MFS_ARCH_8BIT)
    cfg->bus_speed_hz = info->f_cpu_hz / 2u;

  /* Aceleradores detectados ⇒ habilitar los flags del HWV que los declaran.
   * Se dejan intactos: el HWV sólo declara capacidades que el núcleo puede
   * EJECUTAR (MFS-HW-001); hoy la única explotada es CRC-32C, que no tiene bit
   * de HWV. Los aceleradores que sólo el BSP conoce (BLAKE3/Ascon/DMA) se
   * anuncian vía mfs_hal_ops.assets() (fase 8). Las capacidades detectadas
   * quedan accesibles en mfs_arch_info_t para diagnóstico y planificación. */
  return MFS_OK;
}

/* ==== Estado cacheado del último análisis (diagnóstico / tests) ==== */
static mfs_arch_info_t g_arch_last;
static bool g_arch_valid = false;

const mfs_arch_info_t *mf_arch_last(void) {
  if (!g_arch_valid) {
    (void)mfs_arch_detect(&g_arch_last);
  }
  return &g_arch_last;
}
