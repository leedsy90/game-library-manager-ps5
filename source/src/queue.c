/* Persistent batch queue. Jobs run one at a time in a background thread,
 * so nothing depends on the browser staying open. The queue is saved to
 * disk on every state change and resumes after a reboot/reload. */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcond = PTHREAD_COND_INITIALIZER;
static job_t g_jobs[SS_MAX_JOBS];
static int g_njobs = 0;
static int g_next_id = 1;
static bool g_paused = false;
static copy_progress_t g_progress; /* progress of the running copy */
static int g_running_id = 0;
static char g_qfile[SS_MAX_PATH];
static int g_batch_done = 0, g_batch_failed = 0;

static const char *type_name(job_type_t t) {
  return t == JOB_FIX    ? "fix"
         : t == JOB_COPY ? "copy"
         : t == JOB_MOVE ? "move"
                         : "extract";
}
static const char *state_name(job_state_t s) {
  switch (s) {
  case JS_QUEUED:
    return "queued";
  case JS_RUNNING:
    return "running";
  case JS_DONE:
    return "done";
  case JS_FAILED:
    return "failed";
  case JS_CANCELLED:
    return "cancelled";
  }
  return "?";
}

/* ---------------- persistence ---------------- */
static void clean_field(char *s) {
  for (; *s; s++)
    if (*s == '\t' || *s == '\n' || *s == '\r')
      *s = ' ';
}

static void save_locked(void) {
  char tmp[SS_MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_qfile);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  fprintf(f, "#glm-queue v1\n");
  for (int i = 0; i < g_njobs; i++) {
    job_t *j = &g_jobs[i];
    clean_field(j->message);
    clean_field(j->title);
    clean_field(j->verify);
    fprintf(f,
            "%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%llu\t%llu\t%llu\t%llu\t%s"
            "\t%d\t%s\n",
            j->id, (int)j->type, (int)j->state, j->src, j->dst, j->title_id,
            j->title, j->message, j->verify,
            (unsigned long long)j->bytes_total,
            (unsigned long long)j->files_total,
            (unsigned long long)j->started_ms,
            (unsigned long long)j->finished_ms, j->inner, j->flags, j->extras);
  }
  fclose(f);
  rename(tmp, g_qfile);
}

static char *next_field(char **p) {
  char *s = *p;
  if (!s)
    return (char *)"";
  char *t = strchr(s, '\t');
  if (t) {
    *t = 0;
    *p = t + 1;
  } else {
    char *nl = strchr(s, '\n');
    if (nl)
      *nl = 0;
    *p = NULL;
  }
  return s;
}

static void load(void) {
  FILE *f = fopen(g_qfile, "r");
  if (!f)
    return;
  char line[4096];
  while (fgets(line, sizeof(line), f) && g_njobs < SS_MAX_JOBS) {
    if (line[0] == '#')
      continue;
    char *p = line;
    job_t *j = &g_jobs[g_njobs];
    memset(j, 0, sizeof(*j));
    j->id = atoi(next_field(&p));
    j->type = (job_type_t)atoi(next_field(&p));
    j->state = (job_state_t)atoi(next_field(&p));
    snprintf(j->src, sizeof(j->src), "%s", next_field(&p));
    snprintf(j->dst, sizeof(j->dst), "%s", next_field(&p));
    snprintf(j->title_id, sizeof(j->title_id), "%s", next_field(&p));
    snprintf(j->title, sizeof(j->title), "%s", next_field(&p));
    snprintf(j->message, sizeof(j->message), "%s", next_field(&p));
    snprintf(j->verify, sizeof(j->verify), "%s", next_field(&p));
    j->bytes_total = strtoull(next_field(&p), NULL, 10);
    j->files_total = strtoull(next_field(&p), NULL, 10);
    j->started_ms = strtoull(next_field(&p), NULL, 10);
    j->finished_ms = strtoull(next_field(&p), NULL, 10);
    snprintf(j->inner, sizeof(j->inner), "%s", next_field(&p));
    j->flags = atoi(next_field(&p));
    snprintf(j->extras, sizeof(j->extras), "%s", next_field(&p));
    if (j->id <= 0 || !j->src[0])
      continue;
    if (j->state == JS_RUNNING) {
      /* interrupted: requeue; the staging copy is discarded and redone */
      j->state = JS_QUEUED;
      snprintf(j->message, sizeof(j->message),
               "Interrupted (console restarted?) - will run again");
    }
    if (j->id >= g_next_id)
      g_next_id = j->id + 1;
    g_njobs++;
  }
  fclose(f);
}

