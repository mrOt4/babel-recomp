/*
 * Companion screen for Illusion of Gaia, in the game's own art.
 *
 * The pause menu (Select: the stone pedestal with the inventory, scene $FF)
 * and its "Cond." page are opened OFF-SCREEN with the host's speculative
 * capture (snes_host_capture.h): the presses happen in a speculative
 * timeline, the game draws its own screens, the pictures are kept and the
 * timeline is thrown away. Text uses the game's own font (BG3, from VRAM),
 * and the boxes of every page are cut from the menu's box art. Nothing of
 * the art is shipped: it is the player's ROM, drawn by the game.
 *
 * Pages: ITEMS (equip by pad or touch), STATUS, SAVES (the framework's
 * save-state slots), SETTINGS (hands over to the settings menu). Composed
 * at the game's 256x224 and presented at 2x.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes_host_capture.h"
#include "snes_overlay_draw.h"
#include "snes_savestate_menu.h"
#include "snes_settings_menu.h"

#include "iog_companion.h"
#include "iog_inventory.h"
int IogCompletionPercent(void);
#include "iog_i18n.h"

extern Ppu *g_ppu;

enum { kW = 256, kH = 224, kCapW = 512, kCapH = 240 };
enum { kPadSelect = 0x004, kPadDown = 0x020, kPadA = 0x100 };
enum { kPageItems, kPageStatus, kPageSaves, kPageSettings, kPageCount };
static const char *const kPageNames[kPageCount] = {"Items", "Status", "Saves", "Config"};

static uint32_t g_inv[kCapW * kCapH], g_stat[kCapW * kCapH];
static int g_inv_w, g_stat_w, g_have_inv, g_have_stat;
static uint32_t g_canvas[kW * kH];          /* composed at 1x */
static uint32_t g_panel[kW * 2 * kH * 2];   /* presented at 2x */

static int g_open, g_page, g_slot, g_save_slot, g_confirm = -1;
/* Live (Dual / Flip Grip): drawn beside the running game every frame. */
static int g_live, g_live_seen;
static uint8_t g_inv_items[16];   /* the inventory the capture shows */
static uint32_t g_prev_nav;
static int g_ptr_down;

static uint16_t Rd(uint32_t a) { return g_snes->ram[a] | g_snes->ram[a + 1] << 8; }
static void Wr(uint32_t a, uint16_t v) { g_snes->ram[a] = (uint8_t)v; g_snes->ram[a + 1] = (uint8_t)(v >> 8); }

/* ---- capture -------------------------------------------------------------- */
typedef struct {
    int menu_frames, until_menu;   /* stop once the menu has been up this long */
    int status;                    /* also walk to Cond. and open it */
} CapCtx;

static uint32_t CapInput(int frame, void *ud)
{
    const CapCtx *c = (const CapCtx *)ud;
    if (frame < 2) return kPadSelect;
    if (!c->status) return 0;
    /* Items -> Order -> Remove -> Cond., then A: two frames each. */
    if ((frame >= 24 && frame < 26) || (frame >= 30 && frame < 32) || (frame >= 36 && frame < 38))
        return kPadDown;
    if (frame >= 42 && frame < 44) return kPadA;
    return 0;
}

static int CapStop(int frame, void *ud)
{
    CapCtx *c = (CapCtx *)ud;
    if (Rd(0x0644) == 0x00FF) c->menu_frames++;
    return c->status ? frame >= 56 : c->menu_frames >= c->until_menu;
}

static int InPlayQuiet(void)
{
    return g_snes && Rd(0x09AA) != 0 && Rd(0x0644) != 0x00FF && !(Rd(0x09EC) & 0x6000);
}

/* The menu's own font (BG3 2bpp, 256 characters), taken from VRAM while
 * the captured menu is on screen: the gameplay font differs in places. */
static uint16_t g_font[256 * 8];
static int g_have_font;

static void GrabFont(void *ud)
{
    (void)ud;
    if (!g_ppu) return;
    const uint16_t *vram = PpuRenderVram(g_ppu);
    const unsigned base = PPU_bgTileAdr(g_ppu, 2);
    for (int i = 0; i < 256 * 8; i++)
        g_font[i] = vram[(base + i) & 0x7fff];
    g_have_font = 1;
}

