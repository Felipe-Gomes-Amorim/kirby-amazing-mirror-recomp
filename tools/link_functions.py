#!/usr/bin/env python3
"""Funcoes do KatAM que tocam o link (SIO/RCNT/JOY) -> build/analysis/link_functions.csv  (estudo P0, Poppy).

Parte automatica: percorre as funcoes de build/analysis/map.json, segue constantes de registrador (ldr =, mov, lsl, add)
e lista os ACESSOS REAIS (ldr/str com endereco efetivo) a 0x04000120-0x0400015B (SIO/RCNT/JOY), IE/IF/IME e TM3.
Isso elimina falsos positivos de "literal 0x04000140" (que e so um CpuSet de 0x140 palavras, nao JOYCNT).
Parte manual: CURATED = o que cada funcao faz, lido no disassembly (armdis); `confidence` diz o quanto foi lido.

Uso: python tools/link_functions.py [rom.gba]      (precisa de build/analysis/map.json; ~25 s)
"""
import collections
import csv
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_rom as A  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
IOREG = {
    0x04000100: 'TM0CNT_L', 0x04000102: 'TM0CNT_H', 0x0400010C: 'TM3CNT_L', 0x0400010E: 'TM3CNT_H',
    0x04000120: 'SIOMULTI0', 0x04000122: 'SIOMULTI1', 0x04000124: 'SIOMULTI2', 0x04000126: 'SIOMULTI3',
    0x04000128: 'SIOCNT', 0x0400012A: 'SIOMLT_SEND', 0x04000130: 'KEYINPUT', 0x04000132: 'KEYCNT',
    0x04000134: 'RCNT', 0x04000140: 'JOYCNT', 0x04000150: 'JOY_RECV_L', 0x04000152: 'JOY_RECV_H',
    0x04000154: 'JOY_TRANS_L', 0x04000156: 'JOY_TRANS_H', 0x04000158: 'JOYSTAT',
    0x04000200: 'IE', 0x04000202: 'IF', 0x04000208: 'IME',
}
WATCH = (0x04000120, 0x0400015C)
LINK_REGS = {'SIOMULTI0', 'SIOMULTI1', 'SIOMULTI2', 'SIOMULTI3', 'SIOCNT', 'SIOMLT_SEND', 'RCNT', 'JOYCNT',
             'JOY_RECV_L', 'JOY_RECV_H', 'JOY_TRANS_L', 'JOY_TRANS_H', 'JOYSTAT'}

