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

/* ---- emulacao (instancias independentes: testes criam varias) ---- */
Sio *rt_sio_new(LinkTransport *t);            /* t == NULL: cabo desconectado (jogador sozinho) */
void rt_sio_free(Sio *s);
int rt_sio_read(Sio *s, uint32_t addr, int size, uint32_t *out);   /* 1 = o endereco e do SIO/IF */
int rt_sio_write(Sio *s, uint32_t addr, int size, uint32_t v);
void rt_sio_advance(Sio *s, uint32_t cycles);
uint32_t rt_sio_cycles_to_event(Sio *s);      /* 0 = nada pendente */
void rt_sio_poll(Sio *s);
int rt_sio_irq_flag(Sio *s);

/* ---- instancia global do jogo (NULL = mode=off: tudo passa direto ao hw/) ---- */
extern Sio *rt_sio_cur;
int rt_sio_setup(const char *mode, int players, int port, const char *host);   /* 0 ok, <0 erro */
void rt_sio_read_ini(const char *path, char *mode, int *players, int *port, char *host);
uint16_t rt_sio_irq_pending(void);            /* 0x0080 se IRQ serial habilitado e pendente */
void rt_sio_report(void);
extern uint64_t rt_sio_exchanges;

#endif