static int Capture(int status, uint32_t *dst, int *w)
{
    CapCtx c = {0, 22, status};
    if (!status) snes_host_speculative_set_grab(GrabFont);
    int cw = 0, ch = 0;
    if (!InPlayQuiet()) return 0;
    if (!snes_host_speculative_capture(CapInput, CapStop, &c, 120, SNES_SPEC_FULL_BRIGHTNESS,
                                       dst, kCapW * kCapH, &cw, &ch))
        return 0;
    *w = cw;
    return 1;
}

/* The player opened the pause menu: keep its picture as it is drawn (no
 * speculative frames, so no cost). Called after the frame is rasterised,
 * before the inventory enhancements draw over it. */
void IogCompanionGrabMenu(void)
{
    if (!g_ppu || !g_ppu->renderBuffer || RtlSpeculativeFrame() || !IogInventoryPageShown())
        return;
    const int w = 256 + 2 * g_ppu->extraLeftRight;
    if (w > kCapW)
        return;
    for (int y = 0; y < kH; y++)
        memcpy(g_inv + y * w, g_ppu->renderBuffer + (size_t)y * g_ppu->renderPitch, (size_t)w * 4);
    g_inv_w = w;
    g_have_inv = 1;
    memcpy(g_inv_items, &g_snes->ram[0x0AB4], 16);
    GrabFont(NULL);
}

int IogCompanionCaptureInventory(void)
{
    if (Capture(0, g_inv, &g_inv_w)) {
        g_have_inv = 1;
        memcpy(g_inv_items, &g_snes->ram[0x0AB4], 16);
    }
    return g_have_inv;
}

const uint32_t *IogCompanionInventoryImage(int *w, int *h)
{
    if (!g_have_inv) return NULL;
    if (w) *w = g_inv_w;
    if (h) *h = kH;
    return g_inv;
}

/* ---- drawing ---------------------------------------------------------------- */
static void Put(int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < kW && y < kH) g_canvas[y * kW + x] = c | 0xFF000000u;
}

static void Fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            Put(x + i, y + j, c);
}

/* The 256 centre columns of a (possibly widescreen) capture. */
static void Blit(const uint32_t *src, int src_w)
{
    const int off = (src_w - kW) / 2;
    for (int y = 0; y < kH; y++)
        memcpy(g_canvas + y * kW, src + y * src_w + (off > 0 ? off : 0), kW * 4);
}

/* A box in the menu's art: corners and edges cut from the left box of the
 * captured menu, (8,151)-(82,216), borders 5/7 px left/right, 8/6 top/bottom.
 * Without a capture: a plain frame in its colours. */
static void Box(int x, int y, int w, int h)
{
    enum { BX = 8, BY = 151, BR = 82, BB = 216, L = 5, R = 7, T = 8, B = 6 };
    if (!g_have_inv) {
        Fill(x, y, w, h, 0x9C9CBD);
        Fill(x + 2, y + 2, w - 4, h - 4, 0x3952B5);
        Fill(x + 5, y + 5, w - 10, h - 10, 0x000000);
        return;
    }
    const int off = (g_inv_w - kW) / 2;
    const uint32_t *s = g_inv + (off > 0 ? off : 0);
    const int sw = g_inv_w;
#define SRC(px, py) s[(py) * sw + (px)]
    for (int j = 0; j < h; j++) {
        int sy;
        if (j < T) sy = BY + j;
        else if (j >= h - B) sy = BB - (h - j);
        else sy = BY + T + 20;               /* an interior row of the edge */
        for (int i = 0; i < w; i++) {
            int sx;
            if (i < L) sx = BX + i;
            else if (i >= w - R) sx = BR - (w - i);
            else sx = BX + L + 30;
            const int inside = i >= L && i < w - R && j >= T && j < h - B;
            Put(x + i, y + j, inside ? 0x000000 : SRC(sx, sy));
        }
    }
#undef SRC
}

