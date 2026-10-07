/* Minimal HTTP server for the phone/PS5-browser UI. */
#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *drive_label(const char *d) {
  if (!strcmp(d, "/data"))
    return "Internal SSD";
  if (!strcmp(d, "/mnt/ext1"))
    return "M.2 SSD";
  if (!strcmp(d, "/mnt/ext0"))
    return "Extended storage";
  if (!strncmp(d, "/mnt/usb", 8))
    return "USB drive";
  return "Drive";
}

static void url_decode(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '+')
      *o++ = ' ';
    else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
             isxdigit((unsigned char)s[2])) {
      char h[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(h, NULL, 16);
      s += 2;
    } else
      *o++ = *s;
  }
  *o = 0;
}

static bool query_get(const char *query, const char *key, char *out,
                      size_t outsz) {
  out[0] = 0;
  if (!query)
    return false;
  size_t kl = strlen(key);
  const char *p = query;
  while (*p) {
    if (!strncmp(p, key, kl) && p[kl] == '=') {
      p += kl + 1;
      size_t n = strcspn(p, "&");
      if (n >= outsz)
        n = outsz - 1;
      memcpy(out, p, n);
      out[n] = 0;
      url_decode(out);
      return true;
    }
    p += strcspn(p, "&");
    if (*p == '&')
      p++;
  }
  return false;
}

static void send_resp(int fd, int code, const char *ctype, const char *body,
                      size_t len) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: "
                   "%zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                   code, code == 200 ? "OK" : "Error", ctype, len);
  send(fd, hdr, (size_t)n, 0);
  size_t off = 0;
  while (off < len) {
    ssize_t w = send(fd, body + off, len - off, 0);
    if (w <= 0)
      break;
    off += (size_t)w;
  }
}

static void send_json_msg(int fd, bool ok, const char *msg) {
  sbuf_t b;
  sb_init(&b);
  sb_printf(&b, "{\"ok\":%s,\"message\":", ok ? "true" : "false");
  sb_json_str(&b, msg ? msg : "");
  sb_puts(&b, "}");
  send_resp(fd, ok ? 200 : 400, "application/json", b.data, b.len);
  sb_free(&b);
}

