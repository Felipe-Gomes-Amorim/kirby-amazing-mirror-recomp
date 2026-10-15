/* test_minimize.c - minimiza e restaura a janela SDL2 5x durante N s (padrao 60) enquanto o jogo
 * (aqui: onda PCM + cena de PPU) segue pacing pelo relogio do audio, como o runtime.
 * Verifica: nenhum present demora (minimizada ou nao), sem underruns acumulados, sem descartes,
 * o numero de quadros acompanha o relogio (sem salto ao restaurar).
 * Uso: test_minimize.exe [segundos]. Sem placa de som: sai com codigo 2 (use SDL_AUDIODRIVER=dummy). */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "../hw.h"
#include "../window_sdl.h"
#include "../audio_sdl.h"

static uint8_t ewram[0x40000];
static int is_hw(uint32_t a) { return (a >> 24) >= 4 && (a >> 24) <= 7; }
uint32_t gba_read8(uint32_t a)  { return is_hw(a) ? hw_read(a, 1) : ewram[a & 0x3FFFF]; }
uint32_t gba_read16(uint32_t a) { return is_hw(a) ? hw_read(a, 2) : (uint32_t)(ewram[a & 0x3FFFE] | ewram[(a & 0x3FFFE) + 1] << 8); }
uint32_t gba_read32(uint32_t a) { return is_hw(a) ? hw_read(a, 4) : gba_read16(a) | gba_read16(a + 2) << 16; }
void gba_write8(uint32_t a, uint32_t v)  { if (is_hw(a)) hw_write(a, 1, v); else ewram[a & 0x3FFFF] = (uint8_t)v; }
void gba_write16(uint32_t a, uint32_t v) {
    if (is_hw(a)) hw_write(a, 2, v);
    else { ewram[a & 0x3FFFE] = (uint8_t)v; ewram[(a & 0x3FFFE) + 1] = (uint8_t)(v >> 8); }
}
void gba_write32(uint32_t a, uint32_t v) {
    if (is_hw(a)) hw_write(a, 4, v); else { gba_write16(a, v & 0xFFFF); gba_write16(a + 2, v >> 16); }
}

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)

static double now_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 60.0;
    if (secs < 12) secs = 12;
    if (audio_open() != 0) { fprintf(stderr, "test_minimize: sem saida de audio (tente SDL_AUDIODRIVER=dummy)\n"); return 2; }
    if (win_open("hw minimize test", 3) != 0) return 1;

    hw_reset();
    for (int i = 0; i < 0x8000; i++) ewram[i] = (uint8_t)(int8_t)(((i / 16) & 1) ? -50 : 50);
    hw_write(0x04000084, 2, 0x0080); hw_write(0x04000082, 2, 0xA90E | 2);
    hw_write(0x040000BC, 4, 0x02000000); hw_write(0x040000C0, 4, 0x040000A0); hw_write(0x040000C4, 4, 0xB6000004u);
    hw_write(0x04000100, 2, (uint16_t)(0x10000 - 1064)); hw_write(0x04000102, 2, 0x0080);
    for (int i = 0; i < 16; i++) hw_write(0x05000000 + i * 32 + 2, 2, (uint32_t)(i * 1000 + 7));
    for (int r = 0; r < 8; r++) hw_write(0x06000000 + 32 + r * 4, 4, 0x11111111);
    for (int i = 0; i < 32 * 32; i++) hw_write(0x06004000 + i * 2, 2, (uint32_t)(1 | (i & 15) << 12));
    hw_write(0x04000008, 2, 0x0800);
    hw_write(0x04000000, 2, 0x0100);

    const double half = secs / 11.0 * 1000.0;     /* 11 intervalos => 5 minimizacoes + 5 restauracoes completas */
    double t0 = now_ms(), warm_end = t0 + 2000.0;
    int under0 = -1, frames = 0, frames_min = 0, toggles = 0, state_min = 0;
    double max_present = 0, max_present_min = 0, max_wait = 0;
    int16_t tmp[2 * 4096];
    while (now_ms() - t0 < secs * 1000.0) {
        double t = now_ms() - t0;
        int want_min = ((int)(t / half) & 1);                    /* alterna a cada intervalo */
        if (want_min != state_min) { win_test_minimize(want_min); state_min = want_min; toggles++; }
        if (!win_poll()) break;
        hw_set_keys(win_keys());
        hw_step(280896);
        const uint32_t *f = hw_frame_ready();
        double p0 = now_ms();
        win_present(f ? f : hw_get_framebuffer());
        double dp = now_ms() - p0;
        if (dp > max_present) max_present = dp;
        if (!win_visible()) { frames_min++; if (dp > max_present_min) max_present_min = dp; }
        audio_push(tmp, hw_audio_read(tmp, 4096));
        frames++;
        /* relogio do audio e o mestre (como o runtime): espera a fila drenar ate ~4 quadros */
        double w0 = now_ms();
        while (audio_queued() > 3200 && now_ms() - w0 < 100.0) { win_poll(); SDL_Delay(1); }
        if (now_ms() - w0 > max_wait) max_wait = now_ms() - w0;
        if (under0 < 0 && now_ms() > warm_end) under0 = audio_underruns();
    }
    double el = (now_ms() - t0) / 1000.0;
    int under = audio_underruns() - (under0 < 0 ? 0 : under0), dropped = audio_dropped();
    printf("%.1f s: %d quadros (%.2f q/s; esperado 59.73), %d ciclos min/rest (%d trocas), %d quadros minimizados\n",
           el, frames, frames / el, toggles / 2, toggles, frames_min);
    printf("present max=%.2f ms (minimizada: %.2f ms) ; espera pelo audio max=%.1f ms ; underruns apos 2 s=%d ; descartados=%d\n",
           max_present, max_present_min, max_wait, under, dropped);
    CHECK(toggles >= 10, "minimizou e restaurou 5 vezes");
    CHECK(frames_min > 0, "alguns quadros com a janela minimizada");
    CHECK(max_present < 50.0, "present nunca bloqueia (< 50 ms)");
    CHECK(under == 0, "sem underruns depois do aquecimento");
    CHECK(dropped == 0, "sem descartes por latencia");
    CHECK(max_wait < 90.0, "espera pelo audio nunca estoura o limite de 100 ms");
    CHECK(frames / el > 57.0 && frames / el < 62.0, "ritmo ~59,7 q/s (sem salto ao restaurar)");
    audio_close();
    win_close();
    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
