#!/usr/bin/env python3
"""Analise estatica de uma ROM de GBA.

Etapas:
  1. le o header (titulo, codigo do jogo, checksum, entry point)
  2. descida recursiva a partir do entry point, seguindo B/BL/BX e rastreando
     o modo (ARM x Thumb); resolve `ldr rX,=addr ; bx rX` e tabelas de switch
  3. varre a ROM atras de ponteiros para prologos de funcao (heuristica)
  4. classifica cada byte: ARM / Thumb / literal pool / tabela de salto / dados
  5. agrupa instrucoes em funcoes e grava relatorios em build/analysis/

Uso: python tools/analyze_rom.py [rom.gba] [-o build/analysis]
"""
import argparse
import bisect
import csv
import json
import re
import struct
import sys
from array import array
from collections import Counter, defaultdict, deque
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs

ROM_BASE = 0x08000000
CC = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al")
REG_ALIAS = {"ip": "r12", "fp": "r11", "sb": "r9", "sl": "r10", "r13": "sp", "r14": "lr", "r15": "pc"}
# instrucoes cujo primeiro operando e o registrador de destino
WRITES = {
    "mov", "movs", "mvn", "mvns", "add", "adds", "sub", "subs", "rsb", "rsbs", "adc", "adcs", "sbc", "sbcs",
    "and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics", "lsl", "lsls", "lsr", "lsrs", "asr", "asrs",
    "ror", "rors", "neg", "negs", "mul", "muls", "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "mla", "mlas",
}
LDM_PLAIN = {"ldm", "ldmia", "ldmib", "ldmda", "ldmdb", "ldmfd", "ldmfa", "ldmed", "ldmea", "pop"}
RETURN_REGS = {"lr"}


def reg(name):
    name = name.strip().lower()
    return REG_ALIAS.get(name, name)


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_imm(s):
    s = s.strip().lstrip("#")
    try:
        return int(s, 0)
    except ValueError:
        return None


def split_branch(m):
    """'bne' -> ('b', True); 'bl' -> ('bl', False); outro -> (None, False)."""
    if m in ("b", "bl", "bx", "blx"):
        return m, False
    for base in ("blx", "bl", "bx", "b"):
        if m.startswith(base) and m[len(base):] in CC:
            return base, True
    return None, False


class Insn:
    __slots__ = ("addr", "size", "thumb", "mnem", "ops", "kind", "target", "succ", "calls", "cond")

    def __init__(self, addr, size, thumb, mnem, ops):
        self.addr, self.size, self.thumb, self.mnem, self.ops = addr, size, thumb, mnem, ops
        self.kind = "seq"   # seq | jump | call | ret | ind
        self.target = None  # alvo direto de B/BL
        self.succ = []      # sucessores dentro da mesma funcao
        self.calls = []     # alvos de chamada
        self.cond = False