static void state_json(sbuf_t *b) {
  pthread_mutex_lock(&g_scan.lock);
  sb_printf(b, "{\"version\":\"%s\",\"scan\":{\"running\":%s,", SS_VERSION,
            g_scan.running ? "true" : "false");
  sb_printf(b, "\"sizing\":%s,", g_scan.sizing ? "true" : "false");
  sb_printf(b, "\"dirs_seen\":%lu,\"found\":%d,\"started_ms\":%llu,\"current\":",
            g_scan.dirs_seen, g_scan.found,
            (unsigned long long)g_scan.started_ms);
  sb_json_str(b, g_scan.current);
  sb_puts(b, ",");
  sb_printf(b, "\"finished_ms\":%llu,\"smp_api\":%s,\"smp_version\":",
            (unsigned long long)g_scan.finished_ms,
            g_scan.smp_api ? "true" : "false");
  sb_json_str(b, g_scan.smp_version);
  sb_printf(b, ",\"smp_status\":\"%s\",\"smp_message\":",
            smp_status_name((smp_status_t)g_scan.smp_status));
  sb_json_str(b, g_scan.smp_message);
  sb_printf(b, ",\"depth\":%d,\"roots\":[", g_scan.model.depth);
  for (int i = 0; i < g_scan.model.count; i++) {
    if (i)
      sb_putc(b, ',');
    sb_json_str(b, g_scan.model.roots[i]);
  }
  sb_puts(b, "],\"drives\":[");
  for (int i = 0; i < g_scan.drive_count; i++) {
    drive_info_t *d = &g_scan.drives[i];
    char root[SS_MAX_PATH] = "";
    best_root_on_drive(d->mount, root, sizeof(root));
    sb_printf(b, "%s{\"mount\":", i ? "," : "");
    sb_json_str(b, d->mount);
    sb_puts(b, ",\"label\":");
    sb_json_str(b, drive_label(d->mount));
    sb_puts(b, ",\"root\":");
    sb_json_str(b, root);
    sb_printf(b, ",\"total\":%llu,\"avail\":%llu}",
              (unsigned long long)d->total, (unsigned long long)d->avail);
  }
  sb_printf(b, "],\"global_savefix\":%d,\"global_fakelib\":",
            g_scan.global_savefix);
  sb_json_str(b, g_scan.global_fakelib);
  sb_puts(b, ",\"games\":[");
  for (int i = 0; i < g_scan.count; i++) {
    game_t *g = &g_scan.games[i];
    sb_printf(b, "%s{\"id\":%d,\"path\":", i ? "," : "", g->id);
    sb_json_str(b, g->path);
    sb_puts(b, ",\"title_id\":");
    sb_json_str(b, g->title_id);
    sb_puts(b, ",\"title\":");
    sb_json_str(b, g->title);
    sb_puts(b, ",\"kind\":");
    sb_json_str(b, g->kind);
    sb_puts(b, ",\"drive\":");
    sb_json_str(b, g->drive);
    sb_printf(b,
              ",\"size\":%llu,\"files\":%llu,\"visible\":%s,\"smp_sees\":%s,"
              "\"mounted\":%s,\"issue\":\"%s\",\"detail\":",
              (unsigned long long)g->size, (unsigned long long)g->files,
              g->visible ? "true" : "false", g->smp_sees ? "true" : "false",
              g->smp_mounted ? "true" : "false", issue_name(g->issue));
    sb_json_str(b, g->detail);
    sb_puts(b, ",\"fix_dst\":");
    sb_json_str(b, g->fix_dst);
    sb_printf(b, ",\"has_fakelib\":%s,\"savefix\":%d,\"size_state\":%d,\"fakelib\":",
              g->has_fakelib ? "true" : "false", g->savefix, g->size_state);
    sb_json_str(b, g->fakelib_files);
    sb_puts(b, "}");
  }
  sb_puts(b, "]}");
  pthread_mutex_unlock(&g_scan.lock);
  sb_puts(b, ",\"tile\":");
  tile_json(b);
  sb_printf(b, ",\"push\":{\"ntfy\":%s,\"webhook\":%s},\"queue\":",
            g_cfg.ntfy_url[0] ? "true" : "false",
            g_cfg.webhook_url[0] ? "true" : "false");
  queue_json(b);
  /* first-use guide: shown until someone finishes or skips it, on any device */
  char ip[64], gp[SS_MAX_PATH];
  ss_local_ip(ip, sizeof(ip));
  snprintf(gp, sizeof(gp), "%s/guide.done", g_cfg.data_dir);
  sb_puts(b, ",\"ip\":");
  sb_json_str(b, ip);
  sb_printf(b, ",\"port\":%d,\"guide_done\":%s}", g_cfg.http_port,
            is_file(gp) ? "true" : "false");
}

static void handle_job(int fd, const char *q) {
  char action[16], idbuf[16], root[SS_MAX_PATH];
  query_get(q, "action", action, sizeof(action));
  query_get(q, "id", idbuf, sizeof(idbuf));
  query_get(q, "root", root, sizeof(root));
  game_t g;
  if (!scan_get_game(atoi(idbuf), &g)) {
    send_json_msg(fd, false, "unknown game - rescan and try again");
    return;
  }
  char err[256] = "";
  int id = -1;
  if (!strcmp(action, "fix")) {
    if (!g.fix_dst[0]) {
      send_json_msg(fd, false, "no automatic fix for this one");
      return;
    }
    id = queue_add(JOB_FIX, g.path, g.fix_dst, g.title_id, g.title, err,
                   sizeof(err));
  } else if (!strcmp(action, "move") || !strcmp(action, "copy")) {
    if (!root[0] || root[0] != '/') {
      send_json_msg(fd, false, "pick a destination");
      return;
    }
    char dst[SS_MAX_PATH];
    path_join(dst, sizeof(dst), root, path_basename(g.path));
    id = queue_add(!strcmp(action, "move") ? JOB_MOVE : JOB_COPY, g.path, dst,
                   g.title_id, g.title, err, sizeof(err));
  } else {
    send_json_msg(fd, false, "unknown action");
    return;
  }
  if (id < 0)
    send_json_msg(fd, false, err);
  else
    send_json_msg(fd, true, "queued");
}

