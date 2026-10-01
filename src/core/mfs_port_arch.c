/* mfs_port_arch.c — Implementación del puerto del núcleo (8/16/32/64 bits)
 *
 * Auto-detecta la familia de arquitectura (AVR, 8051, STM8, PIC16/18, Z80 o
 * genérica) y provee primitivas deterministas de sección crítica, ciclos,
 * tiempo y WFI para el contrato de puerto de MatrixFS (§20.2).
 *
 * Reglas de codificación (MFS-ARCH-010 rev.2):
 *   - C puro (sin lambdas ni extensiones C++)
 *   - Sin malloc/free
 *   - Buffers estáticos
 *   - Determinismo: sin dependencia de estado no inicializado
 */

#include "mfs_port_arch.h"
#include "matrixfs/mfs_port.h"
#include <string.h>

/* ==== Estado global del puerto 8-bit ==== */
static const mfs_port_arch_ops *g_port_arch_ops = NULL;
static uint8_t g_forced_arch = 0xFF;

/* ==== Declaraciones forward de ops por arquitectura ==== */
#if MFS_8BIT_ARCH_AVR
extern const mfs_port_arch_ops mfs_port_avr_ops;
#endif
#if MFS_8BIT_ARCH_8051
extern const mfs_port_arch_ops mfs_port_8051_ops;
#endif
#if MFS_8BIT_ARCH_STM8
extern const mfs_port_arch_ops mfs_port_stm8_ops;
#endif
#if MFS_8BIT_ARCH_PIC
extern const mfs_port_arch_ops mfs_port_pic_ops;
#endif
#if MFS_8BIT_ARCH_Z80
extern const mfs_port_arch_ops mfs_port_z80_ops;
#endif
extern const mfs_port_arch_ops mfs_port_generic_ops;

/* ==== Tabla de arquitecturas soportadas ==== */
static const mfs_port_arch_ops *const arch_table[] = {
#if MFS_8BIT_ARCH_AVR
    &mfs_port_avr_ops,
#endif
#if MFS_8BIT_ARCH_8051
    &mfs_port_8051_ops,
#endif
#if MFS_8BIT_ARCH_STM8
    &mfs_port_stm8_ops,
#endif
#if MFS_8BIT_ARCH_PIC
    &mfs_port_pic_ops,
#endif
#if MFS_8BIT_ARCH_Z80
    &mfs_port_z80_ops,
#endif
    &mfs_port_generic_ops, /* siempre último = fallback */
    NULL};

/* ==== Implementación genérica (C puro, single-threaded / RTOS externo) ==== */
static uint32_t g_generic_cycles = 0;

static void generic_crit_enter(void) {
  /* Sin RTOS: no-op. Con RTOS: portENTER_CRITICAL(). */
}

static void generic_crit_exit(void) {
  /* Sin RTOS: no-op. Con RTOS: portEXIT_CRITICAL(). */
}

static uint32_t generic_cycles(void) {
  return g_generic_cycles++; /* Incremento simple para simulacion/testing */
}

static uint32_t generic_time_us(void) {
  /* Requiere timer HW real en target; en host se aproxima a cycles() */
  return g_generic_cycles;
}

static void generic_wfi(void) {
  /* NOP: en target real, sustituir por __WFI() del compilador */
  __asm__ volatile("nop");
}

const mfs_port_arch_ops mfs_port_generic_ops = {
    generic_crit_enter, generic_crit_exit,
    generic_cycles,     generic_time_us,
    generic_wfi,        0xFEu,
    "Generic C",        0u};

/* ==== AVR (ATmega/ATtiny, GCC/avr-gcc) ==== */
#if MFS_8BIT_ARCH_AVR
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <util/atomic.h>

static void avr_crit_enter(void) { cli(); }
static void avr_crit_exit(void) { sei(); }

static uint32_t avr_cycles(void) {
#if defined(TCNT1) || defined(TCNT1L)
  /* Timer1 16-bit: lectura atómica low/high */
  uint16_t low, high;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    low = TCNT1L;
    high = TCNT1H;
  }
  return ((uint32_t)high << 8) | low;
#else
  /* Timer0 8-bit con contador de overflows externo */
  extern volatile uint32_t mfs_avr_timer0_overflows;
  uint8_t tcnt0;
  uint32_t ovf;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    tcnt0 = TCNT0;
    ovf = mfs_avr_timer0_overflows;
  }
  return (ovf << 8) | tcnt0;
#endif
}

