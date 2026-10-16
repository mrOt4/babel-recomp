#!/usr/bin/env python3
"""Illusion of Gaia text toolkit: extraction, encoding, language packs.

  iogtext.py extract  <gaialabs_asm_dir> <gaialabs_db_dir> <base.json> <en.json>
  iogtext.py validate <base.json> <rom.sfc>
  iogtext.py structure <base.json> <out structure.txt> [usa.sfc]   (text-free, shipped)
  iogtext.py build    <base.json> <lang.json> <out.iogl> [--fallback en.json] [--map out.map.json]

base.json is the structural description of the ROM's text: every string's
ROM offset and original text (GaiaLabs syntax, dictionary codes intact), the
dictionary tables, the pointer tables [ADR] indexes, and the character and
command tables. It is generated once from a GaiaLabs disassembly; a
translator only ever edits <lang>.json.

How a language is applied at runtime (src/iog_i18n.c):
  * every dialogue string is re-encoded, uncompressed, into "virtual" ROM
    banks served at HiROM $A0-$AF:8000-FFFF (cart overlay banks, mirrors a
    2 MB cartridge never uses); strings from one
    original bank share one virtual bank so bank-relative references
    ([JMP], [ADR] tables, [::] markers) still resolve;
  * the first 5 bytes of each original string become [PRT:@virtual][RET]:
    the game's own text engine prints the translation as a nested string
    and returns. No game code is modified.
"""
import json
import os
import re
import struct
import sys

MAGIC = b'IOGL'
VERSION = 1
VBANK_FIRST = 0xA0     # HiROM $A0-$AF:8000-FFFF (cart overlay banks)
VBANK_COUNT = 16
VBANK_BASE = 0x8000    # upper half only: DB-relative low WRAM must keep working
VBANK_SIZE = 0x8000
PATCH_LEN = 5          # [PRT addr24][RET]
CMD_PRT = 0xCD
CMD_RET = 0xCA

# --------------------------------------------------------------------------
# extract


LINE = re.compile(r'^(widestring|asciistring|spritestring)_([0-9A-F]{6}) ([`|~])(.*)\3\s*$')
BLOCK = re.compile(r'^([A-Za-z0-9_]+) \[\s*$')
ENTRY = re.compile(r'^\s*&([A-Za-z0-9_]+(?:\+M)?)\s*;([0-9A-F]+)')
UNK16 = re.compile(r'^\s*unk16 < `(.*)`, #([0-9A-F]+) >')
LU = re.compile(r'\[LU([12]):([0-9A-F]{1,2})\]')


