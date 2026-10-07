/* Game Library Manager - batch organiser / mover companion for ShadowMount Plus (PS5)
 * Copyright (C) 2026
 * Licensed under the GNU General Public License v3 or later.
 */
#ifndef SS_COMMON_H
#define SS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define SS_VERSION "0.3.9"
#define SS_MAX_PATH 1024
#define SS_MAX_ROOTS 64
#define SS_MAX_GAMES 2048
#define SS_MAX_JOBS 256
#define SS_TILE_ID "FAKE57417" /* home-screen tile title id */

/* ------------------------------------------------------------------ */
/* String buffer                                                       */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf_t;

void sb_init(sbuf_t *b);
void sb_free(sbuf_t *b);
void sb_putc(sbuf_t *b, char c);
void sb_puts(sbuf_t *b, const char *s);
void sb_printf(sbuf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sb_json_str(sbuf_t *b, const char *s); /* writes "escaped" */

/* ------------------------------------------------------------------ */
/* Logging                                                             */
void ss_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ss_log_open(const char *path);

/* ------------------------------------------------------------------ */
/* Utility                                                             */
const char *path_basename(const char *p);
bool path_dirname(const char *p, char *out, size_t outsz);
bool path_join(char *out, size_t outsz, const char *a, const char *b);
bool path_is_under(const char *root, const char *p); /* p == root or child */
bool path_exists(const char *p);
bool is_dir(const char *p);
bool is_file(const char *p);
int mkdir_p(const char *p);
int rm_rf(const char *p);
bool ends_with_ci(const char *s, const char *suffix);
uint64_t now_ms(void);
int read_small_file(const char *path, char *buf, size_t bufsz);
bool parse_param_json(const char *json, char *title_id, size_t tidsz,
                      char *title, size_t titlesz);
bool looks_like_title_id(const char *s);

/* ------------------------------------------------------------------ */
/* Config                                                              */
typedef struct {
  int http_port;
  char data_dir[SS_MAX_PATH];   /* where queue/log/config live */
  char ntfy_url[512];           /* e.g. https://ntfy.sh/my-secret-topic */
  char webhook_url[512];        /* e.g. http://192.168.1.10:8123/api/webhook/x */
  bool toasts;
  bool open_browser;             /* open the PS5 browser on start */
  bool tile;                     /* install the home-screen tile */
  char tile_root[SS_MAX_PATH];   /* SMP scan folder to put the tile in */
  int copy_threads;             /* parallel files during copy */
  int copy_buffer_mb;           /* per-thread buffer */
  int verify_timeout_s;         /* how long to wait for SMP to see a game */
  char smp_host[64];
  int smp_port;
  char smp_config[SS_MAX_PATH]; /* /data/shadowmount/config.ini */
  /* drives to walk looking for games (mount roots) */
  int drive_count;
  char drives[16][SS_MAX_PATH];
  int walk_depth;               /* how deep to look for misplaced games */
} ss_config_t;

extern ss_config_t g_cfg;
void config_defaults(void);
void config_load(const char *path);

/* ------------------------------------------------------------------ */
/* Platform                                                            */
void plat_init(void);
void plat_toast(const char *msg);
bool plat_open_browser(const char *url); /* PS5: open the web browser */
/* POST body to url. content_type may be NULL. Returns HTTP status or -1. */
int plat_http_post(const char *url, const char *content_type,
                   const char *body, size_t len);
typedef struct {
  char mount[SS_MAX_PATH];
  char fstype[32];
  uint64_t total;
  uint64_t avail;
  uint64_t dev;
} drive_info_t;
int plat_list_drives(drive_info_t *out, int max);

/* Raw localhost HTTP (used for SMP API on 127.0.0.1) */
int http_request_raw(const char *host, int port, const char *method,
                     const char *path, const char *ctype, const char *body,
                     sbuf_t *resp_body, int timeout_ms);

/* ------------------------------------------------------------------ */
/* Notifications (toast + push)                                        */
void notify_event(const char *title, const char *msg, bool push);

/* ------------------------------------------------------------------ */
/* SMP scan model                                                      */
typedef struct {
  int count;
  char roots[SS_MAX_ROOTS][SS_MAX_PATH];
  int depth;      /* 1 or 2 */
  bool from_api;  /* settings came from SMP API */
  bool from_cfg;  /* settings came from config.ini */
} smp_model_t;

void smp_model_load(smp_model_t *m);

/* SMP API */
typedef struct {
  char title_id[16];
  char path[SS_MAX_PATH];
  char title_name[128];
  char source_type[16];
  bool mounted;
  bool installed;
  bool managed;
  bool source_available;
} smp_game_t;

bool smp_api_available(char *version_out, size_t vsz);

typedef enum {
  SMP_OK = 0,          /* 1.7+ with the API features we need */
  SMP_NOT_INSTALLED,   /* no /data/shadowmount at all */
  SMP_API_DISABLED,    /* api_enabled=0 in SMP's config.ini */
  SMP_NO_API,          /* not loaded, or a version before the API (<1.7) */
  SMP_TOO_OLD,         /* API answers but version < 1.7 */
  SMP_MISSING_FEATURES /* API answers but lacks a capability we use */
} smp_status_t;
smp_status_t smp_check(char *version, size_t vsz, char *msg, size_t msgsz);
const char *smp_status_name(smp_status_t s);
int smp_api_games(smp_game_t *out, int max); /* -1 on failure */
bool smp_api_scan(void);
bool smp_api_scan_ex(char *deferred_reason, size_t rsz);
bool smp_api_delete(const char *title_id, char *err, size_t errsz,
                    int *job_id);
bool smp_api_job_status(int job_id, char *phase, size_t psz, int *result,
                        bool *finished);
bool smp_api_manual_add(const char *path);
/* set scan_depth (1 or 2) in SMP's config.ini; SMP reloads it live */
bool smp_set_scan_depth(int depth, char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* Image peeking (read-only, no mount)                                 */
typedef enum {
  PEEK_UNSUPPORTED = 0, /* pfs / pfsc: can't look inside */
  PEEK_OK,              /* sce_sys/param.json at image root */
  PEEK_WRAPPED,         /* game is inside one extra top-level folder */
  PEEK_NO_GAME,         /* no param.json found */
  PEEK_ERROR            /* couldn't parse */
} peek_result_t;

peek_result_t image_peek(const char *path, char *title_id, size_t tidsz,
                         char *title, size_t titlesz, char *wrapper,
                         size_t wrapsz);
const char *image_kind(const char *name); /* "ufs","exfat","pfs","pfsc",NULL */
int image_list_dir(const char *path, const char *subdir, char *out,
                   size_t outsz);

/* ------------------------------------------------------------------ */
/* Scanner                                                             */
typedef enum {
  ISSUE_NONE = 0,
  ISSUE_TOO_DEEP,         /* game is below the scan depth */
  ISSUE_WRAPPED_FOLDER,   /* extra wrapper folder around game */
  ISSUE_OUTSIDE_ROOTS,    /* not under any scan root */
  ISSUE_IMAGE_WRAPPED,    /* image has extra top-level folder inside */
  ISSUE_IMAGE_NO_GAME,    /* image has no param.json */
  ISSUE_DUPLICATE,        /* same title id in more than one source */
  ISSUE_BACKPORTS_PLACE,  /* game sitting in a backports folder */
  ISSUE_HIDDEN_NAME,      /* name starts with '.', SMP skips it */
  ISSUE_ARCHIVE,          /* game still packed in .7z/.rar/.zip */
  ISSUE_PKG               /* .pkg installer - SMP doesn't load these */
} issue_t;

typedef struct {
  int id;
  char path[SS_MAX_PATH];
  char title_id[16];
  char title[128];
  char kind[8];          /* folder, ufs, exfat, pfs, pfsc, pkg, archive */
  uint64_t size;
  uint64_t files;
  uint64_t dev;
  char drive[SS_MAX_PATH];
  bool visible;          /* SMP should find it where it is */
  bool smp_sees;         /* SMP API reports this path */
  bool smp_mounted;
  issue_t issue;
  char detail[256];
  char fix_dst[SS_MAX_PATH];  /* suggested destination (full path) */
  bool has_fakelib;
  int size_state;          /* 0 = not measured yet, 1 = done, 2 = partial */
  char fakelib_files[512];
  int savefix;            /* 0 = no save library in fakelib,
                             1 = save library active (fix available),
                             2 = save fix applied (library disabled) */
} game_t;

typedef struct {
  pthread_mutex_t lock;
  bool running;
  uint64_t started_ms;
  uint64_t finished_ms;
  int count;
  game_t games[SS_MAX_GAMES];
  smp_model_t model;
  bool smp_api;
  char smp_version[64];
  int smp_status;
  char smp_message[256];
  int drive_count;
  drive_info_t drives[16];
  char global_fakelib[1024];
  int global_savefix;
  /* live progress while running (written without the lock; display only) */
  char current[SS_MAX_PATH];
  volatile unsigned long dirs_seen;
  volatile int found;
  bool sizing;            /* folder sizes being measured after a scan */
} scan_state_t;

extern scan_state_t g_scan;
void scan_start_async(void);
void scan_run(void);
bool scan_get_game(int id, game_t *out);
const char *issue_name(issue_t i);
/* Save fix: rename libSceSaveData*.sprx in a fakelib folder to .disabled
 * (undo = rename back). Returns files changed, -1 on error. */
int savefix_apply(const char *fakelib_dir, bool undo, char *err, size_t errsz);
int savefix_state(const char *fakelib_dir);
bool is_save_lib(const char *name, bool *disabled);
int image_rename_in_dir(const char *path, const char *dirname,
                        const char *from, const char *to, char *err,
                        size_t errsz);
/* save fix inside an .exfat image (refuses if SMP has it mounted) */
int savefix_apply_image(const game_t *g, bool undo, char *err, size_t errsz);
/* Root that SMP would scan on a given drive, preferring <drive>/homebrew. */
bool best_root_on_drive(const char *drive, char *out, size_t outsz);

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
typedef enum { JOB_FIX = 0, JOB_COPY, JOB_MOVE, JOB_EXTRACT } job_type_t;
#define JOBF_DELETE_SOURCE 1 /* extract: delete the archive afterwards */
#define JOBF_SIBLINGS 2      /* extract: also copy release-folder extras */
typedef enum {
  JS_QUEUED = 0,
  JS_RUNNING,
  JS_DONE,
  JS_FAILED,
  JS_CANCELLED
} job_state_t;

typedef struct {
  int id;
  job_type_t type;
  job_state_t state;
  char src[SS_MAX_PATH];
  char dst[SS_MAX_PATH];   /* final full destination path */
  char title_id[16];
  char title[128];
  char phase[32];
  char message[256];
  char verify[64];         /* verification summary */
  char inner[SS_MAX_PATH]; /* extract: game folder/image inside archive */
  char extras[SS_MAX_PATH]; /* extract: where fixes/backports/DLC go */
  int flags;
  uint64_t bytes_total, bytes_done;
  uint64_t files_total, files_done;
  uint64_t started_ms, finished_ms;
} job_t;

void queue_init(void);
int queue_add(job_type_t type, const char *src, const char *dst,
              const char *title_id, const char *title, char *err,
              size_t errsz);
bool queue_cancel(int id);
void queue_clear_finished(void);
void queue_set_paused(bool p);
bool queue_paused(void);
void queue_json(sbuf_t *b);
bool queue_move_up(int id);
int queue_add_extract(const char *archive, const char *inner, const char *dst,
                      const char *extras, const char *title_id,
                      const char *title, int flags, char *err, size_t errsz);
int queue_cancel_all(void); /* running + queued; finished jobs untouched */

/* ------------------------------------------------------------------ */
/* Copy engine                                                         */
typedef struct {
  volatile uint64_t bytes_done;
  volatile uint64_t files_done;
  uint64_t bytes_total;
  uint64_t files_total;
  volatile int cancel;
  char error[256];
} copy_progress_t;

/* Copy a file or tree src -> dst (dst must not exist). */
int copy_tree(const char *src, const char *dst, copy_progress_t *pr);
int measure_tree(const char *src, uint64_t *bytes, uint64_t *files);
/* -1 if it gave up after max_ms (totals are then partial) */
int measure_tree_ex(const char *src, uint64_t *bytes, uint64_t *files,
                    uint64_t max_ms, char *progress, size_t progress_sz);
int verify_tree(const char *src, const char *dst, char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* Home-screen tile                                                    */
bool tile_install(bool force);
void tile_install_async(bool force);
void tile_json(sbuf_t *b);
void tile_open_page_async(void);
void tile_retry_if_waiting(bool smp_ok);

/* ------------------------------------------------------------------ */
/* .7z archives                                                        */
void archive_info_json(const char *path, sbuf_t *b);
/* pkginfo.c: read-only summary of a PS5 .pkg (no file data) */
void pkg_inspect_json(const char *path, sbuf_t *b);
/* main.c: the console's LAN address ("" if none) */
void ss_local_ip(char *out, size_t sz);
int archive_extract(const char *archive, const char *inner, const char *stage,
                    const char *extras, copy_progress_t *pr, char *err,
                    size_t errsz);
int archive_volumes(const char *path, char out[][SS_MAX_PATH], int max);
int archive_copy_siblings(const char *archive, const char *dest,
                          copy_progress_t *pr, char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* HTTP server                                                         */
void httpd_start(int port);
extern const char g_index_html[];

#endif
