/* Autoteste do SIO multi-play: --selftest-link (sem ROM, sem SDL). Varias instancias de Sio no
 * MESMO processo, ligadas por transporte "null", "hub" (em processo) e "local" (TCP 127.0.0.1). */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(n) Sleep(n)
#else
#include <unistd.h>
#define SLEEP_MS(n) usleep((n) * 1000)
#endif
#include "gba_bus.h"
#include "rt_sio.h"

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; fprintf(stderr, "[selftest-link] FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint32_t rd(Sio *s, uint32_t off, int size) {
    uint32_t v = 0xDEADBEEF;
    rt_sio_read(s, 0x04000000u + off, size, &v);
    return v;
}
static void wr(Sio *s, uint32_t off, int size, uint32_t v) { rt_sio_write(s, 0x04000000u + off, size, v); }

#define SIOCNT 0x128
#define SEND 0x12A
#define MULTI(i) (0x120 + 2 * (i))
#define MODE_MULTI_115200 0x2003
#define START 0x0080
#define IRQEN 0x4000

/* roda ate nenhuma instancia estar ocupada (ou estourar o limite) */
static int run_until_idle(Sio **s, int n, int net) {
    for (int it = 0; it < 4000; it++) {
        int busy = 0;
        for (int i = 0; i < n; i++) {
            rt_sio_poll(s[i]);
            if (rt_sio_cycles_to_event(s[i])) busy = 1;
        }
        if (!busy && it > (net ? 20 : 0)) return 1;
        for (int i = 0; i < n; i++) rt_sio_advance(s[i], 2000);
        if (net) SLEEP_MS(1);
    }
    return 0;
}

static void setup_modes(Sio **s, int n) {
    for (int i = 0; i < n; i++) { wr(s[i], 0x134, 2, 0); wr(s[i], SIOCNT, 2, MODE_MULTI_115200 | IRQEN); }   /* o jogo escreve RCNT=0 (reset e 0x8000) */
}

static void round_check(const char *tag, Sio **s, int n, const uint16_t want[4], int net) {
    if (net)   /* filhos entregam o ACK da rodada anterior (o mestre espera por ele) */
        for (int k = 0; k < 3; k++) { for (int i = 1; i < n; i++) rt_sio_poll(s[i]); SLEEP_MS(2); }
    /* mestre inicia; os filhos devem ficar ocupados (net: depois de um poll) */
    wr(s[0], SIOCNT, 2, MODE_MULTI_115200 | IRQEN | START);
    CHECK(rd(s[0], SIOCNT, 2) & START, "%s: master busy right after Start", tag);
    CHECK(rd(s[0], MULTI(0), 2) == 0xFFFF, "%s: SIOMULTI cleared to FFFF at start", tag);
    for (int w = 0; w < 200 && net; w++) {   /* TCP: espera a mensagem chegar */
        int all = 1;
        for (int i = 1; i < n; i++) { rt_sio_poll(s[i]); if (!(rd(s[i], SIOCNT, 2) & START)) all = 0; }
        if (all) break;
        SLEEP_MS(1);
    }
    for (int i = 1; i < n; i++) {
        rt_sio_poll(s[i]);
        CHECK(rd(s[i], SIOCNT, 2) & START, "%s: child %d busy during the exchange", tag, i);
    }
    CHECK(run_until_idle(s, n, net), "%s: exchange finishes", tag);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            CHECK(rd(s[i], MULTI(k), 2) == want[k], "%s: player %d SIOMULTI%d=%04X want %04X", tag, i, k,
                  rd(s[i], MULTI(k), 2), want[k]);
    for (int i = 0; i < n; i++) {
        uint32_t c = rd(s[i], SIOCNT, 2);
        CHECK(!(c & START), "%s: player %d idle after the exchange", tag, i);
        CHECK(((c >> 4) & 3) == (uint32_t)i, "%s: player %d ID bits = %u", tag, i, (c >> 4) & 3);
        CHECK(((c >> 2) & 1) == (i ? 1u : 0u), "%s: player %d SI = %u", tag, i, (c >> 2) & 1);
        CHECK(!(c & 0x40), "%s: no error bit", tag);
        CHECK((c & 0x7003) == (MODE_MULTI_115200 | IRQEN), "%s: mode/baud/irq kept (%04X)", tag, c & 0x7003);
        CHECK(rt_sio_irq_flag(s[i]), "%s: player %d serial IRQ flagged", tag, i);
        CHECK(rd(s[i], 0x202, 2) & 0x80, "%s: player %d IF bit 7 visible", tag, i);
        wr(s[i], 0x202, 2, 0x80);   /* IF w1c */
        CHECK(!rt_sio_irq_flag(s[i]) && !(rd(s[i], 0x202, 2) & 0x80), "%s: IF bit 7 cleared by w1c", tag);
    }
}

