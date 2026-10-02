/* idf_shim.c — implementacion del shim de ESP-IDF para las pruebas de host.
 *
 * Contiene todo lo que el componente MatrixFS necesita de IDF y que en un host
 * no existe:
 *   · reloj monotonico (clock_gettime) para esp_timer/esp_cpu;
 *   · nombre legible de los esp_err_t;
 *   · estado de cifrado de flash (conmutable por el test);
 *   · FreeRTOS minimo con mutex recursivo REAL (contador de recursion);
 *   · registro VFS (las dos generaciones de API) y despacho POSIX.
 *
 * El despacho imita a IDF:
 *   · busca el montaje cuyo punto de montaje es prefijo de la ruta, con el
 *     limite de componente respetado ("/matrixfsX" no cuelga de "/matrixfs");
 *   · entrega al callback la ruta SIN ese prefijo;
 *   · TRADUCE DESCRIPTORES: el driver ve descriptores LOCALES (los que devuelve
 *     su `open_p`) y la aplicacion ve descriptores GLOBALES. IDF mantiene esa
 *     tabla; el shim la reproduce porque es justo lo que valida
 *     `vfs_slot_of_fd()` en matrixfs_esp_vfs.c.
 */
#include "esp_vfs.h"

#include "idf_shim.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Prototipos de la parte de v6 (esp_vfs_fs_ops_t solo existe alli). */
#if MATRIXFS_SHIM_IDF_PROFILE >= 2
#include "esp_private/socket.h"
esp_err_t esp_vfs_register_fs(const char *base_path,
                              const esp_vfs_fs_ops_t *vfs, int flags, void *ctx);
esp_err_t esp_vfs_register_range_fsops(const esp_vfs_fs_ops_t *vfs, int flags,
                                       void *ctx, int min_fd, int max_fd);
esp_err_t esp_vfs_register_with_id(const esp_vfs_t *vfs, void *ctx, int *vfs_id);
esp_err_t esp_vfs_register_fd(int vfs_id, int *fd);
esp_err_t esp_vfs_unregister_with_id(int vfs_id);
#else
esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx, int min_fd,
                                    int max_fd);
#endif

/* ============================== Reloj ================================== */

static uint64_t shim_now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0u;
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int64_t esp_timer_get_time(void) {
  return (int64_t)shim_now_us();
}

uint32_t esp_cpu_get_cycle_count(void) {
  /* Base de 100 MHz sobre el mismo reloj monotonico: monotonico, no decreciente
   * y truncado a 32 bits como en el target. */
  return (uint32_t)(shim_now_us() * 100u);
}

/* ======================== Codigos de error ============================= */

const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_FAIL:
    return "ESP_FAIL";
  case ESP_ERR_NO_MEM:
    return "ESP_ERR_NO_MEM";
  case ESP_ERR_INVALID_ARG:
    return "ESP_ERR_INVALID_ARG";
  case ESP_ERR_INVALID_STATE:
    return "ESP_ERR_INVALID_STATE";
  case ESP_ERR_INVALID_SIZE:
    return "ESP_ERR_INVALID_SIZE";
  case ESP_ERR_NOT_FOUND:
    return "ESP_ERR_NOT_FOUND";
  case ESP_ERR_NOT_SUPPORTED:
    return "ESP_ERR_NOT_SUPPORTED";
  case ESP_ERR_TIMEOUT:
    return "ESP_ERR_TIMEOUT";
  default:
    return "ESP_ERR_UNKNOWN";
  }
}

/* ====================== Cifrado de flash ============================== */

static bool s_flash_encrypted = false;
static unsigned s_enc_queries = 0u;

bool esp_flash_encryption_enabled(void) {
  s_enc_queries++;
  return s_flash_encrypted;
}

void shim_flash_set_encryption(bool on) { s_flash_encrypted = on; }

unsigned shim_flash_encryption_queries(void) { return s_enc_queries; }

/* ======================= FreeRTOS minimo =============================== */

void shim_port_enter_critical(portMUX_TYPE *mux) {
  if (!mux)
    return;
  mux->nesting++;
  mux->entries++;
}

void shim_port_exit_critical(portMUX_TYPE *mux) {
  if (!mux)
    return;
  if (mux->nesting > 0)
    mux->nesting--;
  mux->exits++;
}

