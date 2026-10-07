/* Fast copy engine.
 *
 * Why the stock copy is slow: one file at a time, 64 KB stdio buffers.
 * With thousands of small files every open/close/metadata round trip is
 * paid serially, and big files never get a deep enough I/O queue.
 *
 * What we do instead:
 *   1. walk the source once, create the whole directory tree up front
 *   2. build a work list; files above SPLIT_SIZE are cut into chunks
 *   3. N worker threads pull work items and copy with large buffers
 *      using pread/pwrite (so chunks of one big file run in parallel)
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SPLIT_SIZE (256ull * 1024 * 1024)
#define CHUNK_SIZE (64ull * 1024 * 1024)

typedef struct {
  char *src;
  char *dst;
  uint64_t off;
  uint64_t len;
  uint64_t fsize;
  mode_t mode;
  bool first_chunk; /* counts toward files_done */
  bool last_chunk;
  bool chunked;
} work_t;

typedef struct {
  work_t *items;
  size_t count, cap;
  size_t next;
  pthread_mutex_t lock;
  copy_progress_t *pr;
  size_t bufsz;
  int err;
} plan_t;

static void plan_add(plan_t *pl, work_t w) {
  if (pl->count == pl->cap) {
    size_t nc = pl->cap ? pl->cap * 2 : 1024;
    work_t *n = realloc(pl->items, nc * sizeof(work_t));
    if (!n) {
      pl->err = ENOMEM;
      return;
    }
    pl->items = n;
    pl->cap = nc;
  }
  pl->items[pl->count++] = w;
}

static void set_error(copy_progress_t *pr, int *err, int e, const char *what,
                      const char *path) {
  if (!*err) {
    *err = e ? e : EIO;
    snprintf(pr->error, sizeof(pr->error), "%s %s: %s", what, path,
             strerror(*err));
  }
  pr->cancel = 1;
}

static void plan_file(plan_t *pl, const char *s, const char *d,
                      const struct stat *st) {
  uint64_t size = (uint64_t)st->st_size;
  pl->pr->bytes_total += size;
  pl->pr->files_total++;
  if (size <= SPLIT_SIZE) {
    work_t w = {strdup(s), strdup(d), 0,    size, size, st->st_mode & 0777,
                true,      true,      false};
    plan_add(pl, w);
    return;
  }
  /* pre-create the big file at its final size so chunks can pwrite */
  int fd = open(d, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 0777);
  if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
    set_error(pl->pr, &pl->err, errno, "create", d);
    if (fd >= 0)
      close(fd);
    return;
  }
  close(fd);
  for (uint64_t off = 0; off < size; off += CHUNK_SIZE) {
    uint64_t len = size - off < CHUNK_SIZE ? size - off : CHUNK_SIZE;
    work_t w = {strdup(s),
                strdup(d),
                off,
                len,
                size,
                st->st_mode & 0777,
                off == 0,
                off + len >= size,
                true};
    plan_add(pl, w);
  }
}

static int plan_tree(plan_t *pl, const char *s, const char *d) {
  struct stat st;
  if (lstat(s, &st) != 0) {
    set_error(pl->pr, &pl->err, errno, "stat", s);
    return -1;
  }
  if (S_ISREG(st.st_mode)) {
    plan_file(pl, s, d, &st);
    return 0;
  }
  if (!S_ISDIR(st.st_mode))
    return 0; /* skip symlinks/sockets */
  if (mkdir(d, st.st_mode & 0777 ? st.st_mode & 0777 : 0777) != 0 &&
      errno != EEXIST) {
    set_error(pl->pr, &pl->err, errno, "mkdir", d);
    return -1;
  }
  DIR *dir = opendir(s);
  if (!dir) {
    set_error(pl->pr, &pl->err, errno, "opendir", s);
    return -1;
  }
  struct dirent *e;
  while ((e = readdir(dir)) && !pl->err && !pl->pr->cancel) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char cs[SS_MAX_PATH], cd[SS_MAX_PATH];
    if (!path_join(cs, sizeof(cs), s, e->d_name) ||
        !path_join(cd, sizeof(cd), d, e->d_name)) {
      set_error(pl->pr, &pl->err, ENAMETOOLONG, "path", s);
      break;
    }
    plan_tree(pl, cs, cd);
  }
  closedir(dir);
  return pl->err ? -1 : 0;
}