static void handle_fix_all(int fd) {
  int added = 0, failed = 0;
  for (int i = 0; i < SS_MAX_GAMES; i++) {
    game_t g;
    if (!scan_get_game(i, &g))
      break;
    if (g.issue == ISSUE_NONE || !g.fix_dst[0])
      continue;
    char err[256];
    if (queue_add(JOB_FIX, g.path, g.fix_dst, g.title_id, g.title, err,
                  sizeof(err)) > 0)
      added++;
    else
      failed++;
  }
  char msg[128];
  snprintf(msg, sizeof(msg), "%d fix(es) queued%s", added,
           failed ? " (some were already queued)" : "");
  send_json_msg(fd, true, msg);
}

static void handle_disable_fakelib(int fd, const char *q) {
  char idbuf[16], file[256];
  query_get(q, "id", idbuf, sizeof(idbuf));
  query_get(q, "file", file, sizeof(file));
  game_t g;
  if (!scan_get_game(atoi(idbuf), &g) || strcmp(g.kind, "folder")) {
    send_json_msg(fd, false, "unknown folder game");
    return;
  }
  if (!file[0] || strchr(file, '/') || !strcmp(file, ".") ||
      !strcmp(file, "..")) {
    send_json_msg(fd, false, "bad file name");
    return;
  }
  char src[SS_MAX_PATH], dst[SS_MAX_PATH + 16];
  snprintf(src, sizeof(src), "%s/fakelib/%s", g.path, file);
  bool enable = ends_with_ci(file, ".disabled");
  if (enable) {
    snprintf(dst, sizeof(dst), "%s", src);
    dst[strlen(dst) - 9] = 0;
  } else {
    snprintf(dst, sizeof(dst), "%s.disabled", src);
  }
  if (rename(src, dst) != 0) {
    send_json_msg(fd, false, strerror(errno));
    return;
  }
  ss_log("fakelib: %s -> %s", src, dst);
  scan_start_async();
  send_json_msg(fd, true, enable ? "re-enabled" : "disabled");
}

