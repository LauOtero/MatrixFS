/* stm32_hal_shim.h — modelo de STM32 HAL para los tests de host.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * NO forma parte del firmware: solo existe para poder COMPILAR y EJECUTAR el
 * port STM32Cube real en un PC, sin toolchain de ST ni hardware. Se activa con
 * -DMFS_STM32_HOST_TEST=1: src/mfs_stm32_hal.h incluye ESTA cabecera en lugar
 * de la del dispositivo y mfs_stm32_hal.c no implementa nada (todo su contenido
 * esta bajo #else), de modo que las funciones mfs_stm32_hal_flash_* las aporta
 * el modelo.
 *
 * Lo que aporta:
 *   · los tipos y constantes del HAL que usan los backends del port (FLASH,
 *     OSPI, QSPI, SPI, I2C, SD/MMC);
 *   · el minimo de CMSIS (DWT, CoreDebug, PRIMASK, WFI) que necesita
 *     src/mfs_stm32_port.c;
 *   · el MODELO FIEL de la flash interna, que es lo que da valor a los tests:
 *     respeta la unidad de programacion, la ECC y los sectores NO uniformes.
 *
 * CLAVE DE FIDELIDAD (donde vive la imagen del modelo):
 *   El port LEE la flash a traves de mfs_stm32_hal_flash_read() (capa HAL), de
 *   modo que la imagen puede vivir en RAM. Aun asi el modelo intenta mapearla
 *   EXACTAMENTE en 0x08000000 (VirtualAlloc en Windows) porque una version
 *   anterior del backend leia con `memcpy` desde la direccion absoluta (XIP):
 *   asi el MISMO shim sirve para las dos formas de leer. Si el mapeo falla se
 *   usa un array estatico y shim_flash_at_flash_base() lo reporta.
 */
#ifndef STM32_HAL_SHIM_H
#define STM32_HAL_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matrixfs/mfs_types.h" /* mfs_st */
/* `mfs_sector_t` es el mismo tipo que `mfs_stm32_sector_t` (matrixfs_stm32.h lo
 * define como typedef de aquel). */
#include "mfs_sectors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * HAL basico
 * ===================================================================== */
typedef enum {
  HAL_OK = 0x00,
  HAL_ERROR = 0x01,
  HAL_BUSY = 0x02,
  HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);

/* =====================================================================
 * CMSIS minimo (para src/mfs_stm32_port.c)
 *
 * ELECCION: __CORTEX_M = 0 ⇒ se ejercita el FALLBACK SIN DWT
 * (mfs_stm32_port.c:177-183). Motivo: DWT->CYCCNT es un registro de silicio que
 * el host no puede emular; con el se declararia un contador de ciclos que
 * devuelve siempre el mismo valor (mfs_port_cycles() ≡ 0 y, con el,
 * mfs_bus_measure_hz() de src/core/mfs_hal.c:15-38 mediria 0 Hz). Con
 * __CORTEX_M = 0 el port usa HAL_GetTick()*1000: monotonico y no nulo.
 * DWT/CoreDebug/mascaras se declaran igualmente para que compilar con
 * -D__CORTEX_M=4 (camino DWT) siga funcionando.
 * ===================================================================== */
#ifndef __CORTEX_M
#define __CORTEX_M 0
#endif
#define CoreDebug_DEMCR_TRCENA_Msk (1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk (1u << 0)

typedef struct {
  volatile uint32_t DEMCR;
} shim_coredebug_t;
typedef struct {
  volatile uint32_t CTRL;
  volatile uint32_t CYCCNT;
} shim_dwt_t;

/* SysTick de CMSIS: el port lo usa como base de tiempo sub-ms sin DWT (STM-13).
 * El host no puede emular un contador de silicio, asi que LOAD queda a 0 (BSS)
 * y mfs_port_time_us() cae al fallback HAL_GetTick()*1000, monotono y no nulo.
 */
typedef struct {
  volatile uint32_t CTRL;
  volatile uint32_t LOAD;
  volatile uint32_t VAL;
  volatile uint32_t CALIB;
} shim_systick_t;

extern shim_coredebug_t *const CoreDebug;
extern shim_dwt_t *const DWT;
extern shim_systick_t *const SysTick;

/* Solo lo usa la rama DWT de mfs_port_time_us(); se define para que compilar
 * con -D__CORTEX_M=4 no rompa el enlace. */
