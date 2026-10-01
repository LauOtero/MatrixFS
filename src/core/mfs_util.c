/* mfs_util.c — límites por modo, nombres de estado y primitivas compartidas */
#include "mfs_internal.h"
#include <string.h>

/* Límites normativos §18.2 (índice = mfs_mode_t) */
const mfs_limits_t mfs_limits[MFS_MODE_COUNT] = {
    /* Ultra-Nano */ {2, 64, 0, 1, 2, 256u, 4096u, MFS_RAM_ULTRA_NANO, 64u,
                      128u},
    /* Nano       */ {4, 128, 0, 2, 4, 1024u, 16384u, MFS_RAM_NANO, 96u, 256u},
    /* Compact    */
    {8, 256, 2, 4, 8, 4096u, 65536u, MFS_RAM_COMPACT, 256u, 512u},
    /* Balanced   */
    {16, 256, 8, 4, 8, 1048576u, 524288u, MFS_RAM_BALANCED, 512u, 4096u},
    /* Extended   */
    {32, 256, 64, 4, 16, 4194304u, 524288u, MFS_RAM_EXTENDED, 1024u, 4096u},
    /* 8-bit Ultra (≤512B): minimal, solo CRC, 1 archivo, sin snapshots */
    {1, 32, 0, 1, 1, 64u, 1024u, MFS_RAM_8BIT_ULTRA, 32u, 64u},
    /* 8-bit Nano (≤1KB): + Blake3/CRC, 2 archivos, 1 snapshot */
    {2, 64, 1, 2, 2, 256u, 4096u, MFS_RAM_8BIT_NANO, 48u, 128u},
    /* 8-bit Compact (≤2KB): + Ascon opcional, 4 archivos, 2 snapshots */
    {4, 128, 2, 3, 4, 1024u, 16384u, MFS_RAM_8BIT_COMPACT, 64u, 256u}};

const char *mfs_ststr(mfs_st st) {
  switch (st) {
  case MFS_OK:
    return "MFS_OK";
  case MFS_EINVAL:
    return "MFS_EINVAL";
  case MFS_EIO:
    return "MFS_EIO";
  case MFS_ENOSPC:
    return "MFS_ENOSPC";
  case MFS_EBACKPRESSURE:
    return "MFS_EBACKPRESSURE";
  case MFS_ETIMEDOUT_BUDGET:
    return "MFS_ETIMEDOUT_BUDGET";
  case MFS_ETABLEFULL:
    return "MFS_ETABLEFULL";
  case MFS_EHW_UNSUPPORTED:
    return "MFS_EHW_UNSUPPORTED";
  case MFS_EHEALTH_BLOCKED:
    return "MFS_EHEALTH_BLOCKED";
  case MFS_ESECURITY_STATE:
    return "MFS_ESECURITY_STATE";
  case MFS_EENERGY:
    return "MFS_EENERGY";
  case MFS_EPUF:
    return "MFS_EPUF";
  case MFS_ECIPHER:
    return "MFS_ECIPHER";
  case MFS_ESNAPMAX:
    return "MFS_ESNAPMAX";
  case MFS_ENOTVIABLE:
    return "MFS_ENOTVIABLE";
  case MFS_EARCH:
    return "MFS_EARCH";
  case MFS_EBUSY:
    return "MFS_EBUSY";
  case MFS_ECORRUPT:
    return "MFS_ECORRUPT";
  case MFS_ENOTMOUNTED:
    return "MFS_ENOTMOUNTED";
  case MFS_ENOENT:
    return "MFS_ENOENT";
  case MFS_EEXISTS:
    return "MFS_EEXISTS";
  case MFS_EAGAIN:
    return "MFS_EAGAIN";
  case MFS_ENOTSUP:
    return "MFS_ENOTSUP";
  case MFS_ESTATE:
    return "MFS_ESTATE";
  case MFS_EBADMSG:
    return "MFS_EBADMSG";
  case MFS_EOVERFLOW:
    return "MFS_EOVERFLOW";
  case MFS_EDEADLK:
    return "MFS_EDEADLK";
  case MFS_EROFS:
    return "MFS_EROFS";
  case MFS_EACCES:
    return "MFS_EACCES";
  default:
    return "MFS_E?";
  }
}

