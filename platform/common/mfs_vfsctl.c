/* mfs_vfsctl.c — CLI de validación de MatrixFS sin kernel.
 *
 * Ejercita la misma ruta de código que los front-ends FUSE (Linux) y WinFsp
 * (Windows) — `platform/common/mfs_vfs.c` — sobre una imagen de fichero o un
 * dispositivo de bloques, sin necesidad de privilegios ni de montar nada.
 * Es la herramienta de diagnóstico de referencia para la fase de validación.
 *
 * Uso:
 *   mfsctl <comando> <dispositivo> [argumentos] [opciones]
 *
 * Comandos:
 *   format  <dev> [--label L]        formatea (0xFF + superblock + raíz)
 *   probe   <dev>                    detecta un volumen MatrixFS
 *   label   <dev>                    muestra la etiqueta de volumen
 *   statfs  <dev>                    capacidad total/libre/usada
 *   verify  <dev> [--full]           verificación de integridad
 *   ls      <dev> <ruta>             lista un directorio
 *   stat    <dev> <ruta>             muestra metadatos (modo/uid/gid/tamaño)
 *   cat     <dev> <ruta>             vuelca el contenido de un fichero
 *   write   <dev> <ruta> <texto...>  crea/sobrescribe con texto
 *   mkdir   <dev> <ruta>             crea un directorio
 *   rm      <dev> <ruta>             borra fichero o directorio
 *   chmod   <dev> <ruta> <octal>     cambia permisos (p.ej. 0640)
 *
 * Opciones globales:
 *   --erase-unit N   bloque de borrado en bytes (por defecto: derivado del
 * medio)
 *   --size-limit N   limita el medio a N bytes (máx. 4 GiB)
 *   --ram N          presupuesto RAM declarado (por defecto 262144)
 *   --ro             monta en sólo lectura
 *   --key HEX        clave de 32 B en hexadecimal (64 dígitos)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "mfs_plat.h"
#include "mfs_vfs.h"

typedef struct {
  mfs_mount_opts o;
  uint8_t key[32];
  bool key_set;
  bool readonly;
} cli_opts;

static void usage(void) {
  printf("mfsctl — CLI de validación MatrixFS (capa VFS portable)\n"
         "  mfsctl format  <dev> [--label L]\n"
         "  mfsctl probe   <dev> [--kv]   (--kv: salida clave=valor, udev)\n"
         "  mfsctl label   <dev>\n"
         "  mfsctl statfs  <dev>\n"
         "  mfsctl verify  <dev> [--full]\n"
         "  mfsctl ls      <dev> <ruta>\n"
         "  mfsctl stat    <dev> <ruta>\n"
         "  mfsctl cat     <dev> <ruta>\n"
         "  mfsctl write   <dev> <ruta> <texto...>\n"
         "  mfsctl mkdir   <dev> <ruta>\n"
         "  mfsctl rm      <dev> <ruta>\n"
         "  mfsctl chmod   <dev> <ruta> <octal>\n"
         "Opciones: --erase-unit N --size-limit N --ram N --ro --key HEX\n");
}

static int hex_to_key(const char *hex, uint8_t out[32]) {
  if (strlen(hex) != 64u)
    return -1;
  for (int i = 0; i < 32; i++) {
    unsigned v = 0u;
    if (sscanf(hex + i * 2, "%2x", &v) != 1)
      return -1;
    out[i] = (uint8_t)v;
  }
  return 0;
}

/* Extrae las opciones de la cola de argumentos; devuelve el nº de no-opciones
 * consumidas para que los comandos recojan sus propios argumentos posicionales.
 */
static void parse_opts(int argc, char **argv, int from, cli_opts *c) {
  mfs_mount_opts_default(&c->o);
  for (int i = from; i < argc; i++) {
    if (strcmp(argv[i], "--erase-unit") == 0 && i + 1 < argc)
      c->o.blk.erase_unit = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--size-limit") == 0 && i + 1 < argc)
      c->o.blk.size_limit = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc)
      c->o.ram_total = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
      c->o.label = argv[++i];
    else if (strcmp(argv[i], "--ro") == 0) {
      c->readonly = true;
      c->o.readonly = true;
    } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
      if (hex_to_key(argv[++i], c->key) != 0) {
        fprintf(stderr, "clave inválida: se esperan 64 dígitos hex\n");
        exit(2);
      }
      c->key_set = true;
    }
  }
  if (c->key_set)
    c->o.key = c->key;
}

