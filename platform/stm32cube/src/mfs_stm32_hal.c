/* mfs_stm32_hal.c — adaptacion al HAL de STM32Cube (flash interna).
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unico fichero que conoce las diferencias del HAL. En host
 * (MFS_STM32_HOST_TEST) se compila contra el modelo de flash de los shims, de
 * modo que el algoritmo del backend (RMW + ECC + borrado por tramo) se prueba
 * de verdad sin silicio.
 */
#include "mfs_stm32_hal.h"

#include <string.h> /* memcpy */

/* =====================================================================
 * Build de host: el modelo de flash vive en los shims y expone la misma
 * superficie que este fichero usa. Aqui no se implementa nada: las funciones
 * las aporta shims/stm32_hal_shim.c.
 * ===================================================================== */
#if defined(MFS_STM32_HOST_TEST)
/* Todo lo aporta el shim. */
#else /* ---------- Destino real (STM32Cube HAL) ---------- */

/* ---------------------------------------------------------------------
 * Tamano de la flash
 * --------------------------------------------------------------------- */
uint32_t mfs_stm32_hal_flash_size(void) {
#if defined(FLASH_SIZE)
  /* Registro FLASH_SIZE: en la mayoria de familias viene en KB. */
#if defined(STM32F1xx) || defined(STM32F0xx) || defined(STM32F3xx) ||          \
    defined(STM32L0xx) || defined(STM32L1xx) || defined(STM32G0xx)
  return (uint32_t)FLASH_SIZE * 1024u;
#else
  return (uint32_t)FLASH_SIZE;
#endif
#elif defined(MFS_STM32_IFLASH_SIZE)
  return (uint32_t)MFS_STM32_IFLASH_SIZE;
#else
  /* Ultimo recurso: el integrador debe declarar la region; se asume 1 MB. */
  return 1024u * 1024u;
#endif
}

/* ---------------------------------------------------------------------
 * Lock / unlock
 * --------------------------------------------------------------------- */
/* Lectura de la flash interna. En destino esta mapeada en el espacio de
 * direcciones (XIP), de modo que basta copiar; si tu familia necesitara un
 * acceso distinto (p. ej. una ventana de datos), es el unico sitio a cambiar.
 */
mfs_st mfs_stm32_hal_flash_read(uint32_t addr, void *dst, uint32_t len) {
  if (!dst || len == 0u)
    return MFS_EINVAL;
  memcpy(dst, (const void *)(uintptr_t)addr, (size_t)len);
  return MFS_OK;
}

mfs_st mfs_stm32_hal_flash_unlock(void) {
  return (HAL_FLASH_Unlock() == HAL_OK) ? MFS_OK : MFS_EIO;
}

mfs_st mfs_stm32_hal_flash_lock(void) {
  return (HAL_FLASH_Lock() == HAL_OK) ? MFS_OK : MFS_EIO;
}

/* ---------------------------------------------------------------------
 * Programacion
 *
 * Se recibe un PUNTERO a los `unit` bytes, no un uint64_t: una quadword
 * (16 B, familias H5/H7/U5) no cabe en el parametro de HAL_FLASH_Program y
 * volcarla en un uint64_t seria un desbordamiento de buffer (lo detecto el
 * modelo de host con -Werror=array-bounds).
 *
 * La firma de HAL_FLASH_Program cambio entre versiones del HAL:
 *   · moderna: HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address,
 * uint64_t Data) · antigua: HAL_FLASH_Program(uint32_t TypeProgram, uint32_t
 * Address, uint32_t Data) Se selecciona por familia; si tu version del HAL no
 * coincide, ajusta SOLO este bloque.
 * --------------------------------------------------------------------- */
