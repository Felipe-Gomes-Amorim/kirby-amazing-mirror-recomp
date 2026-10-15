#include <string.h>
#include "gba_cpu.h"

/* Mapa minimo (igual ao do harness com o oraculo):
 *   0x02000000 EWRAM 256K | 0x03000000 IWRAM 32K | 0x08000000 ROM (rw=nao)
 *   0x09000000 pagina de 4K zerada, so leitura (sentinela de retorno) */
static uint8_t ewram[0x40000];
static uint8_t iwram[0x8000];
static uint8_t sentinel[0x1000];
static const uint8_t *rom;
static uint32_t rom_size;
static uint32_t trace_n;
static uint32_t trace_buf[(RC_MAX_STEPS + 1) * RC_TRACE_WORDS];

RC_API void rc_set_rom(const uint8_t *p, uint32_t n) { rom = p; rom_size = n; }
RC_API uint8_t *rc_ewram(void) { return ewram; }
RC_API uint8_t *rc_iwram(void) { return iwram; }
RC_API uint32_t *rc_trace_buf(void) { return trace_buf; }
RC_API uint32_t rc_trace_len(void) { return trace_n; }
RC_API void rc_reset(void) { trace_n = 0; }

static const uint8_t *map(Cpu *cpu, uint32_t a, uint32_t size, int write) {
    uint32_t off;
    switch (a >> 24) {
    case 2: off = a - 0x02000000u; if (off + size <= sizeof ewram) return ewram + off; break;
    case 3: off = a - 0x03000000u; if (off + size <= sizeof iwram) return iwram + off; break;
    case 8: off = a - 0x08000000u;
        if (!write && rom && off + size <= rom_size) return rom + off;
        break;
    case 9: off = a - 0x09000000u;
        if (!write && off + size <= sizeof sentinel) return sentinel + off;
        break;
    }
    cpu->status = 2;
    cpu->fault_addr = a;
    return 0;
}

static uint32_t rd(Cpu *cpu, uint32_t a, uint32_t size) {
    const uint8_t *p = map(cpu, a, size, 0);
    uint32_t v = 0;
    if (!p) return 0;
    memcpy(&v, p, size); /* host little-endian */
    return v;
}
static void wr(Cpu *cpu, uint32_t a, uint32_t size, uint32_t v) {
    uint8_t *p = (uint8_t *)map(cpu, a, size, 1);
    if (p) memcpy(p, &v, size);
}

static uint32_t ror32(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* LDR nao alinhado rotaciona (ARM7); o oraculo le nao alinhado sem rotacao. */
uint32_t rc_ldr32(Cpu *cpu, uint32_t a) { if (a & 3) cpu->skip = 1; return ror32(rd(cpu, a & ~3u, 4), (a & 3) * 8); }
uint32_t rc_ld32(Cpu *cpu, uint32_t a) { return rd(cpu, a & ~3u, 4); }
uint32_t rc_ld8(Cpu *cpu, uint32_t a) { return rd(cpu, a, 1); }
uint32_t rc_ld8s(Cpu *cpu, uint32_t a) { return (uint32_t)(int32_t)(int8_t)rd(cpu, a, 1); }
/* ARM7: LDRH em endereco impar le o halfword alinhado rotacionado de 8;
 * LDRSH impar vira LDRSB. O oraculo (ARMv5) le o halfword nao alinhado. */
uint32_t rc_ld16(Cpu *cpu, uint32_t a) {
    if (a & 1) cpu->skip = 1;
    return ror32(rd(cpu, a & ~1u, 2), (a & 1) * 8);
}
uint32_t rc_ld16s(Cpu *cpu, uint32_t a) {
    if (a & 1) { cpu->skip = 1; return (uint32_t)(int32_t)(int8_t)rd(cpu, a, 1); }
    return (uint32_t)(int32_t)(int16_t)rd(cpu, a, 2);
}
/* Stores nao alinhados: ARM7 alinha o endereco; oraculo escreve nao alinhado. */
void rc_st32(Cpu *cpu, uint32_t a, uint32_t v) { if (a & 3) cpu->skip = 1; wr(cpu, a & ~3u, 4, v); }
void rc_st16(Cpu *cpu, uint32_t a, uint32_t v) { if (a & 1) cpu->skip = 1; wr(cpu, a & ~1u, 2, v & 0xFFFF); }
void rc_st8(Cpu *cpu, uint32_t a, uint32_t v) { wr(cpu, a, 1, v & 0xFF); }

int rc_trace(Cpu *cpu, uint32_t addr) {
    if (cpu->steps >= RC_MAX_STEPS) {
        cpu->pc = addr;
        cpu->status = 1;
        return 1;
    }
    uint32_t *t = trace_buf + (size_t)trace_n * RC_TRACE_WORDS;
    t[0] = addr;
    memcpy(t + 1, cpu->r, 15 * sizeof(uint32_t));
    t[16] = cpu->n; t[17] = cpu->z; t[18] = cpu->c; t[19] = cpu->v;
    trace_n++;
    cpu->steps++;
    return 0;
}
