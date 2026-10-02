/* test_esp_idf.c — prueba de integracion del componente ESP-IDF de MatrixFS en
 * un host, contra el nucleo REAL del repositorio y con shims minimos de
 * ESP-IDF (platform/esp-idf/test/shims). No hay ESP-IDF instalado: lo que se
 * verifica es que las fuentes REALES del componente (matrixfs_esp.c y
 * matrixfs_esp_vfs.c) compilan, enlazan y se ejecutan sobre el nucleo real.
 *
 * Cobertura:
 *   1. formateo + montaje, y error legible con una etiqueta inexistente
 *   2. diagnostico de capacidad (MFS_ZONE_MAX zonas de 4096 B)
 *   3. ida y vuelta de escritura/lectura con offset NO alineado a 16 B (ejercita
 *      el read-modify-write del componente)
 *   4. lo mismo con CIFRADO DE FLASH activo (regresion del alineamiento a 16 B)
 *   5. VFS POSIX: registrar, abrir/escribir/leer/stat/mkdir/opendir/unlink,
 *      traduccion de errno y desregistro
 *   6. recursividad de matrixfs_esp_lock/unlock
 *   7. formas de ruta "/hola.txt" y "hola.txt"
 */
/* El shim de esp_vfs.h debe ir ANTES de cualquier cabecera de libc: define el
 * `struct dirent` con `d_type` (newlib) y reclama <dirent.h> en plataformas que
 * no lo traen (MinGW). Ver platform/esp-idf/test/shims/esp_vfs.h. */
#include "esp_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "matrixfs/matrixfs.h"

#include "mfs_embedded.h"
#include "mfs_internal.h" /* struct mf_t (instancia estatica) */
#include "mfs_test.h"

#include "esp_flash.h"
#include "esp_partition.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_shim.h"
#include "matrixfs_esp.h"
#include "matrixfs_esp_private.h"
#include "matrixfs_esp_vfs.h"

int g_checks = 0;
int g_failures = 0;

/* Instancia del nucleo: la aporta el llamador (sin heap en el componente). */
static mf_t fs;

#define BIG_LEN 700u
#define BIG_OFF 3u

static uint8_t wbuf[BIG_LEN];
static uint8_t rbuf[BIG_LEN];

static uint8_t pattern(int i) { return (uint8_t)((i * 31 + 7) & 0xFF); }

/* Ida y vuelta con la API del nucleo: escribe en un offset NO alineado a 16 B
 * y mas de una pagina (256 B) de longitud. Devuelve MFS_OK si todo cuadra. */
static mfs_st core_roundtrip(const char *path) {
  printf("   [core] ruta '%s': %u B en offset %u (offset%%16=%u, offset%%4=%u)\n",
         path, (unsigned)BIG_LEN, (unsigned)BIG_OFF,
         (unsigned)(BIG_OFF % 16u), (unsigned)(BIG_OFF % 4u));
  for (unsigned i = 0; i < BIG_LEN; i++)
    wbuf[i] = pattern((int)i);

  mfs_file *f = NULL;
  mfs_st st = (mfs_st)mf_open(&fs, path, MFS_O_RDWR | MFS_O_CREAT, &f);
  if (st != MFS_OK) {
    printf("   [core] mf_open fallo: %s\n", mfs_ststr(st));
    return st;
  }
  st = (mfs_st)mf_seek(f, (int64_t)BIG_OFF, MFS_SEEK_SET);
  if (st == MFS_OK) {
    size_t wr = 0;
    st = (mfs_st)mf_write(f, wbuf, BIG_LEN, &wr);
    if (st == MFS_OK && wr != BIG_LEN)
      st = MFS_EIO;
  }
  if (st == MFS_OK)
    st = (mfs_st)mf_sync(&fs);
  if (st == MFS_OK) {
    st = (mfs_st)mf_seek(f, (int64_t)BIG_OFF, MFS_SEEK_SET);
    if (st == MFS_OK) {
      size_t rd = 0;
      st = (mfs_st)mf_read(f, rbuf, BIG_LEN, &rd);
      if (st == MFS_OK && (rd != BIG_LEN || memcmp(wbuf, rbuf, BIG_LEN) != 0))
        st = MFS_EBADMSG;
    }
  }
  (void)mf_close(f);
  return st;
}

