"""Random-input soak test for the recompiled game (Riven).

Runs build/recomp/program/rt_boot.exe headless (no window, no save) with a seeded random
key script and reports every run that does not end cleanly. Cheap: ~590 frames/s, so a
9000-frame run (2.5 min of game time) takes about 15 s.

  python tools/fuzz_run.py                    # seeds 1..24, 9000 frames, 4 workers
  python tools/fuzz_run.py --seeds 100-199 --frames 12000 --jobs 6
  python tools/fuzz_run.py --show 7           # print the key script of seed 7

A run is OK when it reaches RT_STOP frames, exit code 0 and nao_resolvidos=0.
Failures keep their full output in build/fuzz/seed_<n>.log; results go to build/fuzz/summary.csv.
Limits imposed by the runtime today: --keys takes <= 128 events and <= 1023 characters.
"""
import argparse, csv, os, random, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'recomp', 'program', 'rt_boot.exe')
ROM = os.path.join(ROOT, 'baserom.gba')
OUT = os.path.join(ROOT, 'build', 'fuzz')

# fixed prologue that reaches the first level (frame ~2000), found by the runtime author
INTRO = "START@620+8,A@800,A@1000,A@1200,START@1400,A@1600,A@1900"
# (key, hold range in frames, weight): walking right dominates, like a real playthrough
MOVES = [("RIGHT", (40, 160), 44), ("LEFT", (25, 90), 12), ("A", (4, 24), 16), ("B", (4, 30), 12),
         ("UP", (8, 50), 5), ("DOWN", (8, 40), 4), ("L", (4, 8), 3), ("R", (4, 8), 2),
         ("START", (4, 8), 1), ("SELECT", (4, 8), 1)]


def script(seed, frames):
    rnd = random.Random(seed)
    parts = [INTRO]
    size = len(INTRO)
    t = 2100
    n = 7
    keys = [m[0] for m in MOVES]
    weights = [m[2] for m in MOVES]
    while t < frames - 60 and n < 127:
        k = rnd.choices(keys, weights)[0]
        lo, hi = next(m[1] for m in MOVES if m[0] == k)
        tok = f"{k}@{t}+{rnd.randint(lo, hi)}"
        if size + len(tok) + 1 > 1000:
            break
        parts.append(tok)
        size += len(tok) + 1
        n += 1
        t += rnd.randint(8, 70)
    return ",".join(parts)


def run(seed, frames, timeout):
    keys = script(seed, frames)
    t0 = time.time()
    cmd = [EXE, ROM, '--frames', str(frames), '--keys', keys, '--sav', 'none',
           '--unresolved', os.path.join(OUT, f'seed_{seed}.unres'), '--timeout', str(timeout),
           '--frames-dir', os.path.join(OUT, 'frames')]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 30, cwd=ROOT)
        text = (p.stdout or '') + (p.stderr or '')
        code = p.returncode
    except subprocess.TimeoutExpired as e:
        text = ((e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')) + '\nHANG (timeout)'
        code = -999
    dt = time.time() - t0
    stop = re.search(r'RT_STOP frames pc=(\w+) lr=(\w+)', text)
    unres = re.search(r'unresolved=(\d+)', text)
    ended = bool(stop) and 'HALT: ' not in text and 'HALT: ' not in text
    ok = ended and code == 0 and unres is not None and unres.group(1) == '0'
    reason = ''
    if not ok:
        m = re.search(r'\[rt\] PARADA: ([^\n]*)', text)
        reason = m.group(1) if m else (f'exit={code}' if code else 'no RT_STOP')
        pc = re.search(r'\[rt\] pc=(\w+) lr=(\w+)', text)
        if pc:
            reason += f' pc={pc.group(1)} lr={pc.group(2)}'
        with open(os.path.join(OUT, f'seed_{seed}.log'), 'w') as f:
            f.write('KEYS: ' + keys + '\n\n' + text)
    return seed, ok, reason, round(dt, 1)


def parse_seeds(s):
    if '-' in s:
        a, b = s.split('-')
        return list(range(int(a), int(b) + 1))
    return [int(x) for x in s.split(',')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-24')
    ap.add_argument('--frames', type=int, default=9000)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--timeout', type=int, default=180)
    ap.add_argument('--show', type=int, help='print the key script for one seed and exit')
    a = ap.parse_args()
    if a.show is not None:
        print(script(a.show, a.frames))
        return
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(EXE):
        sys.exit(f'missing {EXE}: run python -m recomp.program first')
    seeds = parse_seeds(a.seeds)
    print(f'{len(seeds)} runs x {a.frames} frames ({a.frames / 3584:.1f} min of game time each), {a.jobs} jobs')
    rows = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for seed, ok, reason, dt in ex.map(lambda s: run(s, a.frames, a.timeout), seeds):
            rows.append((seed, ok, reason, dt))
            print(f'  seed {seed:4d}  {"ok  " if ok else "FAIL"}  {dt:5.1f}s  {reason}')
    with open(os.path.join(OUT, 'summary.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['seed', 'ok', 'reason', 'seconds', 'frames'])
        for r in rows:
            w.writerow([*r, a.frames])
    bad = [r for r in rows if not r[1]]
    total_min = len(rows) * a.frames / 3584
    print(f'\n{len(rows) - len(bad)}/{len(rows)} clean; {total_min:.0f} min of game time covered')
    if bad:
        by = {}
        for _, _, reason, _ in bad:
            key = re.sub(r'pc=\w+ lr=\w+', '', reason).strip() + ' @ ' + (re.search(r'pc=(\w+)', reason).group(1) if re.search(r'pc=(\w+)', reason) else '?')
            by[key] = by.get(key, 0) + 1
        print('distinct failures:')
        for k, v in sorted(by.items(), key=lambda kv: -kv[1]):
            print(f'  {v}x {k}')
        sys.exit(1)


if __name__ == '__main__':
    main()
