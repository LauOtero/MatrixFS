/* modmatrixfs.c — Módulo de usuario (usermod) de MicroPython para MatrixFS.
 *
 * Copyright (c) 2026 MatrixFS contribuidores.
 * Licencia: Apache-2.0 (ver LICENSE en la raíz del repositorio).
 *
 * Este fichero es parte del puerto MicroPython y NO forma parte del núcleo.
 * Se compila como USER_C_MODULE y ofrece:
 *   · El módulo Python `matrixfs` con la clase `MatrixFS`.
 *   · La clase interna `File` devuelta por `MatrixFS.open()`.
 *
 * Además, este módulo APORTA las primitivas obligatorias del puerto MatrixFS
 * (§20.2 de la especificación), que todo integrador debe implementar:
 *   mfs_port_crit_enter / mfs_port_crit_exit
 *   mfs_port_cycles / mfs_port_time_us / mfs_port_wfi
 * No deben declararse en ningún otro fichero del build de MicroPython para
 * evitar símbolos duplicados.
 *
 * Diseño (sin heap para el núcleo):
 *   · La instancia mf_t vive en BSS estático (static), nunca en el GC heap de
 *     MicroPython ni en malloc del núcleo.
 *   · La descripción de flash (mfs_embedded_flash_t) también es estática.
 *   · Se soportan dos orígenes de medio:
 *       (a) un objeto "bdev" con readblocks/writeblocks/ioctl (protocolo
 *           genérico de MicroPython, el mismo que usa LittleFS), y
 *       (b) una etiqueta de partición (str) + tamaño, resuelta en ESP32 vía el
 *           módulo `esp32`/`esp` (no verificado; ver README).
 *
 * IMPORTANTE: este fichero NO se ha compilado contra MicroPython en este
 * repositorio; es una integración de referencia. Ver README.md.
 */

#include <string.h>

#include "py/misc.h"
#include "py/mpconfig.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objint.h"
#include "py/objstr.h"
#include "py/runtime.h"

/* Núcleo MatrixFS: API pública + definición interna de mf_t (estructura
 * opaca para los consumidores). El include path del usermod añade src/. */
#include "matrixfs/matrixfs.h"
#include "mfs_embedded.h"
#include "mfs_internal.h"

/* ==========================================================================
 * Primitivas obligatorias del puerto (mfs_port.h, §20.2)
 * ========================================================================== */

/* Fallback por si el port no define las macros de sección atómica. */
#ifndef MICROPY_BEGIN_ATOMIC_SECTION
#define MICROPY_BEGIN_ATOMIC_SECTION() (0)
#endif
#ifndef MICROPY_END_ATOMIC_SECTION
#define MICROPY_END_ATOMIC_SECTION(state) (void)(state)
#endif

/* Estado devuelto por la sección atómica; se conserva para restaurarlo en la
 * salida. El núcleo no anida secciones críticas, por lo que basta con una
 * única variable. */
static mp_uint_t g_atomic_state;

void mfs_port_crit_enter(void) {
  g_atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
}

void mfs_port_crit_exit(void) { MICROPY_END_ATOMIC_SECTION(g_atomic_state); }

/* MicroPython no expone un contador de ciclos portable; se usa la base de
 * tiempo monotónica en microsegundos (suficiente para las heurísticas del
 * núcleo que sólo requieren monotonía). Sustituir por el contador de ciclos
 * del SoC si se dispone de él. */
uint32_t mfs_port_cycles(void) { return (uint32_t)mp_hal_ticks_us(); }

uint32_t mfs_port_time_us(void) { return (uint32_t)mp_hal_ticks_us(); }

/* Reposo breve durante las ventanas de espera del núcleo. `mp_hal_delay_us(0)`
 * es la forma portable de ceder el control al port sin bloquear. */
void mfs_port_wfi(void) { mp_hal_delay_us(0); }

/* ==========================================================================
 * Traducción de errores tipificados -> excepción MicroPython
 * ========================================================================== */

/* Mapeo exigido por el contrato del módulo: sólo cuatro códigos tienen
 * equivalente directo; cualquier otro error negativo se reporta como EIO. */
static void matrixfs_raise(mfs_st st) {
  int err;
  switch (st) {
  case MFS_ENOENT:
    err = MP_ENOENT;
    break;
  case MFS_EEXISTS:
    err = MP_EEXIST;
    break;
  case MFS_EINVAL:
    err = MP_EINVAL;
    break;
  case MFS_ENOSPC:
    err = MP_ENOSPC;
    break;
  default:
    err = MP_EIO;
    break;
  }
  mp_raise_OSError(err);
}