def cmd_extract(asm_dir, db_dir, base_out, en_out):
    strings, blocks = {}, {}
    for root, _, files in os.walk(asm_dir):
        for name in sorted(files):
            if not name.endswith('.asm'):
                continue
            path = os.path.join(root, name)
            cur = None
            for line in open(path, encoding='utf-8', errors='replace'):
                line = line.rstrip('\n')
                m = LINE.match(line)
                if m:
                    kind, off, _, text = m.groups()
                    strings[f'{kind}_{off}'] = {
                        'kind': kind, 'offset': int(off, 16), 'text': text,
                        'file': os.path.relpath(path, asm_dir)}
                    continue
                b = BLOCK.match(line)
                if b:
                    cur = b.group(1)
                    blocks[cur] = []
                    continue
                if cur is not None:
                    e = ENTRY.match(line)
                    u = UNK16.match(line)
                    if e:
                        blocks[cur].append(e.group(1))
                    elif u and cur.startswith('unk16_'):
                        # A table-only label: text, its [RET], then a raw
                        # 32-bit tail other code reads straight from ROM.
                        tail = bytes.fromhex(u.group(2))
                        strings[cur] = {
                            'kind': 'unk16', 'offset': int(cur.rsplit('_', 1)[1], 16),
                            'text': u.group(1), 'tail': tail.hex(),
                            'file': os.path.relpath(path, asm_dir)}
                    elif line.startswith(']'):
                        cur = None
    dicts = {'1': blocks['dictionary_01EBA8'], '2': blocks['dictionary_01F54D']}
    ascii_lu_words = blocks.get('itemcomp_table_01EB0F', [])
    # Pointer tables reached by [ADR:&table,ram] (bank-relative).
    tables = {}
    for s in strings.values():
        for t in re.findall(r'\[ADR:&([A-Za-z0-9_]+),', s['text']):
            if t in blocks:
                tables[t] = {'offset': int(t.rsplit('_', 1)[1], 16), 'entries': blocks[t]}
    # Pointer tables reached by ASCII [PTR:@table,ram] (24-bit).
    ptr_tables = {}
    for st in strings.values():
        for t in re.findall(r'\[PTR:@([A-Za-z0-9_]+),', st['text']):
            if t in blocks:
                ptr_tables[t] = {'offset': int(t.rsplit('_', 1)[1], 16), 'entries': blocks[t]}
    types = json.load(open(os.path.join(db_dir, 'stringTypes.json'), encoding='utf-8-sig'))
    cmds = json.load(open(os.path.join(db_dir, 'stringCommands.json'), encoding='utf-8-sig'))
    base = {'strings': strings, 'dictionaries': dicts, 'tables': tables, 'ptr_tables': ptr_tables,
            'ascii_lu_words': ascii_lu_words,
            'types': types, 'commands': cmds}
    with open(base_out, 'w', encoding='utf-8') as f:
        json.dump(base, f, ensure_ascii=False, indent=0)

    words = set(dicts['1']) | set(dicts['2'])

    def expand(text, depth=0):
        def sub(m):
            w = strings[dicts[m.group(1)][int(m.group(2), 16)]]['text']
            w = w.replace('[RET]', '')
            return expand(w, depth + 1) if depth < 4 else w
        return LU.sub(sub, text)

    en = {'meta': {'language': 'en', 'name': 'English', 'format': 1},
          'strings': {}}
    for label, s in sorted(strings.items(), key=lambda kv: kv[1]['offset']):
        if label in words or s['kind'] not in ('widestring', 'unk16'):
            continue
        en['strings'][label] = expand(s['text'])
    with open(en_out, 'w', encoding='utf-8') as f:
        json.dump(en, f, ensure_ascii=False, indent=1)
    print(f'{len(strings)} strings, {len(tables)} ADR tables, '
          f'{len(en["strings"])} dialogue strings in {en_out}')


# --------------------------------------------------------------------------
# encoding


class Encoder:
    """GaiaLabs WideString syntax -> ROM bytes."""

    ARG_SIZE = {'Byte': 1, 'Word': 2, 'Address': 3, 'Offset': 2}

    def __init__(self, base, glyphs=None):
        wide = next(t for t in base['types'] if t['name'] == 'WideString')
        self.chars = {}
        for i, ch in enumerate(wide['characterMap']):
            if ch is None or ch in self.chars:
                continue
            raw = (i & 0x0F) | ((i & 0xF0) << 1)   # shiftType h2
            if raw < 0xC0:
                self.chars[ch] = raw
        for ch, raw in (glyphs or {}).items():
            self.chars[ch] = raw
        self.cmds = {c['value']: c for c in base['commands'] if c['set'] == 'WideString'}

    def tokens(self, text):
        i = 0
        while i < len(text):
            if text[i] == '[':
                j = text.index(']', i)
                yield ('cmd', text[i + 1:j])
                i = j + 1
            else:
                yield ('chr', text[i])
                i += 1

    def encode(self, text, resolve):
        """resolve(kind, ref, pos) -> int  for label references.
        Returns (bytes, markers) where markers lists byte positions of [::].

        GaiaLabs omits a WideString's natural terminator ([RET], $CA) from
        the text; it is appended here unless the text already ends with a
        halting command ([END], [RET], [JMP])."""
        out = bytearray()
        markers = []
        last = None
        for kind, tok in self.tokens(text):
            last = (kind, tok)
            if kind == 'chr':
                if tok not in self.chars:
                    raise ValueError(f'no glyph for {tok!r}')
                out.append(self.chars[tok])
                continue
            if tok == '::':
                markers.append(len(out))
                continue
            name, _, argstr = tok.partition(':')
            c = self.cmds.get(name)
            if c is None:
                raise ValueError(f'unknown command [{tok}]')
            out.append(c['key'])
            args = argstr.split(',') if argstr else []
            if c['types'] == ['Binary']:
                out += bytes(int(a, 16) for a in args)
                out.append(c.get('delimiter', 0))
                continue
            if len(args) != len(c['types']):
                raise ValueError(f'[{tok}] expects {len(c["types"])} args')
            for a, t in zip(args, c['types']):
                if a[0] in '&@':
                    v = resolve(a[0], a[1:], len(out))
                else:
                    v = int(a, 16)
                out += v.to_bytes(self.ARG_SIZE[t], 'little')
        if not (last and last[0] == 'cmd' and last[1].partition(':')[0] in HALT):
            out.append(CMD_RET)
        return bytes(out), markers


