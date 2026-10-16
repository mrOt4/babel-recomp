/*
 * Illusion of Gaia gameplay mods: trusted plugins selected by the packages in
 * mods/preloaded/packages (iog.enhancements, iog.cheats).
 *
 * Each one runs at the end of a guest frame (snes_mod_register_frame_callback)
 * and works on WRAM only, so the stock ROM and the recompiled code are
 * untouched and a disabled feature leaves the game exactly as shipped.
 *
 * WRAM layout used here (GaiaLabs' RAM map and patches, and the game's own
 * save routine at $0B:EDC2 for the stats):
 *   (buttons: the physical pad, g_snes->input1_currentState; during play the
 *   game rewrites its own copies at $0656/$0660 through the button type,
 *   so those do not name physical buttons)
 *   $09AA  player actor (0: none)       $09AE  player control flags
 *   $09B2  dash speed east/west         $09B4  dash speed north/south
 *   $09B8  dash brake counter
 *   $0ACE  HP   $0ACA  max HP   $0AD6  DP   $0ADC  DEF   $0ADE  STR
 *
 * The behaviour follows the GaiaLabs enhancement patches (RunButton.asm) as
 * documentation; the code is this project's own.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "mod_runtime.h"
#include "snes/snes.h"
#include "present_filter.h"
#include "snes_ra.h"

static uint16_t Rd(uint32_t a) { return g_snes->ram[a] | g_snes->ram[a + 1] << 8; }
static void Wr(uint32_t a, uint16_t v) { g_snes->ram[a] = (uint8_t)v; g_snes->ram[a + 1] = (uint8_t)(v >> 8); }

static long OptionInt(const char *pkg, const char *feature, const char *option, long fallback)
{
    char v[32];
    if (snes_mod_runtime_feature_option_value_c(pkg, feature, option, v, sizeof(v)) > 0 && v[0])
        return strtol(v, NULL, 0);
    return fallback;
}

/* ---- run button ---------------------------------------------------------- */

/* Physical pad, in the controller's serial order (bit 0 = B). */
enum {
    kPadB = 0x001, kPadY = 0x002, kPadSelect = 0x004, kPadStart = 0x008,
    kPadUp = 0x010, kPadDown = 0x020, kPadLeft = 0x040, kPadRight = 0x080,
    kPadA = 0x100, kPadX = 0x200, kPadL = 0x400, kPadR = 0x800,
};
static uint16_t Pad(void) { return g_snes->input1_currentState & 0x0FFF; }

static uint16_t g_run_mask = kPadY;

/* Holding the run button with a direction starts the game's own dash (the
 * one Will uses after a charged step), in that direction, whenever the game
 * would let the player move. The dash ends by itself like any other. */
static void RunFrame(void)
{
    if (!g_snes)
        return;
    const uint16_t pad = Pad();
    if (!(pad & g_run_mask))
        return;
    if (!Rd(0x09AA) || Rd(0x09B2) || Rd(0x09B4) || (Rd(0x09AE) & 0x3800))
        return;   /* no player, dashing already, or movement locked/ladder */
    const uint16_t dir = pad;
    if (dir & kPadRight)
        Wr(0x09B2, 3);
    else if (dir & kPadLeft)
        Wr(0x09B2, 0xFFFD);
    else if (dir & kPadDown)
        Wr(0x09B4, 3);
    else if (dir & kPadUp)
        Wr(0x09B4, 0xFFFD);
    else
        return;
    Wr(0x09B8, 0);
}

static void ActivateRun(void)
{
    char v[16] = "Y";
    snes_mod_runtime_feature_option_value_c("iog.enhancements", "run", "button", v, sizeof(v));
    g_run_mask = !strcmp(v, "X") ? kPadX : !strcmp(v, "L") ? kPadL : !strcmp(v, "R") ? kPadR : kPadY;
    snes_mod_register_frame_callback(RunFrame);
}

/* ---- item hotswapping ---------------------------------------------------- */

/* Holding the modifier, L/R step the equipped item through the inventory
 * ($0AB4, 16 slots) in either direction; past either end nothing is
 * equipped. The game's item-select sound confirms it (COP 07 #$10 just
 * stores the effect in $06F8 for the next NMI). */
static uint16_t g_swap_mod = kPadX, g_swap_prev;

