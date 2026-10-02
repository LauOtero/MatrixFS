/* esp_attr.h — shim minimo de ESP-IDF (atributos de seccion/alineacion). */
#ifndef MATRIXFS_TEST_SHIM_ESP_ATTR_H
#define MATRIXFS_TEST_SHIM_ESP_ATTR_H

/* En el target alinea a palabra y va a DRAM; en host basta el alineamiento
 * natural de un uint32_t. */
#define WORD_ALIGNED_ATTR
#define IRAM_ATTR
#define DRAM_ATTR

#endif /* MATRIXFS_TEST_SHIM_ESP_ATTR_H */
