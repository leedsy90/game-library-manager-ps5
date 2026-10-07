/* ShadowMount Plus integration: scan model (paths/depth) and API client. */
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Default roots copied from ShadowMount Plus (include/sm_paths.h). */
static const char *k_default_roots[] = {
    "/data/homebrew",         "/data/etaHEN/games",
    "/mnt/ext0/homebrew",     "/mnt/ext0/etaHEN/games",
    "/mnt/ext1/homebrew",     "/mnt/ext1/etaHEN/games",
    "/mnt/usb0/homebrew",     "/mnt/usb1/homebrew",
    "/mnt/usb2/homebrew",     "/mnt/usb3/homebrew",
    "/mnt/usb4/homebrew",     "/mnt/usb5/homebrew",
    "/mnt/usb6/homebrew",     "/mnt/usb7/homebrew",
    "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games",
    "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
    "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games",
    "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",
    "/mnt/usb0",              "/mnt/usb1",
    "/mnt/usb2",              "/mnt/usb3",
    "/mnt/usb4",              "/mnt/usb5",
    "/mnt/usb6",              "/mnt/usb7",
    "/mnt/ext0",              "/mnt/ext1",
    NULL};

static char *trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = 0;
  return s;
}

void smp_model_load(smp_model_t *m) {
  memset(m, 0, sizeof(*m));
  m->depth = 1;
  FILE *f = fopen(g_cfg.smp_config, "r");
  if (f) {
    m->from_cfg = true;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      char *s = trim(line);
      if (!*s || *s == '#' || *s == ';')
        continue;
      char *eq = strchr(s, '=');
      if (!eq)
        continue;
      *eq = 0;
      char *k = trim(s), *v = trim(eq + 1);
      if (!strcasecmp(k, "scan_depth")) {
        int d = atoi(v);
        m->depth = d >= 2 ? 2 : 1;
      } else if (!strcasecmp(k, "recursive_scan")) {
        if (atoi(v) == 1)
          m->depth = 2;
      } else if (!strcasecmp(k, "scanpath")) {
        if (m->count < SS_MAX_ROOTS && *v == '/') {
          size_t n = strlen(v);
          while (n > 1 && v[n - 1] == '/')
            v[--n] = 0;
          snprintf(m->roots[m->count++], SS_MAX_PATH, "%s", v);
        }
      }
    }
    fclose(f);
  }
  if (m->count == 0) {
#ifdef __PROSPERO__
    for (int i = 0; k_default_roots[i] && m->count < SS_MAX_ROOTS; i++)
      snprintf(m->roots[m->count++], SS_MAX_PATH, "%s", k_default_roots[i]);
#else
    /* host build: derive defaults from the configured test drives */
    (void)k_default_roots;
    for (int i = 0; i < g_cfg.drive_count; i++) {
      snprintf(m->roots[m->count++], SS_MAX_PATH, "%s/homebrew",
               g_cfg.drives[i]);
      if (i > 0) /* everything except the "internal" drive is also a root */
        snprintf(m->roots[m->count++], SS_MAX_PATH, "%s", g_cfg.drives[i]);
    }
#endif
  }
}

/* ------------------------------------------------------------------ */
/* Tiny JSON helpers (enough for SMP's flat responses)                 */

static const char *skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p))
    p++;
  return p;
}

/* Return pointer just past the JSON value starting at p. */
static const char *skip_value(const char *p) {
  p = skip_ws(p);
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1])
        p++;
      p++;
    }
    return *p ? p + 1 : p;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = skip_value(p);
        continue;
      }
      if (*p == '{' || *p == '[')
        depth++;
      else if (*p == '}' || *p == ']') {
        depth--;
        if (depth == 0)
          return p + 1;
      }
      p++;
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']')
    p++;
  return p;
}

/* Find the value for key in the object starting at obj ('{'), top level
 * only. Returns pointer to value or NULL. */
static const char *obj_get(const char *obj, const char *key) {
  const char *p = skip_ws(obj);
  if (*p != '{')
    return NULL;
  p++;
  size_t kl = strlen(key);
  for (;;) {
    p = skip_ws(p);
    if (*p != '"')
      return NULL;
    const char *ks = p + 1;
    const char *ke = skip_value(p) - 1;
    p = skip_ws(ke + 1);
    if (*p != ':')
      return NULL;
    p = skip_ws(p + 1);
    if ((size_t)(ke - ks) == kl && !strncmp(ks, key, kl))
      return p;
    p = skip_ws(skip_value(p));
    if (*p == ',') {
      p++;
      continue;
    }
    return NULL;
  }
}