/* ==========================================================================
 * Estado estático (una única instancia; sin heap del núcleo)
 * ========================================================================== */

#define MFS_MP_KIND_BDEV 0u
#define MFS_MP_KIND_ESP 1u

/* Tamaño máximo de bloque aceptado del bdev; cota del scratch estático. Los
 * bdevs de LittleFS usan típicamente 512 o 4096 B. */
#define MFS_MP_MAX_BLOCK 4096u

/* Comandos heredados del protocolo de bloque de MicroPython
 * (MP_BLOCKDEV_IOCTL_*). */
#define MFS_MP_IOCTL_BLOCK_COUNT 4
#define MFS_MP_IOCTL_BLOCK_SIZE 5
#define MFS_MP_IOCTL_BLOCK_ERASE 6

typedef struct {
  uint8_t kind;        /* MFS_MP_KIND_* */
  mp_obj_t obj;        /* bdev, o módulo `esp` en la ruta ESP32 */
  uint32_t base;       /* dirección física base de la región (bdev: 0) */
  uint32_t size;       /* bytes de la región */
  uint32_t block_size; /* unidad de borrado/lectura por bloques */
  uint32_t block_count;
  uint8_t scratch[MFS_MP_MAX_BLOCK]; /* buffer de un bloque para RMW */
} mfs_mp_flash_ctx_t;

static mfs_mp_flash_ctx_t g_ctx;
static mfs_embedded_flash_t g_flash;
static mf_t g_fs;         /* instancia del núcleo en BSS */
static uint8_t g_key[32]; /* copia de la clave (si se aporta) */
static bool g_key_valid;
static bool g_mounted;
static bool g_flash_ready;
static uint32_t g_default_ram; /* RAM por defecto fijada en el constructor */

/* ==========================================================================
 * Backend de medio (a) — objeto bdev con readblocks/writeblocks/ioctl
 * ========================================================================== */

/* Lee `len` bytes a partir de `addr` (dirección lineal dentro de la región)
 * usando llamadas de bloque de tamaño `block_size`. */
static mfs_st mp_bdev_read_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr,
                                void *dst, uint32_t len) {
  uint8_t *out = (uint8_t *)dst;
  while (len > 0u) {
    uint32_t blk = addr / ctx->block_size;
    uint32_t off = addr - blk * ctx->block_size;
    uint32_t chunk = ctx->block_size - off;
    if (chunk > len)
      chunk = len;
    mp_obj_t dest[4];
    mp_load_method(ctx->obj, MP_QSTR_readblocks, dest);
    dest[2] = MP_OBJ_NEW_SMALL_INT(blk);
    if (off == 0u && chunk == ctx->block_size) {
      /* Bloque completo: leer directamente en el destino, sin copia. */
      dest[3] = mp_obj_new_bytearray_by_ref(chunk, out);
      mp_call_method_n_kw(2, 0, dest);
    } else {
      dest[3] = mp_obj_new_bytearray_by_ref(ctx->block_size, ctx->scratch);
      mp_call_method_n_kw(2, 0, dest);
      memcpy(out, ctx->scratch + off, chunk);
    }
    addr += chunk;
    out += chunk;
    len -= chunk;
  }
  return MFS_OK;
}

/* Programa `len` bytes en `addr`. El medio se modela con semántica NOR y el
 * bdev sólo escribe bloques completos, por lo que se hace read-modify-write
 * para no destruir las páginas vecinas del mismo bloque. */
static mfs_st mp_bdev_prog_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr,
                                const void *src, uint32_t len) {
  const uint8_t *in = (const uint8_t *)src;
  while (len > 0u) {
    uint32_t blk = addr / ctx->block_size;
    uint32_t off = addr - blk * ctx->block_size;
    uint32_t chunk = ctx->block_size - off;
    if (chunk > len)
      chunk = len;
    mp_obj_t dest[4];
    /* 1) leer el bloque a scratch */
    mp_load_method(ctx->obj, MP_QSTR_readblocks, dest);
    dest[2] = MP_OBJ_NEW_SMALL_INT(blk);
    dest[3] = mp_obj_new_bytearray_by_ref(ctx->block_size, ctx->scratch);
    mp_call_method_n_kw(2, 0, dest);
    /* 2) superponer los bytes nuevos */
    memcpy(ctx->scratch + off, in, chunk);
    /* 3) reescribir el bloque completo */
    mp_load_method(ctx->obj, MP_QSTR_writeblocks, dest);
    dest[2] = MP_OBJ_NEW_SMALL_INT(blk);
    dest[3] = mp_obj_new_bytearray_by_ref(ctx->block_size, ctx->scratch);
    mp_call_method_n_kw(2, 0, dest);
    addr += chunk;
    in += chunk;
    len -= chunk;
  }
  return MFS_OK;
}

