#!/usr/bin/env python3
"""Import an official European translation (Illusion of Time ES/FR/DE ROM)
into a <lang>.json keyed by the USA string labels.

usage: import_rom.py <base.json> <en.json> <usa.sfc> <eu.sfc> <lang> <out.json>

The European releases run the same text engine and the same event scripts,
so every USA dialogue call site (COP BF / COP 6B <string>) has a counterpart
in the European ROM. Call sites are paired by dynamic programming over both
ROMs' call sites in address order, scoring the surrounding script bytes
(identical apart from pointers) and the string's control-code skeleton.
The paired European string, decoded and with its accented glyph codes turned
into Unicode, becomes the translation of the USA label. References that only
make sense inside the European ROM ([JMP], [ADR], [PRT] addresses) are
replaced by the USA string's own, in order.
"""
import json
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from romtext import RomText  # noqa: E402

# The European fonts put their accented letters in glyph codes $00-$0C
# (GaiaLabs prints them as these placeholder characters).
ACCENTS = {
    'es': {'Ǫ': 'ü', 'į': 'é', 'ņ': 'á', 'ţ': 'í', 'ę': 'ó', 'ť': 'ú', 'Ĕ': 'ñ', 'Ĭ': '¿', 'Ẋ': '¡'},
    'de': {'Ǫ': 'Ä', 'į': 'Ö', 'ņ': 'Ü', 'ţ': 'ä', 'ę': 'ö', 'ť': 'ü', 'Ĕ': 'ß'},
    'fr': {'į': 'à', 'ņ': 'â', 'ţ': 'ç', 'ę': 'é', 'ť': 'è', 'Ĕ': 'ê', 'Ň': 'î', 'Ĭ': 'ï',
           'Ẋ': 'ô', 'Ẍ': 'ù', 'Ĉ': 'û'},
}
NAMES = {'es': 'Español', 'de': 'Deutsch', 'fr': 'Français', 'it': 'Italiano', 'pt': 'Português'}
REF = re.compile(r'\[(JMP|ADR|PRT):[^\]]*\]')
SKEL = re.compile(r'\[(DLG|SIZ|TPL|PAL|SFX|ADR|BCD|DLY|SEP|NAM|SKP|BOR|PAU|DEF|CLD)(?::[^\]]*)?\]')


def call_sites(rom, targets=None):
    """[(offset, opcode, string offset)] for COP BF / COP 6B."""
    out = []
    for op in (0xBF, 0x6B):
        i = 0
        needle = bytes([0x02, op])
        while True:
            i = rom.find(needle, i)
            if i < 0:
                break
            t = (i & 0xFF0000) | rom[i + 2] | rom[i + 3] << 8
            if targets is None or t in targets:
                out.append((i, op, t))
            i += 1
    return sorted(out)


def skeleton(text):
    return tuple(SKEL.findall(text))


def context_score(a, ao, b, bo):
    s = 0
    for k in range(1, 9):
        s += a[ao - k] == b[bo - k]
    for k in range(4, 10):
        s += a[ao + k] == b[bo + k]
    return s


