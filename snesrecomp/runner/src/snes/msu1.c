/* MSU-1 hardware core. See msu1.h for the model and the threading
 * contract. Self-contained: no SDL dependency — it borrows the APU mutex
 * (RtlApuLock/RtlApuUnlock) to serialise its state against the audio
 * thread, the same lock the S-DSP ports already use. */
#include "msu1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>   /* FindFirstFileA / FindNextFileA — MSVC has no <dirent.h> */
#else
#include <dirent.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

/* The audio thread already holds this around RtlRenderAudio; the CPU
 * thread takes it for register accesses. SDL mutexes are recursive, so
 * nesting is safe. Declared here to avoid coupling to common_rtl.h. */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);
/* The device rate the host actually opened (common_rtl.c RtlSetAudioOutputRate
 * / RtlAudioOutputRate). msu1_mix resamples the fixed 44.1 kHz track rate onto
 * this, the same conversion rtl_render_native does for the S-DSP output. */
extern double RtlAudioOutputRate(void);

#define MSU1_RATE            44100
#define MSU1_FRAMES_PER_BLK  (MSU1_RATE / 60)   /* 735 = 44100/60; historical unit, no
                                                    longer msu1_mix's per-call quantum
                                                    (see msu1_mix) */
#define MSU1_PCM_HEADER      8                  /* "MSU1" + uint32 loop point */
#define MSU1_REVISION        0x01
#define MSU1_PATH_MAX        1024

/* Status register ($2000 read) bit layout. */
#define MSU1_ST_DATA_BUSY    0x80
#define MSU1_ST_AUDIO_BUSY   0x40
#define MSU1_ST_AUDIO_REPEAT 0x20
#define MSU1_ST_AUDIO_PLAY   0x10
#define MSU1_ST_AUDIO_ERROR  0x08   /* track missing / load failed */

static const char kMsuId[6] = { 'S', '-', 'M', 'S', 'U', '1' };

static struct {
    bool   armed;        /* env said "on" */
    bool   auto_base;    /* derive base from ROM path */
    bool   have_base;
    char   base[MSU1_PATH_MAX];

    /* Audio channel */
    FILE  *track;        /* current <base>-<N>.pcm, positioned at the read cursor */
    uint32_t loop_point; /* loop restart, in sample-frames from data start */
    long   data_start;   /* byte offset of frame 0 (8 for MSU1 header, 0 if raw) */
    bool   playing;
    bool   repeat;
    bool   audio_error;  /* last track select missing/invalid */
    uint8_t volume;      /* 0..255 linear */
    uint16_t track_lo;   /* latched low byte of $2004, committed on $2005 */
    /* Track selects are opened by the reader thread (async_open below):
     * busy until then, and a play request made meanwhile waits for it. */
    volatile int pending; /* track to open, -1 none */
    volatile bool busy;
    volatile bool play_req;
    volatile bool reset_mix;  /* the mixer drops its resample tail */

    /* Data channel */
    FILE  *data;         /* <base>.msu, opened lazily */
    bool   data_tried;   /* attempted to open data file */
    uint8_t seek[4];     /* latched $2000-$2003, committed on $2003 */
} g;

/* Continuous-phase resample state for msu1_mix: 44.1 kHz track rate -> host
 * output rate, carried ACROSS calls the same way rtl_render_native
 * (common_rtl.c) carries its native->device phase. s_mix_buf is a growable
 * scratch buffer sized to each call's need; s_mix_carry holds the handful of
 * frames (always < ~2, since need is sized per-call) not yet fully consumed.
 * Reset in msu_load_track so a stale tail from the previous track can never
 * bleed into the next one. */
static int16_t *s_mix_buf;
static int s_mix_buf_cap;         /* frames capacity of s_mix_buf */
static double s_mix_phase;        /* fractional 44.1kHz position, carried over */
static int16_t s_mix_carry[8 * 2];
static int s_mix_carry_count;

/* ── lifecycle ───────────────────────────────────────────────────────── */

static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* True if `name` matches "<base>-<N>.pcm" (case-insensitive .pcm) with N a
 * run of digits. On success copies the "<base>" part into `out`. The base
 * itself may contain '-' (e.g. "...the_-_A_Link...") since we only strip the
 * FINAL "-<digits>.pcm". */
