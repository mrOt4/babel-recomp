/*
 * MSU-1 soundtrack for Illusion of Gaia (trusted plugin "iog.msu1").
 *
 * The player chooses an MSU-1 pack folder on the Mods page; the framework
 * hands it to the MSU-1 core (SNESRECOMP_MSU1). Nothing here changes game
 * code:
 *
 *  - Each song with a track in the pack gets its own copy of the game's
 *    silent song (id $1B, taken from the player's ROM) in overlay bank $AF,
 *    and every pointer to that song (the music table at $01:CBA6 and the
 *    scene_meta music entries [id][group][ptr24]) is pointed at the copy.
 *    A song's track is the one of the music id whose table entry it is
 *    (the title theme, loaded as id $14 with its own song, is track 1).
 *    The S-SMP keeps running the game's driver, so sound effects play as
 *    before, while the game's own "is this a different song?" logic still
 *    sees a different pointer per song.
 *  - The music loader ($02:8B91) sends $FF, uploads the song through the IPL,
 *    waits three frames and sends $01. That start is recognised on the APU
 *    port and the song pointer the loader was given ($3E-$40) says which
 *    song started:
 *    a copy starts its MSU-1 track, a real song stops MSU-1 playback.
 *  - $F1 (music fade, COP 05) and $F2 (fade before a song change) fade the
 *    MSU-1 volume out; $F0 stops it.
 *
 * Track numbering and looping follow the existing Illusion of Gaia MSU-1
 * packs, as documented by GaiaLabs (MusicTransitions.asm: bgm_track_map,
 * bgm_loop_map). A missing track leaves that song on the S-SMP.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "mod_runtime.h"
#include "iog_randomizer.h"
#include "snes/cart.h"
#include "snes/msu1.h"
#include "snes/snes.h"

enum {
    kMusicTable = 0x01CBA6,     /* 24-bit pointers, id 1..kMusicIds */
    kMusicIds = 0x1E,
    kSilentId = 0x1B,
    kSceneMetaStart = 0x0D8000, kSceneMetaEnd = 0x0DB000,
    kOverlayIndex = 15,         /* HiROM $AF:8000 */
    kOverlayBank = 0xAF,
};

static const uint8_t kTrack[0x20] = {
    0x00, 0x06, 0x06, 0x07, 0x05, 0x09, 0x0A, 0x0C, 0x0F, 0x10, 0x0B, 0x08, 0x11, 0x06, 0x13, 0x12,
    0x14, 0x0D, 0x02, 0x15, 0x16, 0x0E, 0x04, 0x00, 0x18, 0x17, 0x19, 0x00, 0x03, 0x00, 0x1A, 0x01,
};
/* The title theme is loaded as id $14 with its own song (0x111233), and is
 * track 1 in the packs. Tracks 23-26 (Lola's song, item fanfare, the two
 * melodies) and 28+ are one-shots; the rest loop. */
static const uint32_t kTitleSong = 0xD11233;
static bool Loops(int track) { return !(track >= 0x17 && track <= 0x1A) && track < 0x1C; }

/* The one-shots (Lola's song, the item fanfare, the two melodies) stay on
 * the S-SMP: the scripts that play them wait for the song's own end signal
 * ($FF on APU port 1: Lola teaching the melody, the Wind Melody, Sam, Gaia,
 * the stone coffin, the melody item) and a silent copy never sends it, so
 * the scene waited forever. */
static bool OneShotWaited(int track) { return track >= 0x17 && track <= 0x1A; }

static uint8_t *g_bank;            /* 32 KiB overlay */
/* One silent copy per (music id, original song) pair, so the game's
 * "different pointer = different song" test keeps its answers. */
enum { kMaxCopies = 64 };
static struct { uint32_t orig, addr; uint8_t track; } g_copies[kMaxCopies];
static int g_ncopies;
/* Every real song pointer the game can load (music table and scene_meta):
 * the loader's upload path is also used for other sound data (boss sound
 * effects, the ending), whose "start" must not stop the MSU-1 track. */
