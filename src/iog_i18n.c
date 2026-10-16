/*
 * Illusion of Gaia runtime localization.
 *
 * Languages come from the player's own ROMs: put an official European
 * release (Illusion of Time, Spanish/French/German) in <exe>/languages/ and
 * it becomes selectable. Nothing extracted from any ROM ships with the game;
 * src/iog_text.c pairs the European text with the USA script at runtime.
 *
 * Applying a language:
 *   - its virtual banks are installed as HiROM overlay banks $A0-$AF:8000;
 *   - each relocated string's first 5 bytes in the in-memory USA ROM become
 *     [PRT:@virtual][RET]: the game's own text engine prints the
 *     translation as a nested string and returns. No game code changes;
 *   - the release's own font (accented letters, drawn menu words, HUD
 *     labels) replaces the USA one: scene_meta's font pointer is redirected
 *     to it, and the copy already in VRAM is swapped on a live switch.
 *
 * The ROM file on disk is never modified; switching restores the original
 * bytes first, so it can happen at runtime (deferred while a message is on
 * screen).
 */

#include "iog_i18n.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include "host_paths.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "iog_text.h"
#include "mod_runtime.h"
#include "iog_randomizer.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/snes.h"

enum {
    kMaxLanguages = 8,
};

typedef struct {
    char code[8];
    char path[1024];
    IogPack *pack;               /* built on first use */
} Language;

static Language g_langs[kMaxLanguages];
static int g_nlangs;
static int g_scanned;

static IogPack *g_active;        /* applied pack, NULL: original text */
static uint8_t *g_usa;           /* pristine copy of the USA ROM */
static size_t g_usa_size;

static char g_pending[8];
static int g_has_pending;

/* ---- paths -------------------------------------------------------------- */

/* Directory holding the executable, with a trailing separator; "" if it
 * cannot be determined (paths then resolve against the working directory). */
static const char *ExeDir(void)
{
    /* The framework's executable directory (with trailing separator): it
     * also knows the platforms with no /proc/self/exe (Switch). */
    static char dir[1024];
    static int done;
    if (!done) {
        done = 1;
        if (!snesrecomp_exe_dir_path("", dir, sizeof(dir)))
            dir[0] = 0;
    }
    return dir;
}

static uint8_t *IogReadFile(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 && n <= 8 << 20 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf && size)
        *size = (size_t)n;
    return buf;
}

static int HasRomExtension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc"));
}

static void ConsiderInto(const char *path, Language *tab, int *n)
{
    size_t size = 0;
    uint8_t *rom = IogReadFile(path, &size);
    if (!rom)
        return;
    const uint8_t *img = rom;
    if (size % 0x8000 == 0x200) {   /* copier header */
        img += 0x200;
        size -= 0x200;
    }
    const char *code = IogTextRomLanguage(img, size);
    if (code && *n < kMaxLanguages) {
        int dup = 0;
        for (int i = 0; i < *n; i++)
            dup |= !strcmp(tab[i].code, code);
        if (!dup) {
            Language *l = &tab[(*n)++];
            snprintf(l->code, sizeof(l->code), "%s", code);
            snprintf(l->path, sizeof(l->path), "%s", path);
        }
    }
    free(rom);
}

static void Consider(const char *path) { ConsiderInto(path, g_langs, &g_nlangs); }

/* ROMs in <dir> and one level of subdirectories. */
static void ScanDirInto(const char *dir, int depth, Language *tab, int *n)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (HasRomExtension(e->d_name))
            ConsiderInto(path, tab, n);
        else if (depth > 0)
            ScanDirInto(path, depth - 1, tab, n);
    }
    closedir(d);
}

static void ScanDir(const char *dir, int depth) { ScanDirInto(dir, depth, g_langs, &g_nlangs); }

/* ---- Mods page: ROMs found in languages/ --------------------------------
 * The launcher shows the "Illusion of Time (Spain) ROM" resource as chosen
 * when one sits in languages/, which is where the game would take it from
 * anyway. Its own table: the game's scan runs later and must still see a
 * ROM the player picks on the Mods page meanwhile. Rescanned at most every
 * few seconds (the launcher asks every frame). */
static const char *FoundInLanguagesDir(const char *code)
{
    static Language found[kMaxLanguages];
    static int nfound;
    static time_t last;
    static int scanned;
    const time_t now = time(NULL);
    if (!scanned || now - last >= 3) {
        scanned = 1;
        last = now;
        nfound = 0;
        char dir[1024];
        snprintf(dir, sizeof(dir), "%slanguages", ExeDir());
        ScanDirInto(dir, 1, found, &nfound);
    }
    for (int i = 0; i < nfound; i++)
        if (!strcmp(found[i].code, code))
            return found[i].path;
    return NULL;
}

static int ResolveLang(const char *code, char *out, uint32_t cap)
{
    const char *p = FoundInLanguagesDir(code);
    if (!p || strlen(p) + 1 > cap)
        return 0;
    snprintf(out, cap, "%s", p);
    return 1;
}
static int ResolveEs(char *out, uint32_t cap) { return ResolveLang("es", out, cap); }
static int ResolveFr(char *out, uint32_t cap) { return ResolveLang("fr", out, cap); }
static int ResolveDe(char *out, uint32_t cap) { return ResolveLang("de", out, cap); }