extern uint32_t SystemCoreClock;

uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __enable_irq(void);
void __WFI(void);
void __NOP(void);

/* =====================================================================
 * FLASH interna: tipos y constantes
 * ===================================================================== */
#define FLASH_TYPEPROGRAM_HALFWORD 0x01u
#define FLASH_TYPEPROGRAM_WORD 0x02u
#define FLASH_TYPEPROGRAM_DOUBLEWORD 0x03u
#define FLASH_TYPEPROGRAM_QUADWORD 0x04u

#define FLASH_TYPEERASE_PAGES 0x00u
#define FLASH_TYPEERASE_SECTORS 0x01u
#define FLASH_TYPEERASE_MASSERASE 0x02u

#define FLASH_BANK_1 1u
#define FLASH_BANK_2 2u
#define FLASH_BANK_BOTH 3u
#define FLASH_VOLTAGE_RANGE_3 0x02u

/* Union de los campos que usan las familias con PAGINAS (F0/F1/F3/L0/L1/G0/G4/
 * L4/L5/WB/WL) y con SECTORES (F2/F4/F7/H5/H7/U5). */
typedef struct {
  uint32_t TypeErase;
  uint32_t Banks;
  uint32_t Page;
  uint32_t NbPages;
  uint32_t Sector;
  uint32_t NbSectors;
  uint32_t VoltageRange;
} FLASH_EraseInitTypeDef;

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
/* FIRMA MODERNA de 64 bits (F4/F7/L4/H5/H7/U5). */
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address,
                                    uint64_t Data);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit,
                                    uint32_t *SectorError);

/* =====================================================================
 * OSPI / QUADSPI (tipos y constantes ficticios pero con la forma del HAL)
 * ===================================================================== */
typedef struct {
  void *Instance;
} OSPI_HandleTypeDef;
typedef struct {
  void *Instance;
} QSPI_HandleTypeDef;

typedef struct {
  uint32_t OperationType;
  uint32_t FlashId;
  uint32_t Instruction;
  uint32_t InstructionMode;
  uint32_t InstructionSize;
  uint32_t Address;
  uint32_t AddressMode;
  uint32_t AddressSize;
  uint32_t AlternateBytes;
  uint32_t AlternateBytesMode;
  uint32_t AlternateBytesSize;
  uint32_t DummyCycles;
  uint32_t DataMode;
  uint32_t NbData;
  uint32_t DQSMode;
  uint32_t SIOOMode;
} OSPI_RegularCmdTypeDef;

typedef struct {
  uint32_t Instruction;
  uint32_t InstructionMode;
  uint32_t Address;
  uint32_t AddressMode;
  uint32_t AddressSize;
  uint32_t AlternateBytes;
  uint32_t AlternateByteMode;
  uint32_t AlternateBytesSize;
  uint32_t DummyCycles;
  uint32_t DataMode;
  uint32_t NbData;
  uint32_t DdrMode;
  uint32_t DdrHoldHalfCycle;
  uint32_t SIOOMode;
} QSPI_CommandTypeDef;

#define HAL_OSPI_OPTYPE_COMMON_CFG 0u
#define HAL_OSPI_FLASH_ID_1 0u
#define HAL_OSPI_INSTRUCTION_1_LINE 1u
#define HAL_OSPI_INSTRUCTION_8_BITS 8u
#define HAL_OSPI_INSTRUCTION_16_BITS 16u
#define HAL_OSPI_INSTRUCTION_24_BITS 24u
#define HAL_OSPI_INSTRUCTION_32_BITS 32u
#define HAL_OSPI_ADDRESS_NONE 0u
#define HAL_OSPI_ADDRESS_1_LINE 1u
#define HAL_OSPI_ADDRESS_24_BITS 24u
#define HAL_OSPI_ADDRESS_32_BITS 32u
#define HAL_OSPI_ALTERNATE_BYTES_NONE 0u
#define HAL_OSPI_DATA_NONE 0u
#define HAL_OSPI_DATA_1_LINE 1u
#define HAL_OSPI_DATA_4_LINES 4u
#define HAL_OSPI_DQS_DISABLE 0u
#define HAL_OSPI_SIOO_INST_EVERY_CMD 0u

