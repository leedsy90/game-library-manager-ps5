/* Scanner: walks every drive looking for game folders and images, then
 * decides whether ShadowMount Plus will find each one where it is, and
 * if not, why not and where it should go. Mirrors SMP's own rules:
 *   - a folder game is <dir>/sce_sys/param.json
 *   - with scan_depth=1 it must be a direct child of a scan root
 *   - with scan_depth=2 it may also be a grandchild
 *   - image files follow the same positions
 *   - names starting with '.' are ignored, <root>/backports is ignored
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

scan_state_t g_scan = {.lock = PTHREAD_MUTEX_INITIALIZER};

const char *issue_name(issue_t i) {
  switch (i) {
  case ISSUE_NONE:
    return "ok";
  case ISSUE_TOO_DEEP:
    return "too_deep";
  case ISSUE_WRAPPED_FOLDER:
    return "wrapped_folder";
  case ISSUE_OUTSIDE_ROOTS:
    return "outside_roots";
  case ISSUE_IMAGE_WRAPPED:
    return "image_wrapped";
  case ISSUE_IMAGE_NO_GAME:
    return "image_no_game";
  case ISSUE_DUPLICATE:
    return "duplicate";
  case ISSUE_BACKPORTS_PLACE:
    return "in_backports";
  case ISSUE_HIDDEN_NAME:
    return "hidden_name";
  case ISSUE_ARCHIVE:
    return "archive";
  case ISSUE_PKG:
    return "pkg";
  }
  return "?";
}

typedef struct {
  game_t *games;
  int count;
  const smp_model_t *model;
  const drive_info_t *drive;
} walk_ctx_t;

static bool is_root(const smp_model_t *m, const char *p) {
  for (int i = 0; i < m->count; i++)
    if (!strcmp(m->roots[i], p))
      return true;
  return false;
}

/* Would SMP see an item (folder game or image) at path p? */
static bool smp_would_see(const smp_model_t *m, const char *p) {
  if (path_basename(p)[0] == '.')
    return false;
  char parent[SS_MAX_PATH], grand[SS_MAX_PATH];
  if (!path_dirname(p, parent, sizeof(parent)))
    return false;
  if (is_root(m, parent))
    return true;
  if (m->depth >= 2 && path_dirname(parent, grand, sizeof(grand)) &&
      is_root(m, grand)) {
    const char *pb = path_basename(parent);
    if (pb[0] == '.' || !strcmp(pb, "backports"))
      return false;
    return true;
  }
  return false;
}

bool best_root_on_drive(const char *drive, char *out, size_t outsz) {
  const smp_model_t *m = &g_scan.model;
  char pref[SS_MAX_PATH];
  snprintf(pref, sizeof(pref), "%s/homebrew", drive);
  if (is_root(m, pref)) {
    snprintf(out, outsz, "%s", pref);
    return true;
  }
  for (int i = 0; i < m->count; i++) {
    if (path_is_under(drive, m->roots[i])) {
      snprintf(out, outsz, "%s", m->roots[i]);
      return true;
    }
  }
  return false;
}

static void list_dir_names(const char *dir, char *out, size_t outsz) {
  out[0] = 0;
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  size_t used = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    int n = snprintf(out + used, outsz - used, "%s%s", used ? ", " : "",
                     e->d_name);
    if (n < 0 || (size_t)n >= outsz - used) {
      if (outsz > 4)
        strcpy(out + outsz - 4, "...");
      break;
    }
    used += (size_t)n;
  }
  closedir(d);
}

/* Save library in a fakelib folder? Folders get "<name>.disabled" when the
 * fix is applied; inside .exfat images (renamed in place, same length)
 * ".sprx" becomes ".soff". */
