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
extern int rt_exit_code;
int rt_cfg_load(const char *path);
int rt_cfg_get(int which);
void rt_cfg_defaults(void);
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
    fprintf(stderr, "\n[rt] WATCHDOG: %us without finishing. last backward branch=%08X loops=%u\n",
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

/* ---- localizar a ROM quando o exe e aberto sem argumentos (duplo clique) ---- */
static void exe_dir(char *out, size_t n) {
    out[0] = 0;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (k == 0 || k >= n) { out[0] = 0; return; }
    for (char *p = out + k; p > out; p--)
        if (*p == '\\' || *p == '/') { *p = 0; break; }
#else
    snprintf(out, n, ".");
#endif
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* ordem: baserom.gba ao lado do exe, qualquer *.gba ao lado, baserom.gba no cwd, ultima ROM usada */
static int find_rom(char *out, size_t n) {
    char dir[900];
    exe_dir(dir, sizeof dir);
    snprintf(out, n, "%s/baserom.gba", dir);
    if (file_exists(out)) return 1;
#ifdef _WIN32
    {
        char pat[1000];
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s/*.gba", dir);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(out, n, "%s/%s", dir, fd.cFileName);
            FindClose(h);
            return 1;
        }
    }
#endif
    snprintf(out, n, "baserom.gba");
    if (file_exists(out)) return 1;
    {
        char lp[1000];
        snprintf(lp, sizeof lp, "%s/kirby_lastrom.txt", dir);
        FILE *f = fopen(lp, "rb");
        if (f) {
            size_t k = fread(out, 1, n - 1, f);
            fclose(f);
            out[k] = 0;
            while (k && (out[k - 1] == '\n' || out[k - 1] == '\r' || out[k - 1] == ' ')) out[--k] = 0;
            if (k && file_exists(out)) return 1;
        }
    }
    return 0;
}

static void remember_rom(const char *path) {
    char dir[900], lp[1000], full[1100];
    exe_dir(dir, sizeof dir);
    snprintf(lp, sizeof lp, "%s/kirby_lastrom.txt", dir);
    snprintf(full, sizeof full, "%s", path);
#ifdef _WIN32
    GetFullPathNameA(path, sizeof full, full, NULL);
#endif
    FILE *f = fopen(lp, "wb");
    if (f) {
        fputs(full, f);
        fclose(f);
    }
}

static void fatal_msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Kirby & The Amazing Mirror", MB_OK | MB_ICONINFORMATION);
#endif
}

int main(int argc, char **argv) {
    static char found[1024];
    const char *rom_path = NULL;
    int first = 1;
    if (argc >= 2 && argv[1][0] != '-') {
        rom_path = argv[1];   /* ROM arrastada sobre o exe, ou primeiro argumento */
        first = 2;
    }
    int gui_defaults = argc <= 2;   /* so a ROM (ou nada): modo jogo, janela escala 3 */
    if (!rom_path) {
        if (!find_rom(found, sizeof found)) {
            fatal_msg("ROM not found.\nDrag the .gba file onto kirby.exe, or put "
                      "baserom.gba in the same folder.");
            return 1;
        }
        rom_path = found;
    }
    argv[1 - 1] = argv[0];
    FILE *f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc((size_t)n);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    gba_load_rom(rom, (uint32_t)n);
    hw_reset();
    const char *ppm = NULL, *sav = NULL;
    int window = 0, scale = 3, bench = 0, selftest = 0, scale_explicit = 0;
    const char *ini_arg = NULL;
    rt_opt_stop_at_vblank_wait = 1;
    if (gui_defaults) {
        window = 1;
        rt_opt_stop_at_vblank_wait = 0;
        remember_rom(rom_path);
    }
    hw_set_keys(0x03FF);
    for (int i = first; i < argc; i++) {
        if (!strcmp(argv[i], "--no-stop")) rt_opt_stop_at_vblank_wait = 0;
        else if (!strcmp(argv[i], "--window")) {
            window = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') { scale = atoi(argv[++i]); scale_explicit = 1; }
        }
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--scaling") && i + 1 < argc) {   /* integer | fit | stretch */
            extern int rt_scaling_override;
            const char *v = argv[++i];
            rt_scaling_override = !strcmp(v, "integer") ? 0 : !strcmp(v, "stretch") ? 2 : 1;
        }
        else if (!strcmp(argv[i], "--selftest-input")) {
            extern int rt_selftest_input_entry(void);
            return rt_selftest_input_entry() ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--selftest-video")) {
            extern int rt_selftest_video(void);
            return rt_selftest_video() ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--ini") && i + 1 < argc) ini_arg = argv[++i];
        else if (!strcmp(argv[i], "--selftest-sram")) selftest = 1;
        else if (!strcmp(argv[i], "--selftest-interp")) selftest = 2;
        else if (!strcmp(argv[i], "--selftest-ini")) { extern int rt_selftest_ini(void); return rt_selftest_ini() ? 1 : 0; }
        else if (!strcmp(argv[i], "--no-sram-alias")) { extern int rt_no_alias; rt_no_alias = 1; }
        else if (!strcmp(argv[i], "--interp-all")) { extern int rt_force_interp; rt_force_interp = 1; }
        else if (!strcmp(argv[i], "--no-overlays")) { extern int rt_skip_overlays; rt_skip_overlays = 1; }
        else if (!strcmp(argv[i], "--mute")) { extern int rt_mute; rt_mute = 1; }
        else if (!strcmp(argv[i], "--audio-raw") && i + 1 < argc) { extern const char *rt_audio_raw; rt_audio_raw = argv[++i]; }
        else if (!strcmp(argv[i], "--bench-from") && i + 1 < argc) { extern uint64_t rt_bench_from; rt_bench_from = (uint64_t)atoll(argv[++i]); }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) rt_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--unresolved") && i + 1 < argc) rt_unresolved_path = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) { if (rt_script_keys(argv[++i])) { fprintf(stderr, "--keys: invalid script"); return 1; } }
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
            snprintf(def, sizeof def, "%s", rom_path);
            char *dot = strrchr(def, '.');
            if (dot) strcpy(dot, ".sav");
            else strcat(def, ".sav");
            sav = def;
        }
        if (strcmp(sav, "none")) rt_script_sram(sav);
    }
    {   /* kirby.ini: --ini FILE | ao lado do save | ao lado da ROM */
        char ini[1100];
        if (ini_arg) snprintf(ini, sizeof ini, "%s", ini_arg);
        else {
            snprintf(ini, sizeof ini, "%s", strcmp(sav, "none") ? sav : rom_path);
            char *sl = strrchr(ini, '/');
            char *bs = strrchr(ini, '\\');
            if (bs && (!sl || bs > sl)) sl = bs;
            if (sl) strcpy(sl + 1, "kirby.ini");
            else strcpy(ini, "kirby.ini");
        }
        rt_cfg_load(ini);
        {   /* log do jogo ao lado do ini (hot-plug de controles etc.) */
            extern void rt_input_set_log_entry(const char *path);
            char lg[1100];
            snprintf(lg, sizeof lg, "%s", ini);
            char *e = strrchr(lg, '/');
            char *b2 = strrchr(lg, '\\');
            if (b2 && (!e || b2 > e)) e = b2;
            if (e) strcpy(e + 1, "kirby.log"); else strcpy(lg, "kirby.log");
            rt_input_set_log_entry(lg);
        }
        if (!scale_explicit) scale = rt_cfg_get(0);
        if (rt_cfg_get(5)) { extern int rt_mute; rt_mute = 1; }
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
                      : cpu.stop == RT_STOP_VBLANK_WAIT ? "first VBlankIntrWait"
                      : cpu.stop == RT_STOP_STOPCALL  ? "SWI Stop" : "erro";
    printf("RT_STOP %s pc=%08X lr=%08X sp=%08X calls=%llu\n", why, cpu.pc, cpu.r[14], cpu.r[13],
           (unsigned long long)rt_ncalls);
    { extern uint64_t rt_interp_entries, rt_interp_steps;
      printf("INTERPRETER: %llu entries, %llu instructions\n", (unsigned long long)rt_interp_entries,
             (unsigned long long)rt_interp_steps); }
    { extern uint64_t rt_sram_alias_hits; printf("SRAM routines on stack (ROM alias): %llu uses\n", (unsigned long long)rt_sram_alias_hits); }
    printf("IRQs delivered=%llu (BIOS_IF by handler=%llu, by runtime=%llu; IF cleared by "
           "runtime=%llu) frames_hw=%llu unresolved=%llu\n",
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
    return rt_exit_code ? rt_exit_code : (cpu.stop == RT_STOP_ERROR ? 2 : 0);
}
