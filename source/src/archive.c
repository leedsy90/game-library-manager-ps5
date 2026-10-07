/* .7z support: inspect an archive (what's inside, where the game is) and
 * extract just the game straight onto a chosen drive.
 *
 * Built on the public-domain LZMA SDK (third_party/lzma). The SDK's own
 * extractor decodes a whole solid block into RAM, which is impossible for
 * 90 GB games, so this file streams instead:
 *
 *   reader/decoder thread  ->  ring of 8 MB buffers  ->  writer thread
 *
 * - "Copy" (stored) archives: the reader thread just reads big chunks, so
 *   reading the source drive and writing the target drive overlap.
 * - LZMA2: decoded with Lzma2DecMt, which uses several CPU cores when the
 *   archive was made with multi-threaded 7-Zip (independent blocks).
 * - LZMA: single-threaded (that's the format's limit).
 * Every file's CRC32 from the archive is checked as it is written.
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "Lzma2DecMt.h"
#include "LzmaDec.h"

#define K_COPY 0
#define K_LZMA2 0x21
#define K_LZMA 0x30101

#define PIPE_SLOTS 6
#define SLOT_SIZE (8u << 20)

static const ISzAlloc g_alloc = {SzAlloc, SzFree};
static const ISzAlloc g_alloc_temp = {SzAllocTemp, SzFreeTemp};
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) { CrcGenerateTable(); }

/* ------------------------------------------------------------------ */
/* Multi-volume input (.7z or .7z.001, .7z.002, ...)                   */
typedef struct {
  ISeekInStream vt; /* must be first */
  int n;
  int fd[256];
  uint64_t size[256];
  uint64_t total;
  uint64_t pos;
} volstream_t;

static SRes vol_read_at(volstream_t *v, uint64_t pos, void *buf, size_t *size) {
  size_t want = *size, done = 0;
  uint8_t *b = buf;
  uint64_t base = 0;
  for (int i = 0; i < v->n && done < want; i++) {
    if (pos >= base + v->size[i]) {
      base += v->size[i];
      continue;
    }
    uint64_t off = pos - base;
    size_t chunk = want - done;
    if (chunk > v->size[i] - off)
      chunk = (size_t)(v->size[i] - off);
    ssize_t r = pread(v->fd[i], b + done, chunk, (off_t)off);
    if (r < 0)
      return SZ_ERROR_READ;
    done += (size_t)r;
    pos += (uint64_t)r;
    base += v->size[i];
    if ((size_t)r < chunk)
      break;
  }
  *size = done;
  return SZ_OK;
}

static SRes vol_Read(ISeekInStreamPtr pp, void *buf, size_t *size) {
  volstream_t *v = (volstream_t *)(void *)pp;
  SRes r = vol_read_at(v, v->pos, buf, size);
  v->pos += *size;
  return r;
}

static SRes vol_Seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin) {
  volstream_t *v = (volstream_t *)(void *)pp;
  int64_t np = origin == SZ_SEEK_SET   ? *pos
               : origin == SZ_SEEK_CUR ? (int64_t)v->pos + *pos
                                       : (int64_t)v->total + *pos;
  if (np < 0)
    return SZ_ERROR_READ;
  v->pos = (uint64_t)np;
  *pos = np;
  return SZ_OK;
}

static void vol_close(volstream_t *v) {
  for (int i = 0; i < v->n; i++)
    close(v->fd[i]);
  v->n = 0;
}

/* Open all volumes of an archive. Accepts x.7z or x.7z.001 */
static bool vol_open(volstream_t *v, const char *path) {
  memset(v, 0, sizeof(*v));
  v->vt.Read = vol_Read;
  v->vt.Seek = vol_Seek;
  size_t l = strlen(path);
  bool split = l > 4 && !strcmp(path + l - 4, ".001");
  for (int i = 0; i < 256; i++) {
    char p[SS_MAX_PATH];
    if (!split) {
      if (i > 0)
        break;
      snprintf(p, sizeof(p), "%s", path);
    } else {
      snprintf(p, sizeof(p), "%.*s.%03d", (int)(l - 4), path, i + 1);
    }
    int fd = open(p, O_RDONLY);
    if (fd < 0)
      break;
    struct stat st;
    fstat(fd, &st);
    v->fd[v->n] = fd;
    v->size[v->n] = (uint64_t)st.st_size;
    v->total += (uint64_t)st.st_size;
    v->n++;
  }
  return v->n > 0;
}

/* ------------------------------------------------------------------ */
typedef struct {
  volstream_t vol;
  CLookToRead2 look;
  CSzArEx db;
  bool open;
  char **names; /* UTF-8, '/' separated */
} arc_t;

static void arc_close(arc_t *a) {
  if (a->names) {
    for (UInt32 i = 0; i < a->db.NumFiles; i++)
      free(a->names[i]);
    free(a->names);
  }
  if (a->open)
    SzArEx_Free(&a->db, &g_alloc);
  ISzAlloc_Free(&g_alloc, a->look.buf);
  vol_close(&a->vol);
  memset(a, 0, sizeof(*a));
}

static char *utf16_to_utf8(const UInt16 *s, size_t n) {
  char *o = malloc(n * 3 + 1), *p = o;
  if (!o)
    return NULL;
  for (size_t i = 0; i < n && s[i]; i++) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    if (c == '\\')
      c = '/';
    if (c < 0x80)
      *p++ = (char)c;
    else if (c < 0x800) {
      *p++ = (char)(0xC0 | (c >> 6));
      *p++ = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      *p++ = (char)(0xE0 | (c >> 12));
      *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
      *p++ = (char)(0x80 | (c & 0x3F));
    } else {
      *p++ = (char)(0xF0 | (c >> 18));
      *p++ = (char)(0x80 | ((c >> 12) & 0x3F));
      *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
      *p++ = (char)(0x80 | (c & 0x3F));
    }
  }
  *p = 0;
  return o;
}

