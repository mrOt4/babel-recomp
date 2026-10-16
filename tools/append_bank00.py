#!/usr/bin/env python3
import re, pathlib, sys
ASM_DIR = pathlib.Path.home() / "dev/GaiaLabs/GaiaPacker/bin/Release/net8.0/asm"
CFG = pathlib.Path.home() / "dev/illusion_of_gaia/IllusionOfGaiaUsaSNESRecomp/recomp/bank00.cfg"
CODE_PREFIXES = ("func_", "sub_", "code_", "zero_bytes_", "run_actors_", "stub_")
SYM_RE = re.compile(r"\b((?:" + "|".join(p[:-1] for p in CODE_PREFIXES) + r")_([0-9A-Fa-f]{6}))\b")
symbols = {}
for asm in ASM_DIR.rglob("*.asm"):
    for m in SYM_RE.finditer(asm.read_text(errors="replace")):
        name, hexaddr = m.group(1), m.group(2)
        addr24 = int(hexaddr, 16)
        if (addr24 >> 16) & 0xFF == 0 and name not in symbols:
            symbols[name] = addr24
entries = sorted(((pc, name) for name, addr24 in symbols.items() for pc in [addr24 & 0xFFFF]))
print(f"Bank00: {len(entries)} entries")
text = CFG.read_text()
marker = "# >>> BEGIN symbols.toml"
if marker not in text:
    print("Marker not found, aborting"); sys.exit(1)
before, after = text.split(marker, 1)
lines = [f"func {name} {pc:04X}" for pc, name in entries]
injection = "\n".join(lines) + "\n\n"
CFG.write_text(before.rstrip() + "\n\n" + injection + marker + after)
print(f"Appended to {CFG}")

