#!/usr/bin/env bash
# Stage the Switch SD-card folder from build-switch/:
#
#   <out>/switch/babel-recomp/babel-recomp.nro
#   <out>/switch/babel-recomp/{assets,i18n,rom.cfg}      game runtime files
#   <out>/switch/babel-recomp/mods/preloaded/            built-in mods (catalog)
#   <out>/switch/babel-recomp/mods/                      your own mod folders
#                                                        (e.g. mods/modhd)
#   <out>/switch/babel-recomp/msu1/                      MSU-1 pack (*.pcm)
#   <out>/switch/babel-recomp/languages/                 rom_es.sfc, rom_fr.sfc, rom_de.sfc
#   <out>/switch/babel-recomp/rom.sfc                    your USA ROM
#
# Only the game is staged: no ROM, MSU pack or art pack (those are yours to
# copy). Copy <out>/switch to the root of the SD card.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="${1:-$ROOT/build-switch}"
OUT="${2:-$ROOT/dist}"
D="$OUT/switch/babel-recomp"
[ -f "$B/babel-recomp.nro" ] || { echo "no $B/babel-recomp.nro (build first)"; exit 1; }
rm -rf "$D"
mkdir -p "$D/mods" "$D/msu1" "$D/languages" "$D/shaders"
cp "$B/babel-recomp.nro" "$D/"
# Symbols for crash reports (logs/crash-latest.log): kept beside dist/, not
# copied to the SD card.
mkdir -p "$(dirname "$D")/../switch-symbols"
cp "$B/babel-recomp.elf" "$(dirname "$D")/../switch-symbols/" 2>/dev/null || true
cp -r "$B/assets" "$B/i18n" "$D/"
[ -f "$B/rom.cfg" ] && cp "$B/rom.cfg" "$D/"
cp -r "$B/mods/preloaded" "$D/mods/"
rm -f "$D/mods/preloaded/state.toml"
cat > "$D/LEEME.txt" <<'TXT'
babel-recomp (Illusion of Gaia) para Nintendo Switch

Copia aquí (no se incluyen):
  rom.sfc                  tu ROM de Illusion of Gaia (USA)
  languages/rom_es.sfc ... tus ROMs europeas (Illusion of Time) para otros idiomas
  msu1/                    los .pcm de un pack MSU-1
  mods/<nombre>/           mods descomprimidos (carpeta con manifest.toml),
                           p. ej. mods/modhd
  shaders/                 shaders GLSL de RetroArch (.glslp/.glsl), p. ej. el
                           contenido de shaders_glsl.zip. Si ya tienes RetroArch,
                           también se leen de /retroarch/shaders/shaders_glsl/

Los mods se activan en el menú Mods del lanzador; el shader, en Settings ->
DISPLAY -> Shader. Dentro del juego, ZL + R3 abre los ajustes.
Arranque seguro: mantén ZL + R3 al arrancar si un shader deja la pantalla en negro.
TXT
du -sh "$D"; find "$D" -maxdepth 2 | sort
