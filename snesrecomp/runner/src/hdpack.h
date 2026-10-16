#ifndef SNESRECOMP_HDPACK_H
#define SNESRECOMP_HDPACK_H

/*
 * HD packs: replacement art for SNES characters (8x8 tiles), keyed by what
 * the player's own game puts in VRAM, so a pack carries only the modder's
 * pictures and never data taken from a ROM.
 *
 * A pack is a directory holding hdpack.txt and PNG images:
 *
 *   # comment
 *   scale 4                      art is 4x the SNES size (1, 2 or 4)
 *   image tiles_000.png          following tiles live in this image
 *   tile 4 <tile-hash> <palette-hash|*> <col> <row>
 *
 * <col> <row> count 8x8 cells of the unscaled sheet, so a sheet dumped at 1x
 * can be redrawn at 4x by resizing it and changing `scale`. Hashes are
 * 64-bit FNV-1a (16 hex digits) over the character's VRAM words (low byte
 * first; 8, 16 or 32 words for 2, 4 or 8 bpp) and over the palette colours
 * the character uses (index word, then CGRAM word, colour 0 excluded); `*` matches any palette. Pixels with alpha < 128
 * keep the original pixel. The first pack loaded wins a clash.
 *
 * Dump mode writes such a directory at scale 1 from what the game draws
 * (every character/palette pair not already replaced), as the template a
 * modder paints over. See docs/HD_PACKS.md.
 */

#include <stdbool.h>

struct Ppu;

/* Load <dir>/hdpack.txt. Returns the number of tiles added, -1 on error. */
int HdPack_AddDir(const char *dir);
/* Write a template pack into dir while playing (NULL: off). */
void HdPack_SetDumpDir(const char *dir);
/* Bind the lookup to the PPU when a pack is loaded or dump mode is on. */
void HdPack_Install(struct Ppu *ppu);
/* Write pending dump sheets and hdpack.txt now. Also runs at exit. */
void HdPack_Flush(void);
int HdPack_TileCount(void);

#endif
