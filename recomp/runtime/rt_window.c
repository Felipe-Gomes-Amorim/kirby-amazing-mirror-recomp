/* Janela, ritmo, audio e metricas do runtime (so no executavel com janela ou --bench).
 *   --window [escala]   abre a janela SDL2 (so no build com RT_WINDOW); ritmo 59,7275 quadros/s
 *   --bench             sem janela e sem limite de tempo: mede quadros/s e custo do hw_step
 * Teclas (hw/window_sdl.c): Z=A X=B Backspace=SELECT Enter=START setas S=R A=L Esc=fechar. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"
#ifdef RT_WINDOW
#include "window_sdl.h"
#endif

int hw_audio_read(int16_t *lr, int frames);
int hw_audio_rate(void);
/* quando a Gwen entregar a saida de audio, basta definir audio_push; ate la, descarta */
__attribute__((weak)) void audio_push(const int16_t *lr, int frames);

Cpu *rt_cpu; /* definido em rt_main.c */
uint64_t rt_hwstep_calls, rt_hwstep_cycles;
double rt_hwstep_us;

double rt_now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
#endif
}

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static int paced, use_window;
static double next_us;
static const double FRAME_US = 1e6 * 280896.0 / 16777216.0; /* 59,7275 quadros/s */
static void (*prev_hook)(uint64_t);
static uint64_t late_frames, audio_frames, total_frames;
static double t_start;

uint64_t rt_bench_from;
static double mark_us, mark_hw_us;
static uint64_t mark_frame;

static void win_hook(uint64_t frame) {
    total_frames = frame;
    if (rt_bench_from && frame == rt_bench_from) {
        mark_us = rt_now_us();
        mark_hw_us = rt_hwstep_us;
        mark_frame = frame;
    }
#ifdef RT_WINDOW
    if (use_window) {
        if (!win_poll()) {
            rt_cpu->stop = RT_STOP_FRAMES; /* usuario fechou: o save e gravado no atexit */
            return;
        }
        hw_set_keys(win_keys());
        const uint32_t *fb = hw_get_framebuffer();
        if (fb) win_present(fb);
    }
#endif
    /* audio: ~803 amostras por quadro a 48 kHz */
    static int16_t buf[4096 * 2];
    int n = hw_audio_read(buf, 4096);
    audio_frames += (uint64_t)n;
    if (n > 0 && audio_push) audio_push(buf, n);

    if (paced) { /* QueryPerformanceCounter: dorme so enquanto sobra tempo */
        double now = rt_now_us();
        if (next_us == 0) next_us = now;
        next_us += FRAME_US;
        if (now > next_us) {
            late_frames++;
            if (now - next_us > 5 * FRAME_US) next_us = now; /* ficou muito para tras: ressincroniza */
        }
        for (;;) {
            double rem = next_us - rt_now_us();
            if (rem <= 0) break;
            if (rem > 2000) sleep_ms(1);
        }
    }
    if (prev_hook) prev_hook(frame); /* roteiro --keys/--snap */
}

void rt_window_install(int window, int scale, int pace) {
    use_window = window;
    paced = pace;
    t_start = rt_now_us();
#ifdef RT_WINDOW
    if (window && win_open("Kirby & The Amazing Mirror", scale) < 0) {
        fprintf(stderr, "janela: falha ao abrir (SDL)\n");
        exit(1);
    }
#else
    if (window) {
        fprintf(stderr, "este executavel foi compilado sem janela (RT_WINDOW)\n");
        exit(1);
    }
#endif
    prev_hook = rt_frame_hook;
    rt_frame_hook = win_hook;
}

void rt_window_close(void) {
#ifdef RT_WINDOW
    if (use_window) win_close();
#endif
}

void rt_report_speed(void) {
    double s = (rt_now_us() - t_start) / 1e6;
    if (total_frames == 0 || s <= 0) return;
    printf("VELOCIDADE: %llu quadros em %.2fs = %.1f quadros/s (%.2fx tempo real)%s\n",
           (unsigned long long)total_frames, s, (double)total_frames / s,
           (double)total_frames / s / 59.7275, paced ? " [com ritmo]" : " [sem limite]");
    printf("hw_step: %.2fs (%.0f%% do tempo), %llu chamadas, %.0f ciclos/chamada; "
           "restante (codigo do jogo): %.2fs\n",
           rt_hwstep_us / 1e6, 100.0 * rt_hwstep_us / (s * 1e6), (unsigned long long)rt_hwstep_calls,
           rt_hwstep_calls ? (double)rt_hwstep_cycles / (double)rt_hwstep_calls : 0.0,
           s - rt_hwstep_us / 1e6);
    if (mark_frame && total_frames > mark_frame) {
        double ms = rt_now_us() - mark_us, hw = rt_hwstep_us - mark_hw_us;
        printf("TRECHO q.%llu..q.%llu: %.1f quadros/s (%.2fx), hw_step %.0f%% do tempo\n",
               (unsigned long long)mark_frame, (unsigned long long)total_frames,
               (double)(total_frames - mark_frame) / (ms / 1e6),
               (double)(total_frames - mark_frame) / (ms / 1e6) / 59.7275, 100.0 * hw / ms);
    }
    if (paced) printf("quadros atrasados (mais lento que o tempo real): %llu\n",
                      (unsigned long long)late_frames);
    printf("audio lido: %llu amostras estereo (%.1f s a 48 kHz)\n", (unsigned long long)audio_frames,
           (double)audio_frames / 48000.0);
}

/* --- diagnostico: acesso a regiao inexistente (uma vez so, com contexto) --- */
void gba_bus_unmapped(uint32_t addr, int size, int is_write) {
    static int n;
    if (n++) return;
    fprintf(stderr, "[bus] PRIMEIRO acesso sem regiao: %s%d em 0x%08X\n", is_write ? "write" : "read",
            size * 8, addr);
    if (!rt_cpu) return;
    Cpu *c = rt_cpu;
    fprintf(stderr, "[bus] lr=%08X sp=%08X modo=%s ultimo desvio-para-tras=%08X\n", c->r[14], c->r[13],
            c->thumb ? "thumb" : "arm", rt_loop_site);
    for (int i = 0; i < 13; i += 4)
        fprintf(stderr, "[bus] r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, c->r[i], i + 1,
                c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    fprintf(stderr, "[bus] pilha de retornos esperados (interna -> externa):");
    uint32_t d = rt_depth;
    for (uint32_t k = 0; k < 14 && k < d; k++) fprintf(stderr, " %08X", rt_rets[(d - 1 - k) & 4095u]);
    fprintf(stderr, "\n");
    rt_dump(c, "contexto do acesso sem regiao (pc exato nao e mantido; ver pilha acima)");
}
