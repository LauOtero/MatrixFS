/* matrixfs_stm32.h — API publica del port STM32Cube de MatrixFS.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integra MatrixFS (sistema de archivos embebido en C11, sin heap) con
 * STM32Cube (HAL) sobre los medios de almacenamiento tipicos de un STM32:
 *
 *   · flash INTERNA del MCU      → HAL_FLASH   (mfs_stm32_iflash.c)
 *   · NOR externa OSPI/QUADSPI   → HAL_OSPI/QSPI (mfs_stm32_ospi.c)
 *   · NOR externa SPI            → HAL_SPI     (mfs_stm32_spi_nor.c)
 *   · FRAM / EEPROM SPI o I2C    → HAL_SPI/I2C (mfs_stm32_fram.c)
 *   · SD / eMMC                  → HAL_SD      (mfs_stm32_sd.c)
 *
 * El port NO reimplementa el sistema de archivos: compila el nucleo y la capa
 * embebida compartida (platform/embedded) y solo aporta:
 *   1) las primitivas obligatorias del puerto (§20.2)  [mfs_stm32_port.c];
 *   2) los callbacks de medio sobre el HAL              [backends];
 *   3) la API de montaje/formateo y la validacion de la region reservada.
 *
 * INSTANCIA UNICA. El nucleo usa tablas estaticas y el port aloja el descriptor
 * del medio en estado estatico: solo puede haber UN volumen montado a la vez.
 *
 * SINCRONIZACION. El nucleo no es reentrante. Con FreeRTOS/CMSIS-RTOS activos,
 * `matrixfs_stm32_lock()/unlock()` serializan; el port las usa internamente en
 * el montaje, pero las llamadas DIRECTAS al nucleo (mf_open, mf_write, ...) no
 * pasan por aqui: si el volumen se usa desde varias tareas, la aplicacion debe
 * envolverlas o registrar un mutex con
 * `matrixfs_stm32_set_lock_hooks()`. Ver README.
 */
#ifndef MATRIXFS_STM32_H
#define MATRIXFS_STM32_H

#include <stdbool.h>
#include <stdint.h>

#include "matrixfs/matrixfs.h"
#include "mfs_embedded.h"
/* Tipo de sector compartido con el resto del proyecto: la busqueda de ventana
 * uniforme la implementa platform/common/mfs_sectors.c (probada en host) y el
 * port la reutiliza en vez de duplicar la aritmetica. */
#include "mfs_sectors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Medio seleccionado ==== */
typedef enum {
  MFS_STM32_MEDIA_IFLASH = 0, /* flash interna del MCU (HAL_FLASH)       */
  MFS_STM32_MEDIA_OSPI_NOR,   /* NOR externa OSPI / QUADSPI              */
  MFS_STM32_MEDIA_SPI_NOR,    /* NOR externa SPI (mfs_l2_8bit)           */
  MFS_STM32_MEDIA_FRAM,       /* FRAM SPI o I2C (mfs_l2_8bit)            */
  MFS_STM32_MEDIA_EEPROM,     /* EEPROM SPI o I2C (mfs_l2_8bit)          */
  MFS_STM32_MEDIA_SDMMC       /* SD / eMMC (mfs_l2_managed)              */
} mfs_stm32_media_t;

/* ==== Transporte del medio (SPI/I2C) ==== */
typedef enum {
  MFS_STM32_BUS_SPI = 0,
  MFS_STM32_BUS_I2C = 1
} mfs_stm32_bus_t;

/* ==== Sector/pagina de borrado del dispositivo (direcciones ABSOLUTAS) ====
 * Necesario para las familias con sectores NO uniformes (F1, F4, F7, L0, L1,
 * G0, U5...). El integrador puede aportar su propia tabla; si deja `sectors` a
 * NULL se usa la tabla por familia de mfs_stm32_flash_map.c.
 *
 * Es el MISMO tipo que usa platform/common/mfs_sectors.c. */
typedef mfs_sector_t mfs_stm32_sector_t;

