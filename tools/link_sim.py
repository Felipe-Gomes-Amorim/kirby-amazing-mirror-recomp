"""Bancada de execucao do link do KatAM: roda o codigo REAL da ROM (gLink + lockstep) em N "GBAs" Unicorn
ligados por um modelo de cabo multi-play escrito em Python (Poppy, P0b).

Nao e um emulador de GBA: so ROM/IWRAM/EWRAM, I/O minimo (SIO, Timer3, IE/IF/IME), BIOS CpuSet/CpuFastSet por HLE
e o despacho de IRQ pelo vetor 0 (0x030017B0), exatamente o que as funcoes do link usam. Serve para:
  - decodificar pacotes (cada halfword enviado/recebido fica registrado);
  - testar de forma deterministica o que o jogo faz (soma 0xFFF1, ordem das 15 trocas, jogador ausente...);
  - forcar os erros 3-8 de LinkFrameSync.
Precisa de `unicorn` (esta no .venv do projeto) e da ROM. Uso interativo: veja tests/test_link_protocol.py.
"""
import os
import struct

from unicorn import (UC_ARCH_ARM, UC_HOOK_INTR, UC_MODE_ARM, UC_PROT_ALL, UC_PROT_EXEC, UC_PROT_READ, Uc)
from unicorn import arm_const as AC

ROM_BASE, IWRAM, EWRAM, IO = 0x08000000, 0x03000000, 0x02000000, 0x04000000
RET_ADDR = 0x0A000000          # endereco-sentinela de retorno (nunca executado)
STACK_TOP = IWRAM + 0x7E00

# funcoes do jogo (ROM)
LINK_ENABLE, LINK_INIT, LINK_MAIN, LINK_STOP = 0x0815898C, 0x08157210, 0x08157308, 0x081576BC
LINK_START_TIMER = 0x081576A0   # gLink[8] |= 0x80 se mestre: habilita a cadeia do Timer3 (o lobby chama 1 vez)
BUILD_INPUT, FRAME_SYNC, CLEAR_LOCKSTEP = 0x08030E44, 0x08030FE0, 0x08031BFC
# memoria do jogo
G_FLAGS, G_LINKON, G_STATUS = 0x03002440, 0x03002558, 0x03002554
G_SEND, G_RECV, GLINK = 0x030036B0, 0x03002490, 0x03006A60
LS = 0x020382D0                # estado do lockstep
LS_ERR, N_PLAYERS, MY_ID, KEYS_NOW = 0x02038580, 0x0203AD30, 0x0203AD3C, 0x03002E90
FEFE = 0xFEFE
ABSENT = 0xFFFF


