/* MatrixFS.cpp — envoltorio Arduino (C++) de MatrixFS Ultra «ATLAS».
 *
 * Licencia: Apache-2.0.
 *
 * Este fichero aporta:
 *   · Las primitivas de puerto obligatorias del núcleo (mfs_port_*), con
 *     enlazado C, usando APIs nativas de cada core Arduino.
 *   · Los callbacks read/prog/erase sobre la región de flash reservada.
 *   · La implementación de la clase MatrixFS.
 *
 * El núcleo MatrixFS (src/core, src/crypto, src/ftl, src/tier, src/sec,
 * src/xio y platform/embedded/mfs_embedded.c) NO se compila aquí: debe
 * aportarlo el proyecto. Véase README.md.
 */

#include "MatrixFS.h"

#include <string.h>

/* ==== Selección de arquitectura ======================================== */
#if !defined(ESP32) && !defined(ESP8266) && !defined(ARDUINO_ARCH_RP2040)
#error                                                                         \
    "MatrixFS Arduino: arquitectura no reconocida. Aporta los callbacks de flash y las primitivas mfs_port_* para tu MCU (por ejemplo STM32)."
#endif

#if defined(ESP32)
#include "esp_partition.h"
#elif defined(ARDUINO_ARCH_RP2040)
#include "hardware/flash.h"
#include "pico/stdlib.h"
#ifndef XIP_BASE
#define XIP_BASE 0x10000000u /* base del mapeo XIP de la flash en RP2040 */
#endif
#endif

/* Definición completa de struct mfs_fs (tipo opaco en la API pública). El
 * núcleo es C; se envuelve en extern "C" para dar a sus prototipos el mismo
 * enlazado. Sólo se usa aquí para declarar la instancia estática y leer su
 * modo activo. */
extern "C" {
#include "mfs_internal.h"
}

/* ==== Contexto de la región reservada (compartido con los callbacks) ==== */
typedef struct {
  uint32_t base; /* dirección absoluta en el mapa del MCU (base_addr) */
  uint32_t size;
#if defined(ESP32)
  const esp_partition_t *part;
#endif
} ard_region_t;

static ard_region_t s_region;
static bool s_in_use = false; /* una sola instancia montada a la vez */

#if defined(ARDUINO_ARCH_RP2040)
static uint32_t s_crit_state = 0u; /* estado de PRIMASK en la crítica */
#endif

/* ==== Primitivas de puerto obligatorias (§20.2/§20.3) =================== */
/* Deben tener enlazado C: el núcleo está compilado como C y las referencia con
 * esos nombres exactos. */

extern "C" void mfs_port_crit_enter(void) {
#if defined(ARDUINO_ARCH_RP2040)
  s_crit_state = save_and_disable_interrupts();
#else
  noInterrupts();
#endif
}

extern "C" void mfs_port_crit_exit(void) {
#if defined(ARDUINO_ARCH_RP2040)
  restore_interrupts(s_crit_state);
#else
  interrupts();
#endif
}

extern "C" uint32_t mfs_port_cycles(void) {
  /* Los cores Arduino no exponen un contador de ciclos portable; se usa
   * micros() como fuente monótona. Los presupuestos del núcleo se miden con
   * mfs_port_time_us(). */
  return (uint32_t)micros();
}

extern "C" uint32_t mfs_port_time_us(void) { return (uint32_t)micros(); }

extern "C" void mfs_port_wfi(void) {
#if defined(ARDUINO_ARCH_RP2040)
  __wfi();
#else
  delay(0);
  yield();
#endif
}

/* ==== Callbacks de flash por arquitectura ============================== */
/* La capa embebida (mfs_embedded.c) pasa a estos callbacks direcciones
 * ABSOLUTAS del mapa del MCU (base_addr + offset relativo). */

#if defined(ESP32)
/* ESP32: partición del SDK (esp_partition.h). Los offsets de la API de
 * partición son RELATIVOS al inicio de la partición, de ahí la resta. */
#define ARD_ERASE_UNIT 4096u

static mfs_st ard_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  (void)ctx;
  const esp_partition_t *p = s_region.part;
  if (!p)
    return MFS_EINVAL;
  esp_err_t e = esp_partition_read(p, (size_t)(addr - p->address), dst, len);
  return (e == ESP_OK) ? MFS_OK : MFS_EIO;
}

