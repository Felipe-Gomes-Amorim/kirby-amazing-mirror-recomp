/* window_sdl.c - ver window_sdl.h. Requer SDL2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "window_sdl.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int hidden;            /* 1 = minimizada ou oculta: nao apresenta */
static char drop_path[1024];
static int drop_pending;
static SDL_Window *pwin;       /* tela de progresso */
static SDL_Renderer *pren;

static int env_vsync(void) {
    const char *e = getenv("HW_WIN_VSYNC");
    return e && *e && *e != '0';
}

/* ---------------------------------------------------------------- icone 32x32 (gerado em codigo) */
static int in_ellipse(double x, double y, double cx, double cy, double rx, double ry) {
    double dx = (x - cx) / rx, dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0;
}

static void make_icon(uint32_t *px) {      /* um "Kirby" rosa: corpo, pes vermelhos, olhos, bochechas */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            double fx = x + 0.5, fy = y + 0.5;
            uint32_t c = 0x00000000;                                      /* transparente */
            if (in_ellipse(fx, fy, 9.0, 27.0, 5.5, 3.5) || in_ellipse(fx, fy, 23.0, 27.0, 5.5, 3.5))
                c = in_ellipse(fx, fy, 9.0, 27.0, 4.3, 2.5) || in_ellipse(fx, fy, 23.0, 27.0, 4.3, 2.5)
                        ? 0xFFD02040u : 0xFF6A1020u;                       /* pes */
            if (in_ellipse(fx, fy, 16.0, 15.0, 13.5, 13.0))
                c = in_ellipse(fx, fy, 16.0, 15.0, 12.0, 11.5) ? 0xFFFF9FC8u : 0xFF7A2B50u;   /* corpo + contorno */
            if (in_ellipse(fx, fy, 12.5, 12.0, 1.8, 3.2) || in_ellipse(fx, fy, 19.5, 12.0, 1.8, 3.2))
                c = 0xFF1A1A3Au;                                            /* olhos */
            if (in_ellipse(fx, fy, 12.5, 10.3, 0.9, 1.2) || in_ellipse(fx, fy, 19.5, 10.3, 0.9, 1.2))
                c = 0xFFFFFFFFu;                                            /* brilho */
            if (in_ellipse(fx, fy, 7.5, 17.5, 2.4, 1.6) || in_ellipse(fx, fy, 24.5, 17.5, 2.4, 1.6))
                c = 0xFFFF5A8Au;                                            /* bochechas */
            if (in_ellipse(fx, fy, 16.0, 19.5, 1.8, 1.2))
                c = 0xFFB01840u;                                            /* boca */
            px[y * 32 + x] = c;
        }
}

int win_icon_pixels(uint32_t *out32x32) {
    if (!out32x32) return -1;
    make_icon(out32x32);
    return 0;
}

static void apply_icon(SDL_Window *w) {
    static uint32_t px[32 * 32];
    make_icon(px);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, 32, 32, 32, 32 * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SetWindowIcon(w, s); SDL_FreeSurface(s); }
}

/* ---------------------------------------------------------------- janela do jogo */
int win_open_ex(const char *title, int scale, int vsync) {
    if (scale < 1) scale = 1;
    if (!title || !*title) title = WIN_TITLE;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 240 * scale, 160 * scale, 0);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -2; }
    apply_icon(win);
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
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    hidden = 0; drop_pending = 0;
    return 0;
}

int win_open(const char *title, int scale) { return win_open_ex(title, scale, 0); }

void win_set_fullscreen(int on) {
    if (!win) return;
    SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

int win_is_fullscreen(void) {
    return win && (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) ? 1 : 0;
}

const char *win_get_title(void) { return win ? SDL_GetWindowTitle(win) : ""; }

int win_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE) return 0;
            if (e.key.keysym.sym == SDLK_F11 && !e.key.repeat) win_set_fullscreen(!win_is_fullscreen());
        }
        if (e.type == SDL_DROPFILE) {
            if (e.drop.file) {
                strncpy(drop_path, e.drop.file, sizeof drop_path - 1);
                drop_path[sizeof drop_path - 1] = 0;
                drop_pending = 1;
                SDL_free(e.drop.file);
            }
        }
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

const char *win_dropped_path(void) {
    if (!drop_pending) return NULL;
    drop_pending = 0;
    return drop_path;
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
    tex = 0; ren = 0; win = 0; hidden = 0; drop_pending = 0;
    if (pwin) return;                    /* a tela de progresso ainda usa o video */
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (SDL_WasInit(0) == 0) SDL_Quit();
}

