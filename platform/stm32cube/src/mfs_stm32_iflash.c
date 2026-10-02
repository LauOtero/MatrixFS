/* mfs_stm32_iflash.c — flash INTERNA del STM32 como medio de MatrixFS.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Este es el backend mas delicado del port, porque la flash interna de un STM32
 * NO se comporta como un NOR SPI:
 *
 *   1) UNIDAD DE PROGRAMACION > 1 BYTE. El F4 programa de 4 en 4 bytes (word),
 *      el L4/G4/G0 de 8 (doubleword) y el H5/H7/U5 de 16 (quadword). El nucleo
 *      pide escribir 32 B (token T1) y 64 B (HWV y cabecera de zona), que son
 *      multiplos exactos SOLO si la direccion esta alineada; el backend no
 * puede suponerlo y hace read-modify-write sobre la unidad.
 *
 *   2) ECC. En L4/G4/H5/H7/U5 la flash lleva ECC por hardware:
 *        · programar dos veces la misma double/quadword produce PROGERR, y
 *        · dejar una unidad a medio programar produce ERROR DE ECC AL LEERLA
 *          (ECCC/ECCC2 en H7; DBECCERR/SNECCERR en U5).
 *      Por eso el RMW del port ESP-IDF (leer la ventana y reescribirla entera)
 *      NO seria valido aqui. El algoritmo de abajo lo resuelve sin depender de
 *      la suerte: si la unidad no esta virgen, la operacion solo se acepta si
 * el contenido nuevo YA ESTA contenido en el actual (no-op); en cualquier otro
 *      caso se devuelve MFS_EIO en vez de corromper.
 *
 *   3) SECTORES NO UNIFORMES. Se resuelve con la VENTANA UNIFORME: el
 *      `erase_unit` logico es multiplo del sector fisico y `erase(addr)` borra
 *      TODOS los sectores que cubren [addr, addr+erase_unit). El nucleo solo
 *      borra en multiplos de erase_unit, de modo que la propiedad se mantiene.
 *
 *   4) READ-WHILE-WRITE. En familias de un solo banco
 * (F0/F1/F3/L0/L1/G0/G4/WB/WL) programar o borrar DETIENE la busqueda de
 * instrucciones toda la operacion: un borrado de 45 ms son 45 ms sin ejecutar
 * codigo. Es una propiedad del silicio; el backend la declara y el README la
 * documenta.
 */
#include "mfs_stm32_backend.h"

#include <string.h>

#include "mfs_stm32_flash_map.h"
/* Toda la dependencia del HAL (y del dispositivo del build de host) se aisla
 * en esta cabecera. */
#include "mfs_stm32_hal.h"

/* =====================================================================
 * Estado estatico (sin heap, instancia unica)
 * ===================================================================== */

/* Numero maximo de sectores que se admiten en una region reservada. Un H7 de
 * 2 MB tiene 16 sectores de 128 KB; un F1 de 512 KB tiene 256 paginas de 2 KB;
 * un L0 de 192 KB tiene 1536 paginas de 128 B. 2048 cubre todos los casos. */
#define MFS_STM32_IFLASH_MAX_SECTORS 2048u

typedef struct {
  uint32_t base;       /* base de la ventana uniforme       */
  uint32_t size;       /* bytes utilizables                 */
  uint32_t erase_unit; /* unidad logica de borrado          */
  uint32_t pgm_unit;   /* unidad de programacion (4/8/16)   */
  bool ecc;            /* la familia lleva ECC              */
  /* Mapa COMPLETO del dispositivo (persistente) y rango de sectores que cubren
   * la ventana uniforme. La ventana es un tramo CONTIGUO del mapa ordenado, de
   * modo que NO se duplican sectores: bastan indice y cuenta. */
  const mfs_stm32_sector_t *map;
  uint32_t map_n;
  uint32_t win_first;
  uint32_t win_count;
} iflash_state_t;

static iflash_state_t s_if;

