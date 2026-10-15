"""Suites de teste diferencial: fuzz instrucao-a-instrucao e funcoes reais da ROM."""
import os
import random
import struct

from .decode import AL, Unsupported, decode
from .emit import discover, emit_unit
from .harness import ROM_BASE, Native, Oracle, Rom, build_lib, diff_function, ROOT

BUILD = os.path.join(ROOT, 'build', 'recomp')
BX_LR_ARM, BX_LR_THUMB = 0xE12FFF1E, 0x4770


def _report(name, res, verbose):
    ok = sum(r[0] for r in res)
    skipped = sum(r[1] for r in res)
    fails = [m for r in res for m in r[2]]
    steps = sum(r[3] for r in res)
    if verbose:
        print('%s: %d casos ok (%d passos comparados), %d descartados, %d falhas' % (
            name, ok, steps, skipped, len(fails)))
        for m in fails[:15]:
            print('  FALHA', m)
    return ok, skipped, fails, steps


def build_fuzz(thumb, n_insns, seed):
    """Instrucoes aleatorias validas, cada uma seguida de `bx lr`, em ROM sintetica."""
    rng = random.Random(seed)
    slots, words = [], bytearray()
    tries = 0
    while len(slots) < n_insns and tries < n_insns * 200:
        tries += 1
        addr = ROM_BASE + len(words)
        if thumb:
            hw = rng.getrandbits(16)
            blob = struct.pack('<4H', hw, BX_LR_THUMB, BX_LR_THUMB, 0)
            if hw >> 11 == 0b11110:
                blob = struct.pack('<4H', hw, 0xF800 | rng.getrandbits(11), BX_LR_THUMB, 0)
        else:
            w = rng.getrandbits(32)
            if rng.random() < 0.6:
                w = (w & 0x0FFFFFFF) | (AL << 28)
            if rng.random() < 0.1:   # multiplicacoes longas (umull/smlal...)
                w = (w & 0xF0FFFF0F) | 0x00800090
            blob = struct.pack('<4I', w, BX_LR_ARM, BX_LR_ARM, 0)
        rom = Rom(bytes(words) + blob)
        try:
            ins = decode(rom.read16, addr, thumb)
        except Unsupported:
            continue
        if ins.f.get('odd') or ins.kind in ('swi', 'msr', 'mrs'):
            continue
        slots.append(addr)
        words += blob
    rom = Rom(bytes(words) + b'\0' * 64)
    funcs = [discover(rom.read16, a, thumb, max_span=0) for a in slots]
    return rom, funcs


def fuzz(thumb, n_insns=400, cases=12, seed=1, verbose=True):
    rom, funcs = build_fuzz(thumb, n_insns, seed)
    lib = build_lib(emit_unit(funcs), BUILD)
    nat, ora = Native(lib, rom), Oracle(rom)
    res = [diff_function(nat, ora, f, cases, seed + k) for k, f in enumerate(funcs)]
    return _report('fuzz %s (%d instrucoes)' % ('thumb' if thumb else 'arm', len(funcs)), res, verbose)


def candidates(rom, thumb):
    """Inicios de funcao provaveis: `push {..,lr}` (Thumb) / `stmfd sp!,{..,lr}` (ARM)."""
    d = rom.data
    out = []
    if thumb:
        for o in range(0, len(d) - 1, 2):
            if d[o + 1] == 0xB5:
                out.append(ROM_BASE + o)
    else:
        for o in range(0, len(d) - 3, 4):
            if d[o + 3] == 0xE9 and d[o + 2] == 0x2D and d[o + 1] & 0x40:
                out.append(ROM_BASE + o)
    return out


def rom_functions(rom, thumb, count, seed=7, min_insns=6, max_insns=120):
    rng = random.Random(seed)
    cand = candidates(rom, thumb)
    rng.shuffle(cand)
    funcs = []
    for a in cand:
        try:
            f = discover(rom.read16, a, thumb, max_insns=max_insns)
        except Unsupported:
            continue
        if len(f.insns) >= min_insns:
            funcs.append(f)
            if len(funcs) == count:
                break
    return funcs, len(cand)


def rom_suite(rom_path, thumb, count=60, cases=40, seed=7, verbose=True):
    rom = Rom(open(rom_path, 'rb').read())
    funcs, ncand = rom_functions(rom, thumb, count, seed)
    lib = build_lib(emit_unit(funcs), BUILD)
    nat, ora = Native(lib, rom), Oracle(rom)
    res = [diff_function(nat, ora, f, cases, seed + k) for k, f in enumerate(funcs)]
    tot = sum(len(f.insns) for f in funcs)
    if verbose:
        print('rom %s: %d funcoes (%d instrucoes) de %d candidatas' % (
            'thumb' if thumb else 'arm', len(funcs), tot, ncand))
    return _report('rom %s' % ('thumb' if thumb else 'arm'), res, verbose)