/* Borra el bloque que contiene `addr` (alineado por el núcleo). */
static mfs_st mp_bdev_erase_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr) {
  uint32_t blk = addr / ctx->block_size;
  mp_obj_t dest[4];
  mp_load_method(ctx->obj, MP_QSTR_ioctl, dest);
  dest[2] = MP_OBJ_NEW_SMALL_INT(MFS_MP_IOCTL_BLOCK_ERASE);
  dest[3] = MP_OBJ_NEW_SMALL_INT(blk);
  mp_call_method_n_kw(2, 0, dest);
  return MFS_OK;
}

/* Wrappers con red de seguridad: si el objeto Python levanta una excepción se
 * consume y se reporta MFS_EIO para no propagar un longjmp a través del núcleo
 * (que no es consciente de nlr). */
static mfs_st mp_bdev_read(void *c, uint32_t addr, void *dst, uint32_t len) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_bdev_read_impl((mfs_mp_flash_ctx_t *)c, addr, dst, len);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

static mfs_st mp_bdev_prog(void *c, uint32_t addr, const void *src,
                           uint32_t len) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_bdev_prog_impl((mfs_mp_flash_ctx_t *)c, addr, src, len);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

static mfs_st mp_bdev_erase(void *c, uint32_t addr) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_bdev_erase_impl((mfs_mp_flash_ctx_t *)c, addr);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

/* ==========================================================================
 * Backend de medio (b) — partición de flash ESP32 (etiqueta str)
 * ==========================================================================
 * No verificado: depende de la API del módulo `esp`/`esp32` de MicroPython.
 * Si la resolución de la partición o las primitivas de flash no están
 * disponibles, la construcción del objeto falla con ENOTSUP y se invita a usar
 * un bdev.
 */

static mfs_st mp_esp_read_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr,
                               void *dst, uint32_t len) {
  mp_obj_t dest[4];
  mp_load_method(ctx->obj, MP_QSTR_flash_read, dest);
  dest[2] = mp_obj_new_int_from_uint(addr);
  dest[3] = mp_obj_new_bytearray_by_ref(len, dst);
  mp_call_method_n_kw(2, 0, dest);
  return MFS_OK;
}

static mfs_st mp_esp_prog_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr,
                               const void *src, uint32_t len) {
  mp_obj_t dest[4];
  mp_load_method(ctx->obj, MP_QSTR_flash_write, dest);
  dest[2] = mp_obj_new_int_from_uint(addr);
  /* flash_write() copia el buffer; se construye un bytes temporal. */
  dest[3] = mp_obj_new_bytes((const byte *)src, len);
  mp_call_method_n_kw(2, 0, dest);
  return MFS_OK;
}

static mfs_st mp_esp_erase_impl(mfs_mp_flash_ctx_t *ctx, uint32_t addr) {
  uint32_t aligned = addr - (addr % ctx->block_size);
  mp_obj_t dest[4];
  mp_load_method(ctx->obj, MP_QSTR_flash_erase, dest);
  dest[2] = mp_obj_new_int_from_uint(aligned);
  dest[3] = mp_obj_new_int_from_uint(ctx->block_size);
  mp_call_method_n_kw(2, 0, dest);
  return MFS_OK;
}

static mfs_st mp_esp_read(void *c, uint32_t addr, void *dst, uint32_t len) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_esp_read_impl((mfs_mp_flash_ctx_t *)c, addr, dst, len);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

static mfs_st mp_esp_prog(void *c, uint32_t addr, const void *src,
                          uint32_t len) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_esp_prog_impl((mfs_mp_flash_ctx_t *)c, addr, src, len);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

static mfs_st mp_esp_erase(void *c, uint32_t addr) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mfs_st st = mp_esp_erase_impl((mfs_mp_flash_ctx_t *)c, addr);
    nlr_pop();
    return st;
  }
  return MFS_EIO;
}

/* Resuelve la partición por etiqueta e inicializa g_ctx para la ruta ESP32.
 * Devuelve true si se pudo preparar el backend. */
