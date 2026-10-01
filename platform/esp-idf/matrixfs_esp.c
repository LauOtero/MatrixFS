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
 *
 * Sin heap: el descriptor de flash y el contexto de la particion viven en
 * estado estatico. El nucleo de MatrixFS tambien es de instancia unica, por lo
 * que solo puede haber un volumen montado a la vez (ver README).
 */
#include "matrixfs_esp.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MFS_ESP_TAG "matrixfs_esp"

/* Ancho del buffer alineado para read-modify-write de programacion. Debe ser
 * multiplo de 4 y cubre de sobra una pagina NOR (256 B). */
#define MFS_ESP_RMW_BUF 256u
/* Geometria por defecto si la particion no declara erase_size. */
#define MFS_ESP_DEFAULT_ERASE_UNIT 4096u
#define MFS_ESP_PAGE_SIZE 256u
/* Longitud de la clave criptografica del nucleo (32 bytes). */
#define MFS_ESP_KEY_LEN 32u
#define MFS_ESP_LAST_ERR_LEN 160u
/* Sectores reservados antes de la primera zona ZLF: SB A, SB B y anillo de
 * tokens (mfs_internal.h). El volumen necesita al menos 1 zona adicional. */
#define MFS_ESP_MIN_SECTORS 4u

/* ==== Estado estatico (sin heap, instancia unica) ==== */
typedef struct {
  const esp_partition_t *part;
  uint32_t base; /* direccion fisica absoluta = part->address */
} esp_mfs_ctx_t;

static esp_mfs_ctx_t s_ctx;
static mfs_embedded_flash_t s_flash;
static WORD_ALIGNED_ATTR uint8_t s_rmw[MFS_ESP_RMW_BUF];
static char s_last_err[MFS_ESP_LAST_ERR_LEN];

#if CONFIG_MATRIXFS_ENABLE_CRYPTO
static uint8_t s_key[MFS_ESP_KEY_LEN];
#endif

/* ================== Primitivas obligatorias del puerto (§20.2) =============
 *
 * Las aporta ESTE componente y solo debe enlazarse una implementacion de las
 * mismas en toda la imagen (el nucleo no las trae). En una aplicacion ESP-IDF
 * convencional no debe existir otro mfs_port_*.c.
 * ========================================================================= */

static portMUX_TYPE s_crit_mux = portMUX_INITIALIZER_UNLOCKED;

void mfs_port_crit_enter(void) { portENTER_CRITICAL(&s_crit_mux); }
void mfs_port_crit_exit(void) { portEXIT_CRITICAL(&s_crit_mux); }

/* Contador de ciclos libre-corriente de la CPU (Xtensa o RISC-V en IDF v5). */
uint32_t mfs_port_cycles(void) { return (uint32_t)esp_cpu_get_cycle_count(); }

/* Tiempo monotonico en microsegundos. esp_timer_get_time() devuelve int64_t
 * (microsegundos desde el arranque); se trunca a 32 bits como exige el
 * contrato del puerto (contadores de 32 bits que envuelven). */
uint32_t mfs_port_time_us(void) { return (uint32_t)esp_timer_get_time(); }

/* Idle cooperativo durante una espera (nunca bloqueo de busy-wait). */
void mfs_port_wfi(void) { taskYIELD(); }

/* =========================== Utilidades internas ========================= */

static void esp_set_ok(void) { snprintf(s_last_err, sizeof(s_last_err), "ok"); }

static mfs_st esp_fail(mfs_st st, const char *msg) {
  snprintf(s_last_err, sizeof(s_last_err), "%s: %s", msg, mfs_ststr(st));
  return st;
}

/* ========================= Callbacks de flash =========================== */

/* Lectura directa: esp_partition_read admite destino y longitud sin
 * restricciones de alineacion. `addr` llega como direccion absoluta del mapa
 * del MCU (mfs_embedded suma flash->base_addr); se traduce a offset relativo
 * a la particion. */