# addr: (layer, name, what, confidence[high|medium|low])
CURATED = {
    # ---- motor de pacotes "gLink" (estrutura 0x03006A60); chamado todo quadro pelo laco principal ----
    0x08157210: ('glink', 'LinkInit', 'IME off; IE&=~0xC0; RCNT=0; SIOCNT=0x2000 then |=0x4003 (multi-play, 115200 baud, IRQ on); '
                 'zeroes gLink (0x03006A60, 488 B); CpuSet-copies the ARM IRQ handler 0x08158F54->0x030068E0 (384 B) and the ARM '
                 'buffer-swap helper 0x08158F14->0x03006C50 (64 B); builds the 2 send buffers (gLink+0x60/+0x7C, 14 halfwords each) and '
                 'the per-player 2x14-halfword receive buffers (stride 0x54); finally IE|=0x80 (serial). Arg r0 = initial mode bits', 'high'),
    0x08157308: ('glink', 'LinkMain', 'Per-frame engine. args (send_payload*, recv_dest*, flag). State gLink[1]: 0 start, 1 running, 2 error. '
                 'Master detection (state 0): (SIOCNT&0x30)==0 (ID=0), (SIOCNT&0x88)==8 (SD=1,not busy), SI==0 and gLink[0x14]==-1 -> '
                 'IE: serial off, Timer3 on; SIOCNT.IRQ off; IF=0xC0; TM3CNT_L/H=0xBBBC (reload, stopped); gLink[0]=8; SIOCNT|=0x80 '
                 '(kick first exchange). Running: calls LinkReceive(recv_dest) then LinkBuildSend(send_payload, flag); counts frames '
                 'without any partner (gLink[0xA]>7 -> state 2). Returns the status word (see NOTES)', 'high'),
    0x08157464: ('glink', 'LinkBuildSend', 'Builds the outgoing packet (12 halfwords) in the send buffer: hw0 = frame counter gLink[0xB] | flags<<8 '
                 '(bits0-3 = everConnected^connectedNow, bit4 = LinkMain flag arg, bit5 = 0x03006C90[3]&1, bit6 = gLink[8] bit6); hw1 = '
                 'checksum = ~sum(hw0..hw11, hw1=0) - 14; hw2..hw11 = 20-byte payload (CpuSet 5 words from the game send buffer 0x030036B0). '
                 'Then gLink[0x14] = -1 (next exchange sends the 0xFEFE sync) and, on the master with gLink[8].bit7, TM3CNT_H=0xC0 (restart)', 'high'),
    0x08157534: ('glink', 'LinkReceive', 'Calls the IWRAM helper (0x03006C50) to atomically swap the receive buffers and read the 4 "packet complete" '
                 'flags; for each player validates sum(12 halfwords)==-15 (0xFFF1) and flag; valid -> gLink[3] |= 1<<p, copies the 20-byte '
                 'payload to recv_dest[p*20], keeps header bit5 per player; clears the internal buffer. Then gLink[2] |= gLink[3] and derives '
                 'gLink[8] flags (bits 4-6) from player 0 header', 'high'),
    0x081576a0: ('glink', 'LinkStartTimer', 'if master: gLink[8] |= 0x80 (enables the Timer3 exchange chain; the lobby calls it once at 0x0801A688)', 'high'),
    0x081576bc: ('glink', 'LinkStop', 'IME off; IE&=~0xC0 (serial+timer3 off); SIOCNT=0x2003 (multi-play, no IRQ); TM3CNT_L=0xBBBC; IF=0xC0; gLink[8]&=0x7F', 'high'),
    0x08158f54: ('glink-irq', 'LinkIrqHandler (ARM, IWRAM 0x030068E0)', 'Serial IRQ (IF bit7) AND Timer3 IRQ (IF bit6) share vector slot 0 -> this code. '
                 'Reads SIOMULTI0-3 (one ldm of 2 words), stores SIOCNT.bit6 (error) in gLink[9]. SEND side: idx=gLink[0x14]; -1 => SIOMLT_SEND=0xFEFE and swaps the '
                 'two send buffers, idx->0; idx>=0 => SIOMLT_SEND = sendbuf[idx]; idx++ up to 13. RECV side per player p=3..0: value=SIOMULTI[p]; if value==0xFEFE '
                 'and idx>11 => idx=-1 (sync, then 0); else recvbuf[p][idx]=value; at idx==11 swap buffers and gLink[4+p]=1 (packet complete); idx++ up to 13. '
                 'If master (gLink[0]!=0): TM3CNT_H=0, SIOCNT|=0x80 (next exchange), TM3CNT_H=0xC0 (watchdog restart), IF ack', 'high'),
    0x08158f14: ('glink-irq', 'LinkSwapBuffers (ARM, IWRAM 0x03006C50)', 'With IME off: swaps the 2 groups of 4 receive-buffer pointers (ldm/stm of 8 words at gLink+0x40) '
                 'and returns+clears the 4 packet-complete flags gLink[4..7]. Called through the _call_via_r0 stub from LinkReceive', 'high'),
    0x0815898c: ('glink', 'LinkEnable', 'If link allowed (flags 0x03002440 bit19): IME off, IE&=~0x40, vector[0]=0x030068E0 (the ARM handler), LinkInit(0); then 0x03002558=1 ("link on")', 'high'),
    0x081589e8: ('glink', 'LinkDisable', '0x03002558=0; flags&=~bit20; LinkStop; LinkInit(0); if flags bit19: 0x08158F08', 'high'),
    0x08158934: ('glink', 'LinkReset', 'DMA3 fills the send buffer 0x030036B0 (5 words) and receive area 0x03002490 (20 words) with 0; status 0x03002554=0; link-on flag 0x03002558=0; LinkInit(0)', 'high'),
    0x08030e44: ('lockstep', 'BuildInputPacket', 'Runs before LinkMain when the link is active and not waiting. Appends the local input for this frame to the own ring (0x020382D0+0x20, 16 entries '
                  'x 8 B = frame u32 + keys u16) = KEYINPUT&0x3FF | (2-bit state checksum << 10); builds the type-0x20 payload at 0x030036B0: [0]=0x20, [1]=session byte '
                  '(0x020382D0+6), [2..0xD]= last 8 inputs as 12-bit values packed LSB-first (newest first), [0x10..0x13]=frame number', 'high'),
    0x08030fe0: ('lockstep', 'LinkFrameSync', 'Runs after LinkMain. For each player (N = byte 0x0203AD30): own slot (ID = SIOCNT bits4-5) copies the own ring; others parse the 20-byte '
                  'payload in 0x03002490+p*20: type must be 0x20, session byte must match, rebuilds the remote ring from the 8 redundant inputs (delta = frame - last stored, '
                  'up to 8), checks the 2-bit state checksum. Returns 1 and advances the frame counter (0x020382D0+0) only when every remote input for the next frame is present; '
                  'otherwise waits (flag bit2, counter 0x020382D0+0x2AD++). Returns 0 on fatal error and stores the code in 0x02038580: 3 bad type, 4 session mismatch, '
                  '5 ring overrun, 6 missing-data marker 0xFFFF, 7 more than 16 stalled frames, 8 state-checksum mismatch (desync)', 'high'),
    0x08032e98: ('lockstep', 'LinkErrorScreen', 'Called when LinkFrameSync returns 0 (and from 0x0801F9FC timeouts): clears sound/tasks, creates the error-message task, saves the error code '
                 '(0x02038580), LinkDisable, DMA-clears the send/receive buffers, LinkInit(0), restores the default IRQ vectors from 0x08D5FD9C. Ends the link session', 'high'),
    0x0801a618: ('lobby', 'LobbyLinkTick', 'Menu state: sends payload byte0 = ROM halfword [0x08D60A80] (game/version id) and calls LinkMain(0x030036B0,0x03002490,flag); stores the status word '
                 'in 0x03002554; if status bit5 set picks next state 0x0801A6BD (bulk-transfer) or 0x0801A8E9 depending on mode/slot', 'medium'),
    0x0801a908: ('lobby', 'LobbyLinkStart', 'IME off; vector[0]=0x030068E0; IME on; LinkInit(status bits 8-11 >> 8 = connected-players mask); clears two menu fields', 'high'),
    0x0803169c: ('lobby', 'LobbySendHello', 'Lobby: payload byte0=2 (hello), byte1=[menu+0x1C], hw1=0x0123, byte 0xE=0, 16 B of player data copied from menu+4; then, if I am master '
                  '(status bit7 and ID==0) or child, sets menu flag bit0/bit1 and next state 0x08031765', 'medium'),
    0x080315b8: ('lobby', 'LobbyScanPeers', 'For p=0..3: own slot (ID==p) or status bit (0x100<<p) "ever connected" set; checks recv[p] byte0==2 (hello), byte1 equals own [menu+0x1C] '
                  '(same mode/version) and byte 0xE in {0x20,0x40,0x41}; counts valid peers and builds a mask', 'medium'),
    0x08031764: ('lobby', 'LobbyPhase20', 'Lobby phase 0x20: payload byte 0xE = 0x20, hw 0xC = parameter; sends, then scans peers (LobbyScanPeers) until all agree; clears LinkMain buffers (CpuSet) and advances', 'low'),
    0x08031860: ('lobby', 'LobbyPhase40_42', 'Lobby phase: payload byte 0xE selects 0x40/0x41/0x42 acknowledgements; waits for peers; SIOCNT ID decides who sends', 'low'),
    0x080319f0: ('lobby', 'LobbyPhase40', 'Lobby phase 0x40 (ready): payload byte 0xE=0x40, hw 0xC = param; waits for the peers\' 0x20/0x40 answers via LobbyScanPeers', 'low'),
    0x08031aa8: ('lobby', 'LobbyPhase41', 'Lobby phase 0x41/0x22 (start/confirm): payload byte 0xE=0x41; compares peers\' byte 0xE with 0x22; CpuSet clears buffers when done', 'low'),
    0x0803234c: ('lobby', 'LobbyCompareId', 'Compares own ID (SIOCNT bits4-5) with the menu player index; sets menu state 0x20 (mine) or 8 (other)', 'low'),
    0x080323dc: ('lobby', 'LobbyTickId', 'Small menu tick: reads SIOCNT ID to refresh the player-slot state (sets menu halfword 0x16)', 'low'),
    0x080324bc: ('lobby', 'LobbyRecvCharSel', 'Lobby state reading recv[] payloads and own ID (SIOCNT); handles the per-player selection exchange (counters up to 0xF)', 'low'),
    0x080332bc: ('session', 'StartLinkSession', 'Starts a link game: sets 0x0203AD30 = player count (arg), 0x0203AD3C = own ID (SIOCNT bits4-5 when the link flag 0x0203AD10 bit1 is set, else arg), '
                  'resets per-player state at 0x02020EE0 (stride 0x1A8) then enters the game. Callers: 0x08025650, 0x08025F84, 0x080329C8, 0x08123EA8, 0x08123FD4', 'medium'),
    0x0801f9fc: ('session', 'LinkWaitTimeout', 'State callback: decrements a timer (menu+0x172); on expiry -> error screen 0x08032E98; polls LinkStatus (0x08031C64): result 2 => copies player count '
                  '(0x020382A0+0x28) to 0x0203AD30 and ID to 0x0203AD3C and starts the session; result 1 => waits up to 8 frames; <0 => abort', 'medium'),
    0x0815158c: ('boot', 'MainInit', 'Game init (called from main 0x08152A04): IME off, RCNT read: (RCNT&0xC000) != 0x8000 => flags 0x03002440 = 0x200 ("booted by MultiBoot as a client"); then '
                 'inits hardware/BG/sound; calls LinkInit (0x08157210) at 0x08151BBA. => hw must reset RCNT to 0x8000', 'high'),
    0x080001cc: ('boot', 'BootLinkReset', 'Called from main: RCNT=0, zeroes 0x030004C0/0x03000490/0x03000478 (multiboot structs), state 0x0300050C=-1; then save/slot probing (0x0800A91C/AC00/ACD4)', 'high'),
    # ---- camada MultiBoot / transferencia em bloco (Kirby "single-pak"): fora do escopo online ----
    0x0803024c: ('multiboot', 'KMB_Open', 'IME off; zeroes 0x03000490 (44 B); SIOCNT=0x2000 then |=0x4003; SIOMLT_SEND=0; saves vector[0] and [7] to 0x03000484/0x03000470 and installs 0x08030899 '
                  'in both; IE|=0x80; IME on', 'high'),
    0x08030b38: ('multiboot', 'KMB_Close', 'IME off; mode 0x0300050C=-1; SIOCNT&=0xBFFF; SIOMLT_SEND=0x8F52; restores vectors [0],[7]; IE&=~0x40; IME on', 'high'),
    0x08030898: ('multiboot', 'KMB_IrqHandler (Thumb, ROM)', 'Serial IRQ for the Kirby multiboot handshake: copies SIOCNT/SIOMULTI0-3, ID, error bit; exchanges magic halfwords 0x8F50/0x8F51/0x8F52, '
                  '0x70AE, 0xE4E4 and a 13-bit counter (&0x1FFF) with 0xE000 tag bits; flags peers that answer 0xFFFF', 'medium'),
    0x080302ec: ('multiboot', 'KMB_StateIdle', 'State 0 of 0x0300050C: if SI==0 (parent) runs MultiBootMain(0x030004C0); parent/child detection by comparing recv halfwords with 0xE4xx/0x8F5x patterns', 'medium'),
    0x0803040c: ('multiboot', 'KMB_StateParent', 'State 1: parent handshake: sends 0xE4E4 / counter|tag, starts transfers with SIOCNT|=0x80, tracks which children answered', 'medium'),
    0x080305f8: ('multiboot', 'KMB_StateMultiBoot', 'State 2: drives MultiBootMain(0x030004C0) and the image transfer to children; on completion switches to normal multi-play (SIOCNT|=0x4000)', 'medium'),
    0x08030c40: ('multiboot', 'KMB_Dispatch', 'Dispatches on 0x0300050C (0/1/2) to the three states above', 'high'),
    0x08030bb8: ('multiboot', 'KMB_StartImage', 'Zeroes 0x030004C0 and calls the SDK MultiBootInit variant 0x08157DDC for the image [start,end) (sizes rounded to 16 B)', 'high'),
    0x08019fdc: ('multiboot', 'EnterMultiBootMenu', 'Menu task: RCNT=0, clears multiboot structs, mode=-1, KMB_Open, KMB_StartImage(image table 0x082DE098[choice])', 'high'),
    0x08031d24: ('multiboot', 'EnterMultiBootMenu2', 'Same as 0x08019FDC for the second menu: RCNT=0, clear structs, KMB_Open, KMB_StartImage', 'high'),
    0x0815770c: ('multiboot', 'MultiBootMain (SDK)', 'SDK MultiBoot server state machine (handshake 0x6100/0x62xx/0x63xx..., 0x72xx answers; absent child = SIOMULTI 0xFFFF); polled by KMB_StateMultiBoot', 'medium'),
    0x08157b54: ('multiboot', 'MultiBootSend', 'If (SIOCNT&0x8C)==8 (SD, not busy, no error): SIOMLT_SEND=arg; SIOCNT|=0x80; else resets the MultiBoot state', 'high'),
    0x08157ce0: ('multiboot', 'MultiBootParse', 'Reads SIOMULTI1-3 answers during the MultiBoot handshake', 'medium'),
    0x08157e0c: ('multiboot', 'MultiBootWaitIdle', 'Busy-waits for SIOCNT bit7 to clear (bounded loop 0x795C) then a CPU-speed-dependent delay', 'high'),
    0x08157e54: ('bulk32', 'BulkTransferStep', 'State machine (jump table at 0x08157E78) of the SIO bulk data transfer: states set SIOCNT=0x1000/0x1001 (Normal 32-bit), SIOMULTI0=0, IF=0xC0, '
                  'SIOCNT|=0x80, TM3CNT=0x00C0F318 (timer3 on), IE|=0x40/0x80, checksum accumulation; used after 0x08158100', 'medium'),
    0x08158038: ('bulk32', 'BulkIrqHandler (Thumb, ROM)', 'Installed in vector[0] by 0x0801A6BC: 32-bit SIODATA exchange (0xFEFEFEFE sync, data words from a ROM block up to 0x2000 words), SIOCNT|=0x80, TM3CNT_H=0xC0', 'medium'),
    0x08158100: ('bulk32', 'BulkTransferInit', 'SIOCNT=0x2003; TM3CNT=0; zeroes 0x03006C90 area (CpuSet 7 words); stores data pointer; if master: sums 0x2000 words (32 KiB) of the block, checksum -> struct+0xC, then SIOCNT=0x1000 and 0x1001 (Normal 32-bit, internal clock)', 'high'),
    0x0801a6bc: ('bulk32', 'LobbyStartBulk', 'Selects a 32 KiB block from tables 0x082DE158/0x082DE230/0x082DE308 by mode and slot, LinkStop, vector[0]=0x08158039, BulkTransferInit', 'medium'),
    0x08158ea8: ('irq-default', 'DefaultVector7 (Normal32 stop)', 'Default IRQ vector[7] (pointer at 0x08D5FDB8 in the default-vector table 0x08D5FD9C): IME off; IE&=~0x40 (timer3), IE&=~0x80 (serial); TM3CNT_H=0, TM3CNT_L=0x8000; 0x03006CC0=5. Safety net that shuts the link down if its IRQs fire when no link code owns them', 'medium'),
    # ---- falsos positivos (so IE/IME/KEYINPUT/KEYCNT) ----
    0x080002c8: ('not-link', 'KeypadWait', 'Reads KEYINPUT and IE/IME (reset-combo / wait); the 0x04000130 access fell in the SIO scan range', 'high'),
    0x08152388: ('not-link', 'VBlankHandler', 'IE/IF/KEYINPUT; the vblank IRQ handler', 'medium'),
    0x08152694: ('not-link', 'ReadKeys', 'KEYINPUT read', 'high'),
    0x08152790: ('not-link', 'SetKeyIrq', 'KEYCNT write + IE', 'high'),
}
EXTRA_ROWS = [  # nao estao no mapa de funcoes como tal
    (0x08158a28, 1152, 'dead-code', 'Normal32Library (unreferenced)',
     'Thumb block mapped as data: SDK-style SIO Normal-32 slave code (SIOCNT=0x1008|0x4000, RCNT=0, TM3CNT_H=0xC1, SIODATA32 reads). Not reachable: no direct bl and no data pointer from outside the block',
     'SIOCNT,SIOMULTI0(SIODATA32),RCNT', 'SIOCNT,RCNT,SIOMULTI0', 'high'),
]


