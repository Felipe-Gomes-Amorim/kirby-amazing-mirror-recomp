/* Teste do HLE da BIOS (sem ROM): Div, Sqrt, ArcTan2, CpuSet, CpuFastSet, LZ77, RL. */
#include <stdio.h>
#include <string.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

const RcFunc rc_table[1] = {{0, 0, 0, 0, 0}};
const unsigned rc_table_n = 0;
/* simbolos que o rt_main/rt_window fornecem no executavel real */
double rt_hwstep_us;
uint64_t rt_hwstep_calls, rt_hwstep_cycles;
int rt_measure;
void rt_save_tick(void) {}
double rt_now_us(void) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHA: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static uint8_t rom[16];
    gba_load_rom(rom, sizeof rom);
    Cpu c;
    rt_cpu_reset(&c);

    c.r[0] = (uint32_t)-7; c.r[1] = 2; rt_swi(&c, 6, 0);
    CHECK((int32_t)c.r[0] == -3 && (int32_t)c.r[1] == -1 && c.r[3] == 3, "Div -7/2 = %d r%d abs%u", (int)c.r[0], (int)c.r[1], c.r[3]);
    c.r[0] = 1000000; c.r[1] = 7; rt_swi(&c, 6, 0);
    CHECK(c.r[0] == 142857 && c.r[1] == 1, "Div 1000000/7");

    c.r[0] = 0; rt_swi(&c, 8, 0); CHECK(c.r[0] == 0, "Sqrt 0");
    c.r[0] = 1000000; rt_swi(&c, 8, 0); CHECK(c.r[0] == 1000, "Sqrt 1e6 = %u", c.r[0]);
    c.r[0] = 0xFFFFFFFF; rt_swi(&c, 8, 0); CHECK(c.r[0] == 65535, "Sqrt max = %u", c.r[0]);
    c.r[0] = 99; rt_swi(&c, 8, 0); CHECK(c.r[0] == 9, "Sqrt 99");

    /* ArcTan2: eixos e diagonal (0x2000 = 45 graus em 0..0xFFFF) */
    c.r[0] = 100; c.r[1] = 0; rt_swi(&c, 0x0A, 0); CHECK(c.r[0] == 0, "atan2 +x");
    c.r[0] = 0; c.r[1] = 100; rt_swi(&c, 0x0A, 0); CHECK(c.r[0] == 0x4000, "atan2 +y");
    c.r[0] = (uint32_t)-100; c.r[1] = 0; rt_swi(&c, 0x0A, 0); CHECK(c.r[0] == 0x8000, "atan2 -x");
    c.r[0] = 0; c.r[1] = (uint32_t)-100; rt_swi(&c, 0x0A, 0); CHECK(c.r[0] == 0xC000, "atan2 -y");
    c.r[0] = 1000; c.r[1] = 1000; rt_swi(&c, 0x0A, 0);
    CHECK(c.r[0] > 0x1FF0 && c.r[0] < 0x2010, "atan2 45 graus = %04X", c.r[0]);

    /* CpuSet copia e fill (16 e 32 bits) */
    for (int i = 0; i < 16; i++) gba_write8(0x02000000 + i, (uint32_t)(i + 1));
    c.r[0] = 0x02000000; c.r[1] = 0x02000100; c.r[2] = 4 | (1u << 26); rt_swi(&c, 0x0B, 0);
    CHECK(gba_read32(0x02000100) == 0x04030201 && gba_read32(0x0200010C) == 0x100F0E0D, "CpuSet 32");
    gba_write16(0x02000200, 0xABCD);
    c.r[0] = 0x02000200; c.r[1] = 0x02000300; c.r[2] = 5 | (1u << 24); rt_swi(&c, 0x0B, 0);
    CHECK(gba_read16(0x02000308) == 0xABCD && gba_read16(0x0200030A) == 0, "CpuSet fill16");
    gba_write32(0x02000400, 0x11223344);
    c.r[0] = 0x02000400; c.r[1] = 0x02000500; c.r[2] = 8 | (1u << 24); rt_swi(&c, 0x0C, 0);
    CHECK(gba_read32(0x0200051C) == 0x11223344, "CpuFastSet fill");

    /* LZ77: "ABABABAB" = literais A,B + copia len 6 disp 2 */
    uint8_t lz[] = {0x10, 0x08, 0x00, 0x00, 0x20, 'A', 'B', 0x30, 0x01};
    for (unsigned i = 0; i < sizeof lz; i++) gba_write8(0x02001000 + i, lz[i]);
    c.r[0] = 0x02001000; c.r[1] = 0x02002000; rt_swi(&c, 0x11, 0);
    CHECK(gba_read32(0x02002000) == 0x42414241 && gba_read32(0x02002004) == 0x42414241, "LZ77 %08X", gba_read32(0x02002000));
    c.r[1] = 0x06000000; rt_swi(&c, 0x12, 0);
    CHECK(gba_read16(0x06000000) == 0x4241 && gba_read16(0x06000006) == 0x4241, "LZ77 VRAM");

    /* RL: 3 literais "xyz" + 5x 'q' */
    uint8_t rl[] = {0x30, 0x08, 0x00, 0x00, 0x02, 'x', 'y', 'z', 0x82, 'q'};
    for (unsigned i = 0; i < sizeof rl; i++) gba_write8(0x02003000 + i, rl[i]);
    c.r[0] = 0x02003000; c.r[1] = 0x02004000; rt_swi(&c, 0x14, 0);
    CHECK(gba_read32(0x02004000) == 0x717A7978 && gba_read32(0x02004004) == 0x71717171, "RL %08X %08X", gba_read32(0x02004000), gba_read32(0x02004004));

    printf(fails ? "bios_test: %d falhas\n" : "bios_test: OK\n", fails);
    return fails != 0;
}