/* Abre (monta) el volumen mostrando el error tipificado si falla. */
static mfs_vfs *open_vol(const char *dev, cli_opts *c) {
  char err[160];
  err[0] = '\0';
  mfs_vfs *v = NULL;
  int r = mfs_vfs_mount(dev, &c->o, &v, err, sizeof(err));
  if (r != MFS_OK) {
    fprintf(stderr, "error: %s (%s)\n", err[0] ? err : mfs_ststr((mfs_st)r),
            mfs_ststr((mfs_st)r));
    return NULL;
  }
  return v;
}

static int cmd_format(const char *dev, cli_opts *c) {
  char err[160];
  err[0] = '\0';
  int r = mfs_vfs_format(dev, &c->o, err, sizeof(err));
  if (r != MFS_OK) {
    fprintf(stderr, "format falló: %s\n", err[0] ? err : mfs_ststr((mfs_st)r));
    return 1;
  }
  printf("formateado: %s (etiqueta '%s')\n", dev,
         c->o.label ? c->o.label : "MatrixFS");
  return 0;
}

static int cmd_probe(const char *dev, cli_opts *c, int argc, char **argv) {
  bool kv = false; /* formato clave=valor para udev (IMPORT{program}) */
  for (int i = 2; i < argc; i++)
    if (strcmp(argv[i], "--kv") == 0)
      kv = true;

  char lab[32];
  uint8_t mode = 0xFFu;
  int r = mfs_vfs_probe(dev, &c->o.blk, lab, sizeof(lab), &mode);
  if (r != MFS_OK) {
    if (kv)
      printf("MATRIXFS_VOLUME=0\n");
    else
      printf("%s: no es un volumen MatrixFS (%s)\n", dev, mfs_ststr((mfs_st)r));
    return 1;
  }
  if (kv)
    printf("MATRIXFS_VOLUME=1\nMATRIXFS_LABEL=%s\nMATRIXFS_MODE=%u\n", lab,
           (unsigned)mode);
  else
    printf("%s: volumen MatrixFS válido, etiqueta='%s', modo=%u\n", dev, lab,
           (unsigned)mode);
  return 0;
}

static int iter_print(void *ctx, const char *name, const mfs_vfs_attr *st) {
  (void)ctx;
  printf("  %c %s  %04o  uid=%u gid=%u  %llu B\n",
         (st->mode & MFS_S_IFMT) == MFS_S_IFDIR ? 'd' : '-', name,
         (unsigned)(st->mode & 0777u), st->uid, st->gid,
         (unsigned long long)st->size);
  return 0;
}

static int cmd_ls(mfs_vfs *v, const char *path) {
  int r = mfs_vfs_readdir(v, path, iter_print, NULL);
  if (r != MFS_OK) {
    fprintf(stderr, "ls: %s\n", mfs_ststr((mfs_st)r));
    return 1;
  }
  return 0;
}

static int cmd_stat(mfs_vfs *v, const char *path) {
  mfs_vfs_attr a;
  int r = mfs_vfs_getattr(v, path, &a);
  if (r != MFS_OK) {
    fprintf(stderr, "stat: %s\n", mfs_ststr((mfs_st)r));
    return 1;
  }
  printf("%s: modo=%04o tipo=%s uid=%u gid=%u mtime=%u tamaño=%llu ino=%u\n",
         path, (unsigned)(a.mode & 0777u),
         (a.mode & MFS_S_IFMT) == MFS_S_IFDIR ? "dir" : "file", a.uid, a.gid,
         a.mtime, (unsigned long long)a.size, a.ino);
  return 0;
}

static int cmd_cat(mfs_vfs *v, const char *path) {
  mfs_vfs_fh fh = 0;
  int r = mfs_vfs_open(v, path, MFS_O_RDONLY, &fh);
  if (r != MFS_OK) {
    fprintf(stderr, "cat: %s\n", mfs_ststr((mfs_st)r));
    return 1;
  }
  char buf[1024];
  uint64_t off = 0u;
  for (;;) {
    size_t rd = 0u;
    r = mfs_vfs_read(v, fh, buf, off, sizeof(buf), &rd);
    if (r != MFS_OK) {
      fprintf(stderr, "cat: %s\n", mfs_ststr((mfs_st)r));
      (void)mfs_vfs_release(v, fh);
      return 1;
    }
    if (rd == 0u)
      break;
    fwrite(buf, 1, rd, stdout);
    off += rd;
  }
  (void)mfs_vfs_release(v, fh);
  return 0;
}

