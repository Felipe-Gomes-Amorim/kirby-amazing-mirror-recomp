/* Runtime minimo p/ o C gerado: estado da CPU, shifts, memoria, trace. */
#ifndef GBA_CPU_H
#define GBA_CPU_H
#include <stdint.h>

#ifdef _WIN32
#define RC_API __declspec(dllexport)
#else
#define RC_API __attribute__((visibility("default")))
#endif

#define RC_MAX_STEPS 1000
#define RC_TRACE_WORDS 20 /* pc, r0..r14, n, z, c, v */

typedef struct Cpu {
    uint32_t r[16]; /* r[15] nao e usado: PC e constante no codigo gerado */
    uint32_t n, z, c, v;
    uint32_t thumb;
    uint32_t pc;     /* alvo ao sair da funcao */
    uint32_t steps;
    uint32_t status; /* 0 saiu, 1 limite de passos, 2 falha de memoria */
    uint32_t fault_addr;
    uint32_t skip;   /* acesso nao alinhado onde o oraculo (ARMv5) diverge do ARM7 */
    /* --- so o runtime usa daqui p/ baixo --- */
    uint32_t mode;       /* bits de modo do CPSR (0x10 USR .. 0x1F SYS) */
    uint32_t psr_if;     /* bits I/F do CPSR (0xC0) */
    uint32_t bank_sp[16], bank_lr[16]; /* indexado por mode & 15; SYS usa o de USR */
    uint32_t stop;       /* != 0: o runtime pediu para parar (ver RT_STOP_*) */
} Cpu;

enum { RT_STOP_NONE = 0, RT_STOP_VBLANK_WAIT = 1, RT_STOP_STOPCALL = 2, RT_STOP_ERROR = 3, RT_STOP_FRAMES = 4 };

/* funcoes recompiladas: tabela gerada (rc_table, ordenada por lo) */
typedef struct RcFunc {
    uint32_t lo, hi; /* [lo, hi) cobre as instrucoes da funcao */
    uint32_t thumb;
    void (*fn)(Cpu *);
    uint32_t sig;    /* overlay (RAM): primeiros 4 bytes esperados; 0 = nao confere */
} RcFunc;
extern const RcFunc rc_table[];
extern const unsigned rc_table_n;

/* runtime (gba_rt.c / gba_bios.c) */
void rt_call(Cpu *);                       /* executa a funcao em cpu->pc ate sair */
/* despacha ate cpu->pc == ret; devolve 0 se o controle voltou a um frame MAIS EXTERNO
 * (a pilha guest saltou frames: pop {pc} de epilogo compartilhado, etc.) */
int rt_run_until(Cpu *, uint32_t ret);
extern uint32_t rt_rets[4096]; /* retornos esperados dos frames C ativos */
extern uint32_t rt_depth;
extern uint32_t rt_loops;
void rt_tick_loop(Cpu *, uint32_t site);   /* desvio para tras: tempo + IRQ */
void rt_tick(Cpu *);                       /* avanca o hw e entrega IRQ (ponto seguro) */
int rt_swi(Cpu *, uint32_t num, uint32_t next); /* != 0: sair da funcao (cpu->pc ja setado) */
uint32_t rc_mrs(Cpu *);
void rc_msr(Cpu *, uint32_t fieldmask, uint32_t v);

uint32_t rc_ldr32(Cpu *, uint32_t addr);  /* LDR: rotaciona se nao alinhado */
uint32_t rc_ld32(Cpu *, uint32_t addr);   /* LDM/POP: alinha */
uint32_t rc_ld16(Cpu *, uint32_t addr);
uint32_t rc_ld16s(Cpu *, uint32_t addr);
uint32_t rc_ld8(Cpu *, uint32_t addr);
uint32_t rc_ld8s(Cpu *, uint32_t addr);
void rc_st32(Cpu *, uint32_t addr, uint32_t v);
void rc_st16(Cpu *, uint32_t addr, uint32_t v);
void rc_st8(Cpu *, uint32_t addr, uint32_t v);
int rc_trace(Cpu *, uint32_t addr); /* 1 = estourou o limite: sair */

static inline uint32_t sh_lsl(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) return v;
    if (n < 32) { *c = (v >> (32 - n)) & 1; return v << n; }
    *c = (n == 32) ? (v & 1) : 0;
    return 0;
}
static inline uint32_t sh_lsr(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) return v;
    if (n < 32) { *c = (v >> (n - 1)) & 1; return v >> n; }
    *c = (n == 32) ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t sh_asr(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) return v;
    if (n < 32) { *c = ((int32_t)v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *c = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t sh_ror(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) return v;
    n &= 31;
    if (n == 0) { *c = v >> 31; return v; }
    *c = (v >> (n - 1)) & 1;
    return (v >> n) | (v << (32 - n));
}
static inline uint32_t sh_rrx(uint32_t v, uint32_t *c) {
    uint32_t old = *c;
    *c = v & 1;
    return (v >> 1) | (old << 31);
}

#define SETNZ(res) do { cpu->n = (res) >> 31; cpu->z = ((res) == 0); } while (0)
#define POPCNT(x) __builtin_popcount(x)

#endif
