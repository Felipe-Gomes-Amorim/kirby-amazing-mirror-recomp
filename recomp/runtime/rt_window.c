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
#include <SDL.h>
#include "window_sdl.h"
#include "audio_sdl.h"
uint16_t rt_input_poll(void);
int rt_input_hotkeys(void);
void rt_input_init(void);
void rt_input_close(void);
#endif
int rt_cfg_get(int which);
int rt_scaling_override = -1;   /* --scaling integer|fit|stretch (0/1/2); -1 = kirby.ini */
int rt_exit_code;   /* 0 normal; 77 = F1 (voltar ao launcher) */

int hw_audio_read(int16_t *lr, int frames);
int hw_audio_rate(void);
/* quando a Gwen entregar a saida de audio, basta definir audio_push; ate la, descarta */
#ifndef RT_WINDOW
__attribute__((weak)) void audio_push(const int16_t *lr, int frames);
#endif
int rt_mute;
const char *rt_audio_raw;   /* --audio-raw FILE: PCM int16 estereo cru (diagnostico) */
static FILE *raw_f;
static int audio_on;

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

#ifdef RT_WINDOW
/* Modos de escala ([video] scaling): 0 integer = multiplos inteiros (bordas nos dois eixos se nao
 * couber exato); 1 fit = escala fracionaria preservando 3:2 (bordas so nas laterais num 16:9);
 * 2 stretch = preenche a janela inteira (distorce). window_sdl.c usa SetLogicalSize(240,160), que
 * ja da o "fit"; integer liga SetIntegerScale; stretch remove o tamanho logico. */
void rt_apply_scaling(SDL_Renderer *r, int mode) {
    if (mode == 2) {
        SDL_RenderSetIntegerScale(r, SDL_FALSE);
        SDL_RenderSetLogicalSize(r, 0, 0);
    } else {
        SDL_RenderSetLogicalSize(r, 240, 160);
        SDL_RenderSetIntegerScale(r, mode == 0 ? SDL_TRUE : SDL_FALSE);
    }
}

/* destino do quadro na janela W x H para cada modo (para a tabela de teste) */
static void dest_rect(SDL_Renderer *r, int *x, int *y, int *w, int *h) {
    int ow, oh;
    SDL_GetRendererOutputSize(r, &ow, &oh);
    int lw, lh;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    if (lw == 0 || lh == 0) { *x = 0; *y = 0; *w = ow; *h = oh; return; }
    int x1, y1, x2, y2;
    SDL_RenderLogicalToWindow(r, 0, 0, &x1, &y1);
    SDL_RenderLogicalToWindow(r, (float)lw, (float)lh, &x2, &y2);
    *x = x1; *y = y1; *w = x2 - x1; *h = y2 - y1;
}

