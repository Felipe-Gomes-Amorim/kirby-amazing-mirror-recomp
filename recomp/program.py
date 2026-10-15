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
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from .decode import Unsupported
from .emit import discover, emit_program
from .harness import ROOT, RUNTIME, Rom, zig_cc

OUT = os.path.join(ROOT, 'build', 'recomp', 'program')


def load_functions(rom, csv_path):
    funcs, bad = [], {}
    for row in csv.DictReader(open(csv_path)):
        a, thumb = int(row['addr'], 16), row['mode'] == 'thumb'
        try:
            funcs.append(discover(rom.read16, a, thumb, max_insns=20000, max_span=0x100000))
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
        if name.endswith('.h'):
            hdr.update(open(os.path.join(RUNTIME, name), 'rb').read())
    objs = []

    def compile_one(src):
        with open(src, 'rb') as fh:
            h = hashlib.sha1(hdr.digest() + fh.read()).hexdigest()[:10]
        obj = src[:-2] + '_%s.o' % h
        if not os.path.exists(obj):
            r = subprocess.run(zig_cc() + ['-c', '-O1', '-w', '-I', RUNTIME, '-I', OUT, src, '-o', obj],
                               capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError('compilando %s:\n%s' % (src, r.stderr[-3000:]))
        return obj

    rt_srcs = [os.path.join(RUNTIME, n) for n in ('gba_bus.c', 'gba_rt.c', 'gba_bios.c', 'rt_main.c')]
    t1 = time.time()
    with ThreadPoolExecutor(jobs) as ex:
        objs = list(ex.map(compile_one, srcs + rt_srcs))
    if verbose:
        print('compilado em %.0fs' % (time.time() - t1))
    exe = os.path.join(OUT, 'rt_boot.exe' if os.name == 'nt' else 'rt_boot')
    r = subprocess.run(zig_cc() + ['-o', exe] + objs, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('link:\n' + r.stderr[-3000:])
    return exe


def boot(exe, rom_path, dump):
    r = subprocess.run([exe, rom_path, '--dump', dump], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def iterate(rom_path, csv_path, jobs=8, max_iter=40, verbose=True):
    """Recompila, roda ate falhar, e se a falha for codigo em RAM (overlay), descobre
    a funcao no snapshot da RAM e repete. Retorna (rc, stdout, stderr, overlays)."""
    rom = RamRom(open(rom_path, 'rb').read())
    funcs, bad = load_functions(rom, csv_path)
    overlays = {}
    dump = os.path.join(OUT, 'ram_dump.bin')
    os.makedirs(OUT, exist_ok=True)
    for it in range(max_iter):
        ram_rom = RamRom(rom.data, open(dump, 'rb').read() if os.path.exists(dump) and overlays else None)
        exe = build(ram_rom, funcs + list(overlays.values()), bad, jobs, verbose=verbose and it == 0)
        rc, out, err = boot(exe, rom_path, dump)
        if rc == 0:
            return rc, out, err, overlays
        m = re.search(r'pc=([0-9A-F]{8}) lr=.*? (thumb|arm) ', err)
        if 'sem funcao recompilada' not in err or not m:
            return rc, out, err, overlays
        pc, thumb = int(m.group(1), 16), m.group(2) == 'thumb'
        if pc >= 0x08000000 or pc in overlays:
            return rc, out, err, overlays
        snap = RamRom(rom.data, open(dump, 'rb').read())
        try:
            overlays[pc] = discover(snap.read16, pc, thumb, max_insns=5000, max_span=0x4000)
        except Unsupported as e:
            return rc, out, err + '\n[overlay] nao consegui decodificar %08X: %s\n' % (pc, e), overlays
        if verbose:
            print('[overlay] +%08X (%s, %d insns)' % (pc, 'thumb' if thumb else 'arm',
                                                      len(overlays[pc].insns)))
    return rc, out, err, overlays


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=os.path.join(ROOT, 'baserom.gba'))
    ap.add_argument('--functions', default=os.path.join(ROOT, 'build', 'analysis', 'functions.csv'))
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    rc, out, err, overlays = iterate(a.rom, a.functions, a.jobs)
    print(out)
    print(err[-3000:])
    return rc


if __name__ == '__main__':
    sys.exit(main())