/* Respaldo estatico del mapa cuando lo aporta la tabla por familia. El mapa que
 * aporta el integrador NO se copia: se usa el suyo, que es persistente. El HAL
 * conserva el puntero del mapa, por lo que NUNCA puede apuntar a una variable
 * de pila (antes se le pasaba una tabla de pila y quedaba colgando). */
static mfs_stm32_sector_t s_if_map_backing[MFS_STM32_IFLASH_MAX_SECTORS];

/* =====================================================================
 * Deteccion de la unidad de programacion de la familia
 * ===================================================================== */

/* Familias con ECC en la flash interna y unidad de programacion mayor. Los
 * macros son los que define el CMSIS/HAL del dispositivo.
 *
 * En el build de host (MFS_STM32_HOST_TEST) la unidad y la ECC las declara el
 * modelo de flash, de modo que el MISMO backend se puede probar con 4 B sin ECC
 * (F4), 8 B con ECC (L4/G4) y 16 B con ECC (H7/U5): es justo el rango donde
 * vive el riesgo de corromper una unidad ya programada. */
static uint32_t iflash_pgm_unit(void) {
#if defined(MFS_STM32_HOST_TEST)
  return (uint32_t)MFS_STM32_HOST_PGM_UNIT;
#elif defined(STM32H7xx) || defined(STM32H5xx) || defined(STM32U5xx)
  return 16u; /* quadword */
#elif defined(STM32L4xx) || defined(STM32L5xx) || defined(STM32G0xx) ||        \
    defined(STM32G4xx) || defined(STM32WBxx) || defined(STM32WLxx)
  return 8u; /* doubleword */
#elif defined(STM32F4xx) || defined(STM32F7xx)
  return 4u; /* word */
#elif defined(STM32F0xx) || defined(STM32F1xx) || defined(STM32F3xx)
  return 2u; /* halfword */
#else
  return 4u; /* conservador */
#endif
}

static bool iflash_has_ecc(void) {
#if defined(MFS_STM32_HOST_TEST)
  return (MFS_STM32_HOST_ECC != 0);
#elif defined(STM32H7xx) || defined(STM32H5xx) || defined(STM32U5xx) ||        \
    defined(STM32L4xx) || defined(STM32L5xx) || defined(STM32G0xx) ||          \
    defined(STM32G4xx) || defined(STM32WBxx) || defined(STM32WLxx)
  return true;
#else
  return false;
#endif
}

/* Valor de `TypeProgram` del HAL y la llamada concreta los absorbe
 * mfs_stm32_hal_flash_program() (la firma cambia entre familias y versiones).
 */

/* =====================================================================
 * Acceso elemental
 * ===================================================================== */

static mfs_st iflash_read_raw(uint32_t addr, void *dst, uint32_t len) {
  /* Se lee a traves de la capa HAL y NO con un puntero directo a 0x0800xxxx.
   * En destino la flash esta mapeada (XIP) y el memcpy seria equivalente, pero
   * un acceso por puntero impide probar el backend en host — donde la flash
   * simulada vive en RAM — y ademas ata el nucleo a que la region sea
   * accesible por direccion. */
  return mfs_stm32_hal_flash_read(addr, dst, len);
}

/* Programa un bloque contiguo de unidades completas ya fusionadas.
 * Se usa cuando tenemos varias unidades consecutivas que necesitan escribirse.
 * Devuelve MFS_OK si todo se programó, o el error correspondiente. */
static mfs_st iflash_program_block(uint32_t addr, const uint8_t *src,
                                   uint32_t len, uint32_t unit) {
  return mfs_stm32_hal_flash_program_block(addr, src, len, unit);
}

/* Vacía el bloque acumulado de unidades a programar. */
static mfs_st iflash_flush_block(uint32_t block_start, uint8_t *block_buf,
                                 uint32_t *block_len, uint32_t unit) {
  if (*block_len == 0u)
    return MFS_OK;
  mfs_st st = iflash_program_block(block_start, block_buf, *block_len, unit);
  *block_len = 0u;
  return st;
}