HALT = {'END', 'RET', 'JMP'}


def rom_offset(label):
    return int(label.rsplit('_', 1)[1].split('+')[0], 16)


def hirom(off):
    """The CPU address the game itself uses for a ROM offset: the FastROM
    $80-$BF mirror for upper halves, $C0-$FF otherwise."""
    if off & 0x8000:
        return 0x800000 | off
    return 0xC00000 | off


def cmd_validate(base_path, rom_path):
    base = json.load(open(base_path, encoding='utf-8'))
    rom = open(rom_path, 'rb').read()
    enc = Encoder(base)
    strings = base['strings']
    ok = bad = 0
    for label, s in strings.items():
        if s['kind'] not in ('widestring', 'unk16'):
            continue

        def resolve(kind, ref, pos, _s=s):
            name, _, suffix = ref.partition('+')
            off = rom_offset(name)
            if suffix == 'M':
                off += marker_of(name)
            return (off & 0xFFFF) if kind == '&' else hirom(off)

        def marker_of(name):
            b, m = enc.encode(strings[name]['text'], resolve)
            return m[0]
        try:
            b, _ = enc.encode(s['text'], resolve)
            b += bytes.fromhex(s.get('tail', ''))
        except Exception as e:  # noqa: BLE001
            print(f'{label}: {e}')
            bad += 1
            continue
        if rom[s['offset']:s['offset'] + len(b)] == b:
            ok += 1
        else:
            bad += 1
            if bad <= 10:
                got = rom[s['offset']:s['offset'] + len(b)]
                print(f'{label}: mismatch\n  enc {b[:24].hex()}\n  rom {got[:24].hex()}')
    print(f'validate: {ok} identical to ROM, {bad} different')
    return bad == 0



# --------------------------------------------------------------------------
# extra glyphs

# Dialogue-font codes no string in the game uses (logo pieces and a few
# symbols); a language pack's extra characters are written into these.
FREE_GLYPH_CODES = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                    0x0A, 0x0B, 0x0C, 0x6C, 0x8F, 0xAB, 0xAD, 0xAE, 0xAF]

