/* vita_host.c -- PlayStation Vita start-up.
 *
 * Everything the game reads "beside the executable" lives in
 * ux0:/data/babel-recomp/ (the ROM, saves, config, languages/, msu1/, mods/).
 * The files bundled in the .vpk (app0:/bundle/: i18n/, mods/preloaded/) are
 * copied there on the first launch of each version; the player's own files
 * are never touched. The presenter is SDL's (GXM, hardware scaled). */
#if defined(__vita__)
#include <psp2/power.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define kDataDir "ux0:/data/babel-recomp"
#define kBundle "app0:/bundle"

/* The heap newlib may grow to, and the main thread's stack (the host keeps
 * several kilobyte-sized buffers on it). */
int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

static void CopyFile(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return;
  FILE *out = fopen(to, "wb");
  if (out) {
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(out);
  }
  fclose(in);
}

static void CopyTree(const char *from, const char *to) {
  DIR *d = opendir(from);
  if (!d) return;
  mkdir(to, 0777);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/%s", from, e->d_name);
    snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
    struct stat st;
    if (stat(src, &st) == 0 && S_ISDIR(st.st_mode))
      CopyTree(src, dst);
    else
      CopyFile(src, dst);
  }
  closedir(d);
}

/* Copies the bundled data once per version (a stamp file records it). */
static void InstallBundle(const char *version) {
  char stamp[128] = "";
  FILE *f = fopen(kDataDir "/.bundled", "r");
  if (f) {
    if (!fgets(stamp, sizeof(stamp), f)) stamp[0] = 0;
    fclose(f);
  }
  if (!strcmp(stamp, version)) return;
  CopyTree(kBundle, kDataDir);
  f = fopen(kDataDir "/.bundled", "w");
  if (f) {
    fputs(version, f);
    fclose(f);
  }
}

/* Returns the folder the game works in; the caller makes it the working
 * directory. A console has no environment, so env.txt (KEY=VALUE lines), if
 * present, stands in for it: the same diagnostic switches as on a PC. */
const char *Vita_Startup(const char *version) {
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);
  mkdir(kDataDir, 0777);
  InstallBundle(version ? version : "dev");
  FILE *f = fopen(kDataDir "/env.txt", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = 0;
      char *eq = strchr(line, '=');
      if (line[0] == '#' || !eq) continue;
      *eq = 0;
      setenv(line, eq + 1, 1);
    }
    fclose(f);
  }
  return kDataDir;
}
#endif /* __vita__ */
