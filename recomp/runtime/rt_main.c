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
extern Cpu *rt_cpu;
int rt_measure;
void rt_window_install(int window, int scale, int pace);
void rt_window_close(void);
void rt_report_speed(void);
extern Cpu *rt_cpu;
int rt_measure;
void rt_window_install(int window, int scale, int pace);
void rt_window_close(void);
void rt_report_speed(void);
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
    const char *ppm = NULL, *sav = NULL;
    int window = 0, scale = 3, bench = 0, selftest = 0;
    rt_opt_stop_at_vblank_wait = 1;
    hw_set_keys(0x03FF);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-stop")) rt_opt_stop_at_vblank_wait = 0;
        else if (!strcmp(argv[i], "--window")) {
            window = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') scale = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--selftest-sram")) selftest = 1;
        else if (!strcmp(argv[i], "--selftest-interp")) selftest = 2;
        else if (!strcmp(argv[i], "--no-sram-alias")) { extern int rt_no_alias; rt_no_alias = 1; }
        else if (!strcmp(argv[i], "--no-overlays")) { extern int rt_skip_overlays; rt_skip_overlays = 1; }
        else if (!strcmp(argv[i], "--mute")) { extern int rt_mute; rt_mute = 1; }
        else if (!strcmp(argv[i], "--audio-raw") && i + 1 < argc) { extern const char *rt_audio_raw; rt_audio_raw = argv[++i]; }
        else if (!strcmp(argv[i], "--bench-from") && i + 1 < argc) { extern uint64_t rt_bench_from; rt_bench_from = (uint64_t)atoll(argv[++i]); }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) rt_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--unresolved") && i + 1 < argc) rt_unresolved_path = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) { if (rt_script_keys(argv[++i])) { fprintf(stderr, "--keys invalido"); return 1; } }
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) rt_script_snaps(argv[++i]);
        else if (!strcmp(argv[i], "--frames-dir") && i + 1 < argc) rt_script_dir(argv[++i]);
        else if (!strcmp(argv[i], "--sav") && i + 1 < argc) sav = argv[++i];
        else if (!strcmp(argv[i], "--stop-on-unresolved")) { extern int rt_stop_on_unresolved; rt_stop_on_unresolved = 1; }
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--trace-psr")) { extern int rt_trace_psr; rt_trace_psr = 200; }
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) watchdog_s = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            rt_frames_limit = (uint64_t)atoll(argv[++i]);
            if (rt_frames_limit) rt_opt_stop_at_vblank_wait = 0;
        }
    }

    {   /* save: --sav FILE | <rom>.sav | "none" */
        static char def[1024];
        if (!sav) {
            snprintf(def, sizeof def, "%s", argv[1]);
            char *dot = strrchr(def, '.');
            if (dot) strcpy(dot, ".sav");
            else strcat(def, ".sav");
            sav = def;
        }
        if (strcmp(sav, "none")) rt_script_sram(sav);
    }
    rt_cpu = &cpu;
    if (selftest) {
        extern int rt_selftest_sram(Cpu *);
        extern int rt_selftest_interp(Cpu *);
        return (selftest == 2 ? rt_selftest_interp(&cpu) : rt_selftest_sram(&cpu)) ? 1 : 0;
    }
    rt_script_install();
    if (window || bench) {
        rt_measure = 1;
        rt_window_install(window, scale, window);
        if (window) rt_opt_stop_at_vblank_wait = 0;
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

    rt_window_close();
    if (window || bench) rt_report_speed();
    const char *why = cpu.stop == RT_STOP_FRAMES ? "frames"
                      : cpu.stop == RT_STOP_VBLANK_WAIT ? "primeira VBlankIntrWait"
                      : cpu.stop == RT_STOP_STOPCALL  ? "SWI Stop" : "erro";
    printf("RT_STOP %s pc=%08X lr=%08X sp=%08X calls=%llu\n", why, cpu.pc, cpu.r[14], cpu.r[13],
           (unsigned long long)rt_ncalls);
    { extern uint64_t rt_interp_entries, rt_interp_steps;
      printf("INTERPRETADOR: %llu entradas, %llu instrucoes\n", (unsigned long long)rt_interp_entries,
             (unsigned long long)rt_interp_steps); }
    { extern uint64_t rt_sram_alias_hits; printf("SRAM na pilha (alias p/ ROM): %llu usos\n", (unsigned long long)rt_sram_alias_hits); }
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
