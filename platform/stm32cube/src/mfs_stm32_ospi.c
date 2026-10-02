/* mfs_stm32_ospi.c — NOR externa sobre OSPI (H7/U5/L5) o QUADSPI (F7/L4).
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Es el medio IDEAL para MatrixFS en un STM32: sectores uniformes de 4 KB, sin
 * ECC, sin bloquear la CPU (el acceso es por un periferico, no por la flash
 * interna) y con lectura en sitio.
 *
 * Dos decisiones de diseno que importan:
 *
 *  1) LECTURA EN MODO INDIRECTO (por defecto). La region *memory-mapped* es
 *     comoda, pero en un Cortex-M7 (STM32H7) introduce coherencia de cache:
 * tras programar hay que invalidar la D-cache del rango o se leen datos viejos.
 *     El modo indirecto evita el problema por completo. Si el BSP prefiere el
 *     mapeado, `cfg->memory_mapped = true` y es SU responsabilidad configurar
 * la MPU (non-cacheable o write-through) e invalidar la cache tras programar.
 *
 *  2) PAGE PROGRAM TROCEADO. Una orden PP (0x02) NO puede cruzar la frontera de
 *     pagina del dispositivo: si la cruza, el silicio ENVUELVE dentro de la
 *     pagina y corrompe el dato SIN devolver error. El nucleo pide escribir
 *     hasta `chunk_size` (4096 B) desde direcciones no alineadas, de modo que
 * el troceado es obligatorio (es el defecto S3 corregido en mfs_l2_8bit.c).
 *
 * Las direcciones que llegan del nucleo son ABSOLUTAS en el mapa del MCU
 * (0x90000000 y siguientes en H7); se traducen a offset del dispositivo
 * restando `base_addr`, y la base se declara en el descriptor de la region para
 * que mfs_embedded haga la suma.
 */
#include "mfs_stm32_backend.h"

#include <string.h>

#include "mfs_l2_8bit.h" /* comandos SPI NOR (0x9F, 0x02, 0x20, 0x06, 0x05) */
#include "mfs_stm32_hal.h"

#ifndef MFS_STM32_OSPI_TIMEOUT_MS
#define MFS_STM32_OSPI_TIMEOUT_MS 2000u
#endif

/* Direccion base de la region mapeada, por familia. Se puede sobrescribir. */
#ifndef MFS_STM32_OSPI_MAPPED_BASE
#if defined(STM32H7xx) || defined(STM32U5xx) || defined(STM32L5xx)
#define MFS_STM32_OSPI_MAPPED_BASE 0x90000000u
#else
#define MFS_STM32_OSPI_MAPPED_BASE 0x90000000u
#endif
#endif

/* =====================================================================
 * Estado estatico
 * ===================================================================== */
typedef struct {
  void *h;             /* OSPI_HandleTypeDef* o QSPI_HandleTypeDef* */
  bool is_ospi;        /* true ⇒ OSPI (HAL_OSPI_*), false ⇒ QUADSPI   */
  uint32_t base;       /* direccion base en el mapa del MCU          */
  uint32_t size;       /* bytes del dispositivo                      */
  uint32_t erase_unit; /* 4096 (o el que declare el integrador)      */
  uint32_t page_size;  /* 256 tipico                                 */
  uint8_t addr_bytes;  /* 3 o 4                                      */
  bool memory_mapped;
  bool four_byte_mode; /* el dispositivo esta en modo de 4 bytes      */
} ospi_state_t;

static ospi_state_t s_o;

/* =====================================================================
 * Envoltorios HAL: OSPI y QUADSPI tienen APIs DISTINTAS.
 *
 * La diferencia se aisla aqui en cuatro operaciones: leer ID, leer, programar y
 * borrar. Todo lo demas del backend es comun.
 * ===================================================================== */

