"""Static confidence score for every function in build/analysis/functions.csv (Riven).

No emulator needed. For each function we check structural evidence that it is real
code, then bucket it high / medium / low and print which ones deserve suspicion.
Output: build/indirect/confidence.csv  (read-only use of Poppy's files).

usage: python tools/score_functions.py baserom.gba
"""
import csv, struct, sys, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone import arm as A

BASE = 0x08000000
BAD = {'udf', 'ldc', 'stc', 'mcr', 'mrc', 'cdp', 'ldc2', 'stc2', 'mcrr', 'mrrc', 'bkpt'}


def main():
    rom = open(sys.argv[1], 'rb').read()
    mds = {}
    for m, cm in (('arm', CS_MODE_ARM), ('thumb', CS_MODE_THUMB)):
        md = Cs(CS_ARCH_ARM, cm); md.detail = True; mds[m] = md
    rows = list(csv.DictReader(open('build/analysis/functions.csv')))
    # function [lo,hi) spans literal pools; decode only the code regions inside it
    import bisect
    regs = [(int(r['start'], 16), int(r['end'], 16), r['kind'])
            for r in csv.DictReader(open('build/analysis/regions.csv'))]
    rstart = [r[0] for r in regs]
    out = []
    for r in rows:
        a, lo, hi = int(r['addr'], 16), int(r['lo'], 16), int(r['hi'], 16)
        mode = r['mode']
        ins, decoded, want = [], 0, 0
        k = max(bisect.bisect_right(rstart, lo) - 1, 0)
        while k < len(regs) and regs[k][0] < hi:
            s0, e0, kind = regs[k]
            k += 1
            if kind not in ('arm', 'thumb'):
                continue
            s0, e0 = max(s0, lo), min(e0, hi)
            if e0 <= s0:
                continue
            seg = list(mds[kind].disasm(rom[s0 - BASE:e0 - BASE], s0))
            ins += seg
            decoded += sum(i.size for i in seg)
            want += e0 - s0
        f = collections.OrderedDict()
        f['clean_decode'] = decoded == want and want > 0
        f['no_cop_ops'] = not any(i.mnemonic in BAD for i in ins)
        f['has_return'] = any((i.mnemonic == 'bx' and i.op_str == 'lr') or
                              (i.mnemonic.startswith(('pop', 'ldm')) and 'pc' in i.op_str) or
                              (i.mnemonic == 'bx') for i in ins)
        zeros = sum(1 for i in ins if i.mnemonic in ('movs', 'mov') and i.op_str == 'r0, r0')
        f['not_zero_padding'] = zeros * 2 < max(len(ins), 1)
        # pc-relative literal loads must stay inside the ROM image
        lit_ok = True
        for i in ins:
            if i.mnemonic.startswith('ldr') and len(i.operands) >= 2 and i.operands[1].type == A.ARM_OP_MEM \
                    and i.operands[1].mem.base == A.ARM_REG_PC:
                la = ((i.address + (8 if mode == 'arm' else 4)) & ~3) + i.operands[1].mem.disp
                if not (BASE <= la < BASE + len(rom)):
                    lit_ok = False
        f['literals_in_rom'] = lit_ok
        callers = int(r['callers'] or 0)
        src = r['source']
        hard = f['clean_decode'] and f['no_cop_ops'] and f['has_return'] and f['not_zero_padding'] and lit_ok
        if not hard:
            tier = 'low'
        elif src in ('entry', 'adr', 'bx-const', 'bl') or callers > 0:
            tier = 'high'            # reached by control flow from known code
        elif src == 'addr-taken-leaf' and r.get('tier') == 'A':
            tier = 'medium'
        elif src == 'ptr-scan':
            tier = 'high' if len(ins) >= 4 else 'medium'
        else:
            tier = 'medium' if len(ins) >= 2 else 'low'
        out.append((r['addr'], mode, src, r.get('tier', ''), len(ins), callers, tier,
                    ','.join(k for k, v in f.items() if not v)))
    with open('build/indirect/confidence.csv', 'w', newline='') as o:
        w = csv.writer(o)
        w.writerow(['addr', 'mode', 'source', 'extra_tier', 'insns', 'callers', 'confidence', 'failed_checks'])
        w.writerows(out)
    c = collections.Counter(t[6] for t in out)
    print('confidence:', dict(c), 'of', len(out))
    by = collections.Counter((t[2], t[6]) for t in out)
    for s in sorted({t[2] for t in out}):
        print(f'  {s:16s}', {k: by[(s, k)] for k in ('high', 'medium', 'low')})
    fails = collections.Counter(x for t in out for x in t[7].split(',') if x)
    print('failed checks:', dict(fails))
    low = [t for t in out if t[6] == 'low']
    print('sample low:', [(t[0], t[2], t[4], t[7]) for t in low[:8]])


if __name__ == '__main__':
    main()
