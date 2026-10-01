/* matrixfs_winfsp.c — front-end WinFsp de MatrixFS para Windows 10/11.
 *
 * Expone un volumen MatrixFS como una unidad con letra (p.ej. X:) visible
 * en el Explorador de Archivos de Windows, con operaciones nativas completas
 * (copiar, mover, pegar, eliminar, renombrar, cambiar atributos y tamaño).
 *
 * Al igual que el front-end FUSE de Linux, este fichero es un envoltorio fino
 * sobre el adaptador portable `platform/common/mfs_vfs.c`: permisos POSIX
 * persistidos, metadatos, E/S con desplazamiento explícito y errores
 * tipificados viven en la capa común. Aquí sólo se traduce entre el ABI de
 * WinFsp (NTSTATUS, FSP_FSCTL_FILE_INFO) y el contrato mfs_vfs_*.
 *
 * Compatibilidad: Windows 10 (1809+) y Windows 11 con WinFsp 1.12 o superior.
 * Véase README.md para la compilación (WinFsp SDK + CMake/build.ps1).
 *
 * Uso:
 *   matrixfs_winfsp.exe <dispositivo> [<letra|punto_de_montaje>] [opciones]
 *     <dispositivo>  \\.\X:  |  \\.\PhysicalDriveN  |  ruta\imagen.img
 *     <letra|punto>  X:  |  X:\carpeta  |  C:\ruta\vacia
 *   Opciones:
 *     -o label=NOMBRE     etiqueta de volumen
 *     -o format           formatea el medio antes de montar
 *     -o ro               sólo lectura
 *     -o ram=N            presupuesto RAM declarado (por defecto 262144)
 *     -o erase_unit=N     bloque de borrado (por defecto: derivado del medio)
 *     -d                  traza de WinFsp por consola
 */

#if !defined(_WIN32)
#error "matrixfs_winfsp.c sólo compila en Windows (requiere el SDK de WinFsp)."
#endif

/* DOS REQUISITOS DE COMPILACIÓN (verificados con el SDK de WinFsp 1.12):
 *
 *  1. <winfsp/winfsp.h> debe incluirse ANTES que cualquier otro encabezado de
 *     Windows, y NUNCA con WIN32_LEAN_AND_MEAN definido. WinFsp define
 *     WIN32_NO_STATUS, incluye <winternl.h> y <ntstatus.h> para obtener
 *     NTSTATUS/PNTSTATUS; si <windows.h> ya se ha procesado (o si se recorta
 *     con WIN32_LEAN_AND_MEAN), esas definiciones se pierden y la compilación
 *     falla con «'PNTSTATUS': el nombre de la lista de parámetros no es
 * válido».
 */
#include <winfsp/winfsp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "mfs_vfs.h"

/* ---- NTSTATUS usados (evita depender de <ntstatus.h>) ---- */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
#define STATUS_NOT_A_DIRECTORY ((NTSTATUS)0xC0000103L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY ((NTSTATUS)0xC00000BAL)
#endif
#ifndef STATUS_DISK_FULL
#define STATUS_DISK_FULL ((NTSTATUS)0xC000007FL)
#endif
#ifndef STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_MEDIA_WRITE_PROTECTED ((NTSTATUS)0xC00000A2L)
#endif
#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_IO_DEVICE_ERROR
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185L)
#endif
#ifndef STATUS_FILE_CORRUPT_ERROR
#define STATUS_FILE_CORRUPT_ERROR ((NTSTATUS)0xC0000102L)
#endif
#ifndef STATUS_VOLUME_DISMOUNTED
#define STATUS_VOLUME_DISMOUNTED ((NTSTATUS)0xC000026EL)
#endif
#ifndef STATUS_TOO_MANY_OPENED_FILES
#define STATUS_TOO_MANY_OPENED_FILES ((NTSTATUS)0xC000011FL)
#endif
#ifndef STATUS_FILE_TOO_LARGE
#define STATUS_FILE_TOO_LARGE ((NTSTATUS)0xC0000904L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#endif

/* ---- Atributos Win32 (no expuestos en modo WIN32_LEAN_AND_MEAN) ---- */
#ifndef FILE_ATTRIBUTE_READONLY
#define FILE_ATTRIBUTE_READONLY 0x00000001u
#endif
#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001u
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040u
#endif
#ifndef FILE_ATTRIBUTE_INVALID
#define FILE_ATTRIBUTE_INVALID 0xFFFFFFFFu
#endif

