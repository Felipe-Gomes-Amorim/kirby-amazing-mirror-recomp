"""Function-entry coverage helper (Riven).

  python tools/coverage.py prep    # build/analysis/functions.csv -> build/coverage/entries.txt
  python tools/coverage.py report  # compare build/coverage/hits.txt with the map

Run the mGBA script tools/mgba_coverage.lua between the two steps.
"""
import csv, os, sys, collections

AN = 'build/analysis/functions.csv'
DIR = 'build/coverage'


def load():
    rows = list(csv.DictReader(open(AN)))
    return {int(r['addr'], 16): r for r in rows}


def prep():
    os.makedirs(DIR, exist_ok=True)
    f = load()
    with open(f'{DIR}/entries.txt', 'w') as o:
        for a in sorted(f):
            if 0x08000000 <= a < 0x0A000000:
                o.write(f'0x{a:08X}\n')
    print(f'{len(f)} entries -> {DIR}/entries.txt')


def report():
    f = load()
    import glob
    hits = set()
    files = sorted(glob.glob(f'{DIR}/hits*.txt'))
    for p in files:
        hits |= {int(l, 16) for l in open(p) if l.strip()}
    print(f'merged {len(files)} hit file(s): {[os.path.basename(p) for p in files]}')
    src = collections.defaultdict(lambda: [0, 0])
    for a, r in f.items():
        s = r.get('source') or '?'
        src[s][0] += 1
        src[s][1] += a in hits
    print(f'entries hit: {len(hits & set(f))} / {len(f)}')
    print('by discovery source (hit / total):')
    for s, (n, h) in sorted(src.items(), key=lambda kv: -kv[1][0]):
        print(f'  {s:18s} {h:5d} / {n:5d}  ({100 * h / n:.0f}%)')
    tier = collections.Counter()
    for a, r in f.items():
        if r.get('source') == 'addr-taken-leaf':
            tier[(r.get('tier'), a in hits)] += 1
    print('addr-taken-leaf by tier:', dict(tier))
    outside = sorted(hits - set(f))
    print(f'hit addresses NOT in the map: {len(outside)}')
    for a in outside[:20]:
        print(f'  0x{a:08X}')


if __name__ == '__main__':
    {'prep': prep, 'report': report}[sys.argv[1]]()
