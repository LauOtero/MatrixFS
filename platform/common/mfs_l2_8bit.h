/* mfs_l2_8bit.h — MatrixFS 8-bit L2 Drivers (SPI NOR, FRAM, EEPROM,
 * Internal Flash, SD SPI)
 *
 * Drivers L2 optimizados para 8-bit: código compacto, sin malloc, buffers
 * estáticos, operaciones atómicas, timeouts deterministas.
 */

#ifndef MFS_L2_8BIT_H
#define MFS_L2_8BIT_H

#include "matrixfs/mfs_port.h"
#include "matrixfs/mfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Tipos de driver 8-bit ==== */
typedef enum {
  MFS_L2_8BIT_NONE = 0,
  MFS_L2_8BIT_SPI_NOR,        /* NOR SPI Flash (W25Qxx, GD25Q, AT25, etc.) */
  MFS_L2_8BIT_SPI_FRAM,       /* FRAM SPI (FM25Vxx, CY15Bxx) */
  MFS_L2_8BIT_I2C_FRAM,       /* FRAM I2C (MB85RSxx) */
  MFS_L2_8BIT_SPI_EEPROM,     /* EEPROM SPI (25AA/25LCxx) */
  MFS_L2_8BIT_I2C_EEPROM,     /* EEPROM I2C (24AA/24LCxx) */
  MFS_L2_8BIT_INTERNAL_FLASH, /* Flash interna MCU (STM8, AVR, PIC, etc.) */
  MFS_L2_8BIT_SD_SPI          /* SD Card modo SPI */
} mfs_l2_8bit_type_t;

/* ==== Configuración genérica de driver 8-bit ==== */
typedef struct {
  mfs_l2_8bit_type_t type;

  /* SPI/I2C: funciones de bus (debe proveer el BSP) */
  mfs_st (*spi_init)(void *ctx);
  mfs_st (*spi_deinit)(void *ctx);
  /* `len` es el número REAL de bytes a transferir. Se declara de 32 bits: con
   * 16 se truncaban en silencio las transferencias > 64 KiB. El BSP puede
   * trocear internamente si su controlador tiene un límite menor. */
  mfs_st (*spi_transfer)(void *ctx, const uint8_t *tx, uint8_t *rx,
                         uint32_t len);
  mfs_st (*spi_cs_low)(void *ctx);
  mfs_st (*spi_cs_high)(void *ctx);

  /* I2C específico (FRAM/EEPROM I2C: MB85RSxx, 24AA/24LCxx).
   *
   * Se declaran operaciones de MEMORIA (dirección de memoria + datos), no
   * transferencias crudas: es lo que el dispositivo necesita (enviar la word
   * address y luego leer/escribir) y lo que ofrece el HAL (`HAL_I2C_Mem_Read` /
   * `HAL_I2C_Mem_Write`), que además resuelve el repeated-START. */
  mfs_st (*i2c_init)(void *ctx);
  mfs_st (*i2c_deinit)(void *ctx);
  mfs_st (*i2c_mem_read)(void *ctx, uint8_t dev, uint32_t mem_addr,
                         uint16_t mem_addr_bytes, uint8_t *dst, uint32_t len);
  mfs_st (*i2c_mem_write)(void *ctx, uint8_t dev, uint32_t mem_addr,
                          uint16_t mem_addr_bytes, const uint8_t *src,
                          uint32_t len);
  /* Dirección I2C de 7 bits del dispositivo (FRAM/EEPROM I2C). */
  uint8_t i2c_addr;

  /* Flash interna: funciones de bajo nivel. `addr` es ABSOLUTA en el mapa del
   * MCU. `iflash_write` debe respetar la unidad de programación del
   * dispositivo (y su ECC) y retornar solo tras confirmar (barrera WOB);
   * `iflash_erase_page` borra la unidad de borrado que contiene `addr`. */
  mfs_st (*iflash_init)(void *ctx);
  mfs_st (*iflash_erase_page)(void *ctx, uint32_t addr);
  mfs_st (*iflash_write)(void *ctx, uint32_t addr, const uint8_t *data,
                         uint32_t len);
  mfs_st (*iflash_read)(void *ctx, uint32_t addr, uint8_t *data, uint32_t len);

  /* Parámetros del dispositivo */
  uint32_t total_size; /* Tamaño total en bytes */
  uint32_t page_size;  /* Tamaño de página de PROGRAMACIÓN del dispositivo.
                        * En NOR SPI es la página de Page Program (256 B): una
                        * orden PP NO puede cruzar esa frontera. En EEPROM es la
                        * página de escritura (page write, 32/64 B). */
  uint32_t
      erase_size; /* Tamaño de sector/block erase (0 = no erase necesario) */
  /* Mínimo programable en bytes (NOR/EEPROM byte a byte: 1; flash interna de
   * un STM32F4: 4; L4/G4: 8; H7: 16). Se propaga a mfs_media_geom. */
  uint32_t pgm_gran;
  /* El dispositivo lleva ECC propia (flash interna de un MCU, NOR con ECC
   * interna). Se declara en el HWV (MFS_HWV0_ECC_ON_DIE) y hace que el núcleo
   * elija la clase ECC más tolerante. NO se deduce de `pgm_gran`: un STM32F4
   * programa de 4 en 4 bytes y no tiene ECC. */
  bool ecc_on_die;
  uint16_t addr_bytes;  /* Bytes de dirección: 2 o 3 (SPI) */
  uint8_t addr_width;   /* 16 o 24 bits */
  bool has_quad_spi;    /* Soporte Quad SPI */
  bool has_dtr;         /* Double Transfer Rate */
  uint32_t max_freq_hz; /* Frecuencia máxima SPI/I2C */

  /* Timeouts (µs) - deterministas para 8-bit */
  uint32_t t_prog_max_us;
  uint32_t t_erase_max_us;
  uint32_t t_read_max_us;

  /* Contexto del BSP (pines, periféricos, etc.) */
  void *ctx;
} mfs_l2_8bit_cfg_t;

