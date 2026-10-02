/* main_matrixfs_example.c — ejemplo minimo de uso del port STM32Cube.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * NO es un proyecto completo: es el FRAGMENTO que se inserta en el `main.c` que
 * genera CubeMX, despues de inicializar los perifericos y (si se usa) el RTOS.
 * Ver el README del port para la integracion paso a paso.
 *
 * Se muestran las dos formas de montaje:
 *   A) matrixfs_stm32_mount_default()  — usa mfs_stm32_conf.h (recomendado).
 *   B) matrixfs_stm32_mount()          — configuracion explicita en codigo.
 */

#include "main.h" /* generado por CubeMX: handles, MX_*_Init() */

#include "matrixfs/matrixfs.h"
#include "mfs_internal.h" /* mf_t es OPACO en include/matrixfs */
#include "matrixfs_stm32.h"

/* La instancia del nucleo debe sobrevivir mientras el volumen este montado.
 * Son ~17 KB de .bss en el build de 32 bits por defecto: declararla global (no
 * en la pila de una tarea) evita desbordar el stack y lo deja visible en el .map.
 * Ver README §Presupuesto de RAM: es el mayor consumidor individual. */
static mf_t g_fs;

/* -------------------------------------------------------------------------
 * A) Montaje con la configuracion compilada (mfs_stm32_conf.h)
 * ------------------------------------------------------------------------- */
static int ejemplo_montaje_con_conf(void) {
  mfs_st mfs = matrixfs_stm32_mount_default(&g_fs);
  if (mfs != MFS_OK) {
    /* matrixfs_stm32_last_error() da un mensaje legible y acotado. */
    printf("matrixfs: montaje fallido: %s\r\n", matrixfs_stm32_last_error());
    return -1;
  }

  printf("matrixfs: montado (%s), capacidad util %lu B\r\n",
         matrixfs_stm32_media_name((mfs_stm32_media_t)MFS_STM32_MEDIA),
         (unsigned long)matrixfs_stm32_capacity_bytes());
  if (matrixfs_stm32_capacity_wasteful())
    printf("matrixfs: AVISO: la region reservada es mayor que la capacidad "
           "util (sube MFS_ZONE_MAX si necesitas mas)\r\n");

  /* Diagnostico del HWV resuelto: modo, suite y geometria reales. */
  const mfs_hwv_t *h = matrixfs_stm32_hwv();
  if (h) {
    printf("matrixfs: erase_unit=%lu B, pgm_gran=%lu B, ECC=%s\r\n",
           (unsigned long)h->erase_unit,
           (unsigned long)h->program_granularity,
           (h->flags0 & MFS_HWV0_ECC_ON_DIE) ? "si" : "no");
  }
  return 0;
}

/* -------------------------------------------------------------------------
 * B) Montaje explicito (sin mfs_stm32_conf.h)
 * ------------------------------------------------------------------------- */
static int ejemplo_montaje_explicito(void) {
  mfs_stm32_cfg c;
  memset(&c, 0, sizeof(c));

  c.media = MFS_STM32_MEDIA_OSPI_NOR;
  c.ospi = (void *)&hospi1; /* el handle que declara CubeMX */
  c.ram_total = 64u * 1024u;
  c.forced_mode = MFS_MODE_UNSUPPORTED; /* negociar por viabilidad */
  c.format_if_needed = true;            /* primer arranque */
  c.memory_mapped = false;              /* lectura indirecta (sin cache) */

  mfs_st mfs = matrixfs_stm32_mount(&g_fs, &c);
  if (mfs != MFS_OK)
    return -1;
  return 0;
}

/* -------------------------------------------------------------------------
 * Escritura y lectura
 *
 * AVISO DE CONCURRENCIA: el nucleo NO es reentrante. Si el volumen se usa desde
 * varias tareas de FreeRTOS, hay que serializar estas llamadas. Con
 * matrixfs_stm32_set_lock_hooks() se le dice al port cual es el mutex; y
 * matrixfs_stm32_lock()/unlock() permiten agrupar operaciones compuestas.
 * ------------------------------------------------------------------------- */
static int ejemplo_escribir_y_leer(void) {
  static const char texto[] = "Hola desde MatrixFS en STM32Cube";
  mfs_file *f = NULL;

  matrixfs_stm32_lock();
  mfs_st st = (mfs_st)mf_open(&g_fs, "/hola.txt",
                              MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &f);
  if (st == MFS_OK) {
    size_t wr = 0;
    st = (mfs_st)mf_write(f, texto, sizeof(texto) - 1u, &wr);
    (void)mf_close(f);
  }
  /* mf_sync persiste el WAL: sin el, los datos estan en el medio pero el
   * commit puede no estar cerrado. */
  if (st == MFS_OK)
    st = (mfs_st)mf_sync(&g_fs);
  matrixfs_stm32_unlock();

  if (st != MFS_OK) {
    printf("matrixfs: escritura fallida: %s\r\n", mfs_ststr(st));
    return -1;
  }

  char buf[64] = {0};
  matrixfs_stm32_lock();
  st = (mfs_st)mf_open(&g_fs, "/hola.txt", MFS_O_RDONLY, &f);
  if (st == MFS_OK) {
    size_t rd = 0;
    st = (mfs_st)mf_read(f, buf, sizeof(buf) - 1u, &rd);
    (void)mf_close(f);
  }
  matrixfs_stm32_unlock();

  if (st != MFS_OK) {
    printf("matrixfs: lectura fallida: %s\r\n", mfs_ststr(st));
    return -1;
  }
  printf("matrixfs: leido \"%s\"\r\n", buf);
  return 0;
}

/* -------------------------------------------------------------------------
 * Punto de entrada del ejemplo
 *
 * En un main.c de CubeMX, llamar a esto DESPUES de MX_*_Init() (y despues de
 * osKernelInitialize()/osKernelStart() si se usa RTOS) y antes del bucle
 * principal.
 * ------------------------------------------------------------------------- */
void app_matrixfs_demo(void) {
  if (ejemplo_montaje_explicito() != 0) {
    printf("matrixfs: %s\r\n", matrixfs_stm32_last_error());
    return;
  }

  if (ejemplo_escribir_y_leer() != 0)
    return;

  /* Desmontaje ordenado: cierra el WAL y ceroiza el estado del nucleo
   * (mf_deinit hace sync final + ceroizacion, §15 SEC-002). */
  (void)matrixfs_stm32_deinit(&g_fs);

  /* Si se quiere formatear (DESTRUYE el contenido):
   *   mfs_stm32_cfg c = ...; matrixfs_stm32_format(&g_fs, &c);
   */
}
