# HD packs (graphics mods)

An HD pack replaces what the game draws, 8x8 character by 8x8 character,
with the modder's own pictures, at the SNES resolution or at 2x/4x. Packs are
keyed by what the player's own game puts in video memory, so a pack carries
only new art: nothing taken from a ROM is distributed, and the template a
modder paints over is dumped by the player's own copy of the game.

Code: `runner/src/hdpack.{c,h}` (packs, lookup, dump) and the HD pass in
`runner/src/snes/ppu.c` (`PpuDrawHdLine`). Host wiring: `LoadHdPacks()` in
`runner/src/desktop/host_main.c`.

## Making a pack

1. Dump a template while playing through the scenes you want to change:

   ```sh
   SNESRECOMP_HDPACK_DUMP=/path/to/mypack ./Game rom.sfc
   ```

   The directory fills with `tiles_NNN.png` sheets (32x32 cells of 8x8) and
   `hdpack.txt`, and is kept current while you play. Every character is
   dumped once, in the palette it was on screen with longest; a line ending
   in `# seen with N palettes` marks one the game also shows in others.

2. Redraw the cells you want. For more detail, resize the sheets by 2 or 4
   first (nearest neighbour) and change `scale 1` to `scale 2` or `scale 4`.
   Keep every cell in its place. Transparent pixels (alpha < 128) keep the
   game's own pixel.

3. Delete the `tile` lines you did not change (optional, it saves memory).

4. Try it: `SNESRECOMP_HDPACK=/path/to/mypack ./Game rom.sfc` (several
   packs: separate the directories with `;`, the first one wins a clash).

## hdpack.txt

```text
# comment
scale 4                      art is 4x the SNES size (1, 2 or 4)
image tiles_000.png          following tiles live in this image
tile 4 <tile-hash> <palette-hash|*> <col> <row>
```

- `tile <bpp> <tile-hash> <palette-hash> <col> <row>`: `col`/`row` count
  cells of the unscaled sheet. The tile hash is FNV-1a 64 over the
  character's VRAM words (low byte first). The palette hash covers only the
  colours the character actually uses, so palette animation elsewhere in the
  same palette (water, lights) does not break the match.
- `*` as the palette hash matches the character in any palette: use it for
  art that should not change with palette swaps or fades, keep the hash for
  per-palette art (an enemy and its recoloured variant can then differ).
- An exact palette match wins over `*`.

## HD-2D: emissive maps and effects

For an HD-2D look (depth of field, glowing light sources, graded colour) a
pack can add, still in `hdpack.txt`:

```text
image tiles_000.png
emissive glow_000.png        light map for the image above: same size and
                             layout; opaque pixels glow (their colour)
fx tiltshift 0.35            depth of field towards the top/bottom (0..1)
fx focus_center 0.55         sharp band centre, fraction of screen height
fx focus_width 0.18          sharp band half-height
fx bloom 0.45                glow strength (0..2): bright areas + emissive
fx bloom_threshold 0.75      luminance where bright areas start to glow
fx vignette 0.3              darkened corners (0..1)
fx contrast 1.08             1 = unchanged
fx saturation 1.10           1 = unchanged
```

`fx` lines are the pack's recommended look; a game's own options can
override them through `HdFx_Set`. The effects run on
the 4x canvas (`hdfx.c`, presentation only: they never change emulated
state), about 8 ms per frame with everything on (2-3 ms without blur).
Emissive light is only gathered where replaced art is drawn at 4x.

The effects run on the CPU, so they are costly at 4x: measured on Illusion
of Gaia at 1080p, 125 fps without `fx` lines and 58 with the eight above.

Scaling to the screen is separate (`present_filter.h`): a game or host can
pick crisp pixels (`kPresentFilter_Sharp`, any presenter) or an xBR edge
upscaler for 1x frames (`kPresentFilter_Smooth`, OpenGL presenter).

## Shipping it as a mod

A pack is an ordinary data-only mod package: a `manifest.toml` (see
`MOD_PACKAGES.md`) with one feature and no plugin, plus the pack in a
`hdpack/` directory:

```text
manifest.toml
hdpack/hdpack.txt
hdpack/tiles_000.png
...
```

```toml
format_version = 1
id = "author.hd-graphics"
version = "1.0.0"
name = "HD graphics"
author = "..."
description = "Redrawn tiles and sprites at 4x."
license = "..."
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "<the game's id>"
rom_sha256 = "<the game's ROM SHA-256>"

[[feature]]
id = "graphics"
name = "HD graphics"
description = "Replacement art."
group = "Graphics"
default_enabled = false
```

Zip it as a `.snesmod` and install it from the launcher's Mods page. Every
enabled package with a `hdpack/hdpack.txt` is loaded at start, after
`SNESRECOMP_HDPACK`.

## Rendering

- The pass runs on the new renderer, after each scanline is composed. Every
  visible pixel that is plain BG (Mode 1 BG1-BG3, 8x8 or 16x16 tiles) or OBJ
  is traced back to its character; BG characters are re-derived with the same
  scroll/window/widescreen maths the draw used and checked against the colour
  index actually drawn, so a pixel the pass cannot identify is left as is.
- With `[Graphics] EnhancedMode7 = 1` (the default) and a native-widescreen
  host, replaced lines are also drawn at 4x4 and the frame is presented at 4x;
  otherwise the 1x picture uses an 8x8 average of the art.
- Master brightness (fades) is applied to the art.

Limits of this version:

- Not replaced: Mode 7 planes, pixels changed by colour math (translucency,
  colour windows), Mode 0/2/3 backgrounds, mosaic and offset-per-tile lines.
- Art replaces the character's opaque pixels only: it cannot draw where the
  original character is transparent, so silhouettes must stay inside the
  original shape (detail can be added within it).
- 8bpp characters are not dumped by the BG path (Mode 3); OBJ is always 4bpp.
