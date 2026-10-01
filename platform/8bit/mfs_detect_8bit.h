/* mfs_detect_8bit.h — Auto-detección de capacidades HW en MCU de 8 bits
 *
 * Detecta y clasifica los recursos del microcontrolador para que MatrixFS
 * seleccione automáticamente el modo, la suite de integridad y la estrategia
 * de almacenamiento, igual que en arquitecturas de 16/32 bits.
 *
 * Principios:
 *   - Determinismo: la detección no depende de estado no inicializado.
 *   - Resiliencia: si una capacidad no se puede verificar, se declara ausente
 *     (false-safe) y se usa el fallback software.
 *   - Mínima RAM: toda la detección usa buffers estáticos pequeños.
 */

#ifndef MFS_DETECT_8BIT_H
#define MFS_DETECT_8BIT_H

#include "matrixfs/mfs_port.h"
#include "matrixfs/mfs_types.h"
#include "mfs_port_8bit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Capacidades detectadas del MCU ==== */
typedef struct {
  uint8_t arch_id; /* MFS_8BIT_ARCH_* de mfs_port_8bit.h */
  const char *arch_name;
  uint8_t arch_class; /* siempre 0 para 8-bit */

  uint32_t ram_total;   /* RAM total estimada/detectada (bytes) */
  uint32_t flash_size;  /* Flash interna (bytes), 0 si desconocida */
  uint32_t eeprom_size; /* EEPROM interna (bytes), 0 si desconocida */
  uint32_t f_cpu_hz;    /* Frecuencia de CPU (Hz) */

  /* Capacidades funcionales (0 = ausente) */
  bool has_hw_crc;  /* CRC/checksum por hardware */
  bool has_hw_rng;  /* Generador aleatorio por hardware */
  bool has_eeprom;  /* EEPROM interna */
  bool has_timer16; /* Timer de 16 bits disponible */
  bool has_spi;     /* Periférico SPI */
  bool has_i2c;     /* Periférico I2C/TWI */
  bool has_uart;    /* Periférico UART */

  /* Clasificación resultante */
  mfs_mode_t suggested_mode; /* Modo recomendado según recursos */
  uint8_t suggested_suite;   /* Suite de integridad sugerida */
} mfs_hw_caps_8bit_t;

/* ==== API pública ==== */

/* Ejecuta la detección completa de capacidades del MCU 8-bit.
 * `caps` se rellena con los resultados. Nunca falla de forma bloqueante:
 * en caso de duda declara la capacidad ausente (fallback software). */
mfs_st mfs_detect_8bit_caps(mfs_hw_caps_8bit_t *caps);

/* Ajusta un mfs_config a partir de las capacidades detectadas:
 * rellena arch_class, ram_total, forced_mode y suite_preferred coherentes.
 * No sobreescribe campos ya fijados explícitamente por el integrador. */
mfs_st mfs_detect_8bit_adapt_config(const mfs_hw_caps_8bit_t *caps,
                                    mfs_config *cfg);

/* Detección de RAM disponible por sonda no destructiva (sondeo de direcciones
 * en modelos con RAM mapeada en memoria). Devuelve 0 si no es detectable. */
uint32_t mfs_detect_8bit_ram_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* MFS_DETECT_8BIT_H */