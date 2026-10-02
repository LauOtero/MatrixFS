/* esp_err.h — shim minimo de ESP-IDF (codigos de error). */
#ifndef MATRIXFS_TEST_SHIM_ESP_ERR_H
#define MATRIXFS_TEST_SHIM_ESP_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

/* Nombre legible del codigo (nunca NULL). */
const char *esp_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_ERR_H */