static void test_null(void) {
    Sio *s = rt_sio_new(NULL);
    Sio *a[1] = {s};
    CHECK(rd(s, SIOCNT, 2) == 0, "null: SIOCNT resets to 0 (SI=0 parent, SD=0 not connected)");
    CHECK(rd(s, 0x134, 2) == 0x8000, "null: RCNT resets to 0x8000 (BIOS state after a cartridge boot)");
    wr(s, 0x134, 2, 0);
    wr(s, SIOCNT, 2, MODE_MULTI_115200);
    wr(s, SEND, 2, 0x1234);
    wr(s, SIOCNT, 2, MODE_MULTI_115200 | START);
    CHECK(rd(s, SIOCNT, 2) & START, "null: busy after Start");
    rt_sio_advance(s, 5000);
    CHECK(rd(s, SIOCNT, 2) & START, "null: still busy after 5000 cycles (115200 baud ~ 10.5k)");
    CHECK(run_until_idle(a, 1, 0), "null: finishes");
    CHECK(rd(s, MULTI(0), 2) == 0x1234 && rd(s, MULTI(1), 2) == 0xFFFF && rd(s, MULTI(3), 2) == 0xFFFF,
          "null: own value in SIOMULTI0, absent players FFFF");
    CHECK(!rt_sio_irq_flag(s), "null: no IRQ when the enable bit is off");
    CHECK(((rd(s, SIOCNT, 2) >> 3) & 1) == 0, "null: SD = 0 (cable not connected)");
    /* IRQ habilitado */
    wr(s, SIOCNT, 2, MODE_MULTI_115200 | IRQEN | START);
    run_until_idle(a, 1, 0);
    CHECK(rt_sio_irq_flag(s), "null: IRQ when enabled");
    /* modo normal (nao multi-play): Start nao inicia troca */
    wr(s, 0x202, 2, 0x80);
    wr(s, SIOCNT, 2, 0x0000 | START);
    CHECK(!(rd(s, SIOCNT, 2) & START), "null: Start ignored outside multi-play mode");
    /* RCNT bit 15 = modo geral: tambem desliga */
    wr(s, 0x134, 2, 0x8000);
    wr(s, SIOCNT, 2, MODE_MULTI_115200 | START);
    CHECK(!(rd(s, SIOCNT, 2) & START), "null: RCNT general-purpose mode disables SIO");
    wr(s, 0x134, 2, 0x0000);
    CHECK(rd(s, 0x134, 2) == 0, "null: RCNT readback");
    /* acesso por byte e por palavra */
    wr(s, SIOCNT, 4, MODE_MULTI_115200 | ((uint32_t)0x5678 << 16));
    CHECK(rd(s, SEND, 2) == 0x5678, "null: 32-bit write covers SIOCNT + SIOMLT_SEND");
    CHECK(rd(s, SEND, 1) == 0x78 && rd(s, SEND + 1, 1) == 0x56, "null: byte reads of SIOMLT_SEND");
    rt_sio_free(s);
}

static void test_hub(int players) {
    LinkTransport *t[4];
    link_hub_create(players, t);
    Sio *s[4];
    for (int i = 0; i < players; i++) s[i] = rt_sio_new(t[i]);
    setup_modes(s, players);
    wr(s[0], SEND, 2, 0x1000);
    for (int i = 1; i < players; i++) wr(s[i], SEND, 2, (uint32_t)(0x2000 + i));
    uint16_t want[4] = {0x1000, 0xFFFF, 0xFFFF, 0xFFFF};
    for (int i = 1; i < players; i++) want[i] = (uint16_t)(0x2000 + i);
    char tag[32];
    snprintf(tag, sizeof tag, "hub%d round1", players);
    for (int i = 0; i < players; i++)
        CHECK(((rd(s[i], SIOCNT, 2) >> 3) & 1) == 1, "hub%d: SD=1 when all are connected", players);
    round_check(tag, s, players, want, 0);
    /* 2a troca com valores novos */
    wr(s[0], SEND, 2, 0xAAAA);
    want[0] = 0xAAAA;
    if (players > 1) { wr(s[1], SEND, 2, 0xBBBB); want[1] = 0xBBBB; }
    snprintf(tag, sizeof tag, "hub%d round2", players);
    round_check(tag, s, players, want, 0);
    /* filho tentando iniciar: ignorado */
    wr(s[1], SIOCNT, 2, MODE_MULTI_115200 | IRQEN | START);
    CHECK(!(rd(s[1], SIOCNT, 2) & START), "hub%d: a child cannot start an exchange", players);
    if (players >= 3) {   /* soltar o cabo do jogador 3: slot vira FFFF e SD cai */
        link_hub_disconnect(t[players - 1]);
        want[players - 1] = 0xFFFF;
        snprintf(tag, sizeof tag, "hub%d unplugged", players);
        for (int i = 0; i < players - 1; i++)
            CHECK(((rd(s[i], SIOCNT, 2) >> 3) & 1) == 0, "hub%d: SD drops when a player leaves", players);
        round_check(tag, s, players - 1, want, 0);
    }
    for (int i = 0; i < players; i++) rt_sio_free(s[i]);
}

