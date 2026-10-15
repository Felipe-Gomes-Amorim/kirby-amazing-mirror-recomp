/* Interface interna do runtime (gba_rt.c / gba_bios.c / rt_main.c). */
#ifndef GBA_RT_H
#define GBA_RT_H
#include <stdint.h>
#include "gba_cpu.h"

extern uint64_t rt_ncalls;
extern const char *rt_dump_path; /* se != NULL, rt_abort grava IWRAM+EWRAM aqui */
void rt_cpu_reset(Cpu *);
void rt_set_mode(Cpu *, uint32_t mode);
void rt_dump(Cpu *, const char *why);
void rt_abort(Cpu *, const char *why); /* imprime pc/lr/regs e termina (exit 2) */

/* opcoes do HLE */
extern int rt_opt_stop_at_vblank_wait; /* 1: para na 1a VBlankIntrWait (teste do crt0) */
extern int rt_swi_count[256];

#define RT_IRQ_RET 0x00000138u /* endereco "BIOS" para onde o handler de IRQ retorna */

#endif
