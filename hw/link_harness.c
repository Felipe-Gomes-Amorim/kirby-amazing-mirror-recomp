/* link_harness.c - see link_harness.h. Needs hw.c compiled four times with -DHW_PREFIX=i0_ .. i3_. */
#include <stdio.h>
#include <string.h>
#include "link_harness.h"

/* one set of declarations per instance prefix */
#define INST_DECL(P)                                                                                            \
    uint32_t P##hw_read(uint32_t addr, int size);                                                               \
    void P##hw_write(uint32_t addr, int size, uint32_t v);                                                      \
    void P##hw_reset(void);                                                                                     \
    void P##hw_step(uint32_t cycles);                                                                           \
    uint16_t P##hw_irq_pending(void);                                                                           \
    void P##hw_sio_set_link(void *ctx, void (*start)(void *, uint16_t));                                        \
    void P##hw_sio_set_slot(int slot);                                                                          \
    void P##hw_sio_set_sd(int sd);                                                                              \
    int P##hw_sio_multi_mode(void);                                                                             \
    uint16_t P##hw_sio_send_value(void);                                                                        \
    uint32_t P##hw_sio_slot_cycles(void);                                                                       \
    void P##hw_sio_begin(void);                                                                                 \
    void P##hw_sio_deliver(const uint16_t multi[4], int error);                                                 \
    /* the instances never use DMA here, but hw.c needs its memory callbacks */                                 \
    uint32_t P##gba_read8(uint32_t a) { (void)a; return 0; }                                                    \
    uint32_t P##gba_read16(uint32_t a) { (void)a; return 0; }                                                   \
    uint32_t P##gba_read32(uint32_t a) { (void)a; return 0; }                                                   \
    void P##gba_write8(uint32_t a, uint32_t v) { (void)a; (void)v; }                                            \
    void P##gba_write16(uint32_t a, uint32_t v) { (void)a; (void)v; }                                           \
    void P##gba_write32(uint32_t a, uint32_t v) { (void)a; (void)v; }

INST_DECL(i0_)
INST_DECL(i1_)
INST_DECL(i2_)
INST_DECL(i3_)

typedef struct {
    uint32_t (*read)(uint32_t, int);
    void (*write)(uint32_t, int, uint32_t);
    void (*reset)(void);
    void (*step)(uint32_t);
    uint16_t (*irq_pending)(void);
    void (*set_link)(void *, void (*)(void *, uint16_t));
    void (*set_slot)(int);
    void (*set_sd)(int);
    int (*multi_mode)(void);
    uint16_t (*send_value)(void);
    uint32_t (*slot_cycles)(void);
    void (*begin)(void);
    void (*deliver)(const uint16_t *, int);
} Inst;

#define INST_INIT(P) { P##hw_read, P##hw_write, P##hw_reset, P##hw_step, P##hw_irq_pending, P##hw_sio_set_link, \
                       P##hw_sio_set_slot, P##hw_sio_set_sd, P##hw_sio_multi_mode, P##hw_sio_send_value,        \
                       P##hw_sio_slot_cycles, P##hw_sio_begin, P##hw_sio_deliver }

static const Inst INST[LH_MAX] = { INST_INIT(i0_), INST_INIT(i1_), INST_INIT(i2_), INST_INIT(i3_) };

static const uint16_t FFFF4[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };

/* ---------------------------------------------------------------- cable state */
static int n_inst;
static int plugged[LH_MAX];
static uint64_t now;

typedef struct {
    int active;
    uint64_t start;
    uint32_t T;                      /* cycles per slot */
    int max_slot;                    /* highest participating slot */
    int part[LH_MAX];                /* took part when the master started */
    int sampled[LH_MAX];             /* its turn has passed */
    uint16_t sent[LH_MAX];
    int err;
    int order[LH_MAX]; int norder;
} Xfer;
static Xfer X;
static int transfers;
static int last_order[LH_MAX], last_n;
static uint16_t last_val[LH_MAX];
static int last_err;

static int in_multi(int k) { return k < n_inst && plugged[k] && INST[k].multi_mode(); }

static void refresh_sd(void) {
    int n = 0;
    for (int k = 0; k < n_inst; k++) if (in_multi(k)) n++;
    for (int k = 0; k < n_inst; k++) INST[k].set_sd(in_multi(k) && n >= 2);
}

static void finish(const uint16_t multi[4], int err, int deliver_to_dropped) {
    for (int k = 0; k < n_inst; k++) {
        if (!X.part[k]) continue;
        if (!plugged[k] && !deliver_to_dropped) continue;      /* it already got its abort delivery when it unplugged */
        INST[k].deliver(multi, err);
    }
    memcpy(last_val, multi, sizeof last_val);
    memcpy(last_order, X.order, sizeof last_order);
    last_n = X.norder;
    last_err = err;
    transfers++;
    X.active = 0;
}

/* the master wrote the start bit */
static void on_start(void *ctx, uint16_t data) {
    int slot = (int)(intptr_t)ctx;
    if (slot != 0 || X.active) return;
    memset(&X, 0, sizeof X);
    X.active = 1;
    X.start = now;
    X.T = INST[0].slot_cycles();
    for (int k = 0; k < LH_MAX; k++) X.sent[k] = 0xFFFF;
    X.sent[0] = data;
    X.part[0] = 1;                                               /* the master itself always takes part */
    for (int k = 1; k < n_inst; k++) X.part[k] = plugged[0] && in_multi(k);      /* an unplugged master is alone */
    X.max_slot = 0;
    for (int k = 1; k < n_inst; k++) if (X.part[k]) X.max_slot = k;
    for (int k = 1; k < n_inst; k++) if (X.part[k]) INST[k].begin();             /* children see the busy bit too */
    X.sampled[0] = 1;
    X.order[X.norder++] = 0;
    if (X.max_slot == 0) X.err = 1;                              /* sent into the void */
}