mfs_st mfs_stm32_hal_flash_program_bytes(uint32_t addr, const uint8_t *bytes,
                                         uint32_t unit) {
  if (!bytes)
    return MFS_EINVAL;

  uint32_t type = 0u;
  switch (unit) {
    /* Cada constante FLASH_TYPEPROGRAM_* solo existe en las familias cuyo
     * dispositivo la soporta. STM32U5 (y H5/U5) solo define QUADWORD: las
     * unidades de 2/4/8 B no existen alli, de modo que sin estas guardas el
     * fichero NO compilaba en U5 ("'FLASH_TYPEPROGRAM_HALFWORD' undeclared").
     */
#if defined(FLASH_TYPEPROGRAM_HALFWORD)
  case 2u:
    type = FLASH_TYPEPROGRAM_HALFWORD;
    break;
#endif
#if defined(FLASH_TYPEPROGRAM_WORD)
  case 4u:
    type = FLASH_TYPEPROGRAM_WORD;
    break;
#endif
#if defined(FLASH_TYPEPROGRAM_DOUBLEWORD)
  case 8u:
    type = FLASH_TYPEPROGRAM_DOUBLEWORD;
    break;
#endif
  case 16u:
    /* 128 bits no caben en el uint64_t del HAL: se delega en el gancho debil.
     * Sin el, se RECHAZA en vez de truncar silenciosamente 8 bytes. */
    return mfs_stm32_hal_flash_program_quadword(addr, bytes);
  default:
    return MFS_ENOTSUP;
  }

  uint64_t data = 0u;
  memcpy(&data, bytes, (size_t)unit); /* unit <= 8 aqui */

  /* DESBLOQUEO OBLIGATORIO. Con LOCK=1 el bit PG no se fija y la escritura se
   * PIERDE EN SILENCIO mientras el HAL devuelve HAL_OK. Antes solo desbloqueaba
   * el borrado, de modo que toda la programacion se perdia en silencio si el
   * controlador estaba bloqueado (que es el estado de reset). Se restaura el
   * bloqueo al terminar para no dejarlo abierto. */
  if (mfs_stm32_hal_flash_unlock() != MFS_OK)
    return MFS_EIO;

#if defined(STM32F1xx) || defined(STM32F0xx)
  /* HAL antiguo: el parametro de datos es de 32 bits y solo hay HALFWORD/WORD.
   */
  if (HAL_FLASH_Program(type, addr, (uint32_t)data) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }
#else
  if (HAL_FLASH_Program(type, addr, data) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }
#endif
  (void)mfs_stm32_hal_flash_lock();
  return MFS_OK;
}

/* Gancho debil: el integrador de una familia de 16 B lo sobreescribe. */
__attribute__((weak)) mfs_st
mfs_stm32_hal_flash_program_quadword(uint32_t addr, const uint8_t bytes[16]) {
  (void)addr;
  (void)bytes;
  return MFS_ENOTSUP;
}

/* ---------------------------------------------------------------------
 * Programación en bloque (STM-04)
 *
 * Programa `len` bytes contiguos en `addr` con UN SOLO ciclo
 * unlock/program/lock. `addr` debe estar alineado a `unit` y `len` debe
 * ser múltiplo de `unit`. Devuelve MFS_EINVAL si no se cumple.
 * --------------------------------------------------------------------- */
