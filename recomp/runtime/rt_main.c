/* Executavel de teste do runtime: carrega a ROM e roda o codigo recompilado a
 * partir do entry (0x08000000) ate a primeira VBlankIntrWait ou uma falha.
 * uso: rt_boot <rom.gba> [--no-stop]   (--no-stop: nao para na 1a VBlankIntrWait) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s rom.gba [--no-stop]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)n);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    gba_load_rom(rom, (uint32_t)n);
    hw_reset();
    rt_opt_stop_at_vblank_wait = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-stop")) rt_opt_stop_at_vblank_wait = 0;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) rt_dump_path = argv[++i];
    }

    static Cpu cpu;
    rt_cpu_reset(&cpu);
    cpu.pc = 0x08000000;
    cpu.thumb = 0;
    while (!cpu.stop) rt_call(&cpu);

    const char *why = cpu.stop == RT_STOP_VBLANK_WAIT ? "primeira VBlankIntrWait"
                      : cpu.stop == RT_STOP_STOPCALL  ? "SWI Stop" : "erro";
    printf("RT_STOP %s pc=%08X lr=%08X sp=%08X calls=%llu\n", why, cpu.pc, cpu.r[14], cpu.r[13],
           (unsigned long long)rt_ncalls);
    for (int i = 0; i < 256; i++)
        if (rt_swi_count[i]) printf("SWI 0x%02X x%d\n", i, rt_swi_count[i]);
    return cpu.stop == RT_STOP_ERROR ? 2 : 0;
}
