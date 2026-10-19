/* rt_sio.c - ver rt_sio.h. SIO multi-play (semantica geral do GBATEK) + transportes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sched.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#include "gba_bus.h"
#include "rt_sio.h"

uint64_t rt_sio_exchanges;
extern void rt_log(const char *fmt, ...);

/* =============================================================== emulacao do SIO */
struct Sio {
    LinkTransport *tp;       /* NULL = cabo desconectado */
    uint16_t siocnt;         /* bits gravaveis: baud(0-1), modo(12-13), IRQ(14) */
    uint16_t rcnt, send, multi[4];
    int busy;
    uint32_t remain;         /* ciclos ate o fim da troca */
    uint16_t pending[4];
    int id_latched;          /* bits 4-5 do SIOCNT (so valem apos a 1a troca) */
    int irq_flag;            /* IF bit 7 */
    int ack_pending;         /* filho: avisar o mestre (depois do IRQ servido) que a troca acabou */
    /* lockstep por quadro */
    uint32_t frame, fcyc;    /* quadro do hw e ciclos desde o inicio dele (so avancam com o relogio emulado) */
    LinkRound rounds[LINK_MAX_ROUNDS]; int nrounds;   /* mestre: trocas deste quadro */
    LinkRound sched[LINK_MAX_ROUNDS]; int nsched, isched;   /* filho: trocas do mestre a reproduzir */
    uint16_t sendlog[LINK_MAX_ROUNDS]; int nsendlog;   /* filho: SIOMLT_SEND no inicio de cada troca do quadro */
    uint64_t xhash, xcount;  /* hash FNV-1a (quadro, ciclo, 4 halfwords) de cada troca concluida */
    /* validacao do protocolo do jogo (so estatistica): FEFE = sincronismo, depois 12 halfwords cuja soma
     * (16 bits) tem que ser 0xFFF1 (= -15) */
    int pk_idx[4];
    uint16_t pk_buf[4][12];
    uint32_t pk_sum[4];
    uint32_t pk_last0[4];
    uint64_t pk_valid[4], pk_invalid[4], pk_sync[4];
};

static const uint32_t BAUD[4] = {9600, 38400, 57600, 115200};

Sio *rt_sio_new(LinkTransport *t) {
    Sio *s = (Sio *)calloc(1, sizeof *s);
    s->tp = t;
    s->rcnt = 0x8000;   /* reset da BIOS apos o boot por cartucho (MainInit testa (RCNT & 0xC000) != 0x8000) */
    for (int i = 0; i < 4; i++) s->multi[i] = LINK_ABSENT;
    s->xhash = 1469598103934665603ull;
    if (t && t->lockstep && t->player_id != 0 && t->frame_begin) {   /* filho: so roda o quadro 0 com o do host */
        s->nsched = t->frame_begin(t, 0, s->sched, LINK_MAX_ROUNDS);
        if (s->nsched < 0) s->nsched = 0;
    }
    return s;
}
void rt_sio_free(Sio *s) {
    if (!s) return;
    if (s->tp && s->tp->close) s->tp->close(s->tp);
    free(s);
}

static int is_master(const Sio *s) { return !s->tp || s->tp->player_id == 0; }
static int multiplay(const Sio *s) { return ((s->rcnt >> 15) & 1) == 0 && ((s->siocnt >> 12) & 3) == 2; }

/* 4 jogadores x (1 start + 16 dados + 1 stop) bits ao baud configurado, em ciclos de 16,78 MHz */
static uint32_t exchange_cycles(const Sio *s) {
    return (uint32_t)(72ull * 16777216ull / BAUD[s->siocnt & 3]);
}

static void begin(Sio *s, const uint16_t vals[4]) {
    for (int i = 0; i < 4; i++) { s->multi[i] = LINK_ABSENT; s->pending[i] = vals[i]; }
    s->busy = 1;
    s->remain = exchange_cycles(s);
}

static void packet_stats(Sio *s) {
    static int ex_log = -1;
    if (ex_log < 0) { const char *e = getenv("KIRBY_SIO_EX"); ex_log = e ? atoi(e) : 0; }
    if (ex_log > 0 && rt_sio_exchanges >= (uint64_t)(getenv("KIRBY_SIO_EX_FROM") ? atoi(getenv("KIRBY_SIO_EX_FROM")) : 0)) {
        ex_log--;
        rt_log("[ex #%llu] %04X %04X %04X %04X", (unsigned long long)rt_sio_exchanges, s->pending[0], s->pending[1],
               s->pending[2], s->pending[3]);
    }
    for (int p = 0; p < 4; p++) {
        uint16_t v = s->pending[p];
        if (v == 0xFEFE) { s->pk_idx[p] = 0; s->pk_sum[p] = 0; s->pk_sync[p]++; continue; }
        if (v == LINK_ABSENT) { if (s->pk_idx[p] >= 0 && s->pk_idx[p] > 0) { s->pk_invalid[p]++; } s->pk_idx[p] = -1; continue; }
        if (s->pk_idx[p] < 0 || s->pk_idx[p] >= 12) continue;
        s->pk_sum[p] += v;
        s->pk_buf[p][s->pk_idx[p]] = v;
        if (++s->pk_idx[p] == 12) {
            int ok = (s->pk_sum[p] & 0xFFFF) == 0xFFF1;
            if (ok) s->pk_valid[p]++; else s->pk_invalid[p]++;
            static int pkt_log = -1;
            if (pkt_log < 0) { const char *e = getenv("KIRBY_SIO_PKT"); pkt_log = e ? atoi(e) : 0; }
            if (pkt_log > 0 && (!ok || (s->pk_buf[p][0] & 0xFF) != s->pk_last0[p])) {   /* so mudancas de tipo e invalidos */
                pkt_log--;
                s->pk_last0[p] = s->pk_buf[p][0] & 0xFF;
                rt_log("[pkt] from P%d %s:%s", p + 1, ok ? "valid" : "INVALID", "");
                char line[200] = "";
                for (int k = 0; k < 12; k++) snprintf(line + strlen(line), sizeof line - strlen(line), " %04X", s->pk_buf[p][k]);
                rt_log("[pkt]   %s  (exchange #%llu)", line, (unsigned long long)rt_sio_exchanges);
            }
            s->pk_idx[p] = -1;
        }
    }
}

static void complete(Sio *s) {
    memcpy(s->multi, s->pending, sizeof s->multi);
    packet_stats(s);
    s->busy = 0;
    s->remain = 0;
    s->id_latched = s->tp ? s->tp->player_id & 3 : 0;
    rt_sio_exchanges++;
    {   /* hash do fluxo de halfwords: duas execucoes iguais tem que dar o mesmo valor */
        uint32_t w[2] = {s->frame, s->fcyc};
        const uint8_t *b[2] = {(const uint8_t *)w, (const uint8_t *)s->multi};
        for (int k = 0; k < 2; k++)
            for (size_t i = 0; i < 8; i++) s->xhash = (s->xhash ^ b[k][i]) * 1099511628211ull;
        s->xcount++;
        static FILE *xf; static int xf_init;
        if (!xf_init) { xf_init = 1; const char *e = getenv("KIRBY_SIO_LOG"); if (e && *e) xf = fopen(e, "w"); }
        if (xf) fprintf(xf, "%u %u %04X %04X %04X %04X\n", s->frame, s->fcyc, s->multi[0], s->multi[1], s->multi[2], s->multi[3]);
    }
    if (s->siocnt & 0x4000) s->irq_flag = 1;
    if (s->tp && s->tp->ack && s->tp->player_id != 0) s->ack_pending = 1;
}

void rt_sio_advance(Sio *s, uint32_t cycles) {
    s->fcyc += cycles;
    if (s->busy) {
        if (cycles >= s->remain) complete(s);
        else s->remain -= cycles;
    }
    /* filho em lockstep: reproduz as trocas que o mestre fez neste quadro, no mesmo deslocamento */
    while (s->isched < s->nsched && s->fcyc >= s->sched[s->isched].off && !s->busy) {
        const LinkRound *r = &s->sched[s->isched++];
        if (s->nsendlog < LINK_MAX_ROUNDS) s->sendlog[s->nsendlog++] = s->send;   /* uma por troca (mesmo as puladas) */
        if (multiplay(s)) begin(s, r->v);
    }
}
uint32_t rt_sio_cycles_to_event(Sio *s) {
    uint32_t e = s->busy ? (s->remain ? s->remain : 1) : 0;
    if (!s->busy && s->isched < s->nsched) {
        uint32_t off = s->sched[s->isched].off;
        uint32_t d = off > s->fcyc ? off - s->fcyc : 1;
        if (!e || d < e) e = d;
    }
    return e;
}
int rt_sio_irq_flag(Sio *s) { return s->irq_flag; }

void rt_sio_poll(Sio *s) {
    if (!s->tp || !s->tp->poll) return;
    LinkEvent ev;
    while (s->tp->poll(s->tp, &ev)) {
        if (ev.type == 1 && !s->busy && multiplay(s)) begin(s, ev.values);   /* filho: o mestre iniciou */
    }
    /* poll roda no inicio da iteracao seguinte a conclusao: o IRQ serial ja foi entregue e o handler
     * (que escreve o proximo SIOMLT_SEND) ja terminou. So agora o mestre pode iniciar a proxima troca. */
    if (s->ack_pending && !s->busy) {
        s->ack_pending = 0;
        s->tp->ack(s->tp);
    }
}