/* Mutex recursivo REAL: ranura estatica con contador de recursion. */
struct shim_sem_s {
  bool used;
  int depth;
  int max_depth;
  unsigned takes;
  unsigned gives;
};

#define SHIM_SEM_SLOTS 8
static struct shim_sem_s s_sems[SHIM_SEM_SLOTS];
static int s_last_sem = -1;

static struct shim_sem_s *shim_sem_alloc(void) {
  for (int i = 0; i < SHIM_SEM_SLOTS; i++) {
    if (!s_sems[i].used) {
      memset(&s_sems[i], 0, sizeof(s_sems[i]));
      s_sems[i].used = true;
      s_last_sem = i;
      return &s_sems[i];
    }
  }
  return NULL;
}

static int shim_sem_index(SemaphoreHandle_t sem) {
  if (!sem)
    return -1;
  for (int i = 0; i < SHIM_SEM_SLOTS; i++) {
    if (&s_sems[i] == sem)
      return i;
  }
  return -1;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
  return (SemaphoreHandle_t)shim_sem_alloc();
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  return (SemaphoreHandle_t)shim_sem_alloc();
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  return (SemaphoreHandle_t)shim_sem_alloc();
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks) {
  (void)ticks;
  int i = shim_sem_index(sem);
  if (i < 0)
    return pdFALSE;
  s_sems[i].depth++;
  s_sems[i].takes++;
  if (s_sems[i].depth > s_sems[i].max_depth)
    s_sems[i].max_depth = s_sems[i].depth;
  s_last_sem = i;
  return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem) {
  int i = shim_sem_index(sem);
  if (i < 0)
    return pdFALSE;
  if (s_sems[i].depth > 0)
    s_sems[i].depth--;
  s_sems[i].gives++;
  s_last_sem = i;
  return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
  return xSemaphoreTakeRecursive(sem, ticks);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
  return xSemaphoreGiveRecursive(sem);
}

void vSemaphoreDelete(SemaphoreHandle_t sem) {
  int i = shim_sem_index(sem);
  if (i < 0)
    return;
  memset(&s_sems[i], 0, sizeof(s_sems[i]));
  if (s_last_sem == i)
    s_last_sem = -1;
}

int shim_sem_recursion_depth(void) {
  return (s_last_sem >= 0) ? s_sems[s_last_sem].depth : 0;
}

int shim_sem_recursion_max(void) {
  int m = 0;
  for (int i = 0; i < SHIM_SEM_SLOTS; i++) {
    if (s_sems[i].used && s_sems[i].max_depth > m)
      m = s_sems[i].max_depth;
  }
  return m;
}

int shim_sem_live(void) {
  int n = 0;
  for (int i = 0; i < SHIM_SEM_SLOTS; i++) {
    if (s_sems[i].used)
      n++;
  }
  return n;
}

void shim_sem_reset_stats(void) {
  for (int i = 0; i < SHIM_SEM_SLOTS; i++) {
    s_sems[i].max_depth = s_sems[i].depth;
    s_sems[i].takes = 0u;
    s_sems[i].gives = 0u;
  }
}

static int s_task_sentinel;

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
  return (TaskHandle_t)&s_task_sentinel;
}

const char *pcTaskGetName(TaskHandle_t task) {
  (void)task;
  return "shim_main";
}

void taskYIELD(void) {}

/* ============================ Registro VFS =============================
 *
 * Tabla de montajes y tabla de descriptores. Un descriptOR GLOBAL
 * (el que ve la aplicacion) se traduce a LOCAL (el que ve el driver) restando
 * `fd_base` del montaje. */

#define SHIM_VFS_MAX_MOUNTS 8
#define SHIM_VFS_MAX_FDS 256
#define SHIM_VFS_MAX_DIRS 16
/* Base de los descriptores GLOBALES cuando el montaje se registra con
 * `esp_vfs_register()` (IDF elige numeros libres; el shim usa un rango alto y
 * fijo, sin colisionar con stdio). */
#define SHIM_FD_BASE_DEFAULT 64

typedef struct {
  bool used;
  const esp_vfs_t *legacy; /* camino esp_vfs_t (v5.5 y v6.1 legacy) */
#if MATRIXFS_SHIM_IDF_PROFILE >= 2
  const esp_vfs_fs_ops_t *fs; /* camino esp_vfs_fs_ops_t (v6) */
#endif
  bool is_fs;
  void *ctx;
  bool ctx_ptr;
  int fd_base;
  int min_fd;
  int max_fd;
  char base[64];
} shim_mount_t;

