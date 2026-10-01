/* test_vfs.c — capa de integración VFS portable (§21, §22)
 *
 * Ejerce `platform/common/mfs_vfs.c` — la MISMA ruta de código que consumen
 * los front-ends FUSE 3 (Linux) y WinFsp (Windows) — sobre una imagen real,
 * sin necesidad de FUSE, WinFsp ni privilegios.
 */
#include "mfs_test.h"
#include "mfs_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFS_DEV "mfs_vfs_test.img"
#define VFS_SIZE (1u << 20) /* 1 MiB */
#define VFS_UID 1000u
#define VFS_GID 1000u

/* ---- utilidades ---- */

static bool make_image(const char *path, uint32_t size) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  uint8_t blk[4096];
  memset(blk, 0xFFu, sizeof(blk)); /* medio virgen */
  uint32_t done = 0u;
  while (done < size) {
    uint32_t take = size - done;
    if (take > sizeof(blk))
      take = (uint32_t)sizeof(blk);
    if (fwrite(blk, 1u, take, f) != take) {
      fclose(f);
      return false;
    }
    done += take;
  }
  fclose(f);
  return true;
}

typedef struct {
  char names[16][64];
  uint32_t n;
  uint32_t dirs;
  uint32_t files;
} dirlist_t;

static int on_dirent(void *ctx, const char *name, const mfs_vfs_attr *st) {
  dirlist_t *d = (dirlist_t *)ctx;
  if (d->n < 16u) {
    snprintf(d->names[d->n], sizeof(d->names[0]), "%s", name);
    d->n++;
  }
  if (st) {
    if ((st->mode & MFS_S_IFMT) == MFS_S_IFDIR)
      d->dirs++;
    else
      d->files++;
  }
  return 0; /* continuar */
}

static bool dirlist_has(const dirlist_t *d, const char *name) {
  for (uint32_t i = 0; i < d->n; i++)
    if (strcmp(d->names[i], name) == 0)
      return true;
  return false;
}

static uint32_t perms(const mfs_vfs_attr *a) { return a->mode & 07777u; }

/* ---- suite ---- */