/* =====================================================================
 * Callbacks de la region plana (contrato mfs_embedded_flash_t)
 * ===================================================================== */

static mfs_st if_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  (void)ctx;
  if (!dst || len == 0u)
    return MFS_EINVAL;
  if (addr < s_if.base ||
      (uint64_t)addr + len > (uint64_t)s_if.base + s_if.size)
    return MFS_EINVAL;
  return iflash_read_raw(addr, dst, len);
}

/* Programacion con RMW por unidad, tolerante a ECC, con lote (STM-04).
 *
 * Para cada unidad que cubre [addr, addr+len):
 *   - se lee la unidad completa (el dato llega YA corregido por la ECC del
 *     dispositivo, porque la lectura es por el bus, no por el controlador);
 *   - se fusiona el tramo nuevo;
 *   - si algun byte de la fusion difiere del valor actual:
 *       · si la unidad esta TODA a 0xFF ⇒ se acumula para programar en lote;
 *       · si el valor actual YA CONTIENE el nuevo (a & b == b) ⇒ no-op;
 *       · en otro caso ⇒ MFS_EIO (haría falta borrar el sector; con ECC
 *         intentarlo corrompería la unidad).
 *
 * Cuando se tiene un bloque contiguo de unidades a programar (todas a 0xFF),
 * se programa con UN SOLO ciclo unlock/program/lock mediante
 * mfs_stm32_hal_flash_program_block.
 */
static mfs_st if_prog(void *ctx, uint32_t addr, const void *src, uint32_t len) {
  (void)ctx;
  if (!src || len == 0u)
    return MFS_EINVAL;
  if (addr < s_if.base ||
      (uint64_t)addr + len > (uint64_t)s_if.base + s_if.size)
    return MFS_EINVAL;
  /* NO se exige alineacion de `addr` a la unidad: precisamente para eso esta el
   * RMW de abajo. El nucleo escribe en direcciones alineadas, pero un backend
   * no debe depender de ello. */

  const uint8_t *sp = (const uint8_t *)src;
  uint32_t unit = s_if.pgm_unit;
  uint8_t cur[16];
  uint8_t merged[16];

  /* Buffer para acumular unidades contiguas y programarlas con UN SOLO ciclo
   * unlock/program/lock. El tamano NO depende de `len`: el nucleo puede pedir
   * hasta `chunk_size` (hasta 4 KiB en modo Extended), de modo que si el buffer
   * se llena se VUELCA y se sigue acumulando. Asi el coste en pila es constante
   * (64 B) y el lote sigue evitando un unlock por unidad. */
  uint8_t block_buf[64];
  uint32_t block_start = 0u; /* direccion de inicio del bloque actual */
  uint32_t block_len = 0u;   /* bytes acumulados en block_buf */

  for (uint32_t done = 0u; done < len;) {
    uint32_t a = addr + done;
    uint32_t n = len - done;
    /* No cruzar la frontera de unidad: se trocea en unidades completas. */
    uint32_t off_in_unit = a % unit;
    uint32_t room = unit - off_in_unit;
    if (n > room)
      n = room;
    uint32_t unit_addr = a - off_in_unit;
    /* La unidad que contiene `a` podria empezar antes de la region si la
     * ventana no estuviera alineada; la ventana se calcula con el helper, que
     * garantiza alineacion a sector. Se comprueba por seguridad. */
    if (unit_addr < s_if.base)
      return MFS_EINVAL;

    if (iflash_read_raw(unit_addr, cur, unit) != MFS_OK)
      return MFS_EIO;
    memcpy(merged, cur, (size_t)unit);
    memcpy(merged + off_in_unit, sp + done, (size_t)n);

    if (memcmp(merged, cur, (size_t)unit) != 0) {
      bool all_ff = true;
      bool contained = true;
      for (uint32_t i = 0u; i < unit; i++) {
        if (cur[i] != 0xFFu)
          all_ff = false;
        if ((cur[i] & merged[i]) != merged[i])
          contained = false;
      }
      if (!all_ff) {
        /* Vaciar cualquier bloque pendiente antes de decidir no-op o error. */
        mfs_st st =
            iflash_flush_block(block_start, block_buf, &block_len, unit);
        if (st != MFS_OK)
          return st;
        if (contained) {
          /* El dato pedido ya esta: no se reprograma (seria PROGERR con ECC).
           */
          done += n;
          continue;
        }
        /* Habria que borrar el sector. No se hace en silencio: el nucleo debe
         * haber borrado la zona antes de programar. */
        return MFS_EIO;
      }
      /* Unidad virgen: acumular en el bloque si es contigua. */
      if (block_len > 0u && unit_addr != block_start + block_len) {
        /* No contigua: vaciar bloque anterior. */
        mfs_st st =
            iflash_flush_block(block_start, block_buf, &block_len, unit);
        if (st != MFS_OK)
          return st;
      }
      /* Sin hueco para esta unidad: volcar el bloque lleno y seguir. */
      if (block_len + unit > sizeof(block_buf)) {
        mfs_st st =
            iflash_flush_block(block_start, block_buf, &block_len, unit);
        if (st != MFS_OK)
          return st;
      }
      if (block_len == 0u)
        block_start = unit_addr;
      memcpy(block_buf + block_len, merged, (size_t)unit);
      block_len += unit;
    }
    done += n;
  }

  /* Vaciar el ultimo bloque pendiente. */
  return iflash_flush_block(block_start, block_buf, &block_len, unit);
}