static mfs_st ard_prog(void *ctx, uint32_t addr, const void *src,
                       uint32_t len) {
  (void)ctx;
  const esp_partition_t *p = s_region.part;
  if (!p)
    return MFS_EINVAL;
  /* esp_partition_write retorna tras verificar el estado (barrera WOB). */
  esp_err_t e = esp_partition_write(p, (size_t)(addr - p->address), src, len);
  return (e == ESP_OK) ? MFS_OK : MFS_EIO;
}

static mfs_st ard_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  const esp_partition_t *p = s_region.part;
  if (!p)
    return MFS_EINVAL;
  esp_err_t e =
      esp_partition_erase_range(p, (size_t)(addr - p->address), ARD_ERASE_UNIT);
  return (e == ESP_OK) ? MFS_OK : MFS_EIO;
}

#elif defined(ESP8266)
/* ESP8266: la flash es direccionable de forma lineal; ESP.flashRead/
 * flashWrite/flashEraseSector usan direcciones/offsets de flash ABSOLUTOS. La
 * escritura requiere que el búfer de origen esté en RAM (no en flash), lo que
 * cumple el núcleo (buffers estáticos en RAM). */
static mfs_st ard_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  (void)ctx;
  return ESP.flashRead(addr, dst, len) ? MFS_OK : MFS_EIO;
}

static mfs_st ard_prog(void *ctx, uint32_t addr, const void *src,
                       uint32_t len) {
  (void)ctx;
  return ESP.flashWrite(addr, (const uint32_t *)src, len) ? MFS_OK : MFS_EIO;
}

static mfs_st ard_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  return ESP.flashEraseSector(addr) ? MFS_OK : MFS_EIO;
}

#elif defined(ARDUINO_ARCH_RP2040)
/* RP2040 (Arduino-Pico): la flash se lee por XIP (mapeada en memoria) y se
 * programa/borra con flash_range_* del SDK. save_and_disable_interrupts evita
 * que una ISR ejecute código desde la flash XIP mientras se reprograma.
 * IMPORTANTE: la región reservada debe excluirse del linker y de cualquier
 * otro uso (p. ej. del gestor de ficheros del core). */
static mfs_st ard_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  (void)ctx;
  memcpy(dst, (const void *)(XIP_BASE + addr), len);
  return MFS_OK;
}

static mfs_st ard_prog(void *ctx, uint32_t addr, const void *src,
                       uint32_t len) {
  (void)ctx;
  uint32_t save = save_and_disable_interrupts();
  flash_range_program(addr, (const uint8_t *)src, len);
  restore_interrupts(save);
  return MFS_OK;
}

static mfs_st ard_erase(void *ctx, uint32_t addr) {
  (void)ctx;
  uint32_t save = save_and_disable_interrupts();
  flash_range_erase(addr, FLASH_SECTOR_SIZE);
  restore_interrupts(save);
  return MFS_OK;
}
#endif

/* ==== Instancia única del núcleo ======================================= */
mf_t MatrixFS::_fs;

/* ==== Implementación de la clase ======================================= */

MatrixFS::MatrixFS(const char *partitionLabel, uint32_t ramTotal,
                   uint32_t sizeBytes, uint32_t baseAddr)
    : _label(partitionLabel ? partitionLabel : "matrixfs"),
      _ramTotal(ramTotal), _sizeBytes(sizeBytes), _baseAddr(baseAddr),
      _mounted(false), _lastError(MFS_OK) {
  memset(&_flash, 0, sizeof(_flash));
  mfs_embedded_opts_default(&_opts);
}

MatrixFS::~MatrixFS() { end(); }

bool MatrixFS::resolveRegion() {
#if defined(ESP32)
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, _label);
  if (!p) {
    _lastError = MFS_ENOENT;
    return false;
  }
  s_region.part = p;
  s_region.base = p->address;
  s_region.size = p->size;
#else
  if (_sizeBytes == 0u || (_sizeBytes % 4096u) != 0u) {
    _lastError = MFS_EINVAL;
    return false;
  }
  if ((_baseAddr % 4096u) != 0u) {
    _lastError = MFS_EINVAL;
    return false;
  }
  s_region.base = _baseAddr;
  s_region.size = _sizeBytes;
#endif
  return true;
}

void MatrixFS::configureFlash() {
  memset(&_flash, 0, sizeof(_flash));
  _flash.read = ard_read;
  _flash.prog = ard_prog;
  _flash.erase = ard_erase;
  _flash.ctx = &s_region;
  _flash.base_addr = s_region.base;
  _flash.size = s_region.size;
  _flash.erase_unit = 4096u;
  _flash.page_size = 256u;
  _flash.no_erase = false;
  /* t_prog_max_us/t_erase_max_us/t_read_max_us = 0 => valores por defecto
   * de mfs_embedded_setup(). */
}