static bool mp_esp_prepare(mfs_mp_flash_ctx_t *ctx, const char *label,
                           uint32_t size_arg) {
  bool ok = false;
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t esp =
        mp_import_name(MP_QSTR_esp, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t esp32 =
        mp_import_name(MP_QSTR_esp32, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t part_cls = mp_load_attr(esp32, MP_QSTR_Partition);
    mp_obj_t find_fn = mp_load_attr(part_cls, MP_QSTR_find);
    mp_obj_t kw[2] = {MP_OBJ_NEW_QSTR(MP_QSTR_label),
                      mp_obj_new_str(label, strlen(label))};
    mp_obj_t found = mp_call_function_n_kw(find_fn, 0, 1, kw);
    size_t n = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(found, &n, &items);
    if (n > 0u) {
      mp_obj_t dest[4];
      mp_load_method(items[0], MP_QSTR_info, dest);
      mp_obj_t info = mp_call_method_n_kw(0, 0, dest);
      size_t ni = 0;
      mp_obj_t *iv = NULL;
      mp_obj_get_array(info, &ni, &iv);
      if (ni >= 4u) {
        ctx->kind = MFS_MP_KIND_ESP;
        ctx->obj = esp;
        ctx->base = (uint32_t)mp_obj_get_int(iv[2]);
        ctx->size = (uint32_t)mp_obj_get_int(iv[3]);
        ctx->block_size = 4096u;
        ctx->block_count = ctx->size / ctx->block_size;
        if (ctx->size == 0u && size_arg != 0u)
          ctx->size = size_arg;
        ok = true;
      }
    }
    nlr_pop();
  }
  return ok;
}

/* ==========================================================================
 * Utilidades comunes
 * ========================================================================== */

/* Normaliza la ruta a la forma canónica del núcleo ("/a/b"): '\' -> '/',
 * barra inicial garantizada y sin barra final (salvo la raíz). */
static void mp_norm_path(const char *in, char *out, size_t cap) {
  if (!in || cap == 0u) {
    if (cap)
      out[0] = '\0';
    return;
  }
  size_t o = 0u;
  if (in[0] != '/' && in[0] != '\\') {
    if (o + 1u < cap)
      out[o++] = '/';
  }
  for (size_t i = 0u; in[i] != '\0' && o + 1u < cap; i++) {
    char c = in[i];
    if (c == '\\')
      c = '/';
    if (c == '/' && o > 0u && out[o - 1u] == '/')
      continue;
    out[o++] = c;
  }
  while (o > 1u && out[o - 1u] == '/')
    o--;
  out[o] = '\0';
  if (o == 0u)
    strcpy(out, "/");
}

/* Interpreta el parámetro `mode` de `open()`/'r','w','a','x' + 'b','+'. */
static bool mp_parse_open_mode(const char *m, uint32_t *flags) {
  if (!m || m[0] == '\0')
    return false;
  uint32_t f;
  switch (m[0]) {
  case 'r':
    f = MFS_O_RDONLY;
    break;
  case 'w':
    f = MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC;
    break;
  case 'a':
    f = MFS_O_RDWR | MFS_O_CREAT | MFS_O_APPEND;
    break;
  case 'x':
    f = MFS_O_RDWR | MFS_O_CREAT | MFS_O_EXCL;
    break;
  default:
    return false;
  }
  for (size_t i = 1u; m[i] != '\0'; i++) {
    switch (m[i]) {
    case '+':
      f = (f & ~MFS_O_RDWR) | MFS_O_RDWR;
      break;
    case 'b':
    case 't':
      break; /* binario/texto: el núcleo es binario */
    case 'x':
      f |= MFS_O_EXCL;
      break;
    default:
      return false;
    }
  }
  *flags = f;
  return true;
}

/* Traduce el nombre de modo a mfs_mode_t. Devuelve MFS_MODE_UNSUPPORTED si el
 * nombre no es válido (se trata como error por quien llama). */
static mfs_mode_t mp_parse_mode_name(const char *name) {
  if (strcmp(name, "ultra-nano") == 0)
    return MFS_MODE_ULTRA_NANO;
  if (strcmp(name, "nano") == 0)
    return MFS_MODE_NANO;
  if (strcmp(name, "compact") == 0)
    return MFS_MODE_COMPACT;
  if (strcmp(name, "balanced") == 0)
    return MFS_MODE_BALANCED;
  if (strcmp(name, "extended") == 0)
    return MFS_MODE_EXTENDED;
  if (strcmp(name, "8bit-ultra") == 0)
    return MFS_MODE_8BIT_ULTRA;
  if (strcmp(name, "8bit-nano") == 0)
    return MFS_MODE_8BIT_NANO;
  if (strcmp(name, "8bit-compact") == 0)
    return MFS_MODE_8BIT_COMPACT;
  return MFS_MODE_UNSUPPORTED;
}

/* ==========================================================================
 * Clase File
 * ========================================================================== */

typedef struct {
  mp_obj_base_t base;
  mfs_file *f;
  mf_t *fs;
  bool closed;
} mfs_mp_file_obj_t;

static void matrixfs_check_open(mfs_mp_file_obj_t *self) {
  if (self->closed || self->f == NULL)
    mp_raise_ValueError(MP_ERROR_TEXT("archivo cerrado"));
}

static mp_obj_t matrixfs_file_read(size_t n_args, const mp_obj_t *args) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(args[0]);
  matrixfs_check_open(self);
  vstr_t vstr;
  if (n_args >= 2u) {
    mp_int_t n = mp_obj_get_int(args[1]);
    if (n < 0)
      mp_raise_ValueError(MP_ERROR_TEXT("n negativo"));
    if (n == 0)
      return mp_obj_new_bytes((const byte *)"", 0u);
    vstr_init_len(&vstr, (size_t)n);
    size_t total = 0u;
    while (total < (size_t)n) {
      size_t rd = 0u;
      int st = mf_read(self->f, vstr.buf + total, (size_t)n - total, &rd);
      if (st != MFS_OK) {
        vstr_free(&vstr);
        matrixfs_raise((mfs_st)st);
      }
      if (rd == 0u)
        break;
      total += rd;
    }
    vstr.len = total;
  } else {
    /* Sin argumento: leer hasta EOF por bloques. */
    vstr_init(&vstr, 256u);
    for (;;) {
      vstr_add_len(&vstr, 256u);
      size_t rd = 0u;
      int st = mf_read(self->f, vstr.buf + vstr.len - 256u, 256u, &rd);
      if (st != MFS_OK) {
        vstr_free(&vstr);
        matrixfs_raise((mfs_st)st);
      }
      if (rd < 256u) {
        vstr.len -= (256u - rd);
        break;
      }
    }
  }
  mp_obj_t res = mp_obj_new_bytes((const byte *)vstr.buf, vstr.len);
  vstr_free(&vstr);
  return res;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(matrixfs_file_read_obj, 1, 2,
                                           matrixfs_file_read);

static mp_obj_t matrixfs_file_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
  matrixfs_check_open(self);
  mp_buffer_info_t bi;
  mp_get_buffer_raise(buf_in, &bi, MP_BUFFER_WRITE);
  size_t rd = 0u;
  int st = mf_read(self->f, bi.buf, bi.len, &rd);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);
  return MP_OBJ_NEW_SMALL_INT((mp_int_t)rd);
}
static MP_DEFINE_CONST_FUN_OBJ_2(matrixfs_file_readinto_obj,
                                 matrixfs_file_readinto);