static int msu_match_track_name(const char *name, char *out, size_t out_sz) {
    size_t L = strlen(name);
    if (L < 7) return 0;                                   /* "x-1.pcm" min */
    if (!(  (name[L-4] == '.') &&
            (name[L-3]=='p'||name[L-3]=='P') &&
            (name[L-2]=='c'||name[L-2]=='C') &&
            (name[L-1]=='m'||name[L-1]=='M') )) return 0;
    size_t i = L - 4;                                      /* index of '.' */
    if (i == 0 || !isdigit((unsigned char)name[i-1])) return 0;
    while (i > 0 && isdigit((unsigned char)name[i-1])) i--; /* skip digits */
    if (i == 0 || name[i-1] != '-') return 0;             /* need the '-' */
    size_t base_len = i - 1;
    if (base_len == 0 || base_len >= out_sz) return 0;
    memcpy(out, name, base_len);
    out[base_len] = '\0';
    return 1;
}

/* Permissive base resolution: if g.base names a directory, scan it and adopt
 * the pack base ("<dir>/<name>") shared by the most "<name>-<N>.pcm" files —
 * so SNESRECOMP_MSU1 can point at a folder regardless of the pack's exact
 * name. No-op when g.base is already a file prefix. */
static void msu_resolve_base_if_dir(void) {
    struct stat st;
    if (stat(g.base, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    char dir[MSU1_PATH_MAX];
    strncpy(dir, g.base, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    size_t dlen = strlen(dir);
    while (dlen && (dir[dlen-1] == '/' || dir[dlen-1] == '\\')) dir[--dlen] = '\0';

    /* Tally candidate bases; keep the one backing the most tracks. The
     * directory enumeration is the only platform-specific part — MSVC has no
     * <dirent.h>, so Windows uses FindFirstFileA/FindNextFileA. The per-entry
     * tally below is shared. */
    struct { char base[MSU1_PATH_MAX]; int count; } cand[16];
    int ncand = 0;
    char nb[MSU1_PATH_MAX];

#ifdef _WIN32
    char pat[MSU1_PATH_MAX];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
#endif
        if (msu_match_track_name(name, nb, sizeof(nb))) {
            int found = 0;
            for (int k = 0; k < ncand; k++) {
                if (strcmp(cand[k].base, nb) == 0) { cand[k].count++; found = 1; break; }
            }
            if (!found && ncand < 16) {
                strcpy(cand[ncand].base, nb);
                cand[ncand].count = 1;
                ncand++;
            }
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif

    int best = -1;
    for (int k = 0; k < ncand; k++)
        if (best < 0 || cand[k].count > cand[best].count) best = k;
    if (best < 0) return;   /* no "<name>-<N>.pcm" here — leave base as-is */

    snprintf(g.base, sizeof(g.base), "%s/%s", dir, cand[best].base);
    printf("[MSU-1] pack base auto-detected: %s (%d tracks)\n",
           g.base, cand[best].count);
    fflush(stdout);
}

static bool s_unavailable;

void msu1_set_unavailable(void) { s_unavailable = true; }
bool msu1_available(void) { return !s_unavailable; }

void msu1_init(void) {
    memset(&g, 0, sizeof(g));
    g.pending = -1;
    g.volume = 255;  /* full unless the game says otherwise */
    const char *e = getenv("SNESRECOMP_MSU1");
    if (!e || !*e) return;                 /* default-OFF, silent, byte-identical */
    if (s_unavailable) {
        printf("[MSU-1] not available on this system\n");
        return;
    }
    g.armed = true;
    if (ci_eq(e, "1") || ci_eq(e, "on") || ci_eq(e, "auto") || ci_eq(e, "true")) {
        g.auto_base = true;                /* base supplied later via ROM path */
    } else {
        strncpy(g.base, e, sizeof(g.base) - 1);
        g.have_base = true;
        msu_resolve_base_if_dir();         /* allow pointing at a folder */
    }
    printf("[MSU-1] enabled: base='%s'\n",
           g.have_base ? g.base : "(awaiting ROM path)");
    fflush(stdout);
}

void msu1_set_rom_path(const char *rom_path) {
    if (!g.armed || !g.auto_base || g.have_base || !rom_path || !*rom_path)
        return;
    strncpy(g.base, rom_path, sizeof(g.base) - 1);
    /* Strip the extension, but only if the dot follows the last path
     * separator (don't chop "my.dir/rom"). */
    char *dot = strrchr(g.base, '.');
    char *sep = strrchr(g.base, '/');
    char *bsl = strrchr(g.base, '\\');
    if (bsl > sep) sep = bsl;
    if (dot && (!sep || dot > sep)) *dot = '\0';
    g.have_base = true;
}

bool msu1_enabled(void) {
    return g.armed && g.have_base;
}

/* ── audio track loading ─────────────────────────────────────────────── */

/* ── track streaming ─────────────────────────────────────────────────
 *
 * The audio callback never touches the file: a reader thread keeps a ring
 * of decoded frames ~2 s ahead, and the callback only drains it. Reading
 * straight from disk inside the callback stalled the audio whenever storage
 * was slow (a Switch SD card: audible dropouts while walking). The ring
 * counters are monotonic; the reader owns s_wr, the callback s_rd. s_io
 * guards the FILE and every reset. */
#define MSU_RING_FRAMES  (MSU1_RATE * 2)
#define MSU_CHUNK_FRAMES 4096
static uint32_t s_ring[MSU_RING_FRAMES];          /* one stereo frame each */
static volatile uint32_t s_rd, s_wr;
static volatile int s_eof;                         /* reader hit the end, no loop */
static pthread_mutex_t s_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_io_cv = PTHREAD_COND_INITIALIZER;
static int s_reader_started;

#if defined(__GNUC__) || defined(__clang__)
#define MSU_LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define MSU_STORE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)
#else
#define MSU_LOAD(p) (p)
#define MSU_STORE(p, v) ((p) = (v))
#endif

/* Read one chunk into the ring if there is room. Caller holds s_io.
 * Returns false when there is nothing to do. */
static bool msu_fill_locked(void) {
    static uint32_t chunk[MSU_CHUNK_FRAMES];
    uint32_t wr = s_wr, rd = MSU_LOAD(s_rd);
    if (!g.track || s_eof || wr - rd > MSU_RING_FRAMES - MSU_CHUNK_FRAMES)
        return false;
    {
        size_t n = fread(chunk, 4, MSU_CHUNK_FRAMES, g.track);
        if (n < MSU_CHUNK_FRAMES) {
            if (g.repeat) {
                long off = g.data_start + (long)g.loop_point * 4;
                if (fseek(g.track, off, SEEK_SET) != 0 && n == 0)
                    s_eof = 1;
            } else if (n == 0) {
                s_eof = 1;
            }
        }
        for (size_t i = 0; i < n; i++)
            s_ring[(wr + (uint32_t)i) % MSU_RING_FRAMES] = chunk[i];
        MSU_STORE(s_wr, wr + (uint32_t)n);
    }
    return true;
}

static void msu_open_locked(int track);

static void *msu_reader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&s_io);
    for (;;) {
        const int t = __atomic_exchange_n(&g.pending, -1, __ATOMIC_ACQ_REL);
        if (t >= 0) {
            msu_open_locked(t);
            continue;
        }
        if (msu_fill_locked())
            continue;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 4 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&s_io_cv, &s_io, &ts);
    }
    return NULL;
}

#ifdef __SWITCH__
/* Horizon does not time-slice threads of equal priority on one core: a
 * reader at the main thread's priority only runs when the game thread
 * waits, and a busy frame starves it. Run it one step above the game
 * thread (0x2C) so it preempts it for its short reads. */
#include <switch.h>
static Thread s_reader_thread;
static void msu_reader_nx(void *arg) { msu_reader(arg); }
#endif

static void msu_reader_start(void) {
    if (s_reader_started) return;
#ifdef __SWITCH__
    if (R_SUCCEEDED(threadCreate(&s_reader_thread, msu_reader_nx, NULL, NULL, 0x10000, 0x2B, -2)) &&
        R_SUCCEEDED(threadStart(&s_reader_thread))) {
        s_reader_started = 1;
        return;
    }
#endif
    pthread_t t;
    if (pthread_create(&t, NULL, msu_reader, NULL) == 0) {
        pthread_detach(t);
        s_reader_started = 1;
    }
}

void msu1_pump(void) {
    if (!msu1_enabled() || !g.track)
        return;
    /* Backstop for the reader thread: top the ring up from the game thread
     * when it falls under a second. Never blocks the game thread. */
    if (MSU_LOAD(s_wr) - MSU_LOAD(s_rd) >= MSU1_RATE)
        return;
    if (pthread_mutex_trylock(&s_io) != 0)
        return;
    for (int i = 0; i < 4 && msu_fill_locked(); i++) {}
    pthread_mutex_unlock(&s_io);
}

/* Caller holds s_io. */
static void msu_reset_stream_locked(void) {
    MSU_STORE(s_rd, 0);
    MSU_STORE(s_wr, 0);
    s_eof = 0;
}

static void msu_close_track(void) {
    pthread_mutex_lock(&s_io);
    if (g.track) { fclose(g.track); g.track = NULL; }
    msu_reset_stream_locked();
    pthread_mutex_unlock(&s_io);
}

/* Open <base>-<track>.pcm and leave the file positioned at the first
 * sample. Standard MSU files carry an 8-byte "MSU1" + loop-point header;
 * permissively, a file lacking the magic is treated as raw 44.1 kHz stereo
 * PCM from byte 0 (loop to start). Selecting a track stops playback (spec). */
/* Reader thread, s_io held: the SD card's open and header read happen
 * here, never on the game thread (a slow card stalled whole frames at every
 * music change), and a new select arriving meanwhile simply wins. */
static void msu_open_locked(int track) {
    if (g.track) { fclose(g.track); g.track = NULL; }
    msu_reset_stream_locked();
    char fn[MSU1_PATH_MAX + 32];
    snprintf(fn, sizeof(fn), "%s-%u.pcm", g.base, (unsigned)track);
    FILE *f = fopen(fn, "rb");
    if (f) {
        uint8_t hdr[MSU1_PCM_HEADER];
        if (fread(hdr, 1, MSU1_PCM_HEADER, f) == MSU1_PCM_HEADER &&
            hdr[0] == 'M' && hdr[1] == 'S' && hdr[2] == 'U' && hdr[3] == '1') {
            g.loop_point = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                           ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
            g.data_start = MSU1_PCM_HEADER;
        } else {
            g.loop_point = 0;
            g.data_start = 0;
            fseek(f, 0, SEEK_SET);
        }
    }
    if (__atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) >= 0) {   /* superseded */
        if (f) fclose(f);
        return;
    }
    g.track = f;
    g.audio_error = (f == NULL);
    g.busy = false;
    if (g.play_req) g.playing = (f != NULL);
}