bool is_save_lib(const char *name, bool *disabled) {
  if (strncasecmp(name, "libSceSaveData", 14))
    return false;
  if (ends_with_ci(name, ".soff")) {
    *disabled = true;
    return true;
  }
  *disabled = ends_with_ci(name, ".disabled");
  char base[256];
  snprintf(base, sizeof(base), "%s", name);
  if (*disabled)
    base[strlen(base) - 9] = 0;
  return ends_with_ci(base, ".sprx") || ends_with_ci(base, ".prx");
}

/* fakelib inside an .exfat/.ffpkg image */
static void image_fakelib(game_t *g) {
  char list[2048];
  if (image_list_dir(g->path, "fakelib", list, sizeof(list)) <= 0)
    return;
  g->has_fakelib = true;
  /* list is "name(size), name(size)"; keep names only for the UI */
  size_t o = 0;
  int active = 0, off = 0;
  char *save = NULL;
  for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
    while (*t == ' ')
      t++;
    char *paren = strrchr(t, '(');
    if (paren)
      *paren = 0;
    bool dis;
    if (is_save_lib(t, &dis))
      dis ? off++ : active++;
    int w = snprintf(g->fakelib_files + o, sizeof(g->fakelib_files) - o,
                     "%s%s", o ? ", " : "", t);
    if (w < 0 || (size_t)w >= sizeof(g->fakelib_files) - o)
      break;
    o += (size_t)w;
  }
  g->savefix = active ? 1 : off ? 2 : 0;
}

int savefix_state(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  int active = 0, off = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    bool dis;
    if (is_save_lib(e->d_name, &dis)) {
      if (dis)
        off++;
      else
        active++;
    }
  }
  closedir(d);
  return active ? 1 : off ? 2 : 0;
}

int savefix_apply(const char *dir, bool undo, char *err, size_t errsz) {
  DIR *d = opendir(dir);
  if (!d) {
    snprintf(err, errsz, "no fakelib folder");
    return -1;
  }
  char names[32][256];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < 32) {
    bool dis;
    if (is_save_lib(e->d_name, &dis) && dis == undo)
      snprintf(names[n++], 256, "%s", e->d_name);
  }
  closedir(d);
  int changed = 0;
  for (int i = 0; i < n; i++) {
    char src[SS_MAX_PATH], dst[SS_MAX_PATH];
    path_join(src, sizeof(src), dir, names[i]);
    snprintf(dst, sizeof(dst), "%s", src);
    if (undo)
      dst[strlen(dst) - 9] = 0;
    else
      strncat(dst, ".disabled", sizeof(dst) - strlen(dst) - 1);
    if (path_exists(dst)) {
      snprintf(err, errsz, "%s already exists", path_basename(dst));
      continue;
    }
    if (rename(src, dst) == 0) {
      ss_log("save fix %s: %s", undo ? "undone" : "applied", dst);
      changed++;
    } else {
      snprintf(err, errsz, "%s: %s", names[i], strerror(errno));
    }
  }
  return changed;
}

static game_t *new_game(walk_ctx_t *c, const char *path, const char *kind) {
  if (c->count >= SS_MAX_GAMES)
    return NULL;
  game_t *g = &c->games[c->count];
  memset(g, 0, sizeof(*g));
  g->id = c->count;
  snprintf(g->path, sizeof(g->path), "%s", path);
  snprintf(g->kind, sizeof(g->kind), "%s", kind);
  snprintf(g->drive, sizeof(g->drive), "%s", c->drive->mount);
  g->dev = c->drive->dev;
  c->count++;
  return g;
}

static void add_folder_game(walk_ctx_t *c, const char *path) {
  game_t *g = new_game(c, path, "folder");
  if (!g)
    return;
  char pj[SS_MAX_PATH], buf[64 * 1024];
  snprintf(pj, sizeof(pj), "%s/sce_sys/param.json", path);
  if (read_small_file(pj, buf, sizeof(buf)) > 0)
    parse_param_json(buf, g->title_id, sizeof(g->title_id), g->title,
                     sizeof(g->title));
  /* sizes are measured after the scan (can be slow on USB/exFAT) */
  ss_log("  folder game %s (%s)", path, g->title_id);
  g_scan.found++;

  char fl[SS_MAX_PATH];
  snprintf(fl, sizeof(fl), "%s/fakelib", path);
  if (is_dir(fl)) {
    g->has_fakelib = true;
    list_dir_names(fl, g->fakelib_files, sizeof(g->fakelib_files));
    g->savefix = savefix_state(fl);
  }
}