enum { kMaxKnown = 128 };
static uint32_t g_known[kMaxKnown];
static int g_nknown;
static void AddKnown(uint32_t ptr)
{
    for (int i = 0; i < g_nknown; i++)
        if (g_known[i] == ptr) return;
    if (g_nknown < kMaxKnown) g_known[g_nknown++] = ptr;
}
static bool IsKnownSong(uint32_t ptr)
{
    for (int i = 0; i < g_nknown; i++)
        if (g_known[i] == ptr) return true;
    return false;
}
static bool g_log;

/* Loader state, from the APU port 0 traffic: 1 after $FF, 2 once the IPL
 * transfer starts ($CC), 3 when port 0 has been quiet for two frames (the
 * transfer's own counters also pass through $01). */
static int g_loading;
static int g_quiet;                /* frames since the last port 0 write */
static int g_track;                /* MSU-1 track playing, 0: none */
static int g_volume, g_fading;

static uint32_t Off24(uint32_t a) { return ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF); }

static void Msu(uint16_t reg, uint8_t v) { msu1_write(reg, v); }

static bool TrackExists(int track)
{
    return msu1_track_present((uint16_t)track) != 0;
}

static void Play(int id, int track)
{
    if (track == g_track && !g_fading && g_volume == 0xFF)
        return;
    Msu(0x2004, (uint8_t)track);
    Msu(0x2005, 0);
    Msu(0x2006, 0xFF);
    Msu(0x2007, Loops(track) ? 0x03 : 0x01);
    g_track = track, g_volume = 0xFF, g_fading = 0;
    if (g_log)
        fprintf(stderr, "[msu1] song %02X -> track %d%s\n", id, track, Loops(track) ? " (loop)" : "");
}

static void Stop(void)
{
    if (!g_track)
        return;
    Msu(0x2007, 0);
    if (g_log)
        fprintf(stderr, "[msu1] stop track %d\n", g_track);
    g_track = 0, g_fading = 0;
}

static int ApuWrite(uint16_t reg, uint8_t value)
{
    if (reg != 0x2140)
        return 0;
    g_quiet = 0;
    if (g_loading == 2)
        return 0;   /* IPL transfer: its counters pass through every value */
    if (g_loading == 1) {
        if (value == 0xCC)
            g_loading = 2;
        return 0;
    }
    if (g_loading == 3) {
        if (value == 0xCC) {   /* only a pause between blocks */
            g_loading = 2;
            return 0;
        }
        g_loading = 0;
        if (value == 0x01) {   /* the loader's start, after the upload settled */
            /* $3E-$40: the song the loader was given ($46 has moved on). */
            const uint8_t *ram = g_snes->ram;
            const uint32_t ptr = ram[0x3E] | ram[0x3F] << 8 | ram[0x40] << 16;
            /* A copy means MSU-1; the loader's own $06F2 says which id. */
            int id = 0, track = 0;
            for (int i = 0; i < g_ncopies; i++)
                if (g_copies[i].addr == ptr)
                    id = g_snes->ram[0x06F2], track = g_copies[i].track;   /* id: for the log */
            if (g_log)
                fprintf(stderr, "[msu1] start: song pointer %06X (id %02X)\n", ptr, id);
            if (track)
                Play(id, track);
            else if (IsKnownSong(ptr))
                Stop();   /* a song MSU-1 has no track for: the S-SMP plays it */
            /* else: other sound data, the song keeps playing */
            return 0;
        }
    }
    switch (value) {
    case 0xF1:
    case 0xF2:
        if (g_track)
            g_fading = 1;
        break;
    case 0xF0:
        Stop();
        break;
    case 0xFF:
        g_loading = 1;
        break;
    }
    return 0;   /* the S-SMP still gets every command */
}

/* A savestate (or rewind, or reset) brings the S-SMP back with the song it
 * had; MSU-1 playback is not in the state. Follow the song the game says
 * is loaded ($0687-$0689, written by the loader at $03:E21E). */
