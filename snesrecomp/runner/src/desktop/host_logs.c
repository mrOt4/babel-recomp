/* host_logs.c -- the player-facing logs folder (see host_logs.h).
 *
 * Everything the host and the game print to stderr/stdout goes to
 * logs/babel-recomp.log beside the executable, written unbuffered so a crash
 * still leaves every line before it. When the process had a usable stderr
 * (a terminal, a pipe, a test harness) the log is mirrored back to it by a
 * small thread that follows the file, so nothing changes for whoever reads
 * the console. A Windows GUI build has no stderr at all: there the file is
 * the only record, which is the reason this exists.
 *
 * The Switch keeps its own scheme (switch_crash.c: babel-recomp.log beside
 * the .nro plus logs/); this module only adds the header there. */
#include "host_logs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdl_compat.h"
#include "../host_paths.h"
#include "../host_report.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define HL_DUP _dup
#define HL_WRITE _write
#define HL_FILENO _fileno
#else
#include <sys/stat.h>
#include <unistd.h>
#define HL_DUP dup
#define HL_WRITE write
#define HL_FILENO fileno
#endif

static char s_dir[1024];
static char s_log[1100], s_flag[1100];
static int s_active;
static int s_mirror_fd = -1;
static FILE *s_follow;
static SDL_Thread *s_thread;
static volatile int s_stop;
static SDL_mutex *s_drain_lock;

static void MakeDir(const char *p) {
#if defined(_WIN32)
  _mkdir(p);
#else
  mkdir(p, 0755);
#endif
}

static int Writable(const char *dir) {
  char probe[1100];
  snprintf(probe, sizeof(probe), "%s/.probe", dir);
  FILE *f = fopen(probe, "wb");
  if (!f) return 0;
  fclose(f);
  remove(probe);
  return 1;
}

static void CopyFile(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return;
  FILE *out = fopen(to, "wb");
  if (out) {
    char b[8192];
    size_t k;
    while ((k = fread(b, 1, sizeof(b), in)) > 0) fwrite(b, 1, k, out);
    fclose(out);
  }
  fclose(in);
}