# Accent marks, two rows each, drawn in colour 1 (the font's light ink).
MARKS = {
    'acute':      ['....11..', '...11...'],
    'grave':      ['..11....', '...11...'],
    'circumflex': ['...11...', '..1..1..'],
    'diaeresis':  ['........', '.11..11.'],
    'tilde':      ['..11.1..', '.1.11...'],
    'ring':       ['...11...', '...11...'],
}
COMPOSE = {
    'á': ('a', 'acute'), 'é': ('e', 'acute'), 'í': ('ı', 'acute'), 'ó': ('o', 'acute'), 'ú': ('u', 'acute'),
    'à': ('a', 'grave'), 'è': ('e', 'grave'), 'ì': ('ı', 'grave'), 'ò': ('o', 'grave'), 'ù': ('u', 'grave'),
    'â': ('a', 'circumflex'), 'ê': ('e', 'circumflex'), 'î': ('ı', 'circumflex'), 'ô': ('o', 'circumflex'), 'û': ('u', 'circumflex'),
    'ä': ('a', 'diaeresis'), 'ë': ('e', 'diaeresis'), 'ï': ('ı', 'diaeresis'), 'ö': ('o', 'diaeresis'), 'ü': ('u', 'diaeresis'),
    'ã': ('a', 'tilde'), 'õ': ('o', 'tilde'), 'ñ': ('n', 'tilde'),
    'Á': ('A', 'acute'), 'É': ('E', 'acute'), 'Í': ('I', 'acute'), 'Ó': ('O', 'acute'), 'Ú': ('U', 'acute'),
    'À': ('A', 'grave'), 'È': ('E', 'grave'), 'Â': ('A', 'circumflex'), 'Ê': ('E', 'circumflex'),
    'Ä': ('A', 'diaeresis'), 'Ö': ('O', 'diaeresis'), 'Ü': ('U', 'diaeresis'),
    'Ã': ('A', 'tilde'), 'Õ': ('O', 'tilde'), 'Ñ': ('N', 'tilde'),
}
HAND = {
    # 8x16, '3' background, '1' light ink, '2' dark ink.
    'ß': ['33333333', '33333333', '33333333', '33333333', '33111133', '31133113', '31133113', '31131133',
          '31133113', '31133313', '32233223', '32233223', '32232233', '33333333', '33333333', '33333333'],
    'œ': ['33333333', '33333333', '33333333', '33333333', '33333333', '31111113', '11311311', '11311311',
          '11311111', '11311333', '22322333', '22322322', '32222223', '33333333', '33333333', '33333333'],
    '«': ['33333333', '33333333', '33333333', '33333333', '33333333', '33133133', '31131133', '11311333',
          '11311333', '31131133', '33233233', '33333333', '33333333', '33333333', '33333333', '33333333'],
    '»': ['33333333', '33333333', '33333333', '33333333', '33333333', '33133133', '33113113', '33311311',
          '33311311', '33113113', '33233233', '33333333', '33333333', '33333333', '33333333', '33333333'],
}


def compose_glyph(ch, font, chars):
    """8x16 rows ('0'-'3') for an extra character."""
    if ch in HAND:
        return HAND[ch]
    if ch in ('¿', '¡'):
        src = font[f"{chars['?' if ch == '¿' else '!']:02X}"]
        flipped = [row[::-1] for row in reversed(src)]
        # Rotated, the mark hangs from the top; drop it so it sits on the
        # same baseline band as the letters (rows 4-13).
        return flipped[2:] + ['33333333'] * 2
    if ch == 'ç' or ch == 'Ç':
        rows = list(font[f"{chars['c' if ch == 'ç' else 'C']:02X}"])
        rows[13] = '3332233' + '3'
        rows[14] = '33223333'
        return rows
    base, mark = COMPOSE[ch]
    if base == 'ı':           # dotless i: the font's i with its dot removed
        rows = list(font[f"{chars['i']:02X}"])
        top = next(y for y, r in enumerate(rows) if r != '33333333')
        rows[top] = '33333333'
        if rows[top + 1].strip('3') == rows[top].strip('3'):
            rows[top + 1] = '33333333'
    else:
        rows = list(font[f"{chars[base]:02X}"])
    if base.isupper():
        # Capitals fill the cell from row 0: drop them two rows for the mark.
        rows = ['33333333', '33333333'] + rows[:14]
        at = 0
    else:
        at = 2
    for i, m in enumerate(MARKS[mark]):
        rows[at + i] = ''.join('1' if c == '1' else r for c, r in zip(m, rows[at + i]))
    return rows


