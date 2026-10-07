/* Home-screen tile: a web-link app that opens the page in the PS5
 * browser. See tile_install(). */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const unsigned char tile_icon[];
extern const size_t tile_icon_len;

static pthread_mutex_t g_tile_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_tile_state[32] = "unknown";
static char g_tile_msg[256] = "";
static char g_tile_dir[SS_MAX_PATH] = "";

static void set_state(const char *st, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void set_state(const char *st, const char *fmt, ...) {
  pthread_mutex_lock(&g_tile_lock);
  snprintf(g_tile_state, sizeof(g_tile_state), "%s", st);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_tile_msg, sizeof(g_tile_msg), fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&g_tile_lock);
  ss_log("tile: %s - %s", st, g_tile_msg);
}

/* Called after each scan: if the tile was waiting because ShadowMount
 * wasn't up yet (e.g. Game Library Manager loaded first at boot), try again. */
void tile_retry_if_waiting(bool smp_ok) {
  static bool busy = false;
  pthread_mutex_lock(&g_tile_lock);
  bool waiting = !strcmp(g_tile_state, "waiting_smp");
  bool go = waiting && smp_ok && !busy;
  if (go)
    busy = true;
  pthread_mutex_unlock(&g_tile_lock);
  if (go) {
    tile_install(false);
    busy = false;
  }
}

void tile_json(sbuf_t *b) {
  pthread_mutex_lock(&g_tile_lock);
  sb_printf(b, "{\"title_id\":\"%s\",\"state\":", "GLMG00001");
  sb_json_str(b, g_tile_state);
  sb_puts(b, ",\"message\":");
  sb_json_str(b, g_tile_msg);
  sb_puts(b, ",\"path\":");
  sb_json_str(b, g_tile_dir);
  sb_printf(b, ",\"enabled\":%s}", g_cfg.tile ? "true" : "false");
  pthread_mutex_unlock(&g_tile_lock);
}