static void SwapFrame(void)
{
    if (!g_snes)
        return;
    const uint16_t pad = Pad(), pressed = pad & ~g_swap_prev;
    g_swap_prev = pad;
    if (!(pad & g_swap_mod) || !(pressed & (kPadL | kPadR)))
        return;
    if (!Rd(0x09AA) || (Rd(0x09EC) & 0x4000))
        return;   /* no player, or a menu/message is up */
    const uint8_t *slots = &g_snes->ram[0x0AB4];
    int slot = (int16_t)Rd(0x0AC4);
    if (pressed & kPadR) {
        do slot++; while (slot < 16 && !slots[slot]);
    } else {
        if (slot < 0) slot = 16;
        do slot--; while (slot >= 0 && !slots[slot]);
    }
    if (slot < 0 || slot >= 16) {
        Wr(0x0AC4, 0xFFFF);
        Wr(0x0AC6, 0);
    } else {
        Wr(0x0AC4, (uint16_t)slot);
        Wr(0x0AC6, slots[slot]);
    }
    g_snes->ram[0x06F8] = 0x10;
}

static void ActivateSwap(void)
{
    char v[16] = "X";
    snes_mod_runtime_feature_option_value_c("iog.enhancements", "hotswap", "modifier", v, sizeof(v));
    g_swap_mod = !strcmp(v, "Y") ? kPadY : !strcmp(v, "Select") ? kPadSelect : kPadX;
    g_swap_prev = 0;
    snes_mod_register_frame_callback(SwapFrame);
}

/* ---- cheats -------------------------------------------------------------- */

/* Live switches (the in-game settings): the frame callbacks are always
 * installed (ResetHd) and do nothing while off. */
static int g_cheat_hp, g_cheat_dp, g_cheat_stats;

static void InfiniteHpFrame(void)
{
    if (!g_cheat_hp) return;
    /* $0ACE current HP, $0ACA max (the regen at $00:8206 compares them).
     * Zero is left alone so a death already under way is not undone. */
    if (g_snes && Rd(0x09AA) && Rd(0x0ACE) && Rd(0x0ACE) < Rd(0x0ACA))
        Wr(0x0ACE, Rd(0x0ACA));
}

static uint16_t g_min_dp;
static void DpFrame(void)
{
    if (!g_cheat_dp) return;
    if (g_snes && Rd(0x0AD6) < g_min_dp)
        Wr(0x0AD6, g_min_dp);
}

static uint16_t g_min_str, g_min_def, g_min_maxhp;
static void StatsFrame(void)
{
    if (!g_cheat_stats) return;
    if (!g_snes || !Rd(0x09AA))
        return;   /* only in play: the title and file select use the same RAM */
    if (Rd(0x0ADE) < g_min_str)
        Wr(0x0ADE, g_min_str);
    if (Rd(0x0ADC) < g_min_def)
        Wr(0x0ADC, g_min_def);
    if (Rd(0x0ACA) < g_min_maxhp) {   /* max HP; current HP follows */
        Wr(0x0ACA, g_min_maxhp);
        Wr(0x0ACE, g_min_maxhp);
    }
}

/* Cheats make a session unfair for RetroAchievements: not loaded then. */
static const char kRaCheats[] = "cheats are on";

static void ActivateInfiniteHp(void)
{
    SnesRa_SetBlocked(kRaCheats);
    g_cheat_hp = 1;
}

static void ActivateDp(void)
{
    SnesRa_SetBlocked(kRaCheats);
    g_min_dp = (uint16_t)OptionInt("iog.cheats", "dp", "amount", 99);
    g_cheat_dp = 1;
}

static void ActivateStats(void)
{
    SnesRa_SetBlocked(kRaCheats);
    g_min_str = (uint16_t)OptionInt("iog.cheats", "stats", "strength", 0);
    g_min_def = (uint16_t)OptionInt("iog.cheats", "stats", "defense", 0);
    g_min_maxhp = (uint16_t)OptionInt("iog.cheats", "stats", "max_hp", 0);
    g_cheat_stats = 1;
}

/* ---- hold to advance dialogue --------------------------------------------- */

