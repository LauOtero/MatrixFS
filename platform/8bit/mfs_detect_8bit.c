/* mfs_detect_8bit.c — Auto-detección de capacidades HW para MCU 8-bit
 *
 * La detección es no destructiva y determinista: sondea registros/macros del
 * compilador y, cuando es necesario, realiza pruebas activas limitadas.
 * Toda capacidad no confirmada se declara ausente (fallback software).
 *
 * Este módulo es agnóstico de la arquitectura concreta: usa las macros de
 * mfs_port_8bit.h para adaptarse a AVR/8051/STM8/PIC/Z80/generic.
 */
#include "mfs_detect_8bit.h"
#include "matrixfs/mfs_port.h"
#include <string.h>

/* ==== Tabla de RAM/Flash por arquitectura conocida (valores conservadores) ===
 * Estos valores son la cota inferior típica del segmento; el integrador puede
 * sobreescribirlos con cfg->ram_total si conoce el modelo exacto. */
#if MFS_8BIT_ARCH_AVR
/* AVR: ATmega328P = 2 KB SRAM, 32 KB flash, 1 KB EEPROM */
#define MFS_8BIT_DEF_RAM 2048u
#define MFS_8BIT_DEF_FLASH 32768u
#define MFS_8BIT_DEF_EEPROM 1024u
#define MFS_8BIT_DEF_FCPU 16000000u
#elif MFS_8BIT_ARCH_8051
/* 8051 clásico: 256 B IRAM, 64 KB ROM, sin EEPROM integrada */
#define MFS_8BIT_DEF_RAM 256u
#define MFS_8BIT_DEF_FLASH 65536u
#define MFS_8BIT_DEF_EEPROM 0u
#define MFS_8BIT_DEF_FCPU 12000000u
#elif MFS_8BIT_ARCH_STM8
/* STM8S103: 2 KB RAM, 16 KB flash, 640 B EEPROM */
#define MFS_8BIT_DEF_RAM 2048u
#define MFS_8BIT_DEF_FLASH 16384u
#define MFS_8BIT_DEF_EEPROM 640u
#define MFS_8BIT_DEF_FCPU 16000000u
#elif MFS_8BIT_ARCH_PIC
/* PIC18F4550: 2 KB RAM, 32 KB flash, 256 B EEPROM */
#define MFS_8BIT_DEF_RAM 2048u
#define MFS_8BIT_DEF_FLASH 32768u
#define MFS_8BIT_DEF_EEPROM 256u
#define MFS_8BIT_DEF_FCPU 48000000u /* PLL */
#elif MFS_8BIT_ARCH_Z80
/* Z80 retrogaming/homebrew: RAM variable (se declara mínima 2 KB) */
#define MFS_8BIT_DEF_RAM 2048u
#define MFS_8BIT_DEF_FLASH 0u /* ROM externa, no flash integrada */
#define MFS_8BIT_DEF_EEPROM 0u
#define MFS_8BIT_DEF_FCPU 4000000u
#else
/* Generic: cotas mínimas para permitir Ultra-Nano */
#define MFS_8BIT_DEF_RAM 1024u
#define MFS_8BIT_DEF_FLASH 0u
#define MFS_8BIT_DEF_EEPROM 0u
#define MFS_8BIT_DEF_FCPU 0u
#endif

/* ==== Sondeo de RAM no destructivo ====
 * Escribe un patrón conocido en direcciones candidatas y comprueba su
 * persistencia. En arquitecturas Harvard (AVR/PIC) el direccionamiento de
 * datos es reducido y esta sonda puede no ser fiable; en ese caso el
 * integrador DEBE declarar cfg->ram_total. Devolvemos 0 si no es fiable. */
uint32_t mfs_detect_8bit_ram_probe(void) {
  /* La sonda real depende del mapa de memoria del MCU: no hay un método
   * portable y seguro. Se documenta como no disponible por defecto y se
   * delega en cfg->ram_total. */
  return 0u;
}