static bool arc_open(arc_t *a, const char *path, char *err, size_t errsz) {
  memset(a, 0, sizeof(*a));
  pthread_once(&g_crc_once, crc_init);
  if (!vol_open(&a->vol, path)) {
    snprintf(err, errsz, "can't open %s: %s", path, strerror(errno));
    return false;
  }
  LookToRead2_CreateVTable(&a->look, False);
  a->look.buf = ISzAlloc_Alloc(&g_alloc, 1 << 20);
  a->look.bufSize = 1 << 20;
  a->look.realStream = &a->vol.vt;
  LookToRead2_INIT(&a->look);
  SzArEx_Init(&a->db);
  SRes r = SzArEx_Open(&a->db, &a->look.vt, &g_alloc, &g_alloc_temp);
  if (r != SZ_OK) {
    snprintf(err, errsz,
             r == SZ_ERROR_UNSUPPORTED
                 ? "this archive uses a feature that isn't supported "
                   "(encrypted headers?)"
                 : "not a readable .7z archive (error %d) - is it complete?",
             r);
    arc_close(a);
    return false;
  }
  a->open = true;
  a->names = calloc(a->db.NumFiles ? a->db.NumFiles : 1, sizeof(char *));
  UInt16 *tmp = malloc(4096 * sizeof(UInt16));
  for (UInt32 i = 0; i < a->db.NumFiles && tmp; i++) {
    size_t n = SzArEx_GetFileNameUtf16(&a->db, i, NULL);
    if (n > 4096)
      n = 4096;
    SzArEx_GetFileNameUtf16(&a->db, i, tmp);
    a->names[i] = utf16_to_utf8(tmp, n);
  }
  free(tmp);
  return true;
}

/* method of a folder (solid block); returns -1 if not a single coder */
static long folder_method(const CSzArEx *p, UInt32 f, const Byte **props,
                          unsigned *propsize) {
  CSzFolder folder;
  CSzData sd;
  const Byte *data = p->db.CodersData + p->db.FoCodersOffsets[f];
  sd.Data = data;
  sd.Size = p->db.FoCodersOffsets[f + 1] - p->db.FoCodersOffsets[f];
  if (SzGetNextFolderItem(&folder, &sd) != SZ_OK)
    return -2;
  if (folder.NumCoders != 1 || folder.NumPackStreams != 1)
    return -1;
  if (props) {
    *props = data + folder.Coders[0].PropsOffset;
    *propsize = folder.Coders[0].PropsSize;
  }
  return (long)folder.Coders[0].MethodID;
}

static const char *method_name(long m) {
  switch (m) {
  case K_COPY:
    return "store (no compression)";
  case K_LZMA2:
    return "LZMA2";
  case K_LZMA:
    return "LZMA";
  case 0x30401:
    return "PPMd";
  case 0x6F10701:
    return "AES encrypted";
  case -1:
    return "filter chain (e.g. BCJ2)";
  default:
    return "other";
  }
}

/* ------------------------------------------------------------------ */
/* Inspect                                                             */

static bool find_tid(const char *s, char *out) {
  for (; s && *s; s++)
    if (looks_like_title_id(s)) {
      snprintf(out, 10, "%.9s", s);
      return true;
    }
  return false;
}

typedef struct {
  char kind[8]; /* folder / exfat / ufs / pfs / pfsc */
  char inner[SS_MAX_PATH]; /* folder prefix ("" = root) or image path */
  char name[256];          /* destination name */
  char title_id[16];
  uint64_t size;
  uint64_t files;
} cand_t;

