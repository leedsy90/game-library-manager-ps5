#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ss_config_t g_cfg;

void config_defaults(void) {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.http_port = 8095;
  g_cfg.toasts = true;
  g_cfg.open_browser = false; /* autoloaded at boot: don't pop up */
  g_cfg.tile = true;
  strcpy(g_cfg.tile_root, "/data/homebrew");
  g_cfg.copy_threads = 6;
  g_cfg.copy_buffer_mb = 8;
  g_cfg.verify_timeout_s = 120;
  g_cfg.walk_depth = 5;
  strcpy(g_cfg.smp_host, "127.0.0.1");
  g_cfg.smp_port = 10101;
#ifdef __PROSPERO__
  strcpy(g_cfg.data_dir, "/data/game-library-manager");
  strcpy(g_cfg.smp_config, "/data/shadowmount/config.ini");
  const char *drives[] = {"/data",      "/mnt/ext0",  "/mnt/ext1",
                          "/mnt/usb0",  "/mnt/usb1",  "/mnt/usb2",
                          "/mnt/usb3",  "/mnt/usb4",  "/mnt/usb5",
                          "/mnt/usb6",  "/mnt/usb7",  NULL};
  for (int i = 0; drives[i]; i++)
    strcpy(g_cfg.drives[g_cfg.drive_count++], drives[i]);
#endif
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = 0;
  return s;
}

void config_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    /* write a commented template so the user can find the options */
    f = fopen(path, "w");
    if (f) {
      fprintf(f,
              "# Game Library Manager settings. Remove the # to change a value.\n"
              "# Web page port (open http://<ps5-ip>:PORT on your phone)\n"
              "# http_port=8095\n"
              "#\n"
              "# Phone push via the free ntfy app: pick a hard-to-guess topic\n"
              "# name, subscribe to it in the app, then put it here.\n"
              "# ntfy_url=https://ntfy.sh/game-library-change-me-1234\n"
              "#\n"
              "# Or send a JSON POST to a Home Assistant webhook instead/as well\n"
              "# webhook_url=http://192.168.1.10:8123/api/webhook/game-library-manager\n"
              "#\n"
              "# Also open the page in the PS5 browser when the payload loads\n"
              "# (off by default - use the home-screen tile instead)\n"
              "# open_browser=0\n"
              "# Home-screen tile (registered by ShadowMount Plus)\n"
              "# tile=1\n"
              "# toasts=1\n"
              "# copy_threads=6\n"
              "# copy_buffer_mb=8\n"
              "# verify_timeout_s=120\n");
      fclose(f);
    }
    return;
  }
  char line[1024];
  bool drives_reset = false;
  while (fgets(line, sizeof(line), f)) {
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *k = trim(s), *v = trim(eq + 1);
    if (!strcasecmp(k, "http_port"))
      g_cfg.http_port = atoi(v);
    else if (!strcasecmp(k, "ntfy_url"))
      snprintf(g_cfg.ntfy_url, sizeof(g_cfg.ntfy_url), "%s", v);
    else if (!strcasecmp(k, "webhook_url"))
      snprintf(g_cfg.webhook_url, sizeof(g_cfg.webhook_url), "%s", v);
    else if (!strcasecmp(k, "toasts"))
      g_cfg.toasts = atoi(v) != 0;
    else if (!strcasecmp(k, "open_browser"))
      g_cfg.open_browser = atoi(v) != 0;
    else if (!strcasecmp(k, "tile"))
      g_cfg.tile = atoi(v) != 0;
    else if (!strcasecmp(k, "tile_root"))
      snprintf(g_cfg.tile_root, sizeof(g_cfg.tile_root), "%s", v);
    else if (!strcasecmp(k, "copy_threads"))
      g_cfg.copy_threads = atoi(v);
    else if (!strcasecmp(k, "copy_buffer_mb"))
      g_cfg.copy_buffer_mb = atoi(v);
    else if (!strcasecmp(k, "verify_timeout_s"))
      g_cfg.verify_timeout_s = atoi(v);
    else if (!strcasecmp(k, "walk_depth"))
      g_cfg.walk_depth = atoi(v);
    else if (!strcasecmp(k, "smp_port"))
      g_cfg.smp_port = atoi(v);
    else if (!strcasecmp(k, "smp_config"))
      snprintf(g_cfg.smp_config, sizeof(g_cfg.smp_config), "%s", v);
    else if (!strcasecmp(k, "drive")) {
      if (!drives_reset) {
        g_cfg.drive_count = 0;
        drives_reset = true;
      }
      if (g_cfg.drive_count < 16)
        snprintf(g_cfg.drives[g_cfg.drive_count++], SS_MAX_PATH, "%s", v);
    }
  }
  fclose(f);

  if (g_cfg.copy_threads < 1)
    g_cfg.copy_threads = 1;
  if (g_cfg.copy_threads > 16)
    g_cfg.copy_threads = 16;
  if (g_cfg.copy_buffer_mb < 1)
    g_cfg.copy_buffer_mb = 1;
  if (g_cfg.copy_buffer_mb > 64)
    g_cfg.copy_buffer_mb = 64;
  if (g_cfg.walk_depth < 2)
    g_cfg.walk_depth = 2;
  if (g_cfg.walk_depth > 8)
    g_cfg.walk_depth = 8;
}