static int FileExists(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static void Stamp(char *out, size_t cap, const char *fmt) {
  time_t t = time(NULL);
  struct tm tmv;
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  strftime(out, cap, fmt, &tmv);
}

/* Copy whatever the log gained since the last call to the old stderr. */
static void Drain(void) {
  if (!s_follow || s_mirror_fd < 0) return;
  if (s_drain_lock) SDL_LockMutex(s_drain_lock);
  char b[4096];
  size_t k;
  clearerr(s_follow);
  while ((k = fread(b, 1, sizeof(b), s_follow)) > 0) {
    const char *p = b;
    while (k > 0) {
      const int w = (int)HL_WRITE(s_mirror_fd, p, (unsigned)k);
      if (w <= 0) { k = 0; break; }
      p += w;
      k -= (size_t)w;
    }
  }
  if (s_drain_lock) SDL_UnlockMutex(s_drain_lock);
}

static int SDLCALL MirrorThread(void *ud) {
  (void)ud;
  while (!s_stop) {
    Drain();
    SDL_Delay(25);
  }
  return 0;
}

static void AtExit(void) {
  fflush(stdout);
  fflush(stderr);
  if (s_thread) {
    s_stop = 1;
    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
  }
  Drain();
  if (s_flag[0]) remove(s_flag);
}

static const char *PlatformName(void) {
#if defined(__SWITCH__)
  return "Nintendo Switch";
#elif defined(__ANDROID__)
  return "Android";
#elif defined(__3DS__)
  return "Nintendo 3DS";
#elif defined(__vita__)
  return "PlayStation Vita";
#elif defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "unknown";
#endif
}

void HostLogs_Init(const char *game_name, const char *build) {
  char stamp[64];
#if defined(__SWITCH__)
  /* switch_crash.c already redirected the log and owns logs/. */
  snprintf(s_dir, sizeof(s_dir), "logs");
#else
  if (getenv("SNESRECOMP_LOG_FILE") && !strcmp(getenv("SNESRECOMP_LOG_FILE"), "0"))
    return;
  /* Beside the executable; where that is read-only (an installed copy),
   * SDL's per-user folder. Android has already chdir'd to its files dir. */
#if defined(__ANDROID__)
  snprintf(s_dir, sizeof(s_dir), "logs");
#else
  if (!snesrecomp_exe_dir_path("logs", s_dir, sizeof(s_dir)))
    snprintf(s_dir, sizeof(s_dir), "logs");
#endif
  MakeDir(s_dir);
  if (!Writable(s_dir)) {
    char *pref = SDL_GetPrefPath("babel-recomp", game_name ? game_name : "game");
    if (pref) {
      snprintf(s_dir, sizeof(s_dir), "%slogs", pref);
      SDL_free(pref);
      MakeDir(s_dir);
    }
  }
  snprintf(s_log, sizeof(s_log), "%s/babel-recomp.log", s_dir);
  snprintf(s_flag, sizeof(s_flag), "%s/session-active.flag", s_dir);
  {
    char prev[1100];
    snprintf(prev, sizeof(prev), "%s/previous-session.log", s_dir);
    CopyFile(s_log, prev);
    /* Neither a clean exit nor a crash report: a hang, or it was killed. */
    if (FileExists(s_flag)) {
      char note[1100];
      snprintf(note, sizeof(note), "%s/unclean-previous.log", s_dir);
      FILE *f = fopen(note, "wb");
      if (f) {
        fprintf(f, "The previous session did not end cleanly (a crash, a hang, or the\n"
                   "game was closed by force). Its log is previous-session.log.\n");
        fclose(f);
      }
    }
  }
  /* Keep the old stderr for the mirror, then send both streams to the file. */
  {
    const int fd = HL_FILENO(stderr);
    if (fd >= 0) s_mirror_fd = HL_DUP(fd);
  }
  /* Truncate, then BOTH streams in append mode: two "w"/"a" handles keep
   * separate offsets and overwrite each other's lines. */
  {
    FILE *t = fopen(s_log, "wb");
    if (t) fclose(t);
  }
  if (!freopen(s_log, "a", stderr)) {
    s_mirror_fd = -1;
    return;
  }
  setvbuf(stderr, NULL, _IONBF, 0);
#if defined(__3DS__)
  /* The SD card's devoptab ignores append mode, so two handles would write
   * over each other: stdout becomes the same stream (newlib allows it). */
  stdout = stderr;
#else
  if (freopen(s_log, "a", stdout))
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
  if (s_mirror_fd >= 0) {
    s_follow = fopen(s_log, "rb");
    s_drain_lock = SDL_CreateMutex();
    if (s_follow) s_thread = SDL_CreateThread(MirrorThread, "log-mirror", NULL);
  }
  {
    FILE *f = fopen(s_flag, "wb");
    if (f) fclose(f);
  }
  atexit(AtExit);
#endif
  s_active = 1;
  host_report_set_output_directory(s_dir);
  Stamp(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S");
  fprintf(stderr, "==== %s | build %s | %s | %s ====\n", game_name ? game_name : "babel-recomp",
          build ? build : "dev", PlatformName(), stamp);
#if !defined(__SWITCH__)
  fprintf(stderr, "logs: %s\n", s_dir);
#endif
}

const char *HostLogs_Dir(void) { return s_active ? s_dir : NULL; }

/* From a crash handler: best effort (stdio in a handler is not strictly
 * safe, but the process is going down and this is the record that matters). */
void HostLogs_OnCrash(const char *what) {
  if (!s_active) return;
#if !defined(__SWITCH__)
  char stamp[64], dst[1100];
  fprintf(stderr, "\n==== CRASH (%s): copied to %s/crash-latest.log ====\n",
          what ? what : "?", s_dir);
  fflush(stderr);
  Drain();
  snprintf(dst, sizeof(dst), "%s/crash-latest.log", s_dir);
  CopyFile(s_log, dst);
  Stamp(stamp, sizeof(stamp), "%Y%m%d_%H%M%S");
  snprintf(dst, sizeof(dst), "%s/crash-%s.log", s_dir, stamp);
  CopyFile(s_log, dst);
  /* The post-mortem JSON (registers, stacks, trace rings) beside it. */
  snprintf(dst, sizeof(dst), "%s/crash-latest-report.json", s_dir);
  CopyFile("last_run_report.json", dst);
  /* A crash is reported: not also "unclean". */
  if (s_flag[0]) remove(s_flag);
#else
  (void)what;
#endif
}