static job_t *find_locked(int id) {
  for (int i = 0; i < g_njobs; i++)
    if (g_jobs[i].id == id)
      return &g_jobs[i];
  return NULL;
}

/* ---------------- public API ---------------- */
static int queue_add_ex(job_type_t type, const char *src, const char *dst,
                        const char *title_id, const char *title,
                        const char *inner, const char *extras, int flags,
                        char *err, size_t errsz) {
  if (!path_exists(src)) {
    snprintf(err, errsz, "source not found: %s", src);
    return -1;
  }
  if (path_exists(dst)) {
    /* allowed only for the wrapper case: dst is the folder around src */
    char parent[SS_MAX_PATH];
    if (!(type == JOB_FIX && path_dirname(src, parent, sizeof(parent)) &&
          !strcmp(parent, dst))) {
      snprintf(err, errsz, "destination already exists: %s", dst);
      return -1;
    }
  }
  if (path_is_under(src, dst)) {
    snprintf(err, errsz, "destination is inside the source");
    return -1;
  }
  pthread_mutex_lock(&g_qlock);
  for (int i = 0; i < g_njobs; i++) {
    job_t *o = &g_jobs[i];
    if ((o->state == JS_QUEUED || o->state == JS_RUNNING) &&
        (!strcmp(o->src, src) || !strcmp(o->dst, dst))) {
      pthread_mutex_unlock(&g_qlock);
      snprintf(err, errsz, "already queued (job %d)", o->id);
      return -1;
    }
  }
  if (g_njobs >= SS_MAX_JOBS) {
    /* drop the oldest finished job to make room */
    int drop = -1;
    for (int i = 0; i < g_njobs; i++)
      if (g_jobs[i].state != JS_QUEUED && g_jobs[i].state != JS_RUNNING) {
        drop = i;
        break;
      }
    if (drop < 0) {
      pthread_mutex_unlock(&g_qlock);
      snprintf(err, errsz, "queue is full");
      return -1;
    }
    memmove(&g_jobs[drop], &g_jobs[drop + 1],
            sizeof(job_t) * (size_t)(g_njobs - drop - 1));
    g_njobs--;
  }
  job_t *j = &g_jobs[g_njobs++];
  memset(j, 0, sizeof(*j));
  j->id = g_next_id++;
  j->type = type;
  j->state = JS_QUEUED;
  snprintf(j->src, sizeof(j->src), "%s", src);
  snprintf(j->dst, sizeof(j->dst), "%s", dst);
  snprintf(j->title_id, sizeof(j->title_id), "%s", title_id ? title_id : "");
  snprintf(j->title, sizeof(j->title), "%s",
           title && *title ? title : path_basename(src));
  snprintf(j->inner, sizeof(j->inner), "%s", inner ? inner : "");
  snprintf(j->extras, sizeof(j->extras), "%s", extras ? extras : "");
  j->flags = flags;
  int id = j->id;
  save_locked();
  pthread_cond_broadcast(&g_qcond);
  pthread_mutex_unlock(&g_qlock);
  ss_log("queued job %d: %s %s -> %s", id, type_name(type), src, dst);
  return id;
}

int queue_add(job_type_t type, const char *src, const char *dst,
              const char *title_id, const char *title, char *err,
              size_t errsz) {
  return queue_add_ex(type, src, dst, title_id, title, NULL, NULL, 0, err,
                      errsz);
}

int queue_add_extract(const char *archive, const char *inner, const char *dst,
                      const char *extras, const char *title_id,
                      const char *title, int flags, char *err, size_t errsz) {
  return queue_add_ex(JOB_EXTRACT, archive, dst, title_id, title, inner,
                      extras, flags, err, errsz);
}

bool queue_cancel(int id) {
  bool ok = false;
  pthread_mutex_lock(&g_qlock);
  job_t *j = find_locked(id);
  if (j && j->state == JS_QUEUED) {
    j->state = JS_CANCELLED;
    snprintf(j->message, sizeof(j->message), "Cancelled before starting");
    ok = true;
  } else if (j && j->state == JS_RUNNING && g_running_id == id) {
    g_progress.cancel = 1;
    ok = true;
  }
  save_locked();
  pthread_mutex_unlock(&g_qlock);
  return ok;
}