static int copy_item(plan_t *pl, work_t *w, char *buf) {
  copy_progress_t *pr = pl->pr;
  int in = open(w->src, O_RDONLY);
  if (in < 0)
    return errno;
  int out;
  if (w->chunked)
    out = open(w->dst, O_WRONLY);
  else
    out = open(w->dst, O_WRONLY | O_CREAT | O_TRUNC, w->mode ? w->mode : 0666);
  if (out < 0) {
    int e = errno;
    close(in);
    return e;
  }
  uint64_t off = w->off, left = w->len;
  int rc = 0;
  while (left && !pr->cancel) {
    size_t want = left < pl->bufsz ? (size_t)left : pl->bufsz;
    ssize_t r = pread(in, buf, want, (off_t)off);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      rc = r < 0 ? errno : EIO; /* source shrank: treat as error */
      break;
    }
    size_t done = 0;
    while (done < (size_t)r) {
      ssize_t wr = pwrite(out, buf + done, (size_t)r - done,
                          (off_t)(off + done));
      if (wr < 0 && errno == EINTR)
        continue;
      if (wr <= 0) {
        rc = wr < 0 ? errno : EIO;
        break;
      }
      done += (size_t)wr;
    }
    if (rc)
      break;
    off += (uint64_t)r;
    left -= (uint64_t)r;
    __atomic_add_fetch(&pr->bytes_done, (uint64_t)r, __ATOMIC_RELAXED);
  }
  close(in);
  if (close(out) != 0 && !rc)
    rc = errno;
  if (!rc && !pr->cancel && w->first_chunk)
    __atomic_add_fetch(&pr->files_done, 1, __ATOMIC_RELAXED);
  return rc;
}

static void *worker(void *arg) {
  plan_t *pl = arg;
  char *buf = malloc(pl->bufsz);
  if (!buf) {
    pthread_mutex_lock(&pl->lock);
    set_error(pl->pr, &pl->err, ENOMEM, "alloc", "buffer");
    pthread_mutex_unlock(&pl->lock);
    return NULL;
  }
  for (;;) {
    pthread_mutex_lock(&pl->lock);
    if (pl->next >= pl->count || pl->pr->cancel) {
      pthread_mutex_unlock(&pl->lock);
      break;
    }
    work_t *w = &pl->items[pl->next++];
    pthread_mutex_unlock(&pl->lock);
    int rc = copy_item(pl, w, buf);
    if (rc) {
      pthread_mutex_lock(&pl->lock);
      set_error(pl->pr, &pl->err, rc, "copy", w->src);
      pthread_mutex_unlock(&pl->lock);
      break;
    }
  }
  free(buf);
  return NULL;
}

int copy_tree(const char *src, const char *dst, copy_progress_t *pr) {
  plan_t pl;
  memset(&pl, 0, sizeof(pl));
  pthread_mutex_init(&pl.lock, NULL);
  pl.pr = pr;
  pl.bufsz = (size_t)g_cfg.copy_buffer_mb * 1024 * 1024;
  pr->bytes_total = pr->files_total = 0;
  pr->bytes_done = pr->files_done = 0;
  pr->error[0] = 0;

  if (path_exists(dst)) {
    snprintf(pr->error, sizeof(pr->error), "destination exists: %s", dst);
    return EEXIST;
  }

  plan_tree(&pl, src, dst);

  if (!pl.err && !pr->cancel) {
    int nthreads = g_cfg.copy_threads;
    if ((size_t)nthreads > pl.count)
      nthreads = pl.count ? (int)pl.count : 1;
    pthread_t th[16];
    int started = 0;
    for (int i = 0; i < nthreads; i++) {
      pthread_attr_t a;
      pthread_attr_init(&a);
      pthread_attr_setstacksize(&a, 256 * 1024);
      if (pthread_create(&th[started], &a, worker, &pl) == 0)
        started++;
      pthread_attr_destroy(&a);
    }
    if (!started)
      worker(&pl);
    for (int i = 0; i < started; i++)
      pthread_join(th[i], NULL);
  }

  for (size_t i = 0; i < pl.count; i++) {
    free(pl.items[i].src);
    free(pl.items[i].dst);
  }
  free(pl.items);
  pthread_mutex_destroy(&pl.lock);

  if (pr->cancel && !pl.err) {
    snprintf(pr->error, sizeof(pr->error), "cancelled");
    return ECANCELED;
  }
  return pl.err;
}