static bool obj_str(const char *obj, const char *key, char *out, size_t sz) {
  out[0] = 0;
  const char *v = obj_get(obj, key);
  if (!v || *v != '"')
    return false;
  v++;
  size_t o = 0;
  while (*v && *v != '"' && o + 1 < sz) {
    if (*v == '\\' && v[1]) {
      v++;
      char c = *v;
      if (c == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c == 'u') {
        v += 4;
        c = '?';
      }
      out[o++] = c;
      v++;
      continue;
    }
    out[o++] = *v++;
  }
  out[o] = 0;
  return true;
}

static bool obj_bool(const char *obj, const char *key) {
  const char *v = obj_get(obj, key);
  return v && !strncmp(v, "true", 4);
}

static long long obj_int(const char *obj, const char *key, long long def) {
  const char *v = obj_get(obj, key);
  if (!v || (!isdigit((unsigned char)*v) && *v != '-'))
    return def;
  return strtoll(v, NULL, 10);
}

/* ------------------------------------------------------------------ */
static int api_post(const char *route, const char *body, sbuf_t *out,
                    int timeout_ms) {
  return http_request_raw(g_cfg.smp_host, g_cfg.smp_port, "POST", route,
                          "application/json", body ? body : "{}", out,
                          timeout_ms);
}

#define SMP_MIN_MAJOR 1
#define SMP_MIN_MINOR 7

const char *smp_status_name(smp_status_t s) {
  switch (s) {
  case SMP_OK:
    return "ok";
  case SMP_NOT_INSTALLED:
    return "not_installed";
  case SMP_API_DISABLED:
    return "api_disabled";
  case SMP_NO_API:
    return "no_api";
  case SMP_TOO_OLD:
    return "too_old";
  case SMP_MISSING_FEATURES:
    return "missing_features";
  }
  return "?";
}

/* Read api_enabled / api_port from SMP's own config so we talk to the
 * port the user actually configured. */
static bool read_smp_api_cfg(int *port) {
  bool enabled = true;
  FILE *f = fopen(g_cfg.smp_config, "r");
  if (!f)
    return enabled;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *t = trim(line);
    if (*t == '#' || *t == ';')
      continue;
    char *eq = strchr(t, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *k = trim(t), *v = trim(eq + 1);
    if (!strcasecmp(k, "api_enabled"))
      enabled = atoi(v) != 0;
    else if (!strcasecmp(k, "api_port") && atoi(v) > 0)
      *port = atoi(v);
  }
  fclose(f);
  return enabled;
}

smp_status_t smp_check(char *version, size_t vsz, char *msg, size_t msgsz) {
  version[0] = 0;
  char dir[SS_MAX_PATH];
  if (!path_dirname(g_cfg.smp_config, dir, sizeof(dir)))
    dir[0] = 0;
  bool installed = is_dir(dir) || is_file(g_cfg.smp_config);

  int port = g_cfg.smp_port;
  bool enabled = read_smp_api_cfg(&port);
  g_cfg.smp_port = port;

  sbuf_t b;
  sb_init(&b);
  int st = api_post("/api/v1/version", "{}", &b, 2000);
  bool answered = st == 200 && b.data;
  smp_status_t res;
  if (!answered) {
    if (!installed) {
      res = SMP_NOT_INSTALLED;
      snprintf(msg, msgsz,
               "ShadowMount Plus doesn't seem to be installed (no %s). "
               "Game Library Manager can still find and move games, but nothing will "
               "be mounted until SMP is loaded.",
               dir);
    } else if (!enabled) {
      res = SMP_API_DISABLED;
      snprintf(msg, msgsz,
               "SMP's API is switched off (api_enabled=0 in %s). Set it to 1 "
               "to turn on 'does SMP see it?' checks.",
               g_cfg.smp_config);
    } else {
      res = SMP_NO_API;
      snprintf(msg, msgsz,
               "Can't reach ShadowMount Plus on port %d. Either SMP isn't "
               "loaded yet, or it's older than %d.%d (the first version with "
               "an API). Moving and fixing still work; the 'SMP sees it' "
               "checks are off.",
               port, SMP_MIN_MAJOR, SMP_MIN_MINOR);
    }
    sb_free(&b);
    return res;
  }

  obj_str(b.data, "shadowmount_version", version, vsz);
  int maj = 0, min = 0;
  sscanf(version, "%d.%d", &maj, &min);
  const char *need[] = {"list_games", "rescan", "delete_game_source",
                        "add_manual_source", NULL};
  char missing[128] = "";
  for (int i = 0; need[i]; i++) {
    if (!strstr(b.data, need[i])) {
      size_t l = strlen(missing);
      snprintf(missing + l, sizeof(missing) - l, "%s%s", l ? ", " : "",
               need[i]);
    }
  }
  if (maj < SMP_MIN_MAJOR || (maj == SMP_MIN_MAJOR && min < SMP_MIN_MINOR)) {
    res = SMP_TOO_OLD;
    snprintf(msg, msgsz,
             "ShadowMount Plus %s is too old - Game Library Manager needs %d.%d or "
             "newer for its checks. Update SMP.",
             version[0] ? version : "(unknown)", SMP_MIN_MAJOR, SMP_MIN_MINOR);
  } else if (missing[0]) {
    res = SMP_MISSING_FEATURES;
    snprintf(msg, msgsz,
             "ShadowMount Plus %s answers but is missing: %s. Update SMP to "
             "the latest 1.7 build.",
             version, missing);
  } else {
    res = SMP_OK;
    snprintf(msg, msgsz, "ShadowMount Plus %s connected.", version);
  }
  sb_free(&b);
  return res;
}

