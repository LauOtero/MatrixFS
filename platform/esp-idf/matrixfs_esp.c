/* matrixfs_esp.c — integracion de MatrixFS con ESP-IDF (esp_partition).
 *
 * Copyright 2026 MatrixFS contributors
 *
 * Licencia Apache, Version 2.0 (la "Licencia");
 * no puede usar este fichero salvo en cumplimiento de la Licencia.
 * Puede obtener una copia en:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Salvo que la ley aplicable exija lo contrario o se acuerde por escrito, el
 * software distribuido bajo la Licencia se distribuye "TAL CUAL", SIN GARANTIAS
 * NI CONDICIONES DE NINGUN TIPO, ni expresas ni implicitas. Consulte la
 * Licencia para conocer el permiso y las limitaciones especificas.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Responsabilidades de este fichero:
 *   1) Primitivas obligatorias del puerto (mfs_port_*, §20.2) sobre ESP-IDF.
 *      Se compilan UNA sola vez con este componente.
 *   2) Callbacks read/prog/erase sobre una particion esp_partition.
 *   3) Montaje/formateo mediante la capa embebida compartida (mfs_embedded).
 *   4) Serializacion opcional (CONFIG_MATRIXFS_THREAD_SAFE) y diagnostico de
 *      capacidad.
 *
 * Sin heap para el sistema de archivos: el descriptor de flash y el contexto de
 * la particion viven en estado estatico. El nucleo de MatrixFS tambien es de
 * instancia unica, por lo que solo puede haber un volumen montado a la vez.
 * (El mutex de serializacion si se crea con FreeRTOS, porque un mutex
 * recursivo no es expresable sin asignacion dinamica; son ~80 B.)
 */
#include "matrixfs_esp.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "matrixfs_esp_private.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
/* `esp_flash_encryption_enabled()` vive en `esp_flash_encrypt.h` (componente
 * `bootloader_support`), NO en `esp_flash.h`. Incluir esta ultima y no aquella
 * compilaba en host (el shim la declaraba tambien) pero fallaba con el IDF
 * real: "implicit declaration of function 'esp_flash_encryption_enabled'". */
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MFS_ESP_TAG "matrixfs_esp"

/* Ancho del buffer alineado para read-modify-write de programacion. Debe ser
 * multiplo de la mayor granularidad de escritura soportada (16 B, bloque de
 * cifrado de la flash) y cubre de sobra una pagina NOR (256 B). */
#define MFS_ESP_RMW_BUF 256u
/* Geometria por defecto si la particion no declara erase_size. */
#define MFS_ESP_DEFAULT_ERASE_UNIT 4096u
/* Alineacion que exige esp_partition_erase_range: se valida contra
 * SPI_FLASH_SEC_SIZE (0x1000), NO contra partition->erase_size. */
#define MFS_ESP_ERASE_ALIGN 4096u
/* Granularidad de programacion sin cifrado de flash (esp_partition_write no
 * impone alineacion en particiones sin cifrar, pero 4 B es la unidad natural
 * del controlador y evita lecturas-modificacion-escritura innecesarias). */
#define MFS_ESP_PGM_GRAN_PLAIN 4u
/* ... y con cifrado de flash, que exige multiplos del bloque de cifrado. */
#define MFS_ESP_PGM_GRAN_ENCRYPTED 16u
/* Longitud de la clave criptografica del nucleo (32 bytes). */
#define MFS_ESP_KEY_LEN 32u
#define MFS_ESP_LAST_ERR_LEN 160u
/* Sectores reservados antes de la primera zona ZLF: SB A, SB B y anillo de
 * tokens (src/mfs_internal.h). El volumen necesita al menos 1 zona adicional.
 */
#define MFS_ESP_MIN_SECTORS 4u

#ifndef CONFIG_MATRIXFS_PAGE_SIZE
#define CONFIG_MATRIXFS_PAGE_SIZE 256
#endif

/* ==== Estado estatico (sin heap, instancia unica) ==== */
typedef struct {
  const esp_partition_t *part;
  uint32_t base; /* direccion fisica absoluta = part->address */
} esp_mfs_ctx_t;

