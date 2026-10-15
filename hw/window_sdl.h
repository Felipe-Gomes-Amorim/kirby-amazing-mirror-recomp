/* window_sdl.h - janela e teclado via SDL2 para o hw/ (opcional; so este arquivo depende de SDL).
 * Uso tipico no laco principal (por VBlank):
 *     win_open("Kirby", 3);
 *     while (win_poll()) {          // trata eventos; 0 = usuario fechou
 *         hw_set_keys(win_keys());  // KEYINPUT: bit=0 pressionado
 *         ... roda ate hw_frame_ready() != NULL ...
 *         win_present(fb);          // fb = hw_frame_ready() ou hw_get_framebuffer()
 *     }
 *     win_close();
 * Teclas: Z=A, X=B, Backspace=Select, Enter=Start, setas, A=L, S=R. */
#ifndef WINDOW_SDL_H
#define WINDOW_SDL_H
#include <stdint.h>

int win_open(const char *title, int scale);   /* 0 = ok, <0 = erro (mensagem em stderr); sem vsync */
int win_open_ex(const char *title, int scale, int vsync);  /* vsync=1 (ou env HW_WIN_VSYNC=1): PRESENTVSYNC */
int win_poll(void);                           /* 1 = continua, 0 = fechar; trata minimizar/restaurar sem bloquear */
int win_visible(void);                        /* 0 = minimizada/oculta (win_present nao faz nada) */
void win_test_minimize(int on);               /* so para teste/automacao: minimiza (1) ou restaura (0) */
uint16_t win_keys(void);                      /* formato do KEYINPUT (ativo em 0) */
void win_present(const uint32_t *xrgb8888_240x160);
void win_close(void);

#endif