typedef struct {
  bool used;
  bool open;
  int mount;
} shim_fd_t;

typedef struct {
  bool used;
  int mount;
  shim_dir_handle_t os;
} shim_dir_slot_t;

static shim_mount_t s_mounts[SHIM_VFS_MAX_MOUNTS];
static shim_fd_t s_fds[SHIM_VFS_MAX_FDS];
static shim_dir_slot_t s_dir_slots[SHIM_VFS_MAX_DIRS];
static int s_vfs_ids[SHIM_VFS_MAX_MOUNTS];

int shim_idf_profile(void) { return MATRIXFS_SHIM_IDF_PROFILE; }

/* Contexto que se pasa a los callbacks segun ESP_VFS_FLAG_CONTEXT_PTR. */
static void *shim_cb_ctx(const shim_mount_t *m) {
  return m->ctx_ptr ? m->ctx : NULL;
}

/* Tabla de operaciones del camino v6 (`fs` + su subtabla `dir`). */
typedef struct {
  ssize_t (*write_p)(void *, int, const void *, size_t);
  off_t (*lseek_p)(void *, int, off_t, int);
  ssize_t (*read_p)(void *, int, void *, size_t);
  int (*open_p)(void *, const char *, int, int);
  int (*close_p)(void *, int);
  int (*fstat_p)(void *, int, struct stat *);
  int (*fsync_p)(void *, int);
  int (*stat_p)(void *, const char *, struct stat *);
  int (*unlink_p)(void *, const char *);
  int (*rename_p)(void *, const char *, const char *);
  int (*mkdir_p)(void *, const char *, mode_t);
  DIR *(*opendir_p)(void *, const char *);
  struct dirent *(*readdir_p)(void *, DIR *);
  int (*closedir_p)(void *, DIR *);
} shim_ops_t;

static void shim_ops_of(int m, shim_ops_t *o) {
  const shim_mount_t *mt = &s_mounts[m];
  memset(o, 0, sizeof(*o));
  if (!mt->is_fs) {
    const esp_vfs_t *v = mt->legacy;
    if (!v)
      return;
    o->write_p = v->write_p;
    o->lseek_p = v->lseek_p;
    o->read_p = v->read_p;
    o->open_p = v->open_p;
    o->close_p = v->close_p;
    o->fstat_p = v->fstat_p;
    o->fsync_p = v->fsync_p;
    o->stat_p = v->stat_p;
    o->unlink_p = v->unlink_p;
    o->rename_p = v->rename_p;
    o->mkdir_p = v->mkdir_p;
    o->opendir_p = v->opendir_p;
    o->readdir_p = v->readdir_p;
    o->closedir_p = v->closedir_p;
    return;
  }
#if MATRIXFS_SHIM_IDF_PROFILE >= 2
  const esp_vfs_fs_ops_t *f = mt->fs;
  if (!f)
    return;
  o->write_p = f->write_p;
  o->lseek_p = f->lseek_p;
  o->read_p = f->read_p;
  o->open_p = f->open_p;
  o->close_p = f->close_p;
  o->fstat_p = f->fstat_p;
  o->fsync_p = f->fsync_p;
  o->stat_p = (f->dir && f->dir->stat_p) ? f->dir->stat_p : NULL;
  o->unlink_p = (f->dir && f->dir->unlink_p) ? f->dir->unlink_p : NULL;
  o->rename_p = (f->dir && f->dir->rename_p) ? f->dir->rename_p : NULL;
  o->mkdir_p = (f->dir && f->dir->mkdir_p) ? f->dir->mkdir_p : NULL;
  o->opendir_p = (f->dir && f->dir->opendir_p) ? f->dir->opendir_p : NULL;
  o->readdir_p = (f->dir && f->dir->readdir_p) ? f->dir->readdir_p : NULL;
  o->closedir_p = (f->dir && f->dir->closedir_p) ? f->dir->closedir_p : NULL;
#endif
}

static int shim_mount_free_slot(void) {
  for (int i = 0; i < SHIM_VFS_MAX_MOUNTS; i++) {
    if (!s_mounts[i].used)
      return i;
  }
  return -1;
}

