/* esp_partition.h — shim de ESP-IDF (API esp_partition usada por el componente
 * MatrixFS). La implementacion esta en
 * platform/esp-idf/test/esp_partition_fake.c sobre una particion NOR simulada.
 *
 * FIDELIDAD DE VERSION. Se reproduce la forma de las cabeceras reales, que es
 * IDENTICA en v5.5 y v6.1 salvo un detalle: v6 anade el campo `readonly` al
 * final de `esp_partition_t`. Los campos comunes (address, size, erase_size,
 * label[], type, subtype, encrypted) conservan nombre y orden, de modo que un
 * inicializador designado escrito para v5 compila tambien en v6.
 */
#ifndef MATRIXFS_TEST_SHIM_ESP_PARTITION_H
#define MATRIXFS_TEST_SHIM_ESP_PARTITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_flash_t esp_flash_t;

typedef enum {
  ESP_PARTITION_TYPE_APP = 0x00,
  ESP_PARTITION_TYPE_DATA = 0x01,
  ESP_PARTITION_TYPE_BOOTLOADER = 0x02,
  ESP_PARTITION_TYPE_PARTITION_TABLE = 0x03,
  ESP_PARTITION_TYPE_ANY = 0xff,
} esp_partition_type_t;

typedef enum {
  ESP_PARTITION_SUBTYPE_ANY = 0xff,
  ESP_PARTITION_SUBTYPE_DATA_UNDEFINED = 0x06,
  ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81,
  ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82,
  ESP_PARTITION_SUBTYPE_DATA_LITTLEFS = 0x83,
} esp_partition_subtype_t;

typedef struct {
  esp_flash_t *flash_chip;
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t address;
  uint32_t size;
  uint32_t erase_size;
  char label[17];
  bool encrypted;
#if MATRIXFS_SHIM_IDF_PROFILE >= 2
  bool readonly; /* anadido en IDF v6 */
#endif
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label);

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t src_offset, void *dst, size_t size);

esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t dst_offset, const void *src, size_t size);

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_ESP_PARTITION_H */
