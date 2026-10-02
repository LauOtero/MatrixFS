/* mfs_stm32_mem.c — NOR SPI, FRAM y EEPROM (SPI o I2C) sobre HAL.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reutiliza el driver de dispositivo de platform/common/mfs_l2_8bit.c, que YA
 * implementa los comandos JEDEC (RDID 0x9F), el *write enable*, la espera de
 * WIP (barrera WOB), el troceado por pagina de NOR/EEPROM y las variantes I2C.
 * Este fichero solo aporta el BUS: los callbacks SPI/I2C sobre el HAL.
 *
 * Reparto de responsabilidades (MFS-ARCH-010): el nucleo no habla con el
 * controlador; el driver L2 no habla con el HAL; este fichero no conoce el
 * sistema de archivos.
 *
 * CHIP SELECT: al ser especifico de la placa, se declara como simbolo DEBIL
 * (__attribute__((weak))). Si la aplicacion define
 * `mfs_stm32_spi_cs_low()` / `mfs_stm32_spi_cs_high()`, se usan esas; si no,
 * se asume que el periferico SPI tiene el NSS en modo hardware.
 */
#include "mfs_stm32_backend.h"

#include <string.h>

#include "mfs_l2_8bit.h"
#include "mfs_stm32_hal.h"

/* Timeout de las transacciones HAL, en milisegundos. Acotado y deterministico:
 * el contrato del puerto exige que `prog` retorne solo tras confirmar el estado
 * (barrera WOB), no que espere indefinidamente. */
#ifndef MFS_STM32_BUS_TIMEOUT_MS
#define MFS_STM32_BUS_TIMEOUT_MS 1000u
#endif

/* =====================================================================
 * Chip select (sobreescribible por la aplicacion)
 * ===================================================================== */
__attribute__((weak)) void mfs_stm32_spi_cs_low(void *ctx) { (void)ctx; }
__attribute__((weak)) void mfs_stm32_spi_cs_high(void *ctx) { (void)ctx; }

/* =====================================================================
 * Callbacks de bus SPI
 * ===================================================================== */
static mfs_st stm32_spi_transfer(void *ctx, const uint8_t *tx, uint8_t *rx,
                                 uint32_t len) {
  SPI_HandleTypeDef *h = (SPI_HandleTypeDef *)ctx;
  if (!h || len == 0u)
    return MFS_EINVAL;
  /* HAL usa uint16_t para el tamano: se trocea respetando ese limite en vez de
   * truncar en silencio (era el defecto S6 de mfs_l2_8bit.c). */
  uint32_t done = 0u;
  while (done < len) {
    uint32_t n = len - done;
    if (n > 0xFFFFu)
      n = 0xFFFFu;
    HAL_StatusTypeDef r;
    if (tx && rx)
      r = HAL_SPI_TransmitReceive(h, (uint8_t *)(tx + done), rx + done,
                                  (uint16_t)n, MFS_STM32_BUS_TIMEOUT_MS);
    else if (tx)
      r = HAL_SPI_Transmit(h, (uint8_t *)(tx + done), (uint16_t)n,
                           MFS_STM32_BUS_TIMEOUT_MS);
    else
      r = HAL_SPI_Receive(h, rx + done, (uint16_t)n, MFS_STM32_BUS_TIMEOUT_MS);
    if (r != HAL_OK)
      return MFS_EIO;
    done += n;
  }
  return MFS_OK;
}

static mfs_st stm32_spi_cs_low(void *ctx) {
  mfs_stm32_spi_cs_low(ctx);
  return MFS_OK;
}

static mfs_st stm32_spi_cs_high(void *ctx) {
  mfs_stm32_spi_cs_high(ctx);
  return MFS_OK;
}

/* =====================================================================
 * Callbacks de memoria I2C (FRAM/EEPROM)
 *
 * Se usan las operaciones de MEMORIA del HAL (HAL_I2C_Mem_Read/Write), que ya
 * resuelven el envio de la word address y el repeated-START. HAL admite word
 * address de 8 o 16 bits; por encima de 64 KiB no hay soporte estandar, de modo
 * que se devuelve MFS_ENOTSUP en vez de direccionar mal.
 * ===================================================================== */
