"""Lista registradores de I/O (0x040000xx) referenciados por literais pc-relative na ROM.
Heuristica: varre a ROM inteira como Thumb (halfwords alinhados) e como ARM (words
alinhados); pode contar falsos positivos em regioes de dados. Uso: python hw/scan_io.py ROM"""
import struct, sys, collections

rom = open(sys.argv[1], 'rb').read()
n = len(rom)
def lit(addr):
    return struct.unpack_from('<I', rom, addr)[0] if 0 <= addr <= n - 4 else None
refs = collections.Counter()
# Thumb: ldr rN,[pc,#imm8*4]  = 0x48xx ; addr = ((pc+4)&~3) + imm*4
for a in range(0, n - 1, 2):
    h = rom[a] | rom[a + 1] << 8
    if h >> 11 == 0b01001:
        v = lit(((a + 4) & ~3) + (h & 0xFF) * 4)
        if v is not None and 0x04000000 <= v < 0x04000400:
            refs[v] += 1
# ARM: ldr rN,[pc,#+-imm12]
for a in range(0, n - 3, 4):
    w = struct.unpack_from('<I', rom, a)[0]
    if (w & 0x0F7F0000) == 0x051F0000:
        off = w & 0xFFF
        v = lit(a + 8 + (off if w & 0x00800000 else -off))
        if v is not None and 0x04000000 <= v < 0x04000400:
            refs[v] += 1
for v, c in sorted(refs.items()):
    print(f'{v:08x} {c}')