/* ==== Configuracion del port ==== */
typedef struct {
  mfs_stm32_media_t media;

  /* --- Presupuesto y politica (se traduce a mfs_embedded_opts) --- */
  uint32_t ram_total;       /* RAM real disponible para el FS (bytes) */
  mfs_mode_t forced_mode;   /* MFS_MODE_UNSUPPORTED = automatico      */
  const uint8_t *key;       /* 32 B o NULL (sin cifrado)             */
  uint8_t suite_preferred;  /* mfs_suite_t o 0xFF = negociar         */
  uint32_t bus_speed_hz;    /* 0 ⇒ no declarar                       */
  uint32_t uid, gid;
  uint16_t file_perm, dir_perm;
  bool allow_convergent, dedup_enable, cdc_enable, zrp_enable, dab_enable;
  bool format_if_needed;

  /* --- Flash interna (MFS_STM32_MEDIA_IFLASH) ---
   * La region reservada. DEBE estar libre de codigo y datos: el linker script
   * de platform/stm32cube/linker/matrixfs_region.ld lo verifica con un ASSERT.
   * Si son 0 se usan MFS_STM32_IFLASH_ADDR/SIZE de mfs_stm32_conf.h. */
  uint32_t region_addr;
  uint32_t region_size;
  /* Mapa de sectores del dispositivo. NULL ⇒ tabla por familia. */
  const mfs_stm32_sector_t *sectors;
  uint32_t sector_count;

  /* --- OSPI/QUADSPI, SPI, I2C, SD: handles del HAL ---
   * Se declaran `void *` a proposito: la cabecera publica no debe arrastrar
   * las cabeceras del HAL (que cambian por familia y por version de CubeMX).
   * El tipo esperado en cada caso:
   *   ospi  → OSPI_HandleTypeDef *  (H7/U5/L5) o QSPI_HandleTypeDef * (F7/L4)
   *   spi   → SPI_HandleTypeDef *
   *   i2c   → I2C_HandleTypeDef *
   *   sd    → SD_HandleTypeDef * o MMC_HandleTypeDef *
   * Un handle NULL es error de configuracion (MFS_EINVAL). */
  void *ospi;
  void *spi;
  void *i2c;
  void *sd;

  /* --- Geometria del medio externo ---
   * 0 ⇒ autodetectar (JEDEC RDID en la NOR). En FRAM/EEPROM hay que declararla:
   * esos dispositivos no responden a RDID con una capacidad JEDEC fiable. */
  uint32_t total_size;
  uint32_t erase_unit; /* 0 ⇒ 4096 en NOR, 0 (sin borrado) en FRAM/EEPROM */
  uint32_t page_size;  /* 0 ⇒ 256 (NOR) o 1/64 (FRAM/EEPROM)             */
  uint32_t t_prog_max_us;
  uint32_t t_erase_max_us;
  uint32_t t_read_max_us;

  /* --- FRAM/EEPROM por I2C --- */
  mfs_stm32_bus_t bus;
  uint8_t i2c_addr; /* direccion de 7 bits */

  /* --- OSPI: modo memory-mapped para las lecturas del nucleo.
   * false (recomendado) ⇒ lecturas en modo indirecto, sin problemas de
   * coherencia de cache en Cortex-M7. true ⇒ se asume que el BSP ha configurado
   * la region como non-cacheable/write-through por MPU Y que invalida la
   * D-cache tras programar. */
  bool memory_mapped;

  /* --- SD/eMMC: geometria (0 ⇒ derivar del propio dispositivo) --- */
  uint16_t sd_sector_size;  /* 0 ⇒ 512 */
  uint32_t sd_sector_count; /* 0 ⇒ consultar la tarjeta */
} mfs_stm32_cfg;

/* ==== Ciclo de vida ==== */

/* Monta el volumen. `fs` es la instancia del nucleo (la aporta el llamador y
 * debe sobrevivir al montaje); requiere incluir "mfs_internal.h" para
 * declararla. `cfg` debe seguir valida mientras el volumen este montado
 * (el port guarda punteros a la region y a la tabla de sectores). */
mfs_st matrixfs_stm32_mount(mf_t *fs, const mfs_stm32_cfg *cfg);

/* Formatea el volumen (destruye el contenido). Deja `fs` SIN montar. */
mfs_st matrixfs_stm32_format(mf_t *fs, const mfs_stm32_cfg *cfg);

/* Monta usando MFS_STM32_* de "mfs_stm32_conf.h". El handle del periferico
 * tambien se toma de ahi (extern hspi1/hospi1/..., como los declara CubeMX). */
mfs_st matrixfs_stm32_mount_default(mf_t *fs);

/* Desmonta y libera el descriptor del medio (mf_deinit + cierre del backend). */
mfs_st matrixfs_stm32_deinit(mf_t *fs);

/* ==== Diagnostico ==== */

/* Ultimo mensaje legible (nunca NULL). Buffer estatico por el mutex activo;
 * sin hooks de bloqueo no es thread-safe. */
const char *matrixfs_stm32_last_error(void);

/* Capacidad util REAL del volumen, en bytes. El nucleo direcciona como maximo
 * MFS_ZONE_MAX zonas de `erase_unit` bytes: una region mayor NO se aprovecha
 * entera. Devuelve 0 si no hay volumen preparado. */
uint32_t matrixfs_stm32_capacity_bytes(void);

/* Cierto si la region reservada es mayor que la capacidad util. */
bool matrixfs_stm32_capacity_wasteful(void);

/* HWV resuelto en el ultimo montaje (NULL si no hay ninguno). Permite leer
 * `erase_unit`, `flags0` (p. ej. MFS_HWV0_ECC_ON_DIE) y `media_type` reales. */
const mfs_hwv_t *matrixfs_stm32_hwv(void);

/* Nombre del medio, para trazas. */
const char *matrixfs_stm32_media_name(mfs_stm32_media_t m);

/* ==== Sincronizacion ==== */

/* Hooks de bloqueo del integrador (mutex de FreeRTOS/CMSIS-RTOS). Si se
 * registran, el port los usa para serializar montaje/formateo; las llamadas
 * directas al nucleo siguen siendo responsabilidad de la aplicacion, que puede
 * usar matrixfs_stm32_lock()/unlock() o el mismo mutex. */
typedef void (*mfs_stm32_lock_fn)(void);
void matrixfs_stm32_set_lock_hooks(mfs_stm32_lock_fn lock,
                                   mfs_stm32_lock_fn unlock);

/* Toma/suelta el mutex registrado. Sin hooks son no-op. */
void matrixfs_stm32_lock(void);
void matrixfs_stm32_unlock(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_STM32_H */
