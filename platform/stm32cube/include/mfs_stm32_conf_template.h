/* mfs_stm32_conf_template.h — plantilla de configuracion del port STM32Cube.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * COPIA este fichero a `Core/Inc/mfs_stm32_conf.h` (o al directorio de includes
 * de tu proyecto) y ajusta los valores. Con MFS_STM32_USE_CONF definido, el port
 * ofrece `matrixfs_stm32_mount_default()`, que monta con esta configuracion.
 *
 * SOBREVIVE A LA REGENERACION DE CUBEMX: este fichero vive fuera del arbol que
 * CubeMX regenera (Core/Src, Core/Inc/generated...). Si lo pones en Core/Inc,
 * marca el directorio como "no gestionado" o guarda tu copia en una carpeta
 * propia del proyecto; ver README §Integracion.
 */
#ifndef MFS_STM32_CONF_H
#define MFS_STM32_CONF_H

/* =====================================================================
 * 1. Medio
 *
 * Elige UNO. Los valores coinciden con mfs_stm32_media_t.
 * ===================================================================== */
#define MFS_STM32_MEDIA_IFLASH 0
#define MFS_STM32_MEDIA_OSPI_NOR 1
#define MFS_STM32_MEDIA_SPI_NOR 2
#define MFS_STM32_MEDIA_FRAM 3
#define MFS_STM32_MEDIA_EEPROM 4
#define MFS_STM32_MEDIA_SDMMC 5

#define MFS_STM32_MEDIA MFS_STM32_MEDIA_OSPI_NOR

/* =====================================================================
 * 2. Presupuesto y politica
 *
 * `MFS_STM32_RAM_BUDGET` es lo que el port DECLARA al planificador de
 * viabilidad (seleccion de modo). NO reserva memoria: el consumo real se fija en
 * compilacion (MFS_L2P_SLOTS, MFS_ZONE_MAX, MFS_SCRATCH_MAX) y es bastante
 * mayor. Ver README §Presupuesto de RAM antes de tocarlo.
 * ===================================================================== */
#define MFS_STM32_RAM_BUDGET 65536u

/* MFS_MODE_UNSUPPORTED (0xFF) ⇒ negociar por viabilidad. */
#define MFS_STM32_FORCED_MODE 0xFF

/* 0 = no formatear nunca solo; 1 = formatear si el volumen no es valido. */
#define MFS_STM32_FORMAT_IF_NEEDED 0

/* Clave de cifrado de 32 bytes. Dejala sin definir para montar sin cifrado; NO
 * la escribas en claro en un fichero versionado (cargala de eFuse, de un OTP o
 * de una zona protegida).
 *
 *   extern const uint8_t g_mfs_key[32];
 *   #define MFS_STM32_KEY g_mfs_key
 */

/* =====================================================================
 * 3. Flash interna (MFS_STM32_MEDIA_IFLASH)
 *
 * La region DEBE estar libre de codigo y datos. Reservala en el linker script
 * con platform/stm32cube/linker/matrixfs_region.ld, que incluye los ASSERT de no
 * solapamiento. Mira el mapa de tu dispositivo (`arm-none-eabi-size`,
 * .map) antes de elegirla.
 *
 * OJO: la flash interna tiene un coste real de RAM y de tiempo — ver README.
 * ===================================================================== */
#define MFS_STM32_IFLASH_ADDR 0x08040000u /* inicio de la region reservada */
#define MFS_STM32_IFLASH_REGION_SIZE 0x40000u /* 256 KB                    */

/* Tamano TOTAL de la flash del dispositivo. Necesario para validar el mapa de
 * sectores. Si no se define, se toma del registro FLASH_SIZE del MCU. */
/* #define MFS_STM32_IFLASH_SIZE 0x100000u */ /* 1 MB (F407) */

/* Mapa de sectores propio, si tu variante no coincide con la tabla por familia.
 * La forma siempre correcta es copiarla de tu Reference Manual:
 *
 *   static const mfs_stm32_sector_t g_sectores[] = {
 *     {0x08000000u, 16u*1024u}, {0x08004000u, 16u*1024u}, ...
 *   };
 *   #define MFS_STM32_IFLASH_SECTORS g_sectores
 *   #define MFS_STM32_IFLASH_SECTOR_COUNT (sizeof(g_sectores)/sizeof(g_sectores[0]))
 */

/* =====================================================================
 * 4. Perifericos del HAL
 *
 * Los handles los declara CubeMX en `main.h` (`extern OSPI_HandleTypeDef
 * hospi1;` etc.), asi que basta con incluir ese fichero antes que este. Ajusta
 * los nombres a los de TU proyecto (los de abajo son los que genera CubeMX por
 * defecto para el primer periferico de cada tipo).
 *
 * Los macros deben evaluar a un PUNTERO al handle: de ahi el `&`.
 * ===================================================================== */
#include "main.h" /* declara hospi1/hspi1/hi2c1/hsd1 ... */

#define MFS_STM32_OSPI_HANDLE (&hospi1) /* OSPI_HandleTypeDef (H7/U5/L5) */
#define MFS_STM32_QSPI_HANDLE (&hqspi)  /* QSPI_HandleTypeDef (F7/L4)    */
#define MFS_STM32_SPI_HANDLE (&hspi1)   /* SPI_HandleTypeDef             */
#define MFS_STM32_I2C_HANDLE (&hi2c1)   /* I2C_HandleTypeDef             */
#define MFS_STM32_SD_HANDLE (&hsd1)     /* SD_HandleTypeDef / MMC_...    */

/* =====================================================================
 * 5. Geometria de los medios externos
 * ===================================================================== */
/* OSPI/QUADSPI: 0 ⇒ autodetectar por JEDEC RDID. */
/* #define MFS_STM32_OSPI_TOTAL_SIZE 0x800000u */ /* 8 MB */
/* 0 (recomendado) = lectura indirecta, sin problemas de cache en M7. */
#define MFS_STM32_OSPI_MEMORY_MAPPED 0

/* FRAM / EEPROM: la geometria es OBLIGATORIA (no hay autodeteccion fiable). */
#define MFS_STM32_MEM_BUS_I2C 0 /* 1 para I2C, 0 para SPI */
#define MFS_STM32_I2C_ADDR 0x50u
#define MFS_STM32_MEM_TOTAL_SIZE 0x8000u /* 32 KB (MB85RS256 / 24LC256) */
#define MFS_STM32_MEM_PAGE_SIZE 32u      /* page write de la EEPROM       */
#define MFS_STM32_MEM_ERASE_UNIT 4096u   /* unidad de ZONA del layout     */

#endif /* MFS_STM32_CONF_H */
