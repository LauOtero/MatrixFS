/* esp_log.h — shim minimo de ESP-IDF (macros de log a stdout).
 *
 * Formato equivalente al de IDF: "<LETRA> (tiempo) TAG: mensaje", de modo que
 * los avisos del componente (p. ej. el ESP_LOGW de capacidad desaprovechada)
 * sean visibles en la salida de la prueba.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_LOG_H
#define MATRIXFS_TEST_SHIM_ESP_LOG_H

#include <stdio.h>

#include "esp_timer.h"

#define ESP_LOG_LEVEL(level, tag, fmt, ...)                                    \
  do {                                                                         \
    printf("%s (%lld) %s: " fmt "\n", (level),                                 \
           (long long)(esp_timer_get_time() / 1000), (tag), ##__VA_ARGS__);     \
    fflush(stdout);                                                            \
  } while (0)

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_LEVEL("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_LEVEL("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_LEVEL("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_LEVEL("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_LEVEL("V", tag, fmt, ##__VA_ARGS__)

#endif /* MATRIXFS_TEST_SHIM_ESP_LOG_H */