#define MFS_WF_NAME_MAX 64 /* MFS_NAME_MAX del núcleo */

/* ======================= Estado global del montaje ====================== */

static mfs_vfs *g_vol;       /* volumen montado (instancia única del núcleo) */
static volatile LONG g_stop; /* señal de parada (Ctrl+C)                     */
static BOOL g_ro;            /* montaje de sólo lectura                      */

/* Epoch Unix (1970) ↔ FILETIME de Windows (1601, unidades de 100 ns). */
#define MFS_WIN_TIME_BIAS 116444736000000000ull

static UINT64 mfs_to_winftime(uint32_t sec) {
  return (UINT64)sec * 10000000ull + MFS_WIN_TIME_BIAS;
}

static uint32_t mfs_from_winftime(UINT64 t) {
  if (t <= MFS_WIN_TIME_BIAS)
    return 0u;
  return (uint32_t)((t - MFS_WIN_TIME_BIAS) / 10000000ull);
}

/* ========================= Traducción de errores ======================== */

/* mfs_st → NTSTATUS. Windows usa estos códigos para mostrar los mensajes
 * nativos del Explorador ("acceso denegado", "no se encuentra el archivo"...).
 */
static NTSTATUS mfs_nt(mfs_st st) {
  switch (st) {
  case MFS_OK:
    return STATUS_SUCCESS;
  case MFS_ENOENT:
    return STATUS_OBJECT_NAME_NOT_FOUND;
  case MFS_EEXISTS:
    return STATUS_OBJECT_NAME_COLLISION;
  case MFS_EACCES:
  case MFS_ESECURITY_STATE:
  case MFS_EHEALTH_BLOCKED:
    return STATUS_ACCESS_DENIED;
  case MFS_EROFS:
    return STATUS_MEDIA_WRITE_PROTECTED;
  case MFS_ENOSPC:
  case MFS_ESNAPMAX:
    return STATUS_DISK_FULL;
  case MFS_EBUSY:
  case MFS_ESTATE:
    return STATUS_SHARING_VIOLATION;
  case MFS_EINVAL:
    return STATUS_INVALID_PARAMETER;
  case MFS_ENOTSUP:
  case MFS_EHW_UNSUPPORTED:
  case MFS_ENOTVIABLE:
  case MFS_EARCH:
  case MFS_ECIPHER:
    return STATUS_NOT_SUPPORTED;
  case MFS_EOVERFLOW:
    return STATUS_FILE_TOO_LARGE;
  case MFS_ETABLEFULL:
    return STATUS_TOO_MANY_OPENED_FILES;
  case MFS_ENOTMOUNTED:
    return STATUS_VOLUME_DISMOUNTED;
  case MFS_ECORRUPT:
  case MFS_EBADMSG:
    return STATUS_FILE_CORRUPT_ERROR;
  case MFS_EIO:
  case MFS_ETIMEDOUT_BUDGET:
  case MFS_EPUF:
  case MFS_EENERGY:
  default:
    return STATUS_IO_DEVICE_ERROR;
  }
}

/* Contexto por fichero abierto. WinFsp entrega un PVOID opaco en Create/Open y
 * lo devuelve en el resto de operaciones; se almacena la ruta (necesaria en
 * ReadDirectory/GetFileInfo, que no la reciben) y el handle del adaptador VFS.
 */
typedef struct {
  char *path;    /* ruta en la forma del núcleo ("/a/b") */
  mfs_vfs_fh fh; /* 0 para directorios                   */
  BOOL dir;
} mfs_wf_ctx;

/* Ruta UTF-16 de WinFsp ("\a\b") → UTF-8 con separadores '/' ("/a/b"). */
static char *wf_path_dup(PCWSTR wname) {
  if (!wname)
    wname = L"\\";
  int n = WideCharToMultiByte(CP_UTF8, 0, wname, -1, NULL, 0, NULL, NULL);
  if (n <= 0)
    return NULL;
  char *out = (char *)malloc((size_t)n);
  if (!out)
    return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, wname, -1, out, n, NULL, NULL) <= 0) {
    free(out);
    return NULL;
  }
  for (int i = 0; out[i]; i++)
    if (out[i] == '\\')
      out[i] = '/';
  return out;
}