static void test_tcp(void) {
    const int port = 47911;
    LinkTransport *h = link_local_host(3, port);
    if (!h) { bad++; fprintf(stderr, "[selftest-link] FAIL: could not listen on %d\n", port); return; }
    LinkTransport *c1 = link_local_join("127.0.0.1", port);
    Sio *s[3];
    s[0] = rt_sio_new(h);
    s[1] = rt_sio_new(c1);
    setup_modes(s, 2);
    for (int w = 0; w < 3000; w++) {   /* conecta o 1o filho */
        rt_sio_poll(s[0]); rt_sio_poll(s[1]);
        if (c1->player_id == 1) break;
        SLEEP_MS(1);
    }
    CHECK(c1->player_id == 1, "tcp: first child got id 1");
    LinkTransport *c2 = link_local_join("127.0.0.1", port);
    s[2] = rt_sio_new(c2);
    wr(s[2], 0x134, 2, 0);
    wr(s[2], SIOCNT, 2, MODE_MULTI_115200 | IRQEN);
    for (int w = 0; w < 3000; w++) {
        rt_sio_poll(s[0]); rt_sio_poll(s[1]); rt_sio_poll(s[2]);
        if (c2->player_id == 2) break;
        SLEEP_MS(1);
    }
    CHECK(c2->player_id == 2, "tcp: second child got id 2");
    wr(s[0], SEND, 2, 0x1111);
    wr(s[1], SEND, 2, 0x2222);
    wr(s[2], SEND, 2, 0x3333);
    for (int w = 0; w < 50; w++) { for (int i = 0; i < 3; i++) rt_sio_poll(s[i]); SLEEP_MS(1); }   /* SEND chega ao host */
    CHECK(((rd(s[0], SIOCNT, 2) >> 3) & 1) == 1, "tcp: SD=1 with 3 of 3 connected");
    uint16_t want[4] = {0x1111, 0x2222, 0x3333, 0xFFFF};
    round_check("tcp round1", s, 3, want, 1);
    wr(s[2], SEND, 2, 0x4444);
    for (int w = 0; w < 50; w++) { for (int i = 0; i < 3; i++) rt_sio_poll(s[i]); SLEEP_MS(1); }
    want[2] = 0x4444;
    round_check("tcp round2", s, 3, want, 1);
    /* filho 1 sai: o host percebe e o slot vira FFFF */
    rt_sio_free(s[1]);
    for (int w = 0; w < 100; w++) { rt_sio_poll(s[0]); rt_sio_poll(s[2]); SLEEP_MS(1); }
    want[1] = 0xFFFF;
    Sio *rest[2] = {s[0], s[2]};
    CHECK(((rd(s[0], SIOCNT, 2) >> 3) & 1) == 0, "tcp: SD=0 after a child left");
    wr(s[0], SIOCNT, 2, MODE_MULTI_115200 | IRQEN | START);
    for (int w = 0; w < 100; w++) { rt_sio_poll(s[2]); if (rd(s[2], SIOCNT, 2) & START) break; SLEEP_MS(1); }
    run_until_idle(rest, 2, 1);
    CHECK(rd(s[0], MULTI(1), 2) == 0xFFFF && rd(s[0], MULTI(2), 2) == 0x4444 && rd(s[2], MULTI(1), 2) == 0xFFFF,
          "tcp: left player's slot is FFFF, the remaining child still gets the round");
    rt_sio_free(s[0]);
    rt_sio_free(s[2]);
}

int rt_selftest_link(void) {
    bad = 0;
    test_null();
    test_hub(2);
    test_hub(3);
    test_hub(4);
    test_tcp();
    fprintf(stderr, "[selftest-link] %s\n", bad ? "FAILED" : "OK");
    return bad;
}