static esp_err_t shim_register_legacy(int idx, const esp_vfs_t *vfs, void *ctx,
                                      const char *base_path, int min_fd,
                                      int max_fd) {
  if (!vfs)
    return ESP_ERR_INVALID_ARG;
  if (!vfs->open_p || !vfs->read_p || !vfs->write_p || !vfs->close_p)
    return ESP_ERR_INVALID_ARG;
  const int known = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_DEFAULT |
                    ESP_VFS_FLAG_READONLY_FS | ESP_VFS_FLAG_STATIC;
  if ((vfs->flags & ~known) != 0) {
    fprintf(stderr, "[shim] esp_vfs_t.flags=0x%x desconocido\n", vfs->flags);
    return ESP_ERR_INVALID_ARG;
  }
  if (base_path && (max_fd > 0) &&
      (min_fd < 0 || max_fd <= min_fd || max_fd > SHIM_VFS_MAX_FDS))
    return ESP_ERR_INVALID_ARG;

  shim_mount_t *mt = &s_mounts[idx];
  memset(mt, 0, sizeof(*mt));
  mt->used = true;
  mt->legacy = vfs;
  mt->is_fs = false;
  mt->ctx = ctx;
  mt->ctx_ptr = (vfs->flags & ESP_VFS_FLAG_CONTEXT_PTR) != 0;
  mt->min_fd = min_fd;
  mt->max_fd = max_fd;
  mt->fd_base = (max_fd > 0) ? min_fd : SHIM_FD_BASE_DEFAULT;
  snprintf(mt->base, sizeof(mt->base), "%s",
           base_path ? base_path : CONFIG_MATRIXFS_VFS_MOUNT_POINT);

  if (max_fd > 0) {
    for (int fd = min_fd; fd < max_fd; fd++) {
      if (s_fds[fd].used && s_fds[fd].mount != idx) {
        memset(mt, 0, sizeof(*mt));
        return ESP_ERR_INVALID_STATE;
      }
      s_fds[fd].used = true;
      s_fds[fd].mount = idx;
    }
  }
  return ESP_OK;
}

/* --- Camino esp_vfs_t (v5.5 y v6.1) --- */

esp_err_t esp_vfs_register(const char *base_path, const esp_vfs_t *vfs,
                           void *ctx) {
  if (!base_path)
    return ESP_ERR_INVALID_ARG;
  int idx = shim_mount_free_slot();
  if (idx < 0)
    return ESP_ERR_NO_MEM;
  return shim_register_legacy(idx, vfs, ctx, base_path, 0, 0);
}

esp_err_t esp_vfs_register_fd_range(const esp_vfs_t *vfs, void *ctx, int min_fd,
                                    int max_fd) {
  int idx = shim_mount_free_slot();
  if (idx < 0)
    return ESP_ERR_NO_MEM;
  return shim_register_legacy(idx, vfs, ctx, CONFIG_MATRIXFS_VFS_MOUNT_POINT,
                             min_fd, max_fd);
}

esp_err_t esp_vfs_register_with_id(const esp_vfs_t *vfs, void *ctx,
                                   int *vfs_id) {
  if (!vfs_id)
    return ESP_ERR_INVALID_ARG;
  int idx = shim_mount_free_slot();
  if (idx < 0)
    return ESP_ERR_NO_MEM;
  esp_err_t err =
      shim_register_legacy(idx, vfs, ctx, CONFIG_MATRIXFS_VFS_MOUNT_POINT, 0, 0);
  if (err != ESP_OK)
    return err;
  s_vfs_ids[idx] = 1;
  *vfs_id = idx;
  return ESP_OK;
}

esp_err_t esp_vfs_register_fd(int vfs_id, int *fd) {
  if (!fd || vfs_id < 0 || vfs_id >= SHIM_VFS_MAX_MOUNTS || !s_vfs_ids[vfs_id] ||
      !s_mounts[vfs_id].used)
    return ESP_ERR_INVALID_ARG;
  for (int i = 0; i < SHIM_VFS_MAX_FDS; i++) {
    if (!s_fds[i].used) {
      s_fds[i].used = true;
      s_fds[i].mount = vfs_id;
      *fd = i;
      return ESP_OK;
    }
  }
  return ESP_ERR_NO_MEM;
}