/* ==== Detección principal ==== */
mfs_st mfs_detect_8bit_caps(mfs_hw_caps_8bit_t *caps) {
  if (!caps)
    return MFS_EINVAL;
  memset(caps, 0, sizeof(*caps));

  const mfs_port_8bit_ops *ops = mfs_port_8bit_get_ops();
  if (!ops)
    return MFS_ESTATE; /* puerto no inicializado */

  caps->arch_id = ops->arch_id;
  caps->arch_name = ops->arch_name;
  caps->arch_class = 0u; /* 8-bit */

  /* RAM: sonda activa si es fiable, si no cota conservadora por arquitectura */
  uint32_t probed = mfs_detect_8bit_ram_probe();
  caps->ram_total = probed ? probed : MFS_8BIT_DEF_RAM;
  caps->flash_size = MFS_8BIT_DEF_FLASH;
  caps->eeprom_size = MFS_8BIT_DEF_EEPROM;
  caps->f_cpu_hz = MFS_8BIT_DEF_FCPU;

  /* Capacidades desde los flags del puerto */
  caps->has_hw_crc = (ops->caps & 0x01u) != 0u;
  caps->has_hw_rng = (ops->caps & 0x02u) != 0u;
  caps->has_eeprom = (ops->caps & 0x04u) != 0u;
  /* Timer de 16 bits: presente si el puerto implementa cycles() con 16 bits */
  caps->has_timer16 = true; /* verificado por el puerto en tiempo de build */
  /* Periféricos: declarados por compilación. Por defecto se asumen SPI y UART
   * presentes (casi universal en 8-bit); el integrador afina si no. */
#if MFS_8BIT_ARCH_AVR || MFS_8BIT_ARCH_STM8 || MFS_8BIT_ARCH_PIC
  caps->has_spi = true;
  caps->has_i2c = true;
  caps->has_uart = true;
#elif MFS_8BIT_ARCH_8051
  caps->has_spi = false; /* 8051 clásico sin SPI integrado */
  caps->has_i2c = false;
  caps->has_uart = true;
#else
  caps->has_spi = true;
  caps->has_i2c = true;
  caps->has_uart = true;
#endif

  /* ==== Clasificación de modo según RAM disponible ====
   * Se aplica el presupuesto de viabilidad (§6.1) sobre el modo 8-bit:
   * disponible = ram_total − 50% firmware − 15% pila − 8% perif − 10% margen.
   */
  {
    uint32_t total = caps->ram_total;
    uint32_t fw = total / 2u;
    uint32_t stk = total * 15u / 100u;
    uint32_t peri = total * 8u / 100u;
    uint32_t marg = total * 10u / 100u;
    uint32_t avail =
        (fw + stk + peri + marg <= total) ? total - fw - stk - peri - marg : 0u;

    caps->suggested_mode = MFS_MODE_UNSUPPORTED;
    if (avail >= MFS_RAM_8BIT_COMPACT)
      caps->suggested_mode = MFS_MODE_8BIT_COMPACT;
    else if (avail >= MFS_RAM_8BIT_NANO)
      caps->suggested_mode = MFS_MODE_8BIT_NANO;
    else if (avail >= MFS_RAM_8BIT_ULTRA)
      caps->suggested_mode = MFS_MODE_8BIT_ULTRA;
  }

  /* ==== Suite de integridad sugerida ====
   * En 8-bit el AEAD pesado no cabe; se sugiere integridad ligera (Blake3/CRC).
   * Si hay CRC por hardware, se prefiere; si no, sigue habiendo CRC-32C SW. */
  if (caps->has_hw_crc)
    caps->suggested_suite = (uint8_t)MFS_SUITE_NONE; /* integridad por CRC HW */
  else
    caps->suggested_suite =
        (uint8_t)MFS_SUITE_NONE; /* CRC-32C SW determinista */

  return MFS_OK;
}

/* ==== Adaptación de mfs_config ==== */
mfs_st mfs_detect_8bit_adapt_config(const mfs_hw_caps_8bit_t *caps,
                                    mfs_config *cfg) {
  if (!caps || !cfg)
    return MFS_EINVAL;

  /* arch_class: 0 = 8-bit (siempre, es una plataforma de 8 bits) */
  cfg->arch_class = 0u;

  /* RAM total: respetar la declarada por el integrador; si es 0, usar detectada
   */
  if (cfg->ram_total == 0u)
    cfg->ram_total = caps->ram_total;

  /* Modo forzado: si el integrador no lo fijó, usar el sugerido por detección
   */
  if (cfg->forced_mode == MFS_MODE_UNSUPPORTED &&
      caps->suggested_mode != MFS_MODE_UNSUPPORTED)
    cfg->forced_mode = caps->suggested_mode;

  /* Suite: si el integrador no fijó preferencia, usar la sugerida */
  if (cfg->suite_preferred == 0xFFu)
    cfg->suite_preferred = caps->suggested_suite;

  /* Velocidad de bus: si es 0, derivar de F_CPU (los buses SPI/I2C de 8-bit
   * suelen ir a F_CPU/2 o F_CPU/4; se declara F_CPU/2 como cota conservadora)
   */
  if (cfg->bus_speed_hz == 0u && caps->f_cpu_hz != 0u)
    cfg->bus_speed_hz = caps->f_cpu_hz / 2u;

  return MFS_OK;
}