static void wf_fill_file_info(const mfs_vfs_attr *a, FSP_FSCTL_FILE_INFO *fi) {
  memset(fi, 0, sizeof(*fi));
  fi->FileAttributes = ((a->mode & MFS_S_IFMT) == MFS_S_IFDIR)
                           ? FILE_ATTRIBUTE_DIRECTORY
                           : FILE_ATTRIBUTE_NORMAL;
  if (g_ro)
    fi->FileAttributes |= FILE_ATTRIBUTE_READONLY;
  fi->FileSize = a->size;
  fi->AllocationSize = (a->size + 4095u) & ~4095ull;
  fi->CreationTime = mfs_to_winftime(a->mtime);
  fi->LastAccessTime = mfs_to_winftime(a->mtime);
  fi->LastWriteTime = mfs_to_winftime(a->mtime);
  fi->ChangeTime = mfs_to_winftime(a->mtime);
  fi->IndexNumber = a->ino;
  fi->HardLinks = a->nlink ? a->nlink : 1;
}

static NTSTATUS wf_attr(const mfs_wf_ctx *c, mfs_vfs_attr *out) {
  if (!c || !c->path)
    return STATUS_INVALID_PARAMETER;
  return mfs_nt((mfs_st)mfs_vfs_getattr(g_vol, c->path, out));
}

/* ============================ Operaciones =============================== */

