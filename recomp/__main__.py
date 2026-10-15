"""uso: python -m recomp {fuzz|rom} [--rom baserom.gba]"""
import argparse
import sys

from . import suites


def main():
    ap = argparse.ArgumentParser(prog='recomp')
    ap.add_argument('cmd', choices=['fuzz', 'rom'])
    ap.add_argument('--rom', default='baserom.gba')
    ap.add_argument('--count', type=int, default=60)
    ap.add_argument('--cases', type=int, default=40)
    ap.add_argument('--seed', type=int, default=7)
    a = ap.parse_args()
    bad = 0
    for thumb in (True, False):
        if a.cmd == 'fuzz':
            r = suites.fuzz(thumb, n_insns=a.count * 7, cases=a.cases // 3 or 1, seed=a.seed)
        else:
            r = suites.rom_suite(a.rom, thumb, a.count, a.cases, a.seed)
        bad += len(r[2])
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
