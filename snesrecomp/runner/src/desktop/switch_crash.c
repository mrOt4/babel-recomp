/* switch_crash.c -- crash reports on the Switch.
 *
 * Desktop ports catch a crash with signals or SEH (post_mortem.c,
 * host_report.c); on Horizon a fault arrives at libnx's exception handler,
 * so without this a crash leaves nothing behind but Atmosphere's report.
 * After the ALEKS Switch ports' two-stage reporter:
 *
 *   stage A  __libnx_exception_handler: registers + the breadcrumb ring are
 *            copied into a static record and written raw (open/write, no
 *            stdio, no malloc) to logs/crash_pending.bin. It then returns,
 *            and libnx still hands the crash to the system (creport).
 *   stage B  next boot: the record becomes logs/crash-latest.log, readable,
 *            with the load-bias reference addr2line needs.
 *
 * Also: the previous session's babel-recomp.log is kept as
 * logs/previous-session.log, and a session that ended with neither a clean
 * exit nor a crash record (a hang the player had to close) is reported in
 * logs/unclean-previous.log.
 *
 * Best effort, as ALEKS notes: writing to the SD card inside the handler
 * takes filesystem locks; a crash inside the filesystem itself can lose the
 * record. */
#if defined(__SWITCH__)
#include <switch.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host_report.h"

#define CRASH_MAGIC 0x53435242u   /* "BRCS" */
#define CRASH_VERSION 1u
#define RING_MAX (48 * 1024)

typedef struct CrashRecord {
  uint32_t magic, version, size;
  uint32_t error_desc, pstate, esr, have_regs, ring_entries, ring_entry_size, ring_writes;
  uint64_t gprs[29];
  uint64_t fp, lr, sp, pc, far;
  uint64_t bias_ref;                /* runtime address of SwitchCrash_Init */
  char build[64];
  uint8_t ring[RING_MAX];
} CrashRecord;

static CrashRecord s_rec;
static char s_root[512];
static char s_pending[600], s_report[600], s_flag[600], s_unclean[600];
static char s_build[64];

/* Our own exception stack (the default is small and shared; a stack
 * overflow would have nowhere to run). Overrides libnx's weak symbols. */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
/* With a debugger attached, the debugger gets the exception. */
u32 __nx_exception_ignoredebug = 0;

static volatile int s_in_handler;

void SwitchCrash_Init(const char *root, const char *build);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  if (s_in_handler) return;
  s_in_handler = 1;
  s_rec.magic = CRASH_MAGIC;
  s_rec.version = CRASH_VERSION;
  s_rec.size = sizeof(s_rec);
  s_rec.bias_ref = (uint64_t)(uintptr_t)&SwitchCrash_Init;
  memcpy(s_rec.build, s_build, sizeof(s_rec.build));
  if (ctx) {
    s_rec.error_desc = ctx->error_desc;
    s_rec.pstate = ctx->pstate;
    s_rec.esr = ctx->esr;
    for (int i = 0; i < 29; i++) s_rec.gprs[i] = ctx->cpu_gprs[i].x;
    s_rec.fp = ctx->fp.x;
    s_rec.lr = ctx->lr.x;
    s_rec.sp = ctx->sp.x;
    s_rec.pc = ctx->pc.x;
    s_rec.far = ctx->far.x;
    s_rec.have_regs = 1;
  }
  {
    int n = 0, sz = 0;
    unsigned w = 0;
    const void *ring = host_report_ring_raw(&n, &sz, &w);
    size_t bytes = (size_t)n * (size_t)sz;
    if (ring && bytes <= sizeof(s_rec.ring)) {
      memcpy(s_rec.ring, ring, bytes);
      s_rec.ring_entries = (uint32_t)n;
      s_rec.ring_entry_size = (uint32_t)sz;
      s_rec.ring_writes = w;
    }
  }
  int fd = open(s_pending, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    const uint8_t *p = (const uint8_t *)&s_rec;
    size_t left = sizeof(s_rec);
    while (left) {
      ssize_t k = write(fd, p, left);
      if (k <= 0) break;
      p += k;
      left -= (size_t)k;
    }
    fsync(fd);
    close(fd);
  }
  /* Returning lets libnx break into the system's own crash report. */
}

static const char *ErrorName(uint32_t d) {
  switch (d) {
  case 0x100: return "InstructionAbort";
  case 0x101: return "DataAbort";
  case 0x102: return "MisalignedPC";
  case 0x103: return "MisalignedSP";
  case 0x104: return "Trap";
  case 0x106: return "SError";
  case 0x301: return "BadSVC";
  default: return "Unknown";
  }
}

