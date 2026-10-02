/* esp_flash.h — shim de ESP-IDF (cifrado de flash).
 *
 * `esp_flash_encryption_enabled()` NO se declara en esp_flash.h en IDF real: su
 * cabecera es components/bootloader_support/include/esp_flash_encrypt.h, tanto
 * en v5.5 como en v6.1 (verificado en ambas ramas). El componente incluye
 * "esp_flash.h" y confia en que la declaracion llegue de forma transitiva, asi
 * que este shim reproduce esa cadena incluyendo esp_flash_encrypt.h, que es
 * quien declara la funcion. */
#ifndef MATRIXFS_TEST_SHIM_ESP_FLASH_H
#define MATRIXFS_TEST_SHIM_ESP_FLASH_H

#include "esp_flash_encrypt.h"

#endif /* MATRIXFS_TEST_SHIM_ESP_FLASH_H */
