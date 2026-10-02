/* esp_timer.h — shim minimo de ESP-IDF (reloj monotonico en microsegundos). */
#ifndef MATRIXFS_TEST_SHIM_ESP_TIMER_H
#define MATRIXFS_TEST_SHIM_ESP_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Microsegundos desde el arranque del proceso (CLOCK_MONOTONIC). */
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_TIMER_H */
