/* rt_sio.h - SIO em modo multi-play (cabo link) com transporte plugavel.
 * Registradores (barramento do runtime): SIOMULTI0-3 0x04000120-26, SIOCNT 0x04000128,
 * SIOMLT_SEND 0x0400012A, RCNT 0x04000134, mais o bit 7 (serial) de IF 0x04000202 (o hw/ nao tem
 * o IRQ serial; o runtime o entrega pelo mesmo caminho do IRQ de VBlank: rt_irq_service).
 * Sem transporte (ou mode=off no jogo) o cabo esta "desconectado". */
#ifndef RT_SIO_H
#define RT_SIO_H
#include <stdint.h>

#define LINK_ABSENT 0xFFFFu

typedef struct LinkEvent {
    int type;              /* 1 = ROUND: o mestre iniciou uma troca (values = resultado) */
    uint16_t values[4];
} LinkEvent;

/* Uma troca do mestre dentro de um quadro: `off` = ciclos desde o inicio do quadro do mestre. */
#define LINK_MAX_ROUNDS 32
typedef struct LinkRound {
    uint32_t off;
    uint16_t v[4];
} LinkRound;

/* Medicao da sessao (host e filho): o launcher le o JSON gravado por --link-status / KIRBY_LINK_STATUS. */
typedef struct LinkStats {
    int role;                  /* 0 = host, 1 = filho */
    int players, delay;        /* jogadores; K (quadros de folga do host) */
    uint32_t mask;             /* jogadores presentes */
    char state[16];            /* starting | lockstep | free | dropped */
    uint32_t frame;            /* quadros do hw concluidos */
    double rtt_ms[4], rtt_avg_ms[4], rtt_max_ms[4];   /* host: por filho (PING/PONG) */
    uint64_t wait_frames; double wait_ms, wait_max_ms; /* quadros em que esperou o outro lado */
    uint32_t frames_behind;    /* filho: quadros do host ja recebidos e ainda nao executados */
    uint64_t tx[4], rx[4], tx_bytes, rx_bytes;         /* bytes pelo socket (cabecalho incluso) */
    double seconds;
    uint64_t underflows;
    int desync; uint32_t desync_frame;
} LinkStats;

typedef struct LinkTransport LinkTransport;
struct LinkTransport {
    const char *name;                  /* "null" | "hub" (no processo) | "local" (TCP 127.0.0.1) */
    int player_id;                     /* 0 = mestre; 1..3 = filho (pode mudar ao conectar) */
    int players;                       /* jogadores configurados (1..4) */
    uint32_t (*connected_mask)(LinkTransport *);          /* bit i = jogador i presente (inclui o proprio) */
    void (*publish)(LinkTransport *, uint16_t value);     /* meu SIOMLT_SEND atual (nao bloqueia) */
    int (*exchange)(LinkTransport *, uint16_t my_value, uint16_t out[4]);   /* so mestre: 4 valores */
    int (*poll)(LinkTransport *, LinkEvent *);            /* atende a rede; 1 = evento devolvido */
    void (*close)(LinkTransport *);
    void (*ack)(LinkTransport *);      /* filho: terminei a troca E o handler ja rodou (opcional) */
    /* LOCKSTEP POR QUADRO (lockstep != 0): o host e a referencia; o filho so executa o quadro N depois de o
     * host terminar o N, e o host so comeca o N+1 depois de o filho terminar o N. O host registra as trocas
     * do quadro (LinkRound) e as entrega no fim do quadro; o filho as reproduz nos mesmos deslocamentos e
     * devolve o SIOMLT_SEND que tinha no INICIO de cada troca (`sends`, uma por troca). O host consome essa
     * sequencia em ordem (FIFO) com atraso fixo de LINK_DELAY trocas (~1,2 quadro): o filho nao pode
     * influenciar uma troca do quadro N que o host ja executou, e o FIFO mantem o fluxo de pacotes alinhado. */
    int lockstep;
    void (*frame_end)(LinkTransport *, uint32_t frame, const uint16_t *sends, int ns, const LinkRound *r, int n, uint64_t xhash);
    int (*stats)(LinkTransport *, LinkStats *);
    int (*frame_begin)(LinkTransport *, uint32_t frame, LinkRound *out, int max);   /* filho: espera o quadro; n */
    void *priv;
};

typedef struct Sio Sio;

/* ---- transportes ---- */
LinkTransport *link_null_create(void);
/* hub em processo: cria `players` transportes ligados (out[0] = mestre). Para testes. */
void link_hub_create(int players, LinkTransport *out[4]);
void link_hub_disconnect(LinkTransport *t);   /* simula soltar o cabo desse jogador */
LinkTransport *link_local_host(int players, int port);
LinkTransport *link_local_join(const char *host, int port);
/* liga o lockstep por quadro no transporte local; host: espera os `players` entrarem, join: conecta
 * (ambos ate `wait_s` segundos). 0 = ok, <0 = nao conectou (o transporte e fechado) */
int link_local_lockstep(LinkTransport **t, int wait_s, int delay);   /* delay = K (0..8) ou -1 = automatico (mede o RTT) */
void rt_link_set_bind(const char *addr);      /* host: endereco de escuta (padrao 127.0.0.1; 0.0.0.0 abre para a LAN) */
void rt_link_read_bind(const char *ini_path); /* [link] bind= do kirby.ini (nao sobrepoe --link-bind / KIRBY_LINK_BIND) */
void rt_link_sim(const char *spec);           /* "latencia_ms,jitter_ms,perda_%": rede simulada (testes) */
extern int (*rt_link_idle)(void);             /* esperas longas atendem a janela; 0 = usuario fechou */

/* ---- emulacao (instancias independentes: testes criam varias) ---- */
Sio *rt_sio_new(LinkTransport *t);            /* t == NULL: cabo desconectado (jogador sozinho) */
void rt_sio_free(Sio *s);
int rt_sio_read(Sio *s, uint32_t addr, int size, uint32_t *out);   /* 1 = o endereco e do SIO/IF */
int rt_sio_write(Sio *s, uint32_t addr, int size, uint32_t v);
void rt_sio_advance(Sio *s, uint32_t cycles);
uint32_t rt_sio_cycles_to_event(Sio *s);      /* 0 = nada pendente */
void rt_sio_poll(Sio *s);
void rt_sio_frame_end(Sio *s);                /* fim de cada quadro do hw: barreira do lockstep */
int rt_sio_irq_flag(Sio *s);

/* ---- instancia global do jogo (NULL = mode=off: tudo passa direto ao hw/) ---- */
extern Sio *rt_sio_cur;
int rt_sio_setup(const char *mode, int players, int port, const char *host, int delay);   /* 0 ok, <0 erro; delay -1 = auto */
void rt_sio_read_ini(const char *path, char *mode, int *players, int *port, char *host, int *delay);
int rt_sio_stats(LinkStats *out);             /* 1 = ha link ativo */
extern const char *rt_link_status_path;       /* --link-status: JSON atualizado a cada 60 quadros */
uint16_t rt_sio_irq_pending(void);            /* 0x0080 se IRQ serial habilitado e pendente */
void rt_sio_report(void);
extern uint64_t rt_sio_exchanges;
extern int rt_link_nopace;                    /* --link-nopace: sem ritmo de 60 fps (testes) */

#endif
