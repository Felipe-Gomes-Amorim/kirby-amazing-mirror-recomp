/* gba_bus.h - CONTRATO de memoria/I-O entre o runtime (recomp/, Nasus) e o
 * modelo de hardware (hw/, Gwen). So declaracoes; implementacao vem depois.
 *
 * Enderecos: sempre o endereco GUEST de 32 bits do GBA. Valores: uint32_t, so os
 * N bits baixos sao validos (read8 -> 0..255, read16 -> 0..65535). Little-endian.
 *
 * ---- Quem e dono do que ----
 *  runtime (recomp/runtime/gba_bus.c):
 *     0x02 EWRAM 256K (espelha a cada 0x40000 em 0x02xxxxxx)
 *     0x03 IWRAM  32K (espelha a cada 0x8000 em 0x03xxxxxx)
 *     0x08-0x0D ROM (imagem carregada pelo runtime; espelhos WS0/1/2; leitura alem
 *               do tamanho = open bus, retorna (addr>>1)&0xFFFF como o hw real)
 *     0x0E-0x0F SRAM 64K (so 8 bits; espelhada; arquivo de save)
 *     0x00 BIOS: nao mapeada (HLE das SWI); leitura retorna 0
 *  hw (hw/, implementado por Gwen):
 *     0x04 I/O (0x04000000-0x040003FF + espelho de 0x04000800), 0x05 paleta 1K,
 *     0x06 VRAM 96K (espelho 0x06018000 -> 0x06010000; regiao repete a cada 128K),
 *     0x07 OAM 1K. Espelhos dessas regioes e quirks de escrita de byte ficam em hw/.
 *
 * ---- Semantica comum (ARM7TDMI) ----
 *  - O barramento NAO rotaciona nem estende sinal: isso e da CPU (rc_ldr32, LDRSH...).
 *  - Acesso desalinhado: read16/32 e write16/32 ignoram os bits baixos
 *    (addr & ~1 / addr & ~3). A CPU decide a rotacao do LDR e passa o valor cru.
 *  - Endereco sem regiao (ex.: 0x01xxxxxx, 0x10000000+): leitura = 0, escrita ignorada,
 *    e chama gba_bus_unmapped() (fraco; padrao: registra 1x e segue). Nunca aborta.
 *  - Escrita em ROM: ignorada. Leitura de registrador write-only de I/O: valor
 *    definido por hw/ (normalmente 0).
 *  - Escritas de byte: paleta/VRAM-BG replicam o byte nas duas metades do halfword;
 *    OAM ignora; I/O trata como RMW do halfword. (tudo dentro de hw/).
 *  - Efeitos colaterais (DMA, timers, IF/IE, DISPSTAT) acontecem dentro de
 *    hw_write; o runtime nao conhece registradores de I/O.
 *
 * ---- Chamadas hw -> runtime ----
 *  DMA le/escreve a memoria do jogo pelas mesmas funcoes gba_read/gba_write (nao acessa
 *  arrays diretamente). IRQ: hw mantem IE/IF/IME; o runtime consulta
 *  hw_irq_pending() em pontos seguros (limites de funcao/loop, SWI Halt).
 *
 * ---- Chamadas runtime -> hw (Gwen implementa) ----
 */
#ifndef GBA_BUS_H
#define GBA_BUS_H
#include <stdint.h>

/* --- API que o codigo gerado/DMA usa (runtime implementa o despacho) --- */
uint32_t gba_read8(uint32_t addr);
uint32_t gba_read16(uint32_t addr);
uint32_t gba_read32(uint32_t addr);
void gba_write8(uint32_t addr, uint32_t v);
void gba_write16(uint32_t addr, uint32_t v);
void gba_write32(uint32_t addr, uint32_t v);

/* --- Regioes 0x04-0x07: implementadas por hw/ (size = 1, 2 ou 4) --- */
uint32_t hw_read(uint32_t addr, int size);
void hw_write(uint32_t addr, int size, uint32_t v);

/* --- Runtime: imagem da ROM e acesso direto a RAM (so runtime/testes) --- */
void gba_load_rom(const uint8_t *p, uint32_t n);
uint32_t gba_rom_size(void);
uint8_t *gba_ewram(void);
uint8_t *gba_iwram(void);
/* save SRAM de 32 KiB (espelhada em 0x0E000000..0x0FFFFFFF) */
void gba_sram_set(const uint8_t *p32k);
void gba_sram_get(uint8_t *p32k);
const uint32_t *hw_get_framebuffer(void);
int hw_audio_read(int16_t *lr, int frames);
int hw_audio_rate(void);

/* --- Ciclo de vida / tempo --- */
void hw_reset(void);
/* avanca o hardware `cycles` ciclos de CPU (PPU/timers/DMA); o runtime chama por
 * funcao/bloco com uma estimativa, ou 1 frame por vez no inicio. */
void hw_step(uint32_t cycles);
/* bits de IE&IF pendentes com IME ligado; 0 = nada */
uint16_t hw_irq_pending(void);
/* teclas: bit=1 solto/0 pressionado como KEYINPUT (ativo em baixo) */
void hw_set_keys(uint16_t keyinput);
/* ponteiro p/ framebuffer 240x160 XRGB8888 quando um frame fecha, senao NULL */
const uint32_t *hw_frame_ready(void);

/* --- Gancho opcional (definicao fraca no runtime) --- */
void gba_bus_unmapped(uint32_t addr, int size, int is_write);

#endif