static uint32_t avr_time_us(void) {
  /* Si Timer1 corre a 1 MHz (prescaler ajustado), 1 tick = 1 us */
  return avr_cycles();
}

static void avr_wfi(void) {
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_mode();
}

const mfs_port_arch_ops mfs_port_avr_ops = {
    avr_crit_enter,
    avr_crit_exit,
    avr_cycles,
    avr_time_us,
    avr_wfi,
    1u,
    "AVR (ATmega/ATtiny)",
    0x03u /* CRC HW (XMEGA)/RNG no garantizado, flash interna */};
#endif /* MFS_8BIT_ARCH_AVR */

/* ==== 8051 (Keil C51 / SDCC / IAR) ==== */
#if MFS_8BIT_ARCH_8051
#if defined(SDCC_mcs51) || defined(__SDCC_mcs51)
__sfr __at(0x88) MFS_TCON;
__sfr __at(0x89) MFS_TMOD;
__sfr __at(0x8A) MFS_TL0;
__sfr __at(0x8C) MFS_TH0;
__sfr __at(0xA8) MFS_IE;
#elif defined(__C51__) || defined(__ICC8051__)
#include <reg51.h>
#define MFS_TCON TCON
#define MFS_TMOD TMOD
#define MFS_TL0 TL0
#define MFS_TH0 TH0
#define MFS_IE IE
#endif

static volatile uint32_t mfs_8051_timer_overflows = 0;

static void mfs_8051_crit_enter(void) { MFS_IE &= (uint8_t)~0x80u; }
static void mfs_8051_crit_exit(void) { MFS_IE |= 0x80u; }

static uint32_t mfs_8051_cycles(void) {
  uint8_t tl, th;
  uint32_t ovf;
  MFS_IE &= (uint8_t)~0x80u;
  tl = MFS_TL0;
  th = MFS_TH0;
  ovf = mfs_8051_timer_overflows;
  MFS_IE |= 0x80u;
  return (ovf << 16) | ((uint32_t)th << 8) | tl;
}

static uint32_t mfs_8051_time_us(void) {
  /* Timer0 a 1 MHz (12 clk por tick @ 12 MHz) => 1 tick = 1 us */
  return mfs_8051_cycles();
}

static void mfs_8051_wfi(void) { PCON |= 0x01u; /* IDL: idle mode */ }

const mfs_port_arch_ops mfs_port_8051_ops = {
    mfs_8051_crit_enter, mfs_8051_crit_exit,
    mfs_8051_cycles,     mfs_8051_time_us,
    mfs_8051_wfi,        2u,
    "8051 (MCS-51)",     0u};
#endif /* MFS_8BIT_ARCH_8051 */

/* ==== STM8 (Cosmic / IAR / SDCC) ==== */
#if MFS_8BIT_ARCH_STM8
#include "intrinsics.h"
#include "stm8s.h"

static volatile uint32_t mfs_stm8_tim4_overflows = 0;

static void stm8_crit_enter(void) { __disable_interrupt(); }
static void stm8_crit_exit(void) { __enable_interrupt(); }

static uint32_t stm8_cycles(void) {
  uint16_t cnt;
  uint32_t ovf;
  __disable_interrupt();
  cnt = TIM4->CNTR;
  ovf = mfs_stm8_tim4_overflows;
  __enable_interrupt();
  return (ovf << 16) | cnt;
}

static uint32_t stm8_time_us(void) {
  /* TIM4 a 1 MHz => 1 tick = 1 us */
  return stm8_cycles();
}

static void stm8_wfi(void) { __asm__("wfi"); }

const mfs_port_arch_ops mfs_port_stm8_ops = {
    stm8_crit_enter, stm8_crit_exit, stm8_cycles, stm8_time_us, stm8_wfi, 3u,
    "STM8",          0x04u /* EEPROM interna */};
#endif /* MFS_8BIT_ARCH_STM8 */

/* ==== PIC16/18 (XC8 / MPLAB) ==== */
#if MFS_8BIT_ARCH_PIC
#include <xc.h>

static volatile uint32_t mfs_pic_tmr1_overflows = 0;

static void pic_crit_enter(void) { INTCONbits.GIE = 0; }
static void pic_crit_exit(void) { INTCONbits.GIE = 1; }

static uint32_t pic_cycles(void) {
  uint16_t tmr1;
  uint32_t ovf;
  INTCONbits.GIE = 0;
  tmr1 = ((uint16_t)TMR1H << 8) | TMR1L;
  ovf = mfs_pic_tmr1_overflows;
  INTCONbits.GIE = 1;
  return (ovf << 16) | tmr1;
}

