#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
#endif

/* ------------------------------------------------------------------ */
/* Raw HTTP/1.1 client over a plain TCP socket.                        */
/* Used for the ShadowMount API (127.0.0.1) and plain-http webhooks.   */

static int connect_host(const char *host, int port, int timeout_ms) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
      return -1;
    sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) {
      if (w < 0 && errno == EINTR)
        continue;
      return false;
    }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

int http_request_raw(const char *host, int port, const char *method,
                     const char *path, const char *ctype, const char *body,
                     sbuf_t *resp_body, int timeout_ms) {
  int fd = connect_host(host, port, timeout_ms);
  if (fd < 0)
    return -1;
  size_t blen = body ? strlen(body) : 0;
  sbuf_t req;
  sb_init(&req);
  sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n",
            method, path, host, port);
  sb_puts(&req, "User-Agent: GameLibraryManager/" SS_VERSION "\r\n");
  if (ctype)
    sb_printf(&req, "Content-Type: %s\r\n", ctype);
  sb_printf(&req, "Content-Length: %zu\r\n\r\n", blen);
  if (blen)
    sb_puts(&req, body);
  bool ok = send_all(fd, req.data, req.len);
  sb_free(&req);
  if (!ok) {
    close(fd);
    return -1;
  }
  sbuf_t raw;
  sb_init(&raw);
  char buf[16384];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    buf[n] = 0;
    /* raw bytes may contain NULs in theory; API is JSON so fine */
    sb_puts(&raw, buf);
    if (raw.len > 8 * 1024 * 1024)
      break;
  }
  close(fd);
  int status = -1;
  if (raw.data && !strncmp(raw.data, "HTTP/1.", 7))
    status = atoi(raw.data + 9);
  if (resp_body && raw.data) {
    char *b = strstr(raw.data, "\r\n\r\n");
    if (b) {
      /* handle chunked transfer encoding minimalistically */
      bool chunked = false;
      for (char *h = raw.data; h < b; h++) {
        if (!strncasecmp(h, "Transfer-Encoding: chunked", 26)) {
          chunked = true;
          break;
        }
      }
      b += 4;
      if (!chunked) {
        sb_puts(resp_body, b);
      } else {
        while (*b) {
          long sz = strtol(b, &b, 16);
          if (sz <= 0)
            break;
          b = strstr(b, "\r\n");
          if (!b)
            break;
          b += 2;
          for (long i = 0; i < sz && b[i]; i++)
            sb_putc(resp_body, b[i]);
          b += sz;
          if (!strncmp(b, "\r\n", 2))
            b += 2;
        }
      }
    }
  }
  sb_free(&raw);
  return status;
}

/* Parse http://host[:port]/path */
static bool parse_http_url(const char *url, char *host, size_t hsz, int *port,
                           char *path, size_t psz, bool *https) {
  const char *p;
  if (!strncasecmp(url, "http://", 7)) {
    p = url + 7;
    *https = false;
    *port = 80;
  } else if (!strncasecmp(url, "https://", 8)) {
    p = url + 8;
    *https = true;
    *port = 443;
  } else {
    return false;
  }
  const char *slash = strchr(p, '/');
  size_t hl = slash ? (size_t)(slash - p) : strlen(p);
  if (hl == 0 || hl >= hsz)
    return false;
  memcpy(host, p, hl);
  host[hl] = 0;
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = 0;
    *port = atoi(colon + 1);
  }
  snprintf(path, psz, "%s", slash ? slash : "/");
  return true;
}

/* ------------------------------------------------------------------ */
#ifdef __PROSPERO__

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2AddRequestHeader(int, const char *, const char *, uint32_t);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);

int sceUserServiceInitialize(void *);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);

static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_http_ctx = -1;

void plat_init(void) { sceUserServiceInitialize(0); }

bool plat_open_browser(const char *url) {
  return sceSystemServiceLaunchWebBrowser(url, 0) == 0;
}