def parse_imm(s):
    s = s.strip().lstrip('#')
    try:
        return int(s, 0)
    except ValueError:
        return None


def main():
    rom_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'baserom.gba'
    rom = rom_path.read_bytes()
    m = json.loads((ROOT / 'build' / 'analysis' / 'map.json').read_text(encoding='utf-8'))
    a = A.Analyzer(rom)
    seeds = []
    for f in m['functions']:
        if f.get('exec_addr') is not None:
            continue
        a.add_func(f['addr'], f['mode'] == 'thumb', f['source'])
        seeds.append((f['addr'], f['mode'] == 'thumb', f['source']))
    a.run(seeds)
    fsize = {f['addr']: f['bytes'] for f in m['functions']}

    byfunc = collections.defaultdict(list)
    for ad, ins in a.insns.items():
        byfunc[a.insn_owner.get(ad, ad)].append(ins)
    acc_by = {}
    for fa, lst in byfunc.items():
        lst.sort(key=lambda i: i.addr)
        regs, acc = {}, []
        for ins in lst:
            if not ins.thumb:
                continue
            mn, ops = ins.mnem, ins.ops
            rd = ops[0] if ops else None
            if mn == 'ldr' and len(ops) == 2 and ops[1].startswith('[pc'):
                mm = re.match(r'\[pc(?:,\s*#(-?(?:0x)?[0-9a-fA-F]+))?\]', ops[1])
                imm = parse_imm(mm.group(1)) if mm and mm.group(1) else 0
                regs[A.reg(rd)] = a.word(((ins.addr + 4) & ~3) + imm)
                continue
            if mn in ('movs', 'mov') and len(ops) == 2 and ops[1].startswith('#'):
                regs[A.reg(rd)] = parse_imm(ops[1]) & 0xFFFFFFFF
                continue
            if mn in ('lsls', 'lsl') and len(ops) == 3:
                v, n = regs.get(A.reg(ops[1])), parse_imm(ops[2])
                regs[A.reg(rd)] = (v << n) & 0xFFFFFFFF if v is not None and n is not None else None
                if regs[A.reg(rd)] is None:
                    regs.pop(A.reg(rd), None)
                continue
            if mn in ('adds', 'add') and len(ops) == 3 and ops[2].startswith('#'):
                v, n = regs.get(A.reg(ops[1])), parse_imm(ops[2])
                if v is not None and n is not None:
                    regs[A.reg(rd)] = (v + n) & 0xFFFFFFFF
                else:
                    regs.pop(A.reg(rd), None)
                continue
            mm = re.match(r'^(ldr|str)(b|h|sh|sb)?$', mn)
            if mm and len(ops) == 2 and ops[1].startswith('['):
                parts = [p.strip() for p in ops[1].strip('[]').split(',')]
                base = regs.get(A.reg(parts[0]))
                off = 0
                if len(parts) > 1:
                    off = parse_imm(parts[1]) if parts[1].startswith('#') else None
                if base is not None and off is not None:
                    ea = (base + off) & 0xFFFFFFFF
                    if ea in IOREG or WATCH[0] <= ea < WATCH[1]:
                        acc.append((ins.addr, 'R' if mm.group(1) == 'ldr' else 'W', IOREG.get(ea, hex(ea))))
                if mm.group(1) == 'ldr':
                    regs.pop(A.reg(rd), None)
                continue
            if ins.kind == 'call':
                for r in ('r0', 'r1', 'r2', 'r3'):
                    regs.pop(r, None)
            elif rd and rd.startswith('r') and mn not in ('cmp', 'tst', 'str', 'strb', 'strh', 'push') and not mn.startswith('b'):
                regs.pop(A.reg(rd), None)
        if acc:
            acc_by[fa] = acc

    callers = collections.defaultdict(set)
    for ad, ins in a.insns.items():
        for t in ins.calls:
            callers[t].add(a.insn_owner.get(ad, ad))
    words = struct.unpack_from('<%dI' % (len(rom) // 4), rom, 0)
    want = set(CURATED) | {fa for fa, ac in acc_by.items() if any(n in LINK_REGS for _, _, n in ac)}
    ptrs = collections.defaultdict(list)
    for i, w in enumerate(words):
        if (w & 1) and (w & ~1) in want:
            ptrs[w & ~1].append(0x08000000 + i * 4)

    rows = []
    for fa in sorted(want):
        accs = acc_by.get(fa, [])
        rd_, wr_ = collections.OrderedDict(), collections.OrderedDict()
        for _, rw, name in accs:
            (rd_ if rw == 'R' else wr_)[name] = 1
        touches = int(any(n in LINK_REGS for _, _, n in accs))
        layer, name, what, conf = CURATED.get(fa, ('?', '', 'not curated yet (auto data only)', 'low'))
        rows.append({
            'addr': f'0x{fa:08X}', 'size': fsize.get(fa, ''), 'layer': layer, 'name': name, 'what': what,
            'touches_sio_rcnt_joy': touches,
            'io_read': ' '.join(rd_), 'io_write': ' '.join(wr_),
            'callers': ' '.join(f'0x{c:08X}' for c in sorted(callers.get(fa, ()))),
            'data_pointers': ' '.join(f'0x{p:08X}' for p in ptrs.get(fa, [])),
            'confidence': conf,
        })
    for fa, size, layer, name, what, rd, wr, conf in EXTRA_ROWS:
        rows.append({'addr': f'0x{fa:08X}', 'size': size, 'layer': layer, 'name': name, 'what': what,
                     'touches_sio_rcnt_joy': 1, 'io_read': rd.replace(',', ' '), 'io_write': wr.replace(',', ' '),
                     'callers': '(none)', 'data_pointers': '(none)', 'confidence': conf})
    rows.sort(key=lambda r: int(r['addr'], 16))
    out = ROOT / 'build' / 'analysis' / 'link_functions.csv'
    with open(out, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    real = [r for r in rows if r['touches_sio_rcnt_joy']]
    print(f'{len(rows)} linhas ({len(real)} tocam SIO/RCNT/JOY de verdade) -> {out}')
    print('camadas:', dict(collections.Counter(r['layer'] for r in rows)))
    joy = [r['addr'] for r in rows if 'JOY' in r['io_read'] + r['io_write']]
    print('acessos reais a JOYCNT/JOY_*:', joy or 'nenhum')


if __name__ == '__main__':
    main()