esp_err_t esp_vfs_unregister(const char *base_path) {
  if (!base_path)
    return ESP_ERR_INVALID_ARG;
  for (int i = 0; i < SHIM_VFS_MAX_MOUNTS; i++) {
    if (s_mounts[i].used && strcmp(s_mounts[i].base, base_path) == 0) {
      /* IDF rechaza el desregistro con descriptores abiertos. */
      for (int fd = 0; fd < SHIM_VFS_MAX_FDS; fd++) {
        if (s_fds[fd].used && s_fds[fd].open && s_fds[fd].mount == i)
          return ESP_ERR_INVALID_STATE;
      }
      for (int fd = 0; fd < SHIM_VFS_MAX_FDS; fd++) {
        if (s_fds[fd].used && s_fds[fd].mount == i) {
          s_fds[fd].used = false;
          s_fds[fd].open = false;
          s_fds[fd].mount = -1;
        }
      }
      memset(&s_mounts[i], 0, sizeof(s_mounts[i]));
      s_vfs_ids[i] = 0;
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_vfs_unregister_with_id(int vfs_id) {
  if (vfs_id < 0 || vfs_id >= SHIM_VFS_MAX_MOUNTS || !s_mounts[vfs_id].used)
    return ESP_ERR_INVALID_STATE;
  memset(&s_mounts[vfs_id], 0, sizeof(s_mounts[vfs_id]));
  s_vfs_ids[vfs_id] = 0;
  return ESP_OK;
}

/* --- Camino esp_vfs_fs_ops_t (solo v6) --- */

#if MATRIXFS_SHIM_IDF_PROFILE >= 2

static esp_err_t shim_register_fs(int idx, const esp_vfs_fs_ops_t *vfs,
                                  int flags, void *ctx, const char *base_path,
                                  int min_fd, int max_fd) {
  if (!vfs)
    return ESP_ERR_INVALID_ARG;
  if (!vfs->open_p || !vfs->read_p || !vfs->write_p || !vfs->close_p)
    return ESP_ERR_INVALID_ARG;
  if (max_fd > 0 &&
      (min_fd < 0 || max_fd <= min_fd || max_fd > SHIM_VFS_MAX_FDS))
    return ESP_ERR_INVALID_ARG;
  shim_mount_t *mt = &s_mounts[idx];
  memset(mt, 0, sizeof(*mt));
  mt->used = true;
  mt->is_fs = true;
  mt->fs = vfs;
  mt->ctx = ctx;
  mt->ctx_ptr = (flags & ESP_VFS_FLAG_CONTEXT_PTR) != 0;
  mt->min_fd = min_fd;
  mt->max_fd = max_fd;
  mt->fd_base = (max_fd > 0) ? min_fd : SHIM_FD_BASE_DEFAULT;
  snprintf(mt->base, sizeof(mt->base), "%s",
           base_path ? base_path : CONFIG_MATRIXFS_VFS_MOUNT_POINT);
  if (max_fd > 0) {
    for (int fd = min_fd; fd < max_fd; fd++) {
      s_fds[fd].used = true;
      s_fds[fd].mount = idx;
    }
  }
  return ESP_OK;
}

esp_err_t esp_vfs_register_fs(const char *base_path,
                              const esp_vfs_fs_ops_t *vfs, int flags,
                              void *ctx) {
  if (!base_path)
    return ESP_ERR_INVALID_ARG;
  int idx = shim_mount_free_slot();
  if (idx < 0)
    return ESP_ERR_NO_MEM;
  return shim_register_fs(idx, vfs, flags, ctx, base_path, 0, 0);
}

esp_err_t esp_vfs_register_range_fsops(const esp_vfs_fs_ops_t *vfs, int flags,
                                       void *ctx, int min_fd, int max_fd) {
  int idx = shim_mount_free_slot();
  if (idx < 0)
    return ESP_ERR_NO_MEM;
  return shim_register_fs(idx, vfs, flags, ctx, CONFIG_MATRIXFS_VFS_MOUNT_POINT,
                          min_fd, max_fd);
}

#endif /* MATRIXFS_SHIM_IDF_PROFILE >= 2 */

int shim_vfs_mount_count(void) {
  int n = 0;
  for (int i = 0; i < SHIM_VFS_MAX_MOUNTS; i++) {
    if (s_mounts[i].used)
      n++;
  }
  return n;
}

/* Indice del montaje mas especifico cuyo punto de montaje es prefijo de `path`
 * respetando el limite de componente; -1 si ninguno aplica. */
static int shim_mount_lookup(const char *path) {
  if (!path)
    return -1;
  int best = -1;
  size_t best_len = 0u;
  for (int i = 0; i < SHIM_VFS_MAX_MOUNTS; i++) {
    if (!s_mounts[i].used)
      continue;
    size_t bl = strlen(s_mounts[i].base);
    if (bl == 0u)
      continue;
    if (strncmp(path, s_mounts[i].base, bl) != 0)
      continue;
    char next = path[bl];
    if (next != '\0' && next != '/')
      continue;
    if (bl > best_len) {
      best_len = bl;
      best = i;
    }
  }
  return best;
}

/* Copia `path` sin el prefijo del montaje en `scratch` (>= strlen(path)+1). Un
 * unico '/' se conserva ("/matrixfs/" -> "/"). */
static const char *shim_strip(int mount, const char *path, char *scratch) {
  const char *rel = path + strlen(s_mounts[mount].base);
  if (rel[0] == '\0') {
    scratch[0] = '/';
    scratch[1] = '\0';
  } else {
    size_t n = strlen(rel);
    memcpy(scratch, rel, n + 1u);
  }
  return scratch;
}

bool shim_vfs_path_is_mounted(const char *path) {
  return shim_mount_lookup(path) >= 0;
}

/* ==== Despacho ==== */

static bool shim_fd_valid(int fd, int *out_mount) {
  if (fd < 0 || fd >= SHIM_VFS_MAX_FDS || !s_fds[fd].used || !s_fds[fd].open)
    return false;
  if (out_mount)
    *out_mount = s_fds[fd].mount;
  return true;
}

/* Descriptor GLOBAL -> LOCAL para el driver del montaje `mount`. */
static int shim_fd_local(int mount, int global_fd) {
  return global_fd - s_mounts[mount].fd_base;
}

int shim_vfs_open(const char *path, int flags, int mode) {
  int m = shim_mount_lookup(path);
  if (m < 0) {
    errno = ENOENT;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.open_p) {
    errno = ENOSYS;
    return -1;
  }
  char scratch[512];
  const char *rel = shim_strip(m, path, scratch);

  int local_fd = op.open_p(shim_cb_ctx(&s_mounts[m]), rel, flags, mode);
  if (local_fd < 0)
    return -1;

  int global_fd = local_fd + s_mounts[m].fd_base;
  if (global_fd < 0 || global_fd >= SHIM_VFS_MAX_FDS) {
    if (op.close_p)
      (void)op.close_p(shim_cb_ctx(&s_mounts[m]), local_fd);
    errno = EMFILE;
    return -1;
  }
  s_fds[global_fd].used = true;
  s_fds[global_fd].open = true;
  s_fds[global_fd].mount = m;
  return global_fd;
}

ssize_t shim_vfs_read(int fd, void *buf, size_t n) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.read_p) {
    errno = ENOSYS;
    return -1;
  }
  return op.read_p(shim_cb_ctx(&s_mounts[m]), shim_fd_local(m, fd), buf, n);
}

ssize_t shim_vfs_write(int fd, const void *buf, size_t n) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.write_p) {
    errno = ENOSYS;
    return -1;
  }
  return op.write_p(shim_cb_ctx(&s_mounts[m]), shim_fd_local(m, fd), buf, n);
}