int rt_selftest_video(void) {
    static const int sizes[2][2] = {{1920, 1080}, {1366, 768}};
    static const char *names[3] = {"integer", "fit", "stretch"};
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (win_open_ex("selftest", 1, 0) < 0) {
        fprintf(stderr, "[selftest-video] could not open a dummy window\n");
        return 1;
    }
    SDL_Window *wnd = SDL_GetWindowFromID(1);
    SDL_Renderer *r = wnd ? SDL_GetRenderer(wnd) : NULL;
    if (!r) { fprintf(stderr, "[selftest-video] no renderer\n"); return 1; }
    int bad = 0;
    for (int si = 0; si < 2; si++) {
        SDL_SetWindowSize(wnd, sizes[si][0], sizes[si][1]);
        for (int m = 0; m < 3; m++) {
            rt_apply_scaling(r, m);
            int x, y, w, h;
            dest_rect(r, &x, &y, &w, &h);
            /* esperado */
            int W = sizes[si][0], H = sizes[si][1], ex, ey, ew, eh;
            if (m == 2) { ex = 0; ey = 0; ew = W; eh = H; }
            else {
                double s = (double)W / 240 < (double)H / 160 ? (double)W / 240 : (double)H / 160;
                if (m == 0) s = (int)s;
                ew = (int)(240 * s + 0.5); eh = (int)(160 * s + 0.5);
                ex = (W - ew) / 2; ey = (H - eh) / 2;
            }
            int ok = abs(x - ex) <= 1 && abs(y - ey) <= 1 && abs(w - ew) <= 1 && abs(h - eh) <= 1;
            printf("[video] %dx%d %-7s -> dest x=%d y=%d w=%d h=%d  (expected %d,%d %dx%d) %s\n", W, H,
                   names[m], x, y, w, h, ex, ey, ew, eh, ok ? "OK" : "MISMATCH");
            bad += !ok;
        }
    }
    win_close();
    return bad;
}
#else
int rt_selftest_video(void) {
    fprintf(stderr, "this executable was built without a window (RT_WINDOW)\n");
    return 1;
}
#endif

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
        int hk = rt_input_hotkeys();
        {   /* gancho de teste: KIRBY_TEST_HOTKEY="F1@120" (ou F11@N) simula a tecla no quadro N */
            const char *t = getenv("KIRBY_TEST_HOTKEY");
            const char *at = t ? strchr(t, '@') : NULL;
            if (at && (uint64_t)atoll(at + 1) == frame) hk |= (t[1] == '1' && t[2] == '@') ? 1 : 2;
        }
        if (hk & 2) {   /* F11: alterna tela cheia */
            SDL_Window *wnd = SDL_GetWindowFromID(1);
            if (wnd) {
                Uint32 fl = SDL_GetWindowFlags(wnd);
                SDL_SetWindowFullscreen(wnd, (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0
                                                : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
        if (hk & 1) {   /* F1: volta ao launcher (codigo de saida 77); o save e gravado no atexit */
            { extern void rt_save_flush(const char *why, int force); rt_save_flush("F1", 0); }
            rt_exit_code = 77;
            rt_cpu->stop = RT_STOP_FRAMES;
            return;
        }
        hw_set_keys(rt_input_poll());
        const uint32_t *fb = hw_get_framebuffer();
        if (fb) win_present(fb);
    }
#endif
    /* audio: ~803 amostras por quadro a 48 kHz */
    static int16_t buf[4096 * 2];
    int n = hw_audio_read(buf, 4096);
    audio_frames += (uint64_t)n;
    if (rt_audio_raw && n > 0) {
        if (!raw_f) raw_f = fopen(rt_audio_raw, "wb");
        if (raw_f) fwrite(buf, 4, (size_t)n, raw_f);
    }
#ifdef RT_WINDOW
    if (n > 0 && audio_on) {
        int vol = rt_cfg_get(4);
        if (vol < 100)
            for (int i = 0; i < n * 2; i++) buf[i] = (int16_t)(buf[i] * vol / 100);
        audio_push(buf, n);
    }
#else
    if (n > 0 && audio_push) audio_push(buf, n);
#endif

#ifdef RT_WINDOW
    if (paced && audio_on) {
        /* RELOGIO DE AUDIO como mestre: espera a fila drenar ate ~3 quadros de video (~50 ms).
         * A placa consome a 48 kHz no relogio dela; o ritmo do jogo acompanha, entao a fila nunca
         * enche (sem descartes = sem estalos) nem esvazia. Limite de espera 100 ms (placa parada). */
        const int target = 3200;   /* ~4 quadros de video (67 ms) */
        double t0 = rt_now_us();
        while (audio_queued() > target) {
            double waited = rt_now_us() - t0;
            if (waited > 100000) break;
            int over = audio_queued() - target;
            if (over > 96) sleep_ms(1);   /* >2 ms a drenar: dorme; senao gira */
        }
        next_us = 0;
        {   /* diagnostico: em que quadro a fila esvaziou */
            static int last_u, logged;
            int u = audio_underruns();
            if (u != last_u && logged++ < 12)
                fprintf(stderr, "[audio] underrun at frame %llu (total %d, queue %d)\n",
                        (unsigned long long)frame, u, audio_queued());
            last_u = u;
        }
        goto paced_done;
    }
#endif
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
#ifdef RT_WINDOW
paced_done:
#endif
    if (prev_hook) prev_hook(frame); /* roteiro --keys/--snap */
}

void rt_window_install(int window, int scale, int pace) {
    use_window = window;
    { extern int rt_link_nopace; paced = pace && !rt_link_nopace; }
    t_start = rt_now_us();
#ifdef RT_WINDOW
    if (window && win_open_ex("Kirby & The Amazing Mirror", scale, rt_cfg_get(3)) < 0) {
        fprintf(stderr, "window: failed to open (SDL)\n");
        exit(1);
    }
    if (window) {   /* kirby.ini: tela cheia inicial e escala inteira */
        SDL_Window *wnd = SDL_GetWindowFromID(1);
        SDL_Renderer *rr = wnd ? SDL_GetRenderer(wnd) : NULL;
        if (rr) rt_apply_scaling(rr, rt_scaling_override >= 0 ? rt_scaling_override : rt_cfg_get(6));
        if (wnd && rt_cfg_get(1)) SDL_SetWindowFullscreen(wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
        rt_input_init();
    }
    if (window && !rt_mute && !rt_cfg_get(5)) {
        if (audio_open() < 0) fprintf(stderr, "audio: no output device (continuing without sound)\n");
        else audio_on = 1;
    }
#else
    if (window) {
        fprintf(stderr, "this executable was built without a window (RT_WINDOW)\n");
        exit(1);
    }
#endif
    prev_hook = rt_frame_hook;
    rt_frame_hook = win_hook;
}

void rt_window_close(void) {
    if (raw_f) fclose(raw_f);
#ifdef RT_WINDOW
    if (audio_on) {
        printf("AUDIO: underruns=%d dropped=%d (queue %d)\n", audio_underruns(), audio_dropped(),
               audio_queued());
        audio_close();
    }
    if (use_window) {
        rt_input_close();
        win_close();
    }
#endif
}

void rt_report_speed(void) {
    double s = (rt_now_us() - t_start) / 1e6;
    if (total_frames == 0 || s <= 0) return;
    printf("SPEED: %llu frames in %.2fs = %.1f frames/s (%.2fx real time)%s\n",
           (unsigned long long)total_frames, s, (double)total_frames / s,
           (double)total_frames / s / 59.7275, paced ? " [paced]" : " [unlimited]");
    printf("hw_step: %.2fs (%.0f%% of the time), %llu calls, %.0f cycles/call; "
           "rest (game code): %.2fs\n",
           rt_hwstep_us / 1e6, 100.0 * rt_hwstep_us / (s * 1e6), (unsigned long long)rt_hwstep_calls,
           rt_hwstep_calls ? (double)rt_hwstep_cycles / (double)rt_hwstep_calls : 0.0,
           s - rt_hwstep_us / 1e6);
    if (mark_frame && total_frames > mark_frame) {
        double ms = rt_now_us() - mark_us, hw = rt_hwstep_us - mark_hw_us;
        printf("SEGMENT f.%llu..f.%llu: %.1f frames/s (%.2fx), hw_step %.0f%% of the time\n",
               (unsigned long long)mark_frame, (unsigned long long)total_frames,
               (double)(total_frames - mark_frame) / (ms / 1e6),
               (double)(total_frames - mark_frame) / (ms / 1e6) / 59.7275, 100.0 * hw / ms);
    }
    if (paced) printf("late frames (slower than real time): %llu\n",
                      (unsigned long long)late_frames);
    printf("audio read: %llu stereo samples (%.1f s at 48 kHz)\n", (unsigned long long)audio_frames,
           (double)audio_frames / 48000.0);
}

/* --- diagnostico: acesso a regiao inexistente (uma vez so, com contexto) --- */
void gba_bus_unmapped(uint32_t addr, int size, int is_write) {
    static int n;
    extern int rt_quiet;
    if (rt_quiet || n++) return;
    fprintf(stderr, "[bus] FIRST access to an unmapped address: %s%d at 0x%08X\n", is_write ? "write" : "read",
            size * 8, addr);
    if (!rt_cpu) return;
    Cpu *c = rt_cpu;
    fprintf(stderr, "[bus] lr=%08X sp=%08X mode=%s last backward branch=%08X\n", c->r[14], c->r[13],
            c->thumb ? "thumb" : "arm", rt_loop_site);
    for (int i = 0; i < 13; i += 4)
        fprintf(stderr, "[bus] r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, c->r[i], i + 1,
                c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    fprintf(stderr, "[bus] expected-return stack (inner -> outer):");
    uint32_t d = rt_depth;
    for (uint32_t k = 0; k < 14 && k < d; k++) fprintf(stderr, " %08X", rt_rets[(d - 1 - k) & 4095u]);
    fprintf(stderr, "\n");
    {   /* informativo: o jogo segue (nao e parada) */
        extern const char *rt_dump_label;
        rt_dump_label = "WARNING";
        rt_dump(c, "context of the unmapped access (exact pc is not tracked; see the stack above)");
        rt_dump_label = "HALT";
    }
}

#ifdef RT_WINDOW
int rt_selftest_input(void);
void rt_input_set_log(const char *path);
int rt_selftest_input_entry(void) { return rt_selftest_input(); }
void rt_input_set_log_entry(const char *path) { rt_input_set_log(path); }
#else
int rt_selftest_input_entry(void) {
    fprintf(stderr, "this executable was built without a window (RT_WINDOW)\n");
    return 1;
}
void rt_input_set_log_entry(const char *path) { (void)path; }
#endif