mfs_st mfs_stm32_hal_flash_program_block(uint32_t addr, const uint8_t *bytes,
                                         uint32_t len, uint32_t unit) {
  if (!bytes || len == 0u || (addr % unit) != 0u || (len % unit) != 0u) {
    return MFS_EINVAL;
  }

  uint32_t type = 0u;
  bool quad = false;
  switch (unit) {
    /* Igual que en program_bytes(): las constantes FLASH_TYPEPROGRAM_* solo
     * existen donde el dispositivo las soporta (STM32U5 solo QUADWORD). */
#if defined(FLASH_TYPEPROGRAM_HALFWORD)
  case 2u:
    type = FLASH_TYPEPROGRAM_HALFWORD;
    break;
#endif
#if defined(FLASH_TYPEPROGRAM_WORD)
  case 4u:
    type = FLASH_TYPEPROGRAM_WORD;
    break;
#endif
#if defined(FLASH_TYPEPROGRAM_DOUBLEWORD)
  case 8u:
    type = FLASH_TYPEPROGRAM_DOUBLEWORD;
    break;
#endif
  case 16u:
    /* 16 B (H5/H7/U5): HAL_FLASH_Program solo lleva 64 bits en `Data`, de modo
     * que la quadword se programa con el gancho por unidad. Se marca el camino
     * en vez de retornar: un bloque puede tener VARIAS quadwords y hay que
     * programarlas TODAS dentro del mismo ciclo unlock/lock. */
    quad = true;
    break;
  default:
    return MFS_ENOTSUP;
  }

  if (mfs_stm32_hal_flash_unlock() != MFS_OK)
    return MFS_EIO;

  const uint8_t *p = bytes;
  uint32_t remaining = len;
  mfs_st st = MFS_OK;

  while (remaining >= unit) {
    if (quad) {
      if (mfs_stm32_hal_flash_program_quadword(addr, p) != MFS_OK) {
        st = MFS_EIO;
        break;
      }
    } else {
      uint64_t data = 0u;
      memcpy(&data, p, (size_t)unit);

#if defined(STM32F1xx) || defined(STM32F0xx)
      if (HAL_FLASH_Program(type, addr, (uint32_t)data) != HAL_OK) {
#else
      if (HAL_FLASH_Program(type, addr, data) != HAL_OK) {
#endif
        st = MFS_EIO;
        break;
      }
    }
    addr += unit;
    p += unit;
    remaining -= unit;
  }

  (void)mfs_stm32_hal_flash_lock();
  return st;
}

/* ---------------------------------------------------------------------
 * Borrado de un sector
 *
 * El campo que nombra el sector cambia entre familias (Page en
 * F0/F1/L0/L1/G0/G4, Sector en F2/F4/F7/H7/L4/U5), igual que la seleccion de
 * banco.
 *
 * CORRECCION IMPORTANTE: el indice NO se puede calcular dividiendo
 * `(addr - base) / size`, porque con sectores NO UNIFORMES da un resultado
 * erroneo y HAL_FLASHEx_Erase borraria OTRO sector. En un F4 de 1 MB,
 * 0x08020000 / 128 KB = 1, cuando el sector correcto es el 5; se borraria el
 * sector de 16 KB de 0x08004000. Por eso el indice se OBTIENE DE LA TABLA de
 * sectores, que es la unica fuente correcta.
 * --------------------------------------------------------------------- */
static const mfs_sector_t *s_hal_sectors = NULL;
static uint32_t s_hal_sector_count = 0u;

void mfs_stm32_hal_flash_set_sector_map(const mfs_sector_t *map, uint32_t n) {
  s_hal_sectors = map;
  s_hal_sector_count = n;
}

/* Indice del sector que contiene `addr`. La tabla está ordenada de forma
 * ascendente y contigua (lo garantiza mfs_stm32_flash_map_validate), así que se
 * usa búsqueda binaria: antes un escaneo lineal por sector hacía el borrado
 * O(n·m) — con la familia L0 (1536 sectores) eran decenas de miles de
 * iteraciones. Devuelve -1 si no hay tabla o no lo encuentra. */
static int32_t hal_sector_index(uint32_t addr) {
  uint32_t lo = 0u;
  uint32_t hi = s_hal_sector_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (addr < s_hal_sectors[mid].addr) {
      hi = mid;
    } else if (addr >= s_hal_sectors[mid].addr + s_hal_sectors[mid].size) {
      lo = mid + 1u;
    } else {
      return (int32_t)mid;
    }
  }
  return -1;
}

