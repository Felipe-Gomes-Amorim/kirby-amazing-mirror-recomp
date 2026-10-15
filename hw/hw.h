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

#endif