#define QSPI_INSTRUCTION_1_LINE 1u
#define QSPI_ADDRESS_NONE 0u
#define QSPI_ADDRESS_1_LINE 1u
#define QSPI_ADDRESS_4_LINES 4u
#define QSPI_ADDRESS_24_BITS 24u
#define QSPI_ADDRESS_32_BITS 32u
#define QSPI_ALTERNATE_BYTES_NONE 0u
#define QSPI_DATA_NONE 0u
#define QSPI_DATA_1_LINE 1u
#define QSPI_DATA_4_LINES 4u
#define QSPI_DDR_MODE_DISABLE 0u
#define QSPI_DDR_HHC_ANALOG_DELAY 0u
#define QSPI_SIOO_INST_EVERY_CMD 0u

HAL_StatusTypeDef HAL_OSPI_Command(OSPI_HandleTypeDef *h,
                                   OSPI_RegularCmdTypeDef *cmd,
                                   uint32_t timeout);
HAL_StatusTypeDef HAL_OSPI_Receive(OSPI_HandleTypeDef *h, uint8_t *data,
                                   uint32_t timeout);
HAL_StatusTypeDef HAL_OSPI_Transmit(OSPI_HandleTypeDef *h, uint8_t *data,
                                    uint32_t timeout);
HAL_StatusTypeDef HAL_QSPI_Command(QSPI_HandleTypeDef *h,
                                   QSPI_CommandTypeDef *cmd, uint32_t timeout);
HAL_StatusTypeDef HAL_QSPI_Receive(QSPI_HandleTypeDef *h, uint8_t *data,
                                   uint32_t timeout);
HAL_StatusTypeDef HAL_QSPI_Transmit(QSPI_HandleTypeDef *h, uint8_t *data,
                                    uint32_t timeout);

/* =====================================================================
 * SPI / I2C
 * ===================================================================== */
typedef struct {
  void *Instance;
} SPI_HandleTypeDef;
typedef struct {
  void *Instance;
} I2C_HandleTypeDef;

#define I2C_MEMADD_SIZE_8BIT 0x01u
#define I2C_MEMADD_SIZE_16BIT 0x02u

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *data,
                                   uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *data,
                                  uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx,
                                          uint8_t *rx, uint16_t size,
                                          uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t dev,
                                   uint16_t mem, uint16_t memsize, uint8_t *dst,
                                   uint16_t len, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t dev,
                                    uint16_t mem, uint16_t memsize,
                                    uint8_t *src, uint16_t len,
                                    uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t dev,
                                        uint32_t trials, uint32_t timeout);

/* =====================================================================
 * SD / MMC
 * ===================================================================== */
typedef struct {
  uint32_t LogBlockNbr;
  uint32_t LogBlockSize;
} shim_cardinfo_t;

typedef struct {
  void *Instance;
  shim_cardinfo_t SdCard;
  shim_cardinfo_t MmcCard;
} SD_HandleTypeDef;

typedef SD_HandleTypeDef MMC_HandleTypeDef;

#define HAL_SD_CARD_TRANSFER 4u
#define HAL_MMC_CARD_TRANSFER 4u

HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef *h, uint8_t *dst,
                                    uint32_t lba, uint32_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef *h, const uint8_t *src,
                                     uint32_t lba, uint32_t n,
                                     uint32_t timeout);
uint32_t HAL_SD_GetCardState(SD_HandleTypeDef *h);
HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef *h, uint32_t start,
                               uint32_t end);

/* =====================================================================
 * Modelo de la flash interna (lo que de verdad se prueba)
 * ===================================================================== */

/* Unidad de programacion y ECC del dispositivo SIMULADO. Se sobrescriben desde
 * la linea de comandos para probar 4 B sin ECC (F4), 8 B con ECC (L4/G4) y
 * 16 B con ECC (H7/U5) con el MISMO codigo de backend. Se definen en la
 * cabecera (y no en el .c) para que test y modelo compartan el valor. */
#ifndef MFS_STM32_HOST_PGM_UNIT
#define MFS_STM32_HOST_PGM_UNIT 4u
#endif
#ifndef MFS_STM32_HOST_ECC
#define MFS_STM32_HOST_ECC 0
#endif

/* Base y tamano de la flash modelada: 1 MiB en 0x08000000. */
#define SHIM_FLASH_BASE 0x08000000u
#define SHIM_FLASH_SIZE (1u << 20)