/* ---------------------------------------------------------------- tela de progresso */
/* fonte 5x7: 7 linhas por glifo, 5 bits (bit 4 = coluna da esquerda). Minusculas usam as maiusculas. */
typedef struct { char ch; uint8_t row[7]; } Glyph;
static const Glyph glyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0, 0, 0, 0, 0, 0x0C, 0x0C}}, {',', {0, 0, 0, 0, 0x0C, 0x04, 0x08}},
    {':', {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0}}, {'-', {0, 0, 0, 0x1F, 0, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 0x1F}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04}}, {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
    {'\'', {0x04, 0x04, 0x08, 0, 0, 0, 0}}, {'+', {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0}},
    {'=', {0, 0, 0x1F, 0, 0x1F, 0, 0}}, {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
    {'<', {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01}},
};

static const uint8_t *glyph_lookup(int ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (unsigned i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++)
        if (glyphs[i].ch == ch) return glyphs[i].row;
    return NULL;
}
static const uint8_t *glyph_rows(int ch) {
    const uint8_t *r = glyph_lookup(ch);
    return r ? r : glyph_lookup('?');          /* desconhecido -> '?' */
}

#define PW 480
#define PH 140

static void draw_text(const char *s, int x, int y, int scale, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(pren, r, g, b, 255);
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *rows = glyph_rows((unsigned char)*s);
        if (!rows) continue;
        for (int ry = 0; ry < 7; ry++)
            for (int cx = 0; cx < 5; cx++)
                if (rows[ry] >> (4 - cx) & 1) {
                    SDL_Rect q = { x + cx * scale, y + ry * scale, scale, scale };
                    SDL_RenderFillRect(pren, &q);
                }
    }
}

static void draw_progress(int percent, const char *text) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    SDL_SetRenderDrawColor(pren, 24, 24, 40, 255);
    SDL_RenderClear(pren);
    draw_text(SDL_GetWindowTitle(pwin), 20, 16, 2, 255, 159, 200);
    draw_text(text ? text : "", 20, 52, 2, 230, 230, 240);
    SDL_Rect frame = { 20, 92, PW - 40, 24 };
    SDL_SetRenderDrawColor(pren, 200, 200, 220, 255);
    SDL_RenderDrawRect(pren, &frame);
    SDL_Rect fill = { 22, 94, (PW - 44) * percent / 100, 20 };
    SDL_SetRenderDrawColor(pren, 255, 120, 170, 255);
    if (fill.w > 0) SDL_RenderFillRect(pren, &fill);
    char pc[8];
    snprintf(pc, sizeof pc, "%d%%", percent);
    draw_text(pc, PW - 20 - 6 * 2 * (int)strlen(pc), 70, 2, 200, 200, 220);
    SDL_RenderPresent(pren);
}

int win_open_progress(const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    pwin = SDL_CreateWindow(title && *title ? title : WIN_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            PW, PH, 0);
    if (!pwin) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -2; }
    apply_icon(pwin);
    pren = SDL_CreateRenderer(pwin, -1, SDL_RENDERER_SOFTWARE);   /* sem vsync: nunca bloqueia */
    if (!pren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return -3; }
    draw_progress(0, "");
    return 0;
}

int win_progress(int percent, const char *text) {
    if (!pwin) return 0;
    SDL_Event e;
    int alive = 1;
    while (SDL_PollEvent(&e)) {                      /* mantem o SO feliz ("nao esta respondendo") */
        if (e.type == SDL_QUIT) alive = 0;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) alive = 0;
        if (e.type == SDL_DROPFILE && e.drop.file) {
            strncpy(drop_path, e.drop.file, sizeof drop_path - 1);
            drop_path[sizeof drop_path - 1] = 0;
            drop_pending = 1;
            SDL_free(e.drop.file);
        }
    }
    draw_progress(percent, text);
    return alive;
}

int win_progress_read(uint32_t *out, int cap, int *w, int *h) {
    if (!pren || cap < PW * PH) return -1;
    if (w) *w = PW;
    if (h) *h = PH;
    return SDL_RenderReadPixels(pren, NULL, SDL_PIXELFORMAT_ARGB8888, out, PW * 4);
}

void win_close_progress(void) {
    if (pren) SDL_DestroyRenderer(pren);
    if (pwin) SDL_DestroyWindow(pwin);
    pren = 0; pwin = 0;
    if (!win) {
        if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (SDL_WasInit(0) == 0) SDL_Quit();
    }
}
