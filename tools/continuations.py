"""Find agbcc long-jump continuations (Riven) -> build/indirect/continuations.csv

agbcc emits `bl` as a long jump to the shared epilogue of big functions. The analyzer
sees the target as a new function. Rule: a function whose contiguous successor starts
exactly at its `hi` while it has no reachable return is really split in two; the
successor is a continuation of it. Chains are resolved to the root parent.
usage: python tools/continuations.py baserom.gba
"""
import csv, sys, bisect, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE = 0x08000000


def main():
    rom = open(sys.argv[1], 'rb').read()
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); md.detail = True
    F = sorted(csv.DictReader(open('build/analysis/functions.csv')), key=lambda r: int(r['addr'], 16))
    conf = {r['addr']: r for r in csv.DictReader(open('build/indirect/confidence.csv'))}
    by = {r['addr']: r for r in F}
    parent = {}
    for p, n in zip(F, F[1:]):
        if p['mode'] == n['mode'] == 'thumb' and int(p['hi'], 16) == int(n['addr'], 16) \
                and 'has_return' in conf[p['addr']]['failed_checks']:
            parent[n['addr']] = p['addr']

    def root(a):
        seen = 0
        while a in parent and seen < 16:
            a, seen = parent[a], seen + 1
        return a

    # bl sites that target a continuation (these are the jumps to emit as goto)
    wanted = set(parent)
    sites = collections.defaultdict(list)
    for r in F:
        if r['mode'] != 'thumb':
            continue
        lo, hi = int(r['lo'], 16), int(r['hi'], 16)
        for i in md.disasm(rom[lo - BASE:hi - BASE], lo):
            if i.mnemonic == 'bl':
                t = '0x%08X' % i.operands[0].imm
                if t in wanted:
                    sites[t].append('0x%08X' % i.address)
    with open('build/indirect/continuations.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['child', 'parent', 'root_parent', 'child_source', 'child_bytes', 'bl_sites', 'first_insn'])
        n_bl = 0
        for c in sorted(parent, key=lambda a: int(a, 16)):
            a = int(c, 16)
            i = next(md.disasm(rom[a - BASE:a - BASE + 4], a))
            s = sites.get(c, [])
            n_bl += bool(s)
            w.writerow([c, parent[c], root(c), by[c]['source'], by[c]['bytes'], ' '.join(s),
                        f'{i.mnemonic} {i.op_str}'])
    roots = {root(c) for c in parent}
    print(f'continuations: {len(parent)}  distinct root functions: {len(roots)}  '
          f'with a bl site: {n_bl}  (leaf junk without bl: {len(parent) - n_bl})')


if __name__ == '__main__':
    main()
