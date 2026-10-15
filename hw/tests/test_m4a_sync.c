/* Teste da sequencia de reinicio do M4A (m4aSoundVSync) com DMA1 (A) e DMA2 (B) - independente de recomp/.
 *
 * Reproduz o que o jogo faz (valores lidos no log do hw): Timer0 reload -1064 (264 amostras/quadro,
 * 15768 Hz), buffers PCM de 1584 B por canal, reinicio a cada 6 quadros com
 *     DMAx CNT = 0x84400004 ; CNT_H = 0x0400 ; CNT_H = 0xB600     (DMA1 e DMA2, nesta ordem).
 * 6 quadros = 1584 amostras = exatamente o buffer; a FIFO pre-busca 16..32 B. Seja e_p = 1 se o
 * reinicio p chega DEPOIS do pedido de FIFO que cai logo apos o VBlank, 0 se antes. Entao o periodo
 * p->p+1 tem  99 - e_p + e_(p+1)  pedidos de 16 B:
 *     latencia constante  => 99 pedidos sempre (nada alem do fim, nada pulado);
 *     e: 0 -> 1           => 100 pedidos: le 16 B ALEM do fim do buffer (estalo);
 *     e: 1 -> 0           =>  98 pedidos: os ultimos 16 B do buffer nunca sao buscados (16 amostras puladas).
 * No jogo o buffer A e seguido do buffer B (amostras validas) e o buffer B e seguido de ponteiros de
 * funcao (49 F8 14 08 ...): o mesmo erro e inaudivel em A e vira estalo em B (canal esquerdo).
 *
 * Verifica: (1) A e B recebem pedidos nos mesmos instantes/offsets; (2) L==R em fase com latencia
 * constante, para qualquer valor; (3) com latencia que oscila em torno do pedido, a contagem de
 * pedidos segue a formula acima e o ponteiro aparece no L e nao no R; (4) hw_step_irq para
 * exatamente no IRQ. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../hw.h"

#define A_BASE 0x03000100u
#define B_BASE (A_BASE + 1584u)
#define BUF 1584
#define FRAME 280896u
#define VBLANK0 (160u * 1232u)
#define REQ_PHASE 896u            /* o pedido de FIFO cai 896 ciclos depois do VBlank (por construcao do teste) */

static uint8_t iw[0x8000];
static int is_hw(uint32_t a) { return (a >> 24) >= 4 && (a >> 24) <= 7; }
static uint32_t rd_log[1 << 17];
static int rd_n;
uint32_t gba_read8(uint32_t a)  { return is_hw(a) ? hw_read(a, 1) : iw[a & 0x7FFF]; }
uint32_t gba_read16(uint32_t a) { return is_hw(a) ? hw_read(a, 2) : (uint32_t)(iw[a & 0x7FFE] | iw[(a & 0x7FFE) + 1] << 8); }
uint32_t gba_read32(uint32_t a) {
    if (is_hw(a)) return hw_read(a, 4);
    if (rd_n < (int)(sizeof rd_log / sizeof rd_log[0])) rd_log[rd_n++] = a & ~3u;
    uint32_t o = a & 0x7FFC;
    return iw[o] | iw[o + 1] << 8 | iw[o + 2] << 16 | (uint32_t)iw[o + 3] << 24;
}
void gba_write8(uint32_t a, uint32_t v)  { if (is_hw(a)) hw_write(a, 1, v); }
void gba_write16(uint32_t a, uint32_t v) { if (is_hw(a)) hw_write(a, 2, v); }
void gba_write32(uint32_t a, uint32_t v) { if (is_hw(a)) hw_write(a, 4, v); }

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)

static int16_t out[2 * 80000];
static int nout;

static void step_collect(uint32_t cycles) {
    hw_step(cycles);
    nout += hw_audio_read(out + nout * 2, 80000 - nout);
}

typedef struct {
    int groups_equal;                    /* A e B pediram nos mesmos instantes e offsets */
    int over_a, over_b;                  /* pedidos com offset >= 1584 (a partir do periodo 2) */
    int short_a, short_b;                /* periodos com 98 pedidos */
    int l_has_73, r_has_73, l_eq_r;      /* do 2o reinicio em diante */
    int per_period[16];                  /* pedidos por periodo (canal A), a partir do reinicio 2 */
    int nper;
} Result;

