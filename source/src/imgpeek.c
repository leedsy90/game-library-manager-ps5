/* Read-only peeking inside .exfat and .ffpkg (UFS2) images without
 * mounting them. We only need the top one or two directory levels and
 * sce_sys/param.json, so this is a tiny reader, not a filesystem driver.
 * Nothing here ever writes to the image. */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *image_kind(const char *name) {
  if (ends_with_ci(name, ".ffpkg"))
    return "ufs";
  if (ends_with_ci(name, ".exfat"))
    return "exfat";
  if (ends_with_ci(name, ".ffpfsc"))
    return "pfsc";
  if (ends_with_ci(name, ".ffpfs"))
    return "pfs";
  return NULL;
}

static bool pread_full(int fd, void *buf, size_t n, uint64_t off) {
  uint8_t *p = buf;
  while (n) {
    ssize_t r = pread(fd, p, n, (off_t)off);
    if (r <= 0)
      return false;
    p += r;
    n -= (size_t)r;
    off += (uint64_t)r;
  }
  return true;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) {
  return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* Generic directory entry */
typedef struct {
  char name[256];
  bool dir;
  uint64_t ref;   /* exfat: first cluster, ufs: inode */
  uint64_t size;
  bool contig;    /* exfat NoFatChain */
} dent_t;

#define MAX_DENTS 512

/* ================================================================== */
/* exFAT                                                               */
typedef struct {
  int fd;
  uint64_t fat_off;
  uint64_t heap_off;
  uint32_t clus_size;
  uint32_t root_clus;
  uint32_t clus_count;
} exfat_t;

static bool exfat_open(exfat_t *x, int fd) {
  uint8_t bs[512];
  if (!pread_full(fd, bs, sizeof(bs), 0) || memcmp(bs + 3, "EXFAT   ", 8))
    return false;
  uint32_t bps = 1u << bs[108];
  uint32_t spc = 1u << bs[109];
  x->fd = fd;
  x->fat_off = (uint64_t)le32(bs + 80) * bps;
  x->heap_off = (uint64_t)le32(bs + 88) * bps;
  x->clus_count = le32(bs + 92);
  x->root_clus = le32(bs + 96);
  x->clus_size = bps * spc;
  return x->clus_size > 0 && x->clus_size <= 32 * 1024 * 1024;
}

static uint32_t exfat_next(exfat_t *x, uint32_t c) {
  uint8_t b[4];
  if (!pread_full(x->fd, b, 4, x->fat_off + (uint64_t)c * 4))
    return 0xFFFFFFFF;
  return le32(b);
}

/* Read up to maxlen bytes of a cluster chain into a malloc'd buffer. */
static uint8_t *exfat_read_chain(exfat_t *x, uint32_t first, uint64_t len,
                                 bool contig, size_t maxlen, size_t *outlen) {
  if (len == 0 || len > maxlen)
    len = maxlen;
  uint8_t *buf = malloc(len);
  if (!buf)
    return NULL;
  size_t got = 0;
  uint32_t c = first;
  int guard = 0;
  while (got < len && c >= 2 && c < x->clus_count + 2 && guard++ < 100000) {
    size_t n = x->clus_size;
    if (n > len - got)
      n = len - got;
    if (!pread_full(x->fd, buf + got, n,
                    x->heap_off + (uint64_t)(c - 2) * x->clus_size))
      break;
    got += n;
    c = contig ? c + 1 : exfat_next(x, c);
    if (!contig && c >= 0xFFFFFFF7)
      break;
  }
  *outlen = got;
  return buf;
}

static int exfat_list(exfat_t *x, uint32_t clus, uint64_t len, bool contig,
                      dent_t *out, int max) {
  size_t dlen = 0;
  uint8_t *d = exfat_read_chain(x, clus, len, contig, 1024 * 1024, &dlen);
  if (!d)
    return -1;
  int n = 0;
  for (size_t off = 0; off + 32 <= dlen && n < max; off += 32) {
    uint8_t *e = d + off;
    if (e[0] == 0x00)
      break;
    if (e[0] != 0x85)
      continue;
    int secondaries = e[1];
    uint16_t attrs = le16(e + 4);
    if (off + 32 * (size_t)(secondaries + 1) > dlen)
      break;
    uint8_t *se = e + 32;
    if (se[0] != 0xC0)
      continue;
    dent_t *de = &out[n];
    memset(de, 0, sizeof(*de));
    int namelen = se[3];
    de->contig = (se[1] & 0x02) != 0;
    de->ref = le32(se + 20);
    de->size = le64(se + 24);
    de->dir = (attrs & 0x10) != 0;
    int ni = 0;
    for (int s = 2; s <= secondaries && ni < namelen; s++) {
      uint8_t *ne = e + 32 * s;
      if (ne[0] != 0xC1)
        break;
      for (int k = 0; k < 15 && ni < namelen; k++, ni++) {
        uint16_t ch = le16(ne + 2 + k * 2);
        if (ni < 255)
          de->name[ni] = ch < 0x80 ? (char)ch : '?';
      }
    }
    de->name[ni < 255 ? ni : 255] = 0;
    n++;
    off += 32 * (size_t)secondaries;
  }
  free(d);
  return n;
}

/* ================================================================== */
/* UFS2                                                                */
typedef struct {
  int fd;
  int32_t iblkno, bsize, fsize, frag, inopb, ipg, fpg;
} ufs_t;

static bool ufs_open(ufs_t *u, int fd) {
  uint8_t sb[1376 + 8];
  /* same search order as FreeBSD's SBLOCKSEARCH */
  static const uint64_t locs[] = {65536, 8192, 0, 262144};
  bool found = false;
  for (size_t i = 0; i < sizeof(locs) / sizeof(locs[0]) && !found; i++)
    found = pread_full(fd, sb, sizeof(sb), locs[i]) &&
            le32(sb + 1372) == 0x19540119;
  if (!found)
    return false;
  u->fd = fd;
  u->iblkno = (int32_t)le32(sb + 16);
  u->bsize = (int32_t)le32(sb + 48);
  u->fsize = (int32_t)le32(sb + 52);
  u->frag = (int32_t)le32(sb + 56);
  u->inopb = (int32_t)le32(sb + 120);
  u->ipg = (int32_t)le32(sb + 184);
  u->fpg = (int32_t)le32(sb + 188);
  return u->bsize > 0 && u->fsize > 0 && u->inopb > 0 && u->ipg > 0 &&
         u->fpg > 0 && u->bsize <= 1024 * 1024;
}

static bool ufs_inode(ufs_t *u, uint64_t ino, uint8_t di[256]) {
  uint64_t cg = ino / (uint64_t)u->ipg;
  uint64_t cgimin = cg * (uint64_t)u->fpg + (uint64_t)u->iblkno;
  uint64_t fsba =
      cgimin + ((ino % (uint64_t)u->ipg) / (uint64_t)u->inopb) * u->frag;
  uint64_t off =
      fsba * (uint64_t)u->fsize + (ino % (uint64_t)u->inopb) * 256;
  return pread_full(u->fd, di, 256, off);
}

/* read file data via direct blocks only (enough for small files/dirs) */
static uint8_t *ufs_read(ufs_t *u, const uint8_t di[256], size_t maxlen,
                         size_t *outlen) {
  uint64_t size = le64(di + 16);
  if (size > maxlen)
    size = maxlen;
  uint64_t direct_max = 12ull * (uint64_t)u->bsize;
  if (size > direct_max)
    size = direct_max;
  uint8_t *buf = malloc(size ? size : 1);
  if (!buf)
    return NULL;
  size_t got = 0;
  for (int i = 0; i < 12 && got < size; i++) {
    uint64_t blk = le64(di + 112 + i * 8);
    size_t n = (size_t)u->bsize;
    if (n > size - got)
      n = size - got;
    if (blk == 0)
      memset(buf + got, 0, n);
    else if (!pread_full(u->fd, buf + got, n, blk * (uint64_t)u->fsize))
      break;
    got += n;
  }
  *outlen = got;
  return buf;
}

static int ufs_list(ufs_t *u, uint64_t ino, dent_t *out, int max) {
  uint8_t di[256];
  if (!ufs_inode(u, ino, di))
    return -1;
  if ((le16(di) & 0170000) != 0040000)
    return -1;
  size_t len = 0;
  uint8_t *d = ufs_read(u, di, 1024 * 1024, &len);
  if (!d)
    return -1;
  int n = 0;
  size_t off = 0;
  while (off + 8 <= len && n < max) {
    uint32_t dino = le32(d + off);
    uint16_t reclen = le16(d + off + 4);
    uint8_t type = d[off + 6];
    uint8_t namlen = d[off + 7];
    if (reclen < 8 || off + reclen > len)
      break;
    if (dino && namlen && off + 8 + namlen <= len) {
      dent_t *de = &out[n];
      memset(de, 0, sizeof(*de));
      memcpy(de->name, d + off + 8, namlen);
      de->name[namlen] = 0;
      de->ref = dino;
      de->dir = type == 4; /* DT_DIR */
      if (strcmp(de->name, ".") && strcmp(de->name, ".."))
        n++;
    }
    off += reclen;
  }
  free(d);
  return n;
}

/* ================================================================== */
/* Common logic                                                        */
typedef struct {
  bool is_exfat;
  exfat_t x;
  ufs_t u;
} img_t;

static int img_list(img_t *im, const dent_t *dir, dent_t *out, int max) {
  if (im->is_exfat) {
    if (!dir)
      return exfat_list(&im->x, im->x.root_clus, 0, false, out, max);
    return exfat_list(&im->x, (uint32_t)dir->ref, dir->size, dir->contig, out,
                      max);
  }
  return ufs_list(&im->u, dir ? dir->ref : 2, out, max);
}

static char *img_read_file(img_t *im, const dent_t *f) {
  size_t len = 0;
  uint8_t *buf;
  if (im->is_exfat) {
    buf = exfat_read_chain(&im->x, (uint32_t)f->ref, f->size, f->contig,
                           256 * 1024, &len);
  } else {
    uint8_t di[256];
    if (!ufs_inode(&im->u, f->ref, di))
      return NULL;
    buf = ufs_read(&im->u, di, 256 * 1024, &len);
  }
  if (!buf)
    return NULL;
  char *s = malloc(len + 1);
  if (s) {
    memcpy(s, buf, len);
    s[len] = 0;
  }
  free(buf);
  return s;
}

static const dent_t *find(const dent_t *v, int n, const char *name,
                          bool want_dir) {
  for (int i = 0; i < n; i++)
    if (v[i].dir == want_dir && !strcasecmp(v[i].name, name))
      return &v[i];
  return NULL;
}

/* Does `dir` (NULL = root) contain sce_sys/param.json? Reads it if so. */
static bool check_game_dir(img_t *im, const dent_t *dir, char *tid,
                           size_t tidsz, char *title, size_t titlesz) {
  dent_t *ents = calloc(MAX_DENTS, sizeof(dent_t));
  dent_t *sys = calloc(MAX_DENTS, sizeof(dent_t));
  bool found = false;
  if (!ents || !sys)
    goto out;
  int n = img_list(im, dir, ents, MAX_DENTS);
  if (n <= 0)
    goto out;
  const dent_t *sce = find(ents, n, "sce_sys", true);
  if (!sce)
    goto out;
  int m = img_list(im, sce, sys, MAX_DENTS);
  const dent_t *pj = m > 0 ? find(sys, m, "param.json", false) : NULL;
  if (!pj)
    goto out;
  found = true;
  char *json = img_read_file(im, pj);
  if (json) {
    parse_param_json(json, tid, tidsz, title, titlesz);
    free(json);
  }
out:
  free(ents);
  free(sys);
  return found;
}

peek_result_t image_peek(const char *path, char *title_id, size_t tidsz,
                         char *title, size_t titlesz, char *wrapper,
                         size_t wrapsz) {
  title_id[0] = 0;
  if (title && titlesz)
    title[0] = 0;
  if (wrapper && wrapsz)
    wrapper[0] = 0;
  const char *kind = image_kind(path);
  if (!kind || (strcmp(kind, "exfat") && strcmp(kind, "ufs")))
    return PEEK_UNSUPPORTED;

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return PEEK_ERROR;
  img_t im;
  memset(&im, 0, sizeof(im));
  im.is_exfat = !strcmp(kind, "exfat");
  bool ok = im.is_exfat ? exfat_open(&im.x, fd) : ufs_open(&im.u, fd);
  if (!ok) {
    close(fd);
    return PEEK_ERROR;
  }

  peek_result_t res = PEEK_NO_GAME;
  if (check_game_dir(&im, NULL, title_id, tidsz, title, titlesz)) {
    res = PEEK_OK;
  } else {
    dent_t *root = calloc(MAX_DENTS, sizeof(dent_t));
    int n = root ? img_list(&im, NULL, root, MAX_DENTS) : -1;
    if (n < 0)
      res = PEEK_ERROR;
    for (int i = 0; i < n; i++) {
      if (!root[i].dir || root[i].name[0] == '.' ||
          !strcasecmp(root[i].name, "System Volume Information"))
        continue;
      if (check_game_dir(&im, &root[i], title_id, tidsz, title, titlesz)) {
        res = PEEK_WRAPPED;
        if (wrapper && wrapsz)
          snprintf(wrapper, wrapsz, "%s", root[i].name);
        break;
      }
    }
    free(root);
  }
  close(fd);
  return res;
}

/* List a directory inside an .exfat/.ffpkg image (read-only), e.g. "fakelib"
 * or "sce_sys". Writes "name(size)" entries, comma separated, into out.
 * Returns number of entries, -1 on error. */
int image_list_dir(const char *path, const char *subdir, char *out,
                   size_t outsz) {
  out[0] = 0;
  const char *kind = image_kind(path);
  if (!kind || (strcmp(kind, "exfat") && strcmp(kind, "ufs")))
    return -1;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  img_t im;
  memset(&im, 0, sizeof(im));
  im.is_exfat = !strcmp(kind, "exfat");
  if (!(im.is_exfat ? exfat_open(&im.x, fd) : ufs_open(&im.u, fd))) {
    close(fd);
    return -1;
  }
  dent_t *a = calloc(MAX_DENTS, sizeof(dent_t));
  dent_t *b = calloc(MAX_DENTS, sizeof(dent_t));
  int n = -1;
  if (a && b) {
    n = img_list(&im, NULL, a, MAX_DENTS);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", subdir ? subdir : "");
    char *save = NULL;
    for (char *part = strtok_r(buf, "/", &save); part && n >= 0;
         part = strtok_r(NULL, "/", &save)) {
      const dent_t *d = find(a, n, part, true);
      if (!d) {
        n = -1;
        break;
      }
      dent_t cur = *d;
      n = img_list(&im, &cur, b, MAX_DENTS);
      dent_t *t = a;
      a = b;
      b = t;
    }
    size_t used = 0;
    for (int i = 0; i < n; i++) {
      int w = snprintf(out + used, outsz - used, "%s%s%s(%llu)",
                       used ? ", " : "", a[i].name, a[i].dir ? "/" : "",
                       (unsigned long long)a[i].size);
      if (w < 0 || (size_t)w >= outsz - used)
        break;
      used += (size_t)w;
    }
  }
  free(a);
  free(b);
  close(fd);
  return n;
}

/* ================================================================== */
/* exFAT: rename one file inside an image, in place, same name length.  */
/*                                                                      */
/* Used for the save fix on .exfat images, which ShadowMount mounts      */
/* read-only. Only the directory entry set of that one file is touched:  */
/* the UTF-16 name characters, the stream entry's NameHash and the set's */
/* SetChecksum. Both checksums are verified against the existing entry   */
/* before anything is written, and the entry is re-read afterwards.      */

static uint16_t exfat_name_hash(const uint16_t *name, int len) {
  uint16_t h = 0;
  for (int i = 0; i < len; i++) {
    uint16_t c = name[i];
    if (c >= 'a' && c <= 'z')
      c = (uint16_t)(c - 32); /* up-case (ASCII names only) */
    h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
    h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
  }
  return h;
}

static uint16_t exfat_set_checksum(const uint8_t *set, int nentries) {
  uint16_t c = 0;
  for (int i = 0; i < nentries * 32; i++) {
    if (i == 2 || i == 3)
      continue;
    c = (uint16_t)(((c & 1) ? 0x8000 : 0) + (c >> 1) + set[i]);
  }
  return c;
}

/* byte offsets in the image of every byte-range of a directory */
static int exfat_dir_clusters(exfat_t *x, uint32_t first, uint64_t len,
                              bool contig, uint64_t *offs, int max) {
  int n = 0;
  uint32_t c = first;
  uint64_t got = 0;
  if (len == 0)
    len = 64ull * 1024 * 1024; /* root dir: follow the chain */
  while (n < max && got < len && c >= 2 && c < x->clus_count + 2) {
    offs[n++] = x->heap_off + (uint64_t)(c - 2) * x->clus_size;
    got += x->clus_size;
    c = contig ? c + 1 : exfat_next(x, c);
    if (!contig && c >= 0xFFFFFFF7)
      break;
  }
  return n;
}

static bool exfat_entry_rw(exfat_t *x, const uint64_t *offs, int nclus,
                           uint64_t index, uint8_t e[32], bool write) {
  uint64_t byte = index * 32;
  uint64_t ci = byte / x->clus_size;
  if (ci >= (uint64_t)nclus)
    return false;
  uint64_t off = offs[ci] + byte % x->clus_size;
  if (write)
    return pwrite(x->fd, e, 32, (off_t)off) == 32;
  return pread_full(x->fd, e, 32, off);
}

/* Find directory `dirname` in the root, then rename `from` to `to` inside
 * it (same length, ASCII). Returns 0 on success, otherwise sets err. */
static int exfat_rename_in(exfat_t *x, const char *dirname, const char *from,
                           const char *to, char *err, size_t errsz) {
  int len = (int)strlen(from);
  if ((int)strlen(to) != len || len == 0 || len > 255) {
    snprintf(err, errsz, "names must have the same length");
    return -1;
  }
  /* locate the directory */
  dent_t *root = calloc(MAX_DENTS, sizeof(dent_t));
  if (!root)
    return -1;
  int n = exfat_list(x, x->root_clus, 0, false, root, MAX_DENTS);
  const dent_t *d = n > 0 ? find(root, n, dirname, true) : NULL;
  if (!d) {
    free(root);
    snprintf(err, errsz, "no %s folder in the image", dirname);
    return -1;
  }
  dent_t dir = *d;
  free(root);

  uint64_t offs[4096];
  int nclus = exfat_dir_clusters(x, (uint32_t)dir.ref, dir.size, dir.contig,
                                 offs, 4096);
  uint64_t total = (uint64_t)nclus * x->clus_size / 32;
  uint8_t set[19 * 32];
  for (uint64_t i = 0; i < total; i++) {
    uint8_t e[32];
    if (!exfat_entry_rw(x, offs, nclus, i, e, false))
      break;
    if (e[0] == 0x00)
      break;
    if (e[0] != 0x85)
      continue;
    int sec = e[1];
    if (sec < 2 || sec > 18)
      continue;
    memcpy(set, e, 32);
    bool ok = true;
    for (int k = 1; k <= sec && ok; k++)
      ok = exfat_entry_rw(x, offs, nclus, i + (uint64_t)k, set + 32 * k,
                          false);
    if (!ok || set[32] != 0xC0)
      continue;
    int namelen = set[32 + 3];
    if (namelen != len) {
      i += (uint64_t)sec;
      continue;
    }
    uint16_t name[255];
    int ni = 0;
    for (int k = 2; k <= sec && ni < namelen; k++) {
      uint8_t *ne = set + 32 * k;
      if (ne[0] != 0xC1)
        break;
      for (int c = 0; c < 15 && ni < namelen; c++, ni++)
        name[ni] = le16(ne + 2 + c * 2);
    }
    bool match = ni == len;
    for (int c = 0; c < len && match; c++)
      match = name[c] == (uint8_t)from[c];
    if (!match) {
      i += (uint64_t)sec;
      continue;
    }
    /* sanity: our hash/checksum must reproduce what's on disk */
    if (exfat_name_hash(name, len) != le16(set + 32 + 4) ||
        exfat_set_checksum(set, sec + 1) != le16(set + 2)) {
      snprintf(err, errsz, "entry checksums didn't verify - not touching it");
      return -1;
    }
    /* rewrite name, hash, checksum */
    for (int c = 0; c < len; c++)
      name[c] = (uint8_t)to[c];
    ni = 0;
    for (int k = 2; k <= sec && ni < len; k++) {
      uint8_t *ne = set + 32 * k;
      for (int c = 0; c < 15 && ni < len; c++, ni++) {
        ne[2 + c * 2] = (uint8_t)(name[ni] & 0xFF);
        ne[3 + c * 2] = (uint8_t)(name[ni] >> 8);
      }
    }
    uint16_t h = exfat_name_hash(name, len);
    set[32 + 4] = (uint8_t)(h & 0xFF);
    set[32 + 5] = (uint8_t)(h >> 8);
    uint16_t cs = exfat_set_checksum(set, sec + 1);
    set[2] = (uint8_t)(cs & 0xFF);
    set[3] = (uint8_t)(cs >> 8);
    for (int k = 0; k <= sec; k++) {
      if (!exfat_entry_rw(x, offs, nclus, i + (uint64_t)k, set + 32 * k,
                          true)) {
        snprintf(err, errsz, "write failed: %s", strerror(errno));
        return -1;
      }
    }
    fsync(x->fd);
    /* read back and verify */
    uint8_t chk[19 * 32];
    for (int k = 0; k <= sec; k++)
      if (!exfat_entry_rw(x, offs, nclus, i + (uint64_t)k, chk + 32 * k,
                          false) ||
          memcmp(chk + 32 * k, set + 32 * k, 32)) {
        snprintf(err, errsz, "read-back didn't match after writing");
        return -1;
      }
    return 0;
  }
  snprintf(err, errsz, "%s not found in %s", from, dirname);
  return -1;
}

int image_rename_in_dir(const char *path, const char *dirname,
                        const char *from, const char *to, char *err,
                        size_t errsz) {
  const char *kind = image_kind(path);
  if (!kind || strcmp(kind, "exfat")) {
    snprintf(err, errsz, "only .exfat images can be changed in place");
    return -1;
  }
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    snprintf(err, errsz, "can't open image for writing: %s", strerror(errno));
    return -1;
  }
  exfat_t x;
  int rc = -1;
  if (exfat_open(&x, fd))
    rc = exfat_rename_in(&x, dirname, from, to, err, errsz);
  else
    snprintf(err, errsz, "not a readable exFAT image");
  close(fd);
  return rc;
}
