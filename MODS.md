# Mods

Every mod here is off by default and is switched on from the launcher's
**Mods** page. Nothing taken from a ROM ships with this project: languages,
music and graphics all come from files the player supplies, and the ROM you
play stays the stock USA ROM (the launcher verifies it).

| Package | Feature | What it does |
|---|---|---|
| Languages (`iog.languages`) | Game language | Spanish, French or German text and font from your own *Illusion of Time* ROM |
| Gameplay enhancements (`iog.enhancements`) | Run button | Hold **Y** (or X/L/R) with a direction to sprint |
| | Item hotswapping | Hold **X** and press **L/R** to equip the previous/next item |
| | MSU-1 soundtrack | CD-quality music from an MSU-1 pack |
| | Item stacking | Red Jewels, Herbs and Crystals share one slot, with counters on the inventory screen |
| | Equipped indicator | Frames the equipped item on the inventory screen |
| | New Game Plus | Replay a finished save keeping Will's stats and abilities |
| | Hold to advance dialogue | Keep A/B/Y held to page through dialogue; questions still wait for a press |
| Cheats (`iog.cheats`) | Infinite HP, DP, Stats | Classic cheats with adjustable minimums |
| Apocalypse Gaia (`iog.apocalypse`) | Apocalypse Gaia boss | The restored, unfinished final boss (bring your own patch) |
| Randomizer (`iog.randomizer`) | Randomizer seed | Play an Illusion of Gaia Randomizer (IoGR) seed |
| Graphics effects (`iog.graphics`) | HD scaling | Crisp pixels at any resolution, or smooth redrawn edges (xBR), up to 4K |
| *(any data-only package)* | HD graphics | Replacement art, see `snesrecomp/docs/HD_PACKS.md` |

## MSU-1 soundtrack

What you need: an Illusion of Gaia MSU-1 pack, i.e. a folder with the
tracks as `<name>-<N>.pcm` (for example Conn's pack from the Zeldix forum:
`iog_msu1-1.pcm` ... `iog_msu1-31.pcm`).

1. **Do not patch your ROM.** Packs usually come with an `.ips`/`.asm`
   patch (`iog_msu.ips`) for emulators; this port does that job itself and
   needs the stock ROM. Just keep the `.pcm` files together in one folder.
2. Open the launcher, go to **Mods**, enable **Gameplay enhancements ->
   MSU-1 soundtrack** and pick that folder as **MSU-1 pack folder**.
3. Play. Songs the pack has play from it; sound effects stay the game's
   own; any song the pack lacks plays as usual.

For testing without the launcher: `SNESRECOMP_MSU1=/path/to/pack/folder`
(the track name prefix is detected automatically). `IOG_MSU_LOG=1` prints
every song change and the track chosen.

Track numbering follows the existing packs (Conn's pack and GaiaLabs use
the same layout): 1 Title, 2 World, 3 Town (gulls), 4 Dark Space, 5 Dead
Gold Ship, 6 Town (no gulls), 7 Village, 8 Angkor Wat, 9 Castle,
10 Dungeon, 11 Great Wall, 12 Larai Cliffs, 13 Meeting Mother,
14 Shipwreck, 15 Sky Garden, 16 Mu, 17 Pyramid, 18 Boss, 19 Returned
Memory, 20 Final Battle, 21/22 Ending, 23 Lola's Song, 24 Item fanfare,
25 Wind Melody, 26 Memory Melody. Tracks 23-26 play once; the rest loop.

How it works (`src/iog_msu.c`): each song with a track in the pack is
replaced, in memory, by a copy of the game's own silent song, so the sound
chip keeps running the game's driver (sound effects) while the MSU-1 plays
the track; the song change itself is recognised from the game's music loader.

Known limitation: a few cutscenes wait for the music to finish; with a
one-shot MSU-1 track they may continue earlier than in the original.

Not available on the original 3DS / 2DS (a New 3DS has the CPU to spare for
streaming the soundtrack); the option is hidden there.

## Languages

Enable **Languages -> Game language**, choose Español / Français / Deutsch
and select your own ROM of that release (*Illusion of Time*, Spain /
France / Germany) under the feature, or drop it in `languages/` beside the
executable. **Ctrl+L** switches language in game. Untranslated lines fall
back to English.

