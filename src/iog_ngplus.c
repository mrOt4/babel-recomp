/*
 * New Game Plus (trusted plugin "iog.ngplus").
 *
 * Reaching the epilogue (scene $E5) marks the save file in use as finished
 * (bit 7 of $0B54, in RAM and in the battery save, checksum recomputed).
 * Continuing a finished file then starts the journey again from the school
 * with Will's stats, abilities and Red Jewel tally, but with the story,
 * dungeons and inventory reset; $0B54's low bits count the playthroughs.
 * With the feature off, finished files load as normal.
 *
 * Same state and reset as the GaiaLabs NewGamePlus/NGPEpilogueSwitch
 * patches (used as documentation); done here from the file-select load
 * instead of a new menu entry, so the game's menus stay stock.
 */

#include <stdbool.h>
#include "snes_ra.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "mod_runtime.h"
#include "iog_randomizer.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "snes_host_prompt.h"
#include "iog_i18n.h"

enum {
    kSceneEpilogue = 0xE5, kSceneFileSelect = 0xFA, kSceneEnd = 0xF0,
    kFlags = 0x0B54,                /* bit 7: finished; low bits: NG+ count */
    kSaveBase = 0x200, kSaveSize = 0x200, kSaveFrom = 0x0A00,
};

static uint8_t *Ram(void) { return g_snes->ram; }
static uint16_t Rd(uint32_t a) { return Ram()[a] | Ram()[a + 1] << 8; }
static void Wr(uint32_t a, uint16_t v) { Ram()[a] = (uint8_t)v; Ram()[a + 1] = (uint8_t)(v >> 8); }

/* The game's own save checksum ($03:D9B8): sum and xor of 254 words. */
static void FixChecksum(uint8_t *slot)
{
    uint16_t sum = 0x3652, x = 0x3652;
    for (int i = 0; i < 0x1FC; i += 2) {
        const uint16_t w = slot[i] | slot[i + 1] << 8;
        sum = (uint16_t)(sum + w);
        x ^= w;
    }
    slot[0x1FC] = (uint8_t)sum, slot[0x1FD] = (uint8_t)(sum >> 8);
    slot[0x1FE] = (uint8_t)x, slot[0x1FF] = (uint8_t)(x >> 8);
}

static bool g_applied;

static void MarkFinished(void)
{
    Ram()[kFlags] |= 0x80;
    Cart *cart = g_snes->cart;
    const unsigned slot = Rd(0x0D8C);
    if (!cart || !cart->ram || slot > 2 ||
        kSaveBase + (slot + 1) * kSaveSize > cart->ramSize)
        return;
    uint8_t *s = cart->ram + kSaveBase + slot * kSaveSize;
    s[kFlags - kSaveFrom] |= 0x80;
    FixChecksum(s);
    fprintf(stderr, "[ngplus] save file %u marked as finished\n", slot + 1);
}

static void StartNewGamePlus(void)
{
    SnesRa_SetBlocked("New Game+ changes the save");
    uint8_t *r = Ram();
    Wr(0x0AAC, Rd(0x0AB2));
    memset(r + 0x0A00, 0, 0x60);     /* story flags */
    memset(r + 0x0A80, 0, 0x22);     /* dungeon flags */
    memset(r + 0x0AB4, 0, 0x10);     /* inventory */
    Wr(0x0AD4, 0);
    Wr(0x0AC6, 0);
    Wr(0x0AC4, 0xFFFF);
    memset(r + 0x0B28, 0, 0x0C);     /* pyramid hieroglyphs */
    r[kFlags] = (uint8_t)(((r[kFlags] & 0x7F) + 1) & 0x7F);
    /* COP 26 (#08, #$0050, #$00A0, #00, #$1200): the classroom. */
    Wr(0x0642, 0x0008);
    Wr(0x064C, 0x0050);
    Wr(0x064E, 0x00A0);
    Wr(0x0650, 0x0000);
    Wr(0x0652, 0x1200);
    Wr(0x09AE, Rd(0x09AE) & ~0x2800);
    fprintf(stderr, "[ngplus] New Game Plus #%u\n", r[kFlags] & 0x7F);
}

/* Completion, 0-100: the 87 rooms whose enemies give a stat reward
 * (reward table $01:AADE, flags $0A60 + scene/8), the 50 Red Jewels (given
 * to the Gem Man, BCD $0AB0, plus held, stacked ones included) and the
 * story's end (bit 7 of $0B54): 40 / 40 / 20. -1 outside a game. */