/* Save fix: id=<game id> | id=all | id=global, undo=1 to reverse */
static void handle_savefix(int fd, const char *q) {
  char idbuf[16], undo_s[4];
  query_get(q, "id", idbuf, sizeof(idbuf));
  query_get(q, "undo", undo_s, sizeof(undo_s));
  bool undo = atoi(undo_s) != 0;
  char err[256] = "";
  int changed = 0, games = 0;
  if (!strcmp(idbuf, "global")) {
    int n = savefix_apply("/data/shadowmount/fakelib", undo, err, sizeof(err));
    if (n > 0)
      changed += n, games++;
  } else if (!strcmp(idbuf, "all")) {
    for (int i = 0; i < SS_MAX_GAMES; i++) {
      game_t g;
      if (!scan_get_game(i, &g))
        break;
      if (g.savefix != (undo ? 2 : 1))
        continue;
      char dir[SS_MAX_PATH];
      snprintf(dir, sizeof(dir), "%s/fakelib", g.path);
      int n = strcmp(g.kind, "folder")
                  ? savefix_apply_image(&g, undo, err, sizeof(err))
                  : savefix_apply(dir, undo, err, sizeof(err));
      if (n > 0)
        changed += n, games++;
    }
  } else {
    game_t g;
    if (!scan_get_game(atoi(idbuf), &g)) {
      send_json_msg(fd, false, "unknown game - rescan and try again");
      return;
    }
    char dir[SS_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/fakelib", g.path);
    int n = strcmp(g.kind, "folder")
                ? savefix_apply_image(&g, undo, err, sizeof(err))
                : savefix_apply(dir, undo, err, sizeof(err));
    if (n > 0)
      changed += n, games++;
  }
  scan_start_async();
  char msg[320];
  if (changed)
    snprintf(msg, sizeof(msg),
             "Save fix %s for %d game(s). Takes effect next time the game "
             "starts.",
             undo ? "removed" : "applied", games);
  else
    snprintf(msg, sizeof(msg), "Nothing to change%s%s", err[0] ? ": " : "",
             err);
  send_json_msg(fd, changed > 0, msg);
}

static void handle(int fd) {
  char req[8192];
  size_t got = 0;
  while (got + 1 < sizeof(req)) {
    ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
    if (n <= 0)
      break;
    got += (size_t)n;
    req[got] = 0;
    if (strstr(req, "\r\n\r\n"))
      break;
  }
  req[got] = 0;
  char method[8] = "", target[2048] = "";
  if (sscanf(req, "%7s %2047s", method, target) != 2) {
    close(fd);
    return;
  }
  char *q = strchr(target, '?');
  if (q)
    *q++ = 0;
  const char *path = target;
  bool post = !strcmp(method, "POST");

  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    send_resp(fd, 200, "text/html; charset=utf-8", g_index_html,
              strlen(g_index_html));
  } else if (!strcmp(path, "/api/state")) {
    sbuf_t b;
    sb_init(&b);
    state_json(&b);
    send_resp(fd, 200, "application/json", b.data, b.len);
    sb_free(&b);
  } else if (!strcmp(path, "/api/archive")) {
    /* what's inside a .7z: /api/archive?id=N */
    char idbuf[16];
    query_get(q, "id", idbuf, sizeof(idbuf));
    game_t g;
    if (!scan_get_game(atoi(idbuf), &g) || strcmp(g.kind, "archive")) {
      send_json_msg(fd, false, "unknown archive - rescan and try again");
    } else {
      sbuf_t b;
      sb_init(&b);
      archive_info_json(g.path, &b);
      send_resp(fd, 200, "application/json", b.data, b.len);
      sb_free(&b);
    }
  } else if (!strcmp(path, "/api/pkginfo")) {
    /* summary of a scanned .pkg: /api/pkginfo?id=N (no file data) */
    char idbuf[16];
    query_get(q, "id", idbuf, sizeof(idbuf));
    game_t g;
    if (!scan_get_game(atoi(idbuf), &g) || strcmp(g.kind, "pkg")) {
      send_json_msg(fd, false, "unknown package - rescan and try again");
    } else {
      sbuf_t b;
      sb_init(&b);
      pkg_inspect_json(g.path, &b);
      send_resp(fd, 200, "application/json", b.data, b.len);
      sb_free(&b);
    }
  } else if (!strcmp(path, "/api/imgls")) {
    /* read-only listing inside an image: /api/imgls?id=N&dir=fakelib */
    char idbuf[16], dir[256];
    query_get(q, "id", idbuf, sizeof(idbuf));
    query_get(q, "dir", dir, sizeof(dir));
    game_t g;
    char *out = malloc(32768);
    if (!out || !scan_get_game(atoi(idbuf), &g)) {
      send_json_msg(fd, false, "unknown game");
    } else {
      int n = image_list_dir(g.path, dir, out, 32768);
      sbuf_t b;
      sb_init(&b);
      sb_printf(&b, "{\"ok\":%s,\"count\":%d,\"entries\":",
                n >= 0 ? "true" : "false", n);
      sb_json_str(&b, out);
      sb_puts(&b, "}");
      send_resp(fd, 200, "application/json", b.data, b.len);
      sb_free(&b);
    }
    free(out);
  } else if (!strcmp(path, "/api/log")) {
    /* last ~64 KB of our own log, for remote debugging */
    char lp[SS_MAX_PATH];
    snprintf(lp, sizeof(lp), "%s/game-library-manager.log", g_cfg.data_dir);
    FILE *f = fopen(lp, "r");
    sbuf_t b;
    sb_init(&b);
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      long off = sz > 65536 ? sz - 65536 : 0;
      fseek(f, off, SEEK_SET);
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        sb_puts(&b, buf);
      }
      fclose(f);
    }
    send_resp(fd, 200, "text/plain; charset=utf-8", b.data ? b.data : "",
              b.len);
    sb_free(&b);
  } else if (!post) {
    send_resp(fd, 404, "text/plain", "not found", 9);
  } else if (!strcmp(path, "/api/open")) {
    /* sent by the home-screen tile */
    tile_open_page_async();
    send_json_msg(fd, true, "opening");
  } else if (!strcmp(path, "/api/tile")) {
    tile_install_async(true);
    send_json_msg(fd, true, "reinstalling the home tile");
  } else if (!strcmp(path, "/api/extract")) {
    /* /api/extract?id=N&inner=<path in archive>&name=<dest name>&root=<scan
     * folder>&delete=0|1 */
    char idbuf[16], inner[SS_MAX_PATH], name[256], root[SS_MAX_PATH], del[4],
        ex[4], sib[4];
    query_get(q, "id", idbuf, sizeof(idbuf));
    query_get(q, "inner", inner, sizeof(inner));
    query_get(q, "name", name, sizeof(name));
    query_get(q, "root", root, sizeof(root));
    query_get(q, "delete", del, sizeof(del));
    query_get(q, "extras", ex, sizeof(ex));
    query_get(q, "siblings", sib, sizeof(sib));
    game_t g;
    char err[256] = "", dst[SS_MAX_PATH];
    if (!scan_get_game(atoi(idbuf), &g) || strcmp(g.kind, "archive")) {
      send_json_msg(fd, false, "unknown archive - rescan and try again");
    } else if (root[0] != '/' || !name[0] || strchr(name, '/') ||
               name[0] == '.') {
      send_json_msg(fd, false, "pick a destination");
    } else if (!path_join(dst, sizeof(dst), root, name)) {
      send_json_msg(fd, false, "path too long");
    } else {
      mkdir_p(root);
      /* extras go to <drive>/game-extras/<name>, outside ShadowMount's
       * scan folders so nothing in there gets mistaken for a game */
      char extras[SS_MAX_PATH] = "";
      if (!ex[0] || atoi(ex) || atoi(sib)) {
        const char *drive = NULL;
        for (int i = 0; i < g_cfg.drive_count; i++)
          if (path_is_under(g_cfg.drives[i], root) &&
              (!drive || strlen(g_cfg.drives[i]) > strlen(drive)))
            drive = g_cfg.drives[i];
        if (drive)
          snprintf(extras, sizeof(extras), "%s/game-extras/%s", drive, name);
      }
      int id = queue_add_extract(g.path, inner, dst, extras, g.title_id,
                                 g.title[0] ? g.title : name,
                                 (atoi(del) ? JOBF_DELETE_SOURCE : 0) |
                                     (atoi(sib) ? JOBF_SIBLINGS : 0),
                                 err, sizeof(err));
      send_json_msg(fd, id > 0, id > 0 ? "extraction queued" : err);
    }
  } else if (!strcmp(path, "/api/depth")) {
    char d[8], err[256] = "";
    query_get(q, "depth", d, sizeof(d));
    if (smp_set_scan_depth(atoi(d), err, sizeof(err))) {
      smp_api_scan();
      scan_start_async();
      char msg[160];
      snprintf(msg, sizeof(msg),
               "ShadowMount scan depth set to %d. SMP picks it up on its next "
               "scan; the list is being refreshed.",
               atoi(d));
      send_json_msg(fd, true, msg);
    } else {
      send_json_msg(fd, false, err);
    }
  } else if (!strcmp(path, "/api/scan")) {
    scan_start_async();
    send_json_msg(fd, true, "scan started");
  } else if (!strcmp(path, "/api/job")) {
    handle_job(fd, q);
  } else if (!strcmp(path, "/api/fixall")) {
    handle_fix_all(fd);
  } else if (!strcmp(path, "/api/cancel")) {
    char id[16];
    query_get(q, "id", id, sizeof(id));
    send_json_msg(fd, queue_cancel(atoi(id)), "cancel requested");
  } else if (!strcmp(path, "/api/up")) {
    char id[16];
    query_get(q, "id", id, sizeof(id));
    send_json_msg(fd, queue_move_up(atoi(id)), "moved");
  } else if (!strcmp(path, "/api/cancelall")) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Stopped %d job(s). Finished jobs were left as they are.",
             queue_cancel_all());
    send_json_msg(fd, true, msg);
  } else if (!strcmp(path, "/api/savefix")) {
    handle_savefix(fd, q);
  } else if (!strcmp(path, "/api/clear")) {
    queue_clear_finished();
    send_json_msg(fd, true, "cleared");
  } else if (!strcmp(path, "/api/pause")) {
    queue_set_paused(true);
    send_json_msg(fd, true, "paused");
  } else if (!strcmp(path, "/api/resume")) {
    queue_set_paused(false);
    send_json_msg(fd, true, "resumed");
  } else if (!strcmp(path, "/api/guide")) {
    /* /api/guide?done=1 marks the first-use guide as seen, done=0 resets it */
    char d[4];
    query_get(q, "done", d, sizeof(d));
    char gp[SS_MAX_PATH];
    snprintf(gp, sizeof(gp), "%s/guide.done", g_cfg.data_dir);
    bool ok;
    if (!strcmp(d, "0")) {
      ok = unlink(gp) == 0 || !is_file(gp);
    } else {
      FILE *f = fopen(gp, "w");
      ok = f && fputs("1\n", f) >= 0;
      if (f)
        fclose(f);
    }
    send_json_msg(fd, ok, ok ? "saved" : "couldn't save the guide setting");
  } else if (!strcmp(path, "/api/testpush")) {
    notify_event("Test", "Push notifications are working.", true);
    send_json_msg(fd, true, "sent");
  } else if (!strcmp(path, "/api/manual")) {
    char idbuf[16];
    query_get(q, "id", idbuf, sizeof(idbuf));
    game_t g;
    if (!scan_get_game(atoi(idbuf), &g))
      send_json_msg(fd, false, "unknown game");
    else if (smp_api_manual_add(g.path))
      send_json_msg(fd, true, "added to SMP's manual list");
    else
      send_json_msg(fd, false, "SMP API not reachable");
  } else if (!strcmp(path, "/api/fakelib")) {
    handle_disable_fakelib(fd, q);
  } else {
    send_resp(fd, 404, "text/plain", "not found", 9);
  }
  close(fd);
}

static void *conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  handle(fd);
  return NULL;
}

static void *accept_thread(void *arg) {
  int port = (int)(intptr_t)arg;
  int s = -1;
  for (;;) {
    s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(s, 16) == 0)
      break;
    ss_log("http: can't listen on port %d (%s), retrying", port,
           strerror(errno));
    close(s);
    sleep(5);
  }
  ss_log("web UI on port %d", port);
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) {
      if (errno == EINTR)
        continue;
      sleep(1);
      continue;
    }
    struct timeval tv = {10, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, conn_thread, (void *)(intptr_t)c) != 0)
      close(c);
    pthread_attr_destroy(&a);
  }
  return NULL;
}

void httpd_start(int port) {
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, accept_thread, (void *)(intptr_t)port);
  pthread_attr_destroy(&a);
}