static void Resync(void)
{
    const uint8_t *ram = g_snes->ram;
    const uint32_t ptr = ram[0x0687] | ram[0x0688] << 8 | ram[0x0689] << 16;
    int track = 0;
    for (int i = 0; i < g_ncopies; i++)
        if (g_copies[i].addr == ptr)
            track = g_copies[i].track;
    g_loading = 0;
    g_fading = 0;
    if (g_log)
        fprintf(stderr, "[msu1] state loaded: song %06X -> %s %d\n", ptr, track ? "track" : "none", track);
    if (track) {
        g_track = 0;   /* force a restart from the top */
        Play(ram[0x06F2], track);
    } else {
        Stop();
    }
}

static void Frame(void)
{
    static uint64_t gen;
    static bool init;
    if (!init) {
        init = true;
        gen = RtlStateGeneration();
    } else if (gen != RtlStateGeneration()) {
        gen = RtlStateGeneration();
        if (g_snes)
            Resync();
    }
    msu1_pump();
    if (g_loading == 2 && ++g_quiet >= 2)
        g_loading = 3;   /* upload done; the loader now waits before $01 */
    if (g_fading && g_track) {
        g_volume -= 8;
        if (g_volume <= 0) {
            Stop();
        } else {
            Msu(0x2006, (uint8_t)g_volume);
        }
    }
}

/* The silent song's start clears the S-SMP's RAM from the address it
 * stores at $0FFE to the end: ~40 KiB, half a second during which the driver
 * does not read the ports. The game rarely changes music that soon after a
 * silence; with every song a silent copy it does so all the time (entering
 * Will's house, then Lola's melody), and the $F0 it sends meanwhile is lost:
 * the driver takes it for the stop it already handled, never acknowledges
 * it, and the game waits for that answer forever. A copy plays nothing, so
 * it clears only $FF00-$FFFF; the next real song does its own clearing. */
static void QuickClear(uint8_t *song, uint32_t cap)
{
    uint32_t o = 0;
    while (o + 4 <= cap) {
        const uint32_t len = song[o] | song[o + 1] << 8, dest = song[o + 2] | song[o + 3] << 8;
        if (!len)
            return;
        if (dest == 0x0FFE && len == 2 && o + 6 <= cap) {
            song[o + 4] = 0x00;
            song[o + 5] = 0xFF;
            return;
        }
        o += 4 + len;
    }
}

/* A song blob: seven [len16][dest16][data] blocks, then a zero length. */
static bool IsSong(const uint8_t *rom, uint32_t size, uint32_t off)
{
    int blocks = 0;
    while (off + 4 <= size) {
        const uint32_t len = rom[off] | rom[off + 1] << 8;
        if (!len)
            return blocks == 7;
        if (len > 0x4000 || ++blocks > 7)
            return false;
        off += 4 + len;
    }
    return false;
}

static void Activate(void)
{
    g_log = !getenv("IOG_MSU_QUIET");   /* one line per music change: kept in the log */
    if (IogRandomizerActive()) {
        fprintf(stderr, "[msu1] off: a randomizer seed is loaded\n");
        return;
    }
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || !msu1_enabled()) {
        fprintf(stderr, "[msu1] no MSU-1 pack selected\n");
        return;
    }
    uint8_t *rom = cart->rom;
    const uint32_t size = cart->romSize;
    if (size < 0x200000)
        return;
    /* The silent song, from this ROM. Its layout is the loader's business;
     * the copy takes a generous span (the song is ~150 bytes). */
    const uint8_t *e = rom + kMusicTable + 3 * (kSilentId - 1);
    const uint32_t silent = Off24(e[0] | e[1] << 8 | e[2] << 16);
    const uint32_t silent_len = 0x200;
    if (silent + silent_len > size)
        return;

    /* A song's track: the title theme is track 1; any other song takes the
     * track of the music id whose table entry it is. The song, not the id
     * that loads it, decides: the school loads the town song as id $1D. */
    uint32_t table[0x20] = {0};
    for (int id = 1; id <= kMusicIds; id++) {
        const uint8_t *t = rom + kMusicTable + 3 * (id - 1);
        table[id] = t[0] | t[1] << 8 | t[2] << 16;
        AddKnown(table[id]);
    }
    AddKnown(kTitleSong);
    bool have[0x20] = {false};
    for (int n = 1; n < 0x20; n++)
        have[n] = TrackExists(n);
