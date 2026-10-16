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
    if (s->siocnt & 0x4000) s->irq_flag = 1;
    if (s->tp && s->tp->ack && s->tp->player_id != 0) s->ack_pending = 1;
}

void rt_sio_advance(Sio *s, uint32_t cycles) {
    if (!s->busy) return;
    if (cycles >= s->remain) complete(s);
    else s->remain -= cycles;
}
uint32_t rt_sio_cycles_to_event(Sio *s) { return s->busy ? (s->remain ? s->remain : 1) : 0; }
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
    begin(s, vals);
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

/* =============================================================== transporte "local": TCP em 127.0.0.1 */
/* Cada instancia roda no SEU relogio (processos independentes). Para a troca nao usar um SIOMLT_SEND
 * velho, o mestre so inicia a rodada N+1 depois que todo filho presente confirmou (ACK) a rodada N: o ACK
 * sai quando o filho terminou a troca E o handler do IRQ serial (que escreve o proximo SEND) ja rodou.
 * Espera maxima 50 ms de relogio real por filho (depois segue com o valor que tiver). */
enum { M_HELLO = 1, M_SEND = 2, M_ROUND = 3, M_ACK = 4 };
typedef struct { uint8_t type, a; uint16_t v[4]; uint16_t round; } Msg;   /* 12 bytes */

typedef struct Peer { sock_t fd; Msg buf; int have; } Peer;
typedef struct Local {
    int host, port, players;
    sock_t listener;
    Peer peers[4];            /* host: indice = id do filho; join: peers[0] = host */
    uint16_t latest[4];       /* host: ultimo SEND de cada filho */
    uint16_t acked[4];        /* host: ultima rodada confirmada por cada filho */
    uint16_t round;           /* host: ultima rodada enviada; join: ultima recebida */
    uint32_t mask;            /* host: presentes; join: ultima mascara recebida */
    uint16_t my_send;         /* join: reenvia ao reconectar */
    char addr[64];
    unsigned tries;
    uint64_t ack_waits, ack_timeouts;
} Local;

double rt_now_us(void);

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
static int send_msg(sock_t fd, const Msg *m) {
    int n = (int)send(fd, (const char *)m, (int)sizeof *m, 0);
    return n == (int)sizeof *m ? 0 : -1;
}
/* 1 = mensagem completa; 0 = nada; -1 = desconectou */
static int recv_msg(Peer *p, Msg *m) {
    for (;;) {
        int n = (int)recv(p->fd, (char *)&p->buf + p->have, (int)sizeof(Msg) - p->have, 0);
        if (n == 0) return -1;
        if (n < 0) {
#ifdef _WIN32
            return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
#else
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
#endif
        }
        p->have += n;
        if (p->have == (int)sizeof(Msg)) { *m = p->buf; p->have = 0; return 1; }
    }
}

static uint32_t local_mask(LinkTransport *t) { return ((Local *)t->priv)->mask | (1u << t->player_id); }

static void local_drop(Local *l, int i) {
    if (l->peers[i].fd != SOCK_BAD) sock_close(l->peers[i].fd);
    l->peers[i].fd = SOCK_BAD; l->peers[i].have = 0;
    if (l->host) { l->mask &= ~(1u << i); l->latest[i] = LINK_ABSENT; rt_log("[link] player %d disconnected", i + 1); }
}

static void local_publish(LinkTransport *t, uint16_t v) {
    Local *l = t->priv;
    l->my_send = v;
    if (!l->host && l->peers[0].fd != SOCK_BAD) {
        Msg m = {M_SEND, (uint8_t)t->player_id, {v, 0, 0, 0}, 0};
        if (send_msg(l->peers[0].fd, &m) != 0) local_drop(l, 0);
    }
}

static void local_ack(LinkTransport *t) {   /* filho: confirma a rodada recebida */
    Local *l = t->priv;
    if (!l->host && l->peers[0].fd != SOCK_BAD) {
        Msg m = {M_ACK, (uint8_t)t->player_id, {0, 0, 0, 0}, l->round};
        if (send_msg(l->peers[0].fd, &m) != 0) local_drop(l, 0);
    }
}

/* host: aceita filhos novos e le SEND/ACK de quem ja esta conectado */
static void host_pump(Local *l) {
    for (;;) {
        sock_t c = accept(l->listener, NULL, NULL);
        if (c == SOCK_BAD) break;
        int id = 1;
        while (id < l->players && l->peers[id].fd != SOCK_BAD) id++;
        if (id >= l->players) { sock_close(c); continue; }   /* sala cheia */
        set_nonblock(c);
        l->peers[id].fd = c; l->peers[id].have = 0;
        l->mask |= 1u << id;
        l->latest[id] = LINK_ABSENT;
        l->acked[id] = l->round;   /* entra sincronizado: nao atrasa a rodada em curso */
        Msg h = {M_HELLO, (uint8_t)id, {(uint16_t)l->players, (uint16_t)l->mask, 0, 0}, 0};
        send_msg(c, &h);
        rt_log("[link] player %d connected", id + 1);
    }
    for (int i = 1; i < l->players; i++) {
        if (l->peers[i].fd == SOCK_BAD) continue;
        Msg m;
        int r;
        while ((r = recv_msg(&l->peers[i], &m)) == 1) {
            if (m.type == M_SEND) l->latest[i] = m.v[0];
            else if (m.type == M_ACK) l->acked[i] = m.round;
        }
        if (r < 0) local_drop(l, i);
    }
}