/* Stop the batch: cancel whatever is running and everything still waiting.
 * Jobs that already finished are left exactly as they are - nothing that
 * completed is ever rolled back. Returns how many jobs were cancelled. */
int queue_cancel_all(void) {
  int n = 0;
  pthread_mutex_lock(&g_qlock);
  for (int i = 0; i < g_njobs; i++) {
    job_t *j = &g_jobs[i];
    if (j->state == JS_QUEUED) {
      j->state = JS_CANCELLED;
      snprintf(j->message, sizeof(j->message),
               "Cancelled before starting - nothing was touched");
      n++;
    } else if (j->state == JS_RUNNING && g_running_id == j->id) {
      g_progress.cancel = 1;
      n++;
    }
  }
  save_locked();
  pthread_mutex_unlock(&g_qlock);
  ss_log("cancel all: %d job(s)", n);
  return n;
}

bool queue_move_up(int id) {
  bool ok = false;
  pthread_mutex_lock(&g_qlock);
  for (int i = 1; i < g_njobs; i++) {
    if (g_jobs[i].id == id && g_jobs[i].state == JS_QUEUED) {
      /* swap with previous queued job */
      for (int k = i - 1; k >= 0; k--) {
        if (g_jobs[k].state == JS_QUEUED) {
          job_t t = g_jobs[k];
          g_jobs[k] = g_jobs[i];
          g_jobs[i] = t;
          ok = true;
          break;
        }
      }
      break;
    }
  }
  if (ok)
    save_locked();
  pthread_mutex_unlock(&g_qlock);
  return ok;
}

void queue_clear_finished(void) {
  pthread_mutex_lock(&g_qlock);
  int w = 0;
  for (int i = 0; i < g_njobs; i++)
    if (g_jobs[i].state == JS_QUEUED || g_jobs[i].state == JS_RUNNING)
      g_jobs[w++] = g_jobs[i];
  g_njobs = w;
  save_locked();
  pthread_mutex_unlock(&g_qlock);
}

void queue_set_paused(bool p) {
  pthread_mutex_lock(&g_qlock);
  g_paused = p;
  pthread_cond_broadcast(&g_qcond);
  pthread_mutex_unlock(&g_qlock);
}

bool queue_paused(void) {
  pthread_mutex_lock(&g_qlock);
  bool p = g_paused;
  pthread_mutex_unlock(&g_qlock);
  return p;
}

void queue_json(sbuf_t *b) {
  pthread_mutex_lock(&g_qlock);
  sb_printf(b, "{\"paused\":%s,\"running\":%d,\"jobs\":[",
            g_paused ? "true" : "false", g_running_id);
  for (int i = 0; i < g_njobs; i++) {
    job_t *j = &g_jobs[i];
    uint64_t bd = j->bytes_done, fd = j->files_done, bt = j->bytes_total,
             ft = j->files_total;
    if (j->id == g_running_id) {
      bd = g_progress.bytes_done;
      fd = g_progress.files_done;
      if (g_progress.bytes_total)
        bt = g_progress.bytes_total;
      if (g_progress.files_total)
        ft = g_progress.files_total;
    }
    sb_printf(b, "%s{\"id\":%d,\"type\":\"%s\",\"state\":\"%s\",", i ? "," : "",
              j->id, type_name(j->type), state_name(j->state));
    sb_puts(b, "\"src\":");
    sb_json_str(b, j->src);
    sb_puts(b, ",\"dst\":");
    sb_json_str(b, j->dst);
    sb_puts(b, ",\"title_id\":");
    sb_json_str(b, j->title_id);
    sb_puts(b, ",\"title\":");
    sb_json_str(b, j->title);
    sb_puts(b, ",\"phase\":");
    sb_json_str(b, j->phase);
    sb_puts(b, ",\"message\":");
    sb_json_str(b, j->message);
    sb_puts(b, ",\"verify\":");
    sb_json_str(b, j->verify);
    sb_printf(b,
              ",\"bytes_total\":%llu,\"bytes_done\":%llu,\"files_total\":%llu,"
              "\"files_done\":%llu,\"started_ms\":%llu,\"finished_ms\":%llu,"
              "\"now_ms\":%llu}",
              (unsigned long long)bt, (unsigned long long)bd,
              (unsigned long long)ft, (unsigned long long)fd,
              (unsigned long long)j->started_ms,
              (unsigned long long)j->finished_ms,
              (unsigned long long)now_ms());
  }
  sb_puts(b, "]}");
  pthread_mutex_unlock(&g_qlock);
}