class Analyzer:
    def __init__(self, rom):
        self.rom = rom
        self.n = len(rom)
        self.cs = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
        self.insns = {}            # addr -> Insn
        self.funcs = {}            # addr -> (thumb, source)
        self.pool = set()          # enderecos de words lidos por ldr pc-relativo
        self.tables = []           # dicts: site, base, targets, thumb
        self.unresolved = []       # (addr, mnemonic) saltos/chamadas indiretos sem alvo
        self.ram_targets = Counter()
        self.call_args = []        # (site, alvo, r0, r1, r2) de `bl` direto; None = nao constante
        self.ram_entries = set()   # (endereco RAM, thumb) alvos de bx/blx constantes em RAM
        self.ram_blocks = []       # codigo copiado ROM -> RAM (ver find_ram_code)
        self.prev_pop = None       # registrador do `pop {rN}` anterior (p/ pop;bx = retorno)
        self.func_tier = {}        # addr -> 'A'/'B' (confianca das funcoes extras)
        self.adr_seeds = []        # alvos de `add rX,pc,#imm` (ARM)
        self.bad = []              # enderecos que nao decodificaram
        self.mode_conflicts = 0
        self.conflict_sites = []   # enderecos onde ARM e Thumb se sobrepoem
        self.cur_owner = None      # funcao (entrada) a que a descida em curso pertence
        self.owner = {}            # alvo empilhado -> funcao dona (branch/switch/salto longo)
        self.insn_owner = {}       # endereco de instrucao -> funcao dona
        self.cont = {}             # alvo -> {dona: motivo}: blocos de continuacao (bl como salto longo)
        self._lj_cache = {}
        self.extra_conts = []      # extras que caem dentro de outra funcao

    # ---- acesso a ROM ----
    def off(self, addr):
        o = addr - ROM_BASE
        return o if 0 <= o < self.n else None

    def word(self, addr):
        o = self.off(addr)
        if o is None or o + 4 > self.n:
            return None
        return struct.unpack_from("<I", self.rom, o)[0]

    def decode(self, addr, thumb):
        o = self.off(addr)
        if o is None:
            return None
        for i in self.cs[thumb].disasm(bytes(self.rom[o:o + 4]), addr, 1):
            return i
        return None

    def add_func(self, addr, thumb, src):
        if self.off(addr) is None:
            self.ram_targets[addr] += 1
            return False
        if addr not in self.funcs:
            self.funcs[addr] = (thumb, src)
        return True

    # ---- descida recursiva ----
    def run(self, seeds):
        work = deque(seeds)
        while work:
            addr, thumb, src = work.popleft()
            if self.off(addr) is None:
                continue
            self.cur_owner = self.owner.get(addr, addr)
            self.walk(addr, thumb, work)
            while self.adr_seeds:  # `add rX,pc,#imm` em ARM: ponteiro de codigo (ex.: handler de IRQ)
                t = self.adr_seeds.pop()
                if self.add_func(t, False, "adr"):
                    work.append((t, False, "adr"))

    def walk(self, addr, thumb, work):
        regs = {}
        last_cmp = None
        mov_lr_pc = False
        self.prev_pop = None
        step = 2 if thumb else 4
        while True:
            if addr in self.insns:
                if self.insns[addr].thumb != thumb:
                    self.mode_conflicts += 1
                    self.conflict_sites.append(addr)
                return
            if self.off(addr) is None or addr % step:
                return
            d = self.decode(addr, thumb)
            if d is None:
                self.bad.append(addr)
                return
            ins = Insn(addr, d.size, thumb, d.mnemonic, split_ops(d.op_str))
            self.insns[addr] = ins
            self.insn_owner[addr] = self.cur_owner
            nxt = addr + d.size
            stop = self.classify(ins, regs, last_cmp, mov_lr_pc, work)
            m, ops = ins.mnem, ins.ops
            if m == "cmp" and len(ops) == 2:
                imm = parse_imm(ops[1])
                last_cmp = imm if imm is not None else last_cmp
            mov_lr_pc = (m == "mov" and ops == ["lr", "pc"])
            pm = re.fullmatch(r"\{(r\d+)\}", ops[0]) if (m == "pop" and len(ops) == 1) else None
            self.prev_pop = pm.group(1) if pm else None
            if not stop:
                ins.succ.append(nxt)
            if ins.kind == "jump" and ins.target is not None:
                ins.succ.append(ins.target)
            if stop:
                return
            addr = nxt

    def classify(self, ins, regs, last_cmp, mov_lr_pc, work):
        """Preenche kind/target/succ. Retorna True se o fluxo linear termina aqui."""
        m, ops, thumb, addr = ins.mnem, ins.ops, ins.thumb, ins.addr
        base, cond = split_branch(m)
        ins.cond = cond

        if base in ("b", "bl", "blx") and ops and ops[0].startswith("#"):
            tgt = parse_imm(ops[0])
            ins.target = tgt
            if base == "b":
                ins.kind = "jump"
                self.owner.setdefault(tgt, self.cur_owner)
                work.append((tgt, thumb, "branch"))
                return not cond
            flip = base == "blx"
            tmode = (not thumb) if flip else thumb
            if thumb and base == "bl" and not cond:
                why = self.long_jump_reason(tgt)
                if why:  # agbcc: `bl` como salto longo para o epilogo/continuacao da MESMA funcao
                    ins.kind = "jump"
                    self.cont.setdefault(tgt, {})[self.cur_owner] = why
                    self.owner.setdefault(tgt, self.cur_owner)
                    work.append((tgt, thumb, "longjump"))
                    return True
            ins.kind = "call"
            ins.calls.append(tgt)
            self.call_args.append((addr, tgt) + tuple(
                regs[r][1] if regs.get(r) and regs[r][0] == "c" else None for r in ("r0", "r1", "r2")))
            if self.add_func(tgt, tmode, "bl"):
                work.append((tgt, tmode, "bl"))
            self.after_call(regs)
            return False

        # ---- indirect: bx rX / blx rX / mov pc,rX / ldr pc,... / add pc,... ----
        indirect = None  # 'bx' | 'mov' | 'ldr' | 'add' | 'blx'
        if base == "bx" and ops:
            indirect = "bx"
        elif base == "blx" and ops:
            indirect = "blx"
        elif ops and reg(ops[0]) == "pc":
            core = m
            for suf in CC:
                if len(core) > 2 and core.endswith(suf) and core[:-2] in WRITES:
                    core, ins.cond = core[:-2], True
                    break
            if core in ("mov", "movs"):
                indirect = "mov"
            elif core.startswith("ldr"):
                indirect = "ldr"
            elif core in ("add", "adds"):
                indirect = "add"
            elif core in ("sub", "subs"):
                ins.kind = "ret"
                return not ins.cond
        elif m.startswith(("ldm", "pop")) and re.search(r"\bpc\b", ins.ops[-1] if ops else ""):
            ins.kind = "ret"
            return m in LDM_PLAIN  # condicional continua no fluxo linear
        if indirect == "ldr" and len(ops) > 1 and ops[1].startswith("[sp"):
            ins.kind = "ret"       # ldr pc, [sp], #4
            return not ins.cond
        if indirect is None:
            self.track(ins, regs, last_cmp)
            return False

        src = reg(ops[-1]) if indirect in ("bx", "blx", "mov") else None
        if src in RETURN_REGS and indirect != "blx":
            ins.kind = "ret"
            return not ins.cond
        if indirect == "bx" and src is not None and src == self.prev_pop:
            ins.kind = "ret"       # pop {rN}; bx rN: retorno interworking (agbcc)
            return not ins.cond
        is_call = indirect == "blx" or mov_lr_pc
        ins.kind = "call" if is_call else "ind"
        val = regs.get(src) if src else None
        if val and val[0] == "lr" and indirect == "bx":
            ins.kind = "ret"
            return not ins.cond

        # switch com tabela: add rd,rn,rm ; ldr rd,[rd] ; mov pc,rd / bx rd
        if val and val[0] == "e" and not is_call:
            self.add_table(ins, val[1], last_cmp, work)
            return True
        if indirect == "add" and thumb is False and ops[:2] and reg(ops[1]) == "pc":
            self.add_arm_branch_table(ins, last_cmp, work)
            return True
        if val and val[0] == "c":
            v = val[1]
            tthumb = bool(v & 1) if indirect in ("bx", "blx") else thumb
            tgt = v & ~1 if tthumb else v & ~3
            if self.off(tgt) is None and tgt >> 24 in (2, 3):
                self.ram_entries.add((tgt, tthumb))   # codigo em RAM: resolvido em find_ram_code
            if self.add_func(tgt, tthumb, "bx-const"):
                work.append((tgt, tthumb, "bx-const"))
                ins.target = tgt
                if is_call:
                    ins.calls.append(tgt)
                else:
                    ins.kind = "jump"
            if is_call:
                self.after_call(regs)
            return not (is_call or ins.cond)
        self.unresolved.append((addr, m + " " + " ".join(ops)))
        if is_call:
            self.after_call(regs)
        return not (is_call or ins.cond)

    @staticmethod
    def after_call(regs):
        """Apos um `bl`: esquece constantes, mas mantem copias de lr (`mov ip,lr ... bx ip`)."""
        for k in [k for k, v in regs.items() if v[0] != "lr"]:
            del regs[k]

    def track(self, ins, regs, last_cmp):
        m, ops, addr = ins.mnem, ins.ops, ins.addr
        if m in ("pop",) or m.startswith(("ldm", "stm")):
            self.after_call(regs)
            return
        if not ops:
            return
        rd = reg(ops[0])
        if m == "ldr" and len(ops) >= 2 and ops[1].startswith("[pc"):
            mm = re.match(r"\[pc(?:,\s*#(-?(?:0x)?[0-9a-fA-F]+))?\]", ops[1])
            imm = parse_imm(mm.group(1)) if mm and mm.group(1) else 0
            if mm and imm is not None:
                pc = (addr + 4) & ~3 if ins.thumb else addr + 8
                lit = pc + imm
                self.pool.add(lit)
                w = self.word(lit)
                regs.pop(rd, None)
                if w is not None:
                    regs[rd] = ("c", w)
                return
        if m in ("mov", "movs") and len(ops) == 2:
            if ops[1].startswith("#"):
                imm = parse_imm(ops[1])
                regs.pop(rd, None)
                if imm is not None:
                    regs[rd] = ("c", imm & 0xFFFFFFFF)
            else:
                v = regs.get(reg(ops[1]))
                regs.pop(rd, None)
                if v:
                    regs[rd] = v
                elif reg(ops[1]) == "lr":
                    regs[rd] = ("lr", 0)   # copia de lr: `mov ip,lr ... bx ip` e retorno
            return
        if m == "adr" and ins.thumb and len(ops) == 2 and ops[1].startswith("#"):
            imm = parse_imm(ops[1])
            regs.pop(rd, None)
            if imm is not None:
                regs[rd] = ("c", ((addr + 4) & ~3) + imm)  # veneer Thumb->ARM: adr rN,#0; bx rN
            return
        if m == "add" and not ins.thumb and len(ops) == 3 and reg(ops[1]) == "pc" and ops[2].startswith("#"):
            imm = parse_imm(ops[2])
            if imm is not None and rd != "pc":
                tgt = (addr + 8 + imm) & 0xFFFFFFFF
                if self.off(tgt) is not None:
                    self.adr_seeds.append(tgt)
                    regs[rd] = ("c", tgt)
                    return
        fold = self.fold(m, ops, regs)
        if fold is not None:
            regs.pop(rd, None)
            if fold is not False:
                regs[rd] = ("c", fold & 0xFFFFFFFF)
            return
        if m in ("add", "adds") and len(ops) in (2, 3) and not ops[-1].startswith("#"):
            a, b = (reg(ops[0]), reg(ops[1])) if len(ops) == 2 else (reg(ops[1]), reg(ops[2]))
            for x in (a, b):
                v = regs.get(x)
                if v and v[0] == "c" and ROM_BASE <= v[1] < ROM_BASE + self.n:
                    regs[rd] = ("t", v[1])
                    return
            regs.pop(rd, None)
            return
        if m == "ldr" and len(ops) >= 2:
            mm = re.match(r"\[(\w+)(?:,\s*#0(?:x0)?)?\]$", ops[1])
            if mm:
                v = regs.get(reg(mm.group(1)))
                regs.pop(rd, None)
                if v and v[0] == "t":
                    regs[rd] = ("e", v[1])
                return
        if m in WRITES or m.startswith("ldr"):
            regs.pop(rd, None)

    @staticmethod
    def fold(m, ops, regs):
        """Dobra constantes em ALU simples. None = nao e dobravel aqui; False = resultado desconhecido."""
        def cv(o):
            if o.startswith("#"):
                return parse_imm(o)
            v = regs.get(reg(o))
            return v[1] if v and v[0] == "c" else None
        n = len(ops)
        if m in ("add", "adds", "sub", "subs", "and", "ands", "orr", "orrs", "eor", "eors", "bic", "bics",
                 "lsl", "lsls", "lsr", "lsrs") and n in (2, 3):
            if m in ("add", "adds", "sub", "subs") and n >= 2 and not ops[-1].startswith("#"):
                return None  # add reg,reg: tratado em track (tabelas)
            a, b = (cv(ops[0]), cv(ops[1])) if n == 2 else (cv(ops[1]), cv(ops[2]))
            if a is None or b is None:
                return False
            k = m.rstrip("s") if m not in ("bics", "ands") else m[:-1]
            if k in ("add",):
                return a + b
            if k == "sub":
                return a - b
            if k == "and":
                return a & b
            if k == "orr":
                return a | b
            if k == "eor":
                return a ^ b
            if k == "bic":
                return a & ~b
            if k == "lsl":
                return a << b if b < 32 else 0
            if k == "lsr":
                return a >> b if b < 32 else 0
        if m in ("rsb", "rsbs") and n == 3 and ops[2] == "#0":
            a = cv(ops[1])
            return False if a is None else -a
        if m in ("neg", "negs") and n == 2:
            a = cv(ops[1])
            return False if a is None else -a
        if m in ("mvn", "mvns") and n == 2:
            a = cv(ops[1])
            return False if a is None else ~a
        return None

    # ---- tabelas de salto ----
    def add_table(self, ins, base, last_cmp, work):
        limit = 64 if last_cmp is None else last_cmp + 1
        targets, a = [], base
        while len(targets) < limit:
            w = self.word(a)
            if w is None:
                break
            t = w & ~1 if ins.thumb else w & ~3
            ok = self.off(t) is not None and abs(t - ins.addr) < 0x4000 and (w & 1 == 0 or ins.thumb)
            if not ok:
                break
            targets.append(t)
            a += 4
        if not targets:
            self.unresolved.append((ins.addr, ins.mnem + " <tabela vazia>"))
            return
        self.tables.append({"site": ins.addr, "base": base, "targets": targets, "thumb": ins.thumb})
        ins.kind = "ind"
        ins.succ.extend(targets)
        for t in targets:
            self.owner.setdefault(t, self.cur_owner)
            work.append((t, ins.thumb, "jumptable"))

    def add_arm_branch_table(self, ins, last_cmp, work):
        limit = 64 if last_cmp is None else last_cmp + 1
        targets, a = [], ins.addr + 8
        while len(targets) < limit:
            d = self.decode(a, False)
            if not d or split_branch(d.mnemonic)[0] != "b" or not d.op_str.startswith("#"):
                break
            targets.append(parse_imm(d.op_str))
            a += 4
        if targets:
            self.tables.append({"site": ins.addr, "base": ins.addr + 8, "targets": targets, "thumb": False,
                                "branch_table": True})
            ins.succ.extend(targets)
            ins.kind = "ind"
            for t in targets:
                work.append((t, False, "jumptable"))
            work.append((ins.addr + 8, False, "jumptable"))
        else:
            self.unresolved.append((ins.addr, "add pc,..."))

    # ---- heuristica: ponteiros para prologos ----
    def pointer_seeds(self, lo, hi):
        words = array("I")
        words.frombytes(bytes(self.rom[: self.n - self.n % 4]))
        end = ROM_BASE + self.n
        seeds = {}
        for i, w in enumerate(words):
            if not (ROM_BASE <= w < end) or not (lo <= w < hi) or w in self.insns:
                continue
            o = w - ROM_BASE
            if w & 1:
                if o + 1 < self.n and self.rom[o] == 0xB5:  # push {..,lr}: byte alto do halfword em w-1
                    seeds[w & ~1] = True
            elif w % 4 == 0 and o + 4 <= self.n:
                v = struct.unpack_from("<I", self.rom, o)[0]
                if (v & 0xFFFF0000) == 0xE92D0000 and v & 0x4000:  # stmdb sp!,{..,lr}
                    seeds[w] = False
        return [(w, is_thumb) for w, is_thumb in seeds.items()]

    # ---- `bl` Thumb usado como salto longo ----
    LONGJUMP_WINDOW = 0x2000

    def long_jump_reason(self, tgt):
        """Motivo (str) se `bl tgt` e continuacao da funcao atual; None se for chamada."""
        owner = self.cur_owner
        if owner is None or not (owner < tgt <= owner + self.LONGJUMP_WINDOW) or tgt & 1:
            return None
        if tgt not in self._lj_cache:
            self._lj_cache[tgt] = self.probe_continuation(tgt)
        return self._lj_cache[tgt]

    def probe_continuation(self, tgt):
        """Alvo sem `push` proprio que comeca com epilogo ou que retorna via pop => continuacao."""
        d = self.decode(tgt, True)
        if d is None or d.mnemonic == "push":
            return None
        ops0 = split_ops(d.op_str)
        first = None
        if d.mnemonic in ("add", "adds") and len(ops0) == 2 and ops0[0] == "sp" and ops0[1].startswith("#"):
            first = "starts-add-sp"
        elif d.mnemonic == "pop":
            first = "starts-pop"
        seen, stack = set(), [tgt]
        has_push = stack_ret = lr_ret = False
        while stack and len(seen) < 600:
            a = stack.pop()
            prev_pop = None
            while a not in seen and self.off(a) is not None and len(seen) < 600:
                seen.add(a)
                d = self.decode(a, True)
                if d is None:
                    break
                m, ops = d.mnemonic, split_ops(d.op_str)
                if m == "push":
                    has_push = True
                base, cond = split_branch(m)
                if base == "b" and ops and ops[0].startswith("#"):
                    stack.append(parse_imm(ops[0]))
                    if not cond:
                        break
                elif m == "bx":
                    if ops == ["lr"]:
                        lr_ret = True
                    elif ops and ops[0] == prev_pop:
                        stack_ret = True
                    break
                elif m == "pop" and ops and re.search(r"\bpc\b", ops[-1]):
                    stack_ret = True
                    break
                pm = re.fullmatch(r"\{(r\d+)\}", ops[0]) if (m == "pop" and ops) else None
                prev_pop = pm.group(1) if pm else None
                a += d.size
        if has_push:
            return None
        if stack_ret and not lr_ret:
            return "no-push+stack-return" + ("," + first if first else "")
        if first:
            return first
        return None

    # ---- codigo copiado da ROM para a RAM ----
    SWI_COPY = {0x0B: "CpuSet", 0x0C: "CpuFastSet", 0x11: "LZ77WRAM", 0x14: "RLWRAM"}

    def find_copy_routines(self):
        """addr -> nome: wrappers de SWI (`svc #n; bx lr`) e funcoes tipo memcpy (ldm/stm unitarios)."""
        out = {}
        for ins in self.insns.values():
            if ins.mnem in ("svc", "swi") and ins.ops and ins.thumb:
                nxt = self.insns.get(ins.addr + ins.size)
                name = self.SWI_COPY.get(parse_imm(ins.ops[0]))
                if name and nxt and nxt.mnem == "bx" and nxt.ops == ["lr"]:
                    out[ins.addr] = name
        for f in self.funcs:
            seen, stack, ld, st = set(), [f], False, False
            while stack and len(seen) < 400:
                x = stack.pop()
                if x in seen or x not in self.insns or (x != f and x in self.funcs):
                    continue
                seen.add(x)
                i = self.insns[x]
                ld |= i.mnem == "ldm" and i.thumb and i.ops[-1].count(",") == 0 and i.ops[0].endswith("!")
                st |= i.mnem == "stm" and i.thumb and i.ops[-1].count(",") == 0 and i.ops[0].endswith("!")
                stack.extend(i.succ)
            if ld and st:
                out.setdefault(f, "memcpy")
        return out

    def find_ram_code(self):
        """Chamadas CpuSet/CpuFastSet/LZ77/memcpy com origem em ROM e destino em EWRAM/IWRAM."""
        routines = self.find_copy_routines()
        seen = set()
        for site, tgt, r0, r1, r2 in self.call_args:
            kind = routines.get(tgt)
            if kind is None or (site, kind) in seen:
                continue
            seen.add((site, kind))
            if kind == "memcpy":
                dst, src, n, comp = r0, r1, r2, False
            else:
                dst, src, comp = r1, r0, kind in ("LZ77WRAM", "RLWRAM")
                if r0 is None or r1 is None:
                    continue
                if kind in ("CpuSet", "CpuFastSet"):
                    if r2 is None or r2 >> 24 & 1:      # r2 desconhecido ou fill
                        continue
                    unit = 4 if (kind == "CpuFastSet" or r2 >> 26 & 1) else 2
                    n = (r2 & 0x1FFFFF) * unit
                else:
                    hdr = self.word(src & ~1)
                    n = (hdr >> 8) if hdr is not None else None
            if None in (dst, src) or not (dst >> 24 in (2, 3)) or self.off(src & ~1) is None or not n:
                continue
            src &= ~1
            self.ram_blocks.append({
                "site": site, "method": kind, "store": src, "exec": dst & ~1, "size": n, "compressed": comp,
            })
        self.ram_blocks.sort(key=lambda b: b["site"])
        for b in self.ram_blocks:
            if b["compressed"]:
                b["mode"] = "compressed"
                continue
            o = b["store"] - ROM_BASE
            ws = struct.unpack_from("<%dI" % (b["size"] // 4), self.rom, o)
            al = sum(1 for w in ws if w >> 28 == 0xE)  # ARM: quase tudo com cond=AL
            frac = al / len(ws) if ws else 0
            b["mode"] = "arm" if frac >= 0.6 else "thumb" if frac < 0.2 else "mixed"  # mixed = Thumb + ARM
        return self.ram_blocks

    def ram_to_rom(self, addr):
        for b in self.ram_blocks:
            if not b["compressed"] and b["exec"] <= addr < b["exec"] + b["size"]:
                return b["store"] + (addr - b["exec"])
        return None

    def noreturn_kind(self, f, seen, tails, src, in_ram):
        """Classifica funcoes sem `ret` alcancavel; '' se tem retorno; 'suspect' se nada explica."""
        if any(self.insns[x].kind == "ret" for x in seen):
            return ""
        ins = [self.insns[x] for x in sorted(seen)]
        if f == ROM_BASE:
            return "header-branch"
        if src == "entry":
            return "crt0"
        if src == "adr":
            return "irq-handler"
        if in_ram:
            return "ram-code"
        if len(ins) == 1 and ins[0].mnem == "bx":
            return "bx-stub"
        if len(ins) == 2 and ins[0].mnem == "adr" and ins[1].mnem == "bx":
            return "mode-veneer"
        if any(i.mnem in ("svc", "swi") and i.ops and i.ops[0] in ("#0", "#1", "#3") for i in ins):
            return "softreset-stop"
        if tails:
            return "tail-call"
        loops = [i for i in ins if i.kind == "jump" and not i.cond and i.target is not None
                 and i.target <= i.addr and i.target in seen]
        if loops:
            return "infinite-loop"
        return "suspect"

    # ---- funcoes ----
    def build_functions(self):
        res = []
        callers = defaultdict(set)
        for ins in self.insns.values():
            for t in ins.calls:
                callers[t].add(ins.addr)
        for f, (thumb, src) in sorted(self.funcs.items()):
            if f not in self.insns:
                continue
            seen, stack, calls, tails = set(), [f], set(), set()
            while stack:
                a = stack.pop()
                if a in seen or a not in self.insns:
                    continue
                if a != f and a in self.funcs and f not in self.cont.get(a, ()):
                    tails.add(a)   # salta/cai em outra funcao (tail call ou fallthrough)
                    continue
                seen.add(a)
                ins = self.insns[a]
                calls.update(ins.calls)
                stack.extend(ins.succ)
            if not seen:
                continue
            nbytes = sum(self.insns[a].size for a in seen)
            lo, hi = min(seen), max(seen) + self.insns[max(seen)].size
            has_tbl = any(t["site"] in seen for t in self.tables)
            exec_addr, block = "", ""
            for k, b in enumerate(self.ram_blocks):
                if not b["compressed"] and b["store"] <= f < b["store"] + b["size"]:
                    exec_addr, block = b["exec"] + (f - b["store"]), k
                    break
            res.append({
                "addr": f, "mode": "thumb" if thumb else "arm", "insns": len(seen), "bytes": nbytes,
                "lo": lo, "hi": hi, "callers": len(callers.get(f, ())), "callees": len(calls),
                "jumptable": has_tbl, "source": src,
                "has_return": any(self.insns[x].kind == "ret" for x in seen),
                "tails": len(tails),
                "noreturn_kind": self.noreturn_kind(f, seen, tails, src, exec_addr != ""),
                "continuations": sum(1 for t, o in self.cont.items() if f in o and t in seen), "tier": self.func_tier.get(f, ""),
                "exec_addr": exec_addr, "ram_block": block,
            })
        return res

    def coverage(self):
        cov = bytearray(self.n)  # 0 dados, 1 ARM, 2 Thumb, 3 tabela, 4 pool
        for ins in self.insns.values():
            o = ins.addr - ROM_BASE
            cov[o:o + ins.size] = bytes([2 if ins.thumb else 1]) * ins.size
        for t in self.tables:
            if t.get("branch_table"):
                continue
            o = t["base"] - ROM_BASE
            for k in range(len(t["targets"]) * 4):
                if o + k < self.n and cov[o + k] == 0:
                    cov[o + k] = 3
        for a in self.pool:
            o = a - ROM_BASE
            if 0 <= o and o + 4 <= self.n:
                for k in range(4):
                    if cov[o + k] == 0:
                        cov[o + k] = 4
        return cov


# ---- header ----
def parse_header(rom):
    h = {}
    w = struct.unpack_from("<I", rom, 0)[0]
    h["entry_instruction"] = f"0x{w:08X}"
    if (w >> 24) & 0xFF == 0xEA:
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 1 << 24
        h["entry_branch_target"] = ROM_BASE + 8 + (imm << 2)
    else:
        h["entry_branch_target"] = None
    h["logo_ok"] = bytes(rom[4:12]) == bytes.fromhex("24FFAE51699AA221")
    txt = lambda b: bytes(b).split(b"\0")[0].decode("ascii", "replace")
    h["title"] = txt(rom[0xA0:0xAC])
    h["game_code"] = txt(rom[0xAC:0xB0])
    h["maker_code"] = txt(rom[0xB0:0xB2])
    h["fixed_96"] = rom[0xB2] == 0x96
    h["unit_code"] = rom[0xB3]
    h["device_type"] = rom[0xB4]
    h["version"] = rom[0xBC]
    chk = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    h["complement_check_stored"] = rom[0xBD]
    h["complement_check_computed"] = chk
    h["header_checksum_ok"] = chk == rom[0xBD]
    h["rom_size"] = len(rom)
    return h


def runs(cov):
    names = {0: "data", 1: "arm", 2: "thumb", 3: "jumptable", 4: "literal_pool"}
    out, start = [], 0
    for i in range(1, len(cov) + 1):
        if i == len(cov) or cov[i] != cov[start]:
            out.append((ROM_BASE + start, ROM_BASE + i, names[cov[start]]))
            start = i
    return out


REPO = Path(__file__).resolve().parent.parent
DEFAULT_EXTRAS = [
    str(REPO / "build/indirect/new_functions.csv"),         # folhas de ponteiro (Riven)
    str(REPO / "build/indirect/m4a_handlers.csv"),          # handlers do M4A (tabela de saltos)
    str(REPO / "build/recomp/program/extra_functions.csv"), # descobertas em execucao (runtime do Nasus)
]


def read_extra_rows(path):
    """Linhas {addr:int, mode, source, tier}. Aceita com cabecalho ou sem (addr[,mode])."""
    with open(path, newline="") as f:
        rows = [r for r in csv.reader(f) if r and r[0].strip()]
    if rows and rows[0][0].strip().lower() == "addr":
        head = [h.strip() for h in rows[0]]
        recs = [dict(zip(head, r)) for r in rows[1:]]
    else:
        recs = [{"addr": r[0], "mode": r[1] if len(r) > 1 else "thumb"} for r in rows]
    out = []
    for r in recs:
        out.append({"addr": int(r["addr"].strip(), 16), "mode": (r.get("mode") or "thumb").strip().lower(),
                    "source": (r.get("source") or "runtime").strip(), "tier": (r.get("tier") or "").strip()})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="?", help="caminho da ROM (padrao: baserom.gba ou o unico *.gba da pasta)")
    ap.add_argument("-o", "--out", default="build/analysis")
    ap.add_argument("--extra-functions", action="append", default=None, metavar="CSV",
                    help="CSV de funcoes extras (addr,mode,source,tier; ou sem cabecalho: addr,mode); "
                         "repetivel. Padrao: new_functions.csv, m4a_handlers.csv e o "
                         "extra_functions.csv que o runtime grava. '' desliga")
    ap.add_argument("--confidence", default="build/indirect/confidence.csv",
                    help="CSV addr,confidence (tools/score_functions.py); '' desliga")
    ap.add_argument("--no-ptr-scan", action="store_true", help="nao usar a heuristica de ponteiros")
    args = ap.parse_args()

    rom_path = Path(args.rom) if args.rom else None
    if rom_path is None:
        cands = [Path("baserom.gba")] if Path("baserom.gba").exists() else sorted(Path(".").glob("*.gba"))
        if len(cands) != 1:
            sys.exit("ROM nao encontrada: passe o caminho como argumento")
        rom_path = cands[0]
    rom = rom_path.read_bytes()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    hdr = parse_header(rom)
    print(f"ROM: {rom_path.name}  ({len(rom) / 1048576:.1f} MiB)")
    print(f"  titulo={hdr['title']!r} codigo={hdr['game_code']} fabricante={hdr['maker_code']} versao={hdr['version']}")
    print(f"  logo ok={hdr['logo_ok']}  0x96 fixo={hdr['fixed_96']}  checksum do header ok={hdr['header_checksum_ok']}")
    entry = hdr["entry_branch_target"]
    if entry is None:
        sys.exit("primeira instrucao nao e um B: entry point desconhecido")
    print(f"  entry point: 0x{ROM_BASE:08X} -> B 0x{entry:08X} (ARM)")

    a = Analyzer(rom)
    a.add_func(ROM_BASE, False, "entry")
    a.add_func(entry, False, "entry")
    a.run([(ROM_BASE, False, "entry")])
    n1 = len(a.insns)
    lo = min(a.insns)
    hi = max(a.insns)
    print(f"  passo 1 (sem heuristica): {n1} instrucoes, {len(a.funcs)} funcoes, "
          f"codigo entre 0x{lo:08X} e 0x{hi:08X}")

    if not args.no_ptr_scan:
        seeds = a.pointer_seeds(max(ROM_BASE, lo - 0x10000), hi + 0x10000)
        for addr, thumb in seeds:
            if a.add_func(addr, thumb, "ptr-scan"):
                a.run([(addr, thumb, "ptr-scan")])
        print(f"  passo 2 (ponteiros p/ prologos): +{len(a.insns) - n1} instrucoes "
              f"de {len(seeds)} candidatos")

    extra_files = DEFAULT_EXTRAS if args.extra_functions is None else args.extra_functions
    if "" in extra_files:
        extra_files = []
    for path in extra_files:
        if not Path(path).exists():
            continue  # ex.: o runtime ainda nao gravou extra_functions.csv
        added = known = inside = 0
        for row in read_extra_rows(path):
            addr, thumb = row["addr"], row["mode"] == "thumb"
            if addr in a.funcs:
                known += 1
                continue
            own = a.insn_owner.get(addr)
            if row["source"] == "addr-taken-leaf" and addr in a.insns and own is not None and own != addr:
                # ponteiro "solto" que cai dentro de outra funcao: continuacao, nao funcao nova.
                # (fontes confiaveis - jump table do M4A, runtime - sao sempre entradas de funcao)
                a.extra_conts.append((addr, own))
                a.cont.setdefault(addr, {})[own] = "addr-taken-inside-parent"
                inside += 1
                continue
            if a.add_func(addr, thumb, row["source"]):
                if row.get("tier"):
                    a.func_tier[addr] = row["tier"]
                a.run([(addr, thumb, row["source"])])
                added += 1
        print(f"  extras ({Path(path).name}): +{added} funcoes novas, {known} ja conhecidas"
              + (f", {inside} dentro de outra funcao" if inside else ""))

    # codigo ROM -> RAM: acha as rotinas de copia e segue o codigo armazenado na ROM
    blocks = a.find_ram_code()
    n0, f0 = len(a.insns), len(a.funcs)
    for addr, thumb in sorted(a.ram_entries):
        rom_addr = a.ram_to_rom(addr)
        if rom_addr is not None and a.add_func(rom_addr, thumb, "ram-code"):
            a.run([(rom_addr, thumb, "ram-code")])
    words = array("I")
    words.frombytes(bytes(rom[: len(rom) - len(rom) % 4]))
    for b in blocks:  # blocos ARM: o inicio da copia e uma funcao
        if b["mode"] == "arm" and a.add_func(b["store"], False, "ram-code"):
            a.run([(b["store"], False, "ram-code")])
    for idx, w in enumerate(words):  # ponteiros para dentro do codigo em RAM: tabelas de callbacks
        if w >> 24 not in (2, 3):
            continue
        waddr = ROM_BASE + idx * 4
        if waddr in a.insns or waddr in a.pool or any(
                b["store"] <= waddr < b["store"] + b["size"] for b in blocks):
            continue  # instrucao, literal pool ou o proprio codigo guardado: nao e tabela de ponteiros
        rom_addr = a.ram_to_rom(w & ~1)
        if rom_addr is None:
            continue
        blk = next(b for b in blocks if b["store"] <= rom_addr < b["store"] + b["size"])
        thumb = bool(w & 1)
        if (blk["mode"] == "thumb" and thumb) or (blk["mode"] == "arm" and not thumb and w % 4 == 0):
            if a.add_func(rom_addr, thumb, "ram-ptr"):
                a.run([(rom_addr, thumb, "ram-ptr")])
    print(f"  codigo RAM: {len(blocks)} copias ROM->RAM; seguindo o codigo guardado: "
          f"+{len(a.insns) - n0} instrucoes, +{len(a.funcs) - f0} funcoes")

    conf = {}
    if args.confidence and Path(args.confidence).exists():
        with open(args.confidence, newline="") as f:
            for row in csv.DictReader(f):
                conf[row["addr"].upper().replace("0X", "0x")] = row["confidence"]
        # confidence.csv usa 0x08xxxxxx com digitos em maiusculas ou minusculas: normaliza
        conf = {f"0x{int(k, 16):08X}": v for k, v in conf.items()}
    funcs = a.build_functions()
    for r in funcs:
        r["confidence"] = conf.get(f"0x{r['addr']:08X}", "unscored")
    cov = a.coverage()
    cnt = Counter(cov)
    n_arm = sum(1 for i in a.insns.values() if not i.thumb)
    n_thumb = len(a.insns) - n_arm

    # ---- relatorios ----
    (out / "header.json").write_text(json.dumps(hdr, indent=2), encoding="utf-8")
    with open(out / "functions.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "mode", "insns", "bytes", "lo", "hi", "callers", "callees", "jumptable", "source", "tier", "confidence",
                    "store_addr", "exec_addr", "ram_block", "has_return", "continuations", "noreturn_kind"])
        for r in funcs:
            w.writerow([f"0x{r['addr']:08X}", r["mode"], r["insns"], r["bytes"], f"0x{r['lo']:08X}",
                        f"0x{r['hi']:08X}", r["callers"], r["callees"], int(r["jumptable"]), r["source"], r["tier"],
                        r["confidence"],
                        f"0x{r['addr']:08X}" if r["exec_addr"] != "" else "",
                        f"0x{r['exec_addr']:08X}" if r["exec_addr"] != "" else "", r["ram_block"],
                        int(r["has_return"]), r["continuations"], r["noreturn_kind"]])
    with open(out / "noreturn.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "mode", "bytes", "source", "kind"])
        for r in funcs:
            if r["noreturn_kind"]:
                w.writerow([f"0x{r['addr']:08X}", r["mode"], r["bytes"], r["source"], r["noreturn_kind"]])
    with open(out / "continuations.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr_filho", "addr_pai", "motivo"])
        for t in sorted(a.cont):
            for owner, why in sorted(a.cont[t].items()):
                w.writerow([f"0x{t:08X}", f"0x{owner:08X}", why])
    with open(out / "jumptables.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["site", "table", "entries", "mode", "targets"])
        for t in a.tables:
            w.writerow([f"0x{t['site']:08X}", f"0x{t['base']:08X}", len(t["targets"]),
                        "thumb" if t["thumb"] else "arm", " ".join(f"0x{x:08X}" for x in t["targets"])])
    with open(out / "regions.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "end", "size", "kind"])
        for s, e, k in runs(cov):
            w.writerow([f"0x{s:08X}", f"0x{e:08X}", e - s, k])
    with open(out / "unresolved.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["addr", "instruction"])
        for ad, txt in a.unresolved:
            w.writerow([f"0x{ad:08X}", txt])

    sorted_f = sorted(r["addr"] for r in funcs)
    with open(out / "ram_code.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["block", "method", "call_site", "caller_function", "store_rom", "store_end", "exec_ram",
                    "exec_end", "size", "compressed", "mode", "functions_inside", "entry_points_exec"])
        for k, b in enumerate(blocks):
            k_f = bisect.bisect_right(sorted_f, b["site"]) - 1
            caller_fn = sorted_f[k_f] if k_f >= 0 else None
            inside = [r for r in funcs if r["ram_block"] == k]
            entries = sorted(e for e, _ in a.ram_entries if b["exec"] <= e < b["exec"] + b["size"])
            w.writerow([k, b["method"], f"0x{b['site']:08X}",
                        f"0x{caller_fn:08X}" if caller_fn is not None else "",
                        f"0x{b['store']:08X}", f"0x{b['store'] + b['size']:08X}", f"0x{b['exec']:08X}",
                        f"0x{b['exec'] + b['size']:08X}", b["size"], int(b["compressed"]), b["mode"], len(inside),
                        " ".join(f"0x{e:08X}" for e in entries)])
    map_json = {
        "schema": "kirby-amm-map/1", "rom": rom_path.name, "rom_base": ROM_BASE, "header": hdr,
        "functions": [{
            "addr": r["addr"], "mode": r["mode"], "lo": r["lo"], "hi": r["hi"], "insns": r["insns"],
            "bytes": r["bytes"], "source": r["source"], "tier": r["tier"] or None,
            "confidence": r["confidence"], "jumptable": r["jumptable"], "has_return": r["has_return"],
            "continuations": r["continuations"], "noreturn_kind": r["noreturn_kind"] or None,
            "store_addr": r["addr"] if r["exec_addr"] != "" else None,
            "exec_addr": r["exec_addr"] if r["exec_addr"] != "" else None,
            "ram_block": r["ram_block"] if r["ram_block"] != "" else None,
        } for r in funcs],
        "regions": [{"start": s0, "end": e0, "size": e0 - s0, "kind": k0} for s0, e0, k0 in runs(cov)],
        "ram_code": [{"block": k, **b} for k, b in enumerate(blocks)],
        "continuations": [{"child": t, "parent": o, "why": w} for t, d in sorted(a.cont.items())
                          for o, w in sorted(d.items())],
        "jump_tables": [{"site": t["site"], "table": t["base"], "mode": "thumb" if t["thumb"] else "arm",
                         "targets": t["targets"]} for t in a.tables],
        "unresolved_indirect": [{"addr": ad, "insn": txt} for ad, txt in a.unresolved],
    }
    (out / "map.json").write_text(json.dumps(map_json, separators=(",", ":")), encoding="utf-8")

    mb = lambda c: f"{c / 1024:9.1f} KiB ({100 * c / len(rom):4.1f}%)"
    print("\nResumo")
    print(f"  instrucoes: {n_arm} ARM, {n_thumb} Thumb")
    print(f"  bytes ARM          {mb(cnt[1])}")
    print(f"  bytes Thumb        {mb(cnt[2])}")
    print(f"  bytes lit. pool    {mb(cnt[4])}")
    print(f"  bytes tab. salto   {mb(cnt[3])}")
    print(f"  bytes dados/outros {mb(cnt[0])}")
    by_src = Counter(r["source"] for r in funcs)
    by_mode = Counter(r["mode"] for r in funcs)
    print(f"  funcoes: {len(funcs)}  ({dict(by_mode)})  por origem: {dict(by_src)}")
    print(f"  tabelas de salto: {len(a.tables)} ({sum(len(t['targets']) for t in a.tables)} entradas)")
    print(f"  saltos/chamadas indiretos nao resolvidos: {len(a.unresolved)}")
    print(f"  chamadas para RAM (codigo copiado p/ IWRAM/EWRAM?): {sum(a.ram_targets.values())} "
          f"em {len(a.ram_targets)} enderecos")
    print(f"  falhas de decodificacao: {len(a.bad)}  conflitos ARM/Thumb: {a.mode_conflicts}")
    print(f"  confianca: {dict(Counter(r['confidence'] for r in funcs))}")
    print(f"  codigo RAM: {len(blocks)} copias, {sum(1 for r in funcs if r['ram_block'] != '')} funcoes dentro")
    print(f"  continuacoes (bl como salto longo / extras dentro de funcao): "
          f"{sum(len(d) for d in a.cont.values())}  (extras absorvidos: {len(a.extra_conts)})")
    print(f"  funcoes sem retorno alcancavel: {sum(1 for r in funcs if not r['has_return'])}  "
          f"{dict(Counter(r['noreturn_kind'] for r in funcs if r['noreturn_kind']))}")
    print(f"\nArquivos em {out}/: header.json functions.csv jumptables.csv regions.csv unresolved.csv "
          f"ram_code.csv map.json continuations.csv")


if __name__ == "__main__":
    main()