def glyph_tiles(rows):
    """8x16 rows -> 32 bytes: the SNES 2bpp top tile, then the bottom tile."""
    out = bytearray()
    for y in range(16):
        lo = hi = 0
        for x, c in enumerate(rows[y]):
            v = int(c)
            lo |= (v & 1) << (7 - x)
            hi |= (v >> 1 & 1) << (7 - x)
        out += bytes([lo, hi])
    return bytes(out)


def build_glyphs(lang, font_path, enc):
    extra = lang.get('meta', {}).get('extra_chars', '')
    if not extra:
        return {}
    font = json.load(open(font_path, encoding='utf-8'))['glyphs']
    if len(extra) > len(FREE_GLYPH_CODES):
        raise SystemExit(f'{len(extra)} extra characters, only {len(FREE_GLYPH_CODES)} free glyph codes')
    out = {}
    for ch, code in zip(extra, FREE_GLYPH_CODES):
        out[ch] = {'code': code, 'tiles': glyph_tiles(compose_glyph(ch, font, enc.chars))}
    return out

# --------------------------------------------------------------------------
# build


def cmd_build(base_path, lang_path, out_path, fallback_path=None, font_path=None, map_path=None):
    base = json.load(open(base_path, encoding='utf-8'))
    lang = json.load(open(lang_path, encoding='utf-8'))
    fallback = json.load(open(fallback_path, encoding='utf-8')) if fallback_path else {'strings': {}}
    glyphs = build_glyphs(lang, font_path or os.path.join(os.path.dirname(base_path), 'font_wide.json'),
                          Encoder(base))
    enc = Encoder(base, {ch: g['code'] for ch, g in glyphs.items()})
    strings = base['strings']
    words = set(base['dictionaries']['1']) | set(base['dictionaries']['2'])

    def text_of(label):
        return lang['strings'].get(label) or fallback['strings'].get(label)

    # Which strings move: every dialogue string that has text and is long
    # enough in the ROM to hold the 5-byte redirect.
    labels = sorted((l for l, s in strings.items()
                     if s['kind'] in ('widestring', 'unk16') and l not in words and text_of(l)),
                    key=lambda l: strings[l]['offset'])

    def blob(l, resolve):
        b, m = enc.encode(text_of(l), resolve)
        return b + bytes.fromhex(strings[l].get('tail', '')), m
    starts = sorted(s['offset'] for s in strings.values())

    def room(off):
        nxt = next((o for o in starts if o > off), off + 0x10000)
        return nxt - off

    # Group by original bank; tables go with the bank they live in.
    groups = {}
    for l in labels:
        groups.setdefault(strings[l]['offset'] >> 16, []).append(l)
    tables = base['tables']

    # Two passes: sizes first (references resolved to 0), then placement.
    def sizes(label_list, bank_tables):
        n = 0
        for l in label_list:
            b, _ = blob(l, lambda k, r, p: 0)
            n += len(b)
        for t in bank_tables:
            n += 2 * len(tables[t]['entries'])
        return n

    vbanks = [bytearray(VBANK_SIZE) for _ in range(VBANK_COUNT)]
    fill = [0] * VBANK_COUNT
    placed = {}        # label -> (vbank index, offset)
    table_at = {}      # table -> (vbank index, offset)
    order = []
    for bank, ls in sorted(groups.items()):
        bt = [t for t in tables if tables[t]['offset'] >> 16 == bank]
        need = sizes(ls, bt)
        vi = next((i for i in range(VBANK_COUNT) if fill[i] + need <= VBANK_SIZE), None)
        if vi is None:
            raise SystemExit(f'bank {bank:02X}: {need} bytes do not fit')
        # Reserve offsets in placement order (strings then tables).
        pos = fill[vi]
        for l in ls:
            b, _ = blob(l, lambda k, r, p: 0)
            placed[l] = (vi, pos)
            pos += len(b)
        for t in bt:
            table_at[t] = (vi, pos)
            pos += 2 * len(tables[t]['entries'])
        fill[vi] = pos
        order.append((vi, ls, bt))

    markers = {}

    def encode_placed(l):
        vi, _ = placed[l]

        def resolve(kind, ref, pos):
            name, _, suffix = ref.partition('+')
            if name in placed and placed[name][0] == vi:
                v = VBANK_BASE + placed[name][1] + (markers[name][0] if suffix == 'M' else 0)
                return v if kind == '&' else ((VBANK_FIRST + vi) << 16) | v
            if name in table_at and table_at[name][0] == vi:
                return VBANK_BASE + table_at[name][1]
            if kind == '@' and name in placed:
                t_vi, t_off = placed[name]
                return ((VBANK_FIRST + t_vi) << 16) | (VBANK_BASE + t_off)
            if kind == '@':
                return hirom(rom_offset(name))
            raise ValueError(f'{l}: bank-relative &{ref} left its bank')
        return blob(l, resolve)

    # Markers first (their positions do not depend on references).
    for l in labels:
        _, markers[l] = enc.encode(text_of(l), lambda k, r, p: 0)
    patches = []
    for vi, ls, bt in order:
        for l in ls:
            b, _ = encode_placed(l)
            _, off = placed[l]
            vbanks[vi][off:off + len(b)] = b
            s = strings[l]
            # unk16 blocks are also read raw by other code: copy, never redirect.
            if s['kind'] == 'widestring' and room(s['offset']) >= PATCH_LEN:
                addr = ((VBANK_FIRST + vi) << 16) | (VBANK_BASE + off)
                patches.append((s['offset'], bytes([CMD_PRT]) + addr.to_bytes(3, 'little') + bytes([CMD_RET])))
        for t in bt:
            _, off = table_at[t]
            for i, entry in enumerate(tables[t]['entries']):
                name, _, suffix = entry.partition('+')
                if name in placed and placed[name][0] == vi:
                    v = VBANK_BASE + placed[name][1] + (markers[name][0] if suffix == 'M' else 0)
                else:
                    raise SystemExit(f'table {t}: entry {entry} is not relocated')
                vbanks[vi][off + 2 * i:off + 2 * i + 2] = v.to_bytes(2, 'little')

    if map_path:
        with open(map_path, 'w') as mf:
            json.dump({l: f'{VBANK_FIRST + vi:02X}:{VBANK_BASE + off:04X}' for l, (vi, off) in placed.items()},
                      mf, indent=0)
    used = sum(1 for f in fill if f)
    with open(out_path, 'wb') as f:
        meta = lang.get('meta', {})
        f.write(MAGIC + struct.pack('<I', VERSION))
        f.write(meta.get('language', '??').encode()[:8].ljust(8, b'\0'))
        f.write(meta.get('name', '').encode('utf-8')[:32].ljust(32, b'\0'))
        f.write(struct.pack('<I', used))
        for i in range(used):
            f.write(bytes(vbanks[i]))
        f.write(struct.pack('<I', len(patches)))
        for off, data in patches:
            f.write(struct.pack('<IB', off, len(data)) + data)
        f.write(struct.pack('<I', len(glyphs)))
        for ch, g in glyphs.items():
            f.write(struct.pack('<B', g['code']) + g['tiles'])
    total = sum(fill)
    print(f'{out_path}: {len(labels)} strings, {total} bytes in {used} virtual banks, '
          f'{len(patches)} redirects, {len(glyphs)} glyphs')