void test_vfs_portable(void) {
  TEST_BEGIN(
      "Capa VFS portable: formato, montaje, permisos, E/S y persistencia");

  /* 1. formato y sondeo -------------------------------------------------- */
  CHECK(make_image(VFS_DEV, VFS_SIZE));

  mfs_mount_opts o;
  mfs_mount_opts_default(&o);
  o.uid = VFS_UID;
  o.gid = VFS_GID;
  o.file_perm = 0644u;
  o.dir_perm = 0755u;
  o.label = "VFSTEST";

  char err[128] = {0};
  CHECK_EQ(mfs_vfs_format(VFS_DEV, &o, err, sizeof(err)), MFS_OK);

  char label[32] = {0};
  uint8_t mode = 0xFFu;
  CHECK_EQ(mfs_vfs_probe(VFS_DEV, NULL, label, sizeof(label), &mode), MFS_OK);
  CHECK(strcmp(label, "VFSTEST") == 0);
  CHECK(mode < (uint8_t)MFS_MODE_COUNT);

  /* 2. montaje ----------------------------------------------------------- */
  mfs_vfs *v = NULL;
  CHECK_EQ(mfs_vfs_mount(VFS_DEV, &o, &v, err, sizeof(err)), MFS_OK);
  if (v ==
      NULL) { /* sin montaje el resto de comprobaciones no tendría sentido */
    CHECK(v != NULL);
    remove(VFS_DEV);
    TEST_END("VFS portable");
    return;
  }
  CHECK(!mfs_vfs_is_readonly(v));
  CHECK_EQ(mfs_vfs_uid(v), VFS_UID);
  CHECK_EQ(mfs_vfs_gid(v), VFS_GID);
  CHECK_EQ(strcmp(mfs_vfs_device(v), VFS_DEV), 0);

  mfs_vfs_attr a;
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/", &a), MFS_OK);
  CHECK_EQ(a.mode & MFS_S_IFMT, MFS_S_IFDIR);
  CHECK_EQ(perms(&a), 0755u);
  CHECK_EQ(a.uid, VFS_UID);

  /* 3. directorios y ficheros ------------------------------------------- */
  CHECK_EQ(mfs_vfs_mkdir(v, "/dir", 0750u, VFS_UID, VFS_GID), MFS_OK);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir", &a), MFS_OK);
  CHECK_EQ(a.mode & MFS_S_IFMT, MFS_S_IFDIR);
  CHECK_EQ(perms(&a), 0750u);
  CHECK_EQ(mfs_vfs_getattr(v, "/nope", &a), MFS_ENOENT);

  mfs_vfs_fh fh = 0;
  CHECK_EQ(mfs_vfs_create(v, "/dir/a.txt", 0640u, VFS_UID, VFS_GID, &fh),
           MFS_OK);
  CHECK(fh != 0u);

  const char *text = "hola MatrixFS";
  const size_t tlen = strlen(text);
  size_t wr = 0;
  CHECK_EQ(mfs_vfs_write(v, fh, text, 0u, tlen, &wr), MFS_OK);
  CHECK_EQ(wr, tlen);
  CHECK_EQ(mfs_vfs_flush(v, fh), MFS_OK);

  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/a.txt", &a), MFS_OK);
  CHECK_EQ(a.size, tlen);
  CHECK_EQ(a.mode & MFS_S_IFMT, MFS_S_IFREG);
  CHECK_EQ(perms(&a), 0640u);

  /* E/S con desplazamiento explícito */
  char buf[64];
  size_t rd = 0;
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(mfs_vfs_read(v, fh, buf, 0u, sizeof(buf), &rd), MFS_OK);
  CHECK_EQ(rd, tlen);
  CHECK(memcmp(buf, text, tlen) == 0);

  memset(buf, 0, sizeof(buf));
  CHECK_EQ(mfs_vfs_read(v, fh, buf, 5u, 8u, &rd), MFS_OK);
  CHECK_EQ(rd, 8u);
  CHECK(memcmp(buf, "MatrixFS", 8u) == 0);

  /* más allá de EOF: cero bytes, sin error */
  rd = 0xFFu;
  CHECK_EQ(mfs_vfs_read(v, fh, buf, tlen + 10u, 4u, &rd), MFS_OK);
  CHECK_EQ(rd, 0u);

  /* sobreescritura en el centro sin cambiar el tamaño */
  CHECK_EQ(mfs_vfs_write(v, fh, "XXXXXX", 5u, 6u, &wr), MFS_OK);
  CHECK_EQ(wr, 6u);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/a.txt", &a), MFS_OK);
  CHECK_EQ(a.size, tlen);
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(mfs_vfs_read(v, fh, buf, 0u, tlen, &rd), MFS_OK);
  CHECK(memcmp(buf, "hola XXXXXXFS", tlen) == 0);

  /* 4. permisos POSIX persistidos (§21.2) -------------------------------- */
  mfs_attr at;
  memset(&at, 0, sizeof(at));
  at.mode = 0600u;
  CHECK_EQ(mfs_vfs_setattr(v, "/dir/a.txt", MFS_ATTR_MODE, &at), MFS_OK);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/a.txt", &a), MFS_OK);
  CHECK_EQ(perms(&a), 0600u);

  memset(&at, 0, sizeof(at));
  at.uid = 2000u;
  at.gid = 2000u;
  CHECK_EQ(mfs_vfs_setattr(v, "/dir/a.txt", MFS_ATTR_UID | MFS_ATTR_GID, &at),
           MFS_OK);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/a.txt", &a), MFS_OK);
  CHECK_EQ(a.uid, 2000u);
  CHECK_EQ(a.gid, 2000u);

  /* control de acceso POSIX (propietario / grupo / otros / root) */
  CHECK_EQ(mfs_vfs_access(v, "/dir/a.txt", 2000u, 2000u, 6u), MFS_OK);
  CHECK_EQ(mfs_vfs_access(v, "/dir/a.txt", 0u, 0u, 7u), MFS_OK); /* root */
  CHECK_EQ(mfs_vfs_access(v, "/dir/a.txt", 9999u, 9999u, 4u), MFS_EACCES);
  CHECK_EQ(mfs_vfs_access(v, "/dir/a.txt", 9999u, 9999u, 0u), MFS_OK);
  CHECK_EQ(mfs_vfs_access(v, "/noexiste", 0u, 0u, 4u), MFS_ENOENT);

  /* 5. readdir ----------------------------------------------------------- */
  dirlist_t dl;
  memset(&dl, 0, sizeof(dl));
  CHECK_EQ(mfs_vfs_readdir(v, "/", on_dirent, &dl), MFS_OK);
  CHECK(dl.n >= 1u);
  CHECK(dirlist_has(&dl, "dir"));
  CHECK_EQ(dl.dirs + dl.files, dl.n);

  memset(&dl, 0, sizeof(dl));
  CHECK_EQ(mfs_vfs_readdir(v, "/dir", on_dirent, &dl), MFS_OK);
  CHECK(dirlist_has(&dl, "a.txt"));
  CHECK_EQ(mfs_vfs_readdir(v, "/noexiste", on_dirent, &dl), MFS_ENOENT);

  /* 6. rename ------------------------------------------------------------ */
  CHECK_EQ(mfs_vfs_rename(v, "/dir/a.txt", "/dir/b.txt"), MFS_OK);
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/a.txt", &a), MFS_ENOENT);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/b.txt", &a), MFS_OK);
  CHECK_EQ(a.size, tlen);

  /* 7. truncado ---------------------------------------------------------- */
  CHECK_EQ(mfs_vfs_truncate(v, fh, 5u), MFS_OK);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/b.txt", &a), MFS_OK);
  CHECK_EQ(a.size, 5u);
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(mfs_vfs_read(v, fh, buf, 0u, sizeof(buf), &rd), MFS_OK);
  CHECK_EQ(rd, 5u);
  CHECK(memcmp(buf, "hola ", 5u) == 0);

  /* truncado por ruta (sin handle abierto) */
  CHECK_EQ(mfs_vfs_truncate_path(v, "/dir/b.txt", 2u), MFS_OK);
  memset(&a, 0, sizeof(a));
  CHECK_EQ(mfs_vfs_getattr(v, "/dir/b.txt", &a), MFS_OK);
  CHECK_EQ(a.size, 2u);

  /* 8. statfs / health / verify / etiqueta ------------------------------- */
  uint64_t total = 0, freeb = 0, used = 0;
  CHECK_EQ(mfs_vfs_statfs(v, &total, &freeb, &used), MFS_OK);
  CHECK(total > 0u);
  CHECK(freeb <= total); /* el espacio libre no excede el total */
  CHECK(used <= total);
  CHECK(used + freeb <= total + 4096u); /* contabilidad coherente */

  mfs_health_t h;
  memset(&h, 0, sizeof(h));
  CHECK_EQ(mfs_vfs_health(v, &h), MFS_OK);
  CHECK_EQ(mfs_vfs_verify(v, MFS_VERIFY_FULL), MFS_OK);
  char lbl[32] = {0};
  CHECK_EQ(mfs_vfs_label(v, lbl, sizeof(lbl)), MFS_OK);
  CHECK(strcmp(lbl, "VFSTEST") == 0);

  /* 9. cierre y persistencia tras remontar ------------------------------- */
  CHECK_EQ(mfs_vfs_release(v, fh), MFS_OK);
  CHECK_EQ(mfs_vfs_release(v, fh), MFS_EINVAL); /* handle ya cerrado */
  CHECK_EQ(mfs_vfs_sync(v), MFS_OK);
  mfs_vfs_unmount(v);

  mfs_vfs *v2 = NULL;
  CHECK_EQ(mfs_vfs_mount(VFS_DEV, &o, &v2, err, sizeof(err)), MFS_OK);
  if (v2) {
    memset(&a, 0, sizeof(a));
    CHECK_EQ(mfs_vfs_getattr(v2, "/dir/b.txt", &a), MFS_OK);
    CHECK_EQ(a.size, 2u);
    CHECK_EQ(perms(&a), 0600u);
    CHECK_EQ(a.uid, 2000u);
    CHECK_EQ(a.gid, 2000u);

    memset(&dl, 0, sizeof(dl));
    CHECK_EQ(mfs_vfs_readdir(v2, "/dir", on_dirent, &dl), MFS_OK);
    CHECK(dirlist_has(&dl, "b.txt"));

    /* borrado durable */
    CHECK_EQ(mfs_vfs_unlink(v2, "/dir/b.txt"), MFS_OK);
    CHECK_EQ(mfs_vfs_getattr(v2, "/dir/b.txt", &a), MFS_ENOENT);
    CHECK_EQ(mfs_vfs_rmdir(v2, "/dir"), MFS_OK);
    CHECK_EQ(mfs_vfs_getattr(v2, "/dir", &a), MFS_ENOENT);
    CHECK_EQ(mfs_vfs_sync(v2), MFS_OK);
    mfs_vfs_unmount(v2);
  }

  /* 10. montaje de sólo lectura ----------------------------------------- */
  mfs_mount_opts ro = o;
  ro.readonly = true;
  mfs_vfs *v3 = NULL;
  CHECK_EQ(mfs_vfs_mount(VFS_DEV, &ro, &v3, err, sizeof(err)), MFS_OK);
  if (v3) {
    CHECK(mfs_vfs_is_readonly(v3));
    /* la lectura debe seguir siendo posible en un montaje `ro` */
    memset(&a, 0, sizeof(a));
    CHECK_EQ(mfs_vfs_getattr(v3, "/", &a), MFS_OK);
    CHECK_EQ(a.mode & MFS_S_IFMT, MFS_S_IFDIR);
    CHECK_EQ(mfs_vfs_mkdir(v3, "/x", 0755u, 0u, 0u), MFS_EROFS);
    CHECK_EQ(mfs_vfs_unlink(v3, "/x"), MFS_EROFS);
    CHECK_EQ(mfs_vfs_truncate_path(v3, "/x", 0u), MFS_EROFS);
    mfs_vfs_unmount(v3);
  }

  /* 11. traducción de estados a errno POSIX ----------------------------- */
  CHECK_EQ(mfs_vfs_errno(MFS_OK), 0);
  CHECK_EQ(mfs_vfs_errno(MFS_ENOENT), 2);      /* ENOENT   */
  CHECK_EQ(mfs_vfs_errno(MFS_EIO), 5);         /* EIO      */
  CHECK_EQ(mfs_vfs_errno(MFS_EACCES), 13);     /* EACCES   */
  CHECK_EQ(mfs_vfs_errno(MFS_EROFS), 30);      /* EROFS    */
  CHECK_EQ(mfs_vfs_errno(MFS_ENOSPC), 28);     /* ENOSPC   */
  CHECK_EQ(mfs_vfs_errno(MFS_ETABLEFULL), 28); /* ENOSPC   */
  CHECK_EQ(mfs_vfs_errno(MFS_EEXISTS), 17);    /* EEXIST   */
  CHECK_EQ(mfs_vfs_errno(MFS_EBUSY), 16);      /* EBUSY    */
  CHECK_EQ(mfs_vfs_errno(MFS_ENOTSUP), 95);    /* EOPNOTSUPP */

  CHECK(remove(VFS_DEV) == 0);
  TEST_END("VFS portable");
}