/* ==== Driver L2 8-bit (implementa mfs_l2_driver de mfs_port.h) ==== */
typedef struct mfs_l2_8bit_driver {
  mfs_l2_driver base;    /* Interfaz estándar MatrixFS */
  mfs_l2_8bit_cfg_t cfg; /* Configuración */
  uint8_t status_reg[3]; /* Cache de status registers */
  bool initialized;
  /* Buffers estáticos para evitar malloc (tamaño = max page_size) */
  uint8_t page_buffer[256]; /* Buffer de página (max 256B para 8-bit) */
  uint8_t cmd_buffer[8];    /* Buffer de comandos SPI */
} mfs_l2_8bit_driver_t;

/* ==== API pública ==== */

/* Crea e inicializa driver 8-bit */
mfs_st mfs_l2_8bit_create(const mfs_l2_8bit_cfg_t *cfg,
                          mfs_l2_8bit_driver_t **out_drv);

/* Destruye driver */
void mfs_l2_8bit_destroy(mfs_l2_8bit_driver_t *drv);

/* Obtiene geometría del medio para HWV */
mfs_st mfs_l2_8bit_get_geom(const mfs_l2_8bit_driver_t *drv,
                            mfs_media_geom *geom);

/* Auto-detecta tipo de dispositivo SPI (JEDEC ID) */
mfs_st mfs_l2_8bit_probe_spi(const mfs_l2_8bit_cfg_t *cfg,
                             mfs_l2_8bit_type_t *out_type, uint32_t *out_size);

