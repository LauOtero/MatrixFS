/* esp_flash_encrypt.h — shim de la cabecera REAL que declara
 * esp_flash_encryption_enabled() en IDF v5.5 y v6.1:
 * components/bootloader_support/include/esp_flash_encrypt.h
 *
 * El componente solo usa la consulta de estado; el shim la declara y anade
 * helpers para que la prueba pueda alternarla.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_FLASH_ENCRYPT_H
#define MATRIXFS_TEST_SHIM_ESP_FLASH_ENCRYPT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* true si la particion de la app esta cifrada con flash encryption. El
 * componente lo consulta en cada `esp_prepare` para elegir la granularidad de
 * programacion (4 B sin cifrado, 16 B con cifrado). */
bool esp_flash_encryption_enabled(void);

/* --- Solo del shim: alternar el estado y observar cuantas consultas hubo --- */
void shim_flash_set_encryption(bool on);
unsigned shim_flash_encryption_queries(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_FLASH_ENCRYPT_H */
