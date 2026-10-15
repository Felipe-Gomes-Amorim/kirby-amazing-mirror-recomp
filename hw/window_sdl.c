/* window_sdl.c - ver window_sdl.h. Requer SDL2 (nao instalado/compilado ainda nesta maquina). */
#include <stdio.h>
#include <SDL.h>
#include "window_sdl.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;

int win_open(const char *title, int scale) {
    if (scale < 1) scale = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * scale, 160 * scale, 0);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -2; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return -3; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, 240, 160);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 240, 160);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return -4; }
    return 0;
}

int win_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return 0;
    }
    return 1;
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
    SDL_UpdateTexture(tex, NULL, fb, 240 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void win_close(void) {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = 0; ren = 0; win = 0;
    SDL_Quit();
}