/* Borra TODOS los sectores del dispositivo que cubren [addr, addr+erase_unit).
 * El nucleo solo llama con direcciones multiplos de `erase_unit`, de modo que
 * el tramo siempre empieza en frontera de sector (lo garantiza la ventana
 * uniforme calculada en prepare). */
static mfs_st if_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  if (addr < s_if.base || addr + s_if.erase_unit > s_if.base + s_if.size)
    return MFS_EINVAL;
  uint32_t end = addr + s_if.erase_unit;
  uint32_t cur = addr;

  /* Los sectores de la ventana son contiguos y ascendentes: se avanza el indice
   * en lugar de reescanear desde el principio (antes era O(n·m)). */
  uint32_t i = s_if.win_first;
  const uint32_t iend = s_if.win_first + s_if.win_count;
  while (cur < end) {
    while (i < iend && !(cur >= s_if.map[i].addr &&
                         cur < s_if.map[i].addr + s_if.map[i].size))
      i++;
    if (i >= iend)
      return MFS_EINVAL;
    if (s_if.map[i].addr != cur) {
      /* El tramo no empieza en frontera de sector: la ventana uniforme no se
       * calculo bien. Abortar antes que borrar de mas. */
      return MFS_EINVAL;
    }
    mfs_st st =
        mfs_stm32_hal_flash_erase_sector(s_if.map[i].addr, s_if.map[i].size);
    if (st != MFS_OK)
      return st;
    cur += s_if.map[i].size;
    i++;
  }
  return MFS_OK;
}

/* =====================================================================
 * Preparacion de la region
 * ===================================================================== */

