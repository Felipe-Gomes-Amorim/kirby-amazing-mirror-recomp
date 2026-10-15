/* Executavel de teste do runtime: carrega a ROM e roda o codigo recompilado a
 * partir do entry (0x08000000) ate a primeira VBlankIntrWait ou uma falha.
 * uso: rt_boot <rom.gba> [--no-stop]   (--no-stop: nao para na 1a VBlankIntrWait) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static Cpu cpu;
static unsigned watchdog_s;

static void watchdog_fire(void) {
    fprintf(stderr, "\n[rt] WATCHDOG: %us sem terminar. ultimo desvio-para-tras=%08X loops=%u\n",
            watchdog_s, rt_loop_site, rt_loops);
    rt_abort(&cpu, "watchdog");
    fflush(stderr);
    exit(3);
}
#ifdef _WIN32
static DWORD WINAPI watchdog_thread(LPVOID p) {
    (void)p;
    Sleep(watchdog_s * 1000);
    watchdog_fire();
    return 0;
}
#else
static void *watchdog_thread(void *p) {
    (void)p;
    sleep(watchdog_s);
    watchdog_fire();
    return 0;
}
#endif

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
    const char *ppm = NULL;
    rt_opt_stop_at_vblank_wait = 1;
    hw_set_keys(0x03FF);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-stop")) rt_opt_stop_at_vblank_wait = 0;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) rt_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--unresolved") && i + 1 < argc) rt_unresolved_path = argv[++i];
        else if (!strcmp(argv[i], "--stop-on-unresolved")) { extern int rt_stop_on_unresolved; rt_stop_on_unresolved = 1; }
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--trace-psr")) { extern int rt_trace_psr; rt_trace_psr = 200; }
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) watchdog_s = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            rt_frames_limit = (uint64_t)atoll(argv[++i]);
            if (rt_frames_limit) rt_opt_stop_at_vblank_wait = 0;
        }
    }

    rt_cpu_reset(&cpu);
    if (watchdog_s) {
#ifdef _WIN32
        CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
#else
        pthread_t th;
        pthread_create(&th, NULL, watchdog_thread, NULL);
#endif
    }
    cpu.pc = 0x08000000;
    cpu.thumb = 0;
    while (!cpu.stop) rt_call(&cpu);

    const char *why = cpu.stop == RT_STOP_FRAMES ? "frames"
                      : cpu.stop == RT_STOP_VBLANK_WAIT ? "primeira VBlankIntrWait"
                      : cpu.stop == RT_STOP_STOPCALL  ? "SWI Stop" : "erro";
    printf("RT_STOP %s pc=%08X lr=%08X sp=%08X calls=%llu\n", why, cpu.pc, cpu.r[14], cpu.r[13],
           (unsigned long long)rt_ncalls);
    printf("IRQ entregues=%llu (BIOS_IF pelo handler=%llu, pelo runtime=%llu; IF limpo pelo "
           "runtime=%llu) frames_hw=%llu nao_resolvidos=%llu\n",
           (unsigned long long)rt_irq_delivered, (unsigned long long)rt_irq_biosif_by_handler,
           (unsigned long long)rt_irq_biosif_by_runtime, (unsigned long long)rt_irq_ack_by_runtime,
           (unsigned long long)rt_frames_done, (unsigned long long)rt_unresolved_count);
    if (ppm && rt_last_frame()) {
        FILE *o = fopen(ppm, "wb");
        if (o) {
            fprintf(o, "P6\n240 160\n255\n");
            for (int i = 0; i < 240 * 160; i++) {
                uint32_t p = rt_last_frame()[i];
                fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o);
            }
            fclose(o);
        }
    }
    for (int i = 0; i < 256; i++)
        if (rt_swi_count[i]) printf("SWI 0x%02X x%d\n", i, rt_swi_count[i]);
    return cpu.stop == RT_STOP_ERROR ? 2 : 0;
}