off_t shim_vfs_lseek(int fd, off_t off, int whence) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return (off_t)-1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.lseek_p) {
    errno = ENOSYS;
    return (off_t)-1;
  }
  return op.lseek_p(shim_cb_ctx(&s_mounts[m]), shim_fd_local(m, fd), off,
                    whence);
}

int shim_vfs_close(int fd) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  int rc = op.close_p ? op.close_p(shim_cb_ctx(&s_mounts[m]),
                                   shim_fd_local(m, fd))
                      : -1;
  if (rc == 0) {
    s_fds[fd].open = false;
    if (s_mounts[m].max_fd == 0) {
      s_fds[fd].used = false;
      s_fds[fd].mount = -1;
    }
  }
  return rc;
}

int shim_vfs_fstat(int fd, struct stat *st) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.fstat_p) {
    errno = ENOSYS;
    return -1;
  }
  return op.fstat_p(shim_cb_ctx(&s_mounts[m]), shim_fd_local(m, fd), st);
}

int shim_vfs_fsync(int fd) {
  int m = 0;
  if (!shim_fd_valid(fd, &m)) {
    errno = EBADF;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.fsync_p) {
    errno = ENOSYS;
    return -1;
  }
  return op.fsync_p(shim_cb_ctx(&s_mounts[m]), shim_fd_local(m, fd));
}