typedef struct {
  uint64_t bytes, files, entries;
  dev_t dev;
  uint64_t deadline;   /* 0 = none */
  char *progress;      /* optional live status text */
  size_t progress_sz;
  const char *label;
  bool timed_out;
} measure_ctx_t;

static void measure(measure_ctx_t *m, const char *p, int depth) {
  if (m->timed_out)
    return;
  struct stat st;
  if (lstat(p, &st) != 0)
    return;
  if (S_ISREG(st.st_mode)) {
    m->bytes += (uint64_t)st.st_size;
    m->files++;
    return;
  }
  /* never follow into another filesystem mounted inside the tree */
  if (!S_ISDIR(st.st_mode) || depth > 64 || (depth > 0 && st.st_dev != m->dev))
    return;
  DIR *d = opendir(p);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if ((++m->entries & 255) == 0) {
      if (m->progress)
        snprintf(m->progress, m->progress_sz, "measuring %s (%llu files)",
                 m->label, (unsigned long long)m->files);
      if (m->deadline && now_ms() > m->deadline) {
        m->timed_out = true;
        break;
      }
    }
    char c[SS_MAX_PATH];
    if (path_join(c, sizeof(c), p, e->d_name))
      measure(m, c, depth + 1);
  }
  closedir(d);
}

int measure_tree_ex(const char *src, uint64_t *bytes, uint64_t *files,
                    uint64_t max_ms, char *progress, size_t progress_sz) {
  measure_ctx_t m;
  memset(&m, 0, sizeof(m));
  struct stat st;
  if (lstat(src, &st) == 0)
    m.dev = st.st_dev;
  m.deadline = max_ms ? now_ms() + max_ms : 0;
  m.progress = progress;
  m.progress_sz = progress_sz;
  m.label = src;
  measure(&m, src, 0);
  *bytes = m.bytes;
  *files = m.files;
  return m.timed_out ? -1 : 0;
}

int measure_tree(const char *src, uint64_t *bytes, uint64_t *files) {
  return measure_tree_ex(src, bytes, files, 0, NULL, 0);
}

/* Compare structure: every regular file in src exists in dst with the
 * same size. Returns number of mismatches (0 = good). */
static int verify(const char *s, const char *d, char *err, size_t errsz,
                  int *bad) {
  struct stat ss, ds;
  if (lstat(s, &ss) != 0)
    return 0;
  if (S_ISREG(ss.st_mode)) {
    if (stat(d, &ds) != 0 || !S_ISREG(ds.st_mode) ||
        ds.st_size != ss.st_size) {
      if (!*bad)
        snprintf(err, errsz, "mismatch: %s", d);
      (*bad)++;
    }
    return 0;
  }
  if (!S_ISDIR(ss.st_mode))
    return 0;
  DIR *dir = opendir(s);
  if (!dir)
    return 0;
  struct dirent *e;
  while ((e = readdir(dir))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char cs[SS_MAX_PATH], cd[SS_MAX_PATH];
    if (path_join(cs, sizeof(cs), s, e->d_name) &&
        path_join(cd, sizeof(cd), d, e->d_name))
      verify(cs, cd, err, errsz, bad);
  }
  closedir(dir);
  return 0;
}

int verify_tree(const char *src, const char *dst, char *err, size_t errsz) {
  int bad = 0;
  err[0] = 0;
  verify(src, dst, err, errsz, &bad);
  return bad;
}