static mfs_st esp_flash_read(void *ctx, uint32_t addr, void *dst,
                             uint32_t len) {
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

/* Programacion con read-modify-write alineado a 4 bytes.
 *
 * esp_partition_write exige, en particiones cifradas, que offset y longitud
 * sean multiplos del bloque de cifrado (16 B); en particiones sin cifrar suele
 * tolerar tamanos arbitrarios, pero no esta garantizado en todas las versiones
 * del IDF. Para ser robustos, se alinea el rango a palabras de 4 bytes:
 *   1) se lee la ventana alineada [lo, hi);
 *   2) se sobreescribe solo el tramo [addr, addr+len) con el contenido nuevo;
 *   3) se reprograma la ventana completa.
 * Reprogramar bytes que ya estaban escritos con su mismo valor es seguro en NOR
 * (solo se limpian bits 1->0; reescribir el mismo dato no altera nada), y los
 * bytes del tramo pedido son responsabilidad del nucleo (que borra antes de
 * programar). La ventana nunca rebasa la particion porque su tamano es multiplo
 * del bloque de borrado (multiplo de 4). */
static mfs_st esp_flash_prog(void *ctx, uint32_t addr, const void *src,
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
  uint32_t lo = off & ~3u;
  uint32_t hi = (off + len + 3u) & ~3u;

  for (uint32_t p = lo; p < hi;) {
    uint32_t n = hi - p;
    if (n > MFS_ESP_RMW_BUF)
      n = MFS_ESP_RMW_BUF;
    /* Ventana alineada: p y n son multiplos de 4. */
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
 * alineadas al bloque de borrado; por seguridad se alinea hacia abajo. */
static mfs_st esp_flash_erase(void *ctx, uint32_t addr) {
  esp_mfs_ctx_t *c = (esp_mfs_ctx_t *)ctx;
  if (!c || !c->part)
    return MFS_EINVAL;
  uint32_t unit = c->part->erase_size ? (uint32_t)c->part->erase_size
                                      : (uint32_t)MFS_ESP_DEFAULT_ERASE_UNIT;
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
    snprintf(s_last_err, sizeof(s_last_err), "particion '%s' no encontrada",
             lab);
    return MFS_ENOENT;
  }

  uint32_t unit = p->erase_size ? (uint32_t)p->erase_size
                                : (uint32_t)MFS_ESP_DEFAULT_ERASE_UNIT;
  if (p->size < (uint64_t)MFS_ESP_MIN_SECTORS * unit) {
    snprintf(s_last_err, sizeof(s_last_err),
             "particion '%s' demasiado pequena (%u B): se requieren al menos "
             "%u B",
             lab, (unsigned)p->size, (unsigned)(MFS_ESP_MIN_SECTORS * unit));
    return MFS_EINVAL;
  }

  s_ctx.part = p;
  s_ctx.base = p->address;

  memset(&s_flash, 0, sizeof(s_flash));
  s_flash.read = esp_flash_read;
  s_flash.prog = esp_flash_prog;
  s_flash.erase = esp_flash_erase;
  s_flash.ctx = &s_ctx;
  s_flash.base_addr = p->address;
  s_flash.size = (uint32_t)p->size;
  s_flash.erase_unit = unit;
  s_flash.page_size = (uint16_t)MFS_ESP_PAGE_SIZE;
  s_flash.no_erase = false;
  /* Los presupuestos t_*_max_us quedan a 0: mfs_embedded_setup() aplica los
   * valores NOR por defecto del estandar. */
  return MFS_OK;
}

/* ============================== API publica ============================= */

mfs_st matrixfs_esp_mount(const char *partition_label, mf_t *out_fs,
                          const mfs_embedded_opts *opts) {
  if (!out_fs)
    return esp_fail(MFS_EINVAL, "out_fs es NULL");
  mfs_st st = esp_prepare(partition_label);
  if (st != MFS_OK)
    return st;
  st = mfs_embedded_mount(out_fs, &s_flash, opts);
  if (st != MFS_OK)
    return esp_fail(st, "montaje fallido");
  esp_set_ok();
  return MFS_OK;
}

mfs_st matrixfs_esp_format(const char *partition_label, mf_t *fs,
                           const mfs_embedded_opts *opts) {
  if (!fs)
    return esp_fail(MFS_EINVAL, "fs es NULL");
  mfs_st st = esp_prepare(partition_label);
  if (st != MFS_OK)
    return st;
  st = mfs_embedded_format(fs, &s_flash, opts);
  if (st != MFS_OK)
    return esp_fail(st, "formateo fallido");
  esp_set_ok();
  return MFS_OK;
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
  return s_last_err[0] ? s_last_err : "ok";
}