/* lats[p-1] = latencia (ciclos apos o VBlank) do reinicio p */
static Result run(const uint32_t *lats, int periods) {
    Result r;
    memset(&r, 0, sizeof r);
    r.groups_equal = 1; r.l_eq_r = 1;
    rd_n = 0; nout = 0;
    for (int i = 0; i < BUF; i++) {                       /* conteudo igual em A e B, longe de 73 */
        int8_t v = (int8_t)((i * 7) % 61 - 30);
        iw[(A_BASE & 0x7FFF) + i] = (uint8_t)v;
        iw[(B_BASE & 0x7FFF) + i] = (uint8_t)v;
    }
    static const uint8_t sent[4] = { 0x49, 0xF8, 0x14, 0x08 };    /* "ponteiro" depois do buffer B */
    for (int i = 0; i < 256; i++) iw[(B_BASE & 0x7FFF) + BUF + i] = sent[i & 3];
    hw_reset();
    hw_write(0x04000084, 2, 0x0080);
    hw_write(0x04000082, 2, 0xA90E);
    hw_write(0x040000BC, 4, A_BASE); hw_write(0x040000C0, 4, 0x040000A0);
    hw_write(0x040000C8, 4, B_BASE); hw_write(0x040000CC, 4, 0x040000A4);
    hw_write(0x040000C4, 4, 0xB6000004u); hw_write(0x040000D0, 4, 0xB6000004u);
    hw_write(0x04000100, 2, (uint16_t)(0x10000 - 1064));
    uint32_t t_enable = VBLANK0 + REQ_PHASE - 1064;       /* 1o pop em VBLANK0 + 896 */
    step_collect(t_enable);
    hw_write(0x04000102, 2, 0x0080);
    uint32_t now = t_enable;
    int rd_mark = 0, out_start = 0;
    int req_before[17] = { 0 };
    for (int p = 1; p <= periods; p++) {
        uint32_t t_restart = VBLANK0 + 6u * FRAME * (uint32_t)p + lats[p - 1];
        step_collect(t_restart - now); now = t_restart;
        if (p >= 2) { req_before[p - 2] = rd_n / 8; }         /* pedidos A+B acumulados ate este reinicio */
        if (p == 2) { rd_mark = (rd_n / 8) * 8; out_start = nout; }
        for (int pass = 0; pass < 3; pass++)
            for (int n = 1; n <= 2; n++) {
                uint32_t cnt = 0x040000C4u + (n - 1) * 12u;
                if (pass == 0) hw_write(cnt, 4, 0x84400004u);
                if (pass == 1) hw_write(cnt + 2, 2, 0x0400);
                if (pass == 2) hw_write(cnt + 2, 2, 0xB600);
            }
    }
    /* pedidos: grupos de 4 leituras; grupos pares = A, impares = B (mesmo pop) */
    int groups = rd_n / 4;
    for (int g = 0; g + 1 < groups; g += 2) {
        uint32_t offa = rd_log[g * 4] - A_BASE, offb = rd_log[(g + 1) * 4] - B_BASE;
        if (offa != offb) r.groups_equal = 0;
        if (g * 4 >= rd_mark) {
            if (offa >= (uint32_t)BUF) r.over_a++;
            if (offb >= (uint32_t)BUF) r.over_b++;
        }
    }
    /* pedidos por periodo (do reinicio 2 ao ultimo): grupos A entre dois reinicios */
    for (int p = 0; p + 1 < periods - 1 && p < 15; p++) r.per_period[p] = req_before[p + 1] - req_before[p];
    r.nper = periods - 2;
    for (int p = 0; p < r.nper; p++) {
        if (r.per_period[p] == 98) { r.short_a++; r.short_b++; }
    }
    for (int i = out_start + 2000; i < nout; i++) {           /* da 100 ms para a cauda do 1o periodo sair */
        int L = out[i * 2], R = out[i * 2 + 1];
        if (L == 73 * 256) r.l_has_73 = 1;
        if (R == 73 * 256) r.r_has_73 = 1;
        if (L != R) r.l_eq_r = 0;
    }
    return r;
}

