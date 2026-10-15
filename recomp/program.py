"""Monta o programa recompilado completo (a partir do mapa) e compila o runtime.

uso: python -m recomp.program --rom baserom.gba [--functions build/analysis/functions.csv]
         [--only-closure] [--jobs 8]
Saidas em build/recomp/program/: rc_protos.h, shard_N.c, table.c, rt_boot.exe
"""
import argparse
import csv
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from .decode import Unsupported
from .emit import discover, emit_program
from .harness import ROOT, RUNTIME, Rom, zig_cc

OUT = os.path.join(ROOT, 'build', 'recomp', 'program')


def load_continuations(csv_path):
    """build/analysis/continuations.csv -> ({pai: [filhos]}, {filhos que sao `bl` longo})."""
    p = os.path.join(os.path.dirname(csv_path), 'continuations.csv')
    kids, jumps = {}, set()
    if os.path.exists(p):
        with open(p) as f:
            for r in csv.DictReader(f):
                c, par = int(r['addr_filho'], 16), int(r['addr_pai'], 16)
                kids.setdefault(par, []).append(c)
                if 'stack-return' in r['motivo']:
                    jumps.add(c)
    return kids, jumps


def load_jumptables(csv_path):
    """build/analysis/jumptables.csv -> {endereco do salto: [alvos]} (so Thumb)."""
    p = os.path.join(os.path.dirname(csv_path), 'jumptables.csv')
    out = {}
    if os.path.exists(p):
        with open(p) as f:
            for r in csv.DictReader(f):
                if r['mode'] == 'thumb':
                    out.setdefault(int(r['site'], 16), []).extend(
                        int(t, 16) & ~1 for t in r['targets'].split())
    return out


def load_functions(rom, csv_path):
    funcs, bad = [], {}
    kids, jumps = load_continuations(csv_path)
    jt = load_jumptables(csv_path)
    with open(csv_path) as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        a, thumb = int(row['addr'], 16), row['mode'] == 'thumb'
        try:
            funcs.append(discover(rom.read16, a, thumb, max_insns=20000, max_span=0x100000,
                                  jump_targets=jumps, extra_seeds=kids.get(a, ()), indirect=jt))
        except Unsupported as e:
            bad[a] = str(e)
    return funcs, bad


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return False
    for attempt in range(10):   # Windows: antivirus/indexador pode segurar o arquivo
        try:
            with open(path, 'w') as f:
                f.write(text)
            return True
        except OSError:
            if attempt == 9:
                raise
            time.sleep(0.5)


class RamRom(Rom):
    """ROM + snapshot de IWRAM/EWRAM (para descobrir codigo copiado p/ a RAM)."""

    def __init__(self, data, ram=None):
        super().__init__(data)
        self.ram = ram or b''   # IWRAM (32K) seguido de EWRAM (256K)

    def read16(self, addr):
        if 0x03000000 <= addr < 0x03008000 and self.ram:
            o = addr - 0x03000000
        elif 0x02000000 <= addr < 0x02040000 and self.ram:
            o = 0x8000 + addr - 0x02000000
        else:
            return Rom.read16(self, addr)
        return self.ram[o] | (self.ram[o + 1] << 8)


