/* Teste do PSG (canais 1-4) - independente de recomp/.
 * Escala esperada: nivel(+-vol) * (vol_mestre+1=8) * razao(100%=4) * 32 = 1024 por unidade de nivel,
 * logo vol 15 => +-15360. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../hw.h"

uint32_t gba_read8(uint32_t a)  { (void)a; return 0; }
uint32_t gba_read16(uint32_t a) { (void)a; return 0; }
uint32_t gba_read32(uint32_t a) { (void)a; return 0; }
void gba_write8(uint32_t a, uint32_t v)  { hw_write(a, 1, v); }
void gba_write16(uint32_t a, uint32_t v) { hw_write(a, 2, v); }
void gba_write32(uint32_t a, uint32_t v) { hw_write(a, 4, v); }

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)
#define W16(a, v) hw_write(0x04000000u + (a), 2, (v))
#define R16(a) hw_read(0x04000000u + (a), 2)

static int16_t buf[48000 * 2 + 8192];
static int n;

static void run(double seconds) {
    n = 0;
    uint32_t total = (uint32_t)(seconds * 16777216.0), done = 0;
    while (done < total) {
        uint32_t c = total - done < 4000 ? total - done : 4000;
        hw_step(c); done += c;
        n += hw_audio_read(buf + n * 2, 48000 + 4096 - n);
    }
}
static void setup(int ch) {                 /* ch = 0..3 */
    hw_reset();
    W16(0x84, 0x0080);
    W16(0x80, (uint16_t)((0x100 << ch) | (0x1000 << ch) | 0x77));
    W16(0x82, 0x0002);                       /* PSG 100% */
}
static int peak(double t0, double t1) {      /* |R| maximo na janela [t0,t1] s */
    int m = 0;
    for (int i = (int)(t0 * 48000); i < (int)(t1 * 48000) && i < n; i++) if (abs(buf[i * 2 + 1]) > m) m = abs(buf[i * 2 + 1]);
    return m;
}
static int crossings(double t0, double t1) {
    int c = 0;
    for (int i = (int)(t0 * 48000) + 1; i < (int)(t1 * 48000) && i < n; i++)
        if (buf[(i - 1) * 2 + 1] <= 0 && buf[i * 2 + 1] > 0) c++;
    return c;
}

int main(void) {
    /* T1: canal 1, duty 50%, vol 15 fixo, f=1750 -> 131072/298 = 439,84 Hz */
    setup(0);
    W16(0x62, 0xF080);
    W16(0x64, 1750 | 0x8000);
    CHECK((R16(0x84) & 1) == 1, "T1: status do canal 1 ligado");
    run(1.0);
    int c1 = crossings(0.0, 1.0);
    printf("T1: %d ciclos (esperado 439,8), pico=%d\n", c1, peak(0, 1));
    CHECK(c1 >= 439 && c1 <= 441, "T1: frequencia do pulso");
    CHECK(peak(0.1, 0.9) == 15360, "T1: amplitude +-15360");
    int lr_eq = 1;
    for (int i = 0; i < n; i++) if (buf[i * 2] != buf[i * 2 + 1]) lr_eq = 0;
    CHECK(lr_eq, "T1: L == R (ambos habilitados, mesmo volume)");

    /* T2: envelope decrescente, passo 1 (64 Hz): vol cai 1 a cada 15,6 ms; 1o passo em ~13,7 ms */
    setup(0);
    W16(0x62, 0xF000 | 0x0080 | 0x0100);
    W16(0x64, 1750 | 0x8000);
    run(0.5);
    int p0 = peak(0.0, 0.010), p1 = peak(0.095, 0.105), p2 = peak(0.30, 0.35);
    printf("T2: pico 0-10ms=%d, ~100ms=%d (esperado 9216), 300-350ms=%d\n", p0, p1, p2);
    CHECK(p0 == 15360, "T2: vol inicial 15");
    CHECK(p1 == 9 * 1024, "T2: vol 9 apos 6 passos de envelope");
    CHECK(p2 == 0, "T2: envelope chegou a 0");

    /* T3: length: contador 32 a 256 Hz = 125 ms; bit14 liga o corte */
    setup(0);
    W16(0x62, 0xF080 | 32);
    W16(0x64, 1750 | 0x4000 | 0x8000);
    run(0.3);
    CHECK(peak(0.02, 0.10) == 15360, "T3: toca antes do fim do length");
    CHECK(peak(0.14, 0.30) == 0, "T3: silencio apos o length");
    CHECK((R16(0x84) & 1) == 0, "T3: status desliga no fim do length");

    /* T4: sweep +1/2 a cada 7,8 ms: 1000 -> 1500 -> 2250 > 2047 => canal desliga */
    setup(0);
    W16(0x60, 0x11);                         /* shift 1, subir, tempo 1 */
    W16(0x62, 0xF080);
    W16(0x64, 1000 | 0x8000);
    run(0.1);
    CHECK((R16(0x84) & 1) == 0, "T4: sweep estourou 2047 e desligou o canal");
    CHECK(peak(0.08, 0.10) == 0, "T4: silencio depois do estouro");

    /* T5: canal 2 (regs 0x68/0x6C) com o mesmo som do T1 */
    setup(1);
    W16(0x68, 0xF080);
    W16(0x6C, 1750 | 0x8000);
    run(1.0);
    int c2 = crossings(0.0, 1.0);
    printf("T5: canal 2: %d ciclos\n", c2);
    CHECK(c2 >= 439 && c2 <= 441, "T5: frequencia do pulso no canal 2");
    CHECK(peak(0.1, 0.9) == 15360, "T5: amplitude");

    /* T6: canal 3 (onda): 16 amostras 15 + 16 amostras 0; f=1900 -> 65536/148 = 442,8 Hz */
    setup(2);
    W16(0x70, 0x40);                         /* toca banco 1; a CPU escreve no banco 0 */
    for (int i = 0; i < 16; i++) hw_write(0x04000090u + i, 1, i < 8 ? 0xFF : 0x00);
    W16(0x70, 0x80);                         /* toca banco 0, DAC ligado */
    CHECK(hw_read(0x04000090, 4) == 0, "T6: WAVE_RAM da CPU e o banco que nao toca");
    W16(0x72, 0x2000);                       /* volume 100% */
    W16(0x74, 1900 | 0x8000);
    run(1.0);
    int c3 = crossings(0.0, 1.0);
    printf("T6: onda: %d ciclos (esperado 442,8), pico=%d\n", c3, peak(0, 1));
    CHECK(c3 >= 442 && c3 <= 444, "T6: frequencia da onda");
    CHECK(peak(0.1, 0.9) == 15360, "T6: amplitude da onda");

    /* T7: ruido, vol 15, r=1, s=2 : so confere que alterna e usa +-vol */
    setup(3);
    W16(0x78, 0xF000);
    W16(0x7C, (1 | 2 << 4) | 0x8000);
    run(0.2);
    int tr = 0, mx = -32768, mn = 32767;
    for (int i = 1; i < n; i++) {
        int a = buf[(i - 1) * 2 + 1], b = buf[i * 2 + 1];
        if (a != b) tr++;
        if (b > mx) mx = b;
        if (b < mn) mn = b;
    }
    printf("T7: ruido: %d transicoes em %d amostras, max=%d min=%d\n", tr, n, mx, mn);
    CHECK(tr > 500, "T7: ruido alterna");
    CHECK(mx == 15360 && mn == -15360, "T7: niveis +-vol");

    /* T8: master off => status 0 e silencio */
    W16(0x84, 0);
    CHECK((R16(0x84) & 0x0F) == 0, "T8: master off desliga canais");

    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
