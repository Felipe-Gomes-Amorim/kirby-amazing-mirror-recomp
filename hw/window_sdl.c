/* window_sdl.c - ver window_sdl.h. Requer SDL2. */
#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include "window_sdl.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int hidden;      /* 1 = minimizada ou oculta: nao apresenta */

static int env_vsync(void) {
    const char *e = getenv("HW_WIN_VSYNC");
    return e && *e && *e != '0';
}

int win_open_ex(const char *title, int scale, int vsync) {
    if (scale < 1) scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * scale, 160 * scale, 0);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -2; }
    /* sem PRESENTVSYNC por padrao: o relogio do audio paceia o jogo, e um present com vsync bloqueia
     * (ou trava) com a janela minimizada/oculta. Use vsync=1 ou HW_WIN_VSYNC=1 so se quiser. */
    Uint32 f = SDL_RENDERER_ACCELERATED | ((vsync || env_vsync()) ? SDL_RENDERER_PRESENTVSYNC : 0);
    ren = SDL_CreateRenderer(win, -1, f);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return -3; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, 240, 160);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return -4; }
    hidden = 0;
    return 0;
}

int win_open(const char *title, int scale) { return win_open_ex(title, scale, 0); }

int win_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return 0;
        if (e.type == SDL_WINDOWEVENT) {
            switch (e.window.event) {
            case SDL_WINDOWEVENT_MINIMIZED:
            case SDL_WINDOWEVENT_HIDDEN:
                hidden = 1; break;
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_EXPOSED:
                hidden = win && (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) ? 1 : 0;
                break;
            default: break;
            }
        }
    }
    return 1;
}

int win_visible(void) {
    if (!win) return 0;
    if (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) return 0;
    return !hidden;
}

uint16_t win_keys(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    static const struct { SDL_Scancode sc; int bit; } map[] = {
        { SDL_SCANCODE_Z, 0 }, { SDL_SCANCODE_X, 1 }, { SDL_SCANCODE_BACKSPACE, 2 },
        { SDL_SCANCODE_RETURN, 3 }, { SDL_SCANCODE_RIGHT, 4 }, { SDL_SCANCODE_LEFT, 5 },
        { SDL_SCANCODE_UP, 6 }, { SDL_SCANCODE_DOWN, 7 }, { SDL_SCANCODE_S, 8 }, { SDL_SCANCODE_A, 9 },
    };
    uint16_t v = 0x03FF;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (k[map[i].sc]) v &= (uint16_t)~(1u << map[i].bit);
    return v;
}

void win_present(const uint32_t *fb) {
    if (!tex || !fb) return;
    if (!win_visible()) return;          /* minimizada/oculta: nao apresenta (o present poderia bloquear) */
    SDL_UpdateTexture(tex, NULL, fb, 240 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void win_test_minimize(int on) {         /* gancho de teste/automacao */
    if (!win) return;
    if (on) SDL_MinimizeWindow(win); else SDL_RestoreWindow(win);
}

void win_close(void) {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = 0; ren = 0; win = 0; hidden = 0;
    SDL_Quit();
}