/* A character of the game's font (BG3, 2bpp; tile = ASCII code). */
/* ASCII -> the game's character codes (GaiaLabs' ASCIIString map): mostly
 * the same, but '.' is $20, '-' is $2E, '(' ')' are $3C $3E and "'" $5C. */
static unsigned char GameCode(unsigned char c)
{
    switch (c) {
    case '.': return 0x20;
    case '-': return 0x2E;
    case '(': return 0x3C;
    case ')': return 0x3E;
    case '\'': return 0x5C;
    default: return c;
    }
}

static void CharRaw(int x, int y, unsigned char code, uint32_t fg, uint32_t shade);

/* A character of the game's font: plane value 1 is the face, 2 its shading
 * (the beige of the menu's text), 3 the drop shadow. */
static void Char(int x, int y, unsigned char ch, uint32_t fg, uint32_t shade)
{
    if (ch == ' ') return;
    if (!g_have_font) {
        if (!g_ppu) return;
        GrabFont(NULL);   /* fallback: the gameplay font in VRAM now */
        g_have_font = 0;
    }
    CharRaw(x, y, GameCode(ch), fg, shade);
}

static void CharRaw(int x, int y, unsigned char ch, uint32_t fg, uint32_t shade)
{
    if (!g_have_font && g_ppu) {
        GrabFont(NULL);
        g_have_font = 0;
    }
    for (int r = 0; r < 8; r++) {
        const uint16_t w = g_font[ch * 8 + r];
        for (int c = 0; c < 8; c++) {
            const int p = ((w >> (7 - c)) & 1) | ((w >> (15 - c)) & 1) << 1;
            if (p == 1) Put(x + c, y + r, fg);
            else if (p == 2) Put(x + c, y + r, shade);
            else if (p == 3) Put(x + c, y + r, 0x101010);
        }
    }
}

static uint32_t Shade(uint32_t fg)
{
    /* The menu's text: white face, beige shading; other colours darken. */
    if (fg == 0xFFFFFF || fg == 0xDEC6B5) return 0xDEC6B5;
    return (fg >> 1) & 0x7F7F7F;
}

/* UTF-8 accents -> the European font's codes (read off the font itself):
 * ü $83 é $84 á $85 í $86 ó $87 ú $88 ñ $89 Ñ $8A ¿ $8B ¡ $8C. It has no
 * accented capitals: those are drawn plain. */
static unsigned char AccentCode(unsigned cp)
{
    switch (cp) {
    case 0xFC: return 0x83; case 0xE9: return 0x84; case 0xE1: return 0x85;
    case 0xED: return 0x86; case 0xF3: return 0x87; case 0xFA: return 0x88;
    case 0xF1: return 0x89; case 0xD1: return 0x8A; case 0xBF: return 0x8B;
    case 0xA1: return 0x8C;
    case 0xC1: return 'A'; case 0xC9: return 'E'; case 0xCD: return 'I';
    case 0xD3: return 'O'; case 0xDA: return 'U'; case 0xDC: return 'U';
    default: return '?';
    }
}

static void Text(int x, int y, const char *s, uint32_t fg)
{
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; x += 8) {
        unsigned cp = *p++;
        if ((cp & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) {
            cp = (cp & 0x1F) << 6 | (*p++ & 0x3F);
            CharRaw(x, y, AccentCode(cp), fg, Shade(fg));
            continue;
        }
        Char(x, y, (unsigned char)cp, fg, Shade(fg));
    }
}

/* The companion's words in the game's language. */
static const char *T(const char *en)
{
    static const char *const kEs[][2] = {
        {"Items", "Objetos"}, {"Status", "Estado"}, {"Saves", "Partida"}, {"Config", "Ajustes"},
        {"Touch to choose", "Toca para elegir"}, {"L R:Page  B:Back", "L R:Pág.  B:Atrás"},
        {"A: Equip", "A: Equipar"}, {"(empty)", "(vacío)"}, {"Equipped", "Equipado"},
        {"Slot %02d", "Ranura %02d"}, {"Empty", "Vacía"}, {"A again: LOAD", "A otra vez: CARGAR"},
        {"X again: SAVE", "X otra vez: GUARDAR"}, {"A: Load", "A: Cargar"}, {"X: Save", "X: Guardar"},
        {"A: Open settings", "A: Abrir ajustes"}, {"Completed", "Completado"},
    };
    if (!en || strcmp(IogI18nLanguage(), "es")) return en;
    for (size_t i = 0; i < sizeof(kEs) / sizeof(kEs[0]); i++)
        if (!strcmp(kEs[i][0], en)) return kEs[i][1];
    return en;
}

