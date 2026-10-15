/* hw.h - modelo de hardware GBA (Gwen). Mesmas assinaturas de recomp/runtime/gba_bus.h
 * (contrato do Nasus); declaradas aqui para hw/ compilar sem depender de recomp/.
 * hw/ chama de volta gba_read e gba_write (8/16/32) para o DMA (quem fornece e o runtime ou o teste). */
#ifndef HW_H
#define HW_H
#include <stdint.h>

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
#define HW_FB_W 240
#define HW_FB_H 160

#endif