static void start_master(Sio *s) {
    uint16_t vals[4] = {s->send, LINK_ABSENT, LINK_ABSENT, LINK_ABSENT};
    if (s->tp && s->tp->exchange) s->tp->exchange(s->tp, s->send, vals);
    if (s->tp && s->tp->lockstep && s->nrounds < LINK_MAX_ROUNDS) {
        s->rounds[s->nrounds].off = s->fcyc;
        memcpy(s->rounds[s->nrounds].v, vals, sizeof vals);
        s->nrounds++;
    }
    begin(s, vals);
}

const char *rt_link_status_path;
static void link_status_write(Sio *s) {
    LinkStats st;
    if (!s->tp || !s->tp->stats || !s->tp->stats(s->tp, &st)) return;
    st.frame = s->frame;
    if (s->frame % 600 == 0)
        rt_log("[link] %s K=%d frame %u: rtt %.1f ms (max %.1f), waited in %llu frames (%.1f ms total), tx %.0f B/s rx %.0f B/s%s",
               st.role ? "child" : "host", st.delay, st.frame, st.rtt_ms[1], st.rtt_max_ms[1], (unsigned long long)st.wait_frames,
               st.wait_ms, st.seconds > 0 ? st.tx_bytes / st.seconds : 0, st.seconds > 0 ? st.rx_bytes / st.seconds : 0,
               st.desync ? " DESYNC" : "");
    const char *path = rt_link_status_path ? rt_link_status_path : getenv("KIRBY_LINK_STATUS");
    if (!path || !*path) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    double fr = st.frame ? (double)st.frame : 1;
    fprintf(f, "{\n \"state\": \"%s\", \"role\": \"%s\", \"players\": %d, \"mask\": %u, \"player\": %d, \"delay\": %d, \"delay_max\": %d, \"frame\": %u,\n",
            st.state, st.role ? "join" : "host", st.players, st.mask, s->tp->player_id, st.delay, st.delay_max, st.frame);
    fprintf(f, " \"rtt_ms\": [%.2f, %.2f, %.2f, %.2f], \"rtt_avg_ms\": [%.2f, %.2f, %.2f, %.2f], \"rtt_max_ms\": [%.2f, %.2f, %.2f, %.2f],\n",
            st.rtt_ms[0], st.rtt_ms[1], st.rtt_ms[2], st.rtt_ms[3], st.rtt_avg_ms[0], st.rtt_avg_ms[1], st.rtt_avg_ms[2], st.rtt_avg_ms[3],
            st.rtt_max_ms[0], st.rtt_max_ms[1], st.rtt_max_ms[2], st.rtt_max_ms[3]);
    fprintf(f, " \"wait_frames\": %llu, \"wait_ms\": %.1f, \"wait_max_ms\": %.1f, \"frames_behind\": %u,\n",
            (unsigned long long)st.wait_frames, st.wait_ms, st.wait_max_ms, st.frames_behind);
    fprintf(f, " \"tx_bytes\": %llu, \"rx_bytes\": %llu, \"tx_bytes_per_s\": %.0f, \"rx_bytes_per_s\": %.0f, \"tx_bytes_per_frame\": %.1f, \"rx_bytes_per_frame\": %.1f,\n",
            (unsigned long long)st.tx_bytes, (unsigned long long)st.rx_bytes, st.seconds > 0 ? st.tx_bytes / st.seconds : 0,
            st.seconds > 0 ? st.rx_bytes / st.seconds : 0, st.tx_bytes / fr, st.rx_bytes / fr);
    fprintf(f, " \"underflows\": %llu, \"desync\": %s, \"desync_frame\": %u }\n", (unsigned long long)st.underflows,
            st.desync ? "true" : "false", st.desync_frame);
    fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp, path);
#endif
}

int rt_sio_stats(LinkStats *out) {
    if (!rt_sio_cur || !rt_sio_cur->tp || !rt_sio_cur->tp->stats) return 0;
    int r = rt_sio_cur->tp->stats(rt_sio_cur->tp, out);
    out->frame = rt_sio_cur->frame;
    return r;
}

/* Fim de um quadro do hw (chamado por grab_frame). Em lockstep e a barreira: o host entrega as trocas do
 * quadro e espera o filho terminar o mesmo quadro; o filho avisa que terminou e espera o quadro seguinte. */
void rt_sio_frame_end(Sio *s) {
    LinkTransport *t = s->tp;
    if (!t || !t->lockstep) return;
    while (s->isched < s->nsched && s->nsendlog < LINK_MAX_ROUNDS) { s->sendlog[s->nsendlog++] = s->send; s->isched++; }   /* trocas nao alcancadas */
    if (t->frame_end) t->frame_end(t, s->frame, s->sendlog, s->nsendlog, s->rounds, s->nrounds, s->xhash);
    s->nrounds = 0;
    s->nsendlog = 0;
    s->frame++;
    s->fcyc = 0;
    s->nsched = s->isched = 0;
    if (t->player_id != 0 && t->frame_begin) {
        int n = t->frame_begin(t, s->frame, s->sched, LINK_MAX_ROUNDS);
        s->nsched = n > 0 ? n : 0;
    }
    if (s->frame % 60 == 0) link_status_write(s);
}

static uint16_t siocnt_read(const Sio *s) {
    uint16_t v = s->siocnt & 0x7003;                    /* baud, modo, IRQ */
    int child = s->tp && s->tp->player_id != 0;
    if (child) v |= 1u << 2;                            /* SI: 0 = pai, 1 = filho */
    uint32_t mask = s->tp ? s->tp->connected_mask(s->tp) : 1;
    int players = s->tp ? s->tp->players : 1;
    if (players >= 2 && mask == (1u << players) - 1) v |= 1u << 3;   /* SD: todos prontos */
    v |= (uint16_t)((s->id_latched & 3) << 4);          /* ID (apos a 1a troca) */
    if (s->busy) v |= 0x80;
    return v;
}

static uint16_t reg_read16(const Sio *s, uint32_t off) {
    switch (off) {
    case 0x120: return s->multi[0];
    case 0x122: return s->multi[1];
    case 0x124: return s->multi[2];
    case 0x126: return s->multi[3];
    case 0x128: return siocnt_read(s);
    case 0x12A: return s->send;
    case 0x134: return s->rcnt;
    }
    return 0;
}

static void reg_write16(Sio *s, uint32_t off, uint16_t v) {
    switch (off) {
    case 0x128: {
        s->siocnt = v & 0x7003;
        if ((v & 0x80) && !s->busy && is_master(s) && multiplay(s)) start_master(s);
        break;
    }
    case 0x12A: s->send = v; if (s->tp && s->tp->publish) s->tp->publish(s->tp, v); break;
    case 0x134: s->rcnt = v & 0xC1FF; break;
    default: break;   /* SIOMULTIx somente leitura */
    }
}

static int sio_addr(uint32_t a) { return (a >= 0x04000120u && a < 0x0400012Cu) || (a >= 0x04000134u && a < 0x04000136u); }

/* KIRBY_SIO_TRACE=N: registra os N primeiros acessos do jogo a SIO/IF/IE/TM3 (diagnostico) */
static int trace_left = -1;
static void trace_acc(const char *rw, uint32_t a, int size, uint32_t v) {
    if (trace_left < 0) {
        const char *e = getenv("KIRBY_SIO_TRACE");
        trace_left = e ? atoi(e) : 0;
    }
    if (trace_left == 0) return;
    int hit = (a >= 0x04000120u && a < 0x0400012Cu) || a == 0x04000134u || ((a >= 0x04000200u && a < 0x04000204u) || a == 0x04000208u) ||
              (a >= 0x0400010Cu && a < 0x04000110u);
    if (!hit) return;
    trace_left--;
    extern uint32_t rt_rets[4096], rt_depth;
    rt_log("[sio] %s %08X/%d %s%08X  (caller returns to %08X)", rw, a, size, rw[0] == 'R' ? "-> " : "<- ", v,
           rt_depth ? rt_rets[(rt_depth - 1) & 4095u] : 0);
}

int rt_sio_read(Sio *s, uint32_t a, int size, uint32_t *out) {
    uint32_t end = a + (uint32_t)size;
    int covers_if = a <= 0x04000202u && 0x04000202u < end;
    if (!sio_addr(a) && !covers_if) return 0;
    uint32_t v = 0;
    if (sio_addr(a)) {
        for (int i = 0; i < size; i++) {
            uint32_t ad = a + (uint32_t)i;
            uint16_t h = reg_read16(s, (ad & ~1u) & 0x3FF);
            v |= (uint32_t)((ad & 1) ? h >> 8 : h & 0xFF) << (8 * i);
        }
    } else {   /* IF: o bit 7 (serial) vem do SIO; o resto do hw/ */
        v = hw_read(a, size);
        if (s->irq_flag) v |= 0x80u << (8 * (0x04000202u - a));
    }
    *out = v;
    trace_acc("R", a, size, v);
    return 1;
}

int rt_sio_write(Sio *s, uint32_t a, int size, uint32_t v) {
    trace_acc("W", a, size, v);
    uint32_t end = a + (uint32_t)size;
    int covers_if = a <= 0x04000202u && 0x04000202u < end;
    if (sio_addr(a)) {
        for (int i = 0; i < size;) {
            uint32_t ad = a + (uint32_t)i, off = (ad & ~1u) & 0x3FF;
            if (size == 1 || (ad & 1)) {   /* byte: le-modifica-escreve o halfword */
                uint16_t h = reg_read16(s, off);
                if (off == 0x128) h = s->siocnt | (uint16_t)(s->busy ? 0x80 : 0);
                uint32_t b = (v >> (8 * i)) & 0xFF;
                h = (ad & 1) ? (uint16_t)((h & 0x00FF) | (b << 8)) : (uint16_t)((h & 0xFF00) | b);
                reg_write16(s, off, h);
                i += 1;
            } else {
                reg_write16(s, off, (uint16_t)((v >> (8 * i)) & 0xFFFF));
                i += 2;
            }
        }
        return 1;
    }
    if (covers_if) {   /* IF e write-1-to-clear: bit 7 limpa o IRQ serial; o resto segue para o hw/ */
        if (((v >> (8 * (0x04000202u - a))) & 0x80u) != 0) s->irq_flag = 0;
        hw_write(a, size, v);
        return 1;
    }
    return 0;
}