/* time of the next cable event (capture of a slot, or the end), relative to now; ~0 = none */
static uint64_t next_event(void) {
    if (!X.active) return ~(uint64_t)0;
    uint64_t best = X.start + (uint64_t)(X.max_slot + 1) * X.T;           /* end */
    for (int k = 1; k <= X.max_slot; k++)
        if (X.part[k] && !X.sampled[k]) {
            uint64_t t = X.start + (uint64_t)k * X.T;
            if (t < best) best = t;
        }
    return best > now ? best - now : 0;
}

static void process_events(void) {
    while (X.active) {
        int progressed = 0;
        for (int k = 1; k <= X.max_slot; k++)
            if (X.part[k] && !X.sampled[k] && now >= X.start + (uint64_t)k * X.T) {
                X.sampled[k] = 1;
                if (plugged[k]) { X.sent[k] = INST[k].send_value(); X.order[X.norder++] = k; }
                else { X.sent[k] = 0xFFFF; X.err = 1; }                       /* it left before its turn */
                progressed = 1;
            }
        if (now >= X.start + (uint64_t)(X.max_slot + 1) * X.T) {
            uint16_t m[4];
            for (int k = 0; k < 4; k++) m[k] = X.sent[k];
            finish(m, X.err, 0);
            return;
        }
        if (!progressed) return;
    }
}

/* ---------------------------------------------------------------- public API */
void lh_init(int n) {
    if (n < 2) n = 2;
    if (n > LH_MAX) n = LH_MAX;
    n_inst = n;
    memset(&X, 0, sizeof X);
    now = 0; transfers = 0; last_n = 0; last_err = 0;
    for (int k = 0; k < LH_MAX; k++) last_val[k] = 0xFFFF;
    for (int k = 0; k < n; k++) {
        plugged[k] = 1;
        INST[k].reset();
        INST[k].set_slot(k);
        INST[k].set_link((void *)(intptr_t)k, on_start);
    }
    refresh_sd();
}

void lh_connect(int slot, int on) {
    if (slot < 0 || slot >= n_inst || plugged[slot] == (on ? 1 : 0)) return;
    plugged[slot] = on ? 1 : 0;
    if (!on && X.active && X.part[slot]) {
        if (slot == 0) {                                 /* the master left: every child is aborted at once */
            uint16_t m[4] = { X.sent[0], 0xFFFF, 0xFFFF, 0xFFFF };
            for (int k = 1; k < n_inst; k++) if (X.part[k]) INST[k].deliver(FFFF4, 1);
            INST[0].deliver(m, 1);
            memcpy(last_val, FFFF4, sizeof last_val);
            last_val[0] = X.sent[0];
            memcpy(last_order, X.order, sizeof last_order);
            last_n = X.norder;
            last_err = 1;
            transfers++;
            X.active = 0;
        } else {
            /* a child unplugs: its own unit aborts now. Before its turn the others see 0xFFFF + error (set when
             * its turn arrives); after its turn its data already went out and the transfer is not an error. */
            INST[slot].deliver(FFFF4, 1);
        }
    }
    refresh_sd();
}

int lh_connected(int slot) { return slot >= 0 && slot < n_inst && plugged[slot]; }

void lh_step(uint32_t cycles) {
    while (cycles) {
        refresh_sd();
        process_events();                                /* anything due at `now` happens before time moves */
        uint64_t ev = next_event();                      /* > 0 here, or ~0 when idle */
        uint32_t c = (ev != ~(uint64_t)0 && ev < cycles) ? (uint32_t)ev : cycles;
        for (int k = 0; k < n_inst; k++) INST[k].step(c);
        now += c;
        cycles -= c;
    }
    refresh_sd();
    process_events();
}

uint32_t lh_read(int slot, uint32_t addr, int size) { return INST[slot].read(addr, size); }
void lh_write(int slot, uint32_t addr, int size, uint32_t v) { INST[slot].write(addr, size, v); refresh_sd(); }
uint16_t lh_irq_pending(int slot) { return INST[slot].irq_pending(); }
uint32_t lh_slot_cycles(void) { return INST[0].slot_cycles(); }

void lh_enter_multi(int slot, int irq) {
    INST[slot].write(0x04000134, 2, 0x0000);                       /* RCNT: serial mode */
    INST[slot].write(0x04000128, 2, 0x2003 | (irq ? 0x4000 : 0));  /* multi-play, 115200 baud, IRQ enable */
    if (irq) {
        INST[slot].write(0x04000200, 2, 0x0080);                   /* IE: serial */
        INST[slot].write(0x04000208, 2, 1);                        /* IME */
    }
    refresh_sd();
}
void lh_detach(int slot) { INST[slot].set_link(NULL, NULL); }      /* a console with no cable attached */
void lh_set_send(int slot, uint16_t v) { INST[slot].write(0x0400012A, 2, v); }
void lh_master_start(void) {
    uint16_t cnt = (uint16_t)INST[0].read(0x04000128, 2);
    INST[0].write(0x04000128, 2, (uint16_t)(cnt | 0x80));
}

int lh_transfers(void) { return transfers; }
int lh_active(void) { return X.active; }
int lh_last_order(int *slots, int cap) {
    int n = last_n < cap ? last_n : cap;
    for (int i = 0; i < n; i++) slots[i] = last_order[i];
    return last_n;
}
uint16_t lh_last_value(int slot) { return slot >= 0 && slot < 4 ? last_val[slot] : 0xFFFF; }
int lh_last_error(void) { return last_err; }