int shim_vfs_stat(const char *path, struct stat *st) {
  int m = shim_mount_lookup(path);
  if (m < 0) {
    errno = ENOENT;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.stat_p) {
    errno = ENOSYS;
    return -1;
  }
  char scratch[512];
  const char *rel = shim_strip(m, path, scratch);
  return op.stat_p(shim_cb_ctx(&s_mounts[m]), rel, st);
}

int shim_vfs_unlink(const char *path) {
  int m = shim_mount_lookup(path);
  if (m < 0) {
    errno = ENOENT;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.unlink_p) {
    errno = ENOSYS;
    return -1;
  }
  char scratch[512];
  const char *rel = shim_strip(m, path, scratch);
  return op.unlink_p(shim_cb_ctx(&s_mounts[m]), rel);
}

int shim_vfs_rename(const char *from, const char *to) {
  int m1 = shim_mount_lookup(from);
  int m2 = shim_mount_lookup(to);
  if (m1 < 0 || m1 != m2) {
    errno = EXDEV;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m1, &op);
  if (!op.rename_p) {
    errno = ENOSYS;
    return -1;
  }
  char s1[512];
  char s2[512];
  const char *r1 = shim_strip(m1, from, s1);
  const char *r2 = shim_strip(m1, to, s2);
  return op.rename_p(shim_cb_ctx(&s_mounts[m1]), r1, r2);
}

int shim_vfs_mkdir(const char *path, mode_t mode) {
  int m = shim_mount_lookup(path);
  if (m < 0) {
    errno = ENOENT;
    return -1;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.mkdir_p) {
    errno = ENOSYS;
    return -1;
  }
  char scratch[512];
  const char *rel = shim_strip(m, path, scratch);
  return op.mkdir_p(shim_cb_ctx(&s_mounts[m]), rel, mode);
}

DIR *shim_vfs_opendir(const char *path) {
  int m = shim_mount_lookup(path);
  if (m < 0) {
    errno = ENOENT;
    return NULL;
  }
  shim_ops_t op;
  shim_ops_of(m, &op);
  if (!op.opendir_p) {
    errno = ENOSYS;
    return NULL;
  }
  char scratch[512];
  const char *rel = shim_strip(m, path, scratch);
  shim_dir_handle_t os = op.opendir_p(shim_cb_ctx(&s_mounts[m]), rel);
  if (!os)
    return NULL;
  for (int i = 0; i < SHIM_VFS_MAX_DIRS; i++) {
    if (s_dir_slots[i].used)
      continue;
    s_dir_slots[i].used = true;
    s_dir_slots[i].mount = m;
    s_dir_slots[i].os = os;
    return (DIR *)&s_dir_slots[i];
  }
  if (op.closedir_p)
    (void)op.closedir_p(shim_cb_ctx(&s_mounts[m]), os);
  errno = EMFILE;
  return NULL;
}

struct dirent *shim_vfs_readdir(DIR *d) {
  if (!d)
    return NULL;
  for (int i = 0; i < SHIM_VFS_MAX_DIRS; i++) {
    if ((DIR *)&s_dir_slots[i] != d)
      continue;
    if (!s_dir_slots[i].used)
      return NULL;
    int m = s_dir_slots[i].mount;
    shim_ops_t op;
    shim_ops_of(m, &op);
    if (!op.readdir_p)
      return NULL;
    return op.readdir_p(shim_cb_ctx(&s_mounts[m]), s_dir_slots[i].os);
  }
  return NULL;
}

int shim_vfs_closedir(DIR *d) {
  if (!d)
    return -1;
  for (int i = 0; i < SHIM_VFS_MAX_DIRS; i++) {
    if ((DIR *)&s_dir_slots[i] != d)
      continue;
    if (!s_dir_slots[i].used)
      return -1;
    int m = s_dir_slots[i].mount;
    shim_ops_t op;
    shim_ops_of(m, &op);
    int rc = op.closedir_p ? op.closedir_p(shim_cb_ctx(&s_mounts[m]),
                                          s_dir_slots[i].os)
                           : -1;
    s_dir_slots[i].used = false;
    s_dir_slots[i].os = NULL;
    return rc;
  }
  return -1;
}