static int find_candidates(arc_t *a, const char *archive_path, cand_t *c,
                           int max) {
  int n = 0;
  const char *suffix = "sce_sys/param.json";
  size_t sl = strlen(suffix);
  for (UInt32 i = 0; i < a->db.NumFiles && n < max; i++) {
    const char *nm = a->names[i];
    if (!nm || SzArEx_IsDir(&a->db, i))
      continue;
    size_t l = strlen(nm);
    const char *ik = image_kind(nm);
    if (ik) {
      cand_t *x = &c[n++];
      memset(x, 0, sizeof(*x));
      snprintf(x->kind, sizeof(x->kind), "%s", ik);
      snprintf(x->inner, sizeof(x->inner), "%s", nm);
      snprintf(x->name, sizeof(x->name), "%s", path_basename(nm));
      if (!find_tid(nm, x->title_id))
        find_tid(path_basename(archive_path), x->title_id);
      x->size = SzArEx_GetFileSize(&a->db, i);
      x->files = 1;
      continue;
    }
    if (l < sl || strcasecmp(nm + l - sl, suffix) ||
        (l > sl && nm[l - sl - 1] != '/'))
      continue;
    /* prefix = folder containing sce_sys (with trailing '/'), or "" */
    char prefix[SS_MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%.*s", (int)(l - sl), nm);
    /* must have eboot.bin next to sce_sys */
    char eb[SS_MAX_PATH];
    snprintf(eb, sizeof(eb), "%seboot.bin", prefix);
    bool has_eboot = false;
    for (UInt32 k = 0; k < a->db.NumFiles && !has_eboot; k++)
      has_eboot = a->names[k] && !strcasecmp(a->names[k], eb);
    if (!has_eboot)
      continue;
    cand_t *x = &c[n++];
    memset(x, 0, sizeof(*x));
    strcpy(x->kind, "folder");
    snprintf(x->inner, sizeof(x->inner), "%s", prefix);
    size_t pl = strlen(prefix);
    for (UInt32 k = 0; k < a->db.NumFiles; k++) {
      if (!a->names[k] || strncmp(a->names[k], prefix, pl))
        continue;
      if (!SzArEx_IsDir(&a->db, k)) {
        x->size += SzArEx_GetFileSize(&a->db, k);
        x->files++;
      }
    }
    if (!find_tid(prefix, x->title_id))
      find_tid(path_basename(archive_path), x->title_id);
    if (x->title_id[0])
      snprintf(x->name, sizeof(x->name), "%s", x->title_id);
    else if (pl > 1) {
      char tmp[SS_MAX_PATH];
      snprintf(tmp, sizeof(tmp), "%.*s", (int)(pl - 1), prefix);
      snprintf(x->name, sizeof(x->name), "%s", path_basename(tmp));
    } else {
      snprintf(x->name, sizeof(x->name), "game");
    }
  }
  return n;
}

void archive_info_json(const char *path, sbuf_t *b) {
  arc_t a;
  char err[256];
  if (!arc_open(&a, path, err, sizeof(err))) {
    sb_puts(b, "{\"ok\":false,\"message\":");
    sb_json_str(b, err);
    sb_puts(b, "}");
    return;
  }
  uint64_t unpack = 0, nfiles = 0;
  for (UInt32 i = 0; i < a.db.NumFiles; i++)
    if (!SzArEx_IsDir(&a.db, i)) {
      unpack += SzArEx_GetFileSize(&a.db, i);
      nfiles++;
    }
  /* methods used */
  char methods[256] = "";
  bool supported = true, any_lzma2 = false;
  for (UInt32 f = 0; f < a.db.db.NumFolders; f++) {
    long m = folder_method(&a.db, f, NULL, NULL);
    if (m != K_COPY && m != K_LZMA && m != K_LZMA2)
      supported = false;
    if (m == K_LZMA2)
      any_lzma2 = true;
    const char *mn = method_name(m);
    if (!strstr(methods, mn)) {
      size_t l = strlen(methods);
      snprintf(methods + l, sizeof(methods) - l, "%s%s", l ? ", " : "", mn);
    }
  }
  cand_t *c = calloc(32, sizeof(cand_t));
  int nc = c ? find_candidates(&a, path, c, 32) : 0;
  sb_puts(b, "{\"ok\":true,\"path\":");
  sb_json_str(b, path);
  sb_printf(b,
            ",\"volumes\":%d,\"archive_size\":%llu,\"files\":%llu,"
            "\"unpacked\":%llu,\"blocks\":%u,\"supported\":%s,"
            "\"multithread\":%s,\"methods\":",
            a.vol.n, (unsigned long long)a.vol.total,
            (unsigned long long)nfiles, (unsigned long long)unpack,
            a.db.db.NumFolders, supported ? "true" : "false",
            any_lzma2 ? "true" : "false");
  sb_json_str(b, methods);
  sb_puts(b, ",\"candidates\":[");
  for (int i = 0; i < nc; i++) {
    sb_printf(b, "%s{\"kind\":\"%s\",\"inner\":", i ? "," : "", c[i].kind);
    sb_json_str(b, c[i].inner);
    sb_puts(b, ",\"name\":");
    sb_json_str(b, c[i].name);
    sb_puts(b, ",\"title_id\":");
    sb_json_str(b, c[i].title_id);
    sb_printf(b, ",\"size\":%llu,\"files\":%llu}",
              (unsigned long long)c[i].size, (unsigned long long)c[i].files);
  }
  sb_puts(b, "]");
  /* top-level items that aren't the game (fixes, backports, DLC, ...) */
  sb_puts(b, ",\"extras\":[");
  int ne = 0;
  const char *gp = nc > 0 ? c[0].inner : "";
  size_t gl = strlen(gp);
  for (UInt32 i = 0; i < a.db.NumFiles; i++) {
    const char *nm = a.names[i];
    if (!nm || SzArEx_IsDir(&a.db, i))
      continue;
    if (gl && !strncmp(nm, gp, gl))
      continue;
    char top[512];
    const char *sl = strchr(nm, '/');
    /* inside the game's wrapper folder, show the next level down */
    size_t skip = 0;
    if (gl) {
      const char *gs = strchr(gp, '/');
      size_t w = gs ? (size_t)(gs - gp) + 1 : 0;
      if (w && w < gl && !strncmp(nm, gp, w))
        skip = w;
    }
    sl = strchr(nm + skip, '/');
    snprintf(top, sizeof(top), "%.*s", sl ? (int)(sl - nm) : (int)strlen(nm), nm);
    bool seen = false;
    /* crude de-dup: compare with previous entries' prefixes */
    for (UInt32 k = 0; k < i && !seen; k++) {
      const char *o = a.names[k];
      if (!o || SzArEx_IsDir(&a.db, k) || (gl && !strncmp(o, gp, gl)))
        continue;
      seen = !strncmp(o, top, strlen(top)) &&
             (o[strlen(top)] == '/' || o[strlen(top)] == 0);
    }
    if (seen)
      continue;
    uint64_t sz = 0, nfl = 0;
    for (UInt32 k = 0; k < a.db.NumFiles; k++) {
      const char *o = a.names[k];
      if (o && !SzArEx_IsDir(&a.db, k) && !strncmp(o, top, strlen(top)) &&
          (o[strlen(top)] == '/' || o[strlen(top)] == 0) &&
          !(gl && !strncmp(o, gp, gl))) {
        sz += SzArEx_GetFileSize(&a.db, k);
        nfl++;
      }
    }
    sb_printf(b, "%s{\"path\":", ne++ ? "," : "");
    sb_json_str(b, top);
    sb_printf(b, ",\"files\":%llu,\"size\":%llu}", (unsigned long long)nfl,
              (unsigned long long)sz);
  }
  sb_puts(b, "]");
  /* items sitting next to the archive in its release folder (Fix,
   * Backport, DLC folders...). Only offered when the archive is inside its
   * own folder named with the title id, not loose on a drive. */
  sb_puts(b, ",\"siblings\":[");
  char dir[SS_MAX_PATH];
  int ns = 0;
  char tid[16] = "";
  if (path_dirname(path, dir, sizeof(dir)) && find_tid(path_basename(dir), tid)) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (e->d_name[0] == '.')
        continue;
      char full[SS_MAX_PATH];
      path_join(full, sizeof(full), dir, e->d_name);
      /* skip the archive's own volumes */
      const char *ab = path_basename(path);
      size_t stem = strlen(ab);
      if (stem > 4 && !strcmp(ab + stem - 4, ".001"))
        stem -= 4;
      if (!strncmp(e->d_name, ab, stem))
        continue;
      uint64_t sz = 0, nfl = 0;
      measure_tree_ex(full, &sz, &nfl, 10000, NULL, 0);
      sb_printf(b, "%s{\"path\":", ns++ ? "," : "");
      sb_json_str(b, e->d_name);
      sb_printf(b, ",\"files\":%llu,\"size\":%llu}", (unsigned long long)nfl,
                (unsigned long long)sz);
    }
    if (d)
      closedir(d);
  }
  sb_puts(b, "]}");
  free(c);
  arc_close(&a);
}