/* CRC-32C (Castagnoli, polinomio 0x1EDC6F41 reflejado 0x82F63B78).
 * KAT (§27.1): CRC32C("123456789") == 0xE3069283
 *
 * Dos implementaciones con resultado idéntico bit a bit:
 *   - Host / 16-32 bit: tabla completa de 256 entradas (1 KiB en .bss).
 *   - 8-bit: tabla de nibble de 16 entradas (64 B en .rodata), que preserva
 *     1 KiB de RAM a costa de ~2x CPU. Mismo polinomio y mismo valor final. */
#if MFS_IS_8BIT_TARGET
/* Tabla de nibble: 16 entradas, índice = nibble (4 bits). En .rodata (flash).
 */
static const uint32_t crc32c_nib[16] = {
    0x00000000u, 0x105EC76Fu, 0x20BD8EDEu, 0x30E349B1u,
    0x417B1DBCu, 0x5125DAD3u, 0x61C69362u, 0x7198540Du,
    0x82F63B78u, 0x92A8FC17u, 0xA24BB5A6u, 0xB21572C9u,
    0xC38D26C4u, 0xD3D3E1ABu, 0xE330A81Au, 0xF36E6F75u};

uint32_t mfs_crc32c(const uint8_t *buf, uint32_t len, uint32_t seed) {
  uint32_t c = ~seed;
  for (uint32_t i = 0; i < len; i++) {
    c ^= buf[i];
    c = (c >> 4) ^ crc32c_nib[c & 0x0Fu];
    c = (c >> 4) ^ crc32c_nib[c & 0x0Fu];
  }
  return ~c;
}
#else  /* !MFS_IS_8BIT_TARGET: tabla completa en RAM */
static uint32_t crc32c_table[256];
static bool crc32c_ready;

static void crc32c_init(void) {
  for (uint32_t i = 0; i < 256u; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    crc32c_table[i] = c;
  }
  crc32c_ready = true;
}