static void add_image(walk_ctx_t *c, const char *path, const char *kind) {
  game_t *g = new_game(c, path, kind);
  if (!g)
    return;
  struct stat st;
  if (stat(path, &st) == 0)
    g->size = (uint64_t)st.st_size;
  g->files = 1;
  g->size_state = 1;
  char wrapper[256];
  snprintf(g_scan.current, sizeof(g_scan.current), "reading image %s", path);
  uint64_t t0 = now_ms();
  g_scan.found++;
  peek_result_t pr = image_peek(path, g->title_id, sizeof(g->title_id),
                                g->title, sizeof(g->title), wrapper,
                                sizeof(wrapper));
  ss_log("  image %s: peek=%d title=%s in %llu ms", path, (int)pr, g->title_id,
         (unsigned long long)(now_ms() - t0));
  if (pr == PEEK_OK)
    image_fakelib(g);
  if (pr == PEEK_WRAPPED) {
    g->issue = ISSUE_IMAGE_WRAPPED;
    snprintf(g->detail, sizeof(g->detail),
             "Game files are inside an extra folder '%s' in the image. "
             "SMP needs sce_sys at the image root - rebuild the image from "
             "inside that folder.",
             wrapper);
  } else if (pr == PEEK_NO_GAME) {
    g->issue = ISSUE_IMAGE_NO_GAME;
    snprintf(g->detail, sizeof(g->detail),
             "No sce_sys/param.json found in the image root or one level "
             "down.");
  } else if (pr == PEEK_ERROR) {
    snprintf(g->detail, sizeof(g->detail),
             "Couldn't read the image's filesystem (wrong extension or "
             "damaged?).");
  }
  if (!g->title_id[0]) {
    /* fall back to a title id in the file name */
    const char *b = path_basename(path);
    for (const char *s = b; *s; s++) {
      if (looks_like_title_id(s)) {
        snprintf(g->title_id, 10, "%.9s", s);
        break;
      }
    }
  }
}

/* first title id (AAAA00000) in a string */
static bool find_title_id(const char *s, char *out) {
  for (; *s; s++)
    if (looks_like_title_id(s)) {
      snprintf(out, 10, "%.9s", s);
      return true;
    }
  return false;
}

/* Things that look like a game but that ShadowMount can't load:
 * installer packages and games still inside an archive. */
static void add_other(walk_ctx_t *c, const char *path, const char *dir,
                      const char *name, uint64_t size) {
  bool pkg = ends_with_ci(name, ".pkg");
  bool arc = ends_with_ci(name, ".7z") || ends_with_ci(name, ".7z.001") ||
             ends_with_ci(name, ".rar") || ends_with_ci(name, ".part1.rar") ||
             ends_with_ci(name, ".part01.rar") || ends_with_ci(name, ".zip");
  if (!pkg && !arc)
    return;
  /* second+ volumes of split archives are skipped */
  char tid[16] = "";
  if (!find_title_id(name, tid) && !find_title_id(dir, tid))
    return;
  if (arc && size < 512ull * 1024 * 1024)
    return; /* small archives are fixes/backports, not games */
  if (pkg && size < 256ull * 1024 * 1024)
    return; /* DLC / small add-on packages */
  game_t *g = new_game(c, path, pkg ? "pkg" : "archive");
  if (!g)
    return;
  snprintf(g->title_id, sizeof(g->title_id), "%s", tid);
  g->size = size;
  g->files = 1;
  g->size_state = 1;
  g->issue = pkg ? ISSUE_PKG : ISSUE_ARCHIVE;
  if (pkg)
    snprintf(g->detail, sizeof(g->detail),
             "Installer package (.pkg). ShadowMount doesn't load these - "
             "install it with your PKG installer (e.g. PKG Manager / etaHEN). "
             "Check if convertible tells you whether it could ever become a "
             "folder.");
  else
    snprintf(g->detail, sizeof(g->detail),
             "Game is still packed in an archive, so ShadowMount can't see it. "
             "Press Extract... to unpack it onto a drive.");
  ss_log("  %s %s (%s)", pkg ? "package" : "archive", path, tid);
  g_scan.found++;
}

