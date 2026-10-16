/*
 * Illusion of Gaia widescreen policy.
 *
 * The host rasterizes a field widened on both sides (native_widescreen). IoG
 * maps are authored as a world [0, map_bounds_x) that the camera scrolls
 * across, so each margin may show real map content only as far as the world
 * extends past the camera on that side; beyond the world edge the BG tilemap
 * just wraps (on the title screen a second copy of the logo, in rooms the far
 * end of the map). Those pixels stay black instead.
 *
 * This is the zelda3 model (PpuSetExtraSideSpace): margins are recomputed
 * every frame from the camera and the map bounds. Presentation only: no
 * emulated register or WRAM byte is written, and with widescreen off the
 * budget is 0 and nothing here runs.
 */

#include "iog_widescreen.h"
#include "iog_randomizer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/cart.h"

/* WRAM (GaiaLabs RAM map). */
enum {
    kWramCameraX    = 0x068A,  /* layer_camera_positions: BG1 camera X */
    kWramMapBoundsX = 0x0692,  /* maplayer_bounds_x: BG1 world width, px */
};

static uint16_t WramWord(uint32_t addr)
{
    const uint8_t *ram = g_snes->ram;
    return (uint16_t)(ram[addr] | ram[addr + 1] << 8);
}

/* Camera X for the line the PPU is about to render. The phase comes from the
 * PPU's own BG1 scroll (WIDESCREEN_PATTERNS P1); WRAM only supplies the bits
 * the 10-bit hardware scroll has lost. */