uint32_t mfs_crc32c(const uint8_t *buf, uint32_t len, uint32_t seed) {
  if (!crc32c_ready)
    crc32c_init();
  uint32_t c = ~seed;
  for (uint32_t i = 0; i < len; i++) {
    c = crc32c_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}
#endif /* MFS_IS_8BIT_TARGET */

/* HWV serialización on-flash (§5.2) — accesores LE exclusivos (§20.4) */
void mfs_hwv_serialize(const mfs_hwv_t *h, uint8_t out[64]) {
  memset(out, 0, 64u);
  out[0] = 'M';
  out[1] = 'H';
  out[2] = 'W';
  out[3] = 'V';
  mfs_st16(out + 4, h->hw_version);
  out[6] = h->arch_class;
  out[7] = h->mode_forced;
  mfs_st32(out + 8, h->ram_total);
  mfs_st32(out + 12, h->bus_speed_hz);
  mfs_st32(out + 16, h->t_prog_max_us);
  mfs_st32(out + 20, h->t_erase_max_us);
  mfs_st32(out + 24, h->t_read_max_us);
  mfs_st32(out + 28, h->t_suspend_max_us);
  mfs_st32(out + 32, h->program_granularity);
  mfs_st32(out + 36, h->erase_unit);
  mfs_st16(out + 40, h->oob_bytes);
  out[42] = h->flags0;
  out[43] = h->flags1;
  out[44] = h->flags2;
  out[45] = h->flags3;
  mfs_st32(out + 46, h->t0_size);
  mfs_st32(out + 50, h->t0_write_ns);
  mfs_st32(out + 54, h->profile_id);
  /* bytes 58..59 reservados; 60..63 = CRC-32C del cuerpo (0..59).
   * base_reserved_off NO se persiste: se deriva de la geometría al montar. */
  uint32_t body_crc = mfs_crc32c(out, 60u, 0u);
  mfs_st32(out + 60, body_crc);
}

void mfs_hwv_deserialize(mfs_hwv_t *h, const uint8_t in[64]) {
  memset(h, 0, sizeof(*h));
  h->magic[0] = in[0];
  h->magic[1] = in[1];
  h->magic[2] = in[2];
  h->magic[3] = in[3];
  h->hw_version = mfs_ld16(in + 4);
  h->arch_class = in[6];
  h->mode_forced = in[7];
  h->ram_total = mfs_ld32(in + 8);
  h->bus_speed_hz = mfs_ld32(in + 12);
  h->t_prog_max_us = mfs_ld32(in + 16);
  h->t_erase_max_us = mfs_ld32(in + 20);
  h->t_read_max_us = mfs_ld32(in + 24);
  h->t_suspend_max_us = mfs_ld32(in + 28);
  h->program_granularity = mfs_ld32(in + 32);
  h->erase_unit = mfs_ld32(in + 36);
  h->base_reserved_off = 0u; /* derivado de la geometría en runtime */
  h->oob_bytes = mfs_ld16(in + 40);
  h->flags0 = in[42];
  h->flags1 = in[43];
  h->flags2 = in[44];
  h->flags3 = in[45];
  h->t0_size = mfs_ld32(in + 46);
  h->t0_write_ns = mfs_ld32(in + 50);
  h->profile_id = mfs_ld32(in + 54);
  h->crc = mfs_ld32(in + 60);
}

mfs_st mfs_hwv_validate(const mfs_hwv_t *h) {
  if (h->magic[0] != 'M' || h->magic[1] != 'H' || h->magic[2] != 'W' ||
      h->magic[3] != 'V') {
    return MFS_ECORRUPT;
  }
  /* MFS-ARCH-010 rev.2: arch_class 0=8-bit (permitido con
   * MFS_ALLOW_8BIT_TARGET), 1=16-bit, 2=32-bit; >2 ⇒ rechazo */
  if (h->arch_class > 2u)
    return MFS_EARCH;
  /* Verificación de integridad: recomputar CRC sobre la representación
   * canónica y comparar con el campo persistido. */
  uint8_t ser[64];
  mfs_hwv_t copy = *h;
  copy.magic[0] = 'M';
  copy.magic[1] = 'H';
  copy.magic[2] = 'W';
  copy.magic[3] = 'V';
  mfs_hwv_serialize(&copy, ser);
  uint32_t want = mfs_ld32(ser + 60);
  if (want != h->crc)
    return MFS_ECORRUPT;
  return MFS_OK;
}

/* TFC termométrico (§8.4): bits programados secuencialmente, 1 bit / 16 ciclos.
 * Codificación: los bits válidos arrancan en 1 (erased NOR) y se aclaran a 0.
 * El contador es el número de bits aclarados. Carry anticipado: cuando quedan
 * 2 bits libres emite aviso (caller reserva nueva página). */
uint32_t mfs_tfc_count(const uint8_t *bits, uint32_t nbytes) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < nbytes; i++) {
    uint8_t b = bits[i];
    for (int k = 0; k < 8; k++) {
      if ((b & (uint8_t)(1u << k)) == 0u)
        n++;
    }
  }
  return n * 16u; /* 16 ciclos P/E por bit (§25 TFC intervalo) */
}

bool mfs_tfc_will_carry(const uint8_t *bits, uint32_t nbytes) {
  uint32_t free_bits = 0;
  for (uint32_t i = 0; i < nbytes; i++) {
    uint8_t b = bits[i];
    for (int k = 0; k < 8; k++) {
      if ((b & (uint8_t)(1u << k)) != 0u)
        free_bits++;
    }
  }
  return free_bits <= 2u; /* carry anticipado con 2 bits de aviso (§8.4) */
}

void mfs_tfc_increment(uint8_t *bits, uint32_t nbytes) {
  for (uint32_t i = 0; i < nbytes; i++) {
    for (int k = 0; k < 8; k++) {
      uint8_t mask = (uint8_t)(1u << k);
      if ((bits[i] & mask) != 0u) {
        bits[i] &= (uint8_t)~mask;
        return;
      }
    }
  }
}

#ifdef MFS_DEBUG
void mfs_assert_fail(int line) {
  /* En target: bucle seguro + flag; en host: abort para CI */
  extern void abort(void);
  (void)line;
  abort();
}
#endif