class Gba:
    """Um console: CPU Unicorn + I/O do SIO."""

    def __init__(self, rom, cable, slot):
        self.cable, self.slot = cable, slot
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        u = self.uc
        size = (len(rom) + 0xFFF) & ~0xFFF
        u.mem_map(ROM_BASE, size, UC_PROT_READ | UC_PROT_EXEC)
        u.mem_write(ROM_BASE, rom)
        u.mem_map(IWRAM, 0x8000, UC_PROT_ALL)
        u.mem_map(EWRAM, 0x40000, UC_PROT_ALL)
        u.mmio_map(IO, 0x1000, self._io_read, None, self._io_write, None)
        self.io = bytearray(0x1000)               # registradores "comuns" (armazenamento simples)
        self.siocnt = 0
        self.send = 0
        self.multi = [ABSENT] * 4
        self.rcnt = 0x8000
        self.busy = self.err = 0
        self.id = 0
        self.irq_depth = 0
        self.start_pending = False
        self.sent_log = []        # (indice_da_troca_no_quadro, valor) de cada SIOMLT_SEND LIDO pelo cabo
        self.irq_calls = 0
        u.hook_add(UC_HOOK_INTR, self._intr)

    # ------------------------------------------------------------------ memoria
    def r8(self, a):
        return self.uc.mem_read(a, 1)[0]

    def r16(self, a):
        return struct.unpack('<H', bytes(self.uc.mem_read(a, 2)))[0]

    def r32(self, a):
        return struct.unpack('<I', bytes(self.uc.mem_read(a, 4)))[0]

    def w8(self, a, v):
        self.uc.mem_write(a, bytes([v & 0xFF]))

    def w16(self, a, v):
        self.uc.mem_write(a, struct.pack('<H', v & 0xFFFF))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def rb(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def wb(self, a, data):
        self.uc.mem_write(a, bytes(data))

    # ------------------------------------------------------------------ I/O
    def _siocnt_read(self):
        v = self.siocnt & 0x7003
        if self.slot != 0:
            v |= 0x04
        if self.cable.sd(self):
            v |= 0x08
        v |= self.id << 4
        if self.err:
            v |= 0x40
        if self.busy:
            v |= 0x80
        return v

    def _io_read(self, uc, off, size, _):
        reg = off & ~3
        if 0x120 <= off < 0x128:
            word = [self.multi[(reg - 0x120) // 2] | (self.multi[(reg - 0x120) // 2 + 1] << 16)]
            v = word[0] >> (8 * (off - reg))
        elif 0x128 <= off < 0x12C:
            word = self._siocnt_read() | (self.send << 16)
            v = word >> (8 * (off - 0x128))
        elif off in (0x134, 0x135):
            v = self.rcnt >> (8 * (off - 0x134))
        else:
            v = int.from_bytes(self.io[off:off + size], 'little')
            return v
        return v & ((1 << (8 * size)) - 1)

    def _io_write(self, uc, off, size, value, _):
        mask = (1 << (8 * size)) - 1
        value &= mask
        # compoe o valor de 32 bits "visto" pelo registrador e aplica por campo
        if 0x128 <= off < 0x12C:
            cur = self._siocnt_read() | (self.send << 16)
            shift = 8 * (off - 0x128)
            new = (cur & ~(mask << shift)) | (value << shift)
            self._siocnt_write(new & 0xFFFF, bool(new & 0x80), size, off)
            self.send = (new >> 16) & 0xFFFF
            return
        if off in (0x134, 0x135):
            shift = 8 * (off - 0x134)
            self.rcnt = (self.rcnt & ~(mask << shift) | (value << shift)) & 0xFFFF
            return
        if 0x120 <= off < 0x128:
            return                                   # somente leitura em multi-play
        if off in (0x202, 0x203):                    # IF: w1c
            old = int.from_bytes(self.io[0x202:0x204], 'little')
            old &= ~(value << (8 * (off - 0x202)))
            self.io[0x202:0x204] = old.to_bytes(2, 'little')
            return
        self.io[off:off + size] = value.to_bytes(size, 'little')

    def _siocnt_write(self, v, start_bit, size, off):
        multi_before = self.multi_mode()
        self.siocnt = v & 0x7003
        if not self.multi_mode():
            return
        if not multi_before:
            self.err, self.id, self.busy = 0, 0, 0
            self.multi = [ABSENT] * 4
        if start_bit and self.slot == 0 and not self.busy:
            self.start_pending = True
            self.busy = 1
            self.err = 0

    def multi_mode(self):
        return ((self.siocnt >> 12) & 3) == 2 and not (self.rcnt & 0x8000)

    # registradores comuns para o cabo
    def reg16(self, off):
        return int.from_bytes(self.io[off:off + 2], 'little')

    def set_if(self, bit):
        self.io[0x202:0x204] = (self.reg16(0x202) | bit).to_bytes(2, 'little')

    # ------------------------------------------------------------------ BIOS HLE
    def _intr(self, uc, intno, _):
        if intno != 2:
            return
        pc = uc.reg_read(AC.UC_ARM_REG_PC)
        cpsr = uc.reg_read(AC.UC_ARM_REG_CPSR)
        thumb = bool(cpsr & 0x20)
        num = self.r8(pc - 2) if thumb else (self.r32(pc - 4) & 0xFFFFFF) >> 16
        r0, r1, r2 = (uc.reg_read(x) for x in (AC.UC_ARM_REG_R0, AC.UC_ARM_REG_R1, AC.UC_ARM_REG_R2))
        if num in (0x0B, 0x0C):
            self._cpuset(r0, r1, r2, fast=(num == 0x0C))
        else:
            raise RuntimeError(f'SWI 0x{num:02X} nao implementado (pc=0x{pc:08X})')

    def _cpuset(self, src, dst, ctl, fast):
        count = ctl & 0x1FFFFF
        fill = bool(ctl & (1 << 24))
        word = fast or bool(ctl & (1 << 26))
        n = 4 if word else 2
        if fast:
            count = (count + 7) & ~7
        if fill:
            val = self.r32(src) if word else self.r16(src)
            data = val.to_bytes(n, 'little') * count
        else:
            data = self.rb(src, count * n)
        self.wb(dst, data)

    # ------------------------------------------------------------------ chamadas
    def call(self, addr, *args, thumb=True, max_insns=2_000_000):
        u = self.uc
        for i, a in enumerate(args):
            u.reg_write(AC.UC_ARM_REG_R0 + i, a & 0xFFFFFFFF)
        u.reg_write(AC.UC_ARM_REG_SP, STACK_TOP - 0x400 * self.irq_depth)
        u.reg_write(AC.UC_ARM_REG_LR, RET_ADDR | 1)
        self.irq_depth += 1
        try:
            u.emu_start(addr | (1 if thumb else 0), RET_ADDR, count=max_insns)
        finally:
            self.irq_depth -= 1
        return u.reg_read(AC.UC_ARM_REG_R0)

    def deliver_irq(self, bit):
        """Despacha uma IRQ pelo vetor 0 (0x030017B0), como o crt0 do jogo faz para IF bits 6/7."""
        if not (self.reg16(0x208) & 1) or not (self.reg16(0x200) & bit):
            return False
        vec = self.r32(IWRAM + 0x17B0)
        if vec == 0:
            return False
        self.irq_calls += 1
        self.call(vec & ~1, thumb=bool(vec & 1))
        return True


class Cable:
    """Modelo do cabo multi-play: unidade 0 = mestre. Trocas instantaneas, ordem = indice do slot."""

    def __init__(self, rom, n_units, present=None):
        self.units = [Gba(rom, self, i) for i in range(n_units)]
        self.present = list(present) if present is not None else [True] * n_units
        self.exchange_count = 0
        self.log = []              # por troca: dict(sent=[4], got=[4]) na ordem do cabo

    def sd(self, unit):
        """SD = pelo menos 2 unidades presentes e TODAS as presentes em multi-play."""
        here = [u for i, u in enumerate(self.units) if self.present[i]]
        return len(here) >= 2 and all(u.multi_mode() for u in here)

    def _exchange(self):
        master = self.units[0]
        master.start_pending = False
        sent = []
        for i in range(4):
            if i < len(self.units) and self.present[i] and self.units[i].multi_mode():
                sent.append(self.units[i].send)
            else:
                sent.append(ABSENT)
        for i, u in enumerate(self.units):
            if not self.present[i]:
                continue
            u.multi = list(sent)
            u.id = i
            u.busy = 0
            u.err = 0
        self.exchange_count += 1
        self.log.append({'sent': list(sent), 'got': list(sent)})
        for i, u in enumerate(self.units):
            if self.present[i] and (i != 0 or True) and (u.siocnt & 0x4000):
                u.deliver_irq(0x80)                      # serial IRQ (so se SIOCNT.bit14 e IE.bit7)

    def run_exchanges(self, max_ticks=16):
        """Um 'tick' = um periodo do Timer3 do mestre (1,04 ms). Em cada tick: (1) estouro do Timer3 -> handler
        (le a troca anterior, escolhe o proximo SIOMLT_SEND, reinicia a cadeia); (2) a troca pedida acontece e
        os filhos recebem a IRQ serial. Sem Timer3 ligado, so ha troca se o mestre pediu Start."""
        master = self.units[0]
        done = 0
        for _ in range(max_ticks):
            if not self.present[0]:
                break
            tm3_h = master.reg16(0x10E)
            timer_on = (tm3_h & 0xC0) == 0xC0
            if timer_on:
                master.deliver_irq(0x40)
            if master.start_pending:
                self._exchange()
                done += 1
            elif not timer_on:
                break
        return done


class Session:
    """N consoles ja inicializados como o jogo faz (LinkEnable) e prontos para rodar quadros."""

    def __init__(self, rom, n_units=2, present=None, link_flags=0x80000):
        self.cable = Cable(rom, n_units, present)
        self.units = self.cable.units
        for u in self.units:
            u.w32(G_FLAGS, u.r32(G_FLAGS) | link_flags)
            u.call(LINK_ENABLE)
        self.frame = 0
        self.status = [0] * n_units
        self.timer_started = False
        self.lockstep = False
        self.sync_ok = [1] * n_units

    # ---- camada lockstep (BuildInputPacket + LinkFrameSync, codigo real) ----
    def start_lockstep(self, session_byte=0x5A, check=False, n_players=None):
        n = n_players if n_players is not None else len(self.units)
        for u in self.units:
            u.call(CLEAR_LOCKSTEP)
            u.w16(LS + 4, 3 | (8 if check else 0))     # bit0 = link ativo, bit1 = verificar pacotes, bit3 = checksum de estado
            u.w8(LS + 6, session_byte)
            u.w8(N_PLAYERS, n)
        self.lockstep = True

    def link_frame(self, payloads=None, flag=0, keys=None, tamper=None):
        """Um quadro do laco principal: [BuildInputPacket] -> LinkMain -> [LinkFrameSync] -> ate 16 trocas do cabo."""
        for i, u in enumerate(self.units):
            if not self.cable.present[i]:
                continue
            if self.lockstep:
                u.w16(KEYS_NOW, (keys[i] if keys else 0) & 0x3FF)
                u.call(BUILD_INPUT)
                if tamper:
                    tamper(i, u)          # gancho: altera G_SEND / estado antes do LinkMain
            else:
                u.wb(G_SEND, payloads[i] if payloads and payloads[i] is not None else bytes(20))
            u.call(LINK_MAIN, G_SEND, G_RECV, flag)
            self.status[i] = u.uc.reg_read(AC.UC_ARM_REG_R0)
            u.w32(G_STATUS, self.status[i])
            if self.lockstep:
                self.sync_ok[i] = u.call(FRAME_SYNC) & 0xFF
        if not self.timer_started:               # o lobby (0x0801A688) chama LinkStartTimer 1x depois do 1o LinkMain
            for i, u in enumerate(self.units):
                if self.cable.present[i]:
                    u.call(LINK_START_TIMER)
            self.timer_started = True
        self.cable.run_exchanges(16)
        self.frame += 1
        return list(self.status)

    def error_code(self, unit=0):
        return self.units[unit].r32(LS_ERR)

    def game_frame(self, unit=0):
        return self.units[unit].r32(LS)


def load_rom(path=None):
    path = path or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'baserom.gba')
    with open(path, 'rb') as f:
        return f.read()
