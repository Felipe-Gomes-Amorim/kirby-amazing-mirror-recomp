"""Recursive-descent code discovery for the GBA ROM (ARM + Thumb).

Seeds: entry point, IRQ handler, and anything reached via bl / literal-pool bx.
Output: JSON map of functions + coverage stats. Conservative: indirect jumps it
cannot resolve are listed, not guessed.
usage: discover.py baserom.gba [out.json]
"""
import json, struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_GRP_CALL
from capstone import arm as A

BASE = 0x08000000
ARM, THUMB = 0, 1


class Disc:
    def __init__(self, rom):
        self.rom = rom
        self.mds = {}
        for m, cm in ((ARM, CS_MODE_ARM), (THUMB, CS_MODE_THUMB)):
            md = Cs(CS_ARCH_ARM, cm)
            md.detail = True
            self.mds[m] = md
        self.insns = {}        # addr -> (mode, size)
        self.funcs = {}        # addr -> mode
        self.literals = set()  # addrs of pc-relative literal words (data)
        self.unresolved = []   # (addr, text)
        self.lit_ptrs = {}     # rom pointer found in literal pools -> count
        self.bad = []          # undecodable addrs reached by flow

    def word(self, addr):
        o = addr - BASE
        if 0 <= o <= len(self.rom) - 4:
            return struct.unpack_from('<I', self.rom, o)[0]
        return None

    def in_rom(self, a):
        return BASE <= a < BASE + len(self.rom)

    def decode(self, addr, mode):
        o = addr - BASE
        if not (0 <= o < len(self.rom)):
            return None
        for i in self.mds[mode].disasm(self.rom[o:o + 4], addr, 1):
            return i
        return None

    def run(self, seeds):
        work = [(a, m) for a, m in seeds]
        while work:
            a, m = work.pop()
            if a in self.funcs and self.funcs[a] == m:
                continue
            self.funcs[a] = m
            self.walk(a, m, work)

    def walk(self, entry, mode, work):
        stack = [entry]
        regs = {}                      # tiny const-prop, reset per path start
        while stack:
            a = stack.pop()
            regs = {}
            while True:
                if a in self.insns:
                    break
                i = self.decode(a, mode)
                if i is None:
                    self.bad.append(a)
                    break
                self.insns[a] = (mode, i.size)
                nxt = a + i.size
                mn = i.mnemonic
                ops = i.operands
                cond = i.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID, 0) if hasattr(A, 'ARM_CC_AL') else False
                is_pc_dst = bool(ops) and ops[0].type == A.ARM_OP_REG and ops[0].reg == A.ARM_REG_PC
                # literal pool loads -> const-prop + mark data
                if mn.startswith('ldr') and len(ops) >= 2 and ops[1].type == A.ARM_OP_MEM \
                        and ops[1].mem.base == A.ARM_REG_PC and ops[1].mem.index == 0:
                    la = ((a + (8 if mode == ARM else 4)) & ~3) + ops[1].mem.disp
                    if mode == ARM and ops[1].subtracted:
                        la = ((a + 8) & ~3) - abs(ops[1].mem.disp)
                    w = self.word(la)
                    self.literals.add(la)
                    if w is not None and ops[0].type == A.ARM_OP_REG:
                        if not is_pc_dst:
                            regs[ops[0].reg] = w
                        if self.in_rom(w & ~1):
                            self.lit_ptrs[w] = self.lit_ptrs.get(w, 0) + 1
                    if is_pc_dst:
                        self.unresolved.append((a, f'{mn} {i.op_str}'))
                        if not cond:
                            break
                elif mn in ('mov', 'movs') and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM \
                        and ops[0].type == A.ARM_OP_REG and not is_pc_dst:
                    regs[ops[0].reg] = ops[1].imm & 0xFFFFFFFF
                elif is_pc_dst and mn.startswith(('mov', 'add', 'sub')):
                    self.unresolved.append((a, f'{mn} {i.op_str}'))
                    if not cond:
                        break
                elif mn.startswith('pop') or mn.startswith('ldm'):
                    if any(o.type == A.ARM_OP_REG and o.reg == A.ARM_REG_PC for o in ops):
                        if not cond:
                            break
                elif mn.startswith('bx') or mn.startswith('blx'):
                    if ops and ops[0].type == A.ARM_OP_IMM:        # blx imm
                        work.append((ops[0].imm, THUMB if mode == ARM else ARM))
                    elif ops and ops[0].type == A.ARM_OP_REG:
                        v = regs.get(ops[0].reg)
                        if v is not None and self.in_rom(v & ~1):
                            work.append((v & ~1, THUMB if v & 1 else ARM))
                        elif ops[0].reg != A.ARM_REG_LR:
                            self.unresolved.append((a, f'{mn} {i.op_str}'))
                    if mn.startswith('bx') and not cond:
                        break
                elif mn.startswith('b'):
                    if ops and ops[0].type == A.ARM_OP_IMM and mn not in ('bic', 'bics', 'bfc', 'bfi'):
                        t = ops[0].imm
                        # NB: 'bls'/'blt'/'ble' are conditional branches, not calls;
                        # use capstone's CALL group instead of the mnemonic prefix.
                        if i.group(CS_GRP_CALL):                   # call: new function
                            work.append((t, mode))
                        elif self.in_rom(t):
                            stack.append(t)
                            if not cond and mn == 'b':
                                break
                elif mn in ('swi', 'svc'):
                    pass
                a = nxt