static mp_obj_t matrixfs_file_write(mp_obj_t self_in, mp_obj_t buf_in) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
  matrixfs_check_open(self);
  mp_buffer_info_t bi;
  mp_get_buffer_raise(buf_in, &bi, MP_BUFFER_READ);
  size_t wr = 0u;
  int st = mf_write(self->f, bi.buf, bi.len, &wr);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);
  return MP_OBJ_NEW_SMALL_INT((mp_int_t)wr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(matrixfs_file_write_obj, matrixfs_file_write);

static mp_obj_t matrixfs_file_seek(size_t n_args, const mp_obj_t *args) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(args[0]);
  matrixfs_check_open(self);
  mp_int_t pos = mp_obj_get_int(args[1]);
  int whence = MFS_SEEK_SET;
  if (n_args >= 3u) {
    mp_int_t w = mp_obj_get_int(args[2]);
    if (w == 1)
      whence = MFS_SEEK_CUR;
    else if (w == 2)
      whence = MFS_SEEK_END;
    else if (w != 0)
      mp_raise_ValueError(MP_ERROR_TEXT("whence invalido"));
  }
  int st = mf_seek(self->f, (int64_t)pos, whence);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);
  uint64_t cur = 0u;
  (void)mf_tell(self->f, &cur);
  return mp_obj_new_int_from_uint((uint32_t)cur);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(matrixfs_file_seek_obj, 2, 3,
                                           matrixfs_file_seek);

static mp_obj_t matrixfs_file_tell(mp_obj_t self_in) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
  matrixfs_check_open(self);
  uint64_t pos = 0u;
  int st = mf_tell(self->f, &pos);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);
  return mp_obj_new_int_from_uint((uint32_t)pos);
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_file_tell_obj, matrixfs_file_tell);

static mp_obj_t matrixfs_file_close(mp_obj_t self_in) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->closed && self->f != NULL) {
    (void)mf_close(self->f);
    self->f = NULL;
    self->closed = true;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_file_close_obj, matrixfs_file_close);

static mp_obj_t matrixfs_file_flush(mp_obj_t self_in) {
  mfs_mp_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
  matrixfs_check_open(self);
  int st = mf_sync(self->fs);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_file_flush_obj, matrixfs_file_flush);