static void Cursor(int x, int y, uint32_t c)
{
    for (int j = 0; j < 7; j++)
        for (int i = 0; i <= (j < 4 ? j : 6 - j); i++)
            Put(x + i, y + j, c);
}

static void Frame(int x, int y, int w, int h, uint32_t c)
{
    Fill(x, y, w, 1, c);
    Fill(x, y + h - 1, w, 1, c);
    Fill(x, y, 1, h, c);
    Fill(x + w - 1, y, 1, h, c);
}

enum { kText = 0xFFFFFF, kHi = 0xFFE890, kGold = 0xF8D050, kTeal = 0x73E6CE };

static void PageList(void)
{
    Box(8, 151, 74, 65);
    for (int p = 0; p < kPageCount; p++) {
        const int y = 160 + p * 12;
        if (p == g_page) Cursor(14, y, kHi);
        Text(22, y, T(kPageNames[p]), p == g_page ? kHi : kText);
    }
}

static void Hint(const char *l1, const char *l2, const char *l3)
{
    Box(82, 151, 168, 65);
    if (l1) Text(92, 160, T(l1), kText);
    if (l2) Text(92, 172, T(l2), kText);
    if (l3) Text(92, 184, T(l3), kText);
    Text(92, 200, T(g_live ? "Touch to choose" : "L R:Page  B:Back"), kTeal);
}

/* ---- pages ---------------------------------------------------------------- */
static void SlotPos(int i, int *x, int *y)
{
    const uint8_t *e = g_snes->cart->rom + 0x02EB86 + 4 * i;   /* the menu's slot table */
    *x = e[0] | e[1] << 8;
    *y = e[2] | e[3] << 8;
}

static void DrawItems(void)
{
    if (g_have_inv) Blit(g_inv, g_inv_w);
    else Fill(0, 0, kW, kH, 0x001060);
    const uint8_t *slots = &g_snes->ram[0x0AB4];
    const int equipped = (int16_t)Rd(0x0AC4);
    for (int i = 0; i < 16; i++) {
        int x, y;
        SlotPos(i, &x, &y);
        if (i == equipped && slots[i]) Frame(x - 10, y - 13, 20, 16, kGold);
        if (i == g_slot) {
            Frame(x - 11, y - 14, 22, 18, kHi);
            Frame(x - 12, y - 15, 24, 20, 0x3952B5);
        }
    }
    PageList();
    Hint(slots[g_slot] ? "A: Equip" : "(empty)", g_slot == equipped ? "Equipped" : NULL, NULL);
}

static void Number(int x, int y, unsigned n, int width)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%*u", width, n);
    Text(x, y, buf, kHi);
}

static void DrawStatus(void)
{
    if (g_have_stat && !g_live) {
        Blit(g_stat, g_stat_w);
    } else {
        if (g_have_inv) Blit(g_inv, g_inv_w); else Fill(0, 0, kW, kH, 0x001060);
        Box(8, 8, 240, 136);
        Text(24, 24, "HP", kTeal);
        Number(56, 24, Rd(0x0ACE), 3);
        Text(80, 24, "/", kText);
        Number(88, 24, Rd(0x0ACA), 3);
        Text(24, 40, "DP", kTeal);
        Number(56, 40, Rd(0x0AD6), 3);
        Text(24, 56, "STR", kTeal);
        Number(56, 56, Rd(0x0ADE), 3);
        Text(24, 72, "DEF", kTeal);
        Number(56, 72, Rd(0x0ADC), 3);
    }
    PageList();
    {
        static char buf[24];
        const int pct = IogCompletionPercent();
        if (pct >= 0) snprintf(buf, sizeof(buf), "%s %d/100", T("Completed"), pct);
        Hint(pct >= 0 ? buf : NULL, NULL, NULL);
    }
}