mfs_st mfs_stm32_iflash_prepare(const mfs_stm32_cfg *cfg,
                                mfs_embedded_flash_t *flash) {
  if (!cfg || !flash)
    return MFS_EINVAL;

  uint32_t base = cfg->region_addr;
  uint32_t size = cfg->region_size;
  if (base == 0u || size == 0u)
    return MFS_EINVAL; /* la region reservada es obligatoria */

  /* 1) Mapa de sectores: el del integrador manda (siempre correcto, tomado de
   * su Reference Manual); si no, la tabla por familia, expandida en el respaldo
   * ESTATICO (no en pila: el HAL conserva el puntero y quedaria colgando). */
  const mfs_stm32_sector_t *map = cfg->sectors;
  uint32_t map_n = cfg->sector_count;

  if (!map || map_n == 0u) {
    const mfs_stm32_family_t *fam = mfs_stm32_flash_family();
    if (!fam)
      return MFS_ENOTSUP; /* familia sin tabla: hay que aportarla */
    mfs_st st = mfs_stm32_flash_expand(
        fam, s_if_map_backing, MFS_STM32_IFLASH_MAX_SECTORS, 0u, &map_n);
    if (st != MFS_OK)
      return st;
    map = s_if_map_backing;
  }

  /* 2) La tabla debe cubrir la flash COMPLETA de forma contigua: una tabla mal
   * escrita borraria el sector equivocado. */
  if (!mfs_stm32_flash_map_validate(map, map_n, MFS_STM32_FLASH_BASE_ADDR,
                                    mfs_stm32_hal_flash_size()))
    return MFS_EINVAL;

  /* 3) Ventana uniforme dentro de [base, base+size). */
  mfs_uniform_window_t win;
  mfs_st st =
      mfs_stm32_flash_uniform_window(map, map_n, base, size, 1024u, &win);
  if (st != MFS_OK)
    return st; /* MFS_ENOENT si la region no contiene una ventana valida */

  memset(&s_if, 0, sizeof(s_if));
  s_if.base = win.base;
  s_if.size = win.size;
  s_if.erase_unit = win.unit;
  s_if.pgm_unit = iflash_pgm_unit();
  s_if.ecc = iflash_has_ecc();

  /* 4) La ventana uniforme es un tramo CONTIGUO del mapa ordenado: se guarda su
   * rango de indices, sin duplicar sectores (antes se copiaban a un array de
   * 16 KB y ademas habia otro array de 16 KB en pila). */
  uint32_t first = 0u;
  while (first < map_n && (map[first].addr + map[first].size) <= win.base)
    first++;
  uint32_t win_n = 0u;
  while (first + win_n < map_n && map[first + win_n].addr < win.base + win.size)
    win_n++;
  if (win_n == 0u)
    return MFS_EINVAL;

  s_if.map = map;
  s_if.map_n = map_n;
  s_if.win_first = first;
  s_if.win_count = win_n;

  /* 4b) La capa HAL necesita la tabla COMPLETA del dispositivo para traducir
   * direccion -> indice de sector al borrar (la division por el tamano solo
   * vale con sectores uniformes). Se instala aqui, antes de cualquier borrado.
   */
  mfs_stm32_hal_flash_set_sector_map(map, map_n);

  /* 5) Descriptor de region plana. */
  memset(flash, 0, sizeof(*flash));
  flash->read = if_read;
  flash->prog = if_prog;
  flash->erase = if_erase;
  flash->ctx = &s_if;
  flash->base_addr = s_if.base;
  flash->size = s_if.size;
  flash->erase_unit = s_if.erase_unit;
  flash->page_size =
      (uint16_t)(cfg->page_size ? cfg->page_size : (uint16_t)s_if.pgm_unit);
  flash->program_granularity = s_if.pgm_unit;
  flash->no_erase = false;
  if (cfg->t_prog_max_us)
    flash->t_prog_max_us = cfg->t_prog_max_us;
  if (cfg->t_erase_max_us)
    flash->t_erase_max_us = cfg->t_erase_max_us;
  else
    /* El borrado de un sector de 128 KB del F4 puede llegar a ~1 s en el peor
     * caso (tipico ~1 s para 128 KB, ~250 ms para 16 KB). Se declara un valor
     * conservador: estos presupuestos alimentan el modelo de energia/EDP, no un
     * timeout de polling (el HAL bloquea hasta terminar). AJUSTAR al dato del
     * datasheet de la familia concreta. */
    flash->t_erase_max_us = 1000000u;
  if (cfg->t_read_max_us)
    flash->t_read_max_us = cfg->t_read_max_us;
  /* Declarar la ECC hace que el nucleo elija la clase ECC mas tolerante
   * (src/ftl/mfs_ftl2.c: mfs_eba_select). Es una consecuencia real, no
   * cosmetica. */
  if (s_if.ecc)
    flash->flags0_extra |= (uint8_t)MFS_HWV0_ECC_ON_DIE;

  return MFS_OK;
}