bool smp_api_available(char *version_out, size_t vsz) {
  char v[64], m[256];
  smp_status_t s = smp_check(v, sizeof(v), m, sizeof(m));
  if (version_out && vsz)
    snprintf(version_out, vsz, "%s", v);
  return s == SMP_OK;
}

int smp_api_games(smp_game_t *out, int max) {
  sbuf_t b;
  sb_init(&b);
  int st = api_post("/api/v1/games", "{\"include_size\":false}", &b, 15000);
  if (st != 200 || !b.data) {
    sb_free(&b);
    return -1;
  }
  int n = 0;
  const char *arr = obj_get(b.data, "games");
  if (arr && *arr == '[') {
    const char *p = skip_ws(arr + 1);
    while (*p == '{' && n < max) {
      const char *end = skip_value(p);
      size_t len = (size_t)(end - p);
      char *item = malloc(len + 1);
      if (!item)
        break;
      memcpy(item, p, len);
      item[len] = 0;
      smp_game_t *g = &out[n];
      memset(g, 0, sizeof(*g));
      obj_str(item, "title_id", g->title_id, sizeof(g->title_id));
      obj_str(item, "path", g->path, sizeof(g->path));
      obj_str(item, "title_name", g->title_name, sizeof(g->title_name));
      obj_str(item, "source_type", g->source_type, sizeof(g->source_type));
      g->mounted = obj_bool(item, "mounted");
      g->installed = obj_bool(item, "installed");
      g->managed = obj_bool(item, "managed");
      g->source_available = obj_bool(item, "source_available");
      free(item);
      n++;
      p = skip_ws(end);
      if (*p == ',')
        p = skip_ws(p + 1);
    }
  }
  sb_free(&b);
  return n;
}

bool smp_api_scan_ex(char *deferred_reason, size_t rsz) {
  sbuf_t b;
  sb_init(&b);
  int st = api_post("/api/v1/scan", "{\"reset_attempts\":true}", &b, 5000);
  if (deferred_reason && rsz) {
    deferred_reason[0] = 0;
    if (b.data && obj_bool(b.data, "scan_deferred"))
      obj_str(b.data, "scan_deferred_reason", deferred_reason, rsz);
  }
  sb_free(&b);
  return st == 200;
}

bool smp_api_scan(void) { return smp_api_scan_ex(NULL, 0); }

bool smp_api_delete(const char *title_id, char *err, size_t errsz,
                    int *job_id) {
  char body[128];
  snprintf(body, sizeof(body), "{\"title_id\":\"%s\",\"confirm\":true}",
           title_id);
  sbuf_t b;
  sb_init(&b);
  int st = api_post("/api/v1/games/delete", body, &b, 10000);
  bool ok = false;
  *job_id = 0;
  if (b.data) {
    if (st == 202) {
      *job_id = (int)obj_int(b.data, "job_id", 0);
      ok = true;
    } else if (st == 200 && obj_bool(b.data, "source_missing")) {
      ok = true; /* already gone */
    } else {
      obj_str(b.data, "error", err, errsz);
    }
  } else {
    snprintf(err, errsz, "ShadowMount API not reachable");
  }
  sb_free(&b);
  return ok;
}