static int cmd_write(mfs_vfs *v, const char *path, int argc, char **argv,
                     int from) {
  mfs_vfs_fh fh = 0;
  int r = mfs_vfs_open(v, path, MFS_O_RDWR | MFS_O_CREAT | MFS_O_TRUNC, &fh);
  if (r != MFS_OK) {
    fprintf(stderr, "write: %s\n", mfs_ststr((mfs_st)r));
    return 1;
  }
  uint64_t off = 0u;
  for (int i = from; i < argc; i++) {
    size_t n = strlen(argv[i]);
    if (n == 0u)
      continue;
    size_t wr = 0u;
    r = mfs_vfs_write(v, fh, argv[i], off, n, &wr);
    if (r != MFS_OK || wr != n) {
      fprintf(stderr, "write: %s\n", mfs_ststr((mfs_st)r));
      (void)mfs_vfs_release(v, fh);
      return 1;
    }
    off += n;
    char nl = '\n';
    size_t w2 = 0u;
    if (i + 1 < argc)
      (void)mfs_vfs_write(v, fh, &nl, off, 1u, &w2), off += w2;
  }
  (void)mfs_vfs_flush(v, fh);
  (void)mfs_vfs_release(v, fh);
  printf("escritos %llu bytes en %s\n", (unsigned long long)off, path);
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
  /* Sin esto la CRT traduce '\n' → "\r\n" al volcar a consola/tubería y `cat`
   * corrompería el contenido binario (el fichero extraído no coincidiría). */
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc < 3) {
    usage();
    return 2;
  }
  const char *cmd = argv[1];
  const char *dev = argv[2];

  cli_opts c;
  memset(&c, 0, sizeof(c));
  parse_opts(argc, argv, 2, &c);

  if (strcmp(cmd, "format") == 0)
    return cmd_format(dev, &c);
  if (strcmp(cmd, "probe") == 0)
    return cmd_probe(dev, &c, argc, argv);

  mfs_vfs *v = open_vol(dev, &c);
  if (!v)
    return 1;

  int rc = 0;
  if (strcmp(cmd, "label") == 0) {
    char lab[32];
    if (mfs_vfs_label(v, lab, sizeof(lab)) == MFS_OK)
      printf("%s: etiqueta='%s'\n", dev, lab);
    else
      rc = 1;
  } else if (strcmp(cmd, "statfs") == 0) {
    uint64_t total = 0, freeb = 0, used = 0;
    if (mfs_vfs_statfs(v, &total, &freeb, &used) == MFS_OK)
      printf("%s: total=%llu libre=%llu usado=%llu bytes\n", dev,
             (unsigned long long)total, (unsigned long long)freeb,
             (unsigned long long)used);
    else
      rc = 1;
  } else if (strcmp(cmd, "verify") == 0) {
    mfs_verify_level lvl = MFS_VERIFY_QUICK;
    for (int i = 3; i < argc; i++)
      if (strcmp(argv[i], "--full") == 0)
        lvl = MFS_VERIFY_FULL;
    int r = mfs_vfs_verify(v, lvl);
    printf("%s: verify=%s\n", dev, mfs_ststr((mfs_st)r));
    rc = (r == MFS_OK) ? 0 : 1;
  } else if (argc < 4) {
    usage();
    rc = 2;
  } else if (strcmp(cmd, "ls") == 0) {
    rc = cmd_ls(v, argv[3]);
  } else if (strcmp(cmd, "stat") == 0) {
    rc = cmd_stat(v, argv[3]);
  } else if (strcmp(cmd, "cat") == 0) {
    rc = cmd_cat(v, argv[3]);
  } else if (strcmp(cmd, "write") == 0) {
    rc = cmd_write(v, argv[3], argc, argv, 4);
  } else if (strcmp(cmd, "mkdir") == 0) {
    int r = mfs_vfs_mkdir(v, argv[3], 0755u, 0u, 0u);
    if (r != MFS_OK) {
      fprintf(stderr, "mkdir: %s\n", mfs_ststr((mfs_st)r));
      rc = 1;
    }
  } else if (strcmp(cmd, "rm") == 0) {
    int r = mfs_vfs_unlink(v, argv[3]);
    if (r != MFS_OK)
      r = mfs_vfs_rmdir(v, argv[3]); /* era un directorio */
    if (r != MFS_OK) {
      fprintf(stderr, "rm: %s\n", mfs_ststr((mfs_st)r));
      rc = 1;
    }
  } else if (strcmp(cmd, "chmod") == 0) {
    if (argc < 5) {
      usage();
      rc = 2;
    } else {
      mfs_attr a;
      memset(&a, 0, sizeof(a));
      a.mode = (uint16_t)strtoul(argv[4], NULL, 8);
      int r = mfs_vfs_setattr(v, argv[3], MFS_ATTR_MODE, &a);
      if (r != MFS_OK) {
        fprintf(stderr, "chmod: %s\n", mfs_ststr((mfs_st)r));
        rc = 1;
      }
    }
  } else {
    usage();
    rc = 2;
  }

  (void)mfs_vfs_sync(v);
  mfs_vfs_unmount(v);
  return rc;
}