A few lines have no counterpart in the European releases, such as "Where do
you go?" on the world map and the stone girls. These are translated by hand
(`src/iog_hand_text.inc`). The French and German hand translations are
written without accents.

## Run button, hotswapping, cheats

- **Run**: hold the run button (default Y) and a direction; Will uses the
  game's own dash whenever he can move.
- **Hotswapping**: hold X and press L or R to cycle the equipped item
  through the inventory (past either end nothing is equipped). The game's
  item sound confirms it.
- **Cheats**: *Infinite HP* refills HP every frame; *DP* keeps at least the
  chosen amount; *Stats* keeps strength, defense and max HP at least at the
  chosen values (0 leaves one alone). The HUD shows new values when the game
  next redraws it.

These change the game's memory while you play, so they are part of the
netplay mod set: both players need the same selection.

## Apocalypse Gaia

The Apocalypse Gaia patch restores the unfinished final boss, fought after
Dark Gaia. It is the work of a member of the Illusion of Gaia Randomizer
community and is **not distributed with this project**: download
`IOG_ApocalypseGaia_12.ips` (v1.2, for the US ROM without header) from the
IoGR community (https://www.iogr.app, Discord linked there).

Tick **Apocalypse Gaia -> Apocalypse Gaia boss** on the Mods page and choose
the `.ips` as **Apocalypse Gaia patch**; untick it to play the original. The
patch is applied in memory only (your ROM file stays stock) and save files
are compatible both ways.

While it is on, the game runs entirely through the interpreter
(`src/iog_ips.c`: compiled code was generated from the stock ROM and would
ignore the patched instructions). Measured on the test route it runs at the
same speed.

## Randomizer

The randomizer mode plays seeds of the **Illusion of Gaia Randomizer**
(IoGR, by DontBaguMe and contributors, https://www.iogr.app): items,
abilities and more shuffled with tested logic, an open world, in-game
trackers and its own goals (see the IoGR README).

1. Make a seed from your own US ROM, either on https://www.iogr.app
   (download the randomized ROM), or locally:
   `python3 tools/randomizer/generate.py <your-us-rom.sfc> --seed 1234`
   (fetches IoGR on first use; needs `pip install bsdiff4 bitstring ips.py`;
   options: `--difficulty`, `--goal`, `--statues`).
2. On the Mods page enable **Randomizer -> Randomizer seed** and choose the
   generated `.sfc`.
3. Play. Disable it to go back to the original game.

The game runs interpreted while a seed is loaded (the compiled code belongs
to the stock ROM). Languages, MSU-1, item stacking, New Game Plus and the
Apocalypse Gaia package stand down (IoGR has its own options for the last
two). **Save files:** a seed uses the same save slots as the original game;
use a separate slot (or back up `saves/save.srm`) and don't continue a
randomizer save with the mode off.

## New Game Plus

With **New Game Plus** on:

1. When the **END** screen has been up a few seconds, press any button, or
   wait a minute. The game asks whether to **save the file as finished**, then
   goes back to the title by itself.
2. Continuing a finished file asks **"Start New Game+?"**:
   - **Yes** starts the journey again in the classroom. Will keeps his max HP,
     strength, defense, abilities and Red Jewel tally; the story, dungeons,
     items and pyramid hieroglyphs are reset.
   - **No** continues the file as it was saved.
3. New Game+ is **harder**: the damage Will takes is doubled in the first round
   and tripled from the second round on.

(The state is the same as the GaiaLabs NewGamePlus patch; the game's own
menus are left stock.)

**Completion** shows in the companion screen (Status) and the in-game
settings. It is worth 100 points:

| Part | Points |
|---|---|
| The 87 rooms whose enemies give a HP/STR/DEF reward | 40 |
| The 50 Red Jewels (given to the Gem Man or held) | 40 |
| The end of the story | 20 |

## Inventory

- **Item stacking**: Red Jewels, Herbs and Crystals keep one inventory slot;
  the inventory screen shows how many you carry. Using one brings out the
  next (still equipped). The extra units are stored in the save file.
- **Equipped indicator**: a gold frame marks the equipped item on the
  inventory screen.

## HD graphics

**HD scaling** (Graphics effects package, on by default) keeps the picture
sharp at any window or screen size (720p, 1080p, 4K). It is presentation
only: it never changes how the game plays. Two modes:

- **Sharp pixels**: every SNES pixel is drawn as a solid square block; only
  the single screen pixel on a block's edge is blended, so non-integer sizes
  (e.g. 224 lines on a 1080-line screen) give neither blur nor uneven pixels.
  Works with every renderer (the SDL one needs SDL 3.4 or newer).
- **Smooth edges (xBR)**: the diagonal and curved edges of the art are
  redrawn at the screen's resolution, flat areas stay crisp. Needs
  `[Graphics] OutputMethod = OpenGL` (the default on Switch and Android);
  HD packs and Mode 7 at 4x are already high resolution and are shown sharp.

Off, the picture is scaled as on the original (`LinearFiltering`). Cost: none
measurable (about 285 fps at 1080p on an RTX 4070 SUPER either way).

HD packs replace the art itself. A pack's `fx` lines (tilt-shift, bloom,
vignette, grading) still run on the CPU on the 4x canvas: they blur the
picture and halve the frame rate (58 instead of 125 fps at 1080p measured
with the test pack), so a sharp HD pack should not use them.

See `snesrecomp/docs/HD_PACKS.md`: dump a template from your own game with
`SNESRECOMP_HDPACK_DUMP=<dir>`, redraw it at 1x/2x/4x, and ship it as a
data-only `.snesmod`.

## In-game settings

**Escape** on the keyboard, **ZL + R3** on a pad (rebindable in the
launcher's Hotkeys) opens the settings over the paused game:

- Display: fullscreen (desktop), FPS counter
- Audio: volume
- RetroAchievements: on/off, account and game status
- Illusion of Gaia: HD scaling, language (only those whose ROM you have),
  hold to advance text, MSU-1 soundtrack (next launch)
- System: save states, reset (asks twice), quit (asks twice)

Changes apply at once unless the menu says "applies on the next launch",
and are saved: config.ini for the host's, the mod selection for the game's,
so the launcher shows the same choices.

## Companion screen

Press **I** (keyboard) or **ZL+L3** (pad) during play. The game pauses and
shows a companion screen in the game's own art: the pause menu is opened
off-screen for a moment and its pictures, boxes and font are reused, so
nothing is shipped — everything comes from your ROM.

| Page | What it does |
|---|---|
| Items | Your inventory on the stone pedestal. Choose a slot and press A (or tap it twice) to equip it. |
| Status | The game's own Cond. page. |
| Saves | The save-state slots with their thumbnails. A twice loads, X twice saves. |
| Config | Opens the in-game settings. |

L and R change page, B closes. Everything works by touch as well.

**Dual and Flip Grip.** In the settings, *Companion screen* puts the companion
beside the running game instead of in a pause:

- **Dual** puts the game and the companion side by side.
- **Flip Grip** and **Flip Grip R** stack them and turn the picture a quarter
  turn, for a console held upright.

In these modes you use the companion by touch while you play. Status shows
live numbers. The menu art is captured again whenever your inventory
changes, which causes a short pause right after you pick something up. The
setting is saved as `[General] CompanionLayout` (`normal`, `dual`, `flip`,
`flip_r`).

## Hold to advance dialogue

With **Hold to advance dialogue** on, keeping the confirm button (A, B or Y)
held turns dialogue pages one after another (Slow, Normal or Fast). Questions
-- yes/no and every other choice menu -- always wait for a real press: the
game marks a choice menu in RAM ($065C) and nothing is pressed for you then.
It works on the controller input only (the button is let go for one frame at
a steady rhythm), so it behaves the same with run-ahead and netplay.

## RetroAchievements

Settings -> **RetroAchievements**: tick the box and log in with your
RetroAchievements account. Illusion of Gaia (US) is game 945 there; the game
is recognised from your own ROM file (mods that change the game in memory do
not change what it is recognised as).

- **Softcore only.** Save states, rewind and run-ahead stay available, so
  hardcore is not offered (it would be a false claim to the service).
- Only the session token is stored (`retroachievements.token` beside the
  game), never your password. Log out deletes it.
- No network or a server problem never stops the game; unlocks made offline
  are sent when the connection comes back.
- Achievements are not loaded for a session with **cheats** or a
  **randomizer seed**, and stop when **New Game+** is applied: those games
  are not the one the achievements describe.
- Unlocks appear as a message in the corner of the screen.

Network: libcurl on Linux and macOS, WinHTTP on Windows, devkitPro's
`switch-curl` on Switch (install it before building:
`sudo dkp-pacman -S switch-curl`). Android, 3DS and Vita: not yet.

## Logs and crash reports (all platforms)

If something goes wrong, send the `logs/` folder that sits beside the game
(`sdmc:/switch/babel-recomp/logs/` on Switch).

| File | Contents |
|---|---|
| `babel-recomp.log` | Debug log of this session, starting with the build and platform. On Switch it sits beside the `.nro`. |
| `previous-session.log` | The session before this one. |
| `crash-latest.log` | The log as it was at the moment of the last crash, with the CPU trace. |
| `crash-YYYYmmdd_HHMMSS.log` | One copy of the log per crash. |
| `crash-latest-report.json` | Machine-readable crash report. |
| `crash_report_*.json` | Machine-readable crash reports. |
| `crash_minidump_*.dmp` | Windows minidump of the crash. |
| `unclean-previous.log` | Left when the last session neither exited nor crashed cleanly, for example after a hang. |

If the folder beside the game is read-only, the logs go to the per-user data
folder instead, and the second line of the log gives the path.
`SNESRECOMP_LOG_FILE=0` keeps the output on the console only.

## Switch: safe boot and crash reports

- Hold **ZL + R3** while the game starts: the shader is cleared and the
  default video output restored (for a shader that leaves the screen black).
- Everything the game reports goes to `babel-recomp.log` beside the `.nro`;
  the previous session's is kept as `logs/previous-session.log`.
- A crash leaves `logs/crash-latest.log` (registers, the last events, and
  how to resolve the addresses against `babel-recomp.elf`); a session that
  hung or was closed from HOME is noted in `logs/unclean-previous.log`.

## Folder layout (all platforms, and the only one on consoles)

Beside the executable (`babel-recomp`, `babel-recomp.exe`, or the folder of
`babel-recomp.nro` on Switch):

- `rom.sfc`: your USA ROM, used without asking.
- `languages/`: your European ROMs (any file name; recognised by content).
- `msu1/`: an MSU-1 pack, used when MSU-1 is on and no folder was chosen.
- `mods/<any name>/`: an unpacked mod (a folder with `manifest.toml`, e.g.
  `mods/modhd/`). It appears on the Mods page like an installed one;
  delete the folder to remove it.
- `shaders/`: RetroArch GLSL shaders (`.glslp` presets and `.glsl`), e.g.
  the contents of RetroArch's `shaders_glsl` pack. On Switch,
  `/retroarch/shaders/shaders_glsl/` is read as well.

## RetroArch shaders

Settings -> DISPLAY -> **Shader** browses the folders above (and
`assets/shaders/`) like RetroArch's own menu; pick a `.glslp` preset or a
single `.glsl`. Shaders need the OpenGL renderer, which choosing one selects.
Multi-pass presets, LUT textures, frame history (`Prev`), float and sRGB
framebuffers, `#pragma parameter` defaults and preset parameter overrides
are supported. HD packs
and the HD scaling option are not combined with a shader (the shader gets
the original 1x picture, as in RetroArch).

On Switch the game asks for desktop OpenGL when a shader is chosen (as
RetroArch does there) and falls back to OpenGL ES 3.0 if it is not
available. Measured with Mesa's shader compiler (the one the Switch uses) on
the 620 presets of RetroArch's official `shaders_glsl` pack: 595 load on
desktop OpenGL and 475 on OpenGL ES (GLSL ES lacks macro token pasting and
other things many presets rely on). The 25 that fail on desktop GL have
errors in the shader itself that NVIDIA's compiler tolerates (gizmo-crt,
the GB Player/SGB "tvout" borders, pal, 2xSaL). RetroArch's `#reference`
presets (what its "Save preset" writes) work, parameter overrides included.