static const mp_rom_map_elem_t matrixfs_file_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&matrixfs_file_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&matrixfs_file_readinto_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&matrixfs_file_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&matrixfs_file_seek_obj)},
    {MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&matrixfs_file_tell_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&matrixfs_file_close_obj)},
    {MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&matrixfs_file_flush_obj)},
};
static MP_DEFINE_CONST_DICT(matrixfs_file_locals_dict,
                            matrixfs_file_locals_dict_table);

/* MP_DEFINE_CONST_OBJ_TYPE con forma variádica requiere MicroPython >= 1.19.
 * Si se compila contra una versión anterior, sustituir por una definición
 * explícita de `const mp_obj_type_t` con `.locals_dict`. */
MP_DEFINE_CONST_OBJ_TYPE(matrixfs_file_type, MP_QSTR_File, MP_TYPE_FLAG_NONE,
                         locals_dict, &matrixfs_file_locals_dict);

/* ==========================================================================
 * Clase MatrixFS
 * ========================================================================== */

typedef struct {
  mp_obj_base_t base;
} mfs_mp_obj_t;

static void matrixfs_fill_opts(mfs_embedded_opts *o, uint32_t ram,
                               mfs_mode_t mode, bool fmt) {
  mfs_embedded_opts_default(o);
  o->ram_total = ram ? ram : 32768u;
  o->forced_mode = mode;
  o->format_if_needed = fmt;
  o->key = g_key_valid ? g_key : NULL;
}

static void matrixfs_check_ready(void) {
  if (!g_flash_ready)
    mp_raise_OSError(MP_EINVAL);
}

static mp_obj_t matrixfs_format(mp_obj_t self_in) {
  (void)self_in;
  matrixfs_check_ready();
  mfs_embedded_opts o;
  matrixfs_fill_opts(&o, g_default_ram, MFS_MODE_UNSUPPORTED, false);
  mfs_st st = mfs_embedded_format(&g_fs, &g_flash, &o);
  if (st != MFS_OK)
    matrixfs_raise(st);
  g_mounted = false;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_format_obj, matrixfs_format);

static mp_obj_t matrixfs_mount(size_t n_args, const mp_obj_t *pos_args,
                               mp_map_t *kw_args) {
  enum { ARG_ram, ARG_mode, ARG_key, ARG_format };
  static const mp_arg_t allowed[] = {
      {MP_QSTR_ram, MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_mode, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
      {MP_QSTR_key, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
      {MP_QSTR_format, MP_ARG_BOOL, {.u_bool = false}},
  };
  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  mp_arg_parse_all(n_args - 1u, pos_args + 1u, kw_args, MP_ARRAY_SIZE(allowed),
                   allowed, a);

  matrixfs_check_ready();

  mfs_mode_t mode = MFS_MODE_UNSUPPORTED;
  if (a[ARG_mode].u_obj != mp_const_none) {
    const char *name = mp_obj_str_get_str(a[ARG_mode].u_obj);
    mode = mp_parse_mode_name(name);
    if (mode == MFS_MODE_UNSUPPORTED)
      mp_raise_ValueError(MP_ERROR_TEXT("modo desconocido"));
  }

  if (a[ARG_key].u_obj != mp_const_none) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(a[ARG_key].u_obj, &bi, MP_BUFFER_READ);
    if (bi.len != 32u)
      mp_raise_ValueError(MP_ERROR_TEXT("la clave debe tener 32 bytes"));
    memcpy(g_key, bi.buf, 32u);
    g_key_valid = true;
  } else {
    g_key_valid = false;
  }

  mfs_embedded_opts o;
  uint32_t ram = a[ARG_ram].u_int ? (uint32_t)a[ARG_ram].u_int : g_default_ram;
  matrixfs_fill_opts(&o, ram, mode, a[ARG_format].u_bool);
  mfs_st st = mfs_embedded_mount(&g_fs, &g_flash, &o);
  if (st != MFS_OK)
    matrixfs_raise(st);
  g_mounted = true;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrixfs_mount_obj, 1, matrixfs_mount);

static mp_obj_t matrixfs_umount(mp_obj_t self_in) {
  (void)self_in;
  if (g_mounted) {
    (void)mf_deinit(&g_fs);
    g_mounted = false;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_umount_obj, matrixfs_umount);

static mp_obj_t matrixfs_open(size_t n_args, const mp_obj_t *pos_args,
                              mp_map_t *kw_args) {
  enum { ARG_path, ARG_mode };
  static const mp_arg_t allowed[] = {
      {MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
      {MP_QSTR_mode, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_r)}},
  };
  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  mp_arg_parse_all(n_args - 1u, pos_args + 1u, kw_args, MP_ARRAY_SIZE(allowed),
                   allowed, a);
  if (!g_mounted)
    mp_raise_OSError(MP_EINVAL);

  char np[256];
  mp_norm_path(mp_obj_str_get_str(a[ARG_path].u_obj), np, sizeof(np));
  uint32_t flags = 0u;
  if (!mp_parse_open_mode(mp_obj_str_get_str(a[ARG_mode].u_obj), &flags))
    mp_raise_ValueError(MP_ERROR_TEXT("modo de apertura invalido"));

  mfs_file *f = NULL;
  int st = mf_open(&g_fs, np, flags, &f);
  if (st != MFS_OK)
    matrixfs_raise((mfs_st)st);

  mfs_mp_file_obj_t *self = m_new_obj(mfs_mp_file_obj_t);
  self->base.type = &matrixfs_file_type;
  self->f = f;
  self->fs = &g_fs;
  self->closed = false;
  return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(matrixfs_open_obj, 1, matrixfs_open);

static mp_obj_t matrixfs_stat(mp_obj_t self_in, mp_obj_t path_in) {
  (void)self_in;
  if (!g_mounted)
    mp_raise_OSError(MP_EINVAL);
  char np[256];
  mp_norm_path(mp_obj_str_get_str(path_in), np, sizeof(np));
  mfs_stat st;
  memset(&st, 0, sizeof(st));
  int r = mf_stat(&g_fs, np, &st);
  if (r != MFS_OK)
    matrixfs_raise((mfs_st)r);
  mp_obj_t d = mp_obj_new_dict(5);
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_size),
                    mp_obj_new_int_from_uint(st.size));
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mode),
                    MP_OBJ_NEW_SMALL_INT(st.mode));
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_uid),
                    mp_obj_new_int_from_uint(st.uid));
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_gid),
                    mp_obj_new_int_from_uint(st.gid));
  mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_mtime),
                    mp_obj_new_int_from_uint(st.mtime));
  return d;
}
static MP_DEFINE_CONST_FUN_OBJ_2(matrixfs_stat_obj, matrixfs_stat);