/* ------------------------------------------------------------------ */
/* Extraction                                                          */

typedef struct {
  uint8_t *data[PIPE_SLOTS];
  size_t len[PIPE_SLOTS];
  int head, tail, count;
  bool eof;
  pthread_mutex_t m;
  pthread_cond_t can_put, can_get;
} pipe_t;

typedef struct {
  /* per file in the current folder, in stream order */
  UInt32 *idx;
  int n;
  int cur;
  uint64_t left;  /* bytes left in current file */
  int fd;         /* -1 = skipping this file */
  uint8_t *sbuf;  /* small file: collected in memory, written by the pool */
  size_t sfill;
  char spath[SS_MAX_PATH];
  UInt32 crc;
  bool check_crc;
  UInt32 want_crc;
} folder_plan_t;

/* ---- small-file writer pool --------------------------------------
 * Creating thousands of small files one after another is what makes game
 * folders slow (each open/close is a metadata round-trip). Files up to
 * SMALL_MAX are collected in memory and written by several threads at once
 * while decoding carries on. */
#define SMALL_MAX (1u << 20)
#define POOL_THREADS 6
#define POOL_MAX_BYTES (96u << 20)

typedef struct sjob {
  struct sjob *next;
  char path[SS_MAX_PATH];
  uint8_t *data;
  size_t len;
} sjob_t;

typedef struct {
  sjob_t *head, *tail;
  size_t bytes;
  int busy;
  bool stop;
  pthread_mutex_t m;
  pthread_cond_t cv_work, cv_room;
  pthread_t th[POOL_THREADS];
  int nth;
  copy_progress_t *pr;
  int *write_error;
  char *err;
} spool_t;

