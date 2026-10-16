#!/usr/bin/env python3
"""Decode Illusion of Gaia/Time WideString text straight from a ROM image.

Shared by import_rom.py. The engine, command set and argument layout are the
same in every release; only the character map (per language font) and the
dictionary tables ([LU1]/[LU2]) move.
"""
import json
import re

HALT = {0xC0, 0xCA, 0xD1}          # END, RET, JMP
DICT_PATTERN = re.compile(rb'\xE2\x20\xA9(.)\x48\xAB\xC2\x20\xB9(..)', re.S)


class RomText:
    ARG_SIZE = {'Byte': 1, 'Word': 2, 'Address': 3, 'Offset': 2}

    def __init__(self, rom, base, charmap=None):
        self.rom = rom
        wide = next(t for t in base['types'] if t['name'] == 'WideString')
        self.chars = {}
        for i, ch in enumerate(wide['characterMap']):
            raw = (i & 0x0F) | ((i & 0xF0) << 1)
            if ch is not None and raw < 0xC0 and raw not in self.chars:
                self.chars[raw] = ch
        if charmap:
            self.chars.update(charmap)
        self.cmds = {c['key']: c for c in base['commands'] if c['set'] == 'WideString'}
        self.dicts = self.find_dictionaries()

    def find_dictionaries(self):
        """([LU1] table, [LU2] table) as ROM offsets. The engine's command
        table sits $6E bytes after its entry; entries $16/$17 are the
        dictionary handlers, which load their table with SEP #$20 /
        LDA #bank / PHA / PLB / REP #$20 / LDA table,Y."""
        engine = self.rom.find(bytes.fromhex('080bdaa900005bae9809e220b90000c9'))
        self.engine = engine
        table = engine + 0x6E
        out = []
        for idx in (0x16, 0x17):
            handler = (engine & 0xFF0000) | self.rd16(table + 2 * idx)
            m = DICT_PATTERN.search(self.rom, handler, handler + 40)
            bank = m.group(1)[0]
            addr = m.group(2)[0] | m.group(2)[1] << 8
            out.append(((bank & 0x3F) << 16) | addr)
        return out

    def rd16(self, off):
        return self.rom[off] | self.rom[off + 1] << 8

    def decode(self, off, expand=True, depth=0, limit=4096):
        """-> (text, end_offset). Stops after a halting command."""
        bank = off & 0xFF0000
        out = []
        i = off
        while i - off < limit:
            b = self.rom[i]
            i += 1
            if b < 0xC0:
                ch = self.chars.get(b)
                out.append(ch if ch is not None else f'[#{b:02X}]')
                continue
            c = self.cmds.get(b)
            if c is None:
                raise ValueError(f'bad command {b:02X} at {i - 1:06X}')
            name = c['value']
            if c['types'] == ['Binary']:
                j = self.rom.index(bytes([c.get('delimiter', 0)]), i)
                out.append(f'[{name}:' + ','.join(f'{x:X}' for x in self.rom[i:j]) + ']')
                i = j + 1
                continue
            args = []
            for t in c['types']:
                n = self.ARG_SIZE[t]
                v = int.from_bytes(self.rom[i:i + n], 'little')
                i += n
                args.append(v)
            if name in ('LU1', 'LU2') and expand and depth < 4:
                table = self.dicts[0 if name == 'LU1' else 1]
                word = (table & 0xFF0000) | self.rd16(table + 2 * args[0])
                w, _ = self.decode(word, expand, depth + 1)
                out.append(w.replace('[RET]', ''))
                continue
            out.append(f'[{name}' + (':' + ','.join(f'{a:X}' for a in args) if args else '') + ']')
            if b in HALT:
                break
        text = ''.join(out)
        if text.endswith('[RET]') and depth == 0:
            text = text[:-5]        # GaiaLabs convention: implicit terminator
        return text, i
