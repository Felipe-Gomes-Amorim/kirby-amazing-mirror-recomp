"""Parse GBA ROM header and disassemble the startup code."""
import struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

BASE = 0x08000000

def header(rom):
    h = rom[:0xC0]
    entry_ins = struct.unpack_from('<I', h, 0)[0]
    assert entry_ins >> 24 == 0xEA, 'entry is not B'
    off = entry_ins & 0xFFFFFF
    if off & 0x800000: off -= 0x1000000
    chk = (-sum(h[0xA0:0xBD]) - 0x19) & 0xFF
    return {
        'entry_target': BASE + 8 + off * 4,
        'title': h[0xA0:0xAC].decode('ascii'),
        'game_code': h[0xAC:0xB0].decode('ascii'),
        'maker': h[0xB0:0xB2].decode('ascii'),
        'version': h[0xBC],
        'checksum_stored': h[0xBD],
        'checksum_calc': chk,
        'size': len(rom),
    }

def dis(rom, off, n, thumb=False):
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    for i in md.disasm(rom[off:off + n], BASE + off):
        print(f'{i.address:08x}  {i.bytes.hex():8s}  {i.mnemonic} {i.op_str}')

if __name__ == '__main__':
    rom = open(sys.argv[1], 'rb').read()
    h = header(rom)
    for k, v in h.items():
        print(f'{k:16s} {v:#x}' if isinstance(v, int) else f'{k:16s} {v}')
    print()
    dis(rom, h['entry_target'] - BASE, 0x100)