static void *spool_main(void *arg) {
  spool_t *p = arg;
  for (;;) {
    pthread_mutex_lock(&p->m);
    while (!p->head && !p->stop)
      pthread_cond_wait(&p->cv_work, &p->m);
    if (!p->head && p->stop) {
      pthread_mutex_unlock(&p->m);
      return NULL;
    }
    sjob_t *j = p->head;
    p->head = j->next;
    if (!p->head)
      p->tail = NULL;
    p->busy++;
    pthread_mutex_unlock(&p->m);

    int fd = open(j->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int e = 0;
    if (fd < 0) {
      char d[SS_MAX_PATH];
      if (path_dirname(j->path, d, sizeof(d)))
        mkdir_p(d);
      fd = open(j->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0)
      e = errno;
    size_t off = 0;
    while (!e && off < j->len) {
      ssize_t w = write(fd, j->data + off, j->len - off);
      if (w <= 0) {
        if (w < 0 && errno == EINTR)
          continue;
        e = w < 0 ? errno : EIO;
        break;
      }
      off += (size_t)w;
    }
    if (fd >= 0 && close(fd) != 0 && !e)
      e = errno;
    if (!e) {
      __atomic_add_fetch(&p->pr->bytes_done, j->len, __ATOMIC_RELAXED);
      __atomic_add_fetch(&p->pr->files_done, 1, __ATOMIC_RELAXED);
    }

    pthread_mutex_lock(&p->m);
    if (e && !*p->write_error) {
      *p->write_error = e;
      snprintf(p->err, 256, "can't write %s: %s", j->path, strerror(e));
      p->pr->cancel = 1;
    }
    p->bytes -= j->len;
    p->busy--;
    pthread_cond_broadcast(&p->cv_room);
    pthread_mutex_unlock(&p->m);
    free(j->data);
    free(j);
  }
}

static void spool_put(spool_t *p, const char *path, uint8_t *data,
                      size_t len) {
  sjob_t *j = calloc(1, sizeof(*j));
  snprintf(j->path, sizeof(j->path), "%s", path);
  j->data = data;
  j->len = len;
  pthread_mutex_lock(&p->m);
  while (p->bytes + len > POOL_MAX_BYTES && (p->head || p->busy))
    pthread_cond_wait(&p->cv_room, &p->m);
  p->bytes += len;
  if (p->tail)
    p->tail->next = j;
  else
    p->head = j;
  p->tail = j;
  pthread_cond_signal(&p->cv_work);
  pthread_mutex_unlock(&p->m);
}

static void spool_finish(spool_t *p) {
  pthread_mutex_lock(&p->m);
  p->stop = true;
  pthread_cond_broadcast(&p->cv_work);
  pthread_mutex_unlock(&p->m);
  for (int i = 0; i < p->nth; i++)
    pthread_join(p->th[i], NULL);
}

typedef struct {
  arc_t *a;
  copy_progress_t *pr;
  const char *prefix; /* folder candidate: inner prefix; else NULL */
  const char *image;  /* image candidate: inner path; else NULL */
  const char *stage;  /* where to write (dir for folder, file for image) */
  const char *extras; /* everything else in the archive goes here (or NULL) */
  pipe_t pipe;
  folder_plan_t plan;
  int crc_errors;
  int write_error;
  char err[256];
  spool_t pool;
  /* producer side */
  int slot;
  size_t slot_fill;
  uint64_t decoded;
} xctx_t;

/* output path for archive entry i, or NULL if not part of the game */
static bool safe_rel(const char *rel) {
  /* refuse anything that tries to escape the target folder */
  return *rel && rel[0] != '/' && !strstr(rel, "../") && strcmp(rel, "..") &&
         !(strlen(rel) >= 3 && !strcmp(rel + strlen(rel) - 3, "/.."));
}

/* Where entry i goes: the game part into `stage`, everything else (fixes,
 * backports, DLC...) into `extras` with its archive path kept. */
static bool out_path(xctx_t *x, UInt32 i, char *out, size_t outsz) {
  const char *nm = x->a->names[i];
  if (!nm)
    return false;
  if (x->image) {
    if (!strcmp(nm, x->image))
      return snprintf(out, outsz, "%s", x->stage) < (int)outsz;
  } else {
    size_t pl = strlen(x->prefix);
    if (!strncmp(nm, x->prefix, pl)) {
      const char *rel = nm + pl;
      return safe_rel(rel) && path_join(out, outsz, x->stage, rel);
    }
    /* the wrapper folders that lead to the game folder itself */
    if (pl && !strncmp(x->prefix, nm, strlen(nm)) &&
        x->prefix[strlen(nm)] == '/' && SzArEx_IsDir(&x->a->db, i))
      return false;
  }
  if (!x->extras || !safe_rel(nm))
    return false;
  return path_join(out, outsz, x->extras, nm);
}

static void mkparent(const char *p) {
  char d[SS_MAX_PATH];
  if (path_dirname(p, d, sizeof(d)))
    mkdir_p(d);
}

static void plan_open_current(xctx_t *x) {
  folder_plan_t *pl = &x->plan;
  pl->fd = -1;
  if (pl->cur >= pl->n)
    return;
  UInt32 i = pl->idx[pl->cur];
  pl->left = SzArEx_GetFileSize(&x->a->db, i);
  pl->crc = CRC_INIT_VAL;
  pl->check_crc = SzBitWithVals_Check(&x->a->db.CRCs, i);
  pl->want_crc = pl->check_crc ? x->a->db.CRCs.Vals[i] : 0;
  pl->sbuf = NULL;
  pl->sfill = 0;
  char p[SS_MAX_PATH];
  if (out_path(x, i, p, sizeof(p)) && pl->left <= SMALL_MAX && x->pool.nth) {
    pl->sbuf = malloc(pl->left ? pl->left : 1);
    if (pl->sbuf) {
      snprintf(pl->spath, sizeof(pl->spath), "%s", p);
      return;
    }
  }
  if (out_path(x, i, p, sizeof(p))) {
    mkparent(p);
    pl->fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (pl->fd < 0 && !x->write_error) {
      x->write_error = errno;
      snprintf(x->err, sizeof(x->err), "can't create %s: %s", p,
               strerror(errno));
    }
  }
}

static void plan_close_current(xctx_t *x) {
  folder_plan_t *pl = &x->plan;
  if (pl->sbuf) {
    if (pl->check_crc && CRC_GET_DIGEST(pl->crc) != pl->want_crc) {
      x->crc_errors++;
      if (!x->err[0])
        snprintf(x->err, sizeof(x->err), "CRC mismatch in %s",
                 x->a->names[pl->idx[pl->cur]]);
    }
    spool_put(&x->pool, pl->spath, pl->sbuf, pl->sfill);
    pl->sbuf = NULL;
    return;
  }
  if (pl->fd >= 0) {
    if (close(pl->fd) != 0 && !x->write_error)
      x->write_error = errno;
    __atomic_add_fetch(&x->pr->files_done, 1, __ATOMIC_RELAXED);
    if (pl->check_crc && CRC_GET_DIGEST(pl->crc) != pl->want_crc) {
      x->crc_errors++;
      if (!x->err[0])
        snprintf(x->err, sizeof(x->err), "CRC mismatch in %s",
                 x->a->names[pl->idx[pl->cur]]);
    }
  }
  pl->fd = -1;
}

/* consume decoded bytes of the current folder (writer thread) */
static void dispatch(xctx_t *x, const uint8_t *buf, size_t len) {
  folder_plan_t *pl = &x->plan;
  while (len) {
    while (pl->cur < pl->n && pl->left == 0) {
      plan_close_current(x);
      pl->cur++;
      plan_open_current(x);
    }
    if (pl->cur >= pl->n)
      return; /* trailing data */
    size_t n = len < pl->left ? len : (size_t)pl->left;
    if (pl->sbuf) {
      pl->crc = CrcUpdate(pl->crc, buf, n);
      memcpy(pl->sbuf + pl->sfill, buf, n);
      pl->sfill += n;
    } else if (pl->fd >= 0) {
      pl->crc = CrcUpdate(pl->crc, buf, n);
      size_t off = 0;
      while (off < n) {
        ssize_t w = write(pl->fd, buf + off, n - off);
        if (w <= 0) {
          if (w < 0 && errno == EINTR)
            continue;
          if (!x->write_error) {
            x->write_error = w < 0 ? errno : EIO;
            snprintf(x->err, sizeof(x->err), "write failed: %s",
                     strerror(x->write_error));
          }
          x->pr->cancel = 1;
          return;
        }
        off += (size_t)w;
      }
      __atomic_add_fetch(&x->pr->bytes_done, n, __ATOMIC_RELAXED);
    }
    pl->left -= n;
    buf += n;
    len -= n;
  }
}

static void *writer_main(void *arg) {
  xctx_t *x = arg;
  pipe_t *p = &x->pipe;
  for (;;) {
    pthread_mutex_lock(&p->m);
    while (p->count == 0 && !p->eof)
      pthread_cond_wait(&p->can_get, &p->m);
    if (p->count == 0 && p->eof) {
      pthread_mutex_unlock(&p->m);
      break;
    }
    int s = p->tail;
    pthread_mutex_unlock(&p->m);
    if (!x->pr->cancel)
      dispatch(x, p->data[s], p->len[s]);
    pthread_mutex_lock(&p->m);
    p->tail = (p->tail + 1) % PIPE_SLOTS;
    p->count--;
    pthread_cond_signal(&p->can_put);
    pthread_mutex_unlock(&p->m);
  }
  return NULL;
}

/* producer: get a free slot (blocks while the writer is behind) */
static uint8_t *pipe_slot(xctx_t *x) {
  pipe_t *p = &x->pipe;
  pthread_mutex_lock(&p->m);
  while (p->count == PIPE_SLOTS)
    pthread_cond_wait(&p->can_put, &p->m);
  int s = p->head;
  pthread_mutex_unlock(&p->m);
  return p->data[s];
}

static void pipe_commit(xctx_t *x, size_t len) {
  pipe_t *p = &x->pipe;
  pthread_mutex_lock(&p->m);
  p->len[p->head] = len;
  p->head = (p->head + 1) % PIPE_SLOTS;
  p->count++;
  pthread_cond_signal(&p->can_get);
  pthread_mutex_unlock(&p->m);
}

static void pipe_drain(xctx_t *x) {
  pipe_t *p = &x->pipe;
  pthread_mutex_lock(&p->m);
  while (p->count > 0)
    pthread_cond_wait(&p->can_put, &p->m);
  pthread_mutex_unlock(&p->m);
}

/* ISeqOutStream for the decoders: batch bytes into pipe slots */
typedef struct {
  ISeqOutStream vt;
  xctx_t *x;
} outstream_t;

static void producer_put(xctx_t *x, const uint8_t *buf, size_t size) {
  while (size) {
    uint8_t *slot = pipe_slot(x);
    size_t room = SLOT_SIZE - x->slot_fill;
    size_t n = size < room ? size : room;
    memcpy(slot + x->slot_fill, buf, n);
    x->slot_fill += n;
    buf += n;
    size -= n;
    if (x->slot_fill == SLOT_SIZE) {
      pipe_commit(x, x->slot_fill);
      x->slot_fill = 0;
    }
  }
}

static void producer_flush(xctx_t *x) {
  if (x->slot_fill) {
    pipe_slot(x);
    pipe_commit(x, x->slot_fill);
    x->slot_fill = 0;
  }
  pipe_drain(x);
}

static size_t out_Write(ISeqOutStreamPtr pp, const void *buf, size_t size) {
  outstream_t *o = (outstream_t *)(void *)pp;
  if (o->x->pr->cancel)
    return 0;
  producer_put(o->x, buf, size);
  o->x->decoded += size;
  return size;
}

/* ISeqInStream over the pack stream of a folder */
typedef struct {
  ISeqInStream vt;
  volstream_t *v;
  uint64_t pos, left;
} instream_t;

static SRes in_Read(ISeqInStreamPtr pp, void *buf, size_t *size) {
  instream_t *s = (instream_t *)(void *)pp;
  if (*size > s->left)
    *size = (size_t)s->left;
  if (*size == 0)
    return SZ_OK;
  SRes r = vol_read_at(s->v, s->pos, buf, size);
  s->pos += *size;
  s->left -= *size;
  return r;
}

static SRes decode_folder(xctx_t *x, UInt32 f) {
  CSzArEx *db = &x->a->db;
  const Byte *props;
  unsigned propsize = 0;
  long m = folder_method(db, f, &props, &propsize);
  uint64_t unpack = SzAr_GetFolderUnpackSize(&db->db, f);
  UInt32 ps = db->db.FoStartPackStreamIndex[f];
  instream_t in;
  in.vt.Read = in_Read;
  in.v = &x->a->vol;
  in.pos = db->dataPos + db->db.PackPositions[ps];
  in.left = db->db.PackPositions[ps + 1] - db->db.PackPositions[ps];
  outstream_t out;
  out.vt.Write = out_Write;
  out.x = x;

  if (m == K_COPY) {
    /* stored: read straight into pipe slots */
    uint64_t left = unpack;
    while (left && !x->pr->cancel) {
      uint8_t *slot = pipe_slot(x);
      size_t n = left < SLOT_SIZE ? (size_t)left : SLOT_SIZE;
      size_t got = n;
      if (vol_read_at(&x->a->vol, in.pos, slot, &got) != SZ_OK || got != n)
        return SZ_ERROR_READ;
      in.pos += got;
      left -= got;
      x->decoded += got;
      pipe_commit(x, got);
    }
    pipe_drain(x);
    return x->pr->cancel ? SZ_ERROR_PROGRESS : SZ_OK;
  }
  if (m == K_LZMA2 && propsize == 1) {
    CLzma2DecMtHandle d = Lzma2DecMt_Create(&g_alloc, &g_alloc);
    if (!d)
      return SZ_ERROR_MEM;
    CLzma2DecMtProps mp;
    memset(&mp, 0, sizeof(mp));
    mp.inBufSize_ST = 1 << 20;
    mp.outStep_ST = 1 << 22;
    mp.numThreads = 6;
    mp.inBufSize_MT = 1 << 20;
    mp.outBlockMax = (size_t)1 << 28;
    mp.inBlockMax = mp.outBlockMax + mp.outBlockMax / 16;
    UInt64 inProcessed = 0;
    int isMT = 0;
    SRes r = Lzma2DecMt_Decode(d, props[0], &mp, &out.vt, &unpack, 1, &in.vt,
                               &inProcessed, &isMT, NULL);
    Lzma2DecMt_Destroy(d);
    ss_log("  LZMA2 block %u: %s", f, isMT ? "multi-threaded" : "single-thread");
    producer_flush(x);
    return r;
  }
  if (m == K_LZMA && propsize == 5) {
    CLzmaDec dec;
    LzmaDec_Construct(&dec);
    SRes r = LzmaDec_Allocate(&dec, props, propsize, &g_alloc);
    if (r != SZ_OK)
      return r;
    LzmaDec_Init(&dec);
    uint8_t *ib = malloc(1 << 20), *ob = malloc(1 << 22);
    size_t ipos = 0, ilen = 0;
    uint64_t left = unpack;
    while (r == SZ_OK && left && !x->pr->cancel) {
      if (ipos == ilen) {
        ilen = 1 << 20;
        r = in_Read(&in.vt, ib, &ilen);
        ipos = 0;
        if (r != SZ_OK || ilen == 0) {
          r = r ? r : SZ_ERROR_INPUT_EOF;
          break;
        }
      }
      SizeT inl = ilen - ipos, outl = left < (1u << 22) ? (SizeT)left : (1u << 22);
      ELzmaStatus st;
      r = LzmaDec_DecodeToBuf(&dec, ob, &outl, ib + ipos, &inl,
                              LZMA_FINISH_ANY, &st);
      ipos += inl;
      if (outl)
        out_Write(&out.vt, ob, outl);
      left -= outl;
      if (inl == 0 && outl == 0 && r == SZ_OK)
        r = SZ_ERROR_DATA;
    }
    free(ib);
    free(ob);
    LzmaDec_Free(&dec, &g_alloc);
    producer_flush(x);
    return x->pr->cancel ? SZ_ERROR_PROGRESS : r;
  }
  return SZ_ERROR_UNSUPPORTED;
}

int archive_extract(const char *archive, const char *inner, const char *stage,
                    const char *extras, copy_progress_t *pr, char *err,
                    size_t errsz) {
  arc_t a;
  if (!arc_open(&a, archive, err, errsz))
    return -1;
  xctx_t *x = calloc(1, sizeof(xctx_t));
  if (!x) {
    arc_close(&a);
    return -1;
  }
  x->a = &a;
  x->pr = pr;
  x->stage = stage;
  x->extras = extras && *extras ? extras : NULL;
  bool is_image = image_kind(inner) != NULL;
  if (is_image)
    x->image = inner;
  else
    x->prefix = inner;

  /* make sure the requested game really is there */
  {
    bool found = false;
    char want[SS_MAX_PATH];
    snprintf(want, sizeof(want), "%ssce_sys/param.json", is_image ? "" : inner);
    for (UInt32 i = 0; i < a.db.NumFiles && !found; i++)
      found = a.names[i] &&
              (is_image ? !strcmp(a.names[i], inner)
                        : !strcasecmp(a.names[i], want));
    if (!found) {
      snprintf(err, errsz, "no game at '%s' in this archive", inner);
      free(x);
      arc_close(&a);
      return -1;
    }
  }

  /* which entries do we need, and which solid blocks contain them */
  CSzArEx *db = &a.db;
  UInt32 nf = db->db.NumFolders;
  bool *need = calloc(nf ? nf : 1, sizeof(bool));
  pr->bytes_total = pr->files_total = 0;
  pr->bytes_done = pr->files_done = 0;
  if (!is_image)
    mkdir_p(stage);
  for (UInt32 i = 0; i < db->NumFiles; i++) {
    char p[SS_MAX_PATH];
    if (!out_path(x, i, p, sizeof(p)))
      continue;
    if (SzArEx_IsDir(db, i)) {
      mkdir_p(p);
      continue;
    }
    UInt32 f = db->FileToFolder[i];
    pr->files_total++;
    pr->bytes_total += SzArEx_GetFileSize(db, i);
    if (f == (UInt32)-1) { /* empty file */
      mkparent(p);
      int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd >= 0)
        close(fd);
      pr->files_done++;
    } else {
      need[f] = true;
    }
  }
  if (pr->files_total == 0) {
    snprintf(err, errsz, "nothing to extract for %s", inner);
    free(need);
    free(x);
    arc_close(&a);
    return -1;
  }

  /* small-file pool */
  x->pool.pr = pr;
  x->pool.write_error = &x->write_error;
  x->pool.err = x->err;
  pthread_mutex_init(&x->pool.m, NULL);
  pthread_cond_init(&x->pool.cv_work, NULL);
  pthread_cond_init(&x->pool.cv_room, NULL);
  for (int t = 0; t < POOL_THREADS; t++) {
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 256 * 1024);
    if (pthread_create(&x->pool.th[x->pool.nth], &pa, spool_main, &x->pool) == 0)
      x->pool.nth++;
    pthread_attr_destroy(&pa);
  }

  /* writer thread + buffers */
  int rc = 0;
  pthread_mutex_init(&x->pipe.m, NULL);
  pthread_cond_init(&x->pipe.can_put, NULL);
  pthread_cond_init(&x->pipe.can_get, NULL);
  for (int s = 0; s < PIPE_SLOTS; s++) {
    x->pipe.data[s] = malloc(SLOT_SIZE);
    if (!x->pipe.data[s])
      rc = -1;
  }
  pthread_t wt;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1024 * 1024);
  if (rc == 0 && pthread_create(&wt, &at, writer_main, x) != 0)
    rc = -1;
  pthread_attr_destroy(&at);
  if (rc) {
    snprintf(err, errsz, "out of memory");
  }

  for (UInt32 f = 0; f < nf && rc == 0 && !pr->cancel; f++) {
    if (!need[f])
      continue;
    /* plan: files of this block in stream order */
    UInt32 a0 = db->FolderToFile[f], a1 = db->FolderToFile[f + 1];
    x->plan.idx = malloc(sizeof(UInt32) * (a1 - a0 + 1));
    x->plan.n = 0;
    for (UInt32 i = a0; i < a1; i++)
      if (db->FileToFolder[i] == f)
        x->plan.idx[x->plan.n++] = i;
    x->plan.cur = 0;
    plan_open_current(x);
    SRes r = decode_folder(x, f);
    /* finish the last file(s) of the block */
    while (x->plan.cur < x->plan.n && x->plan.left == 0) {
      plan_close_current(x);
      x->plan.cur++;
      plan_open_current(x);
    }
    plan_close_current(x);
    free(x->plan.idx);
    x->plan.idx = NULL;
    if (r != SZ_OK) {
      rc = -1;
      if (pr->cancel && !x->write_error)
        snprintf(err, errsz, "cancelled");
      else if (x->err[0])
        snprintf(err, errsz, "%s", x->err);
      else if (r == SZ_ERROR_UNSUPPORTED)
        snprintf(err, errsz,
                 "compression method not supported (%s) - extract this one "
                 "on a PC",
                 method_name(folder_method(db, f, NULL, NULL)));
      else
        snprintf(err, errsz,
                 "archive data error %d in block %u - is the file complete "
                 "and undamaged?",
                 (int)r, f);
    }
  }

  /* stop writer */
  if (x->pipe.data[0]) {
    pthread_mutex_lock(&x->pipe.m);
    x->pipe.eof = true;
    pthread_cond_signal(&x->pipe.can_get);
    pthread_mutex_unlock(&x->pipe.m);
    pthread_join(wt, NULL);
  }
  spool_finish(&x->pool);
  if (rc == 0 && x->write_error) {
    rc = -1;
    snprintf(err, errsz, "%s", x->err);
  }
  if (rc == 0 && x->crc_errors) {
    rc = -1;
    snprintf(err, errsz, "%d file(s) failed their CRC check (%s)",
             x->crc_errors, x->err);
  }
  if (rc == 0 && pr->cancel) {
    rc = -1;
    snprintf(err, errsz, "cancelled");
  }
  for (int s = 0; s < PIPE_SLOTS; s++)
    free(x->pipe.data[s]);
  free(need);
  free(x);
  arc_close(&a);
  return rc;
}