static esp_mfs_ctx_t s_ctx;
static mfs_embedded_flash_t s_flash;
static WORD_ALIGNED_ATTR uint8_t s_rmw[MFS_ESP_RMW_BUF];
/* Granularidad de programacion efectiva del medio (4 o 16 B). */
static uint32_t s_pgm_gran = MFS_ESP_PGM_GRAN_PLAIN;
/* Diagnostico de capacidad del ultimo `esp_prepare`. */
static uint32_t s_capacity = 0u;
static bool s_capacity_wasteful = false;
/* Instancia del nucleo del volumen montado (la aporta el llamador). El
 * componente solo guarda el puntero: lo necesita la capa VFS, que no recibe el
 * mf_t en sus callbacks. */
static mf_t *s_mounted_fs = NULL;

#if CONFIG_MATRIXFS_ENABLE_CRYPTO
static uint8_t s_key[MFS_ESP_KEY_LEN];
#endif

/* Buffer de error. Con THREAD_SAFE se usa uno por tarea para que dos tareas no
 * se pisen el mensaje; la clave es el handle de la tarea actual.
 *
 * IDF-10: las ranuras NO se liberan solas al morir una tarea (FreeRTOS no
 * ofrece un gancho portable de borrado), de modo que sin politica de desalojo
 * quedarian ocupadas por tareas ya inexistentes y una tarea viva acabaria
 * reutilizando siempre la ranura 0 (perdiendo el aislamiento). Para evitarlo
 * cada ranura lleva una marca temporal y, cuando la tarea no tiene ranura y no
 * hay ninguna libre, se DESALOJA la de menor marca (la usada menos
 * recientemente). Es determinista y acotado: una tarea viva nunca se queda sin
 * mensaje. */
#if CONFIG_MATRIXFS_THREAD_SAFE
#define MFS_ESP_ERR_SLOTS 4u
typedef struct {
  TaskHandle_t task;
  uint32_t stamp; /* marca de uso para el desalojo LRU */
  char msg[MFS_ESP_LAST_ERR_LEN];
} esp_err_slot_t;
static esp_err_slot_t s_err_slots[MFS_ESP_ERR_SLOTS];
static uint32_t s_err_stamp = 0u;

static char *esp_err_buf(void) {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  uint32_t now = ++s_err_stamp;
  uint32_t victim = 0u;
  for (uint32_t i = 0; i < MFS_ESP_ERR_SLOTS; i++) {
    if (s_err_slots[i].task == me) {
      s_err_slots[i].stamp = now;
      return s_err_slots[i].msg;
    }
    if (s_err_slots[i].task == NULL) {
      s_err_slots[i].task = me;
      s_err_slots[i].stamp = now;
      return s_err_slots[i].msg;
    }
    if (s_err_slots[i].stamp < s_err_slots[victim].stamp)
      victim = i;
  }
  /* Todas ocupadas por OTRAS tareas: desalojar la mas antigua. */
  s_err_slots[victim].task = me;
  s_err_slots[victim].stamp = now;
  return s_err_slots[victim].msg;
}
#else
static char s_last_err[MFS_ESP_LAST_ERR_LEN];
static char *esp_err_buf(void) { return s_last_err; }
#endif

/* ================== Primitivas obligatorias del puerto (§20.2) =============
 *
 * Las aporta ESTE componente y solo debe enlazarse una implementacion de las
 * mismas en toda la imagen (el nucleo no las trae; sus propios envoltorios
 * src/core/mfs_port_arch.c y mfs_port_rtos.c quedan fuera del build del
 * componente, ver CMakeLists.txt).
 *
 * CONTEXTO: `mfs_read` las usa desde contexto de TAREA (mfs_zone.c envuelve la
 * lectura en crit_enter/exit). NO son ISR-safe: portENTER_CRITICAL exige
 * contexto de tarea. Si se necesita la API asincrona del nucleo desde una ISR,
 * hay que aportar variantes *_ISR.
 * ========================================================================= */
static portMUX_TYPE s_crit_mux = portMUX_INITIALIZER_UNLOCKED;

void mfs_port_crit_enter(void) { portENTER_CRITICAL(&s_crit_mux); }
void mfs_port_crit_exit(void) { portEXIT_CRITICAL(&s_crit_mux); }