/* ---------------- worker ---------------- */
static void set_phase(int id, const char *phase) {
  pthread_mutex_lock(&g_qlock);
  job_t *j = find_locked(id);
  if (j)
    snprintf(j->phase, sizeof(j->phase), "%s", phase);
  pthread_mutex_unlock(&g_qlock);
}

static void human_bytes(uint64_t b, char *out, size_t sz) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024 && i < 4) {
    v /= 1024;
    i++;
  }
  snprintf(out, sz, i ? "%.1f %s" : "%.0f %s", v, u[i]);
}

static void human_time(uint64_t ms, char *out, size_t sz) {
  uint64_t s = ms / 1000;
  if (s >= 3600)
    snprintf(out, sz, "%llu hours %llu minutes", (unsigned long long)(s / 3600),
             (unsigned long long)((s % 3600) / 60));
  else if (s >= 60)
    snprintf(out, sz, "%llu minutes %llu seconds", (unsigned long long)(s / 60),
             (unsigned long long)(s % 60));
  else
    snprintf(out, sz, "%llu seconds", (unsigned long long)s);
}

/* Ask SMP whether it now sees the game at dst. */
static void verify_with_smp(job_t *j, char *out, size_t outsz) {
  if (!smp_api_available(NULL, 0)) {
    snprintf(out, outsz, "SMP API not reachable - check SMP 1.7+ is loaded");
    return;
  }
  smp_api_scan();
  uint64_t deadline = now_ms() + (uint64_t)g_cfg.verify_timeout_s * 1000ull;
  smp_game_t *sg = calloc(SS_MAX_GAMES, sizeof(smp_game_t));
  if (!sg) {
    snprintf(out, outsz, "out of memory");
    return;
  }
  bool seen = false, installed = false;
  while (now_ms() < deadline) {
    sleep(5);
    int n = smp_api_games(sg, SS_MAX_GAMES);
    for (int i = 0; i < n; i++) {
      if (!strcmp(sg[i].path, j->dst)) {
        seen = true;
        installed = sg[i].installed;
        if (!j->title_id[0])
          snprintf(j->title_id, sizeof(j->title_id), "%s", sg[i].title_id);
        break;
      }
    }
    if (seen && installed)
      break;
  }
  free(sg);
  if (seen && installed)
    snprintf(out, outsz, "SMP sees it and it's installed");
  else if (seen)
    snprintf(out, outsz, "SMP found it, not installed yet");
  else
    snprintf(out, outsz, "SMP hasn't picked it up after %ds",
             g_cfg.verify_timeout_s);
}

/* Remove the original source after a verified copy. If SMP manages that
 * source, let SMP delete it (it unmounts and holds its safety gates). */
static int delete_source(job_t *j, char *err, size_t errsz) {
  if (j->title_id[0] && smp_api_available(NULL, 0)) {
    smp_game_t *sg = calloc(SS_MAX_GAMES, sizeof(smp_game_t));
    int n = sg ? smp_api_games(sg, SS_MAX_GAMES) : -1;
    bool managed = false;
    for (int i = 0; i < n; i++)
      if (!strcmp(sg[i].path, j->src) && sg[i].managed)
        managed = true;
    free(sg);
    if (managed) {
      int jid = 0;
      ss_log("job %d: asking SMP to delete original %s", j->id, j->src);
      if (!smp_api_delete(j->title_id, err, errsz, &jid))
        return -1;
      if (jid) {
        for (int tries = 0; tries < 3600; tries++) {
          char phase[32] = "";
          int result = 0;
          bool fin = false;
          sleep(1);
          if (!smp_api_job_status(jid, phase, sizeof(phase), &result, &fin))
            continue;
          if (fin) {
            if (strcmp(phase, "completed")) {
              snprintf(err, errsz, "SMP delete %s (errno %d)", phase, result);
              return -1;
            }
            return 0;
          }
        }
        snprintf(err, errsz, "SMP delete timed out");
        return -1;
      }
      return 0;
    }
  }
  ss_log("job %d: deleting original %s (copy verified)", j->id, j->src);
  if (rm_rf(j->src) != 0) {
    snprintf(err, errsz, "couldn't fully delete source: %s", strerror(errno));
    return -1;
  }
  return 0;
}

