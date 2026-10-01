/* mfs_blk.c — driver L2 sobre medio de bloques real o imagen de fichero.
 *
 * Implementa las operaciones del contrato de puerto (§20.2) sobre:
 *   · Linux/POSIX : imágenes de fichero, /dev/sdX, /dev/nvmeXnY, /dev/mtdN
 *   · Windows     : imágenes de fichero, \\.\X: y \\.\PhysicalDriveN
 *
 * Cuando el destino exige alineación a sector (volumen de bloques o MTD), las
 * programaciones de granularidad fina (NOR: 1 byte) se emulan con
 * read-modify-write del sector físico, de modo que la geometría lógica y el
 * layout on-flash de MatrixFS permanecen idénticos en todos los sistemas.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* pread/pwrite/fsync/fstat */
#endif

#include "mfs_blk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfs_plat.h"

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#if defined(__has_include)
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#define MFS_HAVE_BLKIO 1
#endif
#if __has_include(<mtd/mtd-user.h>)
#include <mtd/mtd-user.h>
#define MFS_HAVE_MTD 1
#endif
#endif
#endif
#endif

#define MFS_BLK_MAX_IO 65536u
/* Cota del bloque de borrado derivado (§8.1): acota el coste de un erase. */
#define MFS_BLK_MAX_ERASE (4u * 1024u * 1024u)

struct mfs_blk {
  char path[MFS_BLK_PATH_MAX];
#if defined(_WIN32)
  HANDLE h;
  bool is_volume; /* \\.\X: o \\.\PhysicalDriveN */
#else
  int fd;
  bool is_mtd;
#endif
  bool readonly;
  uint64_t size;       /* bytes accesibles (acotado a 4 GiB por el núcleo) */
  uint32_t rmw_sector; /* 0 ⇒ acceso byte-directo; >0 ⇒ RMW alineado */
  mfs_media_geom geom;
  mfs_l2_driver drv;
  mfs_mutex_t mtx;
};

void mfs_blk_opts_default(mfs_blk_opts *o) {
  if (!o)
    return;
  memset(o, 0, sizeof(*o));
  o->media = MFS_MEDIA_NOR_SPI;
  o->erase_unit = 0u; /* 0 ⇒ derivado del tamaño del medio (ver abajo) */
  o->program_granularity = 1u;
  o->page_size = 256u;
  o->oob_bytes = 0u;
  o->size_limit = 0u;
}

/* Bloque de borrado derivado del tamaño del medio. El núcleo mantiene
 * MFS_ZONE_MAX zonas ZLF en RAM, así que el bloque se elige para que esa
 * ventana cubra el volumen completo; la derivación es determinista a partir
 * del tamaño, de modo que formateo, montaje y sondeo calculan la misma
 * geometría (requisito de compatibilidad cruzada Linux/Windows). */
static uint32_t blk_auto_erase_unit(uint64_t size) {
  uint64_t want = size / (uint64_t)MFS_ZONE_MAX;
  uint32_t eu = 4096u;
  while ((uint64_t)eu < want && eu < MFS_BLK_MAX_ERASE)
    eu <<= 1;
  return eu;
}

/* ============================ E/S de bajo nivel ========================== */

#if defined(_WIN32)

static bool win_is_device_path(const char *p) {
  /* \\.\X:, \\.\PhysicalDriveN, \\?\Volume{GUID}\ */
  return (strncmp(p, "\\\\.\\", 4) == 0) || (strncmp(p, "\\\\?\\", 4) == 0);
}

/* Ruta UTF-8 (con reserva a ANSI) → UTF-16. Se usan las APIs W porque las
 * rutas \\?\Volume{...}\ que devuelve FindFirstVolumeW sólo las entiende el
 * front-end Unicode de Win32. */
static bool win_path_to_wide(const char *p, WCHAR *out, int cap) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, out, cap);
  if (n <= 0)
    n = MultiByteToWideChar(CP_ACP, 0, p, -1, out, cap);
  return n > 0;
}