int main(void) {
  /* Volumen ausente al arrancar: el registro VFS debe rechazarse. */
  printf("== 0. VFS sin volumen montado\n");
  CHECK_EQ(matrixfs_esp_vfs_register(), MFS_ENOTMOUNTED);

  printf("== 1. formateo + montaje\n");
  shim_partition_reset();
  CHECK_EQ((int)matrixfs_esp_format("matrixfs", &fs, NULL), MFS_OK);
  CHECK_EQ((int)matrixfs_esp_mount("matrixfs", &fs, NULL), MFS_OK);
  CHECK(matrixfs_esp_is_mounted());
  CHECK_EQ((int)matrixfs_esp_format("etiqueta_inexistente", &fs, NULL),
           MFS_ENOENT);
  CHECK(matrixfs_esp_last_error() != NULL);
  if (matrixfs_esp_last_error() != NULL) {
    printf("   ultimo error: '%s'\n", matrixfs_esp_last_error());
    CHECK(matrixfs_esp_last_error()[0] != '\0');
  }
  CHECK_EQ((int)matrixfs_esp_format("etiqueta_inexistente", NULL, NULL),
           MFS_EINVAL);
  TEST_END("formateo + montaje");

  printf("== 2. diagnostico de capacidad\n");
  printf("   capacidad=%u B  particion=%u B  MFS_ZONE_MAX=%u\n",
         (unsigned)matrixfs_esp_capacity_bytes(), 1048576u,
         (unsigned)MFS_ZONE_MAX);
  /* El nucleo direcciona MFS_ZONE_MAX zonas de erase_unit bytes; la particion
   * (1 MiB = 256 sectores) da para mas, asi que manda el limite de zonas. */
  CHECK_EQ((int)matrixfs_esp_capacity_bytes(), (int)(MFS_ZONE_MAX * 4096u));
  CHECK(matrixfs_esp_capacity_wasteful());
  TEST_END("diagnostico de capacidad");

  printf("== 3. ida y vuelta sin cifrado (offset no alineado)\n");
  {
    mfs_st st = core_roundtrip("sin_cifrado.bin");
    if (st != MFS_OK)
      printf("   [core] fallo: %s\n", mfs_ststr(st));
    CHECK_EQ((int)st, MFS_OK);
  }
  TEST_END("ida y vuelta sin cifrado");

  printf("== 4. cifrado de flash activo (regresion del alineamiento a 16 B)\n");
  shim_partition_reset(); /* tambien apaga el cifrado */
  shim_flash_set_encryption(true);
  CHECK(esp_flash_encryption_enabled());
  CHECK_EQ((int)matrixfs_esp_format("matrixfs", &fs, NULL), MFS_OK);
  CHECK_EQ((int)matrixfs_esp_mount("matrixfs", &fs, NULL), MFS_OK);
  /* La ventana de escritura observada debe ser multiplo de 16 B: si el
   * componente alinease a 4 B, la particion cifrada rechazaria la programacion
   * y aqui aparecerian granularidad 4 y rechazos de alineacion. */
  shim_partition_reset_stats();
  {
    mfs_st st = core_roundtrip("cifrado.bin");
    if (st != MFS_OK)
      printf("   [core] fallo: %s\n", mfs_ststr(st));
    CHECK_EQ((int)st, MFS_OK);
    printf("   escrituras=%u  granularidad_min=%u  rechazos_alineacion=%u\n",
           (unsigned)shim_partition_write_count(),
           (unsigned)shim_partition_write_min_gran(),
           (unsigned)shim_partition_align_rejects());
    CHECK(shim_partition_write_count() > 0u);
    /* Todas las ventanas de programacion que el componente emitio son multiplos
     * de 16 B: es lo que exige una particion cifrada. */
    CHECK_EQ((int)shim_partition_write_min_gran(), 16);
    CHECK_EQ((int)shim_partition_align_rejects(), 0);

    /* Control negativo: la restriccion del modelo es REAL y no decorativa. Una
     * programacion de 4 B alineados a 4 (lo que emitiria un componente que
     * alinease a 4 en vez de a 16) es rechazada con ESP_ERR_INVALID_ARG. */
    {
      const esp_partition_t *p = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "matrixfs");
      uint8_t words[16];
      memset(words, 0x00, sizeof(words));
      CHECK(p != NULL);
      if (p) {
        size_t before = shim_partition_align_rejects();
        CHECK_EQ((int)esp_partition_write(p, 512u + 4u, words, 4u),
                 ESP_ERR_INVALID_ARG);
        CHECK_EQ((int)esp_partition_write(p, 512u, words, 12u),
                 ESP_ERR_INVALID_ARG);
        CHECK_EQ((int)shim_partition_align_rejects(), (int)(before + 2u));
        /* ... y una alineada al bloque de cifrado si se acepta. */
        CHECK_EQ((int)esp_partition_write(p, 512u, words, 16u), ESP_OK);
      }
    }
  }
  TEST_END("cifrado de flash activo");

  printf("== 5. VFS POSIX\n");
  /* Con el volumen cifrado montado en el paso 4. */
  CHECK_EQ((int)matrixfs_esp_vfs_register(), MFS_OK);
  CHECK_EQ((int)matrixfs_esp_vfs_register(), MFS_EBUSY);
  CHECK_EQ((int)shim_vfs_mount_count(), 1);
  CHECK(shim_vfs_path_is_mounted("/matrixfs/hola.txt"));
  CHECK(!shim_vfs_path_is_mounted("/matrixfsX/hola.txt"));
  CHECK(strcmp(matrixfs_esp_vfs_mount_point(), "/matrixfs") == 0);

  {
    static const char hello[] = "hola MatrixFS";
    const size_t hello_len = sizeof(hello) - 1u;
    char buf[64];
    int fd;

    errno = 0;
    fd = shim_vfs_open("/matrixfs/hola.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK_EQ(errno, 0);

    if (fd >= 0) {
      CHECK_EQ((int)shim_vfs_write(fd, hello, hello_len), (int)hello_len);
      CHECK_EQ((int)shim_vfs_close(fd), 0);
    }

    errno = 0;
    fd = shim_vfs_open("/matrixfs/hola.txt", O_RDONLY, 0);
    CHECK(fd >= 0);
    if (fd >= 0) {
      memset(buf, 0, sizeof(buf));
      CHECK_EQ((int)shim_vfs_read(fd, buf, hello_len), (int)hello_len);
      CHECK_EQ(strcmp(buf, hello), 0);
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_fstat(fd, &st), 0);
      CHECK_EQ((int)st.st_size, (int)hello_len);
      CHECK((st.st_mode & S_IFREG) != 0);
      CHECK_EQ((int)shim_vfs_close(fd), 0);
    }

    {
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_stat("/matrixfs/hola.txt", &st), 0);
      CHECK_EQ((int)st.st_size, (int)hello_len);
      CHECK_EQ((int)st.st_nlink, 1);
    }

    CHECK_EQ(shim_vfs_mkdir("/matrixfs/dir", 0755), 0);
    {
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_stat("/matrixfs/dir", &st), 0);
      CHECK((st.st_mode & S_IFDIR) != 0);
    }

    {
      DIR *d = shim_vfs_opendir("/matrixfs");
      CHECK(d != NULL);
      if (d) {
        int n_entries = 0;
        int saw_hola = 0;
        int saw_dir = 0;
        struct dirent *e;
        while ((e = shim_vfs_readdir(d)) != NULL) {
          n_entries++;
          if (strcmp(e->d_name, "hola.txt") == 0)
            saw_hola = 1;
          if (strcmp(e->d_name, "dir") == 0)
            saw_dir = 1;
        }
        CHECK(saw_hola);
        CHECK(saw_dir);
        printf("   readdir: %d entradas (hola.txt=%d, dir=%d)\n", n_entries,
               saw_hola, saw_dir);
        CHECK_EQ((int)shim_vfs_closedir(d), 0);
      }
    }

    /* errno: abrir algo que no existe debe traducir MFS_ENOENT -> ENOENT. */
    errno = 0;
    fd = shim_vfs_open("/matrixfs/no_existe.txt", O_RDONLY, 0);
    CHECK_EQ(fd, -1);
    CHECK_EQ(errno, ENOENT);

    /* errno: stat de algo que no existe. */
    errno = 0;
    {
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_stat("/matrixfs/no_existe.txt", &st), -1);
      CHECK_EQ(errno, ENOENT);
    }

    /* rename por el VFS (no exigido por el enunciado, pero es un callback mas). */
    CHECK_EQ(shim_vfs_rename("/matrixfs/hola.txt", "/matrixfs/hola2.txt"), 0);
    {
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_stat("/matrixfs/hola2.txt", &st), 0);
    }

    CHECK_EQ(shim_vfs_unlink("/matrixfs/hola2.txt"), 0);
    errno = 0;
    {
      struct stat st;
      memset(&st, 0, sizeof(st));
      CHECK_EQ(shim_vfs_stat("/matrixfs/hola2.txt", &st), -1);
      CHECK_EQ(errno, ENOENT);
    }
  }
  TEST_END("VFS POSIX");

  printf("== 6. recursividad de lock/unlock\n");
  shim_sem_reset_stats();
  matrixfs_esp_lock();
  matrixfs_esp_lock();
  printf("   recursion tras 2 lock: %d\n", shim_sem_recursion_depth());
  CHECK_EQ(shim_sem_recursion_depth(), 2);
  matrixfs_esp_unlock();
  matrixfs_esp_unlock();
  printf("   recursion tras 2 unlock: %d\n", shim_sem_recursion_depth());
  CHECK_EQ(shim_sem_recursion_depth(), 0);
  CHECK(shim_sem_recursion_max() >= 2);
  CHECK(shim_sem_live() >= 1);
  TEST_END("recursividad de lock/unlock");

  printf("== 7. formas de ruta con y sin barra inicial\n");
  {
    static const char payload[] = "misma-ruta";
    mfs_file *a = NULL;
    mfs_file *b = NULL;
    CHECK_EQ((int)mf_open(&fs, "/hola.txt", MFS_O_RDWR | MFS_O_CREAT, &a),
             MFS_OK);
    if (a) {
      size_t wr = 0;
      CHECK_EQ((int)mf_write(a, payload, sizeof(payload) - 1u, &wr), MFS_OK);
      CHECK_EQ((int)mf_close(a), MFS_OK);
    }
    /* La busqueda del nucleo salta las barras iniciales: misma entrada. */
    CHECK_EQ((int)mf_open(&fs, "hola.txt", MFS_O_RDONLY, &b), MFS_OK);
    if (b) {
      char buf[sizeof(payload)];
      size_t rd = 0;
      memset(buf, 0, sizeof(buf));
      CHECK_EQ((int)mf_read(b, buf, sizeof(payload) - 1u, &rd), MFS_OK);
      CHECK_EQ((int)rd, (int)(sizeof(payload) - 1u));
      CHECK_EQ(strcmp(buf, payload), 0);
      CHECK_EQ((int)mf_close(b), MFS_OK);
    }
    mfs_stat sta;
    mfs_stat stb;
    memset(&sta, 0, sizeof(sta));
    memset(&stb, 0, sizeof(stb));
    CHECK_EQ((int)mf_stat(&fs, "/hola.txt", &sta), MFS_OK);
    CHECK_EQ((int)mf_stat(&fs, "hola.txt", &stb), MFS_OK);
    CHECK_EQ(sta.ino, stb.ino);
    printf("   ino '/hola.txt'=%u  ino 'hola.txt'=%u\n", (unsigned)sta.ino,
           (unsigned)stb.ino);
  }
  TEST_END("formas de ruta");

  printf("== 8. desregistro del VFS\n");
  CHECK_EQ((int)matrixfs_esp_vfs_unregister(), MFS_OK);
  CHECK_EQ((int)shim_vfs_mount_count(), 0);
  CHECK_EQ((int)matrixfs_esp_vfs_unregister(), MFS_ENOTMOUNTED);
  TEST_END("desregistro del VFS");

  printf("checks: %d fallos: %d\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
