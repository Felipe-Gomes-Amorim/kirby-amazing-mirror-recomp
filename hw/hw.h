/* hw.h - modelo de hardware GBA (Gwen). Mesmas assinaturas de recomp/runtime/gba_bus.h
 * (contrato do Nasus); declaradas aqui para hw/ compilar sem depender de recomp/.
 * hw/ chama de volta gba_read e gba_write (8/16/32) para o DMA (quem fornece e o runtime ou o teste). */
#ifndef HW_H
#define HW_H
#include <stdint.h>

/* Several independent hardware instances can live in one program by compiling hw.c once per instance with
 * -DHW_PREFIX=i0_ (i1_, ...): every public symbol below, including the gba_read/gba_write callbacks, gets that
 * prefix (used by the cable-link test harness). Normal builds do not define HW_PREFIX. */
#ifdef HW_PREFIX
#define HW_CAT_(a, b) a##b
#define HW_CAT(a, b) HW_CAT_(a, b)
#define HW_NAME(n) HW_CAT(HW_PREFIX, n)
#define gba_read8 HW_NAME(gba_read8)
#define gba_read16 HW_NAME(gba_read16)
#define gba_read32 HW_NAME(gba_read32)
#define gba_write8 HW_NAME(gba_write8)
#define gba_write16 HW_NAME(gba_write16)
#define gba_write32 HW_NAME(gba_write32)
#define hw_read HW_NAME(hw_read)
#define hw_write HW_NAME(hw_write)
#define hw_reset HW_NAME(hw_reset)
#define hw_step HW_NAME(hw_step)
#define hw_step_irq HW_NAME(hw_step_irq)
#define hw_irq_pending HW_NAME(hw_irq_pending)
#define hw_set_keys HW_NAME(hw_set_keys)
#define hw_frame_ready HW_NAME(hw_frame_ready)
#define hw_get_framebuffer HW_NAME(hw_get_framebuffer)
#define hw_audio_read HW_NAME(hw_audio_read)
#define hw_audio_rate HW_NAME(hw_audio_rate)
#define hw_sio_set_link HW_NAME(hw_sio_set_link)
#define hw_sio_set_slot HW_NAME(hw_sio_set_slot)
#define hw_sio_slot HW_NAME(hw_sio_slot)
#define hw_sio_set_sd HW_NAME(hw_sio_set_sd)
#define hw_sio_multi_mode HW_NAME(hw_sio_multi_mode)
#define hw_sio_send_value HW_NAME(hw_sio_send_value)
#define hw_sio_busy HW_NAME(hw_sio_busy)
#define hw_sio_slot_cycles HW_NAME(hw_sio_slot_cycles)
#define hw_sio_begin HW_NAME(hw_sio_begin)
#define hw_sio_deliver HW_NAME(hw_sio_deliver)
#endif

uint32_t gba_read8(uint32_t addr);
uint32_t gba_read16(uint32_t addr);
uint32_t gba_read32(uint32_t addr);
void gba_write8(uint32_t addr, uint32_t v);
void gba_write16(uint32_t addr, uint32_t v);
void gba_write32(uint32_t addr, uint32_t v);

uint32_t hw_read(uint32_t addr, int size);
void hw_write(uint32_t addr, int size, uint32_t v);
void hw_reset(void);
void hw_step(uint32_t cycles);
uint16_t hw_irq_pending(void);
void hw_set_keys(uint16_t keyinput);
const uint32_t *hw_frame_ready(void);
/* Como hw_step, mas PARA no instante em que uma interrupcao habilitada (IME&IE&IF) passa a estar
 * pendente e devolve quantos ciclos consumiu (<= cycles). O chamador entrega o IRQ e depois continua
 * com o que sobrou (cycles - retorno). Evita latencia de IRQ de ate `cycles` ciclos quando o
 * runtime avanca o hw em saltos grandes (ver NOTES.md 'Estalo no som'). */
uint32_t hw_step_irq(uint32_t cycles);
/* framebuffer 240x160 XRGB8888 (sempre valido; linhas ja renderizadas do quadro corrente) */
const uint32_t *hw_get_framebuffer(void);
/* Audio PCM (Direct Sound A/B). Saida estereo int16 intercalada (L,R) a hw_audio_rate() = 48000 Hz,
 * gerada dentro de hw_step (retencao de ordem zero da amostra atual de cada FIFO; PSG ainda nao).
 * Retorna quantos quadros (pares L,R) foram copiados (<= frames); o resto do buffer nao e tocado.
 * Buffer interno de 16384 quadros: se ninguem le, os mais antigos sao descartados. */
int hw_audio_read(int16_t *lr, int frames);
int hw_audio_rate(void);
/* ---- SIO multi-play (cable link). One instance = one GBA; the cable model lives outside (hw/link_harness.c
 * for the tests, the network layer later). Multi-play = SIOCNT bits 12-13 == 2 and RCNT bit 15 == 0.
 * The master (slot 0) starts a transfer by writing SIOCNT bit 7; hw calls the start hook with its SIOMLT_SEND.
 * The cable then collects every unit's SIOMLT_SEND in order (master, child 1, 2, 3) and gives the four values
 * back with hw_sio_deliver() on every unit, which clears the busy bit, sets the ID/error bits and raises the
 * serial IRQ (IF bit 7) if SIOCNT bit 14 is set. With no link attached (a console with no cable) hw completes
 * a master transfer by itself after one slot time: SIOMULTI0 = own data, 1..3 = 0xFFFF, error bit set. */
void hw_sio_set_link(void *ctx, void (*start)(void *ctx, uint16_t master_send));   /* start == NULL detaches */
void hw_sio_set_slot(int slot);                 /* 0 = master (parent), 1..3 = child number (cable position) */
int hw_sio_slot(void);
void hw_sio_set_sd(int sd);                     /* SIOCNT bit 3: all connected units ready (cable decides) */
int hw_sio_multi_mode(void);                    /* 1 while the unit is configured for multi-play */
uint16_t hw_sio_send_value(void);               /* SIOMLT_SEND as the unit would transmit it right now */
int hw_sio_busy(void);                          /* SIOCNT bit 7 */
uint32_t hw_sio_slot_cycles(void);              /* CPU cycles one unit needs to send its 16 bits (from the baud bits) */
void hw_sio_begin(void);                        /* a transfer started elsewhere: set busy, SIOMULTI0-3 = 0xFFFF */
void hw_sio_deliver(const uint16_t multi[4], int error);   /* transfer finished (see above) */

#define HW_FB_W 240
#define HW_FB_H 160

#endif