/* all volume paths of an archive, for deleting after extraction */
int archive_volumes(const char *path, char out[][SS_MAX_PATH], int max) {
  size_t l = strlen(path);
  bool split = l > 4 && !strcmp(path + l - 4, ".001");
  int n = 0;
  for (int i = 0; i < max; i++) {
    if (!split) {
      snprintf(out[n++], SS_MAX_PATH, "%s", path);
      break;
    }
    snprintf(out[n], SS_MAX_PATH, "%.*s.%03d", (int)(l - 4), path, i + 1);
    if (!is_file(out[n]))
      break;
    n++;
  }
  return n;
}

/* Copy the release folder's other items (not the archive) into `dest`. */
int archive_copy_siblings(const char *archive, const char *dest,
                          copy_progress_t *pr, char *err, size_t errsz) {
  char dir[SS_MAX_PATH];
  if (!path_dirname(archive, dir, sizeof(dir)))
    return 0;
  const char *ab = path_basename(archive);
  size_t stem = strlen(ab);
  if (stem > 4 && !strcmp(ab + stem - 4, ".001"))
    stem -= 4;
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  mkdir_p(dest);
  struct dirent *e;
  int n = 0, rc = 0;
  while ((e = readdir(d)) && !rc) {
    if (e->d_name[0] == '.' || !strncmp(e->d_name, ab, stem))
      continue;
    char src[SS_MAX_PATH], dst[SS_MAX_PATH];
    path_join(src, sizeof(src), dir, e->d_name);
    path_join(dst, sizeof(dst), dest, e->d_name);
    if (path_exists(dst))
      continue;
    copy_progress_t p2;
    memset(&p2, 0, sizeof(p2));
    if (copy_tree(src, dst, &p2) != 0) {
      snprintf(err, errsz, "copying %s: %s", e->d_name, p2.error);
      rc = -1;
    } else {
      n++;
      if (pr) {
        __atomic_add_fetch(&pr->files_done, p2.files_done, __ATOMIC_RELAXED);
      }
    }
  }
  closedir(d);
  return rc ? -1 : n;
}