/* Mapa de sectores NO uniforme, tipo STM32F4 de 1 MB de UN SOLO BANCO
 * (STM32F405/407VG, RM0090 tabla de sectores):
 *   4 x 16 KB + 1 x 64 KB + 7 x 128 KB = 12 sectores = 1 MiB.
 * La mezcla 16/64/128 KB es la del F4; hay SIETE sectores de 128 KB (no tres)
 * porque un F4 de 1 MB real los tiene: la tabla de dos bancos de
 * mfs_stm32_flash_map.c (F4_1M_RUNS) deja solo 3 sectores de 128 KB contiguos,
 * y eso NO permite montar volumen (ver test_stm32.c, "restriccion de layout").
 * Es la tabla que el test pasa en `mfs_stm32_cfg.sectors`. */
extern const mfs_sector_t shim_sectors[];
extern const uint32_t shim_sector_count;

/* Instala la geometria ACTIVA del modelo (por defecto `shim_sectors`). Sirve
 * para probar el borrado por tramo con OTRA tabla (p. ej. sectores fisicos de
 * 512 B agrupados en una unidad logica de 1024 B, donde un solo erase del
 * nucleo debe borrar DOS sectores del dispositivo). `shim_flash_reset()` vuelve
 * a la tabla por defecto. */
void shim_flash_set_sectors(const mfs_sector_t *s, uint32_t n);

/* --- API del modelo, para los tests --- */

/* Deja la imagen a 0xFF y los contadores a cero. El modelo queda SIEMPRE listo:
 * si el host no puede mapear 0x08000000 se usa un array estatico (ver
 * shim_flash_at_flash_base()), porque el port actual lee a traves de
 * mfs_stm32_hal_flash_read() y no necesita la direccion real. */
void shim_flash_reset(void);
uint8_t *shim_flash_raw(void);  /* imagen del modelo */
uint32_t shim_flash_size(void); /* 1 MiB */
bool shim_flash_ready(void);    /* el modelo tiene imagen */
/* Cierto si la imagen esta mapeada EXACTAMENTE en 0x08000000 (necesario si el
 * backend volviera a leer con puntero directo: XIP). */
bool shim_flash_at_flash_base(void);
bool shim_flash_was_double_programmed(
    void);                               /* se intento reprogramar una unidad */
uint32_t shim_flash_program_count(void); /* unidades programadas */
uint32_t shim_flash_erase_count(void);   /* sectores borrados */
/* Programas ejecutados con el controlador BLOQUEADO. Evidencia de que el port
 * no llama a HAL_FLASH_Unlock() en el camino de programacion (ver informe y
 * mfs_stm32_hal.c:79-115 frente a :191). */
uint32_t shim_flash_program_while_locked_count(void);
void shim_flash_clear_flags(void);

/* =====================================================================
 * Superficie mfs_stm32_hal_flash_* del build de host
 *
 * El port actual solo necesita _size/_unlock/_lock/_program/_erase_sector/
 * _single_bank; _read/_bytes/_quadword se aportan tambien para que el modelo
 * siga siendo valido si el port pasa a leer/programar a traves de ganchos (en
 * cualquier caso, sobre la MISMA imagen mapeada). Ninguna de ellas se redefine
 * en mfs_stm32_hal.c en host: alli el fichero entero esta bajo #else.
 * ===================================================================== */
uint32_t mfs_stm32_hal_flash_size(void);
mfs_st mfs_stm32_hal_flash_unlock(void);
mfs_st mfs_stm32_hal_flash_lock(void);
mfs_st mfs_stm32_hal_flash_program(uint32_t addr, uint64_t data, uint32_t unit);
mfs_st mfs_stm32_hal_flash_erase_sector(uint32_t addr, uint32_t size);
bool mfs_stm32_hal_flash_single_bank(void);
mfs_st mfs_stm32_hal_flash_read(uint32_t addr, void *dst, uint32_t len);
mfs_st mfs_stm32_hal_flash_program_bytes(uint32_t addr, const uint8_t *bytes,
                                         uint32_t unit);
mfs_st mfs_stm32_hal_flash_program_block(uint32_t addr, const uint8_t *bytes,
                                         uint32_t len, uint32_t unit);
mfs_st mfs_stm32_hal_flash_program_quadword(uint32_t addr,
                                            const uint8_t bytes[16]);

#ifdef __cplusplus
}
#endif
#endif /* STM32_HAL_SHIM_H */
