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


    /* ---------- T16: RCNT resets to 0x8000; multi-play only works after the game writes RCNT = 0 ---------- */
    lh_init(2);
    CHECK(rd(0, 0x04000134) == 0x8000 && rd(1, 0x04000134) == 0x8000, "RCNT resets to 0x8000 (general-purpose mode, as left by the BIOS)");
    lh_write(0, SIOCNT, 2, 0x2003); lh_write(1, SIOCNT, 2, 0x2003);
    lh_master_start();
    CHECK(!lh_active() && !busy(1), "with RCNT=0x8000 the SIOCNT multi-play bits do nothing (no transfer)");
    lh_write(0, 0x04000134, 2, 0x0000); lh_write(1, 0x04000134, 2, 0x0000);
    lh_write(0, SIOCNT, 2, 0x2003); lh_write(1, SIOCNT, 2, 0x2003);
    lh_master_start();
    CHECK(lh_active(), "after RCNT = 0 the same SIOCNT write starts a real transfer");
    lh_step(2 * T);
    lh_write(0, 0x04000134, 2, 0x8000);
    CHECK(rd(0, 0x04000134) == 0x8000, "RCNT is a plain 16-bit register");

    /* ---------- T17: 8 / 16 / 32-bit access to SIOCNT, SIOMLT_SEND and SIOMULTI ---------- */
    setup(2, 0);
    lh_set_send(0, 0xBEEF);
    uint32_t w32 = lh_read(0, SIOCNT, 4);
    CHECK((w32 & 0xFFFF) == rd(0, SIOCNT) && (w32 >> 16) == 0xBEEF, "ldr [SIOCNT] = SIOCNT | SIOMLT_SEND << 16");
    CHECK(lh_read(0, SIOCNT, 1) == (rd(0, SIOCNT) & 0xFFu) && lh_read(0, SIOCNT + 1, 1) == (unsigned)(rd(0, SIOCNT) >> 8),
          "byte reads of SIOCNT and SIOCNT+1");
    lh_write(0, 0x0400012A, 1, 0x34);
    lh_write(0, 0x0400012B, 1, 0x12);
    CHECK(rd(0, 0x0400012A) == 0x1234, "SIOMLT_SEND written as two bytes");
    lh_write(0, 0x0400012A, 2, 0xCAFE);
    CHECK(rd(0, 0x0400012A) == 0xCAFE && lh_read(0, 0x0400012A, 1) == 0xFE, "SIOMLT_SEND 16-bit write, byte read");
    {   /* strb of (old | 0x80) on the low byte starts the transfer, like LinkMain */
        uint32_t lo = lh_read(0, SIOCNT, 1);
        lh_write(0, SIOCNT, 1, lo | 0x80);
        CHECK(lh_active() && busy(0), "byte write of SIOCNT with bit 7 starts the transfer");
        CHECK((rd(0, SIOCNT) & 0x7000) == 0x2000, "...and the mode bits in the high byte are untouched");
        lh_step(2 * T);
        CHECK(!busy(0) && rd(0, MULTI(0)) == 0xCAFE, "byte-started transfer delivered the send value");
    }
    {   /* str (send << 16 | SIOCNT | 0x80): the start bit must see the NEW send value */
        lh_set_send(1, 0x7001);
        uint32_t cnt = rd(0, SIOCNT) & 0x7003;
        lh_write(0, SIOCNT, 4, (0x1234u << 16) | cnt | 0x80);
        lh_step(2 * T);
        CHECK(rd(0, MULTI(0)) == 0x1234 && rd(0, MULTI(1)) == 0x7001, "32-bit write of SIOCNT|SEND<<16 starts with the new send value");
    }
    CHECK(lh_read(1, 0x04000120, 4) == (0x1234u | (0x7001u << 16)), "SIOMULTI0|SIOMULTI1<<16 read as one 32-bit word (ldm pairs)");
    CHECK(lh_read(1, 0x04000124, 4) == 0xFFFFFFFFu, "SIOMULTI2|SIOMULTI3<<16 = 0xFFFFFFFF with 2 units");
    CHECK(lh_read(1, 0x04000121, 1) == 0x12 && lh_read(1, 0x04000122, 1) == 0x01 && lh_read(1, 0x04000123, 1) == 0x70, "byte reads of SIOMULTI");
    lh_write(1, 0x04000120, 4, 0xAAAAAAAAu);
    CHECK(lh_read(1, 0x04000120, 4) == (0x1234u | (0x7001u << 16)), "32-bit write to SIOMULTI is ignored in multi-play");
    {   /* str 0x2000: SIOCNT rewritten as one 32-bit word (mode multi, baud 0) and SIOMLT_SEND cleared */
        lh_write(0, SIOCNT, 4, 0x00002000u);
        CHECK((rd(0, SIOCNT) & 0x7003) == 0x2000 && rd(0, 0x0400012A) == 0, "str 0x2000 sets mode multi / baud 0 and clears SIOMLT_SEND");
    }
    setup(2, 1);                                              /* high byte write: clear the IRQ-enable bit only */
    {
        lh_write(0, SIOCNT + 1, 1, 0x20);                    /* bit 13 stays, bit 14 (IRQ) cleared on the master */
        CHECK((rd(0, SIOCNT) & 0x7000) == 0x2000, "byte write of SIOCNT+1 clears the IRQ-enable bit");
        lh_master_start();
        lh_step(2 * T);
        CHECK(!serial_irq(0) && serial_irq(1), "master without its IRQ bit gets none; the child still does");
    }

    /* ---------- T18: Timer3 as the game's master loop uses it ---------- */
    {
        lh_init(2);
        lh_write(0, 0x04000200, 2, 0x0040);                  /* IE: timer 3 */
        lh_write(0, 0x04000208, 2, 1);
        lh_write(0, 0x0400010C, 4, 0x00C0BBBCu);             /* TM3CNT_L = 0xBBBC (reload), TM3CNT_H = enable + IRQ, F/1 */
        CHECK(lh_read(0, 0x0400010E, 2) == 0x00C0, "TM3CNT_H = 0x00C0 stored (prescaler F/1, IRQ, enable)");
        CHECK(lh_read(0, 0x0400010C, 2) == 0xBBBC, "the counter loads the reload value when enabled");
        lh_step(100);
        CHECK(lh_read(0, 0x0400010C, 2) == 0xBBBC + 100, "counts 1 per CPU cycle (F/1)");
        lh_step(17476 - 100 - 1);
        CHECK((lh_irq_pending(0) & 0x40) == 0 && (lh_read(0, IFREG, 2) & 0x40) == 0, "no overflow 1 cycle early");
        lh_step(1);
        CHECK((lh_read(0, IFREG, 2) & 0x40) != 0 && (lh_irq_pending(0) & 0x40) != 0, "overflow after exactly 17476 cycles raises IF bit 6");
        CHECK(lh_read(0, 0x0400010C, 2) == 0xBBBC, "the counter reloads 0xBBBC on overflow");
        lh_write(0, IFREG, 2, 0x40);
        lh_step(17475);
        CHECK((lh_irq_pending(0) & 0x40) == 0, "period is 17476 cycles (second overflow not yet)");
        lh_step(1);
        CHECK((lh_irq_pending(0) & 0x40) != 0, "second overflow exactly one period later");
        lh_write(0, IFREG, 2, 0x40);
        /* rewriting TM3CNT_H while running does not restart (hardware: only a 0 -> 1 edge reloads) */
        lh_step(5000);
        lh_write(0, 0x0400010E, 2, 0x00C0);
        lh_step(17476 - 5000 - 1);
        CHECK((lh_irq_pending(0) & 0x40) == 0, "rewriting the same TM3CNT_H does not restart the timer");
        lh_step(1);
        CHECK((lh_irq_pending(0) & 0x40) != 0, "...it overflows on the original schedule");
        lh_write(0, IFREG, 2, 0x40);
        /* stopping and writing the 32-bit word again DOES restart (what the game does when re-arming) */
        lh_step(3000);
        lh_write(0, 0x0400010C, 4, 0x00000000u);             /* stop (L = 0, H = 0) */
        lh_step(20000);
        CHECK((lh_irq_pending(0) & 0x40) == 0, "a stopped timer never overflows");
        lh_write(0, 0x0400010C, 4, 0x00C0BBBCu);
        lh_step(17475);
        CHECK((lh_irq_pending(0) & 0x40) == 0, "after stop + 32-bit rewrite the period starts again");
        lh_step(1);
        CHECK((lh_irq_pending(0) & 0x40) != 0, "...and ends exactly 17476 cycles after the rewrite");
        lh_write(0, 0x04000200, 2, 0x0000);
        CHECK(lh_irq_pending(0) == 0, "IE bit 6 gates the CPU interrupt (IF stays set)");
    }

    /* ---------- T19: 4 units x 18 bits fit inside one Timer3 period; the handler reads the RIGHT exchange ---------- */
    {
        lh_init(4);
        for (int k = 0; k < 4; k++) lh_enter_multi(k, 1);
        const uint32_t four = 4 * lh_slot_cycles();
        CHECK(four == 4 * 2610u && four < 17476u, "4 slots x 18 bits at 115200 baud = 10440 cycles < 17476 (Timer3 period)");
        lh_write(0, 0x04000200, 2, 0x00C0);                  /* IE: timer 3 + serial */
        lh_write(0, 0x0400010C, 4, 0x00C0BBBCu);             /* the master's Timer3 drives the exchanges */
        for (int r = 0; r < 6; r++) {
            lh_set_send(0, (uint16_t)(0x1000 + r)); lh_set_send(1, (uint16_t)(0x2000 + r));
            lh_set_send(2, (uint16_t)(0x3000 + r)); lh_set_send(3, (uint16_t)(0x4000 + r));
            lh_master_start();                               /* the handler starts exchange r right after the IRQ */
            lh_step(four - 1);
            CHECK(busy(0) && lh_transfers() == r, "exchange still running 1 cycle before its end");
            lh_step(1);
            CHECK(!busy(0) && lh_transfers() == r + 1, "exchange r is complete after 4 slots");
            CHECK((lh_read(0, IFREG, 2) & 0x40) == 0, "...well before the Timer3 overflow");
            lh_step(17476 - four);                           /* now the Timer3 overflow */
            CHECK((lh_read(0, IFREG, 2) & 0x40) != 0, "Timer3 IRQ (IF bit 6) at the end of the period");
            uint32_t m01 = lh_read(0, 0x04000120, 4), m23 = lh_read(0, 0x04000124, 4);   /* the handler's ldm */
            CHECK(m01 == (uint32_t)((0x1000 + r) | ((0x2000 + r) << 16)) && m23 == (uint32_t)((0x3000 + r) | ((0x4000 + r) << 16)),
                  "the Timer3 handler reads the values of the exchange that just finished, not the previous one");
            if (r > 0) CHECK(m01 != (uint32_t)((0x1000 + r - 1) | ((0x2000 + r - 1) << 16)), "...and they differ from the previous exchange");
            lh_write(0, IFREG, 2, 0xC0);                     /* ack timer + serial */
            for (int k = 1; k < 4; k++) lh_write(k, IFREG, 2, 0x80);
        }
        CHECK(lh_transfers() == 6, "six exchanges, one per Timer3 period");
    }
    {   /* too slow: at 9600 or 57600 baud the exchange has NOT finished when Timer3 overflows */
        static const struct { uint16_t cnt; const char *name; } slow[] = { { 0x2000, "9600 baud: 4 slots take longer than the Timer3 period" },
                                                                          { 0x2002, "57600 baud: 4 slots take longer than the Timer3 period" } };
        for (unsigned q = 0; q < 2; q++) {
            lh_init(4);
            for (int k = 0; k < 4; k++) { lh_enter_multi(k, 1); lh_write(k, SIOCNT, 2, slow[q].cnt | 0x4000); }
            const uint32_t t4 = 4 * lh_slot_cycles();
            lh_write(0, 0x0400010C, 4, 0x00C0BBBCu);
            lh_set_send(0, 0x1111);
            lh_master_start();
            lh_step(17476);
            CHECK(t4 > 17476u && busy(0) && lh_transfers() == 0, slow[q].name);
            CHECK(rd(0, MULTI(0)) == 0xFFFF, "...the handler would still read 0xFFFF / the previous exchange (why 115200 is required)");
        }
    }

    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    return fails != 0;
}