static int local_exchange(LinkTransport *t, uint16_t my, uint16_t out[4]) {
    Local *l = t->priv;
    /* espera os filhos terminarem a rodada anterior (e o handler deles escrever o proximo SEND) */
    double t0 = 0;
    static int noack = -1;
    if (noack < 0) noack = getenv("KIRBY_LINK_NOACK") != NULL;   /* diagnostico: sem rendezvous */
    for (;;) {
        host_pump(l);
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
    Msg m = {M_ROUND, (uint8_t)l->mask, {out[0], out[1], out[2], out[3]}, l->round};
    for (int i = 1; i < l->players; i++)
        if (l->peers[i].fd != SOCK_BAD && send_msg(l->peers[i].fd, &m) != 0) local_drop(l, i);
    return 1;
}

static int local_poll(LinkTransport *t, LinkEvent *ev) {
    Local *l = t->priv;
    if (l->host) { host_pump(l); return 0; }
    if (l->peers[0].fd == SOCK_BAD) {
        if ((l->tries++ & 0x3F) != 0) return 0;   /* tenta conectar de vez em quando */
        sock_t c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)l->port);
        inet_pton(AF_INET, l->addr, &a.sin_addr);
        if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) { sock_close(c); return 0; }
        set_nonblock(c);
        l->peers[0].fd = c; l->peers[0].have = 0;
        return 0;
    }
    Msg m;
    int r = recv_msg(&l->peers[0], &m);
    if (r < 0) { local_drop(l, 0); t->player_id = 1; return 0; }
    if (r == 0) return 0;
    if (m.type == M_HELLO) {
        t->player_id = m.a; l->players = t->players = m.v[0]; l->mask = m.v[1];
        rt_log("[link] connected as player %d of %d", m.a + 1, m.v[0]);
        local_publish(t, l->my_send);
        return 0;
    }
    if (m.type == M_ROUND) {
        l->mask = m.a;
        l->round = m.round;
        ev->type = 1;
        memcpy(ev->values, m.v, sizeof ev->values);
        return 1;
    }
    return 0;
}

static void local_close(LinkTransport *t) {
    Local *l = t->priv;
    if (l->host && l->ack_timeouts)
        rt_log("[link] master waited for child acks %llu times, %llu hit the 50 ms limit",
               (unsigned long long)l->ack_waits, (unsigned long long)l->ack_timeouts);
    for (int i = 0; i < 4; i++) if (l->peers[i].fd != SOCK_BAD) sock_close(l->peers[i].fd);
    if (l->host && l->listener != SOCK_BAD) sock_close(l->listener);
    free(l);
    free(t);
}

static LinkTransport *local_new(Local *l, int id) {
    LinkTransport *t = (LinkTransport *)calloc(1, sizeof *t);
    t->name = "local"; t->player_id = id; t->players = l->players; t->priv = l;
    t->connected_mask = local_mask; t->publish = local_publish; t->exchange = local_exchange;
    t->poll = local_poll; t->close = local_close; t->ack = local_ack;
    return t;
}

LinkTransport *link_local_host(int players, int port) {
    net_init();
    Local *l = (Local *)calloc(1, sizeof *l);
    l->host = 1; l->players = players < 2 ? 2 : players > 4 ? 4 : players; l->port = port;
    for (int i = 0; i < 4; i++) { l->peers[i].fd = SOCK_BAD; l->latest[i] = LINK_ABSENT; }
    l->mask = 1;
    l->listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l->listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(l->listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(l->listener, 4) != 0) {
        rt_log("[link] cannot listen on 127.0.0.1:%d", port);
        sock_close(l->listener);
        free(l);
        return NULL;
    }
    set_nonblock(l->listener);
    rt_log("[link] hosting %d players on 127.0.0.1:%d", l->players, port);
    return local_new(l, 0);
}

LinkTransport *link_local_join(const char *host, int port) {
    net_init();
    Local *l = (Local *)calloc(1, sizeof *l);
    l->host = 0; l->players = 2; l->port = port;
    snprintf(l->addr, sizeof l->addr, "%s", host && *host ? host : "127.0.0.1");
    for (int i = 0; i < 4; i++) { l->peers[i].fd = SOCK_BAD; l->latest[i] = LINK_ABSENT; }
    l->listener = SOCK_BAD;
    rt_log("[link] joining %s:%d", l->addr, port);
    return local_new(l, 1);
}

/* =============================================================== instancia global e configuracao */
Sio *rt_sio_cur;

uint16_t rt_sio_irq_pending(void) {
    if (!rt_sio_cur || !rt_sio_cur->irq_flag) return 0;
    return (hw_read(0x04000208u, 2) & 1) && (hw_read(0x04000200u, 2) & 0x80) ? 0x0080 : 0;
}

int rt_sio_setup(const char *mode, int players, int port, const char *host) {
    if (!mode || !strcmp(mode, "off") || !*mode) return 0;
    LinkTransport *t = NULL;
    if (!strcmp(mode, "host")) t = link_local_host(players, port);
    else if (!strcmp(mode, "join")) t = link_local_join(host, port);
    else if (!strcmp(mode, "solo")) t = link_null_create();
    else { rt_log("[link] unknown mode '%s' (use off|host|join)", mode); return -1; }
    if (!t) return -2;
    rt_sio_cur = rt_sio_new(t);
    return 0;
}

static char *trim_(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* [link] mode= players= port= host=  (o parser compartilhado ignora secoes desconhecidas) */
void rt_sio_read_ini(const char *path, char *mode, int *players, int *port, char *host) {
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
    printf("LINK: %llu exchanges completed (%s)\n", (unsigned long long)rt_sio_exchanges,
           rt_sio_cur->tp ? rt_sio_cur->tp->name : "none");
}