/* =============================================================== transporte "null" */
static uint32_t null_mask(LinkTransport *t) { (void)t; return 1; }
static void null_publish(LinkTransport *t, uint16_t v) { (void)t; (void)v; }
static int null_exchange(LinkTransport *t, uint16_t my, uint16_t out[4]) {
    (void)t;
    out[0] = my; out[1] = out[2] = out[3] = LINK_ABSENT;
    return 1;
}
static int null_poll(LinkTransport *t, LinkEvent *e) { (void)t; (void)e; return 0; }
static void null_close(LinkTransport *t) { free(t); }

LinkTransport *link_null_create(void) {
    LinkTransport *t = (LinkTransport *)calloc(1, sizeof *t);
    t->name = "null"; t->player_id = 0; t->players = 1;
    t->connected_mask = null_mask; t->publish = null_publish; t->exchange = null_exchange;
    t->poll = null_poll; t->close = null_close;
    return t;
}

/* =============================================================== hub em processo (testes) */
typedef struct Hub {
    int players;
    uint32_t mask;
    uint16_t send[4];
    LinkEvent q[4][16];
    int qn[4];
} Hub;

typedef struct HubEnd { Hub *hub; int id; } HubEnd;

static uint32_t hub_mask(LinkTransport *t) { return ((HubEnd *)t->priv)->hub->mask; }
static void hub_publish(LinkTransport *t, uint16_t v) { HubEnd *e = t->priv; e->hub->send[e->id] = v; }
static int hub_exchange(LinkTransport *t, uint16_t my, uint16_t out[4]) {
    HubEnd *e = t->priv;
    Hub *h = e->hub;
    for (int i = 0; i < 4; i++)
        out[i] = i == 0 ? my : ((h->mask >> i) & 1 ? h->send[i] : LINK_ABSENT);
    for (int i = 1; i < h->players; i++)
        if ((h->mask >> i) & 1 && h->qn[i] < 16) {
            h->q[i][h->qn[i]].type = 1;
            memcpy(h->q[i][h->qn[i]].values, out, sizeof out[0] * 4);
            h->qn[i]++;
        }
    return 1;
}
static int hub_poll(LinkTransport *t, LinkEvent *ev) {
    HubEnd *e = t->priv;
    Hub *h = e->hub;
    if (h->qn[e->id] == 0) return 0;
    *ev = h->q[e->id][0];
    memmove(&h->q[e->id][0], &h->q[e->id][1], sizeof(LinkEvent) * (size_t)(--h->qn[e->id]));
    return 1;
}
static void hub_close(LinkTransport *t) { free(t->priv); free(t); }

void link_hub_create(int players, LinkTransport *out[4]) {
    Hub *h = (Hub *)calloc(1, sizeof *h);
    h->players = players;
    h->mask = (1u << players) - 1;
    for (int i = 0; i < 4; i++) h->send[i] = LINK_ABSENT;
    for (int i = 0; i < players; i++) {
        LinkTransport *t = (LinkTransport *)calloc(1, sizeof *t);
        HubEnd *e = (HubEnd *)calloc(1, sizeof *e);
        e->hub = h; e->id = i;
        t->name = "hub"; t->player_id = i; t->players = players; t->priv = e;
        t->connected_mask = hub_mask; t->publish = hub_publish; t->exchange = hub_exchange;
        t->poll = hub_poll; t->close = hub_close;
        out[i] = t;
    }
    for (int i = players; i < 4; i++) out[i] = NULL;
}
void link_hub_disconnect(LinkTransport *t) {
    HubEnd *e = t->priv;
    e->hub->mask &= ~(1u << e->id);
    e->hub->send[e->id] = LINK_ABSENT;
}

/* =============================================================== transporte "local": TCP
 * Protocolo (v2): mensagens [tipo u8][tamanho u16 LE][carga]; carga <= 1024 B.
 *   HELLO   filho->host  magic u32 "KBRL", versao u8, reservado u8, SHA-1 da ROM (20 B)   (nada de ROM trafega)
 *   WELCOME host->filho  id u8, jogadores u8, mascara u8          REJECT host->filho  motivo u8
 *   FRAME   host->filho  (lockstep) trocas do quadro, compactadas (ver enc_frame)
 *   DONE    filho->host  (lockstep) quadro terminado + SENDs do filho por troca (ver enc_done)
 *   PING/PONG  RTT (a carga volta igual)             SEND/ROUND/ACK  modo livre (sem lockstep, KIRBY_LINK_FREE)
 * Lockstep com K quadros de folga: o host so comeca o quadro f+1 depois de DONE(f-K); o filho reproduz os
 * quadros do host na ordem, com atraso. O FIFO de SENDs do filho tem atraso fixo D = 20 + 17*K trocas
 * (o fluxo de trocas depende so de K, nunca do relogio: mesmo hash com ou sem latencia na rede). */
enum { M_HELLO = 1, M_WELCOME = 2, M_REJECT = 3, M_SEND = 4, M_ROUND = 5, M_ACK = 6, M_FRAME = 7, M_DONE = 8,
       M_PING = 9, M_PONG = 10, M_INFO = 11 };
#define PROTO_MAGIC 0x4C52424Bu   /* "KBRL" */
#define PROTO_VER 2
#define MSG_MAX 1024
#define FQ_MAX 32
#define HELLO_LEN 26
/* Maior K que o KatAM aguenta (medido, SUB-GAMES e co-op): 2 jogadores K<=2; 3 ou 4 jogadores K<=1. Com mais jogadores a
 * cadeia do lobby do gLink passa por dois saltos (filho -> host -> outro filho, cada um com D trocas de atraso) e o
 * jogo desiste (link desligado) ja em K=2; K>=3 falha ate com 2 jogadores. */
static int link_k_max(int players) { return players <= 2 ? 2 : 1; }
enum { R_BAD = 1, R_VERSION = 2, R_ROM = 3, R_FULL = 4, R_STARTED = 5 };

typedef struct { uint8_t type; uint16_t len; uint8_t p[MSG_MAX]; } Msg;
typedef struct { double due; int len; uint8_t d[MSG_MAX + 3]; } SimMsg;
typedef struct Peer {
    sock_t fd;
    uint8_t buf[MSG_MAX + 3];
    int have;
    SimMsg *sq; int sqh, sqn; double last_due;   /* fila da simulacao de rede (so com --link-sim) */
    uint64_t tx, rx;
} Peer;

typedef struct FrameIn {   /* filho: quadro do host recebido e ainda nao executado */
    uint32_t frame;
    int n, has_hash;
    uint64_t hash;
    LinkRound r[LINK_MAX_ROUNDS];
} FrameIn;

typedef struct Local {
    int host, port, players, lock, K, started;
    int D;                    /* atraso do SEND dos filhos, em trocas */
    int Kmax;                 /* K maximo para o numero de jogadores */
    int timeout_ms;           /* espera maxima por um par em lockstep */
    sock_t listener;
    Peer peers[4];            /* host: indice = id do filho; join: peers[0] = host */
    Peer pend[4];             /* host: conexoes ainda sem HELLO */
    double pend_t0[4];
    uint16_t latest[4], acked[4], round;
    uint32_t mask;
    uint16_t my_send;
    char addr[64];
    unsigned tries, polls;
    uint64_t ack_waits, ack_timeouts;
    uint8_t sha[20];
    int rejected;             /* filho: o host recusou (motivo) */
    /* lockstep, host */
    /* SENDs dos filhos: elemento e = SEND do filho na troca e do host. O host consome o elemento j-D na troca j
     * (D = 20 em fase lenta, 20+17K em fase rapida), nunca alem de `lim` (o que todo filho ja terminou, contado
     * pelas trocas do proprio host: independe da rede), repetindo o ultimo se precisar. */
    uint16_t el[4][1024]; uint32_t ecount[4]; int64_t last_idx[4];
    uint32_t jr, fr_rounds, ncum[64]; int64_t lim; int Dcur;
    uint64_t underflows;
    uint32_t sent_frames, done_cnt[4];
    uint16_t dec_prev_send[4];   /* ultimo valor decodificado de DONE por filho (compactacao) */
    uint32_t ping_seq;
    /* lockstep, filho */
    FrameIn fq[FQ_MAX]; int fqh, fqn;
    uint32_t cur_frame; int cur_has_hash; uint64_t cur_hash;
    uint16_t enc_prev_send;
    /* codec compartilhado: estado do ultimo FRAME (codificador no host, decodificador no filho) */
    uint16_t prev_v[4]; uint8_t prev_mask; uint32_t prev_delta;
    /* medicao */
    LinkStats st;
    double rtt_sum[4]; uint64_t rtt_n[4];
    double t_start;
} Local;

double rt_now_us(void);
int (*rt_link_idle)(void);    /* chamado durante esperas longas: atende a janela (0 = usuario fechou) */
static double sim_lat, sim_jit, sim_loss;
static int sim_on = -1;
static uint64_t sim_rng = 0x9E3779B97F4A7C15ull;

static int net_started;
static void net_init(void) {
#ifdef _WIN32
    if (!net_started) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#endif
    net_started = 1;
}
static void set_nonblock(sock_t fd) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(fd, FIONBIO, &m);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}
static int would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}
static void wait_fd(sock_t fd, int want_write, int ms) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(fd, &fs);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    select((int)fd + 1, want_write ? NULL : &fs, want_write ? &fs : NULL, NULL, &tv);
}

