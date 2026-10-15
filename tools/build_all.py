"""One command from a ROM to a playable build (Riven).

  python tools/build_all.py [baserom.gba] [--jobs N] [--play [SCALE]]

Runs, in order, everything that used to be done by hand:
  1. check the ROM (SHA-1 of the supported USA release, B8KE)
  2. analyze_rom.py            -> build/analysis/     (function map, pass 1)
  3. resolve_indirect.py       -> build/indirect/     (leaf functions, veneers)
  4. score_functions.py        -> build/indirect/confidence.csv
  5. analyze_rom.py            (pass 2, ingests the extras and the confidence column)
  6. python -m recomp.program  -> build/recomp/program/kirby.exe  (compile + converge)
Nothing here is committed: build/ is ignored, the ROM never leaves the user's machine.
Prints timing per step. This is the script a launcher executable wraps.
"""
import argparse, hashlib, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPPORTED = {
    '274b102b6d940f46861a92b4e65f89a51815c12c': 'Kirby & The Amazing Mirror (USA), B8KE v0',
}


def step(name, cmd):
    t0 = time.time()
    print(f'\n== {name}', flush=True)
    r = subprocess.run(cmd, cwd=ROOT)
    dt = time.time() - t0
    print(f'   -> {"ok" if r.returncode == 0 else "FAILED (%d)" % r.returncode} in {dt:.0f}s', flush=True)
    if r.returncode:
        sys.exit(r.returncode)
    return dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom', nargs='?', default=os.path.join(ROOT, 'baserom.gba'))
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument('--play', nargs='?', const=3, type=int, metavar='SCALE',
                    help='open the game window when the build finishes')
    a = ap.parse_args()
    rom = os.path.abspath(a.rom)
    if not os.path.exists(rom):
        sys.exit(f'ROM not found: {rom}')
    sha = hashlib.sha1(open(rom, 'rb').read()).hexdigest()
    if sha not in SUPPORTED:
        sys.exit(f'Unsupported ROM (SHA-1 {sha}).\nOnly: ' + '; '.join(f'{v} = {k}' for k, v in SUPPORTED.items()))
    print(f'ROM ok: {SUPPORTED[sha]}')
    py = sys.executable
    t = {}
    t['analysis'] = step('1/5 analysis', [py, 'tools/analyze_rom.py', rom, '-o', 'build/analysis'])
    t['indirect'] = step('2/5 indirect branches and leaf functions', [py, 'tools/resolve_indirect.py', rom])
    t['score'] = step('3/5 confidence', [py, 'tools/score_functions.py', rom])
    t['analysis2'] = step('4/5 analysis (second pass)', [py, 'tools/analyze_rom.py', rom, '-o', 'build/analysis'])
    cmd = [py, '-m', 'recomp.program', '--rom', rom, '--jobs', str(a.jobs),
           '--functions', os.path.join(ROOT, 'build', 'analysis', 'functions.csv')]
    cmd += ['--window', str(a.play)] if a.play else ['--frames', '700']
    t['build'] = step('5/5 recompile, converge, link', cmd)
    print('\nDone. ' + ', '.join(f'{k} {v:.0f}s' for k, v in t.items()))


if __name__ == '__main__':
    main()
