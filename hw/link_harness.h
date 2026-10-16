/* link_harness.h - test harness for the GBA link cable (SIO multi-play): 2 to 4 independent hw instances
 * (no game) plus a model of the cable that runs master/child transfers between them.
 *
 * Each instance is a separate compilation of hw.c (-DHW_PREFIX=i0_ ... i3_), so they share no state.
 * Slot 0 is the master (parent), slots 1-3 are children, as in a real cable.
 *
 * Cable model (documented assumptions, to be confirmed against the game in P0):
 *   - participants = units that are plugged in AND configured for multi-play when the master starts;
 *   - a transfer takes (highest participating slot + 1) * slot_cycles (18 bit times at the master's baud rate);
 *     slot k puts its SIOMLT_SEND on the wire at start + k*slot_cycles (so the value is sampled at its turn);
 *   - absent slots read 0xFFFF; every participant gets the same four values, the ID bits (0 master, 1-3 child),
 *     the busy bit cleared and the serial IRQ (IF bit 7) if its SIOCNT bit 14 is set;
 *   - error bit: set when a plugged-in unit unplugs before its turn, when the master unplugs mid-transfer, and
 *     when the master transmits with nobody else connected;
 *   - SD terminal (SIOCNT bit 3) = at least two plugged-in units are in multi-play mode.
 */
#ifndef LINK_HARNESS_H
#define LINK_HARNESS_H
#include <stdint.h>

#define LH_MAX 4

void lh_init(int n);                                  /* 2..4 instances, all plugged in, slot i = instance i */
void lh_connect(int slot, int plugged);               /* plug / unplug the cable of one unit (also mid-transfer) */
int lh_connected(int slot);
void lh_step(uint32_t cycles);                        /* advance every instance and the cable by `cycles` */
uint32_t lh_read(int slot, uint32_t addr, int size);  /* bus access to one instance (like hw_read/hw_write) */
void lh_write(int slot, uint32_t addr, int size, uint32_t v);
uint16_t lh_irq_pending(int slot);                    /* hw_irq_pending() of one instance */
uint32_t lh_slot_cycles(void);                        /* cycles one slot takes at the master's current baud rate */

/* convenience: put a unit in multi-play (SIOCNT mode 2, baud 115200, IRQ on) with its IE/IME ready for the serial IRQ */
void lh_enter_multi(int slot, int irq);
void lh_detach(int slot);                             /* remove the cable model from one unit (hw then completes a master transfer alone) */
void lh_set_send(int slot, uint16_t v);               /* SIOMLT_SEND */
void lh_master_start(void);                           /* the master writes the start bit */

/* observation of the last/current transfer */
int lh_transfers(void);                               /* completed (or aborted) transfers so far */
int lh_active(void);                                  /* a transfer is in progress */
int lh_last_order(int *slots, int cap);               /* slots in the order they put data on the wire */
uint16_t lh_last_value(int slot);                     /* the value delivered for `slot` in the last transfer */
int lh_last_error(void);                              /* error flag delivered in the last transfer */

#endif