def build(rom, funcs, bad=None, jobs=8, nshards=16, verbose=True):
    t0 = time.time()
    protos, shards, table = emit_program(funcs, rom.read16, nshards)
    os.makedirs(OUT, exist_ok=True)
    write_if_changed(os.path.join(OUT, 'rc_protos.h'), protos)
    srcs = []
    for k, text in enumerate(shards + [table]):
        p = os.path.join(OUT, 'shard_%d.c' % k if k < len(shards) else 'table.c')
        write_if_changed(p, text)
        srcs.append(p)
    if verbose:
        n = sum(len(f.insns) for f in funcs)
        print('funcoes: %d recompiladas (%d insns), %d nao suportadas; emitido em %.0fs' % (
            len(funcs), n, len(bad or ()), time.time() - t0))
    # objetos em paralelo; so recompila o que mudou (hash do .c + headers)
    hdr = hashlib.sha1()
    for name in sorted(os.listdir(RUNTIME)):
        if name == 'gba_cpu.h':   # unico header incluido pelo codigo gerado
            hdr.update(open(os.path.join(RUNTIME, name), 'rb').read())
    hdr_all = hashlib.sha1()   # fontes do runtime: qualquer .h conta
    for name in sorted(os.listdir(RUNTIME)):
        if name.endswith('.h'):
            hdr_all.update(open(os.path.join(RUNTIME, name), 'rb').read())
    objs = []

    def compile_one(src, flags=(), key=''):
        with open(src, 'rb') as fh:
            base = hdr_all if os.path.dirname(src) != OUT else hdr
            h = hashlib.sha1(base.digest() + key.encode() + fh.read()).hexdigest()[:10]
        os.makedirs(os.path.join(OUT, 'obj'), exist_ok=True)
        obj = os.path.join(OUT, 'obj', os.path.basename(src)[:-2] + ('_w' if flags else '') + '_%s.o' % h)
        if not os.path.exists(obj):
            r = subprocess.run(zig_cc() + ['-c', '-O1', '-w', '-I', RUNTIME, '-I', OUT,
                                           '-I', os.path.join(ROOT, 'hw')] + list(flags) +
                               [src, '-o', obj], capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError('compilando %s:\n%s' % (src, r.stderr[-3000:]))
        return obj

    rt_srcs = [os.path.join(RUNTIME, n) for n in ('gba_bus.c', 'gba_rt.c', 'gba_bios.c', 'rt_main.c',
                                                    'rt_script.c', 'rt_window.c', 'rt_interp.c')]
    rt_srcs.append(os.path.join(ROOT, 'hw', 'hw.c'))   # strong: substitui os stubs fracos
    t1 = time.time()
    with ThreadPoolExecutor(jobs) as ex:
        objs = list(ex.map(compile_one, srcs + rt_srcs))
    if verbose:
        print('compilado em %.0fs' % (time.time() - t1))
    exe = os.path.join(OUT, 'rt_boot.exe' if os.name == 'nt' else 'rt_boot')
    r = subprocess.run(zig_cc() + ['-o', exe] + objs, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('link:\n' + r.stderr[-3000:])
    build_window(objs, compile_one)
    return exe


SDL_DIR = os.environ.get('SDL2_DIR', 'C:/libs/SDL2-2.32.2/x86_64-w64-mingw32')


def build_window(objs, compile_one):
    """kirby.exe = o mesmo programa + janela SDL2 (hw/window_sdl.c). Precisa do SDL2 devel
    MinGW (SDL2_DIR). Sem SDL2 so ignora; rt_boot.exe continua valido."""
    inc = os.path.join(SDL_DIR, 'include', 'SDL2')
    if not os.path.isdir(inc):
        return None
    flags = ['-DRT_WINDOW', '-I', inc]
    w = [compile_one(os.path.join(RUNTIME, 'rt_window.c'), flags, 'win'),
         compile_one(os.path.join(ROOT, 'hw', 'window_sdl.c'), flags, 'win'),
         compile_one(os.path.join(ROOT, 'hw', 'audio_sdl.c'), flags, 'win')]
    base = [o for o in objs if not os.path.basename(o).startswith('rt_window_')]
    exe = os.path.join(OUT, 'kirby.exe' if os.name == 'nt' else 'kirby')
    imp = os.path.join(SDL_DIR, 'lib', 'libSDL2.dll.a')   # import lib (nao a estatica)
    r = subprocess.run(zig_cc() + ['-o', exe] + base + w +
                       [imp], capture_output=True, text=True)
    if r.returncode:
        print('[janela] link falhou:\n' + r.stderr[-1500:])
        return None
    dll = os.path.join(SDL_DIR, 'bin', 'SDL2.dll')
    if os.path.exists(dll):
        shutil.copy(dll, OUT)
    return exe


def boot(exe, rom_path, dump, extra=()):
    r = subprocess.run([exe, rom_path, '--dump', dump] + list(extra), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def _same_code(old, new, snap, rom):
    """True se `new` e `old` tem o mesmo conjunto de instrucoes e bytes."""
    if set(old.insns) != set(new.insns):
        return False
    return all(old.insns[a].raw == new.insns[a].raw for a in old.insns)


def iterate(rom_path, csv_path, jobs=8, max_iter=40, verbose=True, boot_args=()):
    """Recompila, roda ate falhar, e se a falha for codigo em RAM (overlay), descobre
    a funcao no snapshot da RAM e repete. Retorna (rc, stdout, stderr, overlays)."""
    rom = RamRom(open(rom_path, 'rb').read())
    funcs, bad = load_functions(rom, csv_path)
    overlays = {}
    dump = os.path.join(OUT, 'ram_dump.bin')
    unres = os.path.join(OUT, 'unresolved.log')
    extra_csv = os.path.join(OUT, 'extra_functions.csv')   # semente: log de nao resolvidos
    os.makedirs(OUT, exist_ok=True)
    have = {f.entry for f in funcs}
    extras = []
    if os.path.exists(extra_csv):
        with open(extra_csv) as fh:
            extras = [l.strip().split(',') for l in fh if l.strip()]
    m4a = os.path.join(ROOT, 'build', 'indirect', 'm4a_handlers.csv')   # Riven: handlers do M4A
    if os.path.exists(m4a):
        with open(m4a) as fh:
            for row in csv.DictReader(fh):
                extras.append([row['addr'][2:], row.get('mode', 'thumb')])

    def add_extra(addr, thumb):
        if addr in have:
            return False
        try:
            f = discover(rom.read16, addr, thumb, max_insns=5000, max_span=0x2000)
        except Unsupported:
            return False
        funcs.append(f)
        have.add(addr)
        return True

    for a, m in extras:
        add_extra(int(a, 16), m == 'thumb')
    for it in range(max_iter):
        try:
            os.remove(unres)
        except OSError:
            pass
        ram_rom = RamRom(rom.data, open(dump, 'rb').read() if os.path.exists(dump) and overlays else None)
        exe = build(ram_rom, funcs + list(overlays.values()), bad, jobs, verbose=verbose and it == 0)
        rc, out, err = boot(exe, rom_path, dump, list(boot_args) + ['--stop-on-unresolved'])
        new = []
        if os.path.exists(unres):   # semente: ROM sem funcao, qualquer que seja o desfecho
            with open(unres) as fh:
                for line in fh:
                    a, m_ = line.split(',')[:2]
                    if int(a, 16) < 0x08000000:
                        continue   # RAM: overlays (snapshot) ou interpretador
                    if add_extra(int(a, 16), m_ == 'thumb'):
                        new.append((a, m_))
        if new:
            extras += [list(x) for x in new]
            with open(extra_csv, 'w') as fh:
                fh.write(''.join('%s,%s\n' % tuple(x) for x in extras))
            if verbose:
                print('[unresolved] +%s' % ' '.join(a for a, _ in new))
            continue
        if '--stop-on-unresolved' in err:
            # nao deu para decodificar o alvo: segue com a aproximacao "retorna" (logada)
            rc, out, err = boot(exe, rom_path, dump, boot_args)
        if rc == 0:
            return rc, out, err, overlays
        ms = list(re.finditer(r'pc=([0-9A-F]{8}) lr=.*? (thumb|arm) ', err))
        m = ms[-1] if ms else None   # o ultimo dump e o da parada (ha dumps informativos antes)
        if 'sem funcao recompilada' not in err or not m:
            return rc, out, err, overlays
        pc, thumb = int(m.group(1), 16), m.group(2) == 'thumb'
        if pc >= 0x08000000:
            return rc, out, err, overlays
        snap = RamRom(rom.data, open(dump, 'rb').read())
        try:
            f = discover(snap.read16, pc, thumb, max_insns=5000, max_span=0x4000)
            same = [k for k in overlays if k[0] == pc]
            f.suffix = '_v%d' % len(same) if same else ''
            hi = max(i.next for i in f.insns.values())
            f.words = [snap.read16(a) or 0 for a in range(pc, hi, 2)]
            # variante identica a uma ja emitida = nada mudou: evita laco infinito
            if any(_same_code(overlays[k], f, snap, rom) for k in same):
                return rc, out, err + '\n[overlay] variante repetida em %08X\n' % pc, overlays
            overlays[(pc, len(same))] = f
            pc_key = (pc, len(same))
        except Unsupported as e:
            return rc, out, err + '\n[overlay] nao consegui decodificar %08X: %s\n' % (pc, e), overlays
        if verbose:
            print('[overlay] +%08X%s (%s, %d insns)' % (
                pc, f.suffix, 'thumb' if thumb else 'arm', len(f.insns)))
    return rc, out, err, overlays


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.path.join(ROOT, 'baserom.gba'))
    ap.add_argument('--functions', default=os.path.join(ROOT, 'build', 'analysis', 'functions.csv'))
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--timeout', type=int, default=120, help='watchdog do runtime (s)')
    ap.add_argument('--frames', type=int, default=0, help='roda N quadros (0 = para na 1a espera)')
    ap.add_argument('--keys', help='roteiro: "START@620,A@700+10"')
    ap.add_argument('--snap', help='quadros p/ salvar em build/recomp/frames: "600,2000"')
    ap.add_argument('--sav', help='arquivo de save (padrao: <rom>.sav; "none" desliga)')
    ap.add_argument('--bench-from', type=int, default=0, help='marca o inicio do trecho medido')
    ap.add_argument('--audio-raw', help='grava PCM cru (diagnostico)')
    ap.add_argument('--mute', action='store_true', help='janela sem som')
    ap.add_argument('--bench', action='store_true', help='mede quadros/s sem limite (use com --frames)')
    ap.add_argument('--window', nargs='?', const=3, type=int, metavar='ESCALA',
                    help='abre kirby.exe com janela SDL2 (escala 1-6)')
    a = ap.parse_args()
    frames_dir = os.path.join(ROOT, 'build', 'recomp', 'frames')
    os.makedirs(frames_dir, exist_ok=True)
    args = ['--frames-dir', frames_dir, '--timeout', str(a.timeout), '--unresolved', os.path.join(OUT, 'unresolved.log'),
            '--ppm', os.path.join(OUT, 'last_frame.ppm')]
    ppm = os.path.join(OUT, 'last_frame.ppm')
    if os.path.exists(ppm):
        os.remove(ppm)
    if a.frames:
        args += ['--frames', str(a.frames)]
    if a.bench:
        args += ['--bench']
        if a.audio_raw:
            args += ['--audio-raw', a.audio_raw]
        if a.bench_from:
            args += ['--bench-from', str(a.bench_from)]
    if a.window and not a.frames:
        args += ['--frames', '700']   # converge o mapa (nao resolvidos) antes de abrir a janela
    for flag, val in (('--keys', a.keys), ('--snap', a.snap), ('--sav', a.sav)):
        if val:
            args += [flag, val]
    rc, out, err, overlays = iterate(a.rom, a.functions, a.jobs, boot_args=args)
    print(out)
    for line in err.splitlines():
        if line.startswith('[snap]'):
            print(line)
    print(err[-3000:])
    if a.window and rc == 0:
        exe = os.path.join(OUT, 'kirby.exe' if os.name == 'nt' else 'kirby')
        if not os.path.exists(exe):
            print('kirby.exe nao foi gerado (SDL2 em %s?)' % SDL_DIR)
            return 1
        cmd = [exe, a.rom, '--window', str(a.window), '--unresolved', os.path.join(OUT, 'unresolved.log')]
        if a.mute:
            cmd.append('--mute')
        for flag, val in (('--keys', a.keys), ('--snap', a.snap), ('--sav', a.sav)):
            if val:
                cmd += [flag, val]
        print('abrindo:', ' '.join(cmd))
        return subprocess.run(cmd).returncode
    return rc


if __name__ == '__main__':
    sys.exit(main())