static void walk(walk_ctx_t *c, const char *dir, int depth) {
  if (depth > g_cfg.walk_depth)
    return;
  g_scan.dirs_seen++;
  snprintf(g_scan.current, sizeof(g_scan.current), "%s", dir);
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    /* our own staging area and other hidden stuff */
    if (e->d_name[0] == '.' && !strncmp(e->d_name, ".glm-", 5))
      continue;
    char p[SS_MAX_PATH];
    if (!path_join(p, sizeof(p), dir, e->d_name))
      continue;
    struct stat st;
    if (lstat(p, &st) != 0)
      continue;
    if (S_ISREG(st.st_mode)) {
      const char *k = image_kind(e->d_name);
      if (k)
        add_image(c, p, k);
      else
        add_other(c, p, dir, e->d_name, (uint64_t)st.st_size);
      continue;
    }
    if (!S_ISDIR(st.st_mode))
      continue;
    if ((uint64_t)st.st_dev != c->drive->dev)
      continue; /* another filesystem mounted inside: skip */
    if (!strcmp(e->d_name, "sce_sys") || !strcmp(e->d_name, "fakelib") ||
        !strcmp(e->d_name, "backports") ||
        !strcmp(e->d_name, "System Volume Information") ||
        !strcmp(e->d_name, "$RECYCLE.BIN") ||
        !strcmp(e->d_name, "lost+found") ||
        /* PS5 extended-storage internals on /mnt/extN */
        (depth == 1 && strncmp(dir, "/mnt/ext", 8) == 0 &&
         (!strcmp(e->d_name, "user") || !strcmp(e->d_name, "system_data"))) ||
        !strcmp(e->d_name, "shadowmount") || !strcmp(e->d_name, "game-library-manager"))
      continue;
    char pj[SS_MAX_PATH];
    snprintf(pj, sizeof(pj), "%s/sce_sys/param.json", p);
    if (is_file(pj)) {
      char eb[SS_MAX_PATH];
      snprintf(eb, sizeof(eb), "%s/eboot.bin", p);
      /* sce_sys without eboot.bin = region patch / param swap folders that
       * releases ship alongside the game - not a game */
      if (strcmp(e->d_name, SS_TILE_ID) && is_file(eb))
        add_folder_game(c, p);
      continue; /* never descend into a game */
    }
    walk(c, p, depth + 1);
  }
  closedir(d);
}