/* Contador de ciclos de la CPU (Xtensa o RISC-V en IDF v5).
 *
 * IDF-09: en partes DUALES (ESP32, ESP32-S3) el contador CCOUNT del hardware es
 * POR NUCLEO, de modo que dos tareas en nucleos distintos ven escalas de tiempo
 * incomparables. Por eso:
 *   · en un solo nucleo (CONFIG_FREERTOS_UNICORE) el contador de hardware es
 *     global y se usa directamente;
 *   · en multi-nucleo se deriva una base COMUN del temporizador del sistema
 *     (esp_timer_get_time) escalada por la frecuencia de CPU: monotona y
 *     comparable entre nucleos.
 * El host de prueba no define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, de modo que usa
 * el contador monótono del shim. */
uint32_t mfs_port_cycles(void) {
#if defined(CONFIG_FREERTOS_UNICORE) ||                                        \
    !defined(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
  return (uint32_t)esp_cpu_get_cycle_count();
#else
  return (uint32_t)((uint64_t)esp_timer_get_time() *
                    (uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif
}

/* Tiempo monotonico en microsegundos. esp_timer_get_time() devuelve int64_t
 * (microsegundos desde el arranque); se trunca a 32 bits como exige el
 * contrato del puerto (contadores de 32 bits que envuelven, ~71.6 min).
 * Es correcto con DFS y con light sleep. */
uint32_t mfs_port_time_us(void) { return (uint32_t)esp_timer_get_time(); }

/* Idle cooperativo durante una espera (nunca bloqueo de busy-wait). */
void mfs_port_wfi(void) { taskYIELD(); }

/* ========================= Serializacion (E4) =========================== */
#if CONFIG_MATRIXFS_THREAD_SAFE
static SemaphoreHandle_t s_mutex = NULL;
static portMUX_TYPE s_mutex_mux = portMUX_INITIALIZER_UNLOCKED;

/* Crea el mutex recursivo la primera vez. La ASIGNACION se hace FUERA de la
 * seccion critica (crear un semaforo reserva memoria y puede bloquear: no es
 * legal hacerlo con las interrupciones deshabilitadas); la seccion critica solo
 * publica el puntero, y quien pierde la carrera libera el suyo. */
static bool esp_mutex_ensure(void) {
  if (s_mutex)
    return true;
  SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
  if (!m)
    return false;
  portENTER_CRITICAL(&s_mutex_mux);
  if (!s_mutex)
    s_mutex = m;
  bool won = (s_mutex == m);
  portEXIT_CRITICAL(&s_mutex_mux);
  if (!won)
    vSemaphoreDelete(m); /* otro gano la carrera */
  return true;
}

void matrixfs_esp_lock(void) {
  if (esp_mutex_ensure())
    (void)xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void matrixfs_esp_unlock(void) {
  if (s_mutex)
    (void)xSemaphoreGiveRecursive(s_mutex);
}

/* Para la capa VFS: tomar el mutex solo si ya esta creado no es suficiente
 * (queremos crearlo). Se expone por una funcion interna. */
bool matrixfs_esp_mutex_ensure(void) { return esp_mutex_ensure(); }
#else
void matrixfs_esp_lock(void) {}
void matrixfs_esp_unlock(void) {}
bool matrixfs_esp_mutex_ensure(void) { return true; }
#endif

/* =========================== Utilidades internas ========================= */

static void esp_set_ok(void) {
  snprintf(esp_err_buf(), MFS_ESP_LAST_ERR_LEN, "ok");
}

static mfs_st esp_fail(mfs_st st, const char *msg) {
  snprintf(esp_err_buf(), MFS_ESP_LAST_ERR_LEN, "%s: %s", msg, mfs_ststr(st));
  return st;
}

/* Redondeo hacia arriba a multiplo de `g` (potencia de dos). */
static uint32_t esp_align_up(uint32_t v, uint32_t g) {
  return (v + (g - 1u)) & ~(g - 1u);
}

/* ========================= Callbacks de flash =========================== */

/* Lectura directa: esp_partition_read admite destino y longitud sin
 * restricciones de alineacion. `addr` llega como direccion absoluta del mapa
 * del MCU (mfs_embedded suma flash->base_addr); se traduce a offset relativo
 * a la particion. */
static mfs_st esp_mfs_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  esp_mfs_ctx_t *c = (esp_mfs_ctx_t *)ctx;
  if (!c || !c->part)
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (!dst)
    return MFS_EINVAL;
  if (addr < c->base)
    return MFS_EINVAL;
  uint32_t off = addr - c->base;
  if (off > c->part->size || len > c->part->size - off)
    return MFS_EINVAL;
  return esp_partition_read(c->part, off, dst, (size_t)len) == ESP_OK ? MFS_OK
                                                                      : MFS_EIO;
}

/* Programacion con read-modify-write alineado a `s_pgm_gran`.
 *
 * esp_partition_write exige, en particiones CIFRADAS, que offset y longitud
 * sean multiplos del bloque de cifrado (16 B); en particiones sin cifrar suele
 * tolerar tamanos arbitrarios, pero no esta garantizado en todas las versiones
 * del IDF. Para ser robustos en ambos casos se alinea el rango a la
 * granularidad efectiva:
 *   1) se lee la ventana alineada [lo, hi);
 *   2) se sobreescribe solo el tramo [addr, addr+len) con el contenido nuevo;
 *   3) se reprograma la ventana completa.
 * Reprogramar bytes que ya estaban escritos con su mismo valor es seguro en NOR
 * (solo se limpian bits 1->0; reescribir el mismo dato no altera nada), y los
 * bytes del tramo pedido son responsabilidad del nucleo (que borra antes de
 * programar). La ventana nunca rebasa la particion porque su tamano es multiplo
 * del bloque de borrado (multiplo de 4 y de 16). */
static mfs_st esp_mfs_prog(void *ctx, uint32_t addr, const void *src,
                           uint32_t len) {
  esp_mfs_ctx_t *c = (esp_mfs_ctx_t *)ctx;
  if (!c || !c->part)
    return MFS_EINVAL;
  if (len == 0u)
    return MFS_OK;
  if (!src)
    return MFS_EINVAL;
  if (addr < c->base)
    return MFS_EINVAL;
  uint32_t off = addr - c->base;
  if (off > c->part->size || len > c->part->size - off)
    return MFS_EINVAL;

  const uint8_t *sp = (const uint8_t *)src;
  const uint32_t g = s_pgm_gran;

  /* FAST PATH (IDF-03): si el tramo ya esta alineado a la granularidad, se
   * escribe DIRECTAMENTE, sin leer-modificar-escribir. El nucleo borra la zona
   * antes de programar, de modo que no hay nada que preservar en el tramo (y en
   * NOR reprogramar el mismo valor es inocuo: solo se limpian bits 1->0). Con
   * esto desaparece la lectura extra que antes se hacia en CADA programacion,
   * incluso en particiones sin cifrar y con la direccion ya alineada. */
  if ((off % g) == 0u && (len % g) == 0u) {
    return esp_partition_write(c->part, off, sp, (size_t)len) == ESP_OK
               ? MFS_OK
               : MFS_EIO;
  }

  uint32_t lo = off & ~(g - 1u);
  uint32_t hi = esp_align_up(off + len, g);
  if (hi > c->part->size)
    return MFS_EINVAL;

  for (uint32_t p = lo; p < hi;) {
    /* El trozo debe ser multiplo de g: se redondea a la baja y el resto lo
     * cubre la iteracion siguiente (la ventana total si es multiplo de g). */
    uint32_t n = hi - p;
    if (n > MFS_ESP_RMW_BUF)
      n = MFS_ESP_RMW_BUF;
    n &= ~(g - 1u);
    if (n == 0u)
      n = MFS_ESP_RMW_BUF <= hi - p ? MFS_ESP_RMW_BUF : (hi - p);
    if (n == 0u)
      break;

    if (esp_partition_read(c->part, p, s_rmw, (size_t)n) != ESP_OK)
      return MFS_EIO;
    uint32_t a = (off > p) ? off : p;
    uint32_t b = ((off + len) < (p + n)) ? (off + len) : (p + n);
    if (b > a)
      memcpy(s_rmw + (a - p), sp + (a - off), (size_t)(b - a));
    if (esp_partition_write(c->part, p, s_rmw, (size_t)n) != ESP_OK)
      return MFS_EIO;
    p += n;
  }
  return MFS_OK;
}

/* Borrado de un bloque alineado. El nucleo entrega direcciones absolutas
 * alineadas al bloque de borrado; por seguridad se alinea hacia abajo.
 *
 * OJO: `esp_partition_erase_range` valida la alineacion contra
 * SPI_FLASH_SEC_SIZE (4096) y NO contra `partition->erase_size`. Ademas, en
 * ESP-IDF v5.3 `esp_partition_register_external()` deja `erase_size` a 0 (el
 * struct se reserva con calloc y no se inicializa), de modo que una particion
 * de flash EXTERNO registrada asi daria `erase_unit = 0` si se usara el campo
 * tal cual. Por eso se normaliza a 4096 y se exige que la unidad logica sea
 * multiplo del sector de borrado del IDF. */
static mfs_st esp_mfs_erase(void *ctx, uint32_t addr) {
  esp_mfs_ctx_t *c = (esp_mfs_ctx_t *)ctx;
  if (!c || !c->part)
    return MFS_EINVAL;
  uint32_t unit = c->part->erase_size ? (uint32_t)c->part->erase_size
                                      : (uint32_t)MFS_ESP_DEFAULT_ERASE_UNIT;
  /* El tamaño que acepta esp_partition_erase_range es un multiplo de
   * SPI_FLASH_SEC_SIZE. Si `erase_size` declarara algo mayor y no multiplo, el
   * borrado fallaria en silencio devolviendo MFS_EIO; se rechaza antes. */
  if ((unit % MFS_ESP_ERASE_ALIGN) != 0u)
    return MFS_EINVAL;
  if (addr < c->base)
    return MFS_EINVAL;
  uint32_t off = addr - c->base;
  off -= off % unit; /* alinea hacia abajo (defensivo) */
  if (off >= c->part->size || unit > c->part->size - off)
    return MFS_EINVAL;
  return esp_partition_erase_range(c->part, off, unit) == ESP_OK ? MFS_OK
                                                                 : MFS_EIO;
}

/* ===================== Descubrimiento de la particion =================== */

/* Localiza la particion y rellena el descriptor de flash estatico. Devuelve
 * MFS_OK o un error tipificado. */
static mfs_st esp_prepare(const char *label) {
  const char *lab =
      (label && label[0]) ? label : CONFIG_MATRIXFS_PARTITION_LABEL;
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, lab);
  if (!p) {
    snprintf(esp_err_buf(), MFS_ESP_LAST_ERR_LEN,
             "particion '%s' no encontrada", lab);
    return MFS_ENOENT;
  }

  uint32_t unit = p->erase_size ? (uint32_t)p->erase_size
                                : (uint32_t)MFS_ESP_DEFAULT_ERASE_UNIT;
  if (p->size < (uint64_t)MFS_ESP_MIN_SECTORS * unit) {
    snprintf(esp_err_buf(), MFS_ESP_LAST_ERR_LEN,
             "particion '%s' demasiado pequena (%u B): se requieren al menos "
             "%u B (3 sectores reservados + 1 zona)",
             lab, (unsigned)p->size, (unsigned)(MFS_ESP_MIN_SECTORS * unit));
    return MFS_EINVAL;
  }

  /* Granularidad de programacion efectiva: el bloque de cifrado cuando la
   * flash esta cifrada, la palabra del controlador cuando no. */
  s_pgm_gran = esp_flash_encryption_enabled() ? MFS_ESP_PGM_GRAN_ENCRYPTED
                                              : MFS_ESP_PGM_GRAN_PLAIN;

  s_ctx.part = p;
  s_ctx.base = p->address;

  memset(&s_flash, 0, sizeof(s_flash));
  s_flash.read = esp_mfs_read;
  s_flash.prog = esp_mfs_prog;
  s_flash.erase = esp_mfs_erase;
  s_flash.ctx = &s_ctx;
  s_flash.base_addr = p->address;
  s_flash.size = (uint32_t)p->size;
  s_flash.erase_unit = unit;
  s_flash.page_size = (uint16_t)CONFIG_MATRIXFS_PAGE_SIZE;
  s_flash.program_granularity = s_pgm_gran;
  s_flash.no_erase = false;
#if CONFIG_MATRIXFS_T_PROG_MAX_US
  s_flash.t_prog_max_us = (uint32_t)CONFIG_MATRIXFS_T_PROG_MAX_US;
#endif
#if CONFIG_MATRIXFS_T_ERASE_MAX_US
  s_flash.t_erase_max_us = (uint32_t)CONFIG_MATRIXFS_T_ERASE_MAX_US;
#endif
#if CONFIG_MATRIXFS_T_READ_MAX_US
  s_flash.t_read_max_us = (uint32_t)CONFIG_MATRIXFS_T_READ_MAX_US;
#endif

  /* Diagnostico de capacidad: el nucleo direcciona MFS_ZONE_MAX zonas de
   * `unit` bytes tras los 3 sectores reservados. Una particion mayor no se
   * aprovecha; se avisa en vez de dejar que el usuario lo descubra. */
  {
    uint64_t usable = (uint64_t)(p->size / unit - 3u) * unit;
    uint64_t cap = (uint64_t)MFS_ZONE_MAX * unit;
    s_capacity = (uint32_t)(usable < cap ? usable : cap);
    s_capacity_wasteful = (usable > cap);
  }
  return MFS_OK;
}

/* ============================== API publica ============================= */

mfs_st matrixfs_esp_mount(const char *partition_label, mf_t *out_fs,
                          const mfs_embedded_opts *opts) {
  if (!out_fs)
    return esp_fail(MFS_EINVAL, "out_fs es NULL");
#if CONFIG_MATRIXFS_THREAD_SAFE
  if (!esp_mutex_ensure())
    return esp_fail(MFS_EIO, "no se pudo crear el mutex");
  matrixfs_esp_lock();
#endif
  mfs_st st = esp_prepare(partition_label);
  if (st == MFS_OK) {
    if (s_capacity_wasteful) {
      ESP_LOGW(MFS_ESP_TAG,
               "particion '%s' de %u B: solo se aprovecharan %u B "
               "(MFS_ZONE_MAX=%u zonas de %u B). Sube MATRIXFS_ZONE_MAX si "
               "necesitas mas capacidad.",
               (partition_label && partition_label[0])
                   ? partition_label
                   : CONFIG_MATRIXFS_PARTITION_LABEL,
               (unsigned)s_flash.size, (unsigned)s_capacity,
               (unsigned)MFS_ZONE_MAX, (unsigned)s_flash.erase_unit);
    }
    st = mfs_embedded_mount(out_fs, &s_flash, opts);
    if (st != MFS_OK) {
      st = esp_fail(st, "montaje fallido");
      s_mounted_fs = NULL;
    } else {
      s_mounted_fs = out_fs;
      esp_set_ok();
    }
  }
#if CONFIG_MATRIXFS_THREAD_SAFE
  matrixfs_esp_unlock();
#endif
  return st;
}

mfs_st matrixfs_esp_format(const char *partition_label, mf_t *fs,
                           const mfs_embedded_opts *opts) {
  if (!fs)
    return esp_fail(MFS_EINVAL, "fs es NULL");
#if CONFIG_MATRIXFS_THREAD_SAFE
  if (!esp_mutex_ensure())
    return esp_fail(MFS_EIO, "no se pudo crear el mutex");
  matrixfs_esp_lock();
#endif
  mfs_st st = esp_prepare(partition_label);
  if (st == MFS_OK) {
    /* Formatear deja el volumen SIN montar: cualquier instancia anterior queda
     * invalidada (el contenido del medio se destruyo). */
    s_mounted_fs = NULL;
    st = mfs_embedded_format(fs, &s_flash, opts);
    if (st != MFS_OK)
      st = esp_fail(st, "formateo fallido");
    else
      esp_set_ok();
  }
#if CONFIG_MATRIXFS_THREAD_SAFE
  matrixfs_esp_unlock();
#endif
  return st;
}

#if CONFIG_MATRIXFS_ENABLE_CRYPTO
static int hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Convierte una cadena hexadecimal de 64 digitos en 32 bytes. */
static bool esp_parse_key(const char *hex, uint8_t out[MFS_ESP_KEY_LEN]) {
  if (!hex)
    return false;
  if (strlen(hex) != MFS_ESP_KEY_LEN * 2u)
    return false;
  for (size_t i = 0; i < MFS_ESP_KEY_LEN; i++) {
    int hi = hex_nibble(hex[2u * i]);
    int lo = hex_nibble(hex[2u * i + 1u]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}
#endif

static mfs_mode_t esp_kconfig_mode(void) {
#if defined(CONFIG_MATRIXFS_FORCE_MODE_ULTRA_NANO)
  return MFS_MODE_ULTRA_NANO;
#elif defined(CONFIG_MATRIXFS_FORCE_MODE_NANO)
  return MFS_MODE_NANO;
#elif defined(CONFIG_MATRIXFS_FORCE_MODE_COMPACT)
  return MFS_MODE_COMPACT;
#elif defined(CONFIG_MATRIXFS_FORCE_MODE_BALANCED)
  return MFS_MODE_BALANCED;
#elif defined(CONFIG_MATRIXFS_FORCE_MODE_EXTENDED)
  return MFS_MODE_EXTENDED;
#else
  return MFS_MODE_UNSUPPORTED; /* automatico */
#endif
}

mfs_st matrixfs_esp_mount_default(mf_t *out_fs) {
  if (!out_fs)
    return esp_fail(MFS_EINVAL, "out_fs es NULL");

  mfs_embedded_opts o;
  mfs_embedded_opts_default(&o);
  o.ram_total = (uint32_t)CONFIG_MATRIXFS_RAM_BUDGET;
  o.forced_mode = esp_kconfig_mode();
#if CONFIG_MATRIXFS_FORMAT_IF_NEEDED
  o.format_if_needed = true;
#endif
#if CONFIG_MATRIXFS_ENABLE_CRYPTO
  if (esp_parse_key(CONFIG_MATRIXFS_CRYPTO_KEY_HEX, s_key)) {
    o.key = s_key;
  } else {
    ESP_LOGW(MFS_ESP_TAG,
             "CONFIG_MATRIXFS_CRYPTO_KEY_HEX vacia o invalida (se esperan 64 "
             "digitos hex); se monta sin cifrado");
  }
#endif
  return matrixfs_esp_mount(CONFIG_MATRIXFS_PARTITION_LABEL, out_fs, &o);
}

const char *matrixfs_esp_last_error(void) {
  char *b = esp_err_buf();
  return b[0] ? b : "ok";
}

uint32_t matrixfs_esp_capacity_bytes(void) { return s_capacity; }

bool matrixfs_esp_capacity_wasteful(void) { return s_capacity_wasteful; }

/* ==================== Superficie interna (VFS) ========================= */

mf_t *matrixfs_esp_fs(void) { return s_mounted_fs; }

bool matrixfs_esp_is_mounted(void) { return s_mounted_fs != NULL; }

/* Traduccion de errores del nucleo a `errno` de newlib. Es la misma tabla que
 * usan los front-ends POSIX del proyecto (platform/common/mfs_vfs.c) para que
 * el comportamiento sea coherente entre plataformas. */
int matrixfs_esp_errno(mfs_st st) {
  switch (st) {
  case MFS_OK:
    return 0;
  case MFS_ENOENT:
    return ENOENT;
  case MFS_EEXISTS:
    return EEXIST;
  case MFS_EINVAL:
    return EINVAL;
  case MFS_ENOSPC:
  case MFS_ETABLEFULL:
  case MFS_EBACKPRESSURE:
    return ENOSPC;
  case MFS_EROFS:
    return EROFS;
  case MFS_EACCES:
    return EACCES;
  case MFS_EBUSY:
    return EBUSY;
  case MFS_ENOTSUP:
    return ENOSYS;
  case MFS_EOVERFLOW:
    return EOVERFLOW;
  case MFS_EDEADLK:
    return EDEADLK;
  case MFS_EAGAIN:
    return EAGAIN;
  case MFS_ENOTMOUNTED:
    return ENODEV;
  case MFS_ETIMEDOUT_BUDGET:
    return ETIMEDOUT;
  default:
    return EIO;
  }
}