static void ConvertPending(void) {
  static CrashRecord rec;
  FILE *in = fopen(s_pending, "rb");
  if (!in) return;
  size_t got = fread(&rec, 1, sizeof(rec), in);
  fclose(in);
  remove(s_pending);
  if (got != sizeof(rec) || rec.magic != CRASH_MAGIC || rec.version != CRASH_VERSION)
    return;
  FILE *f = fopen(s_report, "wb");
  if (!f) return;
  fprintf(f, "BABEL-RECOMP CRASH REPORT\n=========================\n\n");
  fprintf(f, "BUILD: %.63s\n", rec.build);
  fprintf(f, "LOAD BIAS REFERENCE: SwitchCrash_Init @ 0x%016llx\n"
             "  bias = that MINUS `nm` of SwitchCrash_Init in the matching .elf;\n"
             "  then: addr2line -e babel-recomp.elf -f -C <ADDR - bias>\n\n",
          (unsigned long long)rec.bias_ref);
  if (rec.have_regs) {
    fprintf(f, "EXCEPTION %s (0x%03x)  esr=0x%08x pstate=0x%08x far=0x%016llx\n",
            ErrorName(rec.error_desc), rec.error_desc, rec.esr, rec.pstate,
            (unsigned long long)rec.far);
    fprintf(f, "  PC 0x%016llx\n  LR 0x%016llx\n  SP 0x%016llx\n  FP 0x%016llx\n",
            (unsigned long long)rec.pc, (unsigned long long)rec.lr,
            (unsigned long long)rec.sp, (unsigned long long)rec.fp);
    for (int i = 0; i < 29; i++)
      fprintf(f, "  x%-2d 0x%016llx\n", i, (unsigned long long)rec.gprs[i]);
  }
  fprintf(f, "\nBREADCRUMBS (oldest first)\n");
  if (rec.ring_entries && rec.ring_entry_size >= 8) {
    const uint32_t n = rec.ring_entries;
    const uint32_t count = rec.ring_writes < n ? rec.ring_writes : n;
    const uint32_t start = rec.ring_writes < n ? 0 : rec.ring_writes % n;
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t *e = rec.ring + (size_t)((start + i) % n) * rec.ring_entry_size;
      uint32_t ms;
      memcpy(&ms, e, 4);
      char msg[256];
      size_t cap = rec.ring_entry_size - 4 < sizeof(msg) ? rec.ring_entry_size - 4 : sizeof(msg);
      memcpy(msg, e + 4, cap);
      msg[cap - 1] = 0;
      fprintf(f, "  [%8u ms] %s\n", ms, msg);
    }
  }
  fclose(f);
}

static void ReportUnclean(void) {
  FILE *flag = fopen(s_flag, "rb");
  if (!flag) return;
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, flag);
  buf[n] = 0;
  fclose(flag);
  FILE *f = fopen(s_unclean, "wb");
  if (!f) return;
  fprintf(f, "The previous session did not end cleanly and left no crash record\n"
             "(a hang, or the game was closed from the HOME menu).\n%s\n"
             "Its log is logs/previous-session.log.\n", buf);
  fclose(f);
}

static void CopyFile(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in) return;
  FILE *out = fopen(to, "wb");
  if (out) {
    char b[4096];
    size_t k;
    while ((k = fread(b, 1, sizeof(b), in)) > 0) fwrite(b, 1, k, out);
    fclose(out);
  }
  fclose(in);
}

/* `root`: the folder of the .nro, absolute (sdmc:/switch/babel-recomp). Call
 * before the session log is reopened: it keeps the previous one. */
void SwitchCrash_Init(const char *root, const char *build) {
  char logs[600], prev_log[600], log[600];
  snprintf(s_root, sizeof(s_root), "%s", root);
  snprintf(s_build, sizeof(s_build), "%s", build ? build : "dev");
  snprintf(logs, sizeof(logs), "%s/logs", s_root);
  mkdir(logs, 0755);
  snprintf(s_pending, sizeof(s_pending), "%s/crash_pending.bin", logs);
  snprintf(s_report, sizeof(s_report), "%s/crash-latest.log", logs);
  snprintf(s_flag, sizeof(s_flag), "%s/session-active.flag", logs);
  snprintf(s_unclean, sizeof(s_unclean), "%s/unclean-previous.log", logs);
  snprintf(log, sizeof(log), "%s/babel-recomp.log", s_root);
  snprintf(prev_log, sizeof(prev_log), "%s/previous-session.log", logs);
  CopyFile(log, prev_log);
  /* A crash record means the crash was reported: not also "unclean". */
  FILE *pending = fopen(s_pending, "rb");
  if (pending) {
    fclose(pending);
    ConvertPending();
    remove(s_flag);
  } else {
    ReportUnclean();
  }
  FILE *f = fopen(s_flag, "wb");
  if (f) {
    fprintf(f, "build=%s\n", s_build);
    fclose(f);
  }
}

void SwitchCrash_MarkCleanExit(void) {
  if (s_flag[0]) remove(s_flag);
}
#endif