static void classify(game_t *g, const smp_model_t *m) {
  if (g->issue == ISSUE_PKG || g->issue == ISSUE_ARCHIVE)
    return;
  g->visible = smp_would_see(m, g->path);
  if (g->visible || g->issue == ISSUE_IMAGE_WRAPPED ||
      g->issue == ISSUE_IMAGE_NO_GAME)
    return;

  const char *base = path_basename(g->path);
  char parent[SS_MAX_PATH], grand[SS_MAX_PATH] = "";
  path_dirname(g->path, parent, sizeof(parent));
  path_dirname(parent, grand, sizeof(grand));

  char root[SS_MAX_PATH];
  bool have_root = best_root_on_drive(g->drive, root, sizeof(root));

  /* destination name: keep images' file names; folders use title id */
  char name[256];
  if (strcmp(g->kind, "folder") || !g->title_id[0])
    snprintf(name, sizeof(name), "%s", base[0] == '.' ? base + 1 : base);
  else
    snprintf(name, sizeof(name), "%s", g->title_id);

  if (base[0] == '.') {
    g->issue = ISSUE_HIDDEN_NAME;
    snprintf(g->detail, sizeof(g->detail),
             "Name starts with a dot, so ShadowMount skips it.");
  } else if (!strcmp(path_basename(parent), "backports")) {
    g->issue = ISSUE_BACKPORTS_PLACE;
    snprintf(g->detail, sizeof(g->detail),
             "Sitting in a backports folder - SMP treats that as backport "
             "overlays, not games.");
  } else if (is_root(m, grand) && strcmp(g->kind, "folder") == 0) {
    /* <root>/<wrapper>/<game> with depth 1 = classic wrapper folder */
    g->issue = ISSUE_WRAPPED_FOLDER;
    snprintf(g->detail, sizeof(g->detail),
             "Inside an extra folder '%s'. SMP (scan_depth=%d) only looks "
             "one level into %s.",
             path_basename(parent), m->depth, grand);
    snprintf(root, sizeof(root), "%s", grand);
    have_root = true;
  } else {
    bool under_any = false;
    for (int i = 0; i < m->count; i++)
      if (path_is_under(m->roots[i], g->path))
        under_any = true;
    if (under_any) {
      g->issue = ISSUE_TOO_DEEP;
      snprintf(g->detail, sizeof(g->detail),
               "Too many folders deep for scan_depth=%d.", m->depth);
    } else {
      g->issue = ISSUE_OUTSIDE_ROOTS;
      snprintf(g->detail, sizeof(g->detail),
               "Not inside any folder ShadowMount scans.");
    }
  }

  if (have_root) {
    char dst[SS_MAX_PATH];
    path_join(dst, sizeof(dst), root, name);
    if (path_exists(dst) && strcmp(dst, parent) != 0) {
      size_t l = strlen(g->detail);
      snprintf(g->detail + l, sizeof(g->detail) - l,
               " Suggested spot %s is already taken.", dst);
    } else {
      snprintf(g->fix_dst, sizeof(g->fix_dst), "%s", dst);
    }
  } else {
    size_t l = strlen(g->detail);
    snprintf(g->detail + l, sizeof(g->detail) - l,
             " No ShadowMount scan folder on this drive - add a scanpath or "
             "move it to another drive.");
  }
}

