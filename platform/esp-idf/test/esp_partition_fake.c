/* esp_partition_fake.c — modelo FIEL de una particion esp_partition sobre RAM.
 *
 * Sustituye a la flash real para poder ejecutar el componente ESP-IDF de
 * MatrixFS en un host. El modelo impone las restricciones REALES de una NOR y
 * de una particion con flash encryption, porque son las que dan valor a la
 * prueba:
 *
 *   1) Limites: toda operacion fuera de [0, size) devuelve ESP_ERR_INVALID_ARG.
 *   2) SEMANTICA NOR en la programacion: el byte almacenado pasa a ser
 *      `old & new` (los bits solo van de 1 a 0). Asignar `new` a ciegas
 *      ocultaria exactamente el fallo que el read-modify-write del componente
 *      existe para evitar.
 *   3) Cifrado de flash activo: `dst_offset` Y `size` deben ser multiplos del
 *      bloque de cifrado (16 B), como documenta IDF para particiones cifradas;
 *      si no, ESP_ERR_INVALID_ARG. Es la prueba de regresion del alineamiento
 *      de 16 B del componente.
 *   4) Borrado: `offset` y `size` multiplos del tamano de sector de la particion
 *      (4096 B) y dentro de limites; los bytes pasan a 0xFF.
 *
 * El medio ES un unico array estatico de 1 MiB en el ambito del fichero (nada
 * de malloc): 256 sectores de 4096 B.
 *
 * Ademas lleva contabilidad de las escrituras ACEPTADAS (granularidad de
 * alineacion observada) para que la prueba pueda demostrar que el componente
 * escribe siempre en bloques de 16 B cuando el cifrado esta activo.
 */
/* La cabecera del shim primero: aporta `struct dirent` con d_type en MinGW. */
#include "esp_vfs.h"

#include "esp_partition.h"

#include <string.h>

#include "esp_flash.h"
#include "idf_shim.h"

/* ============================== Medio ================================== */

#define SHIM_SECTOR_SIZE 4096u
#define SHIM_SECTOR_COUNT 256u                        /* 256 * 4096 = 1 MiB */
#define SHIM_PART_SIZE (SHIM_SECTOR_COUNT * SHIM_SECTOR_SIZE)
#define SHIM_PART_LABEL "matrixfs"
#define SHIM_PART_ADDRESS 0x100000u /* direccion fisica plausible en el mapa */
#define SHIM_PART_TYPE ESP_PARTITION_TYPE_DATA
#define SHIM_PART_SUBTYPE ((esp_partition_subtype_t)0x06) /* data undefined */
#define SHIM_ENC_BLOCK 16u

/* El medio: un unico array estatico de 1 MiB (sin malloc). */
static uint8_t s_medium[SHIM_PART_SIZE];

/* `label` es un array de char en IDF real (char label[17]), no un puntero, de
 * modo que se copia la etiqueta al arrancar. */
static esp_partition_t s_partition = {
    .flash_chip = NULL,
    .type = SHIM_PART_TYPE,
    .subtype = SHIM_PART_SUBTYPE,
    .address = SHIM_PART_ADDRESS,
    .size = SHIM_PART_SIZE,
    .erase_size = SHIM_SECTOR_SIZE,
    .encrypted = false,
};

static bool s_partition_label_set = false;

static void shim_partition_init_label(void) {
  if (s_partition_label_set)
    return;
  memset(s_partition.label, 0, sizeof(s_partition.label));
  memcpy(s_partition.label, SHIM_PART_LABEL, sizeof(SHIM_PART_LABEL));
  s_partition_label_set = true;
}

/* ====================== Contabilidad de escrituras ===================== */

static uint32_t s_wr_count = 0u;
static uint32_t s_wr_min_gran = 16u;
static uint32_t s_align_rejects = 0u;

/* Clasifica el alineamiento de una escritura ACEPTADA: 16, 4 o 1 bytes. Se
 * queda con el peor caso observado. */
static void shim_note_write(size_t offset, size_t size) {
  s_wr_count++;
  if (offset % 16u != 0u || size % 16u != 0u) {
    if (offset % 4u != 0u || size % 4u != 0u)
      s_wr_min_gran = 1u;
    else if (s_wr_min_gran > 4u)
      s_wr_min_gran = 4u;
  }
}

uint32_t shim_partition_write_min_gran(void) { return s_wr_min_gran; }

uint32_t shim_partition_write_count(void) { return s_wr_count; }

uint32_t shim_partition_align_rejects(void) { return s_align_rejects; }

void shim_partition_reset_stats(void) {
  s_wr_count = 0u;
  s_align_rejects = 0u;
  s_wr_min_gran = 16u; /* sin datos: se asume lo mas grueso */
}

/* ============================ Ayudantes ================================ */

void shim_partition_reset(void) {
  memset(s_medium, 0xFF, sizeof(s_medium));
  shim_flash_set_encryption(false);
  shim_partition_reset_stats();
}

uint8_t *shim_partition_raw(void) { return s_medium; }

/* ============================== API ==================================== */

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label) {
  shim_partition_init_label();
  if (type != ESP_PARTITION_TYPE_DATA)
    return NULL;
  if (subtype != ESP_PARTITION_SUBTYPE_ANY && subtype != s_partition.subtype)
    return NULL;
  if (label && strcmp(label, s_partition.label) != 0)
    return NULL;
  return &s_partition;
}

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t src_offset, void *dst, size_t size) {
  if (!partition || (!dst && size > 0u))
    return ESP_ERR_INVALID_ARG;
  if (src_offset > partition->size || size > partition->size - src_offset)
    return ESP_ERR_INVALID_ARG;
  memcpy(dst, s_medium + src_offset, size);
  return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t dst_offset, const void *src, size_t size) {
  if (!partition || (!src && size > 0u))
    return ESP_ERR_INVALID_ARG;
  if (dst_offset > partition->size || size > partition->size - dst_offset)
    return ESP_ERR_INVALID_ARG;

  /* Restriccion REAL de las particiones cifradas: offset y longitud deben ser
   * multiplos del bloque de cifrado (16 B). Es la regresion del alineamiento. */
  if (esp_flash_encryption_enabled() &&
      (dst_offset % SHIM_ENC_BLOCK != 0u || size % SHIM_ENC_BLOCK != 0u)) {
    s_align_rejects++;
    return ESP_ERR_INVALID_ARG;
  }

  const uint8_t *in = (const uint8_t *)src;
  uint8_t *out = s_medium + dst_offset;
  for (size_t i = 0; i < size; i++)
    out[i] &= in[i]; /* NOR: los bits solo van de 1 a 0 */

  shim_note_write(dst_offset, size);
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size) {
  if (!partition)
    return ESP_ERR_INVALID_ARG;
  size_t unit = partition->erase_size ? partition->erase_size : SHIM_SECTOR_SIZE;
  if (offset % unit != 0u || size % unit != 0u)
    return ESP_ERR_INVALID_ARG;
  if (offset > partition->size || size > partition->size - offset)
    return ESP_ERR_INVALID_ARG;
  memset(s_medium + offset, 0xFF, size);
  return ESP_OK;
}