static uint32_t pic_time_us(void) { return pic_cycles(); }

static void pic_wfi(void) {
  SLEEP();
  NOP();
}

const mfs_port_arch_ops mfs_port_pic_ops = {
    pic_crit_enter, pic_crit_exit, pic_cycles, pic_time_us, pic_wfi, 4u,
    "PIC16/18",     0x04u /* EEPROM interna (PFM/DFM) */};
#endif /* MFS_8BIT_ARCH_PIC */

/* ==== Z80 (SDCC / z88dk) ==== */
#if MFS_8BIT_ARCH_Z80
static volatile uint32_t mfs_z80_cycles = 0;

static void z80_crit_enter(void) { __asm__("di"); }
static void z80_crit_exit(void) { __asm__("ei"); }
static uint32_t z80_cycles(void) { return mfs_z80_cycles++; }
static uint32_t z80_time_us(void) { return mfs_z80_cycles; }
static void z80_wfi(void) { __asm__("halt"); }

const mfs_port_arch_ops mfs_port_z80_ops = {z80_crit_enter, z80_crit_exit,
                                            z80_cycles,     z80_time_us,
                                            z80_wfi,        5u,
                                            "Z80",          0u};
#endif /* MFS_8BIT_ARCH_Z80 */

/* ==== Auto-detección e inicialización ==== */
mfs_st mfs_port_arch_init(const mfs_config *cfg) {
  (void)cfg; /* Reservado para futuras opciones (prescaler, timer, etc.) */

  if (g_forced_arch != 0xFFu) {
    for (uint8_t i = 0; arch_table[i] != NULL; i++) {
      if (arch_table[i]->arch_id == g_forced_arch) {
        g_port_arch_ops = arch_table[i];
        break;
      }
    }
    if (!g_port_arch_ops)
      return MFS_EINVAL;
  } else {
    /* Auto-detección por macros de compilador */
#if MFS_8BIT_ARCH_AVR
    g_port_arch_ops = &mfs_port_avr_ops;
#elif MFS_8BIT_ARCH_8051
    g_port_arch_ops = &mfs_port_8051_ops;
#elif MFS_8BIT_ARCH_STM8
    g_port_arch_ops = &mfs_port_stm8_ops;
#elif MFS_8BIT_ARCH_PIC
    g_port_arch_ops = &mfs_port_pic_ops;
#elif MFS_8BIT_ARCH_Z80
    g_port_arch_ops = &mfs_port_z80_ops;
#else
    g_port_arch_ops = &mfs_port_generic_ops;
#endif
  }

  if (!g_port_arch_ops)
    return MFS_EINVAL;

  return MFS_OK;
}

const mfs_port_arch_ops *mfs_port_arch_get_ops(void) { return g_port_arch_ops; }

mfs_st mfs_port_arch_set_arch(uint8_t arch_id) {
  for (uint8_t i = 0; arch_table[i] != NULL; i++) {
    if (arch_table[i]->arch_id == arch_id) {
      g_forced_arch = arch_id;
      g_port_arch_ops = arch_table[i];
      return MFS_OK;
    }
  }
  return MFS_EINVAL;
}

/* ==== Wrappers para mfs_port.h (API global) ====
 * Solo se compilan en el build de target 8-bit. En host (tests de la lógica de
 * detección/selección) mfs_port_host.c ya aporta estas primitivas, así que se
 * desactivan para evitar símbolos duplicados. Definir MFS_PORT_ARCH_GLUE=1 en
 * el build de target. */
#ifdef MFS_PORT_ARCH_GLUE
void mfs_port_crit_enter(void) {
  if (g_port_arch_ops && g_port_arch_ops->crit_enter)
    g_port_arch_ops->crit_enter();
}

void mfs_port_crit_exit(void) {
  if (g_port_arch_ops && g_port_arch_ops->crit_exit)
    g_port_arch_ops->crit_exit();
}

uint32_t mfs_port_cycles(void) {
  if (g_port_arch_ops && g_port_arch_ops->cycles)
    return g_port_arch_ops->cycles();
  return 0u;
}

uint32_t mfs_port_time_us(void) {
  if (g_port_arch_ops && g_port_arch_ops->time_us)
    return g_port_arch_ops->time_us();
  return 0u;
}

void mfs_port_wfi(void) {
  if (g_port_arch_ops && g_port_arch_ops->wfi)
    g_port_arch_ops->wfi();
}
#endif /* MFS_PORT_ARCH_GLUE */