void scan_run(void) {
  uint64_t t0 = now_ms();
  smp_model_t model;
  smp_model_load(&model);

  pthread_mutex_lock(&g_scan.lock);
  g_scan.model = model; /* best_root_on_drive() reads this */
  pthread_mutex_unlock(&g_scan.lock);

  char ver[64] = "", smpmsg[256] = "";
  smp_status_t smpst = smp_check(ver, sizeof(ver), smpmsg, sizeof(smpmsg));
  bool api = smpst == SMP_OK;

  drive_info_t drives[16];
  int nd = plat_list_drives(drives, 16);

  game_t *games = calloc(SS_MAX_GAMES, sizeof(game_t));
  if (!games) {
    ss_log("scan: out of memory");
    return;
  }
  walk_ctx_t c = {.games = games, .count = 0, .model = &model};
  for (int i = 0; i < nd; i++) {
    c.drive = &drives[i];
    ss_log("scanning %s", drives[i].mount);
    uint64_t td = now_ms();
    unsigned long d0 = g_scan.dirs_seen;
    walk(&c, drives[i].mount, 1);
    ss_log("scanned %s: %lu folders in %llu ms", drives[i].mount,
           g_scan.dirs_seen - d0, (unsigned long long)(now_ms() - td));
  }

  for (int i = 0; i < c.count; i++)
    classify(&games[i], &model);

  /* duplicates among sources SMP would see */
  for (int i = 0; i < c.count; i++) {
    if (!games[i].visible || !games[i].title_id[0] ||
        games[i].issue == ISSUE_PKG || games[i].issue == ISSUE_ARCHIVE)
      continue;
    for (int j = i + 1; j < c.count; j++) {
      if (games[j].visible && !strcmp(games[i].title_id, games[j].title_id)) {
        game_t *pair[2] = {&games[i], &games[j]};
        for (int k = 0; k < 2; k++) {
          game_t *a = pair[k], *b = pair[1 - k];
          size_t l = 0;
          if (a->issue == ISSUE_NONE)
            a->issue = ISSUE_DUPLICATE;
          else
            l = strlen(a->detail);
          if (l < sizeof(a->detail))
            snprintf(a->detail + l, sizeof(a->detail) - l,
                     "%sSame title ID also at %s - SMP only uses one.",
                     l ? " " : "", b->path);
        }
      }
    }
  }

  /* what does SMP itself report? */
  if (api) {
    smp_game_t *sg = calloc(SS_MAX_GAMES, sizeof(smp_game_t));
    int ns = sg ? smp_api_games(sg, SS_MAX_GAMES) : -1;
    for (int i = 0; i < c.count; i++) {
      for (int j = 0; j < ns; j++) {
        if (!strcmp(games[i].path, sg[j].path)) {
          games[i].smp_sees = true;
          games[i].smp_mounted = sg[j].mounted;
          if (!games[i].title[0])
            snprintf(games[i].title, sizeof(games[i].title), "%s",
                     sg[j].title_name);
          break;
        }
      }
    }
    free(sg);
  }

  char gfl[1024] = "";
  list_dir_names("/data/shadowmount/fakelib", gfl, sizeof(gfl));

  int problems = 0;
  for (int i = 0; i < c.count; i++)
    if (games[i].issue != ISSUE_NONE)
      problems++;

  pthread_mutex_lock(&g_scan.lock);
  memcpy(g_scan.games, games, sizeof(game_t) * (size_t)c.count);
  g_scan.count = c.count;
  g_scan.smp_api = api;
  g_scan.smp_status = (int)smpst;
  snprintf(g_scan.smp_message, sizeof(g_scan.smp_message), "%s", smpmsg);
  snprintf(g_scan.smp_version, sizeof(g_scan.smp_version), "%s", ver);
  g_scan.drive_count = nd;
  memcpy(g_scan.drives, drives, sizeof(drives));
  snprintf(g_scan.global_fakelib, sizeof(g_scan.global_fakelib), "%s", gfl);
  g_scan.global_savefix = savefix_state("/data/shadowmount/fakelib");
  g_scan.finished_ms = now_ms();
  g_scan.running = false;
  pthread_mutex_unlock(&g_scan.lock);
  free(games);

  ss_log("scan done: %d sources, %d need attention, %llu ms", c.count,
         problems, (unsigned long long)(now_ms() - t0));
}

/* Measure folder game sizes after the results are already on screen. */
static void size_folders(void) {
  pthread_mutex_lock(&g_scan.lock);
  int n = g_scan.count;
  g_scan.sizing = true;
  pthread_mutex_unlock(&g_scan.lock);
  for (int i = 0; i < n; i++) {
    char path[SS_MAX_PATH];
    bool todo = false;
    pthread_mutex_lock(&g_scan.lock);
    if (i < g_scan.count && !strcmp(g_scan.games[i].kind, "folder") &&
        g_scan.games[i].size_state == 0) {
      snprintf(path, sizeof(path), "%s", g_scan.games[i].path);
      todo = true;
    }
    pthread_mutex_unlock(&g_scan.lock);
    if (!todo)
      continue;
    uint64_t bytes = 0, files = 0, t0 = now_ms();
    int rc = measure_tree_ex(path, &bytes, &files, 120000, g_scan.current,
                             sizeof(g_scan.current));
    ss_log("  sized %s: %llu files, %llu MB in %llu ms%s", path,
           (unsigned long long)files, (unsigned long long)(bytes >> 20),
           (unsigned long long)(now_ms() - t0), rc ? " (gave up - partial)" : "");
    pthread_mutex_lock(&g_scan.lock);
    if (i < g_scan.count && !strcmp(g_scan.games[i].path, path)) {
      g_scan.games[i].size = bytes;
      g_scan.games[i].files = files;
      g_scan.games[i].size_state = rc ? 2 : 1;
    }
    pthread_mutex_unlock(&g_scan.lock);
  }
  pthread_mutex_lock(&g_scan.lock);
  g_scan.sizing = false;
  g_scan.current[0] = 0;
  pthread_mutex_unlock(&g_scan.lock);
}