static uint32_t g_thumb[SNES_SAVESTATE_THUMB_W * SNES_SAVESTATE_THUMB_H];

static void DrawSaves(void)
{
    if (g_have_stat) Blit(g_stat, g_stat_w);
    else if (g_have_inv) Blit(g_inv, g_inv_w);
    else Fill(0, 0, kW, kH, 0x001060);
    Box(8, 8, 242, 142);
    const int first = g_save_slot - 1 < 0 ? 0 : g_save_slot - 1 > SNES_SAVESTATE_SLOTS - 3
                      ? SNES_SAVESTATE_SLOTS - 3 : g_save_slot - 1;
    for (int k = 0; k < 3; k++) {
        const int s = first + k, y = 18 + k * 42;
        char buf[40];
        if (s == g_save_slot) Cursor(16, y + 16, kHi);
        /* Thumbnail at a third of its size: 42x37. */
        if (snes_savestate_slot_thumb(s, g_thumb)) {
            for (int j = 0; j < 37; j++)
                for (int i = 0; i < 42; i++)
                    Put(26 + i, y + j, g_thumb[(j * 3) * SNES_SAVESTATE_THUMB_W + i * 3]);
        } else {
            Fill(26, y, 42, 37, 0x10183A);
        }
        Frame(25, y - 1, 44, 39, s == g_save_slot ? kHi : 0x3952B5);
        snprintf(buf, sizeof(buf), T("Slot %02d"), s + 1);
        Text(76, y + 6, buf, s == g_save_slot ? kHi : kText);
        if (snes_savestate_slot_exists(s)) {
            snes_savestate_slot_label(s, buf, sizeof(buf));
            Text(76, y + 20, buf, kText);
        } else {
            Text(76, y + 20, T("Empty"), 0x8C8CA5);
        }
    }
    PageList();
    if (g_confirm == 0) Hint("A again: LOAD", NULL, NULL);
    else if (g_confirm == 1) Hint("X again: SAVE", NULL, NULL);
    else Hint("A: Load", "X: Save", NULL);
}

static void DrawSettings(void)
{
    if (g_have_inv) Blit(g_inv, g_inv_w); else Fill(0, 0, kW, kH, 0x001060);
    PageList();
    Hint("A: Open settings", NULL, NULL);
}

static void Compose(void)
{
    switch (g_page) {
    case kPageItems: DrawItems(); break;
    case kPageStatus: DrawStatus(); break;
    case kPageSaves: DrawSaves(); break;
    default: DrawSettings(); break;
    }
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++) {
            const uint32_t c = g_canvas[y * kW + x] | 0xFF000000u;
            uint32_t *d = g_panel + (size_t)(2 * y) * (2 * kW) + 2 * x;
            d[0] = d[1] = d[2 * kW] = d[2 * kW + 1] = c;
        }
}

/* ---- actions ---------------------------------------------------------------- */
static void Close(void) { g_open = 0; }

static void SetPage(int p)
{
    g_page = (p + kPageCount) % kPageCount;
    g_confirm = -1;
    if (g_page == kPageStatus && !g_have_stat && !g_live)   /* live: drawn values */
        g_have_stat = Capture(1, g_stat, &g_stat_w);
}

static void Equip(int slot)
{
    const uint8_t item = g_snes->ram[0x0AB4 + slot];
    if (!item) return;
    Wr(0x0AC4, (uint16_t)slot);
    Wr(0x0AC6, item);
}

static void Activate(void)
{
    switch (g_page) {
    case kPageItems:
        Equip(g_slot);
        break;
    case kPageSaves:
        if (g_confirm == 0) {
            g_confirm = -1;
            if (snes_savestate_load_slot(g_save_slot)) Close();
        } else {
            g_confirm = 0;
        }
        break;
    case kPageSettings:
        Close();
        snes_settings_menu_open();
        break;
    default:
        break;
    }
}

static void SaveHere(void)
{
    if (g_page != kPageSaves) return;
    if (g_confirm == 1) {
        g_confirm = -1;
        snes_savestate_save_slot(g_save_slot);
    } else {
        g_confirm = 1;
    }
}