static mfs_st stm32_i2c_mem_read(void *ctx, uint8_t dev, uint32_t mem_addr,
                                 uint16_t mem_addr_bytes, uint8_t *dst,
                                 uint32_t len) {
  I2C_HandleTypeDef *h = (I2C_HandleTypeDef *)ctx;
  if (!h || !dst || len == 0u)
    return MFS_EINVAL;
  if (mem_addr_bytes > 2u)
    return MFS_ENOTSUP;
  uint16_t msz =
      (mem_addr_bytes == 1u) ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT;
  uint32_t done = 0u;
  while (done < len) {
    uint32_t n = len - done;
    if (n > 0xFFFFu)
      n = 0xFFFFu;
    /* La direccion de 7 bits se desplaza: el HAL usa la forma de 8 bits. */
    if (HAL_I2C_Mem_Read(h, (uint16_t)(dev << 1), (uint16_t)(mem_addr + done),
                         msz, dst + done, (uint16_t)n,
                         MFS_STM32_BUS_TIMEOUT_MS) != HAL_OK)
      return MFS_EIO;
    done += n;
  }
  return MFS_OK;
}

/* Acknowledge polling de EEPROM: tras una escritura, el dispositivo no responde
 * hasta que su ciclo interno de grabacion (~5 ms tipicos) termina. El contrato
 * §20.3 exige que `prog` retorne SOLO tras confirmar el estado, asi que hay que
 * esperar aqui. Se marca el medio en `prepare`. */
static bool s_is_eeprom = false;

static mfs_st stm32_i2c_mem_write(void *ctx, uint8_t dev, uint32_t mem_addr,
                                  uint16_t mem_addr_bytes, const uint8_t *src,
                                  uint32_t len) {
  I2C_HandleTypeDef *h = (I2C_HandleTypeDef *)ctx;
  if (!h || !src || len == 0u)
    return MFS_EINVAL;
  if (mem_addr_bytes > 2u)
    return MFS_ENOTSUP;
  uint16_t msz =
      (mem_addr_bytes == 1u) ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT;
  /* El troceado por pagina de escritura de la EEPROM lo hace mfs_l2_8bit.c
   * antes de llamar aqui, de modo que una sola operacion no debe cruzarla. */
  if (HAL_I2C_Mem_Write(h, (uint16_t)(dev << 1), (uint16_t)mem_addr, msz,
                        (uint8_t *)src, (uint16_t)len,
                        MFS_STM32_BUS_TIMEOUT_MS) != HAL_OK)
    return MFS_EIO;

  if (!s_is_eeprom)
    return MFS_OK; /* la FRAM ya confirmo con el ACK de la propia transaccion */

  /* Acknowledge polling (STM-11): se sondea el dispositivo hasta que responda,
   * cediendo la CPU con mfs_port_wfi() entre intentos (no girar en vacio).
   * 32 intentos x ~1 ms > t_WR tipico (5 ms). */
  for (uint32_t i = 0; i < 32u; i++) {
    if (HAL_I2C_IsDeviceReady(h, (uint16_t)(dev << 1), 1u, 1u) == HAL_OK)
      return MFS_OK;
    mfs_port_wfi();
  }
  return MFS_ETIMEDOUT_BUDGET;
}

/* =====================================================================
 * Preparacion
 * ===================================================================== */
static mfs_l2_8bit_driver_t *s_drv = NULL;
static mfs_media_geom s_geom;

