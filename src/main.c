/*
 * Illusion Of Gaia Usa — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.h): the pre-boot launcher, ROM
 * resolution and digest checks, config.ini and keybinds.ini, the window and
 * the SDL / OpenGL presenters, audio, gamepads, the in-game save-state
 * browser and rewind filmstrip, the OSD, the pacing clock, crash handlers and
 * the post-mortem report, mod packages, Generate & rebuild, and the netplay
 * barrier when the project is built with it. A fix there reaches this
 * project on a submodule pull; nothing in this file needs to change for it.
 *
 * This file used to BE the host -- a ~700-line copy made at scaffold time --
 * so every project scaffolded before a fix kept the bug. Two ports of the
 * same game, a week apart, had different hosts.
 *
 * Nothing here ever ships a ROM. The player supplies one; the host makes
 * that easy (launcher, then positional argument, then a copy beside the
 * executable, then the rom.cfg cache, then a file picker) and checks what it
 * is handed against the digests in rom_identity.txt.
 *
 * Grow this file only with what is specific to THIS title: a custom
 * presenter (prepare_frame / draw_frame), an SPC player, a pacing rule
 * (keep_pacing_debt), a Mods provider. See SnesDesktopHostGame for the
 * complete list of hooks and what each is for.
 */

#include "host_main.h"
#include "game_rtl.h"
#include "iog_widescreen.h"
#include "iog_inventory.h"
#include "iog_i18n.h"
#include "snes_osd.h"
#include <stdio.h>
#include "snesrecomp_rom_identity.h"  /* generated from rom_identity.txt */
#include "desktop/config.h"
#include "desktop/display_aspect.h"

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"
#ifdef __ANDROID__
/* SDLActivity loads libmain.so and dlsyms its SDL_main. */
#define main SDL_main
#endif

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

/* Widescreen: the PPU rasterizes a field widened by 4/3 (342 px for the
 * default 4:3 pixel aspect, i.e. 16:9 on screen), centred on the authentic
 * 256 columns. Off, the field stays 256 wide and 4:3 is bit-identical. */
static void IogPrepareFrame(int drawable_w, int drawable_h, int *frame_w, int *frame_h)
{
    (void)drawable_w; (void)drawable_h; (void)frame_h;
    if (g_config.widescreen)
        *frame_w = SnesDisplayAspect_ComputeWideFrameWidth(256);
}

static void IogBeginSimFrame(unsigned number)
{
    (void)number;
    IogWidescreenBeginFrame();
}

/* Ctrl+L: next language. Polled here because the language is a title
 * feature, not a framework key binding. */
static void IogPollLanguageHotkey(void)
{
    static int was_down;
    const uint8_t *keys = snesrecomp_sdl_get_keyboard_state();
    if (!keys)
        return;
    int down = keys[SDL_SCANCODE_L] &&
               (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]);
    if (down && !was_down) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Language: %s", IogI18nCycle());
        snes_osd_push(msg, 2000);
    }
    was_down = down;
}

static void IogBeforeRunFrame(void)
{
    static int initialized;
    if (!initialized) {
        initialized = 1;
        IogI18nInit();
    }
    IogPollLanguageHotkey();
    IogI18nFrame();
    IogWidescreenBeforeFrame();
}

#include "iog_companion.h"
/* After a frame is rasterised: presentation added by mods. */
static void IogEndSimFrame(const uint8_t *field, unsigned number)
{
    (void)field;
    (void)number;
    IogCompanionGrabMenu();
    IogInventoryOverlay();
}

void IogSettingsInstall(void);   /* iog_settings.c */
static void IogAfterRunFrame(const SnesDesktopHostFrameStats *st) { (void)st; IogCompanionAfterFrame(); }

static const SnesDesktopHostGame kGameHost = {
    .display_name        = "Illusion Of Gaia Usa",
    .window_title        = "Babel Recomp",
    .region              = SNESRECOMP_ROM_REGION,
    .rom_file            = SNESRECOMP_ROM_FILE,
    .expected_sha256_hex = SNESRECOMP_ROM_EXPECTED_SHA256,
    .expected_crc32_hex  = SNESRECOMP_ROM_EXPECTED_CRC32,
    .game_id             = SNESRECOMP_ROM_GAME_ID,
    .build_version       = SNES_GAME_VERSION,
    .game_info           = &kGameInfo,
    .num_players         = 1,
    /* Battery-backed SRAM shows the launcher's SAVES panel. Leave NULL for a
     * title without one. The path is exe-relative. */
    .sram_path           = "saves/save.srm",
    .widescreen_supported = 1,
    .native_widescreen   = 1,
    /* RetroArch GLSL presets (.glslp/.glsl) on the OpenGL presenter. */
    .shader_supported    = 1,
    .prepare_frame       = IogPrepareFrame,
    .begin_sim_frame     = IogBeginSimFrame,
    .before_run_frame    = IogBeforeRunFrame,
    .end_sim_frame       = IogEndSimFrame,
    .after_config        = IogSettingsInstall,
    .after_run_frame     = IogAfterRunFrame,
    .companion_open      = IogCompanionOpen,
    .companion_nav       = IogCompanionNav,
    .companion_key       = IogCompanionKey,
    .companion_pointer   = IogCompanionPointer,
    .companion_image     = IogCompanionImage,
    .companion_live      = IogCompanionLive,
};

#ifndef __ANDROID__
#undef main   /* desktop: keep plain main() even if SDL_main.h remapped it */
#endif
int main(int argc, char **argv)
{
    return snesrecomp_desktop_main(&kGameHost, argc, argv);
}
