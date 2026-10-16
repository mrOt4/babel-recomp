#!/usr/bin/env python3
"""Generate an Illusion of Gaia Randomizer (IoGR) seed from your own US ROM,
for the "Randomizer" mod (choose the resulting .sfc on the Mods page).

IoGR (https://github.com/DontBaguMe/IoGR, public domain) is fetched into
tools/randomizer/.iogr on first use; its Python dependencies (bsdiff4,
bitstring, ips.py) must be installed:  pip install bsdiff4 bitstring ips.py

usage: generate.py <us-rom.sfc> [--seed N] [--difficulty easy|normal|hard|extreme]
                   [--goal dark_gaia|apocalypse_gaia|red_jewel_hunt|random]
                   [--statues 0-6|random] [--out DIR]

Same settings the website offers are available by editing the
RandomizerData(...) call below. The ROM you pass is only read.
"""
import argparse, os, random, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
IOGR = os.environ.get('IOGR_DIR') or os.path.join(HERE, '.iogr')


def fetch():
    if not os.path.isdir(os.path.join(IOGR, 'src', 'iog_randomizer')):
        subprocess.check_call(['git', 'clone', '--depth', '1',
                               'https://github.com/DontBaguMe/IoGR.git', IOGR])
    sys.path.insert(0, os.path.join(IOGR, 'src'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('rom')
    ap.add_argument('--seed', type=int, default=random.randint(0, 999999999))
    ap.add_argument('--difficulty', default='normal')
    ap.add_argument('--goal', default='dark_gaia')
    ap.add_argument('--statues', default='4')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    fetch()
    from iog_randomizer.randomizer.iogr_rom import Randomizer, generate_filename
    from iog_randomizer.randomizer.models.randomizer_data import RandomizerData
    from iog_randomizer.randomizer.models import enums

    def pick(enum, name):
        name = name.upper().replace('-', '_')
        for v in enum:
            if v.name == name:
                return v
        raise SystemExit('unknown %s: %s (one of %s)' % (enum.__name__, name,
                                                         ', '.join(v.name.lower() for v in enum)))

    settings = RandomizerData(seed=a.seed, difficulty=pick(enums.Difficulty, a.difficulty),
                              goal=pick(enums.Goal, a.goal), statues=a.statues)
    name = generate_filename(settings, 'sfc')
    ok, rom = Randomizer(a.rom).generate_rom(name, settings)
    if not ok:
        raise SystemExit('IoGR failed: %s' % (rom,))
    out = os.path.join(a.out or os.path.dirname(os.path.abspath(a.rom)), name)
    open(out, 'wb').write(rom)
    print(out)


if __name__ == '__main__':
    main()
