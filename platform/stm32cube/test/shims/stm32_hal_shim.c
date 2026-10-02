/* stm32_hal_shim.c — modelo de la flash interna y stubs del HAL para host.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * El modelo de flash NO es un simulador complaciente: reproduce las tres
 * propiedades del silicio que hacen dificil el backend de la flash interna.
 *
 *   1) UNIDAD DE PROGRAMACION. Toda programacion es de
 *      `MFS_STM32_HOST_PGM_UNIT` bytes (2/4/8/16) y debe estar alineada a esa
 *      unidad; si no, error. La unidad del F4 es 4 B (word), la del L4/G4 8 B
 *      (doubleword) y la del H7/H5/U5 16 B (quadword).
 *   2) ECC. Con MFS_STM32_HOST_ECC=1, programar una unidad que YA tiene algun
 *      byte distinto de 0xFF devuelve HAL_ERROR (modela PROGERR) y marca la
 *      bandera `shim_flash_was_double_programmed()`. Es exactamente la
 *      restriccion que obliga al backend a decidir entre "no-op" y MFS_EIO en
 *      vez de reescribir la unidad.
 *   3) SECTORES NO UNIFORMES. El borrado localiza el sector que contiene la
 *      direccion en la tabla tipo STM32F4 (16/64/128 KB) y exige que el tamano
 *      declarado coincida con el real; si no, error. Un backend que "suponga"
 *      uniformidad falla de inmediato en vez de borrar el sector equivocado.
 *
 * ADEMAS: la imagen se mapea en 0x08000000 (VirtualAlloc) cuando el host lo
 * permite, porque una revision del backend (mfs_stm32_iflash.c) leia la flash
 * con memcpy desde la direccion absoluta (XIP). La revision actual lee a traves
 * de mfs_stm32_hal_flash_read(), de modo que funciona con cualquiera de las
 * dos; si el mapeo falla se usa un array estatico (shim_flash_at_flash_base()).
 *
 * El resto de perifericos (OSPI/QSPI/SPI/I2C/SD) son stubs que devuelven
 * HAL_ERROR: aqui solo se comprueba que el codigo del port COMPILA contra la
 * forma del HAL, no que el periferico funcione.
 */
#include "stm32_hal_shim.h"

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* =====================================================================
 * Reloj monotonico
 * ===================================================================== */