def main():
    rom = open(sys.argv[1], 'rb').read()
    d = Disc(rom)
    seeds = [(0x080000C0, ARM), (0x08000100, ARM)]
    d.run(seeds)

    # second pass: literal-pool ROM pointers with Thumb bit that decode as a
    # plausible function prologue (push {..lr}) are likely function pointers.
    # Also consider every aligned ROM word (function-pointer tables, jump tables
    # of handlers). Heuristic: Thumb-bit pointer to a `push {..., lr}` prologue.
    allw = struct.unpack_from('<%dI' % (len(rom) // 4), rom)
    for w in allw:
        if BASE <= w < BASE + len(rom) and (w & 1):
            d.lit_ptrs.setdefault(w, 0)
    for _ in range(8):
        new = []
        for w in sorted(d.lit_ptrs):
            t, m = w & ~1, (THUMB if w & 1 else ARM)
            if t in d.funcs or t in d.insns or t % (2 if m == THUMB else 4):
                continue
            i = d.decode(t, m)
            if i and i.mnemonic in ('push', 'stmdb', 'stmfd') and 'lr' in i.op_str:
                new.append((t, m))
        if not new:
            break
        d.run(new)

    covered = sum(sz for _, sz in d.insns.values()) + 4 * len(d.literals)
    out = {
        'rom_size': len(rom),
        'functions': {f'{a:#010x}': ('thumb' if m else 'arm') for a, m in sorted(d.funcs.items())},
        'unresolved_indirect': [{'addr': f'{a:#010x}', 'insn': t} for a, t in d.unresolved],
        'undecodable': [f'{a:#010x}' for a in sorted(set(d.bad))],
    }
    path = sys.argv[2] if len(sys.argv) > 2 else None
    if path:
        json.dump(out, open(path, 'w'), indent=1)
    print(f'functions        : {len(d.funcs)} '
          f'(arm {sum(1 for m in d.funcs.values() if m == ARM)}, thumb {sum(1 for m in d.funcs.values() if m)})')
    print(f'instructions     : {len(d.insns)}')
    print(f'literal words    : {len(d.literals)}')
    print(f'unresolved jumps : {len(d.unresolved)}')
    print(f'undecodable hits : {len(set(d.bad))}')
    print(f'ROM covered      : {covered} / {len(rom)} bytes ({100*covered/len(rom):.2f}%)')


if __name__ == '__main__':
    main()
