#include "common.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void ss_local_ip(char *out, size_t sz) {
  out[0] = 0;
  struct ifaddrs *ifs = NULL;
  if (getifaddrs(&ifs) != 0)
    return;
  for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
      continue;
    if (i->ifa_flags & IFF_LOOPBACK)
      continue;
    struct sockaddr_in *sa = (struct sockaddr_in *)i->ifa_addr;
    inet_ntop(AF_INET, &sa->sin_addr, out, (socklen_t)sz);
    if (strncmp(out, "127.", 4))
      break;
  }
  freeifaddrs(ifs);
}

static bool already_running(int port) {
  sbuf_t b;
  sb_init(&b);
  int st = http_request_raw("127.0.0.1", port, "GET", "/api/state", NULL,
                            NULL, &b, 1500);
  bool ours = st == 200 && b.data && strstr(b.data, "\"queue\"");
  sb_free(&b);
  return ours;
}

int main(int argc, char **argv) {
  config_defaults();

  /* host-build / testing overrides */
  for (int i = 1; i + 1 < argc; i++) {
    if (!strcmp(argv[i], "--data"))
      snprintf(g_cfg.data_dir, sizeof(g_cfg.data_dir), "%s", argv[++i]);
    else if (!strcmp(argv[i], "--drive") && g_cfg.drive_count < 16)
      snprintf(g_cfg.drives[g_cfg.drive_count++], SS_MAX_PATH, "%s",
               argv[++i]);
    else if (!strcmp(argv[i], "--smp-config"))
      snprintf(g_cfg.smp_config, sizeof(g_cfg.smp_config), "%s", argv[++i]);
    else if (!strcmp(argv[i], "--smp-port"))
      g_cfg.smp_port = atoi(argv[++i]);
  }
  if (!g_cfg.data_dir[0])
    snprintf(g_cfg.data_dir, sizeof(g_cfg.data_dir), "./ss-data");

  mkdir_p(g_cfg.data_dir);
  char p[SS_MAX_PATH];
  snprintf(p, sizeof(p), "%s/game-library-manager.log", g_cfg.data_dir);
  ss_log_open(p);
  snprintf(p, sizeof(p), "%s/config.ini", g_cfg.data_dir);
  config_load(p);

  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], "--port"))
      g_cfg.http_port = atoi(argv[++i]);
#ifndef __PROSPERO__
  if (g_cfg.drive_count)
    snprintf(g_cfg.tile_root, sizeof(g_cfg.tile_root), "%s/homebrew",
             g_cfg.drives[0]);
#endif

  plat_init();

  char local_url[64];
  snprintf(local_url, sizeof(local_url), "http://127.0.0.1:%d/",
           g_cfg.http_port);

  if (already_running(g_cfg.http_port)) {
    /* Sending the payload again = "open the app": just bring the page up
     * in the PS5 browser; the running copy keeps its queue. */
    ss_log("already running - opening the page instead");
    if (!plat_open_browser(local_url))
      plat_toast("Game Library Manager is already running.");
    return 0;
  }

  ss_log("Game Library Manager %s starting", SS_VERSION);
  queue_init();
  httpd_start(g_cfg.http_port);
  scan_start_async();
  tile_install_async(false);

  char ip[64];
  ss_local_ip(ip, sizeof(ip));
  char msg[160];
  snprintf(msg, sizeof(msg), "Game Library Manager %s ready\nOpen http://%s:%d",
           SS_VERSION, ip[0] ? ip : "<ps5-ip>", g_cfg.http_port);
  plat_toast(msg);
  ss_log("%s", msg);
  if (g_cfg.open_browser) {
    sleep(2); /* let the web server bind / the tile app close first */
    plat_open_browser(local_url);
  }

  /* light background rescan so the page and the tile notice ShadowMount
   * coming up later (e.g. when Game Library Manager is loaded first at boot) */
  for (;;) {
    sleep(60);
    if (!g_scan.smp_api)
      scan_start_async();
    else
      tile_retry_if_waiting(true); /* e.g. SMP was busy with a game */
  }
  return 0;
}