static void *scan_thread(void *arg) {
  (void)arg;
  scan_run();
  size_folders();
  tile_retry_if_waiting(g_scan.smp_api);
  return NULL;
}

void scan_start_async(void) {
  pthread_mutex_lock(&g_scan.lock);
  if (g_scan.running) {
    pthread_mutex_unlock(&g_scan.lock);
    return;
  }
  g_scan.running = true;
  g_scan.started_ms = now_ms();
  g_scan.dirs_seen = 0;
  g_scan.found = 0;
  g_scan.current[0] = 0;
  pthread_mutex_unlock(&g_scan.lock);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t, &a, scan_thread, NULL) != 0) {
    pthread_mutex_lock(&g_scan.lock);
    g_scan.running = false;
    pthread_mutex_unlock(&g_scan.lock);
  }
  pthread_attr_destroy(&a);
}

bool scan_get_game(int id, game_t *out) {
  bool ok = false;
  pthread_mutex_lock(&g_scan.lock);
  if (id >= 0 && id < g_scan.count) {
    *out = g_scan.games[id];
    ok = true;
  }
  pthread_mutex_unlock(&g_scan.lock);
  return ok;
}

int savefix_apply_image(const game_t *g, bool undo, char *err, size_t errsz) {
  if (strcmp(g->kind, "exfat")) {
    snprintf(err, errsz,
             "the save fix can only be applied inside .exfat images for now");
    return -1;
  }
  /* never write to an image ShadowMount has mounted */
  if (!smp_api_available(NULL, 0)) {
    snprintf(err, errsz, "ShadowMount isn't reachable, so it can't be "
                         "confirmed the image isn't in use");
    return -1;
  }
  smp_game_t *sg = calloc(SS_MAX_GAMES, sizeof(smp_game_t));
  if (!sg)
    return -1;
  int n = smp_api_games(sg, SS_MAX_GAMES);
  bool mounted = n < 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(sg[i].path, g->path) && sg[i].mounted)
      mounted = true;
  free(sg);
  if (mounted) {
    snprintf(err, errsz, "%s is in use by ShadowMount - close the game fully "
                         "and try again", g->title[0] ? g->title : g->title_id);
    return -1;
  }
  char list[2048];
  if (image_list_dir(g->path, "fakelib", list, sizeof(list)) <= 0) {
    snprintf(err, errsz, "no fakelib folder in the image");
    return -1;
  }
  int changed = 0;
  char *save = NULL;
  for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
    while (*t == ' ')
      t++;
    char *paren = strrchr(t, '(');
    if (paren)
      *paren = 0;
    bool dis;
    if (!is_save_lib(t, &dis) || dis != undo)
      continue;
    size_t l = strlen(t);
    char to[256];
    snprintf(to, sizeof(to), "%s", t);
    if (!undo && ends_with_ci(t, ".sprx"))
      memcpy(to + l - 5, ".soff", 5);
    else if (undo && ends_with_ci(t, ".soff"))
      memcpy(to + l - 5, ".sprx", 5);
    else
      continue;
    if (image_rename_in_dir(g->path, "fakelib", t, to, err, errsz) == 0) {
      ss_log("save fix %s in image %s: %s -> %s", undo ? "undone" : "applied",
             g->path, t, to);
      changed++;
    } else {
      ss_log("save fix in image %s failed for %s: %s", g->path, t, err);
    }
  }
  return changed;
}