mfs_st mfs_stm32_hal_flash_erase_sector(uint32_t addr, uint32_t size) {
  FLASH_EraseInitTypeDef ei;
  uint32_t sector_error = 0u;

  if (size == 0u)
    return MFS_EINVAL;

  /* Sin tabla no se puede saber el indice: se RECHAZA en vez de arriesgar el
   * borrado del sector equivocado. `mfs_stm32_iflash_prepare()` la instala
   * siempre antes de cualquier borrado. */
  int32_t idx = hal_sector_index(addr);
  if (idx < 0)
    return MFS_EINVAL;
  uint32_t index = (uint32_t)idx;

  if (mfs_stm32_hal_flash_unlock() != MFS_OK)
    return MFS_EIO;

  /* --- Familias con PAGINAS y banco explicito --- */
#if defined(STM32F0xx) || defined(STM32F1xx) || defined(STM32F3xx) ||          \
    defined(STM32L0xx) || defined(STM32L1xx) || defined(STM32G0xx) ||          \
    defined(STM32G4xx) || defined(STM32L4xx) || defined(STM32L5xx) ||          \
    defined(STM32WBxx) || defined(STM32WLxx)
  ei.TypeErase = FLASH_TYPEERASE_PAGES;
  ei.Page = index;
  ei.NbPages = 1u;
#if defined(STM32L4xx) || defined(STM32L5xx) || defined(STM32G4xx) ||          \
    defined(STM32WBxx) || defined(STM32G0xx)
  /* Banco segun la posicion del sector en la tabla (la mitad baja es el 1). */
  ei.Banks = (index < (s_hal_sector_count / 2u)) ? FLASH_BANK_1 : FLASH_BANK_2;
#endif
  if (HAL_FLASHEx_Erase(&ei, &sector_error) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }

  /* --- Familias con SECTORES --- */
#elif defined(STM32F2xx) || defined(STM32F4xx) || defined(STM32F7xx) ||        \
    defined(STM32H7xx)
  ei.TypeErase = FLASH_TYPEERASE_SECTORS;
  ei.Sector = index;
  ei.NbSectors = 1u;
#if defined(STM32F4xx) || defined(STM32F7xx)
  ei.VoltageRange = FLASH_VOLTAGE_RANGE_3;
#endif
#if defined(FLASH_BANK_2)
  /* Dispositivo de DOBLE banco: el banco se deduce de la posicion del sector
   * en la tabla (los de la mitad alta son del banco 2). En un F4 de 1 MB los
   * sectores del segundo banco tienen indice >= 12. */
  ei.Banks = (index < 12u) ? FLASH_BANK_1 : FLASH_BANK_2;
#else
  /* Dispositivo de UN solo banco (p. ej. F401/F405/F407/F411/F446): solo existe
   * FLASH_BANK_1. Antes se usaba FLASH_BANK_2 sin guarda y NO compilaba en esos
   * dispositivos. */
  ei.Banks = FLASH_BANK_1;
#endif
  if (HAL_FLASHEx_Erase(&ei, &sector_error) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }

  /* --- Familias nuevas (H5/U5): el TIPO lo dicta el HAL ---
   * STM32U5 usa PAGINAS de 8 KB, no "sectores"; clasificarlo como sectores
   * hacia que este fichero NO compilara en U5
   * ('FLASH_TYPEERASE_SECTORS' undeclared, 'FLASH_EraseInitTypeDef' has no
   * member named 'Sector'). Se decide por la macro que el HAL defina, de modo
   * que la clasificacion no haya que mantenerla a mano. */
#elif defined(STM32H5xx) || defined(STM32U5xx)
#if defined(FLASH_TYPEERASE_PAGES)
  ei.TypeErase = FLASH_TYPEERASE_PAGES;
  ei.Page = index;
  ei.NbPages = 1u;
#else
  ei.TypeErase = FLASH_TYPEERASE_SECTORS;
  ei.Sector = index;
  ei.NbSectors = 1u;
#endif
#if defined(FLASH_BANK_2)
  ei.Banks = (index < (s_hal_sector_count / 2u)) ? FLASH_BANK_1 : FLASH_BANK_2;
#endif
  if (HAL_FLASHEx_Erase(&ei, &sector_error) != HAL_OK) {
    (void)mfs_stm32_hal_flash_lock();
    return MFS_EIO;
  }
#else
  (void)ei;
  (void)sector_error;
  (void)mfs_stm32_hal_flash_lock();
  return MFS_ENOTSUP;
#endif

  return (mfs_stm32_hal_flash_lock() == MFS_OK) ? MFS_OK : MFS_EIO;
}

/* ---------------------------------------------------------------------
 * Read-while-write
 *
 * Un solo banco ⇒ programar/borrar detiene la busqueda de instrucciones. Las
 * familias con doble banco permiten ejecutar desde un banco mientras se opera
 * en el otro. Esto NO es una limitacion del port: es del silicio, y el README
 * explica sus consecuencias en el jitter del GC.
 * --------------------------------------------------------------------- */
bool mfs_stm32_hal_flash_single_bank(void) {
#if defined(STM32H7xx) || defined(STM32H5xx) || defined(STM32U5xx) ||          \
    defined(STM32L5xx)
  return false; /* doble banco: RWW posible */
#elif defined(STM32F4xx) || defined(STM32F7xx)
  /* F4/F7 tienen modelos de UNO y de DOS bancos: solo hay RWW cuando el
   * dispositivo define FLASH_BANK_2 (F42x/F43x en F4; F76x/F77x en F7). Un
   * F401/F405/F407/F411/F446/F72x... es de un solo banco y la CPU se detiene
   * mientras programa o borra. */
#if defined(FLASH_BANK_2)
  return false;
#else
  return true;
#endif
#else
  return true; /* un solo banco: la CPU se detiene al programar/borrar */
#endif
}

#endif /* MFS_STM32_HOST_TEST */