static int sim_active(void);
static void sim_pump(Peer *p);
/* espera dados do par ate `ms`. Na rede de verdade o select acorda na hora; na simulacao a fila de saida de
 * CADA lado so anda se alguem a atende, e o select de 1 ms vira 15 ms no Windows: gira com yield. */
static void wait_peer(Peer *p, int ms) {
    if (!sim_active() && !p->sqn) { wait_fd(p->fd, 0, ms); return; }
    double t0 = rt_now_us();
    do {
        sim_pump(p);
        fd_set fs; struct timeval tv = {0, 0};
        FD_ZERO(&fs); FD_SET(p->fd, &fs);
        if (select((int)p->fd + 1, &fs, NULL, NULL, &tv) > 0) return;
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    } while (rt_now_us() - t0 < ms * 1000.0);
}

/* ---- simulacao de rede (teste): latencia, jitter e perda (perda de TCP = atraso de retransmissao) ---- */
void rt_link_sim(const char *spec) {
    double a = 0, b = 0, c = 0;
    if (spec && sscanf(spec, "%lf,%lf,%lf", &a, &b, &c) >= 1) { sim_lat = a; sim_jit = b; sim_loss = c; sim_on = a > 0 || b > 0 || c > 0; }
    else sim_on = 0;
}
static double sim_rand(void) {   /* 0..1, semente fixa */
    sim_rng ^= sim_rng << 13; sim_rng ^= sim_rng >> 7; sim_rng ^= sim_rng << 17;
    return (double)(sim_rng >> 11) / 9007199254740992.0;
}
static int sim_active(void) {
    if (sim_on < 0) { const char *e = getenv("KIRBY_LINK_SIM"); rt_link_sim(e); }
    return sim_on;
}

static int raw_send(sock_t fd, const uint8_t *b, int n) {
    int sent = 0;
    double t0 = rt_now_us();
    while (sent < n) {
        int k = (int)send(fd, (const char *)b + sent, n - sent, 0);
        if (k > 0) { sent += k; continue; }
        if (k < 0 && would_block() && rt_now_us() - t0 < 5e6) { wait_fd(fd, 1, 20); continue; }
        return -1;
    }
    return 0;
}

static void sim_pump(Peer *p) {
    while (p->sqn > 0) {
        SimMsg *m = &p->sq[p->sqh];
        if (rt_now_us() < m->due) break;
        if (p->fd != SOCK_BAD) raw_send(p->fd, m->d, m->len);
        p->sqh = (p->sqh + 1) & 255; p->sqn--;
    }
}

static int peer_send(Peer *p, int type, const void *payload, int len) {
    uint8_t b[MSG_MAX + 3];
    if (len > MSG_MAX) return -1;
    b[0] = (uint8_t)type; b[1] = (uint8_t)(len & 0xFF); b[2] = (uint8_t)(len >> 8);
    if (len) memcpy(b + 3, payload, (size_t)len);
    p->tx += (uint64_t)len + 3;
    if (!sim_active()) return raw_send(p->fd, b, len + 3);
    if (!p->sq) p->sq = (SimMsg *)calloc(256, sizeof(SimMsg));
    if (p->sqn >= 256) return -1;
    double lat = sim_lat + (sim_rand() * 2 - 1) * sim_jit;
    if (lat < 0) lat = 0;
    if (sim_rand() * 100.0 < sim_loss) lat += 200.0 + 3 * sim_lat;   /* retransmissao por RTO */
    double due = rt_now_us() + lat * 1000.0;
    if (due < p->last_due) due = p->last_due;   /* TCP entrega em ordem */
    p->last_due = due;
    SimMsg *m = &p->sq[(p->sqh + p->sqn) & 255];
    m->due = due; m->len = len + 3; memcpy(m->d, b, (size_t)len + 3);
    p->sqn++;
    sim_pump(p);
    return 0;
}

/* 1 = mensagem completa; 0 = nada; -1 = desconectou ou violou o protocolo */
static int recv_msg(Peer *p, Msg *m) {
    for (;;) {
        int need = p->have < 3 ? 3 : 3 + (p->buf[1] | (p->buf[2] << 8));
        if (p->have >= 3 && need - 3 > MSG_MAX) return -1;
        if (p->have >= need) {
            m->type = p->buf[0]; m->len = (uint16_t)(need - 3);
            memcpy(m->p, p->buf + 3, m->len);
            p->have = 0;
            return 1;
        }
        int n = (int)recv(p->fd, (char *)p->buf + p->have, need - p->have, 0);
        if (n == 0) return -1;
        if (n < 0) return would_block() ? 0 : -1;
        p->have += n;
        p->rx += (uint64_t)n;
        if (p->have >= 3) {
            int len = p->buf[1] | (p->buf[2] << 8);
            if (len > MSG_MAX) return -1;
        }
    }
}

/* ---- SHA-1 da ROM (HELLO): so para conferir que os dois tem a mesma ROM ---- */
static uint32_t rol32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static void sha1_blocks(uint32_t h[5], const uint8_t *p, size_t nblk) {
    while (nblk--) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        p += 64;
    }
}
static void sha1(const uint8_t *d, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    sha1_blocks(h, d, n / 64);
    uint8_t tail[128] = {0};
    size_t r = n % 64;
    memcpy(tail, d + (n - r), r);
    tail[r] = 0x80;
    size_t tl = r < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    sha1_blocks(h, tail, tl / 64);
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}
static void rom_sha1(uint8_t out[20]) {
    static uint8_t cache[20]; static int have;
    if (!have) {
        const uint8_t *r = gba_rom_data();
        if (r) sha1(r, gba_rom_size(), cache); else memset(cache, 0, 20);
        have = 1;
    }
    memcpy(out, cache, 20);
}

static void build_hello(Local *l, uint8_t out[HELLO_LEN]) {
    uint32_t m = PROTO_MAGIC;
    memcpy(out, &m, 4);
    out[4] = PROTO_VER; out[5] = 0;
    memcpy(out + 6, l->sha, 20);
    if (getenv("KIRBY_LINK_TEST_BADROM")) out[6] ^= 0xFF;   /* testes de rejeicao */
    if (getenv("KIRBY_LINK_TEST_BADVER")) out[4] = PROTO_VER + 1;
}