int IogCompletionPercent(void)
{
    if (!g_snes || !Rd(0x09AA))
        return -1;
    const uint8_t *rom = g_snes->cart->rom;
    int rooms = 0, got = 0;
    for (int sc = 0; sc < 256; sc++) {
        if (!rom[0x01AADE + sc])
            continue;
        rooms++;
        got += (Ram()[0x0A60 + sc / 8] >> (sc & 7)) & 1;
    }
    const unsigned bcd = Ram()[0x0AB0];
    int jewels = (bcd >> 4) * 10 + (bcd & 15);
    for (int i = 0; i < 16; i++)
        jewels += Ram()[0x0AB4 + i] == 0x01;
    jewels += Ram()[0x0B56];
    if (jewels > 50) jewels = 50;
    const int finished = (Ram()[kFlags] & 0x80) || (Ram()[kFlags] & 0x7F);
    int pct = (rooms ? 40 * got / rooms : 40) + 40 * jewels / 50 + (finished ? 20 : 0);
    return pct > 100 ? 100 : pct;
}

static bool Es(void) { return !strcmp(IogI18nLanguage(), "es"); }

static void AfterEndAnswer(int yes)
{
    if (yes)
        MarkFinished();   /* the file shows as finished: New Game+ from it */
    snes_host_request_reset();
}

static void AfterNgpAnswer(int yes)
{
    if (yes)
        StartNewGamePlus();
}

/* Harder New Game+: damage taken is multiplied (x2 the first time round,
 * x3 after). Only real losses in play count, so healing, max-HP rewards
 * and the infinite-HP cheat behave as usual. */
static uint16_t g_last_hp;
static void Difficulty(void)
{
    const int round = Ram()[kFlags] & 0x7F;
    const uint16_t hp = Rd(0x0ACE);
    if (round && Rd(0x09AA) && hp < g_last_hp && hp > 0) {
        const int mult = round >= 2 ? 3 : 2;
        const int loss = (g_last_hp - hp) * (mult - 1);
        const int left = (int)hp - loss;
        Wr(0x0ACE, (uint16_t)(left > 0 ? left : 0));
    }
    g_last_hp = Rd(0x0ACE);
}

static int g_end_frames;
static bool g_asked_end, g_saw_epilogue;

static void Frame(void)
{
    if (!g_snes)
        return;
    const uint16_t scene = Rd(0x0644);
    Difficulty();
    if (scene == kSceneEpilogue)
        g_saw_epilogue = true;
    /* The END screen (the classroom after the credits): ask whether to keep
     * the file as finished, then go back to the title by itself. Once the
     * picture has been up a while: a button, or a minute. */
    if (scene == kSceneEnd && g_saw_epilogue) {
        g_end_frames++;
        const bool pressed = g_end_frames > 300 && (Rd(0x0660) & 0xF0F0);   /* raw: the scene masks input */
        if (!g_asked_end && (pressed || g_end_frames > 3600)) {
            g_asked_end = true;
            snes_host_request_prompt(
                Es() ? "¡JUEGO COMPLETADO!\n¿GUARDAR LA PARTIDA\nCOMO COMPLETADA?"
                     : "GAME COMPLETE!\nSAVE THE FILE\nAS FINISHED?",
                Es() ? "SÍ, GUARDAR" : "YES, SAVE", Es() ? "NO" : "NO", AfterEndAnswer);
        }
    } else {
        g_end_frames = 0;
        g_asked_end = false;
    }
    /* A file is being continued: the file-select scene has queued the
     * saved map ($0642) with that file's RAM block already loaded. A
     * finished file: ask for New Game+ (the answer comes before the next
     * frame, so the map change waits for it). */
    if (scene == kSceneFileSelect && Rd(0x0642) &&
        (Ram()[kFlags] & 0x80)) {
        if (!g_applied) {
            g_applied = true;
            snes_host_request_prompt(
                Es() ? "PARTIDA COMPLETADA.\n¿EMPEZAR NEW GAME+?\n(MÁS DIFICULTAD)"
                     : "FINISHED FILE.\nSTART NEW GAME+?\n(HARDER)",
                Es() ? "SÍ, NEW GAME+" : "YES, NEW GAME+",
                Es() ? "NO, CONTINUAR" : "NO, CONTINUE", AfterNgpAnswer);
        }
    } else if (scene != kSceneFileSelect) {
        g_applied = false;
    }
}

static void Activate(void)
{
    if (!IogRandomizerActive())
        snes_mod_register_frame_callback(Frame);
}

SNES_MOD_CONSTRUCTOR(register_iog_ngplus)
{
    snes_mod_register_activation_plugin("iog.ngplus", Activate);
}
