#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------- string buffer ---------------- */
void sb_init(sbuf_t *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void sb_free(sbuf_t *b) {
  free(b->data);
  sb_init(b);
}

static void sb_reserve(sbuf_t *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap)
    return;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < b->len + extra + 1)
    nc *= 2;
  char *n = realloc(b->data, nc);
  if (!n)
    abort();
  b->data = n;
  b->cap = nc;
}

void sb_putc(sbuf_t *b, char c) {
  sb_reserve(b, 1);
  b->data[b->len++] = c;
  b->data[b->len] = 0;
}

void sb_puts(sbuf_t *b, const char *s) {
  size_t n = strlen(s);
  sb_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
}

void sb_printf(sbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[512];
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n < sizeof(tmp)) {
    sb_puts(b, tmp);
    return;
  }
  sb_reserve(b, (size_t)n);
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

void sb_json_str(sbuf_t *b, const char *s) {
  sb_putc(b, '"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"':
      sb_puts(b, "\\\"");
      break;
    case '\\':
      sb_puts(b, "\\\\");
      break;
    case '\n':
      sb_puts(b, "\\n");
      break;
    case '\r':
      sb_puts(b, "\\r");
      break;
    case '\t':
      sb_puts(b, "\\t");
      break;
    default:
      if (*p < 0x20)
        sb_printf(b, "\\u%04x", *p);
      else
        sb_putc(b, (char)*p);
    }
  }
  sb_putc(b, '"');
}

/* ---------------- logging ---------------- */
static FILE *g_logf = NULL;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void ss_log_open(const char *path) {
  pthread_mutex_lock(&g_log_lock);
  if (g_logf)
    fclose(g_logf);
  /* keep the log bounded: rotate when > 1 MB */
  struct stat st;
  if (stat(path, &st) == 0 && st.st_size > 1024 * 1024) {
    char old[SS_MAX_PATH];
    snprintf(old, sizeof(old), "%s.1", path);
    rename(path, old);
  }
  g_logf = fopen(path, "a");
  pthread_mutex_unlock(&g_log_lock);
}

void ss_log(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

  pthread_mutex_lock(&g_log_lock);
  printf("[GLM] %s\n", msg);
  fflush(stdout);
  if (g_logf) {
    fprintf(g_logf, "%s %s\n", ts, msg);
    fflush(g_logf);
  }
  pthread_mutex_unlock(&g_log_lock);
}

/* ---------------- paths ---------------- */
const char *path_basename(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

bool path_dirname(const char *p, char *out, size_t outsz) {
  const char *s = strrchr(p, '/');
  if (!s)
    return false;
  size_t n = (size_t)(s - p);
  if (n == 0)
    n = 1; /* "/" */
  if (n >= outsz)
    return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

bool path_join(char *out, size_t outsz, const char *a, const char *b) {
  size_t la = strlen(a);
  int n;
  if (la > 0 && a[la - 1] == '/')
    n = snprintf(out, outsz, "%s%s", a, b);
  else
    n = snprintf(out, outsz, "%s/%s", a, b);
  return n > 0 && (size_t)n < outsz;
}

bool path_is_under(const char *root, const char *p) {
  size_t lr = strlen(root);
  while (lr > 1 && root[lr - 1] == '/')
    lr--;
  if (strncmp(root, p, lr) != 0)
    return false;
  return p[lr] == 0 || p[lr] == '/';
}

bool path_exists(const char *p) {
  struct stat st;
  return lstat(p, &st) == 0;
}

bool is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int mkdir_p(const char *p) {
  char tmp[SS_MAX_PATH];
  if (strlen(p) >= sizeof(tmp))
    return -1;
  strcpy(tmp, p);
  for (char *s = tmp + 1; *s; s++) {
    if (*s == '/') {
      *s = 0;
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
      *s = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

int rm_rf(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode))
    return unlink(p);
  DIR *d = opendir(p);
  if (!d)
    return -1;
  struct dirent *e;
  int rc = 0;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    char c[SS_MAX_PATH];
    if (!path_join(c, sizeof(c), p, e->d_name)) {
      rc = -1;
      continue;
    }
    if (rm_rf(c) != 0)
      rc = -1;
  }
  closedir(d);
  if (rmdir(p) != 0)
    rc = -1;
  return rc;
}

bool ends_with_ci(const char *s, const char *suffix) {
  size_t ls = strlen(s), lf = strlen(suffix);
  if (lf > ls)
    return false;
  return strcasecmp(s + ls - lf, suffix) == 0;
}

uint64_t now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

int read_small_file(const char *path, char *buf, size_t bufsz) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  size_t off = 0;
  while (off + 1 < bufsz) {
    ssize_t n = read(fd, buf + off, bufsz - 1 - off);
    if (n <= 0)
      break;
    off += (size_t)n;
  }
  close(fd);
  buf[off] = 0;
  return (int)off;
}

bool looks_like_title_id(const char *s) {
  /* 4 uppercase letters + 5 digits, e.g. PPSA01234, CUSA12345 */
  if (strlen(s) < 9)
    return false;
  for (int i = 0; i < 4; i++)
    if (!isupper((unsigned char)s[i]))
      return false;
  for (int i = 4; i < 9; i++)
    if (!isdigit((unsigned char)s[i]))
      return false;
  return true;
}

/* Find "key": "value" (first occurrence, after optional start pointer) */
static bool json_find_string(const char *json, const char *key, char *out,
                             size_t outsz) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p)
    return false;
  p += strlen(pat);
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;
  if (*p != ':')
    return false;
  p++;
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;
  if (*p != '"')
    return false;
  p++;
  size_t o = 0;
  while (*p && *p != '"' && o + 1 < outsz) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'u') { /* skip unicode escapes */
        p += 4;
        out[o++] = '?';
        p++;
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = 0;
  return o > 0;
}

bool parse_param_json(const char *json, char *title_id, size_t tidsz,
                      char *title, size_t titlesz) {
  bool ok = json_find_string(json, "titleId", title_id, tidsz);
  if (title && titlesz) {
    title[0] = 0;
    /* prefer en-GB / en-US localized titleName, then default */
    const char *langs[] = {"en-GB", "en-US", NULL};
    for (int i = 0; langs[i] && !title[0]; i++) {
      char pat[32];
      snprintf(pat, sizeof(pat), "\"%s\"", langs[i]);
      const char *p = strstr(json, pat);
      if (p)
        json_find_string(p, "titleName", title, titlesz);
    }
    if (!title[0])
      json_find_string(json, "titleName", title, titlesz);
  }
  return ok;
}
