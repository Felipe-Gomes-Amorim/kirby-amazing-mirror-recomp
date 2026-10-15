/* window_sdl.h - janela e teclado via SDL2 para o hw/ (opcional; so este arquivo depende de SDL).
 * Uso tipico no laco principal (por VBlank):
 *     win_open(NULL, 3);            // NULL = titulo padrao WIN_TITLE
 *     while (win_poll()) {          // trata eventos; 0 = usuario fechou
 *         const char *rom = win_dropped_path();   // arquivo solto na janela (ou NULL)
 *         hw_set_keys(win_keys());  // KEYINPUT: bit=0 pressionado
 *         ... roda ate hw_frame_ready() != NULL ...
 *         win_present(fb);          // fb = hw_frame_ready() ou hw_get_framebuffer()
 *     }
 *     win_close();
 * Teclas: Z=A, X=B, Backspace=Select, Enter=Start, setas, A=L, S=R, F11=tela cheia, Esc=sair.
 *
 * Tela de progresso (launcher, sem console):
 *     win_open_progress("Kirby Recomp");
 *     win_progress(30, "Compilando 4/16...");     // devolve 0 se o usuario fechou a janela
 *     win_close_progress();
 */
#ifndef WINDOW_SDL_H
#define WINDOW_SDL_H
#include <stdint.h>

#define WIN_TITLE "Kirby & The Amazing Mirror - Recomp"

int win_open(const char *title, int scale);   /* 0 = ok, <0 = erro (mensagem em stderr); sem vsync; title NULL = WIN_TITLE */
int win_open_ex(const char *title, int scale, int vsync);  /* vsync=1 (ou env HW_WIN_VSYNC=1): PRESENTVSYNC */
int win_poll(void);                           /* 1 = continua, 0 = fechar; trata minimizar/restaurar/soltar/F11 sem bloquear */
int win_visible(void);                        /* 0 = minimizada/oculta (win_present nao faz nada) */
void win_test_minimize(int on);               /* so para teste/automacao: minimiza (1) ou restaura (0) */
uint16_t win_keys(void);                      /* formato do KEYINPUT (ativo em 0) */
void win_present(const uint32_t *xrgb8888_240x160);
void win_close(void);

/* arquivo solto na janela (SDL_DROPFILE): devolve o caminho UMA vez (depois NULL ate o proximo drop).
 * O ponteiro vale ate o proximo drop. */
const char *win_dropped_path(void);

/* mode: 0 integer (whole-number zoom, black bars), 1 fit (aspect preserved; default), 2 stretch (fills the
 * window) - the CFG_SCALING_* values of config_ini.h. Can be called before or after win_open. */
void win_set_scaling(int mode);
int win_get_scaling(void);

void win_set_fullscreen(int on);              /* tela cheia "desktop" (sem mudar a resolucao) */
int win_is_fullscreen(void);
const char *win_get_title(void);              /* titulo atual da janela do jogo */

/* ---- tela de progresso (janela propria, 480x140, barra + texto com fonte 5x7 embutida) ---- */
int win_open_progress(const char *title);     /* 0 = ok */
int win_progress(int percent, const char *text);   /* 0..100; 1 = continua, 0 = o usuario fechou */
void win_close_progress(void);

/* 5x7 bitmap font: 7 row bytes (bit 4 = leftmost column); unknown characters give the '?' glyph */
const uint8_t *win_font_rows(int ch);

/* ---- ganchos de teste (leitura de pixels sem tela) ---- */
int win_icon_pixels(uint32_t *out32x32);      /* ARGB do icone embutido (1024 valores) */
int win_progress_read(uint32_t *out, int cap, int *w, int *h);   /* ARGB da janela de progresso */

#endif