/* Selecting a track stops playback (spec); the reader opens it. */
static void msu_load_track(uint16_t track) {
    g.playing = false;
    g.audio_error = false;
    g.busy = true;
    /* A new track starts its own 44.1 kHz stream; the mixer drops the
     * previous one's resample tail and phase (its own state). */
    g.reset_mix = true;
    msu_reader_start();
    __atomic_store_n(&g.pending, (int)track, __ATOMIC_RELEASE);
    pthread_cond_signal(&s_io_cv);
}

int msu1_track_present(uint16_t track) {
    if (!msu1_enabled()) return 0;
    char fn[MSU1_PATH_MAX + 32];
    snprintf(fn, sizeof(fn), "%s-%u.pcm", g.base, (unsigned)track);
    FILE *f = fopen(fn, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ── register interface (CPU thread) ─────────────────────────────────── */

uint8_t msu1_read(uint16_t reg) {
    if (!msu1_enabled()) return 0;
    RtlApuLock();
    uint8_t out = 0;
    switch (reg & 0x7) {
        case 0x0: /* $2000 status */
            out = (uint8_t)((g.busy        ? MSU1_ST_AUDIO_BUSY   : 0) |
                            (g.repeat      ? MSU1_ST_AUDIO_REPEAT : 0) |
                            (g.playing     ? MSU1_ST_AUDIO_PLAY   : 0) |
                            (g.audio_error ? MSU1_ST_AUDIO_ERROR  : 0) |
                            (MSU1_REVISION & 0x07));
            break;
        case 0x1: /* $2001 data read port (auto-increment via file pos) */
            if (!g.data && !g.data_tried) {
                g.data_tried = true;
                char dn[MSU1_PATH_MAX + 8];
                snprintf(dn, sizeof(dn), "%s.msu", g.base);
                g.data = fopen(dn, "rb");
            }
            if (g.data) {
                int c = fgetc(g.data);
                out = (c == EOF) ? 0 : (uint8_t)c;
            }
            break;
        default:  /* $2002-$2007 identification "S-MSU1" */
            out = (uint8_t)kMsuId[(reg & 0x7) - 0x2];
            break;
    }
    RtlApuUnlock();
    return out;
}

void msu1_write(uint16_t reg, uint8_t val) {
    if (!msu1_enabled()) return;
    /* The audio registers are single stores the mixer reads as it goes;
     * only the data channel (a FILE) needs the APU lock. Taking it for every
     * write stalled the game thread behind a whole audio block at each fade
     * step and track change. */
    const bool data = (reg & 0x7) <= 0x3;
    if (data) RtlApuLock();
    switch (reg & 0x7) {
        case 0x0: case 0x1: case 0x2: /* $2000-$2002 data seek bytes (latch) */
            g.seek[reg & 0x7] = val;
            break;
        case 0x3: { /* $2003 commits the 32-bit data seek */
            g.seek[3] = val;
            uint32_t off = (uint32_t)g.seek[0] | ((uint32_t)g.seek[1] << 8) |
                           ((uint32_t)g.seek[2] << 16) | ((uint32_t)g.seek[3] << 24);
            if (!g.data && !g.data_tried) {
                g.data_tried = true;
                char dn[MSU1_PATH_MAX + 8];
                snprintf(dn, sizeof(dn), "%s.msu", g.base);
                g.data = fopen(dn, "rb");
            }
            if (g.data) fseek(g.data, (long)off, SEEK_SET);
            break;
        }
        case 0x4: /* $2004 track number low byte (latch) */
            g.track_lo = val;
            break;
        case 0x5: /* $2005 commits the 16-bit track select + loads it */
            msu_load_track((uint16_t)(g.track_lo | ((uint16_t)val << 8)));
            break;
        case 0x6: /* $2006 volume */
            g.volume = val;
            break;
        case 0x7: /* $2007 control: bit0 play, bit1 repeat */
            g.repeat = (val & 0x2) != 0;
            if (val & 0x1) {
                /* Resume from the current cursor; only a valid track plays
                 * (one still opening starts when the reader has it). */
                g.play_req = true;
                g.playing = (!g.busy && g.track != NULL && !g.audio_error);
            } else {
                g.play_req = false;
                g.playing = false;  /* pause, cursor retained */
            }
            break;
    }
    if (data) RtlApuUnlock();
}

/* ── audio mix (audio thread, APU lock already held) ─────────────────── */

/* Fill `frames` stereo frames from the track, looping or stopping at EOF.
 * Returns the count actually backed by audio; the remainder is zeroed.
 *
 * KNOWN REMAINING ISSUE: this fread()s straight off disk from inside the
 * audio callback (msu1_mix -> here, under the APU lock). A slow/contended
 * filesystem can stall the audio thread. Not addressed here -- fixing it
 * means moving track I/O off-thread (background prefetch into a ring the
 * callback only drains), which is a larger change than this pass's scope
 * (msu1_mix's per-call resample math). Recorded for a future pass. */
static int msu_read_frames(int16_t *dst, int frames) {
    uint32_t rd = s_rd, wr = MSU_LOAD(s_wr);
    int avail = (int)(wr - rd);
    int n = avail < frames ? avail : frames;
    for (int i = 0; i < n; i++) {
        uint32_t v = s_ring[(rd + (uint32_t)i) % MSU_RING_FRAMES];
        memcpy(dst + (size_t)i * 2, &v, 4);
    }
    MSU_STORE(s_rd, rd + (uint32_t)n);
    pthread_cond_signal(&s_io_cv);
    if (n < frames && !s_eof) {
        static int stats = -1;
        if (stats < 0) stats = getenv("SNESRECOMP_MSU_STATS") != NULL;
        if (stats)
            fprintf(stderr, "[MSU-1] underrun: wanted %d, had %d\n", frames, n);
    }
    if (n < frames) {
        /* Underrun: silence rather than wait. At the end of a track that does
         * not loop, the reader has stopped: stop playing (spec). */
        if (s_eof && MSU_LOAD(s_wr) == s_rd)
            g.playing = false;
        memset(dst + (size_t)n * 2, 0, (size_t)(frames - n) * 4);
    }
    return n;
}

static inline int16_t clamp_s16(int v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

void msu1_mix(int16_t *out, int out_frames) {
    if (g.reset_mix) {
        g.reset_mix = false;
        s_mix_phase = 0.0;
        s_mix_carry_count = 0;
    }
    if (!msu1_enabled() || !g.playing || !g.track || out_frames <= 0)
        return;

    /* Continuous-phase resample from the fixed 44.1 kHz track rate onto
     * whatever the host asks for THIS call (out_frames, at s_render_output_rate
     * -- see RtlAudioOutputRate), with the fractional phase carried in a
     * static across calls. Mirrors rtl_render_native's native->device
     * conversion in common_rtl.c exactly (span/need/retire math below is the
     * same shape).
     *
     * Previously this consumed exactly one fixed MSU1_FRAMES_PER_BLK (735 =
     * 44100/60) block per call and stretched it onto out_frames with the
     * phase reset every call -- correct only when the host called once per
     * emulated 60 Hz frame. Hosts that instead pull fixed-size device chunks
     * (Super Mario Kart / Super Mario RPG: ~320 frames, ~100 calls/s) got
     * exactly one 735-frame block's worth of track audio per call regardless
     * of how much output time out_frames actually spanned -- roughly 1.7x
     * too fast, and warped at every chunk boundary. */
    double output_rate = RtlAudioOutputRate();
    if (output_rate < 1.0) output_rate = MSU1_RATE;
    const double ratio = (double)MSU1_RATE / output_rate;

    double span = s_mix_phase + (double)(out_frames - 1) * ratio;
    int need = (int)span + 2;
    if (need < 2) need = 2;

    if (need > s_mix_buf_cap) {
        int16_t *nb = (int16_t *)realloc(s_mix_buf,
                                          (size_t)need * 2 * sizeof(int16_t));
        if (!nb) return;   /* OOM: skip this call, carried state unchanged */
        s_mix_buf = nb;
        s_mix_buf_cap = need;
    }

    int have = s_mix_carry_count < need ? s_mix_carry_count : need;
    if (have > 0)
        memcpy(s_mix_buf, s_mix_carry, (size_t)have * 2 * sizeof(int16_t));
    if (need > have)
        /* msu_read_frames always fills its full request (zero-padding past
         * EOF when not looping), so s_mix_buf[0..need) is fully populated
         * after this -- it also advances the file cursor / handles looping,
         * which is why it still runs even when volume is 0 below. */
        msu_read_frames(s_mix_buf + (size_t)have * 2, need - have);

    if (g.volume != 0) {
        const int vol = g.volume;  /* 0..255, linear */
        for (int i = 0; i < out_frames; i++) {
            double pos = s_mix_phase + (double)i * ratio;
            int idx = (int)pos;
            double frac = pos - idx;
            int idx1 = idx + 1;
            if (idx  > need - 1) idx  = need - 1;
            if (idx1 > need - 1) idx1 = need - 1;

            for (int ch = 0; ch < 2; ch++) {
                int s0 = s_mix_buf[idx * 2 + ch];
                int s1 = s_mix_buf[idx1 * 2 + ch];
                int s  = (int)(s0 + (s1 - s0) * frac);  /* linear interpolation */
                s = (s * vol) / 255;
                int o = out[i * 2 + ch] + s;
                out[i * 2 + ch] = clamp_s16(o);
            }
        }
    }
    /* else: still advance the cursor above; just don't mix (matches prior
     * behaviour -- the track position moves whether or not it's audible). */

    double consumed_span = s_mix_phase + (double)out_frames * ratio;
    int retire = (int)consumed_span;
    if (retire > need) retire = need;
    int remain = need - retire;
    int carry_cap = (int)(sizeof(s_mix_carry) / (2 * sizeof(int16_t)));
    if (remain > carry_cap) remain = carry_cap;  /* defensive; math above keeps this tiny */
    if (remain > 0)
        memmove(s_mix_carry, s_mix_buf + (size_t)retire * 2,
                (size_t)remain * 2 * sizeof(int16_t));
    s_mix_carry_count = remain;
    s_mix_phase = consumed_span - (double)retire;
}