static mp_obj_t matrixfs_listdir(size_t n_args, const mp_obj_t *args) {
  (void)args[0];
  if (!g_mounted)
    mp_raise_OSError(MP_EINVAL);
  char np[256];
  if (n_args >= 2u)
    mp_norm_path(mp_obj_str_get_str(args[1]), np, sizeof(np));
  else
    mp_norm_path("/", np, sizeof(np));

  mfs_dir *d = NULL;
  int r = mf_opendir(&g_fs, np, &d);
  if (r != MFS_OK)
    matrixfs_raise((mfs_st)r);
  mp_obj_t list = mp_obj_new_list(0, NULL);
  for (;;) {
    mfs_dirent de;
    memset(&de, 0, sizeof(de));
    int rr = mf_readdir(d, &de);
    if (rr == MFS_ENOENT)
      break;
    if (rr != MFS_OK) {
      (void)mf_closedir(d);
      matrixfs_raise((mfs_st)rr);
    }
    mp_obj_list_append(list, mp_obj_new_str(de.name, strlen(de.name)));
  }
  (void)mf_closedir(d);
  return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(matrixfs_listdir_obj, 1, 2,
                                           matrixfs_listdir);

static mp_obj_t matrixfs_mkfs(mp_obj_t self_in) {
  return matrixfs_format(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(matrixfs_mkfs_obj, matrixfs_mkfs);

/* Constructor: MatrixFS(source[, size][, ram=]) donde `source` es una etiqueta
 * de partición (str) o un objeto bdev (con readblocks). */
static mp_obj_t matrixfs_make_new(const mp_obj_type_t *type, size_t n_args,
                                  size_t n_kw, const mp_obj_t *all_args) {
  enum { ARG_source, ARG_size, ARG_ram };
  static const mp_arg_t allowed[] = {
      {MP_QSTR_source,
       MP_ARG_REQUIRED | MP_ARG_OBJ,
       {.u_rom_obj = MP_ROM_NONE}},
      {MP_QSTR_size, MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_ram, MP_ARG_INT, {.u_int = 0}},
  };
  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
                            allowed, a);

  memset(&g_ctx, 0, sizeof(g_ctx));
  memset(&g_flash, 0, sizeof(g_flash));
  g_flash_ready = false;
  g_mounted = false;
  g_key_valid = false;
  g_default_ram = (uint32_t)a[ARG_ram].u_int;

  mp_obj_t source = a[ARG_source].u_obj;
  if (mp_obj_is_type(source, &mp_type_str)) {
    /* Ruta (b): etiqueta de partición de flash (sólo ESP32, best-effort). */
    const char *label = mp_obj_str_get_str(source);
    if (!mp_esp_prepare(&g_ctx, label, (uint32_t)a[ARG_size].u_int)) {
      /* Best-effort en ESP32; en otros ports, usar un bdev. */
      mp_raise_OSError(MP_ENOTSUP);
    }
    g_flash.read = mp_esp_read;
    g_flash.prog = mp_esp_prog;
    g_flash.erase = mp_esp_erase;
  } else {
    /* Ruta (a): objeto bdev con protocolo de bloque. */
    mp_obj_t dest[2];
    if (!mp_load_method_maybe(source, MP_QSTR_readblocks, dest)) {
      mp_raise_TypeError(MP_ERROR_TEXT("se espera un bdev con readblocks o "
                                       "una etiqueta str"));
    }
    g_ctx.kind = MFS_MP_KIND_BDEV;
    g_ctx.obj = source;
    g_ctx.base = 0u;
    /* Tamaño de bloque y número de bloques por ioctl (protocolo MicroPython).
     */
    mp_obj_t bd[4];
    mp_load_method(source, MP_QSTR_ioctl, bd);
    bd[2] = MP_OBJ_NEW_SMALL_INT(MFS_MP_IOCTL_BLOCK_SIZE);
    bd[3] = MP_OBJ_NEW_SMALL_INT(0);
    mp_int_t bs = mp_obj_get_int(mp_call_method_n_kw(2, 0, bd));
    if (bs <= 0 || (uint32_t)bs > MFS_MP_MAX_BLOCK ||
        ((uint32_t)bs & ((uint32_t)bs - 1u)) != 0u) {
      mp_raise_ValueError(MP_ERROR_TEXT("block_size de bdev invalido"));
    }
    g_ctx.block_size = (uint32_t)bs;
    mp_load_method(source, MP_QSTR_ioctl, bd);
    bd[2] = MP_OBJ_NEW_SMALL_INT(MFS_MP_IOCTL_BLOCK_COUNT);
    bd[3] = MP_OBJ_NEW_SMALL_INT(0);
    g_ctx.block_count = (uint32_t)mp_obj_get_int(mp_call_method_n_kw(2, 0, bd));
    g_ctx.size = g_ctx.block_count * g_ctx.block_size;
    g_flash.read = mp_bdev_read;
    g_flash.prog = mp_bdev_prog;
    g_flash.erase = mp_bdev_erase;
  }

  if (g_ctx.size == 0u)
    mp_raise_ValueError(MP_ERROR_TEXT("region de flash vacia"));

  g_flash.ctx = &g_ctx;
  g_flash.base_addr = g_ctx.base;
  g_flash.size = g_ctx.size;
  g_flash.erase_unit = g_ctx.block_size;
  g_flash.page_size =
      (g_ctx.block_size < 256u) ? (uint16_t)g_ctx.block_size : 256u;
  g_flash.no_erase = false;
  g_flash_ready = true;

  mfs_mp_obj_t *self = m_new_obj(mfs_mp_obj_t);
  self->base.type = type;
  return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t matrixfs_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_format), MP_ROM_PTR(&matrixfs_format_obj)},
    {MP_ROM_QSTR(MP_QSTR_mkfs), MP_ROM_PTR(&matrixfs_mkfs_obj)},
    {MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&matrixfs_mount_obj)},
    {MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&matrixfs_umount_obj)},
    {MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&matrixfs_open_obj)},
    {MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&matrixfs_stat_obj)},
    {MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&matrixfs_listdir_obj)},
};
static MP_DEFINE_CONST_DICT(matrixfs_locals_dict, matrixfs_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(matrixfs_type, MP_QSTR_MatrixFS, MP_TYPE_FLAG_NONE,
                         make_new, matrixfs_make_new, locals_dict,
                         &matrixfs_locals_dict);

/* ==========================================================================
 * Registro del módulo
 * ========================================================================== */

static const mp_rom_map_elem_t mp_module_matrixfs_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_matrixfs)},
    {MP_ROM_QSTR(MP_QSTR_MatrixFS), MP_ROM_PTR(&matrixfs_type)},
    {MP_ROM_QSTR(MP_QSTR_VERSION_MAJOR), MP_ROM_INT(MFS_VERSION_MAJOR)},
    {MP_ROM_QSTR(MP_QSTR_VERSION_MINOR), MP_ROM_INT(MFS_VERSION_MINOR)},
};
static MP_DEFINE_CONST_DICT(mp_module_matrixfs_globals,
                            mp_module_matrixfs_globals_table);

const mp_obj_module_t mp_module_matrixfs = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mp_module_matrixfs_globals,
};

MP_REGISTER_MODULE(MP_QSTR_matrixfs, mp_module_matrixfs);