static NTSTATUS wf_get_volume_info(FSP_FILE_SYSTEM *fs,
                                   FSP_FSCTL_VOLUME_INFO *vi) {
  (void)fs;
  uint64_t total = 0, freeb = 0, used = 0;
  memset(vi, 0, sizeof(*vi));
  if (mfs_vfs_statfs(g_vol, &total, &freeb, &used) == MFS_OK) {
    vi->TotalSize = total;
    vi->FreeSize = freeb;
  }
  char lab[32];
  lab[0] = '\0';
  if (mfs_vfs_label(g_vol, lab, sizeof(lab)) == MFS_OK && lab[0]) {
    WCHAR wlab[32];
    int n = MultiByteToWideChar(CP_UTF8, 0, lab, -1, wlab, 32);
    if (n > 0) {
      memcpy(vi->VolumeLabel, wlab, (size_t)n * sizeof(WCHAR));
      vi->VolumeLabelLength = (UINT16)((n - 1) * sizeof(WCHAR));
    }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS wf_get_security_by_name(FSP_FILE_SYSTEM *fs, PWSTR file_name,
                                        PUINT32 pattr, PSECURITY_DESCRIPTOR sd,
                                        SIZE_T *psd_size) {
  (void)fs;
  (void)sd;
  char *p = wf_path_dup(file_name);
  if (!p)
    return STATUS_INVALID_PARAMETER;
  mfs_vfs_attr a;
  mfs_st st = (mfs_st)mfs_vfs_getattr(g_vol, p, &a);
  free(p);
  if (st != MFS_OK)
    return mfs_nt(st);
  if (pattr)
    *pattr = ((a.mode & MFS_S_IFMT) == MFS_S_IFDIR) ? FILE_ATTRIBUTE_DIRECTORY
                                                    : FILE_ATTRIBUTE_NORMAL;
  /* Sin descriptor de seguridad propio: la autorización efectiva la realizan
   * los permisos POSIX persistidos del volumen en el adaptador VFS. */
  if (psd_size)
    *psd_size = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS wf_create(FSP_FILE_SYSTEM *fs, PWSTR file_name,
                          UINT32 create_options, UINT32 granted_access,
                          UINT32 file_attributes, PSECURITY_DESCRIPTOR sd,
                          UINT64 allocation_size, PVOID *pctx,
                          FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)sd;
  (void)allocation_size;
  (void)granted_access;
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;

  char *p = wf_path_dup(file_name);
  if (!p)
    return STATUS_INVALID_PARAMETER;
  mfs_wf_ctx *c = (mfs_wf_ctx *)calloc(1, sizeof(*c));
  if (!c) {
    free(p);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  c->path = p;

  if (create_options & FILE_DIRECTORY_FILE) {
    mfs_st st = (mfs_st)mfs_vfs_mkdir(g_vol, p, 0755u, 0u, 0u);
    if (st == MFS_EEXISTS) {
      /* El Explorador abre directorios existentes con FILE_DIRECTORY_FILE. */
      mfs_vfs_attr a;
      st = (mfs_st)mfs_vfs_getattr(g_vol, p, &a);
      if (st == MFS_OK && (a.mode & MFS_S_IFMT) != MFS_S_IFDIR)
        st = MFS_EEXISTS;
    }
    if (st != MFS_OK) {
      free(c->path);
      free(c);
      return mfs_nt(st);
    }
    c->dir = TRUE;
  } else {
    uint16_t mode = (file_attributes & FILE_ATTRIBUTE_READONLY) ? 0444u : 0644u;
    mfs_vfs_fh fh = 0;
    mfs_st st = (mfs_st)mfs_vfs_create(g_vol, p, mode, 0u, 0u, &fh);
    if (st != MFS_OK) {
      free(c->path);
      free(c);
      return mfs_nt(st);
    }
    c->fh = fh;
  }

  *pctx = c;
  mfs_vfs_attr a;
  if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
    wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_open(FSP_FILE_SYSTEM *fs, PWSTR file_name,
                        UINT32 create_options, UINT32 granted_access,
                        PVOID *pctx, FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)granted_access;
  char *p = wf_path_dup(file_name);
  if (!p)
    return STATUS_INVALID_PARAMETER;
  mfs_vfs_attr a;
  mfs_st st = (mfs_st)mfs_vfs_getattr(g_vol, p, &a);
  if (st != MFS_OK) {
    free(p);
    return mfs_nt(st);
  }
  BOOL isdir = ((a.mode & MFS_S_IFMT) == MFS_S_IFDIR) ? TRUE : FALSE;
  if ((create_options & FILE_DIRECTORY_FILE) && !isdir) {
    free(p);
    return STATUS_NOT_A_DIRECTORY;
  }
  if ((create_options & FILE_NON_DIRECTORY_FILE) && isdir) {
    free(p);
    return STATUS_FILE_IS_A_DIRECTORY;
  }

  mfs_wf_ctx *c = (mfs_wf_ctx *)calloc(1, sizeof(*c));
  if (!c) {
    free(p);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  c->path = p;
  c->dir = isdir;
  if (!isdir) {
    mfs_vfs_fh fh = 0;
    st = (mfs_st)mfs_vfs_open(g_vol, p, g_ro ? MFS_O_RDONLY : MFS_O_RDWR, &fh);
    if (st != MFS_OK && !g_ro)
      st = (mfs_st)mfs_vfs_open(g_vol, p, MFS_O_RDONLY, &fh);
    if (st != MFS_OK) {
      free(c->path);
      free(c);
      return mfs_nt(st);
    }
    c->fh = fh;
  }
  *pctx = c;
  wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_overwrite(FSP_FILE_SYSTEM *fs, PVOID ctx,
                             UINT32 file_attributes, BOOLEAN replace_attrs,
                             UINT64 allocation_size, FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)file_attributes;
  (void)replace_attrs;
  (void)allocation_size;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c || c->dir)
    return STATUS_INVALID_PARAMETER;
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;
  mfs_vfs_attr a;
  if (wf_attr(c, &a) != STATUS_SUCCESS)
    return STATUS_INVALID_PARAMETER;
  mfs_st st = (mfs_st)mfs_vfs_truncate(g_vol, c->fh, 0u);
  if (st != MFS_OK)
    return mfs_nt(st);
  if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
    wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

/* Cleanup completa la operación de borrado: el gestor de WinFsp abre el nodo,
 * comprueba si se puede borrar y, si procede, envía Cleanup con el flag
 * FspCleanupDelete y el nombre del nodo (parámetro que sólo se rellena en ese
 * caso). Es el punto donde debe ejecutarse la eliminación real. */
static void wf_cleanup(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR file_name,
                       ULONG flags) {
  (void)fs;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return;

  if (c->fh)
    (void)mfs_vfs_flush(g_vol, c->fh);

  if (flags & FspCleanupDelete) {
    char *p = wf_path_dup(file_name ? file_name : L"\\");
    if (p) {
      mfs_st st = c->dir ? (mfs_st)mfs_vfs_rmdir(g_vol, p)
                         : (mfs_st)mfs_vfs_unlink(g_vol, p);
      if (st != MFS_OK)
        fprintf(stderr, "matrixfs: no se pudo eliminar '%s' (%s)\n", p,
                mfs_ststr(st));
      free(p);
    }
  }
}

static void wf_close(FSP_FILE_SYSTEM *fs, PVOID ctx) {
  (void)fs;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return;
  if (c->fh)
    (void)mfs_vfs_release(g_vol, c->fh);
  free(c->path);
  free(c);
}

static NTSTATUS wf_read(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buffer,
                        UINT64 offset, ULONG length,
                        PULONG pbytes_transferred) {
  (void)fs;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c || c->dir)
    return STATUS_INVALID_PARAMETER;
  size_t rd = 0u;
  mfs_st st =
      (mfs_st)mfs_vfs_read(g_vol, c->fh, buffer, offset, (size_t)length, &rd);
  if (st != MFS_OK)
    return mfs_nt(st);
  *pbytes_transferred = (ULONG)rd;
  return STATUS_SUCCESS;
}

static NTSTATUS wf_write(FSP_FILE_SYSTEM *fs, PVOID ctx, PVOID buffer,
                         UINT64 offset, ULONG length, BOOLEAN write_to_eof,
                         BOOLEAN constrained_io, PULONG pbytes_transferred,
                         FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)constrained_io;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c || c->dir)
    return STATUS_INVALID_PARAMETER;
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;

  uint64_t off = offset;
  if (write_to_eof) {
    mfs_vfs_attr a;
    if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
      off = a.size;
  }
  size_t wr = 0u;
  mfs_st st =
      (mfs_st)mfs_vfs_write(g_vol, c->fh, buffer, off, (size_t)length, &wr);
  if (st != MFS_OK)
    return mfs_nt(st);
  *pbytes_transferred = (ULONG)wr;

  mfs_vfs_attr a;
  if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
    wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_flush(FSP_FILE_SYSTEM *fs, PVOID ctx,
                         FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return STATUS_SUCCESS;
  if (c->fh)
    (void)mfs_vfs_flush(g_vol, c->fh);
  if (fi) {
    mfs_vfs_attr a;
    if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
      wf_fill_file_info(&a, fi);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS wf_get_file_info(FSP_FILE_SYSTEM *fs, PVOID ctx,
                                 FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return STATUS_INVALID_PARAMETER;
  mfs_vfs_attr a;
  mfs_st st = (mfs_st)mfs_vfs_getattr(g_vol, c->path, &a);
  if (st != MFS_OK)
    return mfs_nt(st);
  wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_set_basic_info(FSP_FILE_SYSTEM *fs, PVOID ctx,
                                  UINT32 file_attributes, UINT64 creation_time,
                                  UINT64 last_access_time,
                                  UINT64 last_write_time, UINT64 change_time,
                                  FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)creation_time;
  (void)last_access_time;
  (void)change_time;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return STATUS_INVALID_PARAMETER;
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;

  mfs_attr a;
  uint32_t mask = 0u;
  memset(&a, 0, sizeof(a));
  if (file_attributes != FILE_ATTRIBUTE_INVALID) {
    mfs_vfs_attr cur;
    if (mfs_vfs_getattr(g_vol, c->path, &cur) == MFS_OK) {
      uint32_t perm = cur.mode & 0777u;
      /* El atributo "sólo lectura" de Windows se refleja en los bits POSIX de
       * escritura, que quedan persistidos en el volumen. */
      if (file_attributes & FILE_ATTRIBUTE_READONLY)
        perm &= ~0222u;
      else
        perm |= 0200u;
      a.mode = (uint16_t)perm;
      mask |= MFS_ATTR_MODE;
    }
  }
  if (last_write_time != 0u) {
    a.mtime = mfs_from_winftime(last_write_time);
    mask |= MFS_ATTR_MTIME;
  }
  if (mask) {
    mfs_st st = (mfs_st)mfs_vfs_setattr(g_vol, c->path, mask, &a);
    if (st != MFS_OK)
      return mfs_nt(st);
  }
  mfs_vfs_attr cur;
  if (mfs_vfs_getattr(g_vol, c->path, &cur) == MFS_OK)
    wf_fill_file_info(&cur, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_set_file_size(FSP_FILE_SYSTEM *fs, PVOID ctx,
                                 UINT64 new_size, BOOLEAN set_alloc_size,
                                 FSP_FSCTL_FILE_INFO *fi) {
  (void)fs;
  (void)set_alloc_size;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c || c->dir)
    return STATUS_INVALID_PARAMETER;
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;
  mfs_st st = (mfs_st)mfs_vfs_truncate(g_vol, c->fh, new_size);
  if (st != MFS_OK)
    return mfs_nt(st);
  mfs_vfs_attr a;
  if (mfs_vfs_getattr(g_vol, c->path, &a) == MFS_OK)
    wf_fill_file_info(&a, fi);
  return STATUS_SUCCESS;
}

static NTSTATUS wf_rename(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR file_name,
                          PWSTR new_file_name, BOOLEAN replace_if_exists) {
  (void)fs;
  (void)ctx;
  (void)replace_if_exists; /* el núcleo reemplaza de forma atómica */
  if (g_ro)
    return STATUS_MEDIA_WRITE_PROTECTED;
  char *from = wf_path_dup(file_name);
  char *to = wf_path_dup(new_file_name);
  if (!from || !to) {
    free(from);
    free(to);
    return STATUS_INVALID_PARAMETER;
  }
  mfs_st st = (mfs_st)mfs_vfs_rename(g_vol, from, to);
  free(from);
  free(to);
  return mfs_nt(st);
}

/* ---- Enumeración de directorios ----
 * WinFsp entrega un búfer opaco y el ayudante FspFileSystemAddDirInfo copia
 * cada entrada; cuando devuelve FALSE el búfer está lleno y la enumeración
 * termina con éxito (el Explorador volverá a pedir el resto). No se incluyen
 * las entradas "." y "..": el gestor de WinFsp las sintetiza. */
typedef struct {
  PVOID buffer;
  ULONG length;
  PULONG pbytes;
  NTSTATUS status; /* primer error real (STATUS_SUCCESS si no hay) */
  union {
    FSP_FSCTL_DIR_INFO di;
    BYTE raw[sizeof(FSP_FSCTL_DIR_INFO) + MFS_WF_NAME_MAX * sizeof(WCHAR)];
  } scratch;
} wf_rd_state;

static NTSTATUS wf_emit_entry(wf_rd_state *s, const char *name,
                              const mfs_vfs_attr *a) {
  WCHAR wname[MFS_WF_NAME_MAX];
  int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MFS_WF_NAME_MAX);
  if (n <= 0)
    return STATUS_INVALID_PARAMETER;

  FSP_FSCTL_DIR_INFO *di = &s->scratch.di;
  memset(&s->scratch, 0, sizeof(s->scratch));
  wf_fill_file_info(a, &di->FileInfo);
  memcpy(di->FileNameBuf, wname, (size_t)n * sizeof(WCHAR));
  /* `Size` = estructura fija + nombre (sin el terminador NUL). */
  di->Size =
      (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + (size_t)(n - 1) * sizeof(WCHAR));

  if (!FspFileSystemAddDirInfo(di, s->buffer, s->length, s->pbytes))
    return STATUS_BUFFER_OVERFLOW; /* búfer lleno: fin normal del lote */
  return STATUS_SUCCESS;
}

/* Adaptador al iterador del VFS portable. Devuelve != 0 para detener. */
static int mfs_wf_emit(void *userctx, const char *name, const mfs_vfs_attr *a) {
  wf_rd_state *s = (wf_rd_state *)userctx;
  NTSTATUS r = wf_emit_entry(s, name, a);
  if (r == STATUS_SUCCESS)
    return 0;
  if (r == STATUS_BUFFER_OVERFLOW)
    return 1; /* lote completo */
  s->status = r;
  return 1;
}

static NTSTATUS wf_read_directory(FSP_FILE_SYSTEM *fs, PVOID ctx, PWSTR pattern,
                                  PWSTR marker, PVOID buffer, ULONG length,
                                  PULONG pbytes_transferred) {
  (void)fs;
  (void)pattern; /* se sirve el listado completo; el filtrado lo hace WinFsp */
  (void)marker;
  mfs_wf_ctx *c = (mfs_wf_ctx *)ctx;
  if (!c)
    return STATUS_INVALID_PARAMETER;

  wf_rd_state s;
  memset(&s, 0, sizeof(s));
  s.buffer = buffer;
  s.length = length;
  s.pbytes = pbytes_transferred;
  s.status = STATUS_SUCCESS;
  *pbytes_transferred = 0;

  mfs_st st = (mfs_st)mfs_vfs_readdir(g_vol, c->path, mfs_wf_emit, &s);
  if (s.status != STATUS_SUCCESS)
    return s.status;
  if (st == MFS_EBUSY)
    return STATUS_SUCCESS; /* búfer lleno: listado parcial válido */
  return mfs_nt(st);
}

/* Tabla de operaciones. Se usan inicializadores designados para que el
 * compilador valide cada firma y para no depender del orden de los campos, que
 * WinFsp amplía entre versiones (la estructura real de WinFsp 1.12 tiene 32
 * entradas: SetVolumeLabel, CanDelete, reparse points, streams, EA, …). Las no
 * soportadas quedan a NULL y WinFsp devuelve el error tipificado adecuado. */
static FSP_FILE_SYSTEM_INTERFACE g_wf_interface = {
    .GetVolumeInfo = wf_get_volume_info,
    .GetSecurityByName = wf_get_security_by_name,
    .Create = wf_create,
    .Open = wf_open,
    .Overwrite = wf_overwrite,
    .Cleanup = wf_cleanup,
    .Close = wf_close,
    .Read = wf_read,
    .Write = wf_write,
    .Flush = wf_flush,
    .GetFileInfo = wf_get_file_info,
    .SetBasicInfo = wf_set_basic_info,
    .SetFileSize = wf_set_file_size,
    .Rename = wf_rename,
    .ReadDirectory = wf_read_directory,
};

/* =============================== main =================================== */

static BOOL WINAPI ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
      type == CTRL_CLOSE_EVENT) {
    InterlockedExchange(&g_stop, 1);
    return TRUE;
  }
  return FALSE;
}

static void win_usage(void) {
  wprintf(
      L"uso: matrixfs_winfsp <dispositivo> [<letra|punto>] [opciones]\n"
      L"  <dispositivo>  \\\\.\\X:  |  \\\\.\\PhysicalDriveN  |  imagen.img\n"
      L"  <letra|punto>  X:  |  X:\\carpeta  |  C:\\ruta\\vacia\n"
      L"  -o label=NOMBRE   etiqueta de volumen\n"
      L"  -o format         formatea antes de montar\n"
      L"  -o ro             sólo lectura\n"
      L"  -o ram=N          presupuesto RAM declarado\n"
      L"  -o erase_unit=N   bloque de borrado\n"
      L"  -d                traza de depuración de WinFsp\n");
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    win_usage();
    return 2;
  }

  const wchar_t *wdevice = argv[1];
  const wchar_t *wmount = (argc >= 3 && argv[2][0] != L'-') ? argv[2] : NULL;

  static char label_utf8[32];
  mfs_mount_opts mo;
  mfs_mount_opts_default(&mo);
  BOOL debug = FALSE;
  BOOL do_format = FALSE;

  for (int i = 2; i < argc; i++) {
    if (wcscmp(argv[i], L"-d") == 0) {
      debug = TRUE;
    } else if (wcscmp(argv[i], L"-o") == 0 && i + 1 < argc) {
      wchar_t *o = argv[++i];
      if (wcscmp(o, L"format") == 0)
        do_format = TRUE;
      else if (wcscmp(o, L"ro") == 0)
        mo.readonly = true;
      else if (wcsncmp(o, L"label=", 6) == 0) {
        label_utf8[0] = '\0';
        WideCharToMultiByte(CP_UTF8, 0, o + 6, -1, label_utf8,
                            sizeof(label_utf8), NULL, NULL);
        mo.label = label_utf8;
      } else if (wcsncmp(o, L"ram=", 4) == 0)
        mo.ram_total = (uint32_t)wcstoul(o + 4, NULL, 0);
      else if (wcsncmp(o, L"erase_unit=", 11) == 0)
        mo.blk.erase_unit = (uint32_t)wcstoul(o + 11, NULL, 0);
    }
  }
  g_ro = mo.readonly ? TRUE : FALSE;

  /* El driver de bloque acepta la ruta en UTF-8 (o ANSI como reserva); se
   * convierte aquí para soportar imágenes en directorios con nombres Unicode.
   */
  char device[MAX_PATH];
  if (WideCharToMultiByte(CP_UTF8, 0, wdevice, -1, device, sizeof(device), NULL,
                          NULL) <= 0 &&
      WideCharToMultiByte(CP_ACP, 0, wdevice, -1, device, sizeof(device), NULL,
                          NULL) <= 0) {
    fwprintf(stderr, L"matrixfs: ruta de dispositivo no válida\n");
    return 2;
  }

  char err[256];
  err[0] = '\0';

  if (do_format) {
    mfs_st st = (mfs_st)mfs_vfs_format(device, &mo, err, sizeof(err));
    if (st != MFS_OK) {
      fwprintf(stderr, L"matrixfs: formateo de '%s' falló: %hs\n", wdevice,
               err[0] ? err : mfs_ststr(st));
      return 1;
    }
  }

  {
    char lab[32];
    uint8_t mode = 0xFFu;
    mfs_st st = (mfs_st)mfs_vfs_probe(device, &mo.blk, lab, sizeof(lab), &mode);
    if (st != MFS_OK) {
      fwprintf(stderr,
               L"matrixfs: '%s' no contiene un volumen MatrixFS válido (%hs).\n"
               L"          Use 'matrixfs-ctl format %hs' para crearlo.\n",
               wdevice, mfs_ststr(st), device);
      return 1;
    }
  }

  mfs_st st = (mfs_st)mfs_vfs_mount(device, &mo, &g_vol, err, sizeof(err));
  if (st != MFS_OK) {
    fwprintf(stderr, L"matrixfs: montaje de '%s' falló: %hs\n", wdevice,
             err[0] ? err : mfs_ststr(st));
    return 1;
  }

  FSP_FSCTL_VOLUME_PARAMS vp;
  memset(&vp, 0, sizeof(vp));
  vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
  vp.SectorSize = 512;
  vp.SectorsPerAllocationUnit = 8; /* unidades de asignación de 4 KiB */
  vp.MaxComponentLength =
      MFS_WF_NAME_MAX - 1;    /* en bytes (límite del núcleo) */
  vp.FileInfoTimeout = 1000;  /* caché de metadatos de 1 s (coherencia) */
  vp.CaseSensitiveSearch = 0; /* nombres insensibles a mayúsculas (Windows) */
  vp.CasePreservedNames = 1;
  vp.UnicodeOnDisk = 0; /* el núcleo almacena nombres como bytes UTF-8 */
  vp.PersistentAcls = 0;
  vp.PostCleanupWhenModifiedOnly = 1;
  vp.PassQueryDirectoryPattern = 0;
  vp.FlushAndPurgeOnCleanup = 0;
  vp.VolumeCreationTime = mfs_to_winftime(0u);
  vp.VolumeSerialNumber = 0x4D465355u; /* "MFSU" */
  wcscpy_s(vp.FileSystemName, 16, L"MatrixFS");
  wcscpy_s(vp.Prefix, sizeof(vp.Prefix) / sizeof(WCHAR), L"");

  FSP_FILE_SYSTEM *fs = NULL;
  NTSTATUS ns =
      FspFileSystemCreate(L"\\matrixfs\\volume", &vp, &g_wf_interface, &fs);
  if (!NT_SUCCESS(ns)) {
    fwprintf(stderr, L"matrixfs: FspFileSystemCreate falló (0x%08X)\n",
             (unsigned)ns);
    mfs_vfs_unmount(g_vol);
    return 1;
  }

  FspFileSystemSetOperationGuardStrategy(
      fs, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_FINE);
  if (debug)
    FspFileSystemSetDebugLog(fs, (UINT32)-1);

  if (wmount) {
    ns = FspFileSystemSetMountPoint(fs, (PWSTR)wmount);
    if (!NT_SUCCESS(ns)) {
      fwprintf(
          stderr,
          L"matrixfs: no se pudo asignar el punto de montaje '%s' (0x%08X)\n",
          wmount, (unsigned)ns);
      FspFileSystemDelete(fs);
      mfs_vfs_unmount(g_vol);
      return 1;
    }
  }

  SetConsoleCtrlHandler(ctrl_handler, TRUE);
  ns = FspFileSystemStartDispatcher(fs, 0);
  if (!NT_SUCCESS(ns)) {
    fwprintf(stderr, L"matrixfs: FspFileSystemStartDispatcher falló (0x%08X)\n",
             (unsigned)ns);
    FspFileSystemDelete(fs);
    mfs_vfs_unmount(g_vol);
    return 1;
  }

  wprintf(L"matrixfs: volumen '%s' montado. Pulse Ctrl+C para desmontar.\n",
          wdevice);
  while (!InterlockedCompareExchange(&g_stop, 0, 0))
    Sleep(200);

  FspFileSystemStopDispatcher(fs);
  FspFileSystemDelete(fs);
  (void)mfs_vfs_sync(g_vol);
  mfs_vfs_unmount(g_vol);
  wprintf(L"matrixfs: volumen desmontado.\n");
  return 0;
}