def main():
    base_p, en_p, us_p, eu_p, lang, out_p = sys.argv[1:7]
    base = json.load(open(base_p, encoding='utf-8'))
    en = json.load(open(en_p, encoding='utf-8'))['strings']
    us = open(us_p, 'rb').read()
    eu = open(eu_p, 'rb').read()
    us_text = RomText(us, base)
    eu_text = RomText(eu, base)

    off2label = {s['offset']: l for l, s in base['strings'].items() if l in en}
    us_sites = call_sites(us, set(off2label))
    eu_sites = []
    for off, op, t in call_sites(eu):
        try:
            text, _ = eu_text.decode(t)
        except (ValueError, IndexError):
            continue
        if '[#' in text:
            continue
        eu_sites.append((off, op, t, text))

    us_skel = [skeleton(en[off2label[t]]) for _, _, t in us_sites]
    eu_skel = [skeleton(x[3]) for x in eu_sites]
    n, m = len(us_sites), len(eu_sites)

    def score(i, j):
        uo, uop, _ = us_sites[i]
        eo, eop, _, _ = eu_sites[j]
        if uop != eop:
            return -1
        s = context_score(us, uo, eu, eo)
        if us_skel[i] == eu_skel[j]:
            s += 6
        return s if s >= 10 else -1

    # Monotonic alignment maximising the summed score (LCS-style DP, with a
    # band around the diagonal: the two ROMs keep their scripts in order).
    band = 250
    best = [[0] * (m + 1) for _ in range(n + 1)]
    move = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        row, prev = best[i], best[i - 1]
        centre = i * m // n
        lo, hi = max(1, centre - band), min(m, centre + band)
        for j in range(1, m + 1):
            if j < lo or j > hi:
                # outside the band: carry the best neighbour
                if prev[j] >= row[j - 1]:
                    row[j], move[i][j] = prev[j], 1
                else:
                    row[j], move[i][j] = row[j - 1], 2
                continue
            v, mv = prev[j], 1
            if row[j - 1] > v:
                v, mv = row[j - 1], 2
            sc = score(i - 1, j - 1)
            if sc >= 0 and prev[j - 1] + sc > v:
                v, mv = prev[j - 1] + sc, 3
            row[j], move[i][j] = v, mv
    pairs = []
    i, j = n, m
    while i and j:
        mv = move[i][j]
        if mv == 3:
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        elif mv == 1:
            i -= 1
        else:
            j -= 1

    accents = ACCENTS.get(lang, {})
    votes = {}
    for ui, ej in pairs:
        label = off2label[us_sites[ui][2]]
        votes.setdefault(label, []).append(eu_sites[ej][3])
    raw = {label: max(set(t), key=t.count) for label, t in votes.items()}

    # Second pass: strings only reached through other strings. A USA
    # reference (&label, @label, ADR table) and the European reference in the
    # same position name the same thing.
    strings_meta = base['strings']
    tables = base['tables']
    queue = list(raw)
    while queue:
        label = queue.pop()
        us_refs = [m.group(0) for m in REF.finditer(en.get(label, ''))]
        eu_refs = [m.group(0) for m in REF.finditer(raw[label])]
        if len(us_refs) != len(eu_refs):
            continue
        bank = strings_meta[label]['offset'] & 0xFF0000
        for ur, er in zip(us_refs, eu_refs):
            ukind, uargs = ur[1:-1].split(':', 1)
            eargs = er[1:-1].split(':', 1)[1].split(',')
            first = uargs.split(',')[0]
            name = first[1:].split('+')[0]
            if '+M' in first:
                continue
            eu_addr = int(eargs[0], 16)
            if ukind == 'PRT':
                targets = [(name, (eu_addr & 0x3F0000) | (eu_addr & 0xFFFF))]
            elif ukind == 'JMP':
                targets = [(name, bank | eu_addr)]
            else:  # ADR: a bank-relative table of string pointers
                t = tables.get(name)
                if not t:
                    continue
                eu_table = (eu_text.engine & 0) | bank | eu_addr
                targets = []
                for k, entry in enumerate(t['entries']):
                    ename = entry.split('+')[0]
                    if '+M' in entry:
                        continue
                    targets.append((ename, bank | eu_text.rd16(eu_table + 2 * k)))
            for us_label, eu_off in targets:
                if us_label in raw or us_label not in en:
                    continue
                try:
                    text, _ = eu_text.decode(eu_off)
                except (ValueError, IndexError):
                    continue
                if '[#' in text:
                    continue
                if strings_meta[us_label]['kind'] == 'unk16':
                    text = text.split('[RET]')[0]
                raw[us_label] = text
                queue.append(us_label)

    strings = {}
    skipped = 0
    for label, text in raw.items():
        for k, v in accents.items():
            text = text.replace(k, v)
        us_refs = REF.findall(en[label])
        eu_refs = REF.findall(text)
        if eu_refs != us_refs:
            skipped += 1          # structure differs; keep English
            continue
        full_us = [m.group(0) for m in REF.finditer(en[label])]
        it = iter(full_us)
        text = REF.sub(lambda _m: next(it), text)
        if '[::]' in en[label]:
            # Marker after the same number of line/page breaks.
            before = en[label].split('[::]')[0]
            k = len(re.findall(r'\[(?:N|FIN)\]', before))
            parts = re.split(r'(\[(?:N|FIN)\])', text)
            idx = 2 * k
            text = ''.join(parts[:idx]) + '[::]' + ''.join(parts[idx:])
        strings[label] = text

    doc = {
        'meta': {'language': lang, 'name': NAMES.get(lang, lang), 'format': 1,
                 'source': 'Imported from the official European release',
                 'extra_chars': ''.join(sorted(set(accents.values())))},
        'strings': dict(sorted(strings.items())),
    }
    with open(out_p, 'w', encoding='utf-8') as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
    print(f'{lang}: {len(us_sites)} USA sites, {len(eu_sites)} European sites, '
          f'{len(pairs)} paired, {len(strings)} strings imported '
          f'({skipped} kept in English: reference structure differs)')


if __name__ == '__main__':
    main()