int main(void) {
    uint32_t lat[16];
    printf("== latencia constante (reinicio no mesmo ponto a cada 6 quadros) ==\n");
    printf("  lat  over_A over_B  98ped  L=73  R=73  L==R  grupos_iguais  pedidos/periodo\n");
    static const uint32_t consts[] = { 0, 300, 895, 896, 1000, 1231, 5000 };
    for (unsigned i = 0; i < sizeof consts / sizeof consts[0]; i++) {
        for (int p = 0; p < 16; p++) lat[p] = consts[i];
        Result r = run(lat, 10);
        printf("%5u  %5d  %5d  %5d  %4d  %4d  %4d  %6d        %d %d %d ...\n", consts[i], r.over_a, r.over_b, r.short_a,
               r.l_has_73, r.r_has_73, r.l_eq_r, r.groups_equal, r.per_period[0], r.per_period[1], r.per_period[2]);
        CHECK(r.groups_equal, "A e B pedem nos mesmos instantes e offsets");
        CHECK(r.over_a == 0 && r.over_b == 0, "latencia constante: nenhuma leitura alem do fim do buffer");
        CHECK(r.short_a == 0, "latencia constante: nenhum periodo com 98 pedidos");
        CHECK(!r.l_has_73 && !r.r_has_73, "latencia constante: sem ponteiro no audio");
        CHECK(r.l_eq_r, "latencia constante: L e R identicos (em fase)");
        for (int p = 0; p < r.nper - 1; p++) CHECK(r.per_period[p] == 99, "99 pedidos por periodo");
    }

    printf("== latencia que oscila em torno do pedido (como o runtime com IRQ atrasado) ==\n");
    for (int p = 0; p < 16; p++) lat[p] = (p & 1) ? 1000 : 0;       /* e: 0,1,0,1,... */
    Result j = run(lat, 12);
    printf("  over_A=%d over_B=%d 98ped=%d  L=73:%d R=73:%d  L==R:%d  grupos_iguais=%d  pedidos/periodo:", j.over_a, j.over_b,
           j.short_a, j.l_has_73, j.r_has_73, j.l_eq_r, j.groups_equal);
    for (int p = 0; p < j.nper - 1; p++) printf(" %d", j.per_period[p]);
    printf("\n");
    CHECK(j.groups_equal, "oscilando: A e B continuam pedindo nos mesmos instantes");
    CHECK(j.over_a == j.over_b && j.over_b > 0, "oscilando (0->1): leitura alem do fim, igual em A e B");
    CHECK(j.l_has_73 && !j.r_has_73, "oscilando: ponteiro (73) so no L; A le o inicio de B (inaudivel)");
    int saw100 = 0, saw98 = 0;
    for (int p = 0; p < j.nper - 1; p++) { if (j.per_period[p] == 100) saw100 = 1; if (j.per_period[p] == 98) saw98 = 1; }
    CHECK(saw100 && saw98, "oscilando: periodos de 100 (0->1) e 98 (1->0) pedidos, como medido no jogo (400/392 palavras)");

    /* hw_step_irq: para exatamente no VBlank */
    hw_reset();
    hw_write(0x04000004, 2, 0x0008);                   /* DISPSTAT: IRQ de VBlank */
    hw_write(0x04000200, 2, 0x0001);                   /* IE */
    hw_write(0x04000208, 2, 1);                        /* IME */
    hw_step(100000);
    uint32_t used = hw_step_irq(FRAME);
    CHECK(used == VBLANK0 - 100000u, "hw_step_irq devolve os ciclos ate o VBlank");
    CHECK((hw_irq_pending() & 1) != 0, "IRQ de VBlank pendente");
    used = hw_step_irq(1000);
    CHECK(used == 1000, "ja pendente: nao para de novo (so IRQ novo)");
    hw_write(0x04000202, 2, 1);                        /* IF w1c */
    used = hw_step_irq(FRAME);
    CHECK(used == FRAME - 1000u, "proximo VBlank exatamente 1 quadro depois");

    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