static int blk_raw_io(mfs_blk *b, uint64_t off, void *buf, uint32_t len,
                      bool write) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(b->h, li, NULL, FILE_BEGIN))
    return -1;
  DWORD n = 0;
  BOOL ok = write ? WriteFile(b->h, buf, len, &n, NULL)
                  : ReadFile(b->h, buf, len, &n, NULL);
  if (!ok || n != len)
    return -1;
  return 0;
}

static void blk_detect_align(mfs_blk *b) {
  if (!b->is_volume)
    return;
  STORAGE_PROPERTY_QUERY q;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR d;
  DWORD br = 0;
  memset(&q, 0, sizeof(q));
  q.PropertyId = StorageAccessAlignmentProperty;
  q.QueryType = PropertyStandardQuery;
  memset(&d, 0, sizeof(d));
  if (DeviceIoControl(b->h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d,
                      sizeof(d), &br, NULL)) {
    if (d.BytesPerPhysicalSector >= 512u)
      b->rmw_sector = d.BytesPerPhysicalSector;
  }
  if (b->rmw_sector == 0u)
    b->rmw_sector = 512u;
}

#else /* POSIX */

static int blk_raw_io(mfs_blk *b, uint64_t off, void *buf, uint32_t len,
                      bool write) {
  size_t done = 0u;
  while (done < len) {
    ssize_t n = write ? pwrite(b->fd, (const uint8_t *)buf + done, len - done,
                               (off_t)(off + done))
                      : pread(b->fd, (uint8_t *)buf + done, len - done,
                              (off_t)(off + done));
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static void blk_detect_align(mfs_blk *b) {
  if (b->is_mtd)
    return; /* en MTD la granularidad la fija el propio dispositivo */
  struct stat sb;
  if (fstat(b->fd, &sb) != 0)
    return;
  if (!S_ISBLK(sb.st_mode))
    return; /* fichero imagen: acceso byte-directo */
#if defined(MFS_HAVE_BLKIO)
  {
    int ssz = 0;
    if (ioctl(b->fd, BLKSSZGET, &ssz) == 0 && ssz >= 512)
      b->rmw_sector = (uint32_t)ssz;
  }
#endif
  if (b->rmw_sector == 0u)
    b->rmw_sector = 512u;
}

#endif

/* Lee `len` bytes del medio. Si el destino exige alineación a sector (volumen
 * crudo o disco físico), se lee el sector completo y se copia el fragmento
 * pedido: Windows rechaza las E/S no alineadas sobre handles de volumen. */
static int blk_pread(mfs_blk *b, uint64_t off, void *dst, uint32_t len) {
  if (off + len > b->size)
    return -1;
  if (b->rmw_sector == 0u)
    return blk_raw_io(b, off, dst, len, false);

  uint8_t *p = (uint8_t *)dst;
  uint32_t sec = b->rmw_sector;
  static uint8_t tmp[MFS_BLK_MAX_IO];
  if (sec > sizeof(tmp))
    return -1;
  while (len > 0u) {
    uint64_t soff = off - (off % sec);
    uint32_t o = (uint32_t)(off - soff);
    uint32_t n = sec - o;
    if (n > len)
      n = len;
    if (blk_raw_io(b, soff, tmp, sec, false) != 0)
      return -1;
    memcpy(p, tmp + o, n);
    off += n;
    p += n;
    len -= n;
  }
  return 0;
}

/* Escribe `len` bytes al medio respetando la alineación física: si el destino
 * exige sectores, se lee-modifica-escribe el sector (o sectores) afectados. */
static int blk_pwrite(mfs_blk *b, uint64_t off, const void *src, uint32_t len) {
  if (b->readonly || off + len > b->size)
    return -1;
  if (b->rmw_sector == 0u)
    return blk_raw_io(b, off, (void *)(uintptr_t)src, len, true);

  const uint8_t *p = (const uint8_t *)src;
  uint32_t sec = b->rmw_sector;
  static uint8_t tmp[MFS_BLK_MAX_IO];
  while (len > 0u) {
    uint64_t soff = off - (off % sec);
    uint32_t o = (uint32_t)(off - soff);
    uint32_t n = sec - o;
    if (n > len)
      n = len;
    if (n > sizeof(tmp))
      return -1;
    if (blk_raw_io(b, soff, tmp, sec, false) != 0)
      return -1;
    memcpy(tmp + o, p, n);
    if (blk_raw_io(b, soff, tmp, sec, true) != 0)
      return -1;
    off += n;
    p += n;
    len -= n;
  }
  return 0;
}

/* ========================== Callbacks del driver ========================= */

static mfs_st cb_read(void *ctx, uint32_t addr, void *dst, uint32_t len) {
  mfs_blk *b = (mfs_blk *)ctx;
  if (!b || !dst || len == 0u || len > MFS_BLK_MAX_IO)
    return MFS_EINVAL;
  mfs_mutex_lock(&b->mtx);
  int r = blk_pread(b, addr, dst, len);
  mfs_mutex_unlock(&b->mtx);
  return (r == 0) ? MFS_OK : MFS_EIO;
}

static mfs_st cb_prog(void *ctx, uint32_t addr, const void *src, uint32_t len) {
  mfs_blk *b = (mfs_blk *)ctx;
  if (!b || !src || len == 0u || len > MFS_BLK_MAX_IO)
    return MFS_EINVAL;
  if (b->readonly)
    return MFS_EROFS;
  mfs_mutex_lock(&b->mtx);
  int r = blk_pwrite(b, addr, src, len);
  mfs_mutex_unlock(&b->mtx);
  return (r == 0) ? MFS_OK : MFS_EIO;
}

static mfs_st cb_erase(void *ctx, uint32_t addr) {
  mfs_blk *b = (mfs_blk *)ctx;
  if (!b)
    return MFS_EINVAL;
  if (b->readonly)
    return MFS_EROFS;
  uint32_t eu = b->geom.erase_unit ? b->geom.erase_unit : 4096u;
  uint32_t base = addr - (addr % eu);
  mfs_mutex_lock(&b->mtx);
  int r = 0;
#if !defined(_WIN32) && defined(MFS_HAVE_MTD)
  if (b->is_mtd) {
    mtd_info_t mi;
    if (ioctl(b->fd, MEMGETINFO, &mi) == 0) {
      erase_info_t ei;
      ei.start = base;
      ei.length = mi.erasesize;
      r = (ioctl(b->fd, MEMERASE, &ei) == 0) ? 0 : -1;
    } else
      r = -1;
  } else
#endif
  {
    static uint8_t ff[4096];
    uint32_t left = eu;
    memset(ff, 0xFFu, sizeof(ff));
    while (left > 0u && r == 0) {
      uint32_t n = (left > sizeof(ff)) ? (uint32_t)sizeof(ff) : left;
      r = blk_pwrite(b, (uint64_t)base + (eu - left), ff, n);
      left -= n;
    }
  }
  mfs_mutex_unlock(&b->mtx);
  return (r == 0) ? MFS_OK : MFS_EIO;
}

/* ============================== Apertura ================================ */

mfs_blk *mfs_blk_open(const char *path, bool readonly, const mfs_blk_opts *opts,
                      char *err, size_t errlen) {
  mfs_blk_opts def;
  if (!opts) {
    mfs_blk_opts_default(&def);
    opts = &def;
  }
  if (!path || path[0] == '\0') {
    if (err && errlen)
      snprintf(err, errlen, "dispositivo no especificado");
    return NULL;
  }
  mfs_blk *b = (mfs_blk *)calloc(1, sizeof(*b));
  if (!b) {
    if (err && errlen)
      snprintf(err, errlen, "sin memoria");
    return NULL;
  }
  if (strlen(path) >= MFS_BLK_PATH_MAX) {
    if (err && errlen)
      snprintf(err, errlen, "ruta demasiado larga");
    free(b);
    return NULL;
  }
  strcpy(b->path, path);
  b->readonly = readonly;
  mfs_mutex_init(&b->mtx);

#if defined(_WIN32)
  b->is_volume = win_is_device_path(path);
  DWORD acc = GENERIC_READ | (readonly ? 0u : GENERIC_WRITE);
  DWORD sh = FILE_SHARE_READ | FILE_SHARE_WRITE;
  WCHAR wpath[MFS_BLK_PATH_MAX];
  if (!win_path_to_wide(path, wpath, (int)MFS_BLK_PATH_MAX)) {
    if (err && errlen)
      snprintf(err, errlen, "ruta no convertible a UTF-16: '%s'", path);
    free(b);
    return NULL;
  }
  b->h = CreateFileW(wpath, acc, sh, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                     NULL);
  if (b->h == INVALID_HANDLE_VALUE) {
    if (err && errlen)
      snprintf(err, errlen, "no se puede abrir '%s' (error %lu)", path,
               (unsigned long)GetLastError());
    free(b);
    return NULL;
  }
  {
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (!GetFileSizeEx(b->h, &sz) || sz.QuadPart <= 0) {
      /* Volumen o disco crudo: GetFileSizeEx devuelve 0 o falla, la longitud
       * real se consulta con IOCTL_DISK_GET_LENGTH_INFO. */
      if (b->is_volume) {
        GET_LENGTH_INFORMATION gli;
        DWORD br = 0;
        if (DeviceIoControl(b->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0u, &gli,
                            (DWORD)sizeof(gli), &br, NULL))
          sz.QuadPart = (LONGLONG)gli.Length.QuadPart;
      }
    }
    if (sz.QuadPart <= 0) {
      if (err && errlen)
        snprintf(err, errlen, "no se puede determinar el tamano de '%s'", path);
      CloseHandle(b->h);
      free(b);
      return NULL;
    }
    b->size = (uint64_t)sz.QuadPart;
  }
  blk_detect_align(b);
#else
  b->is_mtd = (strncmp(path, "/dev/mtd", 8) == 0);
  int fl = readonly ? O_RDONLY : O_RDWR;
  b->fd = open(path, fl);
  if (b->fd < 0) {
    if (err && errlen)
      snprintf(err, errlen, "no se puede abrir '%s': %s", path,
               strerror(errno));
    free(b);
    return NULL;
  }
  struct stat sb;
  if (fstat(b->fd, &sb) != 0) {
    if (err && errlen)
      snprintf(err, errlen, "stat('%s') fallo: %s", path, strerror(errno));
    close(b->fd);
    free(b);
    return NULL;
  }
  if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
    off_t e = lseek(b->fd, 0, SEEK_END);
    b->size = (e > 0) ? (uint64_t)e : (uint64_t)sb.st_size;
  } else {
    b->size = (uint64_t)sb.st_size;
  }
  if (b->size == 0u) {
    if (err && errlen)
      snprintf(err, errlen, "'%s' tiene tamano 0", path);
    close(b->fd);
    free(b);
    return NULL;
  }
  blk_detect_align(b);
#endif

  /* Geometría lógica: la misma en todos los sistemas para que el layout
   * on-flash sea intercambiable (requisito de compatibilidad cruzada). */
  if (opts->size_limit != 0u && (uint64_t)opts->size_limit < b->size)
    b->size = opts->size_limit;
  if (b->size > 0xFFFFFFFFull)
    b->size = 0xFFFFFFFFull; /* el núcleo direcciona con 32 bits */

  memset(&b->geom, 0, sizeof(b->geom));
  b->geom.type = opts->media;
  b->geom.base_addr = 0u;
  b->geom.size = (uint32_t)b->size;
  b->geom.erase_unit =
      opts->erase_unit ? opts->erase_unit : blk_auto_erase_unit(b->size);
  b->geom.program_granularity =
      opts->program_granularity ? opts->program_granularity : 1u;
  b->geom.page_size = opts->page_size ? opts->page_size : 256u;
  b->geom.oob_bytes = opts->oob_bytes;
  b->geom.t_prog_max_us = 300u;
  b->geom.t_erase_max_us = 5000u;
  b->geom.t_read_max_us = 100u;
  b->geom.t_suspend_max_us = 0u;
  b->geom.flags0 = 0u;
  b->geom.flags1 = 0u;
  b->geom.zones_per_block = 1u;
  b->geom.zone_size = 0u;

#if !defined(_WIN32) && defined(MFS_HAVE_MTD)
  if (b->is_mtd) {
    mtd_info_t mi;
    if (ioctl(b->fd, MEMGETINFO, &mi) == 0) {
      b->geom.erase_unit = mi.erasesize;
      b->geom.program_granularity = mi.writesize ? mi.writesize : 1u;
      b->geom.page_size = (uint16_t)(mi.writesize ? mi.writesize : 256u);
      b->geom.oob_bytes = (uint16_t)mi.oobsize;
      b->geom.t_erase_max_us =
          (uint32_t)((uint64_t)mi.erasesize * 1000ull / 1048576ull + 1000u);
      b->size = (uint64_t)mi.size;
      b->geom.size = (uint32_t)b->size;
      b->geom.type = MFS_MEDIA_NOR_SPI;
    }
  }
#endif

  /* Erase unit debe ser potencia de dos y caber en el medio. */
  if (b->geom.erase_unit == 0u ||
      (b->geom.erase_unit & (b->geom.erase_unit - 1u)) != 0u ||
      b->geom.erase_unit > b->geom.size) {
    if (err && errlen)
      snprintf(err, errlen, "erase_unit invalido (%u)", b->geom.erase_unit);
    mfs_blk_close(b);
    return NULL;
  }

  b->drv.read = cb_read;
  b->drv.prog = cb_prog;
  b->drv.erase = cb_erase;
  b->drv.suspend = NULL;
  b->drv.resume = NULL;
  b->drv.t0_read = NULL;
  b->drv.t0_prog = NULL;
  b->drv.rail_ok = NULL;
  b->drv.dma_read = NULL;
  b->drv.dma_crc = NULL;
  b->drv.ctx = b;
  return b;
}

void mfs_blk_close(mfs_blk *b) {
  if (!b)
    return;
#if defined(_WIN32)
  if (b->h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(b->h);
    CloseHandle(b->h);
  }
#else
  if (b->fd >= 0) {
    (void)fsync(b->fd);
    close(b->fd);
  }
#endif
  mfs_mutex_destroy(&b->mtx);
  free(b);
}

const mfs_l2_driver *mfs_blk_driver(mfs_blk *b) { return b ? &b->drv : NULL; }

void mfs_blk_geom(const mfs_blk *b, mfs_media_geom *g) {
  if (b && g)
    *g = b->geom;
}

int mfs_blk_flush(mfs_blk *b) {
  if (!b)
    return -1;
#if defined(_WIN32)
  return FlushFileBuffers(b->h) ? 0 : -1;
#else
  return fsync(b->fd);
#endif
}

int mfs_blk_erase_all(mfs_blk *b) {
  if (!b)
    return -1;
  if (b->readonly)
    return -1;
  uint32_t eu = b->geom.erase_unit;
  uint64_t off = 0u;
  while (off + eu <= b->size) {
    if (cb_erase(b, (uint32_t)off) != MFS_OK)
      return -1;
    off += eu;
  }
  return mfs_blk_flush(b);
}

uint64_t mfs_blk_size(const mfs_blk *b) { return b ? b->size : 0u; }
bool mfs_blk_is_readonly(const mfs_blk *b) { return b ? b->readonly : true; }
const char *mfs_blk_path(const mfs_blk *b) { return b ? b->path : ""; }