/* ---- codec compacto ---- */
static int put_varint(uint8_t *o, uint32_t v) {
    int n = 0;
    while (v >= 0x80) { o[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    o[n++] = (uint8_t)v;
    return n;
}
static int get_varint(const uint8_t *p, int len, int *pos, uint32_t *v) {
    uint32_t r = 0; int sh = 0;
    while (*pos < len && sh < 35) {
        uint8_t b = p[(*pos)++];
        r |= (uint32_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) { *v = r; return 0; }
        sh += 7;
    }
    return -1;
}

/* FRAME: u16 frame, u8 (mascara | 0x10 se tem hash), u8 n, [u64 hash], n x rodada.
 * rodada: u8 b (bits 0-3 = slots presentes, bit 4 = mesmo delta do anterior, bit 5 = mesmos valores),
 *         [varint delta de ciclos desde a rodada anterior (a 1a: desde o inicio do quadro)], [halfwords presentes] */
static int enc_frame(Local *l, uint8_t *o, uint32_t frame, int mask, int has_hash, uint64_t hash, const LinkRound *r, int n) {
    int k = 0;
    o[k++] = (uint8_t)frame; o[k++] = (uint8_t)(frame >> 8);
    o[k++] = (uint8_t)(mask | (has_hash ? 0x10 : 0));
    o[k++] = (uint8_t)n;
    if (has_hash) { memcpy(o + k, &hash, 8); k += 8; }
    uint32_t prev_off = 0;
    for (int i = 0; i < n; i++) {
        uint32_t delta = r[i].off - prev_off;
        uint8_t pm = 0;
        for (int s = 0; s < 4; s++) if (r[i].v[s] != LINK_ABSENT) pm |= (uint8_t)(1u << s);
        int same_delta = i > 0 && delta == l->prev_delta;
        int same_vals = pm == l->prev_mask && !memcmp(r[i].v, l->prev_v, sizeof l->prev_v);
        int bi = k++;
        o[bi] = (uint8_t)(pm | (same_delta ? 0x10 : 0) | (same_vals ? 0x20 : 0));
        if (!same_delta) k += put_varint(o + k, delta);
        if (!same_vals)
            for (int s = 0; s < 4; s++)
                if ((pm >> s) & 1) { o[k++] = (uint8_t)r[i].v[s]; o[k++] = (uint8_t)(r[i].v[s] >> 8); }
        l->prev_delta = delta; l->prev_mask = pm; memcpy(l->prev_v, r[i].v, sizeof l->prev_v);
        prev_off = r[i].off;
    }
    return k;
}
static int dec_frame(Local *l, const Msg *m, FrameIn *f, int *mask_out) {
    const uint8_t *p = m->p; int len = m->len, k = 0;
    if (len < 4) return -1;
    f->frame = (uint32_t)(p[0] | (p[1] << 8));
    *mask_out = p[2] & 0x0F;
    f->has_hash = (p[2] >> 4) & 1;
    f->n = p[3];
    k = 4;
    if (f->n > LINK_MAX_ROUNDS) return -1;
    if (f->has_hash) { if (k + 8 > len) return -1; memcpy(&f->hash, p + k, 8); k += 8; }
    uint32_t off = 0;
    for (int i = 0; i < f->n; i++) {
        if (k >= len) return -1;
        uint8_t b = p[k++];
        uint32_t delta = l->prev_delta;
        if (!(b & 0x10)) { if (get_varint(p, len, &k, &delta)) return -1; }
        uint8_t pm = b & 0x0F;
        if (b & 0x20) pm = l->prev_mask;
        else {
            for (int s = 0; s < 4; s++) l->prev_v[s] = LINK_ABSENT;
            for (int s = 0; s < 4; s++)
                if ((pm >> s) & 1) { if (k + 2 > len) return -1; l->prev_v[s] = (uint16_t)(p[k] | (p[k + 1] << 8)); k += 2; }
        }
        l->prev_delta = delta; l->prev_mask = pm;
        off += delta;
        f->r[i].off = off;
        memcpy(f->r[i].v, l->prev_v, sizeof f->r[i].v);
    }
    return 0;
}

/* DONE: u16 frame, u8 flags (bit 0 = a pilha de trocas divergiu do host), u8 n, bitmap "igual ao anterior"
 * (ceil(n/8) B), depois so os halfwords diferentes */
static int enc_done(Local *l, uint8_t *o, uint32_t frame, int desync, const uint16_t *sends, int ns) {
    int k = 0;
    o[k++] = (uint8_t)frame; o[k++] = (uint8_t)(frame >> 8);
    o[k++] = (uint8_t)desync; o[k++] = (uint8_t)ns;
    int nb = (ns + 7) / 8;
    memset(o + k, 0, (size_t)nb);
    int bm = k;
    k += nb;
    for (int i = 0; i < ns; i++) {
        if (sends[i] == l->enc_prev_send) o[bm + i / 8] |= (uint8_t)(1u << (i % 8));
        else { o[k++] = (uint8_t)sends[i]; o[k++] = (uint8_t)(sends[i] >> 8); l->enc_prev_send = sends[i]; }
    }
    return k;
}
static int dec_done(Local *l, int child, const Msg *m, uint16_t *out, int *ns, uint32_t *frame, int *desync) {
    const uint8_t *p = m->p; int len = m->len;
    if (len < 4) return -1;
    *frame = (uint32_t)(p[0] | (p[1] << 8));
    *desync = p[2] & 1;
    *ns = p[3];
    if (*ns > LINK_MAX_ROUNDS) return -1;
    int nb = (*ns + 7) / 8, k = 4 + nb;
    if (k > len) return -1;
    for (int i = 0; i < *ns; i++) {
        if (!((p[4 + i / 8] >> (i % 8)) & 1)) {
            if (k + 2 > len) return -1;
            l->dec_prev_send[child] = (uint16_t)(p[k] | (p[k + 1] << 8));
            k += 2;
        }
        out[i] = l->dec_prev_send[child];
    }
    return 0;
}

/* ---- util ---- */
static uint32_t local_mask(LinkTransport *t) { return ((Local *)t->priv)->mask | (1u << t->player_id); }

static void local_drop(Local *l, int i) {
    if (l->peers[i].fd != SOCK_BAD) sock_close(l->peers[i].fd);
    l->peers[i].fd = SOCK_BAD; l->peers[i].have = 0; l->peers[i].sqn = 0;
    if (l->host) {
        l->mask &= ~(1u << i); l->latest[i] = LINK_ABSENT;
        rt_log("[link] player %d disconnected (0xFFFF from now on)", i + 1);
    } else {
        l->mask = 0;
        rt_log("[link] host disconnected (playing without cable)");
    }
    snprintf(l->st.state, sizeof l->st.state, "dropped");
}

static int idle(void) { return rt_link_idle ? rt_link_idle() : 1; }

static void note_rtt(Local *l, int i, double ms) {
    LinkStats *s = &l->st;
    s->rtt_ms[i] = ms;
    l->rtt_sum[i] += ms; l->rtt_n[i]++;
    s->rtt_avg_ms[i] = l->rtt_sum[i] / (double)l->rtt_n[i];
    if (ms > s->rtt_max_ms[i]) s->rtt_max_ms[i] = ms;
}

static void local_publish(LinkTransport *t, uint16_t v) {
    Local *l = t->priv;
    l->my_send = v;
    if (!l->lock && !l->host && l->peers[0].fd != SOCK_BAD) {
        uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        if (peer_send(&l->peers[0], M_SEND, b, 2) != 0) local_drop(l, 0);
    }
}

static void local_ack(LinkTransport *t) {   /* filho (modo livre): confirma a rodada recebida */
    Local *l = t->priv;
    if (!l->host && l->peers[0].fd != SOCK_BAD) {
        uint8_t b[2] = {(uint8_t)l->round, (uint8_t)(l->round >> 8)};
        if (peer_send(&l->peers[0], M_ACK, b, 2) != 0) local_drop(l, 0);
    }
}

/* ---- host: aceita conexoes, confere o HELLO, atende mensagens ---- */
static void reject(Peer *p, int why) {
    uint8_t b = (uint8_t)why;
    peer_send(p, M_REJECT, &b, 1);
    sock_close(p->fd);
    p->fd = SOCK_BAD;
}

static void host_accept(Local *l) {
    if (l->listener == SOCK_BAD) return;
    for (;;) {
        sock_t c = accept(l->listener, NULL, NULL);
        if (c == SOCK_BAD) break;
        int k = 0;
        while (k < 4 && l->pend[k].fd != SOCK_BAD) k++;
        if (k == 4) { sock_close(c); continue; }
        set_nonblock(c);
        memset(&l->pend[k], 0, sizeof l->pend[k]);
        l->pend[k].fd = c;
        l->pend_t0[k] = rt_now_us();
    }
    for (int k = 0; k < 4; k++) {
        Peer *p = &l->pend[k];
        if (p->fd == SOCK_BAD) continue;
        Msg m;
        int r = recv_msg(p, &m);
        if (r == 0 && rt_now_us() - l->pend_t0[k] > 3e6) r = -1;   /* nao mandou HELLO em 3 s */
        if (r < 0) { rt_log("[link] rejected a connection (no valid HELLO)"); sock_close(p->fd); p->fd = SOCK_BAD; continue; }
        if (r == 0) continue;
        uint32_t magic = 0;
        if (m.type != M_HELLO || m.len != HELLO_LEN) { rt_log("[link] rejected a connection (bad HELLO)"); reject(p, R_BAD); continue; }
        memcpy(&magic, m.p, 4);
        if (magic != PROTO_MAGIC) { rt_log("[link] rejected a connection (bad magic)"); reject(p, R_BAD); continue; }
        if (m.p[4] != PROTO_VER) { rt_log("[link] rejected a connection (protocol version %d, want %d)", m.p[4], PROTO_VER); reject(p, R_VERSION); continue; }
        if (memcmp(m.p + 6, l->sha, 20)) { rt_log("[link] rejected a connection (different ROM)"); reject(p, R_ROM); continue; }
        if (l->started) { rt_log("[link] rejected a connection (session already started)"); reject(p, R_STARTED); continue; }
        int id = 1;
        while (id < l->players && l->peers[id].fd != SOCK_BAD) id++;
        if (id >= l->players) { rt_log("[link] rejected a connection (room full)"); reject(p, R_FULL); continue; }
        l->peers[id] = *p;
        p->fd = SOCK_BAD; p->sq = NULL;
        l->mask |= 1u << id;
        l->latest[id] = l->lock ? 0 : LINK_ABSENT;
        l->acked[id] = l->round;
        l->done_cnt[id] = 0;
        uint8_t w[3] = {(uint8_t)id, (uint8_t)l->players, (uint8_t)l->mask};
        peer_send(&l->peers[id], M_WELCOME, w, 3);
        rt_log("[link] player %d connected", id + 1);
    }
}

static void host_msg(Local *l, int i, const Msg *m) {
    switch (m->type) {
    case M_SEND: if (m->len >= 2) l->latest[i] = (uint16_t)(m->p[0] | (m->p[1] << 8)); break;
    case M_ACK: if (m->len >= 2) l->acked[i] = (uint16_t)(m->p[0] | (m->p[1] << 8)); break;
    case M_PONG:
        if (m->len >= 12) {
            double t0;
            memcpy(&t0, m->p + 4, 8);
            note_rtt(l, i, (rt_now_us() - t0) / 1000.0);
        }
        break;
    case M_DONE: {
        uint16_t s[LINK_MAX_ROUNDS];
        int ns, desync;
        uint32_t fr;
        if (dec_done(l, i, m, s, &ns, &fr, &desync)) { rt_log("[link] player %d sent a bad DONE", i + 1); local_drop(l, i); break; }
        for (int k = 0; k < ns; k++) l->el[i][l->ecount[i]++ & 1023] = s[k];
        l->done_cnt[i]++;
        if (desync && !l->st.desync) {
            l->st.desync = 1; l->st.desync_frame = fr;
            rt_log("[link] DESYNC: player %d reports that its exchange stream differs from the host's near frame %u", i + 1, fr);
        }
        break;
    }
    default: break;
    }
}

/* atende todos os filhos sem bloquear */
static void host_drain(Local *l) {
    host_accept(l);
    for (int i = 1; i < l->players; i++) {
        Peer *p = &l->peers[i];
        if (p->fd == SOCK_BAD) continue;
        sim_pump(p);
        Msg m;
        int r;
        while ((r = recv_msg(p, &m)) == 1) host_msg(l, i, &m);
        if (r < 0) local_drop(l, i);
    }
}

static void send_pings(Local *l) {
    for (int i = 1; i < l->players; i++) {
        if (l->peers[i].fd == SOCK_BAD) continue;
        uint8_t b[12];
        double t = rt_now_us();
        uint32_t seq = ++l->ping_seq;
        memcpy(b, &seq, 4); memcpy(b + 4, &t, 8);
        peer_send(&l->peers[i], M_PING, b, 12);
    }
}

static int local_exchange(LinkTransport *t, uint16_t my, uint16_t out[4]) {
    Local *l = t->priv;
    if (l->lock) {   /* lockstep: cada filho entrega o SEND de uma troca de ha ~D trocas */
        out[0] = my;
        for (int i = 1; i < 4; i++) {
            if (!((l->mask >> i) & 1)) { out[i] = LINK_ABSENT; continue; }
            int64_t tg = (int64_t)l->jr - l->Dcur;
            if (tg > l->lim - 1) tg = l->lim - 1;            /* so o que o filho ja terminou */
            if (tg < l->last_idx[i]) tg = l->last_idx[i];    /* nunca volta */
            if (tg < 0) l->latest[i] = 0;                    /* SEND de reset */
            else if ((uint64_t)tg < l->ecount[i]) { l->latest[i] = l->el[i][tg & 1023]; l->last_idx[i] = tg; }
            else l->underflows++;
            out[i] = l->latest[i];
        }
        l->jr++; l->fr_rounds++;
        return 1;
    }
    /* modo livre: espera os filhos terminarem a rodada anterior (ate 50 ms de relogio real) */
    double t0 = 0;
    static int noack = -1;
    if (noack < 0) noack = getenv("KIRBY_LINK_NOACK") != NULL;
    for (;;) {
        host_drain(l);
        if (noack) break;
        int late = 0;
        for (int i = 1; i < l->players; i++)
            if (l->peers[i].fd != SOCK_BAD && l->acked[i] != l->round) late = 1;
        if (!late) break;
        if (t0 == 0) { t0 = rt_now_us(); l->ack_waits++; }
        else if (rt_now_us() - t0 > 50000.0) { l->ack_timeouts++; break; }
    }
    l->round++;
    out[0] = my;
    for (int i = 1; i < 4; i++) out[i] = (l->mask >> i) & 1 ? l->latest[i] : LINK_ABSENT;
    uint8_t b[11];
    b[0] = (uint8_t)l->mask;
    for (int k = 0; k < 4; k++) { b[1 + 2 * k] = (uint8_t)out[k]; b[2 + 2 * k] = (uint8_t)(out[k] >> 8); }
    b[9] = (uint8_t)l->round; b[10] = (uint8_t)(l->round >> 8);
    for (int i = 1; i < l->players; i++)
        if (l->peers[i].fd != SOCK_BAD && peer_send(&l->peers[i], M_ROUND, b, 11) != 0) local_drop(l, i);
    return 1;
}

/* ---- filho: mensagens do host ---- */
static void child_msg(Local *l, const Msg *m) {
    switch (m->type) {
    case M_PING:   /* devolve igual: o host calcula o RTT */
        peer_send(&l->peers[0], M_PONG, m->p, m->len);
        break;
    case M_INFO:   /* host: K efetivo e maximo (so para a medicao) */
        if (m->len >= 2) { l->K = m->p[0]; l->Kmax = m->p[1]; }
        break;
    case M_WELCOME:
        if (m->len >= 3) {
            l->players = m->p[1]; l->mask = m->p[2];
            if (l->lock == 0) rt_log("[link] connected as player %d of %d", m->p[0] + 1, m->p[1]);
        }
        break;
    case M_REJECT:
        l->rejected = m->len ? m->p[0] : R_BAD;
        rt_log("[link] the host rejected this connection (reason %d)", l->rejected);
        break;
    case M_FRAME: {
        if (l->fqn >= FQ_MAX) break;
        FrameIn *f = &l->fq[(l->fqh + l->fqn) & (FQ_MAX - 1)];
        int mask;
        if (dec_frame(l, m, f, &mask)) { rt_log("[link] bad FRAME from the host"); local_drop(l, 0); break; }
        l->mask = (uint32_t)mask;
        l->fqn++;
        break;
    }
    default: break;
    }
}

/* le o que ha no socket do host. 0 = ok, -1 = caiu */
static int child_drain(Local *l) {
    Peer *p = &l->peers[0];
    if (p->fd == SOCK_BAD) return -1;
    sim_pump(p);
    for (;;) {
        if (l->lock && l->fqn >= FQ_MAX) return 0;   /* fila cheia: deixa no socket */
        Msg m;
        int r = recv_msg(p, &m);
        if (r == 0) return 0;
        if (r < 0) { local_drop(l, 0); return -1; }
        child_msg(l, &m);
        if (l->rejected) { local_drop(l, 0); return -1; }
    }
}

static int connect_now(Local *l, int ms);

static int local_poll(LinkTransport *t, LinkEvent *ev) {
    Local *l = t->priv;
    l->polls++;
    if (l->lock) {   /* lockstep: so atende PING/PONG e enfileira quadros (a espera de verdade esta em frame_begin/frame_end) */
        if ((l->polls & 15) == 0) {
            if (l->host) host_drain(l);
            else if (l->peers[0].fd != SOCK_BAD) child_drain(l);
        }
        return 0;
    }
    if (l->host) { host_drain(l); return 0; }
    if (l->rejected) return 0;
    if (l->peers[0].fd == SOCK_BAD) {
        if ((l->tries++ & 0x3F) != 0) return 0;   /* tenta conectar de vez em quando */
        if (connect_now(l, 5) != 0) return 0;
        return 0;
    }
    sim_pump(&l->peers[0]);
    Msg m;
    int r = recv_msg(&l->peers[0], &m);
    if (r < 0) { local_drop(l, 0); t->player_id = 1; return 0; }
    if (r == 0) return 0;
    if (m.type == M_WELCOME && m.len >= 3) {
        t->player_id = m.p[0]; l->players = t->players = m.p[1]; l->mask = m.p[2];
        rt_log("[link] connected as player %d of %d", m.p[0] + 1, m.p[1]);
        local_publish(t, l->my_send);
        return 0;
    }
    if (m.type == M_ROUND && m.len >= 11) {
        l->mask = m.p[0];
        l->round = (uint16_t)(m.p[9] | (m.p[10] << 8));
        ev->type = 1;
        for (int k = 0; k < 4; k++) ev->values[k] = (uint16_t)(m.p[1 + 2 * k] | (m.p[2 + 2 * k] << 8));
        return 1;
    }
    child_msg(l, &m);
    return 0;
}

/* ---- lockstep por quadro ---- */
static void note_wait(Local *l, double t0) {
    double ms = (rt_now_us() - t0) / 1000.0;
    if (ms > 0.5) { l->st.wait_frames++; l->st.wait_ms += ms; if (ms > l->st.wait_max_ms) l->st.wait_max_ms = ms; }
}

static void local_frame_end(LinkTransport *t, uint32_t frame, const uint16_t *sends, int ns, const LinkRound *r, int n, uint64_t xhash) {
    Local *l = t->priv;
    uint8_t b[MSG_MAX];
    l->st.frame = frame + 1;
    if (!l->host) {   /* filho: terminei o quadro `frame` */
        if (l->peers[0].fd == SOCK_BAD) return;
        int desync = 0;
        { const char *e = getenv("KIRBY_LINK_FAULT"); if (e && (uint32_t)atoi(e) == frame) xhash ^= 1; }   /* teste da deteccao de dessincronia */
        if (l->cur_frame == frame && l->cur_has_hash && l->cur_hash != xhash) {
            desync = 1;
            if (!l->st.desync) {
                l->st.desync = 1; l->st.desync_frame = frame;
                rt_log("[link] DESYNC: the exchange stream differs from the host's at frame %u (mine %016llx, host %016llx)",
                       frame, (unsigned long long)xhash, (unsigned long long)l->cur_hash);
            }
        }
        int k = enc_done(l, b, frame, desync, sends, ns);
        if (peer_send(&l->peers[0], M_DONE, b, k) != 0) local_drop(l, 0);
        return;
    }
    int has_hash = frame % 60 == 0;
    int k = enc_frame(l, b, frame, (int)l->mask, has_hash, xhash, r, n);
    for (int i = 1; i < l->players; i++)
        if (l->peers[i].fd != SOCK_BAD && peer_send(&l->peers[i], M_FRAME, b, k) != 0) local_drop(l, i);
    l->sent_frames = frame + 1;
    l->ncum[frame & 63] = l->jr;
    l->Dcur = l->fr_rounds >= 8 ? l->D : 20;   /* fase rapida (gLink, ~16 trocas/quadro) x lenta (deteccao, 1/quadro) */
    l->fr_rounds = 0;
    if (frame % 30 == 0) send_pings(l);
    /* o quadro f+1 so comeca depois de todo filho terminar o quadro f-K (K=0: o proprio f) */
    for (int i = 1; i < l->players; i++) {
        double t0 = rt_now_us(), last_idle = t0;
        while (l->peers[i].fd != SOCK_BAD && (int64_t)l->sent_frames - (int64_t)l->done_cnt[i] > l->K) {
            host_drain(l);
            if (l->peers[i].fd == SOCK_BAD || (int64_t)l->sent_frames - (int64_t)l->done_cnt[i] <= l->K) break;
            double now = rt_now_us();
            if (now - last_idle > 20000.0) { last_idle = now; if (!idle()) return; }
            if (now - t0 > l->timeout_ms * 1000.0) {
                rt_log("[link] player %d did not finish frame %u in %d ms: dropping it", i + 1, frame, l->timeout_ms);
                local_drop(l, i);
                break;
            }
            wait_peer(&l->peers[i], 4);
        }
        note_wait(l, t0);
    }
    {   /* o que todo filho ja terminou: quadros <= frame-K */
        int64_t g = (int64_t)frame - l->K;
        l->lim = g >= 0 ? l->ncum[g & 63] : 0;
    }
}

static int local_frame_begin(LinkTransport *t, uint32_t frame, LinkRound *out, int max) {
    Local *l = t->priv;
    if (l->host || l->peers[0].fd == SOCK_BAD) return 0;
    double t0 = rt_now_us(), last_idle = t0;
    while (l->fqn == 0) {
        if (child_drain(l) < 0) return 0;
        if (l->fqn) break;
        double now = rt_now_us();
        if (now - last_idle > 20000.0) { last_idle = now; if (!idle()) return 0; }
        if (now - t0 > l->timeout_ms * 1000.0) {
            rt_log("[link] the host sent nothing for %d ms (frame %u)", l->timeout_ms, frame);
            local_drop(l, 0);
            return 0;
        }
        wait_peer(&l->peers[0], 4);
    }
    note_wait(l, t0);
    FrameIn *f = &l->fq[l->fqh];
    l->fqh = (l->fqh + 1) & (FQ_MAX - 1); l->fqn--;
    if ((f->frame & 0xFFFF) != (frame & 0xFFFF)) {
        rt_log("[link] frame order error: got %u, expected %u", f->frame, frame & 0xFFFF);
        local_drop(l, 0);
        return 0;
    }
    int n = f->n < max ? f->n : max;
    memcpy(out, f->r, sizeof(LinkRound) * (size_t)n);
    l->cur_frame = frame; l->cur_has_hash = f->has_hash; l->cur_hash = f->hash;
    l->st.frames_behind = (uint32_t)l->fqn;
    return n;
}

static int local_stats(LinkTransport *t, LinkStats *out) {
    Local *l = t->priv;
    LinkStats *s = &l->st;
    s->role = l->host ? 0 : 1;
    s->players = l->players; s->delay = l->K; s->delay_max = l->Kmax;
    s->mask = l->mask | (1u << t->player_id);
    s->tx_bytes = s->rx_bytes = 0;
    for (int i = 0; i < 4; i++) { s->tx[i] = l->peers[i].tx; s->rx[i] = l->peers[i].rx; s->tx_bytes += l->peers[i].tx; s->rx_bytes += l->peers[i].rx; }
    s->seconds = (rt_now_us() - l->t_start) / 1e6;
    s->underflows = l->underflows;
    if (!l->lock) snprintf(s->state, sizeof s->state, "free");
    else if (!strcmp(s->state, "starting") || !s->state[0]) snprintf(s->state, sizeof s->state, "lockstep");
    *out = *s;
    return 1;
}

static void local_close(LinkTransport *t) {
    Local *l = t->priv;
    if (l->host && l->lock && l->underflows)
        rt_log("[link] lockstep: %llu exchanges ran out of child data (K too small for this network)", (unsigned long long)l->underflows);
    if (l->host && l->ack_timeouts)
        rt_log("[link] master waited for child acks %llu times, %llu hit the 50 ms limit",
               (unsigned long long)l->ack_waits, (unsigned long long)l->ack_timeouts);
    for (int i = 0; i < 4; i++) {
        if (l->peers[i].fd != SOCK_BAD) sock_close(l->peers[i].fd);
        if (l->pend[i].fd != SOCK_BAD) sock_close(l->pend[i].fd);
        free(l->peers[i].sq);
    }
    if (l->host && l->listener != SOCK_BAD) sock_close(l->listener);
    free(l);
    free(t);
}

static LinkTransport *local_new(Local *l, int id) {
    LinkTransport *t = (LinkTransport *)calloc(1, sizeof *t);
    t->name = "local"; t->player_id = id; t->players = l->players; t->priv = l;
    t->connected_mask = local_mask; t->publish = local_publish; t->exchange = local_exchange;
    t->poll = local_poll; t->close = local_close; t->ack = local_ack; t->stats = local_stats;
    l->t_start = rt_now_us();
    l->timeout_ms = 8000;
    { const char *e = getenv("KIRBY_LINK_TIMEOUT_MS"); if (e && atoi(e) > 0) l->timeout_ms = atoi(e); }
    snprintf(l->st.state, sizeof l->st.state, "starting");
    rom_sha1(l->sha);
    for (int i = 0; i < 4; i++) { l->peers[i].fd = SOCK_BAD; l->pend[i].fd = SOCK_BAD; l->latest[i] = LINK_ABSENT; }
    for (int s = 0; s < 4; s++) l->prev_v[s] = LINK_ABSENT;
    for (int i = 0; i < 4; i++) l->last_idx[i] = -1;
    l->Dcur = 20;
    return t;
}

LinkTransport *link_local_host(int players, int port) {
    net_init();
    Local *l = (Local *)calloc(1, sizeof *l);
    l->host = 1; l->players = players < 2 ? 2 : players > 4 ? 4 : players; l->port = port;
    l->mask = 1;
    l->listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    {   /* 127.0.0.1 por padrao; KIRBY_LINK_BIND=0.0.0.0 (ou um IP) abre para a LAN */
        const char *b = getenv("KIRBY_LINK_BIND");
        inet_pton(AF_INET, b && *b ? b : "127.0.0.1", &a.sin_addr);
    }
    if (bind(l->listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(l->listener, 4) != 0) {
        rt_log("[link] cannot listen on port %d", port);
        sock_close(l->listener);
        free(l);
        return NULL;
    }
    set_nonblock(l->listener);
    LinkTransport *t = local_new(l, 0);
    rt_log("[link] hosting %d players on port %d", l->players, port);
    return t;
}

LinkTransport *link_local_join(const char *host, int port) {
    net_init();
    Local *l = (Local *)calloc(1, sizeof *l);
    l->host = 0; l->players = 2; l->port = port;
    snprintf(l->addr, sizeof l->addr, "%s", host && *host ? host : "127.0.0.1");
    l->listener = SOCK_BAD;
    LinkTransport *t = local_new(l, 1);
    rt_log("[link] joining %s:%d", l->addr, port);
    return t;
}

static sock_t try_connect(const char *addr, int port, int ms) {
    sock_t c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) {   /* nome em vez de IP */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(addr, NULL, &hints, &res) != 0 || !res) { sock_close(c); return SOCK_BAD; }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    set_nonblock(c);
    if (connect(c, (struct sockaddr *)&a, sizeof a) == 0) return c;
    if (would_block()) {
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e); FD_SET(c, &w); FD_SET(c, &e);
        struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
        if (select((int)c + 1, NULL, &w, &e, &tv) > 0 && FD_ISSET(c, &w) && !FD_ISSET(c, &e)) {
            int err = 0;
#ifdef _WIN32
            int len = sizeof err;
#else
            socklen_t len = sizeof err;
#endif
            getsockopt(c, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            if (err == 0) return c;
        }
    }
    sock_close(c);
    return SOCK_BAD;
}

/* filho: abre a conexao e manda o HELLO. 0 = conectou */
static int connect_now(Local *l, int ms) {
    sock_t c = try_connect(l->addr, l->port, ms);
    if (c == SOCK_BAD) return -1;
    memset(&l->peers[0], 0, sizeof l->peers[0]);
    l->peers[0].fd = c;
    uint8_t h[HELLO_LEN];
    build_hello(l, h);
    if (peer_send(&l->peers[0], M_HELLO, h, HELLO_LEN) != 0) { sock_close(c); l->peers[0].fd = SOCK_BAD; return -1; }
    return 0;
}

/* Status do launcher ANTES do primeiro quadro (a espera pelos jogadores acontece sem janela): JSON reduzido,
 * mesmas chaves de link_status_write; `player` = indice deste jogador (-1 = ainda desconhecido). */
const char *rt_link_start_file;     /* --link-start-file: se existir, o host deixa de esperar (>= 2 presentes) */
static void wait_status(Local *l, const char *state, int player) {
    const char *path = rt_link_status_path ? rt_link_status_path : getenv("KIRBY_LINK_STATUS");
    if (!path || !*path) return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "{ \"state\": \"%s\", \"role\": \"%s\", \"players\": %d, \"mask\": %u, \"player\": %d, \"delay\": %d, "
               "\"delay_max\": %d, \"frame\": 0, \"rtt_ms\": [0, 0, 0, 0], \"desync\": false }\n",
            state, l->host ? "host" : "join", l->players, (unsigned)l->mask, player,
            l->K, link_k_max(l->players));
    fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp, path);
#endif
}
static int start_file_present(Local *l) {
    if (!rt_link_start_file || !*rt_link_start_file) return 0;
    if (__builtin_popcount((unsigned)l->mask) < 2) return 0;
    FILE *f = fopen(rt_link_start_file, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int link_local_lockstep(LinkTransport **tp, int wait_s, int delay) {
    LinkTransport *t = *tp;
    if (!t || strcmp(t->name, "local")) return 0;
    Local *l = t->priv;
    double t0 = rt_now_us(), limit = (wait_s > 0 ? wait_s : 120) * 1e6;
    if (l->host) {
        uint32_t full = (1u << l->players) - 1;
        l->lock = 1;
        rt_log("[link] lockstep: waiting for %d player(s)...", l->players - 1);
        double last_idle = t0, last_st = 0;
        while ((l->mask & full) != full && rt_now_us() - t0 < limit) {
            host_accept(l);
            if (rt_now_us() - last_st > 250000.0) { last_st = rt_now_us(); wait_status(l, "waiting", 0); }
            if (start_file_present(l)) { rt_log("[link] start requested: starting with mask %X", l->mask); break; }
            if ((l->mask & full) != full) wait_fd(l->listener, 0, 30);
            if (rt_now_us() - last_idle > 50000.0) { last_idle = rt_now_us(); if (!idle()) break; }
        }
        if ((l->mask & full) != full && !start_file_present(l)) rt_log("[link] timeout: starting with mask %X", l->mask);
        wait_status(l, "starting", 0);
        /* RTT de cada filho (5 pings) -> K automatico */
        double worst = 0;
        for (int i = 1; i < l->players; i++) {
            if (l->peers[i].fd == SOCK_BAD) continue;
            double rtts[5];
            int nr = 0;
            for (int k = 0; k < 5; k++) {
                uint32_t before = (uint32_t)l->rtt_n[i];
                send_pings(l);
                double w0 = rt_now_us();
                while ((uint32_t)l->rtt_n[i] == before && l->peers[i].fd != SOCK_BAD && rt_now_us() - w0 < 3e6) {
                    host_drain(l);
                    wait_peer(&l->peers[i], 5);
                }
                if ((uint32_t)l->rtt_n[i] != before) rtts[nr++] = l->st.rtt_ms[i];
            }
            for (int a = 1; a < nr; a++) { double v = rtts[a]; int b = a - 1; while (b >= 0 && rtts[b] > v) { rtts[b + 1] = rtts[b]; b--; } rtts[b + 1] = v; }
            double med = nr ? rtts[nr / 2] : 0;
            if (med > worst) worst = med;
        }
        l->Kmax = link_k_max(l->players);
        if (delay >= 0) {
            l->K = delay > 8 ? 8 : delay;
            if (l->K > l->Kmax) {
                if (getenv("KIRBY_LINK_FORCE_K")) rt_log("[link] warning: K=%d is above %d for %d players (forced by KIRBY_LINK_FORCE_K: the game will not reach the lobby)", l->K, l->Kmax, l->players);
                else {
                    rt_log("[link] K=%d is above %d for %d players, using %d", l->K, l->Kmax, l->players, l->Kmax);
                    l->K = l->Kmax;
                }
            }
        } else if (worst < 12.0) l->K = 0;
        else { l->K = (int)((worst - 12.0) / 16.7 + 0.999) + 1; if (l->K > l->Kmax) l->K = l->Kmax; }
        rt_log("[link] RTT %.1f ms -> input delay K=%d frame(s) (%s)", worst, l->K, delay >= 0 ? "configured" : "auto");
        l->D = 20 + 17 * l->K;
        { const char *e = getenv("KIRBY_LINK_FIFO"); if (e && atoi(e) > 0 && atoi(e) < 200) l->D = atoi(e); }
        l->Dcur = 20;
        for (int i = 1; i < l->players; i++)
            if (l->peers[i].fd != SOCK_BAD) { uint8_t b[2] = {(uint8_t)l->K, (uint8_t)l->Kmax}; peer_send(&l->peers[i], M_INFO, b, 2); }
        l->started = 1;
        sock_close(l->listener);   /* quem chegar depois nao entra: o quadro dele nao bateria com o do host */
        l->listener = SOCK_BAD;
    } else {
        wait_status(l, "connecting", -1);
        while (l->peers[0].fd == SOCK_BAD && !l->rejected && rt_now_us() - t0 < limit) {
            if (connect_now(l, 250) != 0) { if (!idle()) break; }
        }
        Msg m;
        int rc = -3;
        if (l->peers[0].fd != SOCK_BAD) {
            for (;;) {
                sim_pump(&l->peers[0]);
                rc = recv_msg(&l->peers[0], &m);
                if (rc != 0) break;
                if (rt_now_us() - t0 > limit) { rc = -2; break; }
                wait_peer(&l->peers[0], 20);
            }
        }
        if (rc != 1 || m.type != M_WELCOME || m.len < 3) {
            if (rc == 1 && m.type == M_REJECT) rt_log("[link] the host rejected this connection (reason %d: %s)", m.len ? m.p[0] : 0,
                m.len && m.p[0] == R_ROM ? "different ROM" : m.len && m.p[0] == R_VERSION ? "different protocol version" :
                m.len && m.p[0] == R_FULL ? "room full" : m.len && m.p[0] == R_STARTED ? "session already started" : "bad HELLO");
            else rt_log("[link] could not join %s:%d (host not found or not accepting)", l->addr, l->port);
            t->close(t);
            *tp = NULL;
            return -1;
        }
        t->player_id = m.p[0]; l->players = t->players = m.p[1]; l->mask = m.p[2];
        l->lock = 1;
        rt_log("[link] connected as player %d of %d (lockstep)", m.p[0] + 1, m.p[1]);
        wait_status(l, "waiting", m.p[0]);
        /* o host pode ainda estar esperando outros jogadores (ate `limit`): o primeiro dado dele (INFO) so chega
         * quando ele comeca. Sem isto o filho desistia dos 8 s de timeout do quadro 0. Nao consome nada. */
        while (!sim_active() && rt_now_us() - t0 < limit) {
            fd_set fs;
            struct timeval tv = {0, 200000};
            FD_ZERO(&fs); FD_SET(l->peers[0].fd, &fs);
            if (select((int)l->peers[0].fd + 1, &fs, NULL, NULL, &tv) != 0) break;
            if (!idle()) break;
        }
    }
    t->lockstep = 1;
    t->frame_end = local_frame_end;
    t->frame_begin = local_frame_begin;
    t->ack = NULL;
    snprintf(l->st.state, sizeof l->st.state, "lockstep");
    return 0;
}

/* =============================================================== instancia global e configuracao */
int rt_link_nopace;
Sio *rt_sio_cur;

uint16_t rt_sio_irq_pending(void) {
    if (!rt_sio_cur || !rt_sio_cur->irq_flag) return 0;
    return (hw_read(0x04000208u, 2) & 1) && (hw_read(0x04000200u, 2) & 0x80) ? 0x0080 : 0;
}

int rt_sio_setup(const char *mode, int players, int port, const char *host, int delay) {
    if (!mode || !strcmp(mode, "off") || !*mode) return 0;
    LinkTransport *t = NULL;
    if (!strcmp(mode, "host")) t = link_local_host(players, port);
    else if (!strcmp(mode, "join")) t = link_local_join(host, port);
    else if (!strcmp(mode, "solo")) t = link_null_create();
    else { rt_log("[link] unknown mode '%s' (use off|host|join)", mode); return -1; }
    if (!t) return -2;
    if (!getenv("KIRBY_LINK_FREE") && strcmp(mode, "solo")) {   /* KIRBY_LINK_FREE=1: troca livre sem lockstep (diagnostico) */
        const char *w = getenv("KIRBY_LINK_WAIT_S");
        if (link_local_lockstep(&t, w ? atoi(w) : 120, delay) < 0) return -3;
    }
    rt_sio_cur = rt_sio_new(t);
    return 0;
}

static char *trim_(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

void rt_link_set_bind(const char *addr) {
#ifdef _WIN32
    _putenv_s("KIRBY_LINK_BIND", addr);
#else
    setenv("KIRBY_LINK_BIND", addr, 1);
#endif
}

/* [link] bind= (host: endereco de escuta). Fora de rt_sio_read_ini de proposito: o teste de paridade da Gwen
 * extrai aquela funcao sozinha (hw/tests/extract_rt_ini.py) e ela nao pode depender de mais nada. */
void rt_link_read_bind(const char *path) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char line[256], sect[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        char *s = trim_(line);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", s + 1); }
            for (char *p = sect; *p; p++) *p = (char)tolower((unsigned char)*p);
            continue;
        }
        char *eq = strchr(s, '=');
        if (strcmp(sect, "link") || !eq) continue;
        *eq = 0;
        char *k = trim_(s), *v = trim_(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(k, "bind") && *v && !getenv("KIRBY_LINK_BIND")) rt_link_set_bind(v);
    }
    fclose(f);
}

/* [link] mode= players= port= host=  (o parser compartilhado ignora secoes desconhecidas) */
void rt_sio_read_ini(const char *path, char *mode, int *players, int *port, char *host, int *delay) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char line[256], sect[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        char *s = trim_(line);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", s + 1); }
            for (char *p = sect; *p; p++) *p = (char)tolower((unsigned char)*p);
            continue;
        }
        if (strcmp(sect, "link")) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim_(s), *v = trim_(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(k, "mode")) snprintf(mode, 16, "%s", v);
        else if (!strcmp(k, "players")) { int n = atoi(v); if (n >= 2 && n <= 4) *players = n; }
        else if (!strcmp(k, "port")) { int n = atoi(v); if (n >= 1 && n <= 65535) *port = n; }
        else if (!strcmp(k, "host")) snprintf(host, 64, "%s", v);
        else if (!strcmp(k, "delay")) { if (!strcmp(v, "auto")) *delay = -1; else { int n = atoi(v); if (n >= 0 && n <= 8) *delay = n; } }
    }
    fclose(f);
}

void rt_sio_report(void) {
    if (!rt_sio_cur) return;
    Sio *s = rt_sio_cur;
    printf("LINK packets (12 halfwords, sum must be 0xFFF1) seen by this player, per sender slot:");
    for (int p = 0; p < 4; p++)
        printf(" P%d valid=%llu invalid=%llu sync=%llu%s", p + 1, (unsigned long long)s->pk_valid[p],
               (unsigned long long)s->pk_invalid[p], (unsigned long long)s->pk_sync[p], p < 3 ? " |" : "\n");
    printf("LINK: %llu exchanges completed (%s%s)\n", (unsigned long long)rt_sio_exchanges,
           rt_sio_cur->tp ? rt_sio_cur->tp->name : "none", rt_sio_cur->tp && rt_sio_cur->tp->lockstep ? ", lockstep" : "");
    printf("LINK exchange hash: %016llx over %llu exchanges, %u frames\n", (unsigned long long)s->xhash,
           (unsigned long long)s->xcount, s->frame);
}