static mfs_st ospi_cmd_read_id(void *h, bool is_ospi, uint8_t out[3]) {
#if defined(HAL_OSPI_MODULE_ENABLED) || defined(STM32H7xx) ||                  \
    defined(STM32U5xx) || defined(STM32L5xx)
  if (is_ospi) {
    OSPI_HandleTypeDef *ho = (OSPI_HandleTypeDef *)h;
    OSPI_RegularCmdTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    cmd.FlashId = HAL_OSPI_FLASH_ID_1;
    cmd.Instruction = MFS_SPI_NOR_CMD_RDID;
    cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    cmd.AddressMode = HAL_OSPI_ADDRESS_NONE;
    cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = HAL_OSPI_DATA_1_LINE;
    cmd.NbData = 3u;
    cmd.DummyCycles = 0u;
    cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
    cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    if (HAL_OSPI_Command(ho, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_OSPI_Receive(ho, out, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
#if defined(HAL_QSPI_MODULE_ENABLED) || defined(STM32F7xx) || defined(STM32L4xx)
  {
    QSPI_HandleTypeDef *hq = (QSPI_HandleTypeDef *)h;
    QSPI_CommandTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Instruction = MFS_SPI_NOR_CMD_RDID;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = 3u;
    cmd.DummyCycles = 0u;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    if (HAL_QSPI_Command(hq, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_QSPI_Receive(hq, out, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
  (void)h;
  (void)is_ospi;
  (void)out;
  return MFS_ENOTSUP; /* HAL sin OSPI ni QUADSPI habilitados */
}

/* Lee `len` bytes del offset `off` del dispositivo. */
static mfs_st ospi_read_indirect(uint32_t off, void *dst, uint32_t len) {
  if (len == 0u)
    return MFS_OK;
  if ((uint64_t)off + len > s_o.size)
    return MFS_EINVAL;
#if defined(HAL_OSPI_MODULE_ENABLED) || defined(STM32H7xx) ||                  \
    defined(STM32U5xx) || defined(STM32L5xx)
  if (s_o.is_ospi) {
    OSPI_HandleTypeDef *ho = (OSPI_HandleTypeDef *)s_o.h;
    OSPI_RegularCmdTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    cmd.FlashId = HAL_OSPI_FLASH_ID_1;
    cmd.Instruction = MFS_SPI_NOR_CMD_READ;
    cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    cmd.Address = off;
    cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
    cmd.AddressSize = s_o.four_byte_mode ? HAL_OSPI_ADDRESS_32_BITS
                                         : HAL_OSPI_ADDRESS_24_BITS;
    cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = HAL_OSPI_DATA_1_LINE;
    cmd.NbData = len;
    cmd.DummyCycles = 0u;
    cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
    cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    if (HAL_OSPI_Command(ho, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_OSPI_Receive(ho, dst, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
#if defined(HAL_QSPI_MODULE_ENABLED) || defined(STM32F7xx) || defined(STM32L4xx)
  {
    QSPI_HandleTypeDef *hq = (QSPI_HandleTypeDef *)s_o.h;
    QSPI_CommandTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Instruction = MFS_SPI_NOR_CMD_READ;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Address = off;
    /* STM-05: direccionamiento de 4 bytes (32 bits) para dispositivos > 16 MiB.
     * AddressMode = QSPI_ADDRESS_1_LINE (1 linea de datos para la fase de
     * direccion). AddressSize = QSPI_ADDRESS_32_BITS (4 bytes de direccion).
     * QSPI_ADDRESS_4_LINES es para fase de direccion en 4 lineas paralelas,
     * que NO es lo mismo y no soportan los NOR SPI estandar. */
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize =
        s_o.four_byte_mode ? QSPI_ADDRESS_32_BITS : QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = len;
    cmd.DummyCycles = 0u;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    if (HAL_QSPI_Command(hq, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_QSPI_Receive(hq, dst, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
  (void)off;
  (void)dst;
  return MFS_ENOTSUP;
}

/* Emite una orden SIN datos (WREN, sector erase...). */
static mfs_st ospi_cmd(uint8_t instruction, bool with_addr, uint32_t addr) {
#if defined(HAL_OSPI_MODULE_ENABLED) || defined(STM32H7xx) ||                  \
    defined(STM32U5xx) || defined(STM32L5xx)
  if (s_o.is_ospi) {
    OSPI_HandleTypeDef *ho = (OSPI_HandleTypeDef *)s_o.h;
    OSPI_RegularCmdTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    cmd.FlashId = HAL_OSPI_FLASH_ID_1;
    cmd.Instruction = instruction;
    cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    if (with_addr) {
      cmd.Address = addr;
      cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
      cmd.AddressSize = s_o.four_byte_mode ? HAL_OSPI_ADDRESS_32_BITS
                                           : HAL_OSPI_ADDRESS_24_BITS;
    } else {
      cmd.AddressMode = HAL_OSPI_ADDRESS_NONE;
    }
    cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = HAL_OSPI_DATA_NONE;
    cmd.DummyCycles = 0u;
    cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
    cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    return (HAL_OSPI_Command(ho, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) == HAL_OK)
               ? MFS_OK
               : MFS_EIO;
  }
#endif
#if defined(HAL_QSPI_MODULE_ENABLED) || defined(STM32F7xx) || defined(STM32L4xx)
  {
    QSPI_HandleTypeDef *hq = (QSPI_HandleTypeDef *)s_o.h;
    QSPI_CommandTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Instruction = instruction;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    if (with_addr) {
      cmd.Address = addr;
      /* STM-05: igual que en lectura. */
      cmd.AddressMode = QSPI_ADDRESS_1_LINE;
      cmd.AddressSize =
          s_o.four_byte_mode ? QSPI_ADDRESS_32_BITS : QSPI_ADDRESS_24_BITS;
    } else {
      cmd.AddressMode = QSPI_ADDRESS_NONE;
    }
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_NONE;
    cmd.DummyCycles = 0u;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    return (HAL_QSPI_Command(hq, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) == HAL_OK)
               ? MFS_OK
               : MFS_EIO;
  }
#endif
  (void)instruction;
  (void)with_addr;
  (void)addr;
  return MFS_ENOTSUP;
}

/* Escribe `len` bytes en `off` (ya troceado a la pagina). */
static mfs_st ospi_write_indirect(uint32_t off, const void *src, uint32_t len) {
  if (len == 0u)
    return MFS_OK;
#if defined(HAL_OSPI_MODULE_ENABLED) || defined(STM32H7xx) ||                  \
    defined(STM32U5xx) || defined(STM32L5xx)
  if (s_o.is_ospi) {
    OSPI_HandleTypeDef *ho = (OSPI_HandleTypeDef *)s_o.h;
    OSPI_RegularCmdTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    cmd.FlashId = HAL_OSPI_FLASH_ID_1;
    cmd.Instruction = MFS_SPI_NOR_CMD_PP;
    cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    cmd.Address = off;
    cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
    cmd.AddressSize = s_o.four_byte_mode ? HAL_OSPI_ADDRESS_32_BITS
                                         : HAL_OSPI_ADDRESS_24_BITS;
    cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = HAL_OSPI_DATA_1_LINE;
    cmd.NbData = len;
    cmd.DummyCycles = 0u;
    cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
    cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    if (HAL_OSPI_Command(ho, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_OSPI_Transmit(ho, (uint8_t *)src, MFS_STM32_OSPI_TIMEOUT_MS) !=
        HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
#if defined(HAL_QSPI_MODULE_ENABLED) || defined(STM32F7xx) || defined(STM32L4xx)
  {
    QSPI_HandleTypeDef *hq = (QSPI_HandleTypeDef *)s_o.h;
    QSPI_CommandTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Instruction = MFS_SPI_NOR_CMD_PP;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Address = off;
    /* STM-05: igual que en lectura. */
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize =
        s_o.four_byte_mode ? QSPI_ADDRESS_32_BITS : QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = len;
    cmd.DummyCycles = 0u;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    if (HAL_QSPI_Command(hq, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_QSPI_Transmit(hq, (uint8_t *)src, MFS_STM32_OSPI_TIMEOUT_MS) !=
        HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
  (void)off;
  (void)src;
  return MFS_ENOTSUP;
}

/* Lee el registro de estado 1 (bit 0 = WIP). */
static mfs_st ospi_read_sr(uint8_t *out) {
#if defined(HAL_OSPI_MODULE_ENABLED) || defined(STM32H7xx) ||                  \
    defined(STM32U5xx) || defined(STM32L5xx)
  if (s_o.is_ospi) {
    OSPI_HandleTypeDef *ho = (OSPI_HandleTypeDef *)s_o.h;
    OSPI_RegularCmdTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
    cmd.FlashId = HAL_OSPI_FLASH_ID_1;
    cmd.Instruction = MFS_SPI_NOR_CMD_RDSR;
    cmd.InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
    cmd.InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
    cmd.AddressMode = HAL_OSPI_ADDRESS_NONE;
    cmd.AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = HAL_OSPI_DATA_1_LINE;
    cmd.NbData = 1u;
    cmd.DQSMode = HAL_OSPI_DQS_DISABLE;
    cmd.SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
    if (HAL_OSPI_Command(ho, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_OSPI_Receive(ho, out, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
#if defined(HAL_QSPI_MODULE_ENABLED) || defined(STM32F7xx) || defined(STM32L4xx)
  {
    QSPI_HandleTypeDef *hq = (QSPI_HandleTypeDef *)s_o.h;
    QSPI_CommandTypeDef cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Instruction = MFS_SPI_NOR_CMD_RDSR;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = 1u;
    cmd.DummyCycles = 0u;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    if (HAL_QSPI_Command(hq, &cmd, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    if (HAL_QSPI_Receive(hq, out, MFS_STM32_OSPI_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    return MFS_OK;
  }
#endif
  (void)out;
  return MFS_ENOTSUP;
}

/* Barrera WOB: espera a que WIP baje, con TIMEOUT REAL en microsegundos.
 * Cada iteración hace una transacción RDSR completa; se usa
 * mfs_port_time_us() para medir el tiempo transcurrido y evitar esperas
 * indefinidas. Devuelve MFS_ETIMEDOUT si expira el presupuesto. */
static mfs_st ospi_wait_ready(uint32_t timeout_us) {
  uint32_t start = mfs_port_time_us();
  if (timeout_us == 0u)
    timeout_us = 50000u; /* 50 ms por defecto (program page tipico) */
  for (;;) {
    uint8_t sr = 0xFFu;
    mfs_st st = ospi_read_sr(&sr);
    if (st != MFS_OK)
      return st;
    if ((sr & MFS_SPI_NOR_SR_WIP) == 0u)
      return MFS_OK;
    mfs_port_wfi();
    if (mfs_port_time_us() - start >= timeout_us)
      return MFS_ETIMEDOUT_BUDGET;
  }
}

/* =====================================================================
 * Callbacks de la region plana
 * ===================================================================== */
static mfs_st ospi_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  (void)ctx;
  if (addr < s_o.base)
    return MFS_EINVAL;
  return ospi_read_indirect(addr - s_o.base, dst, len);
}

static mfs_st ospi_prog(void *ctx, uint32_t addr, const void *src,
                        uint32_t len) {
  (void)ctx;
  if (!src || len == 0u)
    return MFS_EINVAL;
  if (addr < s_o.base || (uint64_t)addr + len > (uint64_t)s_o.base + s_o.size)
    return MFS_EINVAL;

  const uint8_t *sp = (const uint8_t *)src;
  uint32_t page = s_o.page_size ? s_o.page_size : 256u;

  /* TROCEADO OBLIGATORIO por frontera de pagina (ver cabecera del fichero). */
  for (uint32_t done = 0u; done < len;) {
    uint32_t off = (addr - s_o.base) + done;
    uint32_t room = page - (off % page);
    uint32_t n = len - done;
    if (n > room)
      n = room;

    mfs_st st = ospi_cmd(MFS_SPI_NOR_CMD_WREN, false, 0u);
    if (st != MFS_OK)
      return st;
    st = ospi_write_indirect(off, sp + done, n);
    if (st != MFS_OK)
      return st;
    /* Timeout de programacion de pagina: tipico 0.4-3 ms, usamos 50 ms. */
    st = ospi_wait_ready(50000u);
    if (st != MFS_OK)
      return st;
    done += n;
  }
  return MFS_OK;
}

static mfs_st ospi_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  if (addr < s_o.base)
    return MFS_EINVAL;
  uint32_t off = addr - s_o.base;
  uint32_t unit = s_o.erase_unit ? s_o.erase_unit : 4096u;
  if ((off % unit) != 0u || (uint64_t)off + unit > s_o.size)
    return MFS_EINVAL;

  mfs_st st;
  if (unit == 4096u) {
    st = ospi_cmd(MFS_SPI_NOR_CMD_WREN, false, 0u);
    if (st == MFS_OK)
      st = ospi_cmd(MFS_SPI_NOR_CMD_SE, true, off);
  } else if (unit == 32768u) {
    st = ospi_cmd(MFS_SPI_NOR_CMD_WREN, false, 0u);
    if (st == MFS_OK)
      st = ospi_cmd(MFS_SPI_NOR_CMD_BE32, true, off);
  } else if (unit == 65536u) {
    st = ospi_cmd(MFS_SPI_NOR_CMD_WREN, false, 0u);
    if (st == MFS_OK)
      st = ospi_cmd(MFS_SPI_NOR_CMD_BE64, true, off);
  } else {
    return MFS_ENOTSUP; /* sin comando estandar para este taman~o */
  }
  if (st != MFS_OK)
    return st;
  /* Timeout de borrado de sector (4 KB): tipico 50-300 ms, usamos 500 ms. */
  return ospi_wait_ready(500000u);
}

/* =====================================================================
 * Preparacion (incluye autodeteccion JEDEC)
 * ===================================================================== */
mfs_st mfs_stm32_ospi_prepare(const mfs_stm32_cfg *cfg,
                              mfs_embedded_flash_t *flash) {
  if (!cfg || !flash || !cfg->ospi)
    return MFS_EINVAL;

  memset(&s_o, 0, sizeof(s_o));
  s_o.h = cfg->ospi;
#if defined(STM32H7xx) || defined(STM32U5xx) || defined(STM32L5xx)
  s_o.is_ospi = true;
#else
  s_o.is_ospi = false;
#endif
  s_o.base = MFS_STM32_OSPI_MAPPED_BASE;
  s_o.page_size = cfg->page_size ? cfg->page_size : 256u;
  s_o.erase_unit = cfg->erase_unit ? cfg->erase_unit : 4096u;
  s_o.memory_mapped = cfg->memory_mapped;
  s_o.addr_bytes = 3u;

  /* Geometria: declarada o autodetectada por JEDEC RDID (0x9F).
   * NUNCA se inventa: si no responde, error tipificado. */
  if (cfg->total_size != 0u) {
    s_o.size = cfg->total_size;
  } else {
    uint8_t id[3] = {0xFFu, 0xFFu, 0xFFu};
    mfs_st st = ospi_cmd_read_id(s_o.h, s_o.is_ospi, id);
    if (st != MFS_OK)
      return st;
    if (id[2] == 0xFFu)
      return MFS_EIO; /* sin dispositivo o linea mal */
    /* Capacidad JEDEC = 2^id[2] bytes. */
    if ((id[2] & 0x1Fu) > 30u)
      return MFS_EINVAL;
    s_o.size = 1u << (id[2] & 0x1Fu);
    /* El byte de tipo distingue NOR de otros; se asume NOR SPI (se comprueba
     * que el tamano sea razonable). */
    if (s_o.size < 4096u)
      return MFS_EINVAL;
  }

  /* Modo de 4 bytes: los dispositivos > 16 MiB lo requieren. El BSP debe haber
   * emitido EN4B (0xB7) en la inicializacion del periferico; aqui se declara
   * para que las ordenes usen direccion de 32 bits. */
  if (s_o.size > 0x1000000u) {
    s_o.addr_bytes = 4u;
    s_o.four_byte_mode = true;
  }

  if (s_o.erase_unit != 4096u && s_o.erase_unit != 32768u &&
      s_o.erase_unit != 65536u)
    return MFS_ENOTSUP;

  memset(flash, 0, sizeof(*flash));
  flash->read = ospi_read;
  flash->prog = ospi_prog;
  flash->erase = ospi_erase;
  flash->ctx = &s_o;
  flash->base_addr = s_o.base;
  flash->size = s_o.size;
  flash->erase_unit = s_o.erase_unit;
  flash->page_size = (uint16_t)s_o.page_size;
  flash->program_granularity = 1u; /* NOR: programable byte a byte */
  flash->no_erase = false;
  if (cfg->t_prog_max_us)
    flash->t_prog_max_us = cfg->t_prog_max_us;
  if (cfg->t_erase_max_us)
    flash->t_erase_max_us = cfg->t_erase_max_us;
  if (cfg->t_read_max_us)
    flash->t_read_max_us = cfg->t_read_max_us;
  /* Suspend/resume disponibles en la mayoria de NOR; se declaran solo si el BSP
   * los cablea. Como el driver no los aporta (base.suspend = NULL), el nucleo
   * los retira el mismo (src/core/mfs_hal.c). */
  flash->flags0_extra = 0u;
  return MFS_OK;
}
