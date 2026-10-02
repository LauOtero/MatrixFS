/* matrixfs_region.ld — reserva de la region de MatrixFS en la flash interna.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * STM32Cube NO tiene tabla de particiones (a diferencia de ESP-IDF): la region
 * del sistema de archivos hay que reservarla en el LINKER SCRIPT del proyecto.
 * Sin esta reserva, el enlazador colocaria codigo o datos dentro de la region y
 * MatrixFS los borraria al formatear o al reciclar zonas.
 *
 * COMO USARLO
 *
 *  1) Elige la region. Debe empezar en frontera de SECTOR de tu dispositivo y
 *     tener el tamano de un numero entero de sectores del MISMO tamano (la
 *     "ventana uniforme": ver el README). En un STM32F407 de 1 MB, los ultimos
 *     sectores son 3 x 128 KB = 384 KB en 0x080A0000..0x08100000.
 *
 *  2) Incluye este fichero AL FINAL de tu script de enlazado (.ld), despues de
 *     las secciones y de la definicion de MEMORY:
 *
 *         INCLUDE platform/stm32cube/linker/matrixfs_region.ld
 *
 *     (o pega su contenido). Los ASSERT se evaluan al enlazar y ABORTAN el build
 *     si la region se solapa con el codigo/datos o se sale de la flash: es
 *     preferible un error de enlace a un borrado silencioso de codigo.
 *
 *  3) Declara los mismos valores en mfs_stm32_conf.h
 *     (MFS_STM32_IFLASH_ADDR / MFS_STM32_IFLASH_REGION_SIZE). Si no coinciden,
 *     el port lo detecta: `mfs_stm32_flash_map_validate()` comprueba el mapa y
 *     el montaje devuelve MFS_EINVAL.
 *
 *  4) Si usas STM32CubeIDE, anade este fichero con "Include" en las
 *     propiedades del proyecto o pega el bloque al final de STM32xxxxx_FLASH.ld.
 *
 * NOTA sobre el tamano de la region: el volumen UTIL de MatrixFS esta limitado
 * por MFS_ZONE_MAX * erase_unit (128 x 4 KB = 512 KB con sectores de 4 KB, o
 * 128 x 128 KB = 16 MB con los de 128 KB). Una region mayor no se aprovecha: el
 * port lo reporta con matrixfs_stm32_capacity_wasteful().
 */

/* ---------------------------------------------------------------------------
 * Parametros: AJUSTAR a tu dispositivo.
 * Los valores por defecto son de un STM32F407VG (1 MB) usando los tres ultimos
 * sectores de 128 KB.
 * ------------------------------------------------------------------------- */
_MFS_REGION_START = DEFINED(_MFS_REGION_START) ? _MFS_REGION_START : 0x080A0000;
_MFS_REGION_SIZE  = DEFINED(_MFS_REGION_SIZE)  ? _MFS_REGION_SIZE  : 0x00060000; /* 384 KB */
_MFS_REGION_END   = _MFS_REGION_START + _MFS_REGION_SIZE;

/* Limite de la flash del dispositivo. Ajustar si tu MCU no tiene 1 MB. */
_MFS_FLASH_START = 0x08000000;
_MFS_FLASH_END   = 0x08100000; /* 0x08000000 + 1 MB */

/* ---------------------------------------------------------------------------
 * Comprobaciones de coherencia (fallan el ENLACE, no en ejecucion)
 * ------------------------------------------------------------------------- */

/* La region debe empezar dentro de la flash y terminar dentro de ella. */
ASSERT(_MFS_REGION_START >= _MFS_FLASH_START,
       "MatrixFS: la region empieza fuera de la flash")
ASSERT(_MFS_REGION_END <= _MFS_FLASH_END,
       "MatrixFS: la region termina fuera de la flash")

/* La region debe empezar ALINEADA a sector. Se comprueba contra los tamanos de
 * sector mas habituales; si tu dispositivo usa otro, ajusta la lista. Un
 * desalineamiento haria que el borrado de la primera zona arrastrara codigo. */
ASSERT((_MFS_REGION_START % 4096) == 0,
       "MatrixFS: la region debe estar alineada a 4 KB como minimo")

/* La region NO puede solaparse con codigo, datos inicializados ni .bss.
 * _etext/_edata/_end los define el script del proyecto; si tu script usa otros
 * nombres (__etext, _sdata...), ajusta estas referencias. */
ASSERT(_MFS_REGION_START >= _etext,
       "MatrixFS: la region se solapa con el CODIGO (.text)")
ASSERT(_MFS_REGION_START >= _edata,
       "MatrixFS: la region se solapa con los DATOS inicializados (.data)")
ASSERT(_MFS_REGION_START >= _end,
       "MatrixFS: la region se solapa con .bss / heap / stack")

/* La region debe caber el layout reservado del nucleo: SB A (sector 0),
 * SB B (sector 1), anillo de tokens (sector 2) y al menos UNA zona. Con
 * sectores de 4 KB son 16 KB como minimo. */
ASSERT(_MFS_REGION_SIZE >= 16384,
       "MatrixFS: la region es menor que el layout minimo (3 sectores reservados + 1 zona de 4 KB)")

/* ---------------------------------------------------------------------------
 * Simbolos exportados para el firmware (utiles si prefieres no repetir las
 * direcciones en mfs_stm32_conf.h)
 * ------------------------------------------------------------------------- */
PROVIDE(_mfs_region_start = _MFS_REGION_START);
PROVIDE(_mfs_region_end   = _MFS_REGION_END);
PROVIDE(_mfs_region_size  = _MFS_REGION_SIZE);