#define TRACK_FOR(PTR, OUT) do { \
        OUT = (PTR) == kTitleSong ? 1 : 0; \
        for (int i_ = 1; !OUT && i_ <= kMusicIds; i_++) \
            if (table[i_] == (PTR) && i_ != kSilentId && kTrack[i_]) OUT = kTrack[i_]; \
        if (OUT && !have[OUT]) OUT = 0; \
        if (OUT && OneShotWaited(OUT)) OUT = 0; } while (0)

    g_bank = calloc(1, 0x8000);
    uint32_t fill = 0;
    int patched = 0;
    /* One silent copy per song, so the game's "different pointer = different
     * song" test keeps its answers. */
#define COPY_FOR(ORIG, TRACK, OUT) do { \
        OUT = 0; \
        for (int k_ = 0; k_ < g_ncopies; k_++) \
            if (g_copies[k_].orig == (ORIG)) OUT = g_copies[k_].addr; \
        if (!OUT && g_ncopies < kMaxCopies && fill + silent_len <= 0x8000) { \
            memcpy(g_bank + fill, rom + silent, silent_len); \
            QuickClear(g_bank + fill, silent_len); \
            OUT = (uint32_t)kOverlayBank << 16 | (0x8000 + fill); \
            g_copies[g_ncopies].orig = (ORIG), g_copies[g_ncopies].track = (uint8_t)(TRACK); \
            g_copies[g_ncopies++].addr = OUT; \
            fill += silent_len; \
        } } while (0)
    for (int id = 1; id <= kMusicIds; id++) {
        int track;
        TRACK_FOR(table[id], track);
        if (!track)
            continue;
        uint32_t copy;
        COPY_FOR(table[id], track, copy);
        if (!copy)
            continue;
        uint8_t *t = rom + kMusicTable + 3 * (id - 1);
        t[0] = (uint8_t)copy, t[1] = (uint8_t)(copy >> 8), t[2] = (uint8_t)(copy >> 16);
        patched++;
    }
    /* scene_meta music entries: [id][group][ptr24]. */
    for (uint32_t o = kSceneMetaStart + 2; o + 3 <= kSceneMetaEnd; o++) {
        const int id = rom[o - 2];
        if (id < 1 || id > 0x1F || rom[o - 1] > 2 || rom[o + 2] < 0x80 || rom[o + 2] >= 0xE0)
            continue;
        const uint32_t ptr = rom[o] | rom[o + 1] << 8 | rom[o + 2] << 16;
        if (IsSong(rom, size, Off24(ptr)))
            AddKnown(ptr);
        int track;
        TRACK_FOR(ptr, track);
        if (!track || !IsSong(rom, size, Off24(ptr)))
            continue;
        uint32_t copy;
        COPY_FOR(ptr, track, copy);
        if (!copy)
            continue;
        rom[o] = (uint8_t)copy, rom[o + 1] = (uint8_t)(copy >> 8), rom[o + 2] = (uint8_t)(copy >> 16);
        patched++;
    }
#undef COPY_FOR
#undef TRACK_FOR
    if (!g_ncopies) {
        fprintf(stderr, "[msu1] the pack has none of the game's tracks\n");
        free(g_bank);
        g_bank = NULL;
        return;
    }
    cart_set_overlay_bank(cart, kOverlayIndex, g_bank);
    snes_mod_register_apu_write_callback(ApuWrite);
    snes_mod_register_frame_callback(Frame);
    fprintf(stderr, "[msu1] %d songs on MSU-1 (%d references)\n", g_ncopies, patched);
}

SNES_MOD_CONSTRUCTOR(register_iog_msu1)
{
    snes_mod_register_activation_plugin("iog.msu1", Activate);
}