/* write via temp file + rename so a half-written file never exists */
static int write_file(const char *path, const void *data, size_t len,
                      mode_t mode) {
  char tmp[SS_MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return -1;
  const unsigned char *p = data;
  size_t left = len;
  while (left) {
    ssize_t w = write(fd, p, left);
    if (w <= 0) {
      close(fd);
      unlink(tmp);
      return -1;
    }
    p += w;
    left -= (size_t)w;
  }
  if (close(fd) != 0 || rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void *tile_thread(void *arg) {
  bool force = arg != NULL;
  tile_install(force);
  return NULL;
}

void tile_install_async(bool force) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, tile_thread, force ? (void *)1 : NULL);
  pthread_attr_destroy(&a);
}

/* The tile is a "web link" app, the same kind Payload Manager, Web File
 * Manager and ShadowMount register: /user/app/<ID>/sce_sys with a
 * param.json whose deeplinkUri points at our page. No eboot, no
 * ShadowMount involvement - pressing it just opens the PS5 browser.
 * (Versions up to 0.3.2 used an eboot-based tile, FAKE57417, which the
 * console rejected with "cannot open app"; it is removed here.) */
#define TILE_APP_ID "GLMG00001"
#define OLD_TILE_ID "FAKE57417"

#ifdef __PROSPERO__
#include <ps5/kernel.h>
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

static int register_title(const char *id) {
  static bool init = false;
  if (!init) {
    int e = sceAppInstUtilInitialize();
    if (e)
      ss_log("tile: sceAppInstUtilInitialize 0x%08X", (unsigned)e);
    init = true;
  }
  int (*install_dir)(const char *, const char *, void *) = 0;
  uint32_t handle;
  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
    install_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
  if (install_dir)
    return install_dir(id, "/user/app/", 0);
  return sceAppInstUtilAppInstallAll(0);
}
static int unregister_title(const char *id) {
  return sceAppInstUtilAppUnInstall(id);
}
#define APP_BASE_DIR "/user/app"
#else
static int register_title(const char *id) {
  ss_log("tile: (host) would register %s", id);
  return 0;
}
static int unregister_title(const char *id) {
  ss_log("tile: (host) would uninstall %s", id);
  return 0;
}
#define APP_BASE_DIR "/tmp/glm-host-user-app"
#endif

static void remove_old_tile(void) {
  /* old eboot tile folder that ShadowMount registered */
  char old[SS_MAX_PATH];
  snprintf(old, sizeof(old), "%s/%s", g_cfg.tile_root, OLD_TILE_ID);
  char marker[SS_MAX_PATH];
  snprintf(marker, sizeof(marker), "%s/glm.version", old);
  char marker2[SS_MAX_PATH];
  snprintf(marker2, sizeof(marker2), "%s/shadowsort.version", old);
  if (is_file(marker) || is_file(marker2)) {
    int r = unregister_title(OLD_TILE_ID);
    ss_log("tile: removing old launcher %s (uninstall 0x%08X)", old,
           (unsigned)r);
    rm_rf(old);
  }
}

bool tile_install(bool force) {
  if (!g_cfg.tile) {
    set_state("disabled", "Home tile switched off (tile=0 in config.ini).");
    return false;
  }
  remove_old_tile();

  char dir[SS_MAX_PATH], sys[SS_MAX_PATH], pj[SS_MAX_PATH], ic[SS_MAX_PATH];
  snprintf(dir, sizeof(dir), "%s/%s", APP_BASE_DIR, TILE_APP_ID);
  snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
  snprintf(pj, sizeof(pj), "%s/param.json", sys);
  snprintf(ic, sizeof(ic), "%s/icon0.png", sys);
  pthread_mutex_lock(&g_tile_lock);
  snprintf(g_tile_dir, sizeof(g_tile_dir), "%s", dir);
  pthread_mutex_unlock(&g_tile_lock);

  char param[768];
  snprintf(param, sizeof(param),
           "{\n"
           "  \"titleId\": \"%s\",\n"
           "  \"applicationCategoryType\": 65536,\n"
           "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
           "  \"localizedParameters\": {\n"
           "    \"defaultLanguage\": \"en-US\",\n"
           "    \"en-US\": { \"titleName\": \"Game Library Manager\" },\n"
           "    \"en-GB\": { \"titleName\": \"Game Library Manager\" }\n"
           "  }\n"
           "}\n",
           TILE_APP_ID, g_cfg.http_port);

  char cur[1024];
  bool same = read_small_file(pj, cur, sizeof(cur)) > 0 && !strcmp(cur, param) &&
              is_file(ic);
  if (same && !force) {
    set_state("installed", "On your home screen as \"Game Library Manager\".");
    return true;
  }
  if (mkdir_p(sys) != 0) {
    set_state("error", "Can't create %s: %s", sys, strerror(errno));
    return false;
  }
  if (write_file(pj, param, strlen(param), 0644) ||
      write_file(ic, tile_icon, tile_icon_len, 0644)) {
    set_state("error", "Couldn't write the tile files in %s: %s", dir,
              strerror(errno));
    return false;
  }
  int r = register_title(TILE_APP_ID);
  if (r != 0 && (uint32_t)r != 0x80990002u) {
    set_state("error", "The console refused to add the tile (0x%08X).",
              (unsigned)r);
    return false;
  }
  set_state("installed", "On your home screen as \"Game Library Manager\".");
  return true;
}

/* ---- /api/open: bring the page up in the PS5 browser ---- */
static void *open_thread(void *arg) {
  (void)arg;
  /* give the tile's app time to close so the browser isn't covered */
  usleep(1500 * 1000);
  char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_cfg.http_port);
  if (!plat_open_browser(url))
    plat_toast("Game Library Manager: couldn't open the browser.");
  return NULL;
}

void tile_open_page_async(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, open_thread, NULL);
  pthread_attr_destroy(&a);
}