mfs_st mfs_stm32_mem_prepare(const mfs_stm32_cfg *cfg, mfs_stm32_media_t media,
                             const mfs_l2_driver **out_l2,
                             const mfs_media_geom **out_geom) {
  if (!cfg || !out_l2 || !out_geom)
    return MFS_EINVAL;
  if (media != MFS_STM32_MEDIA_SPI_NOR && media != MFS_STM32_MEDIA_FRAM &&
      media != MFS_STM32_MEDIA_EEPROM)
    return MFS_EINVAL;

  const bool fram = (media == MFS_STM32_MEDIA_FRAM);
  const bool eeprom = (media == MFS_STM32_MEDIA_EEPROM);
  const bool i2c = (cfg->bus == MFS_STM32_BUS_I2C);

  mfs_l2_8bit_cfg_t c;
  memset(&c, 0, sizeof(c));
  s_is_eeprom = eeprom;

  /* Tipo y bus */
  if (i2c) {
    if (!cfg->i2c)
      return MFS_EINVAL;
    c.type = fram ? MFS_L2_8BIT_I2C_FRAM : MFS_L2_8BIT_I2C_EEPROM;
    c.i2c_mem_read = stm32_i2c_mem_read;
    c.i2c_mem_write = stm32_i2c_mem_write;
    c.i2c_addr = cfg->i2c_addr;
    c.ctx = cfg->i2c;
  } else {
    if (!cfg->spi)
      return MFS_EINVAL;
    c.type = fram ? MFS_L2_8BIT_SPI_FRAM
                  : (eeprom ? MFS_L2_8BIT_SPI_EEPROM : MFS_L2_8BIT_SPI_NOR);
    c.spi_transfer = stm32_spi_transfer;
    c.spi_cs_low = stm32_spi_cs_low;
    c.spi_cs_high = stm32_spi_cs_high;
    c.ctx = cfg->spi;
  }
  c.pgm_gran = 1u; /* NOR, FRAM y EEPROM son byte-programables */

  /* Geometria: en FRAM/EEPROM es OBLIGATORIA (no responden a RDID con una
   * capacidad JEDEC fiable). En NOR SPI puede autodetectarse. */
  c.total_size = cfg->total_size;
  if (fram) {
    /* La FRAM no tiene borrado ni pagina de escritura. */
    c.erase_size = 0u;
    c.page_size = cfg->page_size ? cfg->page_size : 1u;
    c.t_prog_max_us = cfg->t_prog_max_us ? cfg->t_prog_max_us : 100u;
    c.t_erase_max_us = 0u;
    c.t_read_max_us = cfg->t_read_max_us ? cfg->t_read_max_us : 20u;
  } else if (eeprom) {
    /* EEPROM: SIN borrado, pero CON pagina de escritura (32/64 B tipicos). El
     * driver trocea por esa pagina y respeta la barrera WOB. */
    c.erase_size = 0u;
    c.page_size = cfg->page_size ? cfg->page_size : 32u;
    c.t_prog_max_us = cfg->t_prog_max_us ? cfg->t_prog_max_us : 5000u;
    c.t_erase_max_us = 0u;
    c.t_read_max_us =
        cfg->t_read_max_us ? cfg->t_read_max_us : (i2c ? 500u : 100u);
  } else {
    c.erase_size = cfg->erase_unit ? cfg->erase_unit : 4096u;
    c.page_size = cfg->page_size ? cfg->page_size : 256u;
    c.t_prog_max_us = cfg->t_prog_max_us ? cfg->t_prog_max_us : 700u;
    c.t_erase_max_us = cfg->t_erase_max_us ? cfg->t_erase_max_us : 45000u;
    c.t_read_max_us = cfg->t_read_max_us ? cfg->t_read_max_us : 100u;
  }
  /* Direccionamiento SPI según densidad: 16 bits ≤ 64 KiB y 24 bits ≤ 16 MiB.
   * Antes era `>16 MiB ? 3 : 2`, de modo que TODO dispositivo de 64 KiB–16 MiB
   * (lo habitual) se direccionaba con 2 bytes y la dirección se TRUNCABA. Por
   * encima de 16 MiB el driver rechaza (MFS_ENOTSUP) en vez de direccionar mal:
   * el modo de 4 bytes + EN4B no está implementado todavía. */
  c.addr_bytes = (c.total_size <= 0x10000u) ? 2u : 3u;
  c.addr_width = (c.addr_bytes == 2u) ? 16u : 24u;
  c.ecc_on_die = false;

  mfs_l2_8bit_driver_t *d = NULL;
  mfs_st st = mfs_l2_8bit_create(&c, &d);
  if (st != MFS_OK)
    return st;
  s_drv = d;

  st = mfs_l2_8bit_get_geom(s_drv, &s_geom);
  if (st != MFS_OK)
    return st;

  *out_l2 = &s_drv->base;
  *out_geom = &s_geom;
  return MFS_OK;
}