uint32_t HAL_GetTick(void) {
#if defined(_WIN32)
  /* Milisegundos reales desde el arranque del sistema (monotonico). */
  return (uint32_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                    (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

void HAL_Delay(uint32_t ms) { (void)ms; }

/* =====================================================================
 * CMSIS minimo
 * ===================================================================== */
static shim_coredebug_t s_coredebug;
static shim_dwt_t s_dwt;
shim_coredebug_t *const CoreDebug = &s_coredebug;
shim_dwt_t *const DWT = &s_dwt;

/* SysTick del modelo: LOAD = 0 (BSS) para que mfs_port_time_us() (STM-13) caiga
 * al fallback HAL_GetTick()*1000 en el host. */
static shim_systick_t s_systick;
shim_systick_t *const SysTick = &s_systick;

/* Solo se usa en la rama DWT (compilacion con -D__CORTEX_M>=3). */
uint32_t SystemCoreClock = 16000000u;

static uint32_t s_primask = 0u;
uint32_t __get_PRIMASK(void) { return s_primask; }
void __disable_irq(void) { s_primask = 1u; }
void __enable_irq(void) { s_primask = 0u; }
void __WFI(void) { s_dwt.CYCCNT++; }
void __NOP(void) {}

/* =====================================================================
 * Imagen de la flash
 *
 * Se intenta mapear en la direccion REAL del dispositivo (0x08000000) para que
 * el modelo sirva tanto si el backend lee por la capa HAL como si leyera con un
 * puntero directo (XIP). Si el host no concede esa direccion, se cae a un array
 * estatico: el backend actual (mfs_stm32_hal_flash_read) no depende de ella.
 * ===================================================================== */
static uint8_t s_flash_fallback[SHIM_FLASH_SIZE];
static uint8_t *s_flash = s_flash_fallback;
static bool s_at_flash_base = false;

/* Contadores/estado del modelo. */
static uint32_t s_prog_calls = 0u;
static uint32_t s_erase_calls = 0u;
static bool s_double_programmed = false;
static uint32_t s_unlock_depth = 0u; /* controlador: 0 ⇒ BLOQUEADO */
static uint32_t s_prog_while_locked = 0u;

/* Geometria ACTIVA del modelo (por defecto, la tipo F4 de shim_sectors). */
static const mfs_sector_t *s_active = NULL;
static uint32_t s_active_n = 0u;

static void shim_active_default(void) {
  if (!s_active) {
    s_active = shim_sectors;
    s_active_n = shim_sector_count;
  }
}

void shim_flash_set_sectors(const mfs_sector_t *s, uint32_t n) {
  s_active = (s && n) ? s : shim_sectors;
  s_active_n = (s && n) ? n : shim_sector_count;
}

void shim_flash_reset(void) {
#if defined(_WIN32)
  if (!s_at_flash_base) {
    void *p = VirtualAlloc((void *)(uintptr_t)SHIM_FLASH_BASE, SHIM_FLASH_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)SHIM_FLASH_BASE) {
      s_flash = (uint8_t *)p;
      s_at_flash_base = true;
    } else if (p) {
      (void)VirtualFree(p, 0, MEM_RELEASE);
    }
  }
#endif
  memset(s_flash, 0xFF, SHIM_FLASH_SIZE);
  /* La tabla por defecto vuelve a ser la activa. */
  s_active = NULL;
  s_active_n = 0u;
  shim_active_default();
  s_prog_calls = 0u;
  s_erase_calls = 0u;
  s_double_programmed = false;
  s_prog_while_locked = 0u;
  s_unlock_depth = 0u; /* estado de reset: FLASH_CR.LOCK = 1 */
}

uint8_t *shim_flash_raw(void) { return s_flash; }
uint32_t shim_flash_size(void) { return SHIM_FLASH_SIZE; }
bool shim_flash_ready(void) { return s_flash != NULL; }
bool shim_flash_at_flash_base(void) { return s_at_flash_base; }
bool shim_flash_was_double_programmed(void) { return s_double_programmed; }
uint32_t shim_flash_program_count(void) { return s_prog_calls; }
uint32_t shim_flash_erase_count(void) { return s_erase_calls; }
uint32_t shim_flash_program_while_locked_count(void) {
  return s_prog_while_locked;
}

/* =====================================================================
 * Geometria del dispositivo modelado
 * ===================================================================== */
const mfs_sector_t shim_sectors[] = {
    /* 4 x 16 KB */
    {0x08000000u, 16u * 1024u},
    {0x08004000u, 16u * 1024u},
    {0x08008000u, 16u * 1024u},
    {0x0800C000u, 16u * 1024u},
    /* 1 x 64 KB */
    {0x08010000u, 64u * 1024u},
    /* 7 x 128 KB (un F4 de 1 MB de un solo banco tiene SIETE, no tres) */
    {0x08020000u, 128u * 1024u},
    {0x08040000u, 128u * 1024u},
    {0x08060000u, 128u * 1024u},
    {0x08080000u, 128u * 1024u},
    {0x080A0000u, 128u * 1024u},
    {0x080C0000u, 128u * 1024u},
    {0x080E0000u, 128u * 1024u},
};
const uint32_t shim_sector_count =
    (uint32_t)(sizeof(shim_sectors) / sizeof(shim_sectors[0]));

/* =====================================================================
 * Reset de banderas/contadores del modelo
 * ===================================================================== */
void shim_flash_clear_flags(void) {
  s_double_programmed = false;
  s_prog_calls = 0u;
  s_erase_calls = 0u;
  s_prog_while_locked = 0u;
}

/* =====================================================================
 * Modelo PURO del HAL (lo que haria el silicio)
 * ===================================================================== */
static int shim_sector_of(uint32_t addr) {
  shim_active_default();
  for (uint32_t i = 0; i < s_active_n; i++) {
    if (addr >= s_active[i].addr && addr < s_active[i].addr + s_active[i].size)
      return (int)i;
  }
  return -1;
}

static inline uint32_t shim_off(uint32_t addr) {
  return addr - SHIM_FLASH_BASE;
}

static bool shim_range_ok(uint32_t addr, uint32_t len) {
  return addr >= SHIM_FLASH_BASE &&
         (uint64_t)addr + len <= (uint64_t)SHIM_FLASH_BASE + SHIM_FLASH_SIZE;
}

/* Programa `unit` bytes (1..16) de `bytes` en `addr` con la semantica del
 * dispositivo: alineacion obligatoria; con ECC, PROGERR si la unidad no esta
 * virgen; sin ECC, semantica NOR (los bits solo van de 1 a 0). */
static HAL_StatusTypeDef shim_model_program(uint32_t addr, const uint8_t *bytes,
                                            uint32_t unit) {
  if (!bytes || unit == 0u || unit > 16u)
    return HAL_ERROR;
  if ((addr % unit) != 0u || !shim_range_ok(addr, unit))
    return HAL_ERROR;
  if (!s_flash)
    return HAL_ERROR;

  if (s_unlock_depth == 0u) {
    /* El port programa SIN desbloquear el controlador. En silicio eso hace que
     * el bit PG no se fije y la escritura se PIERDA EN SILENCIO (el HAL
     * devuelve HAL_OK porque FLASH_SR no marca error). El modelo NO emula esa
     * perdida silenciosa (haría fallar TODOS los tests por un motivo ajeno a la
     * logica de ECC/unidad que se quiere probar): programa igualmente, pero
     * CUENTA la vez para que el test pueda aportar la evidencia. */
    s_prog_while_locked++;
  }

  uint32_t off = shim_off(addr);
  s_prog_calls++;

  bool all_ff = true;
  for (uint32_t i = 0; i < unit; i++) {
    if (s_flash[off + i] != 0xFFu) {
      all_ff = false;
      break;
    }
  }

  if (!all_ff && MFS_STM32_HOST_ECC) {
    /* Con ECC solo se acepta reprogramar si el dato pedido YA ESTA contenido
     * (no-op); en cualquier otro caso, PROGERR. */
    bool contained = true;
    for (uint32_t i = 0; i < unit; i++) {
      if ((s_flash[off + i] & bytes[i]) != bytes[i]) {
        contained = false;
        break;
      }
    }
    s_double_programmed = true;
    return contained ? HAL_OK : HAL_ERROR;
  }

  /* NOR: los bits solo van de 1 a 0. */
  for (uint32_t i = 0; i < unit; i++)
    s_flash[off + i] &= bytes[i];
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void) {
  s_unlock_depth++;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void) {
  if (s_unlock_depth)
    s_unlock_depth--;
  return HAL_OK;
}

static uint32_t shim_type_size(uint32_t type) {
  switch (type) {
  case FLASH_TYPEPROGRAM_HALFWORD:
    return 2u;
  case FLASH_TYPEPROGRAM_WORD:
    return 4u;
  case FLASH_TYPEPROGRAM_DOUBLEWORD:
    return 8u;
  case FLASH_TYPEPROGRAM_QUADWORD:
    return 16u;
  default:
    return 0u;
  }
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address,
                                    uint64_t Data) {
  uint32_t unit = shim_type_size(TypeProgram);
  if (unit == 0u)
    return HAL_ERROR;

  uint8_t b[16];
  memset(b, 0xFF, sizeof(b));
  memcpy(b, &Data, 8u); /* la firma solo lleva 64 bits */

  /* HAL_FLASH_Program lleva la unidad completa en `Data`, asi que por ESTA via
   * el modelo solo puede recibir 8 B. La unidad de 16 B (H5/H7/U5) NO se
   * programa por aqui: el port la delega en
   * mfs_stm32_hal_flash_program_quadword(), que si recibe el puntero. Si
   * alguien pidiera QUADWORD por esta via, se avisa en vez de escribir media
   * unidad con datos inventados. */
  if (unit > 8u)
    return HAL_ERROR;
  return shim_model_program(Address, b, unit);
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit,
                                    uint32_t *SectorError) {
  if (!pEraseInit || !s_flash)
    return HAL_ERROR;
  shim_active_default();
  if (SectorError)
    *SectorError = 0xFFFFFFFFu;

  uint32_t first;
  uint32_t count;
  if (pEraseInit->TypeErase == FLASH_TYPEERASE_SECTORS) {
    first = pEraseInit->Sector; /* indice de sector, como el HAL real */
    count = pEraseInit->NbSectors ? pEraseInit->NbSectors : 1u;
  } else if (pEraseInit->TypeErase == FLASH_TYPEERASE_PAGES) {
    first = pEraseInit->Page; /* indice de pagina, como el HAL real */
    count = pEraseInit->NbPages ? pEraseInit->NbPages : 1u;
  } else {
    return HAL_ERROR; /* mass erase: no lo usa el port */
  }

  if (count == 0u || first >= s_active_n || first + count > s_active_n)
    return HAL_ERROR;

  /* El HAL identifica el sector por INDICE. Aqui se traduce indice →
   * direccion/tamano con la tabla del dispositivo, que es lo que hace el
   * silicio. (El numero de sector que calcula el port en destino,
   * mfs_stm32_hal.c:169 `(addr - base) / size`, es correcto solo con sectores
   * uniformes; ver el informe.) */
  for (uint32_t k = 0; k < count; k++) {
    const mfs_sector_t *s = &s_active[first + k];
    memset(&s_flash[shim_off(s->addr)], 0xFF, s->size);
    s_erase_calls++;
  }
  return HAL_OK;
}

/* =====================================================================
 * Adaptacion del port (en destino vive en mfs_stm32_hal.c)
 * ===================================================================== */
uint32_t mfs_stm32_hal_flash_size(void) { return SHIM_FLASH_SIZE; }

mfs_st mfs_stm32_hal_flash_unlock(void) {
  return (HAL_FLASH_Unlock() == HAL_OK) ? MFS_OK : MFS_EIO;
}

mfs_st mfs_stm32_hal_flash_lock(void) {
  return (HAL_FLASH_Lock() == HAL_OK) ? MFS_OK : MFS_EIO;
}

mfs_st mfs_stm32_hal_flash_program(uint32_t addr, uint64_t data,
                                   uint32_t unit) {
  /* Punto de entrada de 64 bits (firma de mfs_stm32_hal.h en las revisiones del
   * port anteriores al cambio a `_program_bytes`). Se mantiene para que el shim
   * siga sirviendo con cualquiera de las dos superficies; el port ACTUAL usa
   * mfs_stm32_hal_flash_program_bytes() y, para 16 B, el gancho de quadword. */
  if (unit == 0u || unit > 16u)
    return MFS_EINVAL;
  if ((addr % unit) != 0u || !shim_range_ok(addr, unit))
    return MFS_EINVAL;

  /* Mismo mapeo unidad → TypeProgram que hace el port en
   * mfs_stm32_hal_flash_program_bytes(). */
  uint32_t type;
  switch (unit) {
  case 2u:
    type = FLASH_TYPEPROGRAM_HALFWORD;
    break;
  case 4u:
    type = FLASH_TYPEPROGRAM_WORD;
    break;
  case 8u:
    type = FLASH_TYPEPROGRAM_DOUBLEWORD;
    break;
  default:
    type = FLASH_TYPEPROGRAM_QUADWORD;
    break;
  }
  return (HAL_FLASH_Program(type, addr, data) == HAL_OK) ? MFS_OK : MFS_EIO;
}

mfs_st mfs_stm32_hal_flash_read(uint32_t addr, void *dst, uint32_t len) {
  if (!dst || len == 0u || !s_flash)
    return MFS_EINVAL;
  if (!shim_range_ok(addr, len))
    return MFS_EINVAL;
  memcpy(dst, &s_flash[shim_off(addr)], len);
  return MFS_OK;
}

mfs_st mfs_stm32_hal_flash_program_bytes(uint32_t addr, const uint8_t *bytes,
                                         uint32_t unit) {
  if (!bytes)
    return MFS_EINVAL;
  /* Mismas unidades que tipos tiene HAL_FLASH_Program (HALFWORD/WORD/
   * DOUBLEWORD/QUADWORD): cualquier otra cosa es ENOTSUP, nunca truncar. */
  if (unit != 2u && unit != 4u && unit != 8u && unit != 16u)
    return MFS_ENOTSUP;
  if ((addr % unit) != 0u || !shim_range_ok(addr, unit))
    return MFS_EINVAL;
  return (shim_model_program(addr, bytes, unit) == HAL_OK) ? MFS_OK : MFS_EIO;
}

mfs_st mfs_stm32_hal_flash_program_quadword(uint32_t addr,
                                            const uint8_t bytes[16]) {
  if (!bytes)
    return MFS_EINVAL;
  if ((addr % 16u) != 0u || !shim_range_ok(addr, 16u))
    return MFS_EINVAL;
  return (shim_model_program(addr, bytes, 16u) == HAL_OK) ? MFS_OK : MFS_EIO;
}

/* STM-04: programacion en bloque con UN SOLO ciclo unlock/program/lock. Cada
 * unidad se programa con la semantica del dispositivo (alineacion, ECC); el
 * modelo lo ejercita igual que la implementacion real, de modo que un bloque de
 * varias unidades (p. ej. 4 quadwords en H5/H7/U5) se verifica de verdad. */
mfs_st mfs_stm32_hal_flash_program_block(uint32_t addr, const uint8_t *bytes,
                                         uint32_t len, uint32_t unit) {
  if (!bytes || len == 0u || unit == 0u || unit > 16u)
    return MFS_EINVAL;
  if ((addr % unit) != 0u || (len % unit) != 0u)
    return MFS_EINVAL;
  if (!shim_range_ok(addr, len))
    return MFS_EINVAL;
  if (mfs_stm32_hal_flash_unlock() != MFS_OK)
    return MFS_EIO;
  for (uint32_t off = 0u; off < len; off += unit) {
    if (shim_model_program(addr + off, bytes + off, unit) != HAL_OK) {
      (void)mfs_stm32_hal_flash_lock();
      return MFS_EIO;
    }
  }
  return (mfs_stm32_hal_flash_lock() == MFS_OK) ? MFS_OK : MFS_EIO;
}

/* El modelo ya resuelve direccion -> sector con su propia tabla, de modo que el
 * aviso del setter es informativo: se guarda para poder comprobarlo. */
static const mfs_sector_t *s_hal_map = NULL;
static uint32_t s_hal_map_n = 0u;

void mfs_stm32_hal_flash_set_sector_map(const mfs_sector_t *map, uint32_t n) {
  s_hal_map = map;
  s_hal_map_n = n;
}

const mfs_sector_t *shim_hal_sector_map(uint32_t *n) {
  if (n)
    *n = s_hal_map_n;
  return s_hal_map;
}

mfs_st mfs_stm32_hal_flash_erase_sector(uint32_t addr, uint32_t size) {
  /* Espejo de mfs_stm32_hal_flash_erase_sector() (rama de SECTORES, la del F4):
   * el wrapper desbloquea, borra UN sector y vuelve a bloquear. La diferencia
   * es que aqui el sector se localiza por DIRECCION + TAMANO en la tabla (que
   * es la informacion de la que dispone el backend), no con `(addr-base)/size`.
   */
  int i = shim_sector_of(addr);
  if (i < 0)
    return MFS_EINVAL;
  if (size == 0u || size != s_active[i].size)
    return MFS_EINVAL; /* tamano declarado != tamano real del sector */
  if (s_active[i].addr != addr)
    return MFS_EINVAL; /* el borrado debe empezar en frontera de sector */

  if (mfs_stm32_hal_flash_unlock() != MFS_OK)
    return MFS_EIO;

  FLASH_EraseInitTypeDef ei;
  memset(&ei, 0, sizeof(ei));
  ei.TypeErase = FLASH_TYPEERASE_SECTORS;
  ei.Sector = (uint32_t)i;
  ei.NbSectors = 1u;
  ei.Banks = FLASH_BANK_BOTH;
  ei.VoltageRange = FLASH_VOLTAGE_RANGE_3;

  uint32_t sector_error = 0u;
  if (HAL_FLASHEx_Erase(&ei, &sector_error) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }
  return (mfs_stm32_hal_flash_lock() == MFS_OK) ? MFS_OK : MFS_EIO;
}

bool mfs_stm32_hal_flash_single_bank(void) { return true; }

/* =====================================================================
 * Stubs de los demas perifericos
 *
 * Solo existen para que mfs_stm32_ospi.c / mfs_stm32_mem.c / mfs_stm32_sd.c
 * COMPILEN si se anaden a la linea de comandos. En este test NO se compilan:
 * sus `*_prepare` se sustituyen por stubs de enlace (test/stubs_backends.c).
 * ===================================================================== */
HAL_StatusTypeDef HAL_OSPI_Command(OSPI_HandleTypeDef *h,
                                   OSPI_RegularCmdTypeDef *cmd, uint32_t t) {
  (void)h;
  (void)cmd;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_OSPI_Receive(OSPI_HandleTypeDef *h, uint8_t *d,
                                   uint32_t t) {
  (void)h;
  (void)d;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_OSPI_Transmit(OSPI_HandleTypeDef *h, uint8_t *d,
                                    uint32_t t) {
  (void)h;
  (void)d;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_QSPI_Command(QSPI_HandleTypeDef *h,
                                   QSPI_CommandTypeDef *cmd, uint32_t t) {
  (void)h;
  (void)cmd;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_QSPI_Receive(QSPI_HandleTypeDef *h, uint8_t *d,
                                   uint32_t t) {
  (void)h;
  (void)d;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_QSPI_Transmit(QSPI_HandleTypeDef *h, uint8_t *d,
                                    uint32_t t) {
  (void)h;
  (void)d;
  (void)t;
  return HAL_ERROR;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n,
                                   uint32_t t) {
  (void)h;
  (void)d;
  (void)n;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *d, uint16_t n,
                                  uint32_t t) {
  (void)h;
  (void)d;
  (void)n;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx,
                                          uint8_t *rx, uint16_t n, uint32_t t) {
  (void)h;
  (void)tx;
  (void)rx;
  (void)n;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t dev,
                                   uint16_t mem, uint16_t msz, uint8_t *dst,
                                   uint16_t len, uint32_t t) {
  (void)h;
  (void)dev;
  (void)mem;
  (void)msz;
  (void)dst;
  (void)len;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t dev,
                                    uint16_t mem, uint16_t msz, uint8_t *src,
                                    uint16_t len, uint32_t t) {
  (void)h;
  (void)dev;
  (void)mem;
  (void)msz;
  (void)src;
  (void)len;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t dev,
                                        uint32_t trials, uint32_t t) {
  (void)h;
  (void)dev;
  (void)trials;
  (void)t;
  return HAL_ERROR;
}

HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef *h, uint8_t *dst,
                                    uint32_t lba, uint32_t n, uint32_t t) {
  (void)h;
  (void)dst;
  (void)lba;
  (void)n;
  (void)t;
  return HAL_ERROR;
}
HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef *h, const uint8_t *src,
                                     uint32_t lba, uint32_t n, uint32_t t) {
  (void)h;
  (void)src;
  (void)lba;
  (void)n;
  (void)t;
  return HAL_ERROR;
}
uint32_t HAL_SD_GetCardState(SD_HandleTypeDef *h) {
  (void)h;
  return HAL_SD_CARD_TRANSFER;
}
HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef *h, uint32_t start,
                               uint32_t end) {
  (void)h;
  (void)start;
  (void)end;
  return HAL_ERROR;
}