static int do_rename(job_t *j, char *err, size_t errsz) {
  char parent[SS_MAX_PATH];
  path_dirname(j->dst, parent, sizeof(parent));
  if (mkdir_p(parent) != 0) {
    snprintf(err, errsz, "can't create %s: %s", parent, strerror(errno));
    return -1;
  }
  char srcparent[SS_MAX_PATH];
  path_dirname(j->src, srcparent, sizeof(srcparent));
  if (!strcmp(srcparent, j->dst)) {
    /* wrapper case: <root>/X/<game>  ->  <root>/X */
    char tmp[SS_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s/.glm-fix-%d", parent, j->id);
    if (rename(j->src, tmp) != 0) {
      snprintf(err, errsz, "rename failed: %s", strerror(errno));
      return -1;
    }
    if (rmdir(j->dst) != 0) {
      /* wrapper still holds other files: keep them, renamed aside */
      char aside[SS_MAX_PATH];
      snprintf(aside, sizeof(aside), "%s.leftovers", j->dst);
      if (rename(j->dst, aside) != 0) {
        rename(tmp, j->src); /* put it back */
        snprintf(err, errsz, "wrapper folder not empty and can't be renamed");
        return -1;
      }
    }
    if (rename(tmp, j->dst) != 0) {
      snprintf(err, errsz, "final rename failed: %s", strerror(errno));
      return -1;
    }
    return 0;
  }
  if (rename(j->src, j->dst) != 0) {
    snprintf(err, errsz, "%s", strerror(errno));
    return errno == EXDEV ? EXDEV : -1;
  }
  return 0;
}

static void run_job(job_t *jcopy) {
  job_t j = *jcopy;
  char err[256] = "";
  int rc = 0;
  uint64_t t0 = now_ms();

  struct stat ss, dsp;
  char dparent[SS_MAX_PATH];
  path_dirname(j.dst, dparent, sizeof(dparent));
  mkdir_p(dparent);
  bool same_fs = stat(j.src, &ss) == 0 && stat(dparent, &dsp) == 0 &&
                 ss.st_dev == dsp.st_dev;

  memset(&g_progress, 0, sizeof(g_progress));
  if (j.type != JOB_EXTRACT)
    measure_tree_ex(j.src, &g_progress.bytes_total, &g_progress.files_total,
                    60000, NULL, 0);

  bool renamed = false;
  uint64_t copy_ms = 0;
  if (j.type == JOB_EXTRACT) {
    char stage[SS_MAX_PATH], xstage[SS_MAX_PATH] = "", xparent[SS_MAX_PATH];
    snprintf(stage, sizeof(stage), "%s/.glm-stage-%d", dparent, j.id);
    rm_rf(stage);
    if (j.extras[0] && path_dirname(j.extras, xparent, sizeof(xparent))) {
      mkdir_p(xparent);
      snprintf(xstage, sizeof(xstage), "%s/.glm-xstage-%d", xparent, j.id);
      rm_rf(xstage);
    }
    set_phase(j.id, "extracting");
    uint64_t c0 = now_ms();
    rc = archive_extract(j.src, j.inner, stage, xstage, &g_progress, err,
                         sizeof(err));
    copy_ms = now_ms() - c0;
    if (rc) {
      rm_rf(stage);
      if (xstage[0])
        rm_rf(xstage);
    } else {
      if (xstage[0]) {
        if (!is_dir(xstage)) {
          /* archive had nothing besides the game */
        } else {
          char xdst[SS_MAX_PATH];
          snprintf(xdst, sizeof(xdst), "%s", j.extras);
          for (int n = 2; path_exists(xdst) && n < 100; n++)
            snprintf(xdst, sizeof(xdst), "%s (%d)", j.extras, n);
          if (rename(xstage, xdst) != 0)
            ss_log("job %d: couldn't place extras at %s: %s (left at %s)",
                   j.id, xdst, strerror(errno), xstage);
          else
            ss_log("job %d: extras (fixes/backports/DLC) at %s", j.id, xdst);
        }
      }
      if (j.extras[0] && (j.flags & JOBF_SIBLINGS)) {
        set_phase(j.id, "copying extras");
        char serr[256] = "";
        int n = archive_copy_siblings(j.src, j.extras, &g_progress, serr,
                                      sizeof(serr));
        if (n < 0)
          ss_log("job %d: copying release-folder extras failed: %s", j.id,
                 serr);
        else
          ss_log("job %d: copied %d release-folder item(s) to %s", j.id, n,
                 j.extras);
      }
      set_phase(j.id, "finalising");
      if (rename(stage, j.dst) != 0) {
        snprintf(err, sizeof(err), "final rename failed: %s (files left at %s)",
                 strerror(errno), stage);
        rc = -1;
      } else if (j.flags & JOBF_DELETE_SOURCE) {
        char vols[64][SS_MAX_PATH];
        int nv = archive_volumes(j.src, vols, 64);
        for (int i = 0; i < nv; i++) {
          ss_log("job %d: deleting archive %s (extracted and CRC-checked)",
                 j.id, vols[i]);
          unlink(vols[i]);
        }
      }
    }
  } else if ((j.type == JOB_FIX || j.type == JOB_MOVE) && same_fs) {
    renamed = true;
    set_phase(j.id, "renaming");
    rc = do_rename(&j, err, sizeof(err));
    if (rc == 0) {
      g_progress.bytes_done = g_progress.bytes_total;
      g_progress.files_done = g_progress.files_total;
    }
  } else {
    /* copy into a hidden staging name next to the destination (same
     * filesystem, so the final step is an atomic rename and SMP never
     * sees a half-copied game) */
    char stage[SS_MAX_PATH];
    snprintf(stage, sizeof(stage), "%s/.glm-stage-%d", dparent, j.id);
    rm_rf(stage); /* leftover from an interrupted run */
    set_phase(j.id, "copying");
    uint64_t c0 = now_ms();
    rc = copy_tree(j.src, stage, &g_progress);
    copy_ms = now_ms() - c0;
    if (rc) {
      snprintf(err, sizeof(err), "%s", g_progress.error);
      rm_rf(stage);
    } else {
      set_phase(j.id, "verifying copy");
      char verr[256];
      int bad = verify_tree(j.src, stage, verr, sizeof(verr));
      if (bad) {
        snprintf(err, sizeof(err), "%d file(s) differ after copy (%s)", bad,
                 verr);
        rm_rf(stage);
        rc = -1;
      }
    }
    if (!rc && (j.type == JOB_MOVE || j.type == JOB_FIX)) {
      set_phase(j.id, "removing original");
      if (delete_source(&j, err, sizeof(err)) != 0) {
        /* keep the original, discard the copy -> no duplicates */
        rm_rf(stage);
        rc = -1;
      }
    }
    if (!rc) {
      set_phase(j.id, "finalising");
      if (rename(stage, j.dst) != 0) {
        snprintf(err, sizeof(err), "final rename failed: %s (copy left at %s)",
                 strerror(errno), stage);
        rc = -1;
      }
    }
  }

  uint64_t elapsed = now_ms() - t0;
  char verify[64] = "";
  if (!rc && !g_progress.cancel) {
    set_phase(j.id, "checking with SMP");
    verify_with_smp(&j, verify, sizeof(verify));
  }

  pthread_mutex_lock(&g_qlock);
  job_t *live = find_locked(j.id);
  if (live) {
    live->finished_ms = now_ms();
    live->bytes_done = g_progress.bytes_done;
    live->files_done = g_progress.files_done;
    live->bytes_total = g_progress.bytes_total;
    live->files_total = g_progress.files_total;
    snprintf(live->title_id, sizeof(live->title_id), "%s", j.title_id);
    snprintf(live->verify, sizeof(live->verify), "%s", verify);
    live->phase[0] = 0;
    if (g_progress.cancel && rc) {
      live->state = JS_CANCELLED;
      snprintf(live->message, sizeof(live->message),
               "Cancelled - partial copy removed, original untouched");
    } else if (rc) {
      live->state = JS_FAILED;
      snprintf(live->message, sizeof(live->message), "%s", err);
    } else {
      live->state = JS_DONE;
      char sz[32], tm[64];
      human_bytes(g_progress.bytes_total, sz, sizeof(sz));
      unsigned long long nf = (unsigned long long)g_progress.files_total;
      if (renamed) {
        snprintf(live->message, sizeof(live->message),
                 "Moved instantly (same drive): %s, %llu file%s", sz, nf,
                 nf == 1 ? "" : "s");
      } else {
        human_time(elapsed, tm, sizeof(tm));
        double secs = copy_ms ? (double)copy_ms / 1000.0 : 0.001;
        snprintf(live->message, sizeof(live->message),
                 "%s, %llu file%s in %s (%s at %.0f MB/s%s)", sz, nf,
                 nf == 1 ? "" : "s", tm,
                 j.type == JOB_EXTRACT ? "extracted" : "copied",
                 (double)g_progress.bytes_total / 1048576.0 / secs,
                 j.type == JOB_EXTRACT ? ", all CRCs OK" : "");
      }
    }
    *jcopy = *live;
  }
  g_running_id = 0;
  save_locked();
  pthread_mutex_unlock(&g_qlock);
}

static void *worker_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_qlock);
    job_t *next = NULL;
    while (!next) {
      if (!g_paused)
        for (int i = 0; i < g_njobs; i++)
          if (g_jobs[i].state == JS_QUEUED) {
            next = &g_jobs[i];
            break;
          }
      if (!next)
        pthread_cond_wait(&g_qcond, &g_qlock);
    }
    next->state = JS_RUNNING;
    next->started_ms = now_ms();
    next->message[0] = 0;
    g_running_id = next->id;
    job_t j = *next;
    int remaining = 0;
    for (int i = 0; i < g_njobs; i++)
      if (g_jobs[i].state == JS_QUEUED)
        remaining++;
    save_locked();
    pthread_mutex_unlock(&g_qlock);

    char msg[256];
    snprintf(msg, sizeof(msg), "%s (%d more queued)", j.title, remaining);
    notify_event(j.type == JOB_FIX       ? "Fixing"
                 : j.type == JOB_COPY    ? "Copying"
                 : j.type == JOB_EXTRACT ? "Extracting"
                                         : "Moving",
                 msg, false);

    run_job(&j);

    char body[512];
    if (j.state == JS_DONE) {
      g_batch_done++;
      snprintf(body, sizeof(body), "%s - %s.%s%s%s", j.title, j.message,
               j.verify[0] ? " " : "", j.verify, j.verify[0] ? "." : "");
    } else {
      g_batch_failed++;
      snprintf(body, sizeof(body), "%s - %s.", j.title, j.message);
    }

    /* what's next? */
    pthread_mutex_lock(&g_qlock);
    char nexttitle[128] = "";
    int left = 0;
    for (int i = 0; i < g_njobs; i++)
      if (g_jobs[i].state == JS_QUEUED) {
        if (!left)
          snprintf(nexttitle, sizeof(nexttitle), "%s", g_jobs[i].title);
        left++;
      }
    bool paused = g_paused;
    pthread_mutex_unlock(&g_qlock);

    size_t l = strlen(body);
    if (left && !paused)
      snprintf(body + l, sizeof(body) - l, " Next: %s (%d left)", nexttitle,
               left);
    else if (left && paused)
      snprintf(body + l, sizeof(body) - l, " Queue paused, %d left.", left);

    const char *title = j.state == JS_DONE        ? "Done"
                        : j.state == JS_CANCELLED ? "Cancelled"
                                                  : "FAILED";
    notify_event(title, body, true);

    if (!left) {
      char fin[128];
      snprintf(fin, sizeof(fin), "%d done, %d failed", g_batch_done,
               g_batch_failed);
      notify_event("Queue finished", fin, true);
      g_batch_done = g_batch_failed = 0;
      scan_start_async(); /* refresh the library view */
    }
  }
  return NULL;
}

void queue_init(void) {
  snprintf(g_qfile, sizeof(g_qfile), "%s/queue.tsv", g_cfg.data_dir);
  pthread_mutex_lock(&g_qlock);
  load();
  pthread_mutex_unlock(&g_qlock);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024); /* PS5 default thread stacks are small */
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, worker_main, NULL);
  pthread_attr_destroy(&a);
}