void plat_toast(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static bool http2_ready(void) {
  if (g_http_ctx >= 0)
    return true;
  sceNetInit();
  int mem = sceNetPoolCreate("glm", 64 * 1024, 0);
  if (mem < 0)
    return false;
  int ssl = sceSslInit(256 * 1024);
  if (ssl < 0)
    return false;
  int ctx = sceHttp2Init(mem, ssl, 256 * 1024, 1);
  if (ctx < 0)
    return false;
  g_http_ctx = ctx;
  return true;
}

int plat_http_post(const char *url, const char *content_type,
                   const char *body, size_t len) {
  char host[256], path[1024];
  int port;
  bool https;
  if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path),
                      &https))
    return -1;
  if (!https) {
    /* plain http (LAN Home Assistant, self-hosted ntfy): raw socket */
    return http_request_raw(host, port, "POST", path, content_type, body,
                            NULL, 8000);
  }
  pthread_mutex_lock(&g_http_lock);
  int status = -1;
  if (http2_ready()) {
    int tmpl = sceHttp2CreateTemplate(g_http_ctx, "GameLibraryManager/" SS_VERSION,
                                      3, 1);
    if (tmpl >= 0) {
      int req = sceHttp2CreateRequestWithURL(tmpl, "POST", url, len);
      if (req >= 0) {
        if (content_type)
          sceHttp2AddRequestHeader(req, "Content-Type", content_type, 0);
        if (sceHttp2SendRequest(req, body, len) == 0) {
          sceHttp2GetStatusCode(req, &status);
          char sink[512];
          while (sceHttp2ReadData(req, sink, sizeof(sink)) > 0) {
          }
        }
        sceHttp2DeleteRequest(req);
      }
      sceHttp2DeleteTemplate(tmpl);
    }
  }
  pthread_mutex_unlock(&g_http_lock);
  return status;
}

int plat_list_drives(drive_info_t *out, int max) {
  int count = 0;
  for (int i = 0; i < g_cfg.drive_count && count < max; i++) {
    const char *d = g_cfg.drives[i];
    struct statfs sf;
    struct stat st;
    if (statfs(d, &sf) != 0 || stat(d, &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    /* /mnt/extN and /mnt/usbN only count when something is mounted there
     * (otherwise statfs reports the parent filesystem). /data is not its own
     * mount on the PS5 - it lives on the internal /user filesystem - so it
     * is accepted as long as it exists. */
    if (strcmp(sf.f_mntonname, d) != 0 && strcmp(d, "/data") != 0)
      continue;
    snprintf(out[count].fstype, sizeof(out[count].fstype), "%s",
             sf.f_fstypename);
    out[count].total = (uint64_t)sf.f_blocks * sf.f_bsize;
    out[count].avail = (uint64_t)sf.f_bavail * sf.f_bsize;
    snprintf(out[count].mount, sizeof(out[count].mount), "%s", d);
    out[count].dev = (uint64_t)st.st_dev;
    count++;
  }
  return count;
}

#else /* ---------------- host build (Linux) for testing ------------- */

void plat_init(void) {}

bool plat_open_browser(const char *url) {
  printf("[BROWSER] %s\n", url);
  fflush(stdout);
  return true;
}

void plat_toast(const char *msg) { printf("[TOAST] %s\n", msg); }

int plat_http_post(const char *url, const char *content_type,
                   const char *body, size_t len) {
  (void)len;
  char host[256], path[1024];
  int port;
  bool https;
  if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path),
                      &https))
    return -1;
  if (https) {
    printf("[PUSH https not supported on host build] %s : %s\n", url, body);
    return 200;
  }
  return http_request_raw(host, port, "POST", path, content_type, body, NULL,
                          8000);
}

int plat_list_drives(drive_info_t *out, int max) {
  int count = 0;
  for (int i = 0; i < g_cfg.drive_count && count < max; i++) {
    const char *d = g_cfg.drives[i];
    struct stat st;
    struct statvfs vf;
    if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode) || statvfs(d, &vf) != 0)
      continue;
    snprintf(out[count].mount, sizeof(out[count].mount), "%s", d);
    strcpy(out[count].fstype, "host");
    out[count].total = (uint64_t)vf.f_blocks * vf.f_frsize;
    out[count].avail = (uint64_t)vf.f_bavail * vf.f_frsize;
    out[count].dev = (uint64_t)st.st_dev;
    count++;
  }
  return count;
}
#endif
