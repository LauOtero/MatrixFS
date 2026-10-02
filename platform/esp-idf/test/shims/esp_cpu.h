/* esp_cpu.h — shim minimo de ESP-IDF (contador de ciclos de CPU). */
#ifndef MATRIXFS_TEST_SHIM_ESP_CPU_H
#define MATRIXFS_TEST_SHIM_ESP_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contador de ciclos monotonico. En host se implementa con
 * clock_gettime(CLOCK_MONOTONIC) convertido a una base de 100 MHz, de modo que
 * sea monotonico, no decreciente y con valor distinto de cero. */
uint32_t esp_cpu_get_cycle_count(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_CPU_H */