def cmd_structure(base_path, out_path, rom_path=None):
    """Text-free layout description shipped with the game: which ROM offsets
    hold dialogue strings, their [::] markers (byte offsets in the original
    encoding) and the [ADR] pointer tables. The runtime reads the text itself
    from the player's ROM."""
    base = json.load(open(base_path, encoding='utf-8'))
    enc = Encoder(base)
    strings = base['strings']
    words = set(base['dictionaries']['1']) | set(base['dictionaries']['2'])
    lines = ['# Illusion of Gaia text layout (no text). Generated by tools/i18n/iogtext.py structure.',
             '# S <label> <kind> <rom offset> [marker byte offsets...]',
             '# T <label> <rom offset> <entries...>   (entry+M = at the entry\'s marker)',
             '# F <rom offset>   24-bit pointer to the compressed BG3 font (scene_meta entry 0)',
             '# A <label> <rom offset>   menu/item string (ASCII engine)',
             '# P <label> <rom offset> <entries>   table reached by [PTR]',
             '# N <table> <region start> <region end>   overworld map names (sprite text)',
             '# Q <string> <LDA #imm offset>   prologue (sprite text, placed in the Z space)',
             '# Z <start> <end>   free ROM space ($FF padding) in the prologue\'s bank',
             '# E <table> <maps>   scene_actors: per map, actors up to $FF, then the scene title',
             'F 0D8008', 'F 0D9BAD', 'F 0DAFC2']
    for label, s in sorted(strings.items(), key=lambda kv: kv[1]['offset']):
        if label in words or s['kind'] not in ('widestring', 'unk16'):
            continue
        _, markers = enc.encode(s['text'], lambda k, r, p: 0)
        lines.append(f"S {label} {s['kind']} {s['offset']:06X}" + ''.join(f' {m:X}' for m in markers))
    for t, v in sorted(base['tables'].items()):
        lines.append(f"T {t} {v['offset']:06X} " + ' '.join(v['entries']))
    # Menu/item text (ASCII engine). Words of its [LU] table are left alone.
    lu_words = set()
    for name in base['blocks_lu'] if 'blocks_lu' in base else []:
        lu_words.add(name)
    for label, s in sorted(strings.items(), key=lambda kv: kv[1]['offset']):
        if s['kind'] == 'asciistring' and label not in base.get('ascii_lu_words', []):
            lines.append(f"A {label} {s['offset']:06X}")
    for t, v in sorted(base.get('ptr_tables', {}).items()):
        lines.append(f"P {t} {v['offset']:06X} {len(v['entries'])}")
    # Sprite text: repacked in place (no redirect command in that engine).
    if rom_path:
        rom = open(rom_path, 'rb').read()
        offs = sorted(v['offset'] for v in strings.values())

        def end_of(o):
            i = o
            while rom[i] != 0xCA:
                i += 1
            return i + 1
        sprites = sorted(v['offset'] for v in strings.values() if v['kind'] == 'spritestring')
        maps = [o for o in sprites if o >> 16 == 0x03]
        lines.append(f"N 03B1D4 {min(maps):06X} {max(end_of(o) for o in maps):06X}")
        pro = [o for o in sprites if o >> 16 == 0x0B]
        for o in pro:
            site = rom.find(bytes([0xA9, o & 0xFF, (o >> 8) & 0xFF]), o & 0xFF0000, (o & 0xFF0000) + 0x10000)
            lines.append(f"Q {o:06X} {site + 1:06X}")
        lines.append("Z 0BFA74 0C0000")
    lines.append("E 0C8000 256")
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{out_path}: {len(lines) - 5} records')


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == 'extract':
        cmd_extract(*args)
    elif cmd == 'structure':
        cmd_structure(*args)
    elif cmd == 'validate':
        sys.exit(0 if cmd_validate(*args) else 1)
    elif cmd == 'build':
        opts = {}
        for flag in ('--fallback', '--map'):
            if flag in args:
                i = args.index(flag)
                opts[flag] = args[i + 1]
                args = args[:i] + args[i + 2:]
        cmd_build(*args, fallback_path=opts.get('--fallback'), map_path=opts.get('--map'))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