/* ==== Helpers para comandos SPI NOR estándar ==== */
#define MFS_SPI_NOR_CMD_RDID 0x9F       /* Read JEDEC ID */
#define MFS_SPI_NOR_CMD_READ 0x03       /* Read Data (normal) */
#define MFS_SPI_NOR_CMD_FAST_READ 0x0B  /* Fast Read */
#define MFS_SPI_NOR_CMD_PP 0x02         /* Page Program */
#define MFS_SPI_NOR_CMD_SE 0x20         /* Sector Erase (4KB) */
#define MFS_SPI_NOR_CMD_BE32 0x52       /* Block Erase 32KB */
#define MFS_SPI_NOR_CMD_BE64 0xD8       /* Block Erase 64KB */
#define MFS_SPI_NOR_CMD_CHIP_ERASE 0xC7 /* Chip Erase */
#define MFS_SPI_NOR_CMD_WREN 0x06       /* Write Enable */
#define MFS_SPI_NOR_CMD_WRDI 0x04       /* Write Disable */
#define MFS_SPI_NOR_CMD_RDSR 0x05       /* Read Status Register 1 */
#define MFS_SPI_NOR_CMD_RDSR2 0x35      /* Read Status Register 2 */
#define MFS_SPI_NOR_CMD_RDSR3 0x15      /* Read Status Register 3 */
#define MFS_SPI_NOR_CMD_WRSR 0x01       /* Write Status Register */
#define MFS_SPI_NOR_CMD_RDCR 0x15       /* Read Config Register (algunos) */
#define MFS_SPI_NOR_CMD_EN4B 0xB7       /* Enter 4-Byte Address Mode */
#define MFS_SPI_NOR_CMD_EX4B 0xE9       /* Exit 4-Byte Address Mode */

/* Bits de Status Register 1 */
#define MFS_SPI_NOR_SR_WIP 0x01  /* Write In Progress */
#define MFS_SPI_NOR_SR_WEL 0x02  /* Write Enable Latch */
#define MFS_SPI_NOR_SR_BP0 0x04  /* Block Protect 0 */
#define MFS_SPI_NOR_SR_BP1 0x08  /* Block Protect 1 */
#define MFS_SPI_NOR_SR_BP2 0x10  /* Block Protect 2 */
#define MFS_SPI_NOR_SR_TB 0x20   /* Top/Bottom Protect */
#define MFS_SPI_NOR_SR_SEC 0x40  /* Sector Protect */
#define MFS_SPI_NOR_SR_SRP0 0x80 /* Status Register Protect 0 */

/* ==== Helpers para FRAM/EEPROM ==== */
#define MFS_FRAM_CMD_WREN 0x06
#define MFS_FRAM_CMD_WRDI 0x04
#define MFS_FRAM_CMD_RDSR 0x05
#define MFS_FRAM_CMD_WRSR 0x01
#define MFS_FRAM_CMD_READ 0x03
#define MFS_FRAM_CMD_WRITE 0x02
#define MFS_FRAM_CMD_FRD 0x0B /* Fast Read (algunos) */

/* ==== Helpers para SD SPI ==== */
#define MFS_SD_CMD_GO_IDLE_STATE 0
#define MFS_SD_CMD_SEND_IF_COND 8
#define MFS_SD_CMD_SEND_CSD 9
#define MFS_SD_CMD_SEND_CID 10
#define MFS_SD_CMD_STOP_TRANSMISSION 12
#define MFS_SD_CMD_SEND_STATUS 13
#define MFS_SD_CMD_SET_BLOCKLEN 16
#define MFS_SD_CMD_READ_SINGLE_BLOCK 17
#define MFS_SD_CMD_READ_MULTIPLE_BLOCK 18
#define MFS_SD_CMD_WRITE_SINGLE_BLOCK 24
#define MFS_SD_CMD_WRITE_MULTIPLE_BLOCK 25
#define MFS_SD_CMD_APP_CMD 55
#define MFS_SD_ACMD_SD_SEND_OP_COND 41
#define MFS_SD_ACMD_SET_BUS_WIDTH 6

#define MFS_SD_R1_IDLE_STATE 0x01
#define MFS_SD_R1_ILLEGAL_COMMAND 0x04
#define MFS_SD_DATA_TOKEN_SINGLE 0xFE
#define MFS_SD_DATA_TOKEN_MULTIPLE 0xFC
#define MFS_SD_START_BLOCK_TOKEN 0xFE

#ifdef __cplusplus
}
#endif

#endif /* MFS_L2_8BIT_H */