static void ScanLanguages(void)
{
    if (g_scanned)
        return;
    g_scanned = 1;
    /* ROMs chosen on the Mods page ("Languages" package) come first. */
    static const char *const kResources[] = {"rom_es", "rom_fr", "rom_de"};
    for (size_t i = 0; i < sizeof(kResources) / sizeof(kResources[0]); i++) {
        char chosen[1024];
        if (snes_mod_runtime_feature_resource_path_c("iog.languages", "language", kResources[i],
                                                     chosen, sizeof(chosen)))
            Consider(chosen);
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%slanguages", ExeDir());
    ScanDir(dir, 1);
    const char *extra = getenv("IOG_LANG_ROMS");
    if (extra && *extra)
        ScanDir(extra, 1);
    for (int i = 0; i < g_nlangs; i++)
        fprintf(stderr, "[i18n] language available: %s (%s)\n", g_langs[i].code, g_langs[i].path);
}

/* ---- applying ------------------------------------------------------------ */

static void SwapFont(const uint8_t *from, const uint8_t *to);

static void Unapply(void)
{
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!g_active || !cart)
        return;
    for (int i = g_active->npatches - 1; i >= 0; i--) {
        IogPatch *p = &g_active->patches[i];
        memcpy(cart->rom + p->offset, p->original, p->len);
    }
    if (g_active->has_font)
        SwapFont(g_active->font, g_active->usa_font);
    for (int i = 0; i < kIogVirtualBanks; i++)
        cart_set_overlay_bank(cart, i, NULL);
    g_active = NULL;
}

static void Apply(IogPack *p)
{
    Cart *cart = g_snes->cart;
    for (int i = 0; i < p->nbanks; i++)
        cart_set_overlay_bank(cart, i, p->banks[i]);
    for (int i = 0; i < p->npatches; i++) {
        IogPatch *pt = &p->patches[i];
        if (pt->offset + pt->len > cart->romSize)
            continue;
        memcpy(pt->original, cart->rom + pt->offset, pt->len);
        memcpy(cart->rom + pt->offset, pt->data, pt->len);
    }
    if (p->has_font)
        SwapFont(p->usa_font, p->font);
    g_active = p;
    fprintf(stderr, "[i18n] language %s (%s): dialogue %d/%d, menus %d/%d, sprite text %d, %d banks, %d patches%s\n",
            p->code, p->name, p->strings_translated, p->strings_total, p->menu_translated,
            p->menu_total, p->sprite_translated, p->nbanks, p->npatches, p->has_font ? ", own font" : "");
}

static IogPack *PackFor(const char *code)
{
    for (int i = 0; i < g_nlangs; i++) {
        Language *l = &g_langs[i];
        if (strcmp(l->code, code))
            continue;
        if (!l->pack) {
            size_t size = 0;
            uint8_t *eu = IogReadFile(l->path, &size);
            if (!eu)
                return NULL;
            const uint8_t *img = eu;
            if (size % 0x8000 == 0x200)
                img += 0x200, size -= 0x200;
            char structure[1024];
            snprintf(structure, sizeof(structure), "%si18n/structure.txt", ExeDir());
            l->pack = IogTextBuildPack(g_usa, g_usa_size, img, size, structure);
            free(eu);
        }
        return l->pack;
    }
    return NULL;
}

/* The dialogue engine flags a message in progress in $09EC bit 13 (COP BF
 * sets it for the whole message). A language switch must not pull the
 * virtual banks out from under a string being printed. */
static int TextActive(void)
{
    return g_snes && (g_snes->ram[0x09ED] & 0x20);
}

static void SwitchNow(const char *code)
{
    g_has_pending = 0;
    Cart *cart = g_snes ? g_snes->cart : NULL;
    if (!cart)
        return;
    if (!g_usa) {
        g_usa_size = cart->romSize;
        g_usa = malloc(g_usa_size);
        memcpy(g_usa, cart->rom, g_usa_size);
    }
    Unapply();
    if (!strcmp(code, "original"))
        return;
    ScanLanguages();
    IogPack *p = PackFor(code);
    if (!p) {
        fprintf(stderr, "[i18n] no %s ROM found in %slanguages/ (or IOG_LANG_ROMS)\n", code, ExeDir());
        return;
    }
    Apply(p);
}

int IogI18nSetLanguage(const char *code)
{
    if (IogRandomizerActive())
        return 0;   /* the seed's text is not the stock layout */
    snprintf(g_pending, sizeof(g_pending), "%s", code && *code ? code : "original");
    g_has_pending = 1;
    return 1;
}

/* Whether a ROM for `code` was found ("original" always is). */
int IogI18nAvailable(const char *code)
{
    if (!code || !strcmp(code, "original"))
        return 1;
    ScanLanguages();
    for (int i = 0; i < g_nlangs; i++)
        if (!strcmp(g_langs[i].code, code))
            return 1;
    return 0;
}

