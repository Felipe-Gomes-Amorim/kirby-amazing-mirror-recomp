"""Classify and resolve indirect branches found by tools/analyze_rom.py.

Reads build/analysis/{functions,unresolved}.csv (read-only), writes
build/indirect/{resolved,unresolved,new_functions}.csv and a summary.

Findings that drive the design:
  * `pop {rN}; bx rN` is the agbcc interworking *return*, not a jump.
  * Real function-pointer calls go through libgcc stubs `_call_via_rN`
    (functions whose whole body is `bx rN`) via `bl stub`.
  * Thumb->ARM veneers: `adr rN,#0 ; bx rN`.
usage: resolve_indirect.py baserom.gba [analysis_dir] [out_dir]
"""
import csv, os, struct, sys, collections, bisect
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM
from capstone import arm as A

BASE = 0x08000000
REGS = {A.ARM_REG_R0 + i: f'r{i}' for i in range(13)}


def main():
    rom = open(sys.argv[1], 'rb').read()
    adir = sys.argv[2] if len(sys.argv) > 2 else 'build/analysis'
    odir = sys.argv[3] if len(sys.argv) > 3 else 'build/indirect'
    os.makedirs(odir, exist_ok=True)
    th = Cs(CS_ARCH_ARM, CS_MODE_THUMB); th.detail = True
    word = lambda a: struct.unpack_from('<I', rom, a - BASE)[0] if BASE <= a < BASE + len(rom) - 3 else None
    in_rom = lambda a: BASE <= a < BASE + len(rom)

    funcs = {int(r['addr'], 16): r for r in csv.DictReader(open(f'{adir}/functions.csv'))}
    starts = sorted(funcs)
    unres = [(int(r['addr'], 16), r['instruction']) for r in csv.DictReader(open(f'{adir}/unresolved.csv'))]

    def dis(a, n):  # n bytes of thumb at a
        return list(th.disasm(rom[a - BASE:a - BASE + n], a))

    # 1) classify unresolved bx sites
    returns, stubs, veneers, other = [], {}, [], []
    for a, ins in unres:
        prev = dis(a - 2, 2) if a - 2 >= BASE else []
        f = funcs.get(a)
        if f and int(f['bytes']) <= 4:
            stubs[a] = ins.split()[1]
        elif prev and prev[0].mnemonic == 'pop':
            returns.append(a)
        elif prev and prev[0].mnemonic == 'adr':
            veneers.append(a)
        else:
            other.append((a, ins))
    print(f'unresolved sites     : {len(unres)}')
    print(f'  returns (pop;bx rN): {len(returns)}')
    print(f'  call_via stubs     : {len(stubs)}  {sorted(f"{s:#x}:{r}" for s, r in stubs.items())}')
    print(f'  thumb->arm veneers : {len(veneers)}')
    print(f'  other              : {len(other)}')

    # 2) veneers: adr rN,#imm ; bx rN  -> ARM code at that address
    resolved, new_funcs = [], {}
    for a in veneers:
        ad = dis(a - 2, 2)[0]
        for i in dis(a - 2, 2):
            op = i.operands
        # adr target: capstone gives absolute address in op imm
        t = ((ad.address + 4) & ~3) + ad.operands[1].imm   # adr: Align(pc,4)+imm
        resolved.append((a, 'veneer', t, 'arm'))
        new_funcs[t] = 'arm'

    # 3) calls through stubs: find every `bl stub` in known thumb code
    sites = []
    for fa in starts:
        r = funcs[fa]
        if r['mode'] != 'thumb':
            continue
        lo, hi = int(r['lo'], 16), int(r['hi'], 16)
        for i in dis(lo, hi - lo):
            if i.mnemonic == 'bl' and i.operands[0].imm in stubs:
                sites.append((i.address, i.operands[0].imm, fa))

    why = collections.Counter()
    unresolved_rows = []
    for site, stub, fa in sites:
        reg = stubs[stub]
        lo = int(funcs[fa]['lo'], 16)
        # backward slice: linear within the function, stop at control flow
        body = dis(lo, site - lo)
        want, trace, result = reg, [], None
        for i in reversed(body[-14:]):
            mn, ops = i.mnemonic, i.operands
            dst = REGS.get(ops[0].reg) if ops and ops[0].type == A.ARM_OP_REG else None
            if mn in ('b', 'bx', 'bl') or (mn == 'pop' and 'pc' in i.op_str) or mn.startswith('b') and mn != 'bic':
                if mn == 'bl' and want not in ('r0', 'r1', 'r2', 'r3', 'ip', 'r12'):
                    continue  # callee-saved regs survive calls
                result = ('barrier', mn)
                break
            if dst != want:
                continue
            if mn == 'ldr' and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_PC:
                la = ((i.address + 4) & ~3) + ops[1].mem.disp
                w = word(la)
                result = ('const', w)
                break
            if mn in ('mov', 'adds', 'movs', 'add') and len(ops) >= 2 and ops[1].type == A.ARM_OP_REG \
                    and (len(ops) == 2 or ops[2].type == A.ARM_OP_IMM and ops[2].imm == 0):
                want = REGS.get(ops[1].reg, want)   # copy: follow the source
                continue
            if mn == 'ldr' and ops[1].type == A.ARM_OP_MEM:
                result = ('load', f'[{REGS.get(ops[1].mem.base, "?")}+{ops[1].mem.disp:#x}]')
                break
            result = ('computed', mn)
            break
        if result is None:
            result = ('barrier', 'start')
        kind, val = result
        if kind == 'const' and val is not None and in_rom(val & ~1):
            t = val & ~1
            mode = 'thumb' if val & 1 else 'arm'
            resolved.append((site, f'call_via_{reg}', t, mode))
            if t not in funcs:
                new_funcs[t] = mode
            why['const-rom-ptr'] += 1
        elif kind == 'const':
            why['const-nonrom'] += 1
            unresolved_rows.append((site, reg, 'const-nonrom', f'{val:#x}' if val is not None else ''))
        else:
            why[f'{kind}'] += 1
            unresolved_rows.append((site, reg, kind, str(val)))

    # 4) address-taken leaf functions the main analyzer misses (no `push {lr}`):
    # odd ROM word -> target in an unclassified ('data') region that decodes to a
    # short straight-line run ending in a return. Tier A: sits in a table next to
    # another thumb pointer (vtable/callback table); tier B: isolated.
    regions = [(int(r['start'], 16), int(r['end'], 16), r['kind'])
               for r in csv.DictReader(open(f'{adir}/regions.csv'))]
    rstart = [r[0] for r in regions]

    def kind_at(a):
        i = bisect.bisect_right(rstart, a) - 1
        return regions[i][2] if i >= 0 and a < regions[i][1] else None

    # real code ends where the call-graph stops (0x0815BD84 here); farther away
    # is gfx/audio/map data that merely looks like pointers. Ignore our own
    # earlier additions when computing the extent (the CSV may include them).
    code_end = max(int(r['hi'], 16) for r in funcs.values()
                   if r.get('source') != 'addr-taken-leaf') + 0x100
    # the analyzer may already have ingested an earlier run of this step; re-derive
    prior = {a for a, r in funcs.items() if r.get('source') == 'addr-taken-leaf'}
    known = set(funcs) - prior
    words = struct.unpack_from('<%dI' % (len(rom) // 4), rom)
    isptr = lambda w: bool(w & 1) and in_rom(w & ~1)
    leafs = {}
    for k, w in enumerate(words):
        if not isptr(w):
            continue
        t = w & ~1
        if t in known or t in new_funcs or t >= code_end or (kind_at(t) != 'data' and t not in prior):
            continue
        n, ok = 0, False
        for i in th.disasm(rom[t - BASE:t - BASE + 100], t):
            n += 1
            if i.mnemonic in ('udf', 'ldc', 'stc', 'mcr', 'mrc', 'cdp', 'ldc2', 'stc2'):
                break   # coprocessor/undefined ops never occur on ARM7TDMI GBA code
            if (i.mnemonic == 'bx' and i.op_str == 'lr') or (i.mnemonic == 'pop' and 'pc' in i.op_str):
                ok = True
                break
            if i.mnemonic in ('b', 'bx') or n > 24:
                break
        if not ok:
            continue
        nb = [words[j] for j in (k - 1, k + 1) if 0 <= j < len(words)]
        tier = 'A' if any(isptr(x) for x in nb) else 'B'
        if t not in leafs or tier == 'A':
            leafs[t] = tier
    ta = sum(1 for v in leafs.values() if v == 'A')
    print(f'address-taken leaf funcs missed by analyzer: {len(leafs)} (tier A table-adjacent {ta}, tier B {len(leafs) - ta})')

    # 4b) function-pointer TABLE completion: a run of >= 5 consecutive Thumb pointers into the code
    # area where most targets are already known functions is a dispatch table (the M4A command
    # table is one). Add the remaining entries if they decode to code that reaches a return.
    # Found because a from-scratch build had to rediscover 13 of them at run time (slow rebuilds).
    def plausible(t):
        n, ok = 0, False
        for i in th.disasm(rom[t - BASE:t - BASE + 200], t):
            n += 1
            if i.mnemonic in ('udf', 'ldc', 'stc', 'mcr', 'mrc', 'cdp', 'bkpt'):
                return False
            if (i.mnemonic == 'bx') or (i.mnemonic == 'pop' and 'pc' in i.op_str) or \
                    (i.mnemonic in ('mov', 'movs') and i.op_str.startswith('pc,')):
                ok = True
                break
            if n > 60:
                break
        return ok and n >= 2

    tables_done = {}
    k = 0
    while k < len(words):
        if not (isptr(words[k]) and (words[k] & ~1) < code_end):
            k += 1
            continue
        j = k
        while j < len(words) and isptr(words[j]) and (words[j] & ~1) < code_end:
            j += 1
        run = words[k:j]
        if len(run) >= 5:
            tg = [w & ~1 for w in run]
            nk = sum(1 for t in tg if t in known or t in new_funcs or t in leafs)
            if nk * 2 >= len(tg):          # at least half already functions
                for t in set(tg):
                    if t in known or t in new_funcs or t in leafs or t in tables_done or t % 2:
                        continue
                    if plausible(t):
                        tables_done[t] = BASE + k * 4
        k = j
    for t in tables_done:
        leafs.setdefault(t, 'T')
    print(f'pointer-table completion: {len(tables_done)} functions from '
          f'{len({v for v in tables_done.values()})} table(s)')

    print(f'\nindirect call sites (bl stub): {len(sites)}')
    for k, v in why.most_common():
        print(f'  {k:16s} {v}')
    tgts = {t for _, _, t, _ in resolved}
    print(f'resolved edges       : {len(resolved)}  distinct targets {len(tgts)}')
    print(f'targets NOT in functions.csv (new functions): {len(new_funcs)}')

    with open(f'{odir}/resolved.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['site', 'via', 'target', 'mode'])
        for s, v, t, m in sorted(resolved):
            w.writerow([f'{s:#010x}', v, f'{t:#010x}', m])
    with open(f'{odir}/unresolved.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['site', 'reg', 'reason', 'detail'])
        for s, r, k, d in sorted(unresolved_rows):
            w.writerow([f'{s:#010x}', r, k, d])
    with open(f'{odir}/new_functions.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['addr', 'mode', 'source', 'tier'])
        for t, m in sorted(new_funcs.items()):
            w.writerow([f'{t:#010x}', m, 'indirect', ''])
        for t, tier in sorted(leafs.items()):
            w.writerow([f'{t:#010x}', 'thumb', 'ptr-table' if tier == 'T' else 'addr-taken-leaf', tier])
    # also: what the stub/veneer 'other' sites look like
    with open(f'{odir}/other_sites.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['addr', 'insn'])
        for a, i in other:
            w.writerow([f'{a:#010x}', i])


if __name__ == '__main__':
    main()
