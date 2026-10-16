#ifndef IOG_TEXT_H
#define IOG_TEXT_H

/*
 * Illusion of Gaia text pipeline, run on the player's own ROMs.
 *
 * Nothing extracted from a ROM ships with the game. i18n/structure.txt only
 * says where the dialogue strings live in the USA ROM (offsets, [::] marker
 * positions, [ADR] tables). At runtime the USA ROM (the one being played)
 * and, for a translation, an official European ROM supplied by the player
 * are read here:
 *
 *   - every USA call site of the text engine (COP BF / COP 6B <string>) is
 *     paired with its European counterpart by dynamic programming over the
 *     surrounding script bytes and the string's control-code skeleton;
 *   - strings reached only through other strings ([JMP], [PRT], [ADR]
 *     tables) are paired by following both references in step;
 *   - the European text (glyph codes and commands, dictionary expanded) is
 *     re-encoded uncompressed into 32 KiB "virtual" banks for the HiROM
 *     overlay at $A0-$AF:8000, with its references rewritten to the USA
 *     layout; strings with no counterpart are re-encoded from the USA ROM;
 *   - the release's own font (compressed, QuintetLZ) is copied to a virtual
 *     bank and scene_meta's font pointer is redirected to it.
 */

#include <stddef.h>
#include <stdint.h>

/* Overlay banks $A0-$AE carry text; $AF (index 15) belongs to iog_msu.c. */
enum { kIogVirtualBanks = 15, kIogVirtualBankSize = 0x8000, kIogFontSize = 0x2000 };

typedef struct IogPatch {
    uint32_t offset;           /* USA ROM offset */
    uint32_t len;
    uint8_t *data;             /* bytes to write ([PRT:@virtual][RET], a pointer, text) */
    uint8_t *original;         /* filled in when applied */
} IogPatch;

typedef struct IogPack {
    char code[8];
    char name[32];
    uint8_t *banks[kIogVirtualBanks];
    int nbanks;
    IogPatch *patches;
    int npatches, cap_patches;
    /* The release's own BG3 font (menu font $000-$0FF, dialogue font
     * $100-$1FF: accented letters, drawn words, HUD labels) and the USA one,
     * decompressed, for swapping what is already in VRAM on a live switch. */
    uint8_t font[0x2000];
    uint8_t usa_font[0x2000];
    int has_font;
    int strings_translated, strings_total;     /* dialogue */
    int menu_translated, menu_total;           /* menus, items, HUD */
    int sprite_translated;                     /* map names, prologue */
} IogPack;

/* Language of an Illusion of Time ROM from its header, e.g. "es";
 * NULL if the image is not one. */
const char *IogTextRomLanguage(const uint8_t *rom, size_t size);

/* Build a language pack. usa: pristine USA ROM; eu: the European ROM of
 * that language. structure_path: i18n/structure.txt. Returns NULL (and
 * prints why) on failure. */
IogPack *IogTextBuildPack(const uint8_t *usa, size_t usa_size,
                          const uint8_t *eu, size_t eu_size,
                          const char *structure_path);
void IogTextFreePack(IogPack *pack);


#endif