const char *IogI18nLanguage(void)
{
    return g_active ? g_active->code : "original";
}

static void Persist(const char *code)
{
    char path[1024];
    snprintf(path, sizeof(path), "%slanguage.txt", ExeDir());
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", code);
        fclose(f);
    }
}

const char *IogI18nCycle(void)
{
    ScanLanguages();
    const char *cur = g_has_pending ? g_pending : IogI18nLanguage();
    int i = -1;                      /* -1: original */
    for (int k = 0; k < g_nlangs; k++)
        if (!strcmp(g_langs[k].code, cur))
            i = k;
    i = i + 1 >= g_nlangs ? -1 : i + 1;
    const char *code = i < 0 ? "original" : g_langs[i].code;
    IogI18nSetLanguage(code);
    Persist(code);
    if (i < 0)
        return g_nlangs ? "Original (English)" : "Original (no language ROMs in languages/)";
    static const struct { const char *code, *name; } kNames[] = {
        {"es", "Español"}, {"fr", "Français"}, {"de", "Deutsch"}, {"it", "Italiano"},
    };
    for (size_t k = 0; k < sizeof(kNames) / sizeof(kNames[0]); k++)
        if (!strcmp(kNames[k].code, code))
            return kNames[k].name;
    return code;
}

/* ---- font ---------------------------------------------------------------- */

/* The font lives at the BG3 character base once loaded; a scene load after a
 * switch picks up the redirected pointer by itself, but what is already in
 * VRAM is swapped here, only if it really is the font being replaced (two
 * signature tiles: the menu 'A' and the dialogue 'A'). */
static void SwapFontIn(Ppu *ppu, const uint8_t *from, const uint8_t *to)
{
    if (!ppu)
        return;
    const int base = (ppu->bgTileAdr >> 8 & 0xf) << 12;
    if (base + kIogFontSize / 2 > 0x8000)
        return;
    uint8_t *vram = (uint8_t *)&ppu->vram[base];
    static const int kSig[] = {0x41, 0x140};
    for (size_t i = 0; i < sizeof(kSig) / sizeof(kSig[0]); i++)
        if (memcmp(vram + kSig[i] * 16, from + kSig[i] * 16, 16))
            return;
    memcpy(vram, to, kIogFontSize);
}

static void SwapFont(const uint8_t *from, const uint8_t *to)
{
    SwapFontIn(g_ppu, from, to);
    if (g_snes && g_snes->ppu != g_ppu)
        SwapFontIn(g_snes->ppu, from, to);
}

/* ---- per frame -------------------------------------------------------------- */

void IogI18nFrame(void)
{
    /* Test hook: IOG_LANG_TEST=<frame>:<code> requests a switch mid-game. */
    static int test_frame = -1;
    static char test_code[9];
    if (test_frame == -1) {
        const char *t = getenv("IOG_LANG_TEST");
        test_frame = (t && sscanf(t, "%d:%8s", &test_frame, test_code) == 2) ? test_frame : -2;
    }
    extern int snes_frame_counter;
    if (test_frame >= 0 && snes_frame_counter >= test_frame) {
        test_frame = -2;
        fprintf(stderr, "[i18n] test switch to %s at frame %d (text %s)\n", test_code,
                snes_frame_counter, TextActive() ? "active: deferred" : "idle");
        IogI18nSetLanguage(test_code);
    }
    if (g_has_pending && !TextActive())
        SwitchNow(g_pending);
}

void IogI18nInit(void)
{
    const char *env = getenv("IOG_LANG");
    char code[16] = "";
    if (env && *env) {
        snprintf(code, sizeof(code), "%s", env);
    } else if (snes_mod_runtime_feature_enabled_c("iog.languages", "language") > 0) {
        /* The Mods page is authoritative when its feature is on: the plugin
         * already chose; a language.txt left by Ctrl+L must not override. */
        return;
    } else {
        char path[1024];
        snprintf(path, sizeof(path), "%slanguage.txt", ExeDir());
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%15s", code) != 1)
                code[0] = 0;
            fclose(f);
        }
    }
    if (code[0])
        IogI18nSetLanguage(code);
}

/* Mods page: "Languages" package, feature "language", option "code". Used
 * when neither IOG_LANG nor language.txt chose a language. */
static void ActivateFromMods(void)
{
    char code[16];
    if (snes_mod_runtime_feature_option_value_c("iog.languages", "language", "code", code,
                                                sizeof(code)) > 0 && code[0] && !g_has_pending &&
        !g_active) {
        const char *env = getenv("IOG_LANG");
        if (!(env && *env))
            IogI18nSetLanguage(code);
    }
}

SNES_MOD_CONSTRUCTOR(register_iog_i18n)
{
    snes_mod_register_activation_plugin("iog.i18n", ActivateFromMods);
    snes_mod_register_resource_resolver("iog.languages", "language", "rom_es", ResolveEs);
    snes_mod_register_resource_resolver("iog.languages", "language", "rom_fr", ResolveFr);
    snes_mod_register_resource_resolver("iog.languages", "language", "rom_de", ResolveDe);
}