bool smp_api_job_status(int job_id, char *phase, size_t psz, int *result,
                        bool *finished) {
  char body[64];
  snprintf(body, sizeof(body), "{\"job_id\":%d}", job_id);
  sbuf_t b;
  sb_init(&b);
  int st = api_post("/api/v1/games/storage/status", body, &b, 5000);
  bool ok = st == 200 && b.data;
  if (ok) {
    obj_str(b.data, "state", phase, psz);
    *result = (int)obj_int(b.data, "result_status", 0);
    *finished = !strcmp(phase, "completed") || !strcmp(phase, "failed") ||
                !strcmp(phase, "cancelled");
  }
  sb_free(&b);
  return ok;
}

bool smp_api_manual_add(const char *path) {
  sbuf_t body, b;
  sb_init(&body);
  sb_init(&b);
  sb_puts(&body, "{\"path\":");
  sb_json_str(&body, path);
  sb_puts(&body, "}");
  int st = api_post("/api/v1/manual/add", body.data, &b, 5000);
  sb_free(&body);
  sb_free(&b);
  return st == 200;
}

/* ------------------------------------------------------------------ */
/* Notifications                                                       */

static void url_encode(sbuf_t *b, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
      sb_putc(b, (char)*p);
    else
      sb_printf(b, "%%%02X", *p);
  }
}

void notify_event(const char *title, const char *msg, bool push) {
  ss_log("%s: %s", title, msg);
  if (g_cfg.toasts) {
    char t[512];
    snprintf(t, sizeof(t), "Game Library Manager: %s\n%s", title, msg);
    plat_toast(t);
  }
  if (!push)
    return;
  if (g_cfg.ntfy_url[0]) {
    /* ntfy accepts the title as a query parameter, so no custom headers
     * are needed; the body is the message. */
    sbuf_t u;
    sb_init(&u);
    sb_puts(&u, g_cfg.ntfy_url);
    sb_puts(&u, strchr(g_cfg.ntfy_url, '?') ? "&title=" : "?title=");
    url_encode(&u, title);
    sb_puts(&u, "&tags=video_game");
    int st = plat_http_post(u.data, "text/plain", msg, strlen(msg));
    if (st < 200 || st >= 300)
      ss_log("ntfy push failed (status %d)", st);
    sb_free(&u);
  }
  if (g_cfg.webhook_url[0]) {
    sbuf_t j;
    sb_init(&j);
    sb_puts(&j, "{\"source\":\"game-library-manager\",\"title\":");
    sb_json_str(&j, title);
    sb_puts(&j, ",\"message\":");
    sb_json_str(&j, msg);
    sb_puts(&j, "}");
    int st = plat_http_post(g_cfg.webhook_url, "application/json", j.data,
                            j.len);
    if (st < 200 || st >= 300)
      ss_log("webhook push failed (status %d)", st);
    sb_free(&j);
  }
}

/* Edit scan_depth in SMP's own config.ini, keeping every other line as is.
 * ShadowMount watches the file and reloads it without a restart. */
bool smp_set_scan_depth(int depth, char *err, size_t errsz) {
  if (depth != 1 && depth != 2) {
    snprintf(err, errsz, "scan depth must be 1 or 2");
    return false;
  }
  sbuf_t out;
  sb_init(&out);
  bool done = false;
  FILE *f = fopen(g_cfg.smp_config, "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      char tmp[1024];
      snprintf(tmp, sizeof(tmp), "%s", line);
      char *t = trim(tmp);
      char *eq = strchr(t, '=');
      if (*t != '#' && *t != ';' && eq) {
        *eq = 0;
        char *k = trim(t);
        if (!strcasecmp(k, "scan_depth")) {
          if (!done)
            sb_printf(&out, "scan_depth=%d\n", depth);
          done = true;
          continue;
        }
        if (!strcasecmp(k, "recursive_scan")) {
          /* deprecated key that forces depth 2; drop it */
          continue;
        }
      }
      sb_puts(&out, line);
      if (line[0] && line[strlen(line) - 1] != '\n')
        sb_putc(&out, '\n');
    }
    fclose(f);
  }
  if (!done)
    sb_printf(&out, "scan_depth=%d\n", depth);
  char tmpp[SS_MAX_PATH];
  snprintf(tmpp, sizeof(tmpp), "%s.glm-tmp", g_cfg.smp_config);
  FILE *w = fopen(tmpp, "w");
  bool ok = w && fwrite(out.data, 1, out.len, w) == out.len;
  if (w && fclose(w) != 0)
    ok = false;
  if (ok)
    ok = rename(tmpp, g_cfg.smp_config) == 0;
  if (!ok) {
    snprintf(err, errsz, "couldn't write %s: %s", g_cfg.smp_config,
             strerror(errno));
    unlink(tmpp);
  } else {
    ss_log("set scan_depth=%d in %s", depth, g_cfg.smp_config);
  }
  sb_free(&out);
  return ok;
}