static void Move(int dx, int dy)
{
    if (g_page == kPageItems) {
        int col = g_slot % 4 + dx, row = g_slot / 4 + dy;
        col = (col + 4) % 4;
        row = (row + 4) % 4;
        g_slot = row * 4 + col;
    } else if (g_page == kPageSaves && dy) {
        g_save_slot += dy;
        if (g_save_slot < 0) g_save_slot = 0;
        if (g_save_slot >= SNES_SAVESTATE_SLOTS) g_save_slot = SNES_SAVESTATE_SLOTS - 1;
        g_confirm = -1;
    }
}

/* ---- host hooks ------------------------------------------------------------- */
int IogCompanionOpen(void)
{
    if (!g_snes || !Rd(0x09AA)) return 0;   /* not in play */
    /* The pictures belong to this moment: re-take them (the game is paused,
     * so the short wait is not a hitch in play). Status on first visit. */
    IogCompanionCaptureInventory();
    g_have_stat = 0;
    if (g_page == kPageStatus) g_have_stat = Capture(1, g_stat, &g_stat_w);
    const int eq = (int16_t)Rd(0x0AC4);
    g_slot = eq >= 0 && eq < 16 ? eq : 0;
    g_confirm = -1;
    g_prev_nav = ~0u;
    g_open = 1;
    return 1;
}

void IogCompanionNav(uint32_t in, uint32_t ticks)
{
    (void)ticks;
    const uint32_t p = in & ~g_prev_nav;
    g_prev_nav = in;
    if (!g_open && !g_live_seen) return;
    g_live = !g_open;   /* beside the running game: the right stick */
    if (p & SNES_PAD_B) {
        if (g_live) return;
        if (g_confirm >= 0) g_confirm = -1; else Close();
        return;
    }
    if (p & SNES_PAD_L) SetPage(g_page - 1);
    if (p & SNES_PAD_R) SetPage(g_page + 1);
    if (p & SNES_PAD_A) Activate();
    if (p & SNES_PAD_X) SaveHere();
    if (p & SNES_PAD_LEFT) Move(-1, 0);
    if (p & SNES_PAD_RIGHT) Move(1, 0);
    if (p & SNES_PAD_UP) Move(0, -1);
    if (p & SNES_PAD_DOWN) Move(0, 1);
}

void IogCompanionKey(int key, int repeat)
{
    (void)repeat;
    /* SDL keycodes: Escape, I, arrows, Enter, Q/E pages, S save. */
    switch (key) {
    case 27: case 'i': if (g_confirm >= 0) g_confirm = -1; else Close(); break;
    case 'q': SetPage(g_page - 1); break;
    case 'e': SetPage(g_page + 1); break;
    case '\r': case ' ': Activate(); break;
    case 's': SaveHere(); break;
    case 0x40000050: Move(-1, 0); break;   /* SDLK_LEFT  */
    case 0x4000004F: Move(1, 0); break;    /* SDLK_RIGHT */
    case 0x40000052: Move(0, -1); break;   /* SDLK_UP    */
    case 0x40000051: Move(0, 1); break;    /* SDLK_DOWN  */
    default: break;
    }
}

void IogCompanionPointer(int px, int py, int down)
{
    g_live = !g_open;
    const int x = px / 2, y = py / 2;   /* panel -> 1x */
    const int was = g_ptr_down;
    g_ptr_down = down;
    if (!down || was) return;            /* act on the press */
    /* Page list. */
    if (x >= 10 && x < 80 && y >= 156 && y < 156 + 12 * kPageCount) {
        const int p = (y - 156) / 12;
        if (p == g_page && p == kPageSettings) Activate();
        else SetPage(p);
        return;
    }
    if (g_page == kPageItems) {
        for (int i = 0; i < 16; i++) {
            int sx, sy;
            SlotPos(i, &sx, &sy);
            if (x >= sx - 11 && x < sx + 11 && y >= sy - 14 && y < sy + 4) {
                if (g_slot == i) Equip(i);
                g_slot = i;
                return;
            }
        }
    } else if (g_page == kPageSaves) {
        const int first = g_save_slot - 1 < 0 ? 0 : g_save_slot - 1 > SNES_SAVESTATE_SLOTS - 3
                          ? SNES_SAVESTATE_SLOTS - 3 : g_save_slot - 1;
        if (x >= 16 && x < 248 && y >= 14 && y < 14 + 3 * 42) {
            const int s = first + (y - 14) / 42;
            if (s == g_save_slot) Activate(); else { g_save_slot = s; g_confirm = -1; }
            return;
        }
        if (y >= 156 && x >= 90) {          /* hint box: top half load, bottom save */
            if (y < 178) Activate(); else SaveHere();
        }
    } else if (g_page == kPageSettings && y >= 156 && x >= 90) {
        Activate();
    }
}