bool MatrixFS::begin(bool formatIfNeeded) {
  if (_mounted)
    return true;
  if (s_in_use) {
    _lastError = MFS_EBUSY;
    return false;
  }
  if (!resolveRegion())
    return false;
  configureFlash();

  mfs_embedded_opts_default(&_opts);
  if (_ramTotal)
    _opts.ram_total = _ramTotal;
  _opts.format_if_needed = formatIfNeeded;

  mfs_st st = mfs_embedded_mount(&_fs, &_flash, &_opts);
  _lastError = st;
  if (st != MFS_OK)
    return false;
  _mounted = true;
  s_in_use = true;
  return true;
}

bool MatrixFS::format() {
  if (_mounted)
    end();
  else if (s_in_use) {
    _lastError = MFS_EBUSY;
    return false;
  }
  if (!resolveRegion())
    return false;
  configureFlash();

  mfs_embedded_opts_default(&_opts);
  if (_ramTotal)
    _opts.ram_total = _ramTotal;
  _opts.format_if_needed = false;

  mfs_st st = mfs_embedded_format(&_fs, &_flash, &_opts);
  _lastError = st;
  if (st != MFS_OK)
    return false;
  _mounted = true;
  s_in_use = true;
  return true;
}

void MatrixFS::end() {
  if (!_mounted)
    return;
  mf_deinit(&_fs);
  _mounted = false;
  s_in_use = false;
}

int MatrixFS::record(mfs_st st) {
  _lastError = st;
  return (int)st;
}

int MatrixFS::writeFile(const char *path, const void *data, size_t len) {
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  mfs_file *f = NULL;
  int r = mf_open(&_fs, path, MFS_O_WRONLY | MFS_O_CREAT | MFS_O_TRUNC, &f);
  if (r != MFS_OK)
    return record((mfs_st)r);
  size_t wr = 0;
  r = mf_write(f, data, len, &wr);
  int rc = mf_close(f);
  if (r == MFS_OK)
    r = rc;
  return record((mfs_st)r);
}

int MatrixFS::readFile(const char *path, void *buf, size_t cap, size_t *out) {
  if (out)
    *out = 0;
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  mfs_file *f = NULL;
  int r = mf_open(&_fs, path, MFS_O_RDONLY, &f);
  if (r != MFS_OK)
    return record((mfs_st)r);
  size_t rd = 0;
  r = mf_read(f, buf, cap, &rd);
  int rc = mf_close(f);
  if (r == MFS_OK)
    r = rc;
  if (out)
    *out = rd;
  return record((mfs_st)r);
}

int MatrixFS::mkdir(const char *path) {
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  return record((mfs_st)mf_mkdir(&_fs, path));
}

int MatrixFS::remove(const char *path) {
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  return record((mfs_st)mf_unlink(&_fs, path));
}

int MatrixFS::rename(const char *from, const char *to) {
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  return record((mfs_st)mf_rename(&_fs, from, to));
}

bool MatrixFS::exists(const char *path) {
  if (!_mounted)
    return false;
  mfs_stat st;
  return mf_stat(&_fs, path, &st) == MFS_OK;
}

static bool list_append(const mfs_dirent *entry, void *user) {
  String *out = static_cast<String *>(user);
  if (!out || !entry)
    return false;
  if (out->length())
    *out += '\n';
  *out += entry->name;
  return true;
}

int MatrixFS::list(const char *path, ListCallback cb, void *user) {
  if (!_mounted)
    return record(MFS_ENOTMOUNTED);
  mfs_dir *d = NULL;
  int r = mf_opendir(&_fs, path, &d);
  if (r != MFS_OK)
    return record((mfs_st)r);
  mfs_dirent de;
  for (;;) {
    r = mf_readdir(d, &de);
    if (r == MFS_ENOENT) { /* fin de iteración */
      r = MFS_OK;
      break;
    }
    if (r != MFS_OK)
      break;
    if (cb && !cb(&de, user))
      break;
  }
  mf_closedir(d);
  return record((mfs_st)r);
}

int MatrixFS::list(const char *path, String &out) {
  return list(path, list_append, &out);
}

mfs_mode_t MatrixFS::mode() const {
  if (!_mounted)
    return MFS_MODE_UNSUPPORTED;
  return (mfs_mode_t)_fs.mode;
}

const char *MatrixFS::lastErrorString() const { return mfs_ststr(_lastError); }