/* Text waits ([FIN] / [WAI], $03:E6A4 / $03:E6D2) act on a NEW press: the
 * edge in $0656, so a held button counts once. While a message is up
 * ($09EC & $2000) and no choice is being asked (COP BE sets $065C = $0F00
 * for its menu and clears it after), the held confirm buttons are let go for
 * one frame in every `g_advance_period`, timed by the NMI counter $36: the
 * next frame is a fresh press and the page turns. A pure function of the
 * pad word and RAM, so run-ahead and netplay frames see the same thing. */
static uint8_t g_advance_period;   /* 0: off */

static uint32_t HoldAdvanceFilter(uint32_t in)
{
    if (!g_advance_period || !g_snes || !Rd(0x09AA))
        return in;
    const uint32_t adv = in & (kPadA | kPadB | kPadY);
    if (!adv || !(Rd(0x09EC) & 0x2000) || Rd(0x065C) != 0)
        return in;
    if ((g_snes->ram[0x36] % g_advance_period) == 0)
        in &= ~adv;
    return in;
}

static void ActivateHoldAdvance(void)
{
    char v[16] = "normal";
    snes_mod_runtime_feature_option_value_c("iog.enhancements", "hold_advance", "speed", v, sizeof(v));
    g_advance_period = !strcmp(v, "fast") ? 8 : !strcmp(v, "slow") ? 32 : 16;
}

/* In-game settings (iog_settings.c): 0 off, 1 slow, 2 normal, 3 fast. */
int IogHoldAdvanceGet(void)
{
    return g_advance_period == 0 ? 0 : g_advance_period == 32 ? 1 : g_advance_period == 16 ? 2 : 3;
}
void IogHoldAdvanceSet(int v)
{
    static const uint8_t kPeriods[] = {0, 32, 16, 8};
    g_advance_period = kPeriods[v & 3];
}

/* ---- HD scaling (presentation only) -------------------------------------- */

/* How the picture is scaled up to the screen: crisp square pixels, or the
 * art's edges redrawn smooth (present_filter.h). Off: the [Graphics]
 * LinearFiltering setting, as without mods. */
static void ActivateHd(void)
{
    char v[16] = "";
    snes_mod_runtime_feature_option_value_c("iog.graphics", "hd", "filter", v, sizeof(v));
    PresentFilter_Set(strcmp(v, "smooth") == 0 ? kPresentFilter_Smooth : kPresentFilter_Sharp);
}

static void ResetHd(void)
{
    /* The filter is always installed; it does nothing while the period is 0,
     * so the in-game settings can turn it on mid-session. */
    g_advance_period = 0;
    snes_mod_register_input_filter(HoldAdvanceFilter);
    g_cheat_hp = g_cheat_dp = g_cheat_stats = 0;
    g_min_dp = 99;
    snes_mod_register_frame_callback(InfiniteHpFrame);
    snes_mod_register_frame_callback(DpFrame);
    snes_mod_register_frame_callback(StatsFrame);
    SnesRa_SetBlocked(NULL);   /* each launch decides again (plugins follow) */
    PresentFilter_Set(kPresentFilter_Config);
}

SNES_MOD_CONSTRUCTOR(register_iog_mods)
{
    snes_mod_register_reset_callback(ResetHd);
    snes_mod_register_presentation_plugin("iog.hd", ActivateHd);
    snes_mod_register_activation_plugin("iog.run", ActivateRun);
    snes_mod_register_activation_plugin("iog.hold_advance", ActivateHoldAdvance);
    snes_mod_register_activation_plugin("iog.hotswap", ActivateSwap);
    snes_mod_register_activation_plugin("iog.cheat.hp", ActivateInfiniteHp);
    snes_mod_register_activation_plugin("iog.cheat.dp", ActivateDp);
    snes_mod_register_activation_plugin("iog.cheat.stats", ActivateStats);
}

/* In-game settings (iog_settings.c): 0 infinite HP, 1 DP, 2 stats. */
int IogCheatGet(int which)
{
    return which == 0 ? g_cheat_hp : which == 1 ? g_cheat_dp : g_cheat_stats;
}

void IogCheatSet(int which, int on)
{
    static const char *const kFeat[] = {"infinite_hp", "dp", "stats"};
    if (which < 0 || which > 2) return;
    if (which == 0) g_cheat_hp = on; else if (which == 1) g_cheat_dp = on; else g_cheat_stats = on;
    if (on) SnesRa_SetBlocked(kRaCheats);
    snes_mod_runtime_persist_feature_c("iog.cheats", kFeat[which], on, NULL, NULL);
}
