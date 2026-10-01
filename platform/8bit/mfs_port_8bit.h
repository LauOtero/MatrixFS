/* mfs_port_8bit.h — MatrixFS Ultra 8-bit Port Abstraction Layer
 *
 * Capa de abstracción unificada para arquitecturas de 8 bits (AVR, 8051, STM8,
 * PIC16/18). Proporciona primitivas de sección crítica, tiempo, ciclos y WFI
 * deterministas.
 *
 * Cada arquitectura implementa mfs_port_8bit_ops con sus primitivas nativas.
 * La detección automática selecciona el ops correcto en mfs_port_8bit_init().
 */

#ifndef MFS_PORT_8BIT_H
#define MFS_PORT_8BIT_H

#include "matrixfs/mfs_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Detección de arquitectura en tiempo de compilación ==== */
#if defined(__AVR__) || defined(__AVR_ARCH__)
#define MFS_8BIT_ARCH_AVR 1
#else
#define MFS_8BIT_ARCH_AVR 0
#endif

#if defined(__CSMC__) || defined(SDCC_mcs51) || defined(__SDCC_mcs51) ||       \
    defined(__C51__) || defined(__ICC8051__) || defined(_SDCC_)
#define MFS_8BIT_ARCH_8051 1
#else
#define MFS_8BIT_ARCH_8051 0
#endif

#if defined(__STM8__) || defined(__STM8)
#define MFS_8BIT_ARCH_STM8 1
#else
#define MFS_8BIT_ARCH_STM8 0
#endif

#if defined(_PIC14) || defined(_PIC18) || defined(__PIC14E__) ||               \
    defined(__PIC18F__)
#define MFS_8BIT_ARCH_PIC 1
#else
#define MFS_8BIT_ARCH_PIC 0
#endif

#if defined(__Z80__) || defined(__Z80)
#define MFS_8BIT_ARCH_Z80 1
#else
#define MFS_8BIT_ARCH_Z80 0
#endif

/* Arquitectura genérica (fallback C puro) */
#define MFS_8BIT_ARCH_GENERIC 1

/* ==== Estructura de operaciones por arquitectura ==== */
typedef struct mfs_port_8bit_ops {
  /* Sección crítica: latencia ≤ 1 µs, nesting-safe */
  void (*crit_enter)(void);
  void (*crit_exit)(void);

  /* Contador de ciclos libre-corriente (para timeouts y medición bus) */
  uint32_t (*cycles)(void);

  /* Tiempo monotónico en microsegundos (para EDP, watchdogs) */
  uint32_t (*time_us)(void);

  /* Wait-for-interrupt / idle de bajo consumo */
  void (*wfi)(void);

  /* Arquitectura detectada (MFS_8BIT_ARCH_*) */
  uint8_t arch_id;

  /* Nombre legible para diagnóstico */
  const char *arch_name;

  /* Flags de capacidad: bit 0 = tiene HW CRC, bit 1 = tiene HW RNG, bit 2 =
   * tiene EEPROM interna */
  uint8_t caps;
} mfs_port_8bit_ops;

/* ==== API pública ==== */

/* Inicializa el puerto 8-bit: detecta arquitectura y registra ops en mfs_port.h
 */
mfs_st mfs_port_8bit_init(const mfs_config *cfg);

/* Obtiene los ops de la arquitectura actual (para diagnóstico) */
const mfs_port_8bit_ops *mfs_port_8bit_get_ops(void);

/* Fuerza una arquitectura específica (para testing/simulación) */
mfs_st mfs_port_8bit_set_arch(uint8_t arch_id);

/* ==== Helpers atómicos para 8-bit (implementación por defecto) ==== */

/* Enter critical: deshabilita interrupciones globalmente */
static inline void mfs_port_8bit_crit_enter_default(void) {
#if MFS_8BIT_ARCH_AVR
  __builtin_avr_cli();
#elif MFS_8BIT_ARCH_8051
  EA = 0;
#elif MFS_8BIT_ARCH_STM8
  __asm__("sim");
#elif MFS_8BIT_ARCH_PIC
  INTCONbits.GIE = 0;
#else
  /* Generic: asumimos single-threaded o RTOS externo */
  extern void mfs_generic_crit_enter(void);
  mfs_generic_crit_enter();
#endif
}

/* Exit critical: habilita interrupciones */
static inline void mfs_port_8bit_crit_exit_default(void) {
#if MFS_8BIT_ARCH_AVR
  __builtin_avr_sei();
#elif MFS_8BIT_ARCH_8051
  EA = 1;
#elif MFS_8BIT_ARCH_STM8
  __asm__("rim");
#elif MFS_8BIT_ARCH_PIC
  INTCONbits.GIE = 1;
#else
  extern void mfs_generic_crit_exit(void);
  mfs_generic_crit_exit();
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* MFS_PORT_8BIT_H */