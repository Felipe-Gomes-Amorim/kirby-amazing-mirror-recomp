/* test_link.c - SIO multi-play between 2 to 4 independent hw instances through hw/link_harness.c (no game).
 * Checks the send order (master, then children 1, 2, 3), 0xFFFF for absent units, the ID/SI/SD bits, the error
 * bit, the serial IRQ at the end, a master with no children, a child or the master dropping mid-transfer, and the
 * transfer time at different baud rates. Build/run: hw/tests/run_link.ps1. */
#include <stdio.h>
#include <string.h>
#include "../link_harness.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

#define SIOCNT 0x04000128u
#define MULTI(i) (0x04000120u + 2u * (unsigned)(i))
#define IFREG 0x04000202u

static uint16_t rd(int s, uint32_t a) { return (uint16_t)lh_read(s, a, 2); }
static int busy(int s) { return (rd(s, SIOCNT) & 0x80) != 0; }
static int err_bit(int s) { return (rd(s, SIOCNT) & 0x40) != 0; }
static int id_bits(int s) { return (rd(s, SIOCNT) >> 4) & 3; }
static int si_bit(int s) { return (rd(s, SIOCNT) & 0x04) != 0; }
static int sd_bit(int s) { return (rd(s, SIOCNT) & 0x08) != 0; }
static int serial_irq(int s) { return (lh_irq_pending(s) & 0x80) != 0; }
static void ack_irq(int s) { lh_write(s, IFREG, 2, 0x80); }

static void setup(int n, int irq) {
    lh_init(n);
    for (int k = 0; k < n; k++) { lh_enter_multi(k, irq); lh_set_send(k, (uint16_t)(0xA000 + k * 0x111)); }
}

static int order_is(const int *exp, int n) {
    int got[8];
    int cnt = lh_last_order(got, 8);
    if (cnt != n) return 0;
    for (int i = 0; i < n; i++) if (got[i] != exp[i]) return 0;
    return 1;
}

int main(void) {
    /* ---------- T1: two units, the whole life of a transfer ---------- */
    setup(2, 1);
    const uint32_t T = lh_slot_cycles();
    CHECK(T == (16777216u / 115200u) * 18u, "slot time = 18 bit times at 115200 baud");
    CHECK(sd_bit(0) && sd_bit(1), "SD terminal high on both units (2 units in multi-play)");
    CHECK(!si_bit(0) && si_bit(1), "SI terminal: master 0, child 1");
    CHECK(id_bits(0) == 0 && id_bits(1) == 0, "ID bits are 0 before the first transfer");
    CHECK(rd(0, MULTI(0)) == 0xFFFF && rd(1, MULTI(1)) == 0xFFFF, "SIOMULTI starts at 0xFFFF");
    lh_set_send(0, 0x1111);
    lh_set_send(1, 0x2222);
    lh_master_start();
    CHECK(busy(0) && busy(1), "master and child are busy right after the start bit");
    CHECK(rd(0, MULTI(0)) == 0xFFFF && rd(1, MULTI(1)) == 0xFFFF, "SIOMULTI reset to 0xFFFF at the start");
    lh_step(2 * T - 1);
    CHECK(busy(0) && busy(1) && lh_active() && !serial_irq(0), "still busy 1 cycle before the end (2 slots)");
    lh_step(1);
    CHECK(!busy(0) && !busy(1) && !lh_active() && lh_transfers() == 1, "transfer ends exactly after 2 slots");
    for (int s = 0; s < 2; s++) {
        CHECK(rd(s, MULTI(0)) == 0x1111 && rd(s, MULTI(1)) == 0x2222, "both units hold master and child data");
        CHECK(rd(s, MULTI(2)) == 0xFFFF && rd(s, MULTI(3)) == 0xFFFF, "SIOMULTI2/3 = 0xFFFF (no such units)");
        CHECK(!err_bit(s), "no error on a good transfer");
        CHECK(serial_irq(s), "serial IRQ raised on every unit at the end");
    }
    CHECK(id_bits(0) == 0 && id_bits(1) == 1, "ID bits after the transfer: master 0, child 1");
    const int ord01[] = { 0, 1 };
    CHECK(order_is(ord01, 2), "wire order: master then child 1");
    ack_irq(0);
    CHECK(!serial_irq(0) && serial_irq(1), "IF is write-1-to-clear per unit");
    ack_irq(1);

    /* ---------- T2: send order and the moment each value is sampled (4 units) ---------- */
    setup(4, 1);
    lh_set_send(0, 0xAAA0); lh_set_send(1, 0xAAA1); lh_set_send(2, 0xAAA2); lh_set_send(3, 0xAAA3);
    lh_master_start();
    lh_set_send(0, 0x0000);                                  /* the master value was latched at the start */
    lh_step(T / 2);
    lh_set_send(1, 0xBBB1);                                  /* before child 1's turn (at T): counts */
    lh_step(T / 2 + 10);
    lh_set_send(1, 0xCCC1);                                  /* after its turn: too late */
    lh_set_send(2, 0xBBB2);                                  /* before child 2's turn (at 2T): counts */
    lh_step(4 * T);
    CHECK(lh_transfers() == 1 && !busy(0), "4-unit transfer completed");
    CHECK(rd(0, MULTI(0)) == 0xAAA0 && rd(0, MULTI(1)) == 0xBBB1 && rd(0, MULTI(2)) == 0xBBB2 && rd(0, MULTI(3)) == 0xAAA3,
          "each value is sampled at its unit's turn (master at the start)");
    for (int s = 1; s < 4; s++)
        CHECK(rd(s, MULTI(0)) == 0xAAA0 && rd(s, MULTI(1)) == 0xBBB1 && rd(s, MULTI(2)) == 0xBBB2 && rd(s, MULTI(3)) == 0xAAA3,
              "every unit received the same four values");
    const int ord0123[] = { 0, 1, 2, 3 };
    CHECK(order_is(ord0123, 4), "wire order: master, child 1, child 2, child 3");
    for (int s = 0; s < 4; s++) {
        CHECK(id_bits(s) == s, "ID bits 0,1,2,3");
        CHECK(si_bit(s) == (s != 0), "SI bit: only the master has it low");
        CHECK(serial_irq(s) && !err_bit(s), "IRQ on all four, no error");
    }

    /* ---------- T3: 4 slots, but unit 2 is not plugged ---------- */
    setup(4, 1);
    lh_connect(2, 0);
    CHECK(sd_bit(0) && sd_bit(1) && sd_bit(3) && !sd_bit(2), "SD high only on the plugged units");
    lh_master_start();
    lh_step(4 * T - 1);
    CHECK(busy(0) && busy(3), "the transfer lasts until the last present slot (4 slots)");
    lh_step(1);
    CHECK(!busy(0) && !busy(1) && !busy(3), "done after 4 slots");
    CHECK(rd(0, MULTI(2)) == 0xFFFF, "the absent unit's slot reads 0xFFFF");
    CHECK(rd(0, MULTI(0)) == 0xA000 && rd(0, MULTI(1)) == 0xA111 && rd(0, MULTI(3)) == 0xA333, "present units' values arrive");
    CHECK(!err_bit(0) && !err_bit(1) && !err_bit(3), "a unit that was never there is not an error");
    CHECK(id_bits(3) == 3, "child at slot 3 keeps ID 3 (slots are not compacted)");
    const int ord013[] = { 0, 1, 3 };
    CHECK(order_is(ord013, 3), "wire order skips the absent unit: 0, 1, 3");
    CHECK(!busy(2) && !serial_irq(2), "an unplugged unit sees nothing");

    /* ---------- T4: three units ---------- */
    setup(3, 1);
    lh_master_start();
    lh_step(3 * T);
    CHECK(rd(2, MULTI(2)) == 0xA222 && rd(2, MULTI(3)) == 0xFFFF && id_bits(2) == 2, "3 units: slot 3 reads 0xFFFF, ID 2");

    /* ---------- T5: master without children ---------- */
    setup(2, 1);
    lh_connect(1, 0);
    CHECK(!sd_bit(0), "SD low when the master is alone");
    lh_set_send(0, 0x5555);
    lh_master_start();
    CHECK(busy(0) && !busy(1), "only the master is busy");
    lh_step(T - 1);
    CHECK(busy(0), "a lone master still takes one slot time");
    lh_step(1);
    CHECK(!busy(0) && lh_transfers() == 1, "lone master transfer completes");
    CHECK(rd(0, MULTI(0)) == 0x5555 && rd(0, MULTI(1)) == 0xFFFF && rd(0, MULTI(2)) == 0xFFFF && rd(0, MULTI(3)) == 0xFFFF,
          "SIOMULTI0 = own data, the rest 0xFFFF");
    CHECK(err_bit(0), "error bit set when nobody answered (documented assumption)");
    CHECK(serial_irq(0) && id_bits(0) == 0, "serial IRQ still raised; ID 0");
    CHECK(!serial_irq(1), "the unplugged child gets nothing");
    {   /* a console with no cable model at all: hw completes the transfer by itself */
        setup(2, 1);
        lh_detach(0);
        lh_set_send(0, 0x7777);
        lh_master_start();
        lh_step(T - 1);
        CHECK(busy(0), "no link attached: busy for one slot time");
        lh_step(1);
        CHECK(!busy(0) && rd(0, MULTI(0)) == 0x7777 && rd(0, MULTI(1)) == 0xFFFF && err_bit(0) && serial_irq(0),
              "no link attached: hw completes alone (data, 0xFFFF, error, IRQ) so the game never hangs");
    }

    /* ---------- T6: a child leaves mid-transfer ---------- */
    setup(4, 1);
    lh_master_start();
    lh_step(T + T / 2);                                      /* child 1 has gone; child 2's turn is at 2T */
    lh_connect(2, 0);
    CHECK(!busy(2) && serial_irq(2) && err_bit(2) && rd(2, MULTI(0)) == 0xFFFF, "the unit that left aborts at once (error, IRQ, 0xFFFF)");
    CHECK(busy(0) && lh_active(), "the others keep going");
    lh_step(3 * T);
    CHECK(lh_transfers() == 1 && !busy(0) && !busy(1) && !busy(3), "the transfer still finishes for the others");
    CHECK(rd(0, MULTI(0)) == 0xA000 && rd(0, MULTI(1)) == 0xA111 && rd(0, MULTI(2)) == 0xFFFF && rd(0, MULTI(3)) == 0xA333,
          "dropped child reads 0xFFFF, later child 3 still answers");
    CHECK(err_bit(0) && err_bit(1) && err_bit(3), "error bit set on the remaining units");
    CHECK(lh_last_error() == 1, "harness reports the error");
    const int ord013b[] = { 0, 1, 3 };
    CHECK(order_is(ord013b, 3), "child 2 never put data on the wire");

    setup(4, 1);                                             /* leaving AFTER its own turn is not an error */
    lh_master_start();
    lh_step(T + T / 2);
    lh_connect(1, 0);                                        /* child 1 sent at T, leaves at 1.5T */
    lh_step(3 * T);
    CHECK(rd(0, MULTI(1)) == 0xA111 && !err_bit(0) && !err_bit(2) && !err_bit(3),
          "a child that already sent its data counts, and the transfer is clean for the others");
    CHECK(!busy(1) && err_bit(1), "...but its own unit aborted with the error flag");

    /* ---------- T7: the master leaves mid-transfer ---------- */
    setup(4, 1);
    lh_set_send(0, 0x9999);
    lh_master_start();
    lh_step(T + T / 2);
    ack_irq(0);
    lh_connect(0, 0);
    for (int s = 1; s < 4; s++)
        CHECK(!busy(s) && err_bit(s) && serial_irq(s) && rd(s, MULTI(0)) == 0xFFFF, "children are aborted at once when the master leaves");
    CHECK(!busy(0) && err_bit(0) && rd(0, MULTI(0)) == 0x9999 && rd(0, MULTI(1)) == 0xFFFF, "the master unit keeps its own data only");
    CHECK(lh_transfers() == 1 && !lh_active(), "the aborted transfer is counted once");
    for (int s = 1; s < 4; s++) ack_irq(s);
    lh_step(10 * T);
    CHECK(lh_transfers() == 1 && !serial_irq(1) && !serial_irq(2) && !serial_irq(3), "no late delivery after the abort");

    /* ---------- T8: back to back, and a start while busy ---------- */
    setup(2, 1);
    lh_set_send(0, 0x0101); lh_set_send(1, 0x0202);
    lh_master_start();
    lh_step(T);
    lh_master_start();                                       /* busy: ignored */
    lh_step(T);
    CHECK(lh_transfers() == 1 && !busy(0), "a start bit while busy does not restart the transfer");
    ack_irq(0); ack_irq(1);
    lh_set_send(0, 0x0303); lh_set_send(1, 0x0404);
    lh_master_start();
    lh_step(2 * T);
    CHECK(lh_transfers() == 2 && rd(1, MULTI(0)) == 0x0303 && rd(1, MULTI(1)) == 0x0404, "second transfer carries the new values");
    CHECK(id_bits(1) == 1 && serial_irq(0) && serial_irq(1), "ID persists, IRQ raised again");

    /* ---------- T9: only the master starts ---------- */
    setup(2, 1);
    lh_write(1, SIOCNT, 2, (uint16_t)(rd(1, SIOCNT) | 0x80));
    CHECK(!busy(1) && !busy(0) && !lh_active(), "a start bit written on a child does nothing");
    lh_step(5 * T);
    CHECK(lh_transfers() == 0, "...and no transfer happens");

    /* ---------- T10: register rules ---------- */
    setup(2, 1);
    lh_write(0, MULTI(0), 2, 0x1234);
    CHECK(rd(0, MULTI(0)) == 0xFFFF, "SIOMULTI0 is read-only in multi-play");
    lh_write(0, SIOCNT, 2, 0x6003 | 0x74);                   /* try to force SI, SD, ID, error */
    CHECK(!si_bit(0) && id_bits(0) == 0 && !err_bit(0) && !busy(0), "bits 2, 4-6 are not writable (hardware-driven)");
    CHECK((rd(0, SIOCNT) & 0x700B) == 0x600B, "mode, baud and IRQ-enable are stored; SD comes from the cable");
    lh_write(0, SIOCNT, 2, 0x1234);                          /* mode 1 (normal 32-bit): plain storage as before */
    CHECK(rd(0, SIOCNT) == 0x1234, "outside multi-play SIOCNT is plain storage (unchanged behaviour)");
    lh_write(0, MULTI(0), 2, 0x4321);
    CHECK(rd(0, MULTI(0)) == 0x4321, "outside multi-play SIOMULTI0 is writable (unchanged behaviour)");

    /* ---------- T11: transfer time follows the baud rate ---------- */
    setup(2, 1);
    lh_write(0, SIOCNT, 2, 0x6000); lh_write(1, SIOCNT, 2, 0x6000);       /* ... baud index 0 = 9600 */
    uint32_t T96 = lh_slot_cycles();
    CHECK(T96 == (16777216u / 9600u) * 18u, "slot time at 9600 baud");
    lh_master_start();
    lh_step(2 * T96 - 1);
    CHECK(busy(0) && busy(1), "9600 baud: busy until the second slot ends");
    lh_step(1);
    CHECK(!busy(0) && !busy(1), "9600 baud: done after exactly 2 slots");

    /* ---------- T12: no serial IRQ when it is not enabled ---------- */
    setup(2, 0);
    lh_master_start();
    lh_step(2 * T);
    CHECK(lh_transfers() == 1 && !serial_irq(0) && !serial_irq(1), "no IRQ with SIOCNT bit 14 clear");
    CHECK(((unsigned)lh_read(0, IFREG, 2) & 0x80) == 0, "IF bit 7 stays clear too");

    /* ---------- T13: a unit not in multi-play does not take part ---------- */
    lh_init(3);
    lh_enter_multi(0, 1); lh_enter_multi(2, 1);              /* unit 1 stays in its reset mode */
    lh_set_send(0, 0xD000); lh_set_send(2, 0xD222);
    CHECK(sd_bit(0) && sd_bit(2) && !sd_bit(1), "SD counts only the units in multi-play");
    lh_master_start();
    lh_step(3 * T);
    CHECK(rd(0, MULTI(1)) == 0xFFFF && rd(0, MULTI(2)) == 0xD222 && !err_bit(0), "a unit outside multi-play reads as absent");

    /* ---------- T14: SD follows plugging ---------- */
    lh_init(3);
    for (int k = 0; k < 3; k++) lh_enter_multi(k, 0);
    CHECK(sd_bit(0) && sd_bit(1) && sd_bit(2), "3 units: SD high");
    lh_connect(1, 0); lh_connect(2, 0);
    CHECK(!sd_bit(0), "only the master left: SD low");
    lh_connect(2, 1);
    CHECK(sd_bit(0) && sd_bit(2) && !sd_bit(1), "plugging a unit back raises SD again");

    /* ---------- T15: instances are independent ---------- */
    lh_init(2);
    lh_write(0, 0x05000000, 2, 0x7C00);
    lh_write(0, 0x04000200, 2, 0x0001);
    CHECK(lh_read(0, 0x05000000, 2) == 0x7C00 && lh_read(1, 0x05000000, 2) == 0, "palette write on unit 0 is invisible on unit 1");
    CHECK(lh_read(1, 0x04000200, 2) == 0, "IE of unit 1 untouched");
    lh_step(1000);
    lh_write(1, 0x04000004, 2, 0x0008);
    CHECK(lh_read(0, 0x04000004, 2) != lh_read(1, 0x04000004, 2), "DISPSTAT is per unit");

    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    return fails != 0;
}