int IogCompanionImage(const uint32_t **px, int *w, int *h)
{
    if (!g_open) return 0;
    g_live = 0;
    Compose();
    *px = g_panel;
    *w = kW * 2;
    *h = kH * 2;
    return 1;
}

/* The live picture, at 1x. Before the first capture (or outside play)
 * nothing: the host then shows the game alone. */
int IogCompanionLive(const uint32_t **px, int *w, int *h)
{
    g_live_seen = 1;
    if (!g_snes || !Rd(0x09AA) || g_open) return 0;   /* no capture yet: plain boxes */
    g_live = 1;
    Compose();
    g_live = 0;
    *px = g_canvas;
    *w = kW;
    *h = kH;
    return 1;
}

/* After every real frame (the host's after_run_frame hook). */
void IogCompanionAfterFrame(void)
{
    /* Live layout: the first picture of the menu art (later ones come from
     * the player's own pause menu, IogCompanionGrabMenu). */
    if (g_live_seen) {
        extern int snes_frame_counter;
        /* A capture fails where the game will not open its menu (some
         * scenes, cutscenes); then wait longer each time, up to half a
         * minute, starting over when the map changes. */
        static int last_try = -100000, fails;
        static uint16_t last_map = 0xFFFF;
        const uint16_t map = Rd(0x0644);
        if (map != last_map) {
            last_map = map;
            fails = 0;
        }
        const int wait = 60 << (fails < 5 ? fails : 5);
        /* Only the first picture is captured: later ones come for free from
         * the player's own pause menu (IogCompanionGrabMenu). A capture
         * runs ~24 frames of the game at once -- a hitch in play. */
        const int changed = !g_have_inv;
        if (changed && !g_open && snes_frame_counter - last_try >= wait && InPlayQuiet()) {
            last_try = snes_frame_counter;
            const int before = g_have_inv;
            g_have_inv = 0;
            if (IogCompanionCaptureInventory()) {
                fails = 0;
            } else {
                g_have_inv = before;
                fails++;
            }
        }
    }
    /* IOG_COMPANION_TEST=<frame>: open the companion there on page
     * IOG_COMPANION_PAGE and write companion.ppm (headless check). */
    static long test_at = -2;
    if (test_at == -2) {
        const char *v = getenv("IOG_COMPANION_TEST");
        test_at = v ? strtol(v, NULL, 0) : -1;
    }
    extern int snes_frame_counter;
    if (test_at < 0 || snes_frame_counter < test_at || !InPlayQuiet())
        return;
    test_at = -1;
    const char *pg = getenv("IOG_COMPANION_PAGE");
    g_page = pg ? atoi(pg) % kPageCount : 0;
    if (!IogCompanionOpen()) return;
    if (g_page == kPageStatus && !g_have_stat) g_have_stat = Capture(1, g_stat, &g_stat_w);
    Compose();
    FILE *f = fopen("companion.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", kW * 2, kH * 2);
        for (int i = 0; i < kW * 2 * kH * 2; i++) {
            const uint32_t p = g_panel[i];
            fputc(p >> 16 & 255, f); fputc(p >> 8 & 255, f); fputc(p & 255, f);
        }
        fclose(f);
    }
    fprintf(stderr, "[companion] test page %d written (inv=%d stat=%d)\n", g_page, g_have_inv, g_have_stat);
    g_open = 0;
}
