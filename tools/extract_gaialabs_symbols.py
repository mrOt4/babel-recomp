#!/usr/bin/env python3
"""Extract function symbols from GaiaLabs asm output into bankNN.cfg files."""
import re
import pathlib
import collections
import sys

ASM_DIR = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path.home() / "dev/GaiaLabs/GaiaPacker/bin/Release/net8.0/asm"
OUT_DIR = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else pathlib.Path.home() / "dev/illusion_of_gaia/IllusionOfGaiaUsaSNESRecomp/recomp"

# Only these prefixes are code (skip data: asciistring_, misc_, fx_, etc.)
CODE_PREFIXES = ("func_", "sub_", "code_", "zero_bytes_", "run_actors_", "stub_")
SYM_RE = re.compile(r"\b((?:" + "|".join(p[:-1] for p in CODE_PREFIXES) + r")_([0-9A-Fa-f]{6}))\b")

symbols = {}
for asm in ASM_DIR.rglob("*.asm"):
    text = asm.read_text(errors="replace")
    for m in SYM_RE.finditer(text):
        name, hexaddr = m.group(1), m.group(2)
        addr24 = int(hexaddr, 16)
        if name not in symbols:
            symbols[name] = addr24

print(f"Found {len(symbols)} unique symbols across {len(list(ASM_DIR.rglob(chr(42)+chr(46)+chr(97)+chr(115)+chr(109))))} files")

by_bank = collections.defaultdict(list)
for name, addr24 in symbols.items():
    bank = (addr24 >> 16) & 0xFF
    pc = addr24 & 0xFFFF
    by_bank[bank].append((pc, name))

for bank, entries in sorted(by_bank.items()):
    entries.sort()
    out = OUT_DIR / f"bank{bank:02X}.cfg"
    if bank == 0 and (OUT_DIR / "bank00.cfg").exists():
        print(f"Bank 00: {len(entries)} entries (NOT overwriting existing bank00.cfg)")
        print(f"  Append these manually to bank00.cfg after the symbols.toml block:")
        for pc, name in entries[:5]:
            print(f"  func {name} {pc:04X}")
        print(f"  ... ({len(entries)} total)")
        continue
    with out.open("w") as f:
        f.write(f"# Auto-generated from GaiaLabs asm output.\n")
        f.write(f"bank = {bank}\n")
        for pc, name in entries:
            f.write(f"func {name} {pc:04X}\n")
    print(f"Wrote {out.name}: {len(entries)} funcs")

