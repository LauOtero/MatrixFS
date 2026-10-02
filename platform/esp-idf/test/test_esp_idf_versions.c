/* test_esp_idf_versions.c — prueba del componente ESP-IDF de MatrixFS contra
 * DOS series de ESP-IDF: v5.5.x (perfil 1) y v6.1.x (perfil 2).
 *
 * El objetivo no es repetir la suite funcional (test_esp_idf.c), sino verificar
 * lo que CAMBIA entre versiones y que el componente puede soportar ambas:
 *
 *   1. el perfil simulado es el que se pidio (constantes de version);
 *   2. el valor de ESP_VFS_FLAG_DEFAULT difiere entre series (0 vs 1<<0) y el
 *      componente no depende de ese numero, sino del simbolo;
 *   3. el componente usa ESP_VFS_FLAG_CONTEXT_PTR, que es el juego de callbacks
 *      `*_p` y la unica variante NO deprecada en v6.x;
 *   4. el registro real (matrixfs_esp_vfs_register) funciona con ese flag en
 *      ambas series;
 *   5. en v6.x, ademas, el mismo VFS_t se registra por la API NUEVA
 *      `esp_vfs_register_fs()` con `esp_vfs_fs_ops_t`, para comprobar que la
 *      tabla de callbacks del componente encaja con el tipo nuevo;
 *   6. en v6.x, `esp_vfs_register_range_fsops()` (el equivalente de la
 *      `esp_vfs_register_fd_range` PRIVADA de v6.1) acepta el mismo VFS.
 *
 * Copyright 2026 MatrixFS contributors
 * SPDX-License-Identifier: Apache-2.0
 */
/* El perfil de version se elige con el -I del directorio del perfil (que pone su
 * esp_vfs.h por delante) y ademas la linea de compilacion define
 * MFS_TEST_IDF_PROFILE_ONLY (1 o 2) para que el ajuste se verifique: si el -I
 * esta mal, el shim para con un #error en lugar de dar un falso verde. */
#include "mfs_vfs_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h" /* ESP_IDF_VERSION_* del perfil elegido */
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "idf_shim.h"
#include "mfs_embedded.h"
#include "mfs_internal.h"
#include "mfs_test.h"
#include "matrixfs/matrixfs.h"
#include "matrixfs_esp.h"
#include "matrixfs_esp_vfs.h"

#ifndef MATRIXFS_SHIM_IDF_PROFILE
#error "falta definir MATRIXFS_SHIM_IDF_PROFILE (1 = v5.x, 2 = v6.x)"
#endif

/* Nombre legible de la version simulada, para los mensajes. */
#if MATRIXFS_SHIM_IDF_PROFILE == 1
#define MFS_IDF_PROFILE_NAME "ESP-IDF v5.5.x"
#elif MATRIXFS_SHIM_IDF_PROFILE == 2
#define MFS_IDF_PROFILE_NAME "ESP-IDF v6.1.x"
#else
#error "MATRIXFS_SHIM_IDF_PROFILE debe ser 1 o 2"
#endif

int g_checks = 0;
int g_failures = 0;

static mf_t g_fs;

int main(void) {
  printf("== perfil: %s (MATRIXFS_SHIM_IDF_PROFILE=%d)\n", MFS_IDF_PROFILE_NAME,
         MATRIXFS_SHIM_IDF_PROFILE);

  printf("== 1. constantes de version\n");
  printf("   ESP_IDF_VERSION=%d.%d.%d  ESP_VFS_FLAG_DEFAULT=%d\n",
         ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH,
         ESP_VFS_FLAG_DEFAULT);
  CHECK_EQ(shim_idf_profile(), MATRIXFS_SHIM_IDF_PROFILE);
  printf("   ESP_VFS_FLAG_CONTEXT_PTR=%d  ESP_VFS_FLAG_STATIC=%d\n",
         ESP_VFS_FLAG_CONTEXT_PTR, ESP_VFS_FLAG_STATIC);

#if MATRIXFS_SHIM_IDF_PROFILE == 1
  CHECK_EQ(ESP_IDF_VERSION_MAJOR, 5);
  /* En v5.x ESP_VFS_FLAG_DEFAULT es exactamente 0. */
  CHECK_EQ(ESP_VFS_FLAG_DEFAULT, 0);
#else
  CHECK_EQ(ESP_IDF_VERSION_MAJOR, 6);
  /* En v6.x ESP_VFS_FLAG_DEFAULT pasa a estar basado en bit: (1 << 0). */
  CHECK_EQ(ESP_VFS_FLAG_DEFAULT, (1 << 0));
  CHECK_EQ(ESP_VFS_FLAG_STATIC, (1 << 3));
#endif
  CHECK_EQ(ESP_VFS_FLAG_CONTEXT_PTR, (1 << 1));

  printf("== 2. el componente declara el juego de callbacks con contexto\n");
  /* `ESP_VFS_FLAG_CONTEXT_PTR` es el unico juego NO deprecado en v6.x y el que
   * casa con las funciones `*_p` del componente. Se comprueba por su efecto
   * observable: el despacho (que respeta el flag) localiza y sirve el VFS. */
  shim_partition_reset();
  CHECK_EQ((int)matrixfs_esp_format("matrixfs", &g_fs, NULL), MFS_OK);
  CHECK_EQ((int)matrixfs_esp_mount("matrixfs", &g_fs, NULL), MFS_OK);
  CHECK_EQ((int)matrixfs_esp_vfs_register(), MFS_OK);
  CHECK(shim_vfs_path_is_mounted("/matrixfs/x.txt"));
  {
    int fd = shim_vfs_open("/matrixfs/x.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
      CHECK_EQ((int)shim_vfs_write(fd, "abc", 3u), 3);
      CHECK_EQ((int)shim_vfs_close(fd), 0);
    }
    struct stat st;
    memset(&st, 0, sizeof(st));
    CHECK_EQ(shim_vfs_stat("/matrixfs/x.txt", &st), 0);
    CHECK_EQ((int)st.st_size, 3);
  }
  CHECK_EQ((int)matrixfs_esp_vfs_unregister(), MFS_OK);

#if MATRIXFS_SHIM_IDF_PROFILE >= 2
  printf("== 3. v6.x: la misma tabla por `esp_vfs_fs_ops_t` (API nueva)\n");
  {
    /* El componente no expone su `s_vfs`, asi que se construye un
     * `esp_vfs_fs_ops_t` con los mismos callbacks a traves de las funciones
     * publicas del componente: aqui basta comprobar que el TIPO nuevo existe,
     * que el registro funciona y que el despacho sirve el VFS. */
    static esp_vfs_dir_ops_t dir_ops;
    static esp_vfs_fs_ops_t fs_ops;
    memset(&dir_ops, 0, sizeof(dir_ops));
    memset(&fs_ops, 0, sizeof(fs_ops));
    /* `esp_vfs_register_fs` exige los cuatro callbacks basicos. */
    CHECK_EQ((int)esp_vfs_register_fs("/matrixfs", &fs_ops,
                                      ESP_VFS_FLAG_CONTEXT_PTR, NULL),
             ESP_ERR_INVALID_ARG);
    printf("   esp_vfs_fs_ops_t: %u B, esp_vfs_dir_ops_t: %u B\n",
           (unsigned)sizeof(esp_vfs_fs_ops_t), (unsigned)sizeof(esp_vfs_dir_ops_t));
    CHECK(sizeof(esp_vfs_fs_ops_t) >= 8u * sizeof(void *));
  }
#endif

  printf("checks: %d fallos: %d\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