static int CameraX(void)
{
    int wram = WramWord(kWramCameraX);
    int cam = (wram & ~0x3FF) | (g_ppu->hScroll[0] & 0x3FF);
    if (cam - wram > 512) cam -= 1024;
    if (wram - cam > 512) cam += 1024;
    return cam;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void IogWidescreenBeginFrame(void)
{
    if (!g_ppu || !g_snes || g_ppu->extraLeftRight == 0)
        return;
    const int budget = g_ppu->extraLeftRight;
    int bounds = WramWord(kWramMapBoundsX);
    /* No world loaded (boot, some menus): keep the authentic 256 columns. */
    if (bounds < 256)
        bounds = 256;
    const int cam = CameraX();
    int left = Clamp(cam, 0, budget);
    int right = Clamp(bounds - (cam + 256), 0, budget);
    /* Mode 7 (prologue sea, world map) is a plane that genuinely continues
     * past the screen edges; map bounds do not apply to it. */
    if ((g_ppu->bgmode & 7) == 7)
        left = right = budget;
    PpuSetExtraSideSpace(g_ppu, left, right, 0);
    /* In-game HUD (BG3 rows 0-3: PLAYER + life orbs | DP ornament | ENEMY):
     * anchor its outer groups to the widened edges. Only while the HUD is
     * really there, so title, file select, menus and the prologue keep their
     * BG3 layout. Keyed on the DP ornament (row 1: $0EF at columns 11 and 20,
     * $01E at 15), which is drawn with the HUD in every release; the PLAYER
     * label is blank for a while after a scene loads. */
    const uint16_t *map = &g_ppu->vram[PPU_bgTilemapAdr(g_ppu, 2) & 0x7fff];
    const uint16_t *row1 = map + 32;
    if ((row1[11] & 0x3ff) == 0x0EF && (row1[20] & 0x3ff) == 0x0EF &&
        (row1[15] & 0x3ff) == 0x01E) {
        PpuSetWidescreenHudSplit(g_ppu, 32, 88, 168);
        PpuSetWidescreenHudAlwaysVisible(g_ppu, true);
    } else {
        /* The split is sticky PPU state: drop it as soon as the HUD goes
         * (file select, menus), or their BG3 boxes get torn at column 168. */
        PpuSetWidescreenHudSplit(g_ppu, 0, 0, 0);
        PpuSetWidescreenHudAlwaysVisible(g_ppu, false);
    }
    /* The renderer only writes the live span of each line; what lies outside
     * it would keep an older frame's pixels, so blank it first. */
    if (g_ppu->renderBuffer && (left < budget || right < budget)) {
        for (int y = 0; y < 240; y++) {
            uint32_t *line = (uint32_t *)&g_ppu->renderBuffer[(size_t)y * g_ppu->renderPitch];
            if (left < budget)
                memset(line, 0, sizeof(uint32_t) * (size_t)(budget - left));
            if (right < budget)
                memset(line + budget + 256 + right, 0, sizeof(uint32_t) * (size_t)(budget - right));
        }
    }
    if (getenv("IOG_WS_LOG"))
        fprintf(stderr, "[iog_ws] cam=%d wram=%d hs=%d bounds=%d -> L=%d R=%d cur=%d/%d\n", cam,
                WramWord(kWramCameraX), g_ppu->hScroll[0], bounds, left, right,
                g_ppu->extraLeftCur, g_ppu->extraRightCur);
}

/* ---- sprite culling --------------------------------------------------------
 *
 * Two routines of bank $03 drop what lies outside the native 256 columns.
 * Both are run by the interpreter (no compiled variant for the M/X state
 * they are called in), so their immediates can be retargeted in the ROM
 * image; the originals are checked first and restored when the margin goes.
 *
 *  - Actor list (func_03C5FF): keeps an actor whose box overlaps
 *    [camera, camera+256):  SBC $068A ; CMP #$0100.  The camera operand is
 *    pointed at a spare WRAM word holding camera - margin, and the window
 *    becomes 256 + 2*margin wide (WIDESCREEN_PATTERNS P7: widen, keep base).
 *  - OAM writer (func_03C714 / sub $C849): base $1A = camera - 16; each
 *    piece is kept when x + 16 < $0110 and written as x = (sum) - 16. Base,
 *    limit and bias grow by the margin so pieces in the margins get their
 *    9-bit X instead of being parked at $E080. Same for the player path.
 *  - Extra sprite list ($7F:3100, $C797): x - camera < $0110; widening the
 *    limit lets the right margin through (its stored X is not biased, so
 *    the left margin cannot be reached without new code).
 *
 * Nothing is patched while the margin is 0 (4:3 is untouched). */
enum { kWramCullCamera = 0x1E00 };   /* spare word, never used by the game */

typedef struct CullSite {
    uint32_t offset;
    uint16_t stock;
    int16_t per_margin;              /* value = stock + per_margin * margin */
    uint8_t camera;                  /* operand becomes kWramCullCamera */
} CullSite;

static const CullSite kCullSites[] = {
    {0x03C617, 0x068A, 0, 1}, {0x03C61A, 0x0100, 2, 0},   /* actor list, left */
    {0x03C62A, 0x068A, 0, 1}, {0x03C62D, 0x0100, 2, 0},   /* actor list, right */
    {0x03C729, 0x0010, 1, 0},                              /* OAM base */
    {0x03C8E5, 0x0110, 2, 0}, {0x03C8EA, 0x000F, 1, 0},   /* actor pieces */
    {0x03C9C2, 0x0110, 2, 0}, {0x03C9C7, 0x000F, 1, 0},   /* player pieces */
    {0x03C7BE, 0x0110, 1, 0},                              /* extra list */
};
enum { kNumCullSites = sizeof(kCullSites) / sizeof(kCullSites[0]) };
static int g_cull_margin;            /* margin the ROM is patched for, 0: stock */
static int g_cull_unknown;           /* not the ROM we know: leave it alone */

static void PatchCull(int margin)
{
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart || g_cull_unknown || margin == g_cull_margin)
        return;
    if (!g_cull_margin) {
        for (int i = 0; i < kNumCullSites; i++) {
            const CullSite *c = &kCullSites[i];
            if (c->offset + 2 > cart->romSize ||
                (cart->rom[c->offset] | cart->rom[c->offset + 1] << 8) != c->stock) {
                g_cull_unknown = 1;
                return;
            }
        }
    }
    for (int i = 0; i < kNumCullSites; i++) {
        const CullSite *c = &kCullSites[i];
        uint16_t v = !margin ? c->stock
                   : c->camera ? kWramCullCamera
                   : (uint16_t)(c->stock + c->per_margin * margin);
        cart->rom[c->offset] = (uint8_t)v;
        cart->rom[c->offset + 1] = (uint8_t)(v >> 8);
    }
    g_cull_margin = margin;
}

void IogWidescreenBeforeFrame(void)
{
    if (!g_ppu || !g_snes)
        return;
    static int disabled = -1;
    if (disabled < 0)
        disabled = getenv("IOG_WS_NO_CULL_PATCH") != NULL;
    PatchCull(disabled || IogRandomizerActive() ? 0 : g_ppu->extraLeftRight);
    if (g_cull_margin) {
        uint16_t cam = (uint16_t)(WramWord(kWramCameraX) - g_cull_margin);
        g_snes->ram[kWramCullCamera] = (uint8_t)cam;
        g_snes->ram[kWramCullCamera + 1] = (uint8_t)(cam >> 8);
    }
}
