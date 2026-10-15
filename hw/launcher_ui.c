/* launcher_ui.c - see launcher_ui.h. Retro-style immediate-mode UI on SDL2, English only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "launcher_ui.h"
#include "config_ini.h"
#include "window_sdl.h"      /* WIN_TITLE + win_icon_pixels */

/* ------------------------------------------------------------------ widgets */
typedef enum { W_BUTTON, W_RADIO, W_CHECK, W_SLIDER } WType;

typedef struct {
    const char *id;
    WType type;
    SDL_Rect r;
    char label[40];
    int enabled;
    int on;                 /* radio selected / checkbox checked */
    int primary;            /* highlighted button (Play) */
} Widget;

#define MAX_W 24
static Widget wd[MAX_W];
static int nw;

static SDL_Window *win;
static SDL_Renderer *ren;
static KirbyConfig cfg;
static KlStatus st;
static char ini[512];
static char dropped[1024];
static int poll_ms = 100;
static int focus;                    /* index into wd[] */
static int hover = -1;
static int dragging;                 /* slider drag */
static int page;                     /* 0 main, 1 keyboard info, 2 controller info */
static char note[64];
static Uint32 note_until;
static int result;
static int running;

static const SDL_Color C_BG = { 20, 20, 36, 255 }, C_PANEL = { 34, 34, 58, 255 }, C_TEXT = { 230, 230, 240, 255 },
                       C_ACC = { 255, 120, 170, 255 }, C_DIM = { 110, 110, 135, 255 },
                       C_FOCUS = { 255, 220, 80, 255 }, C_OK = { 120, 220, 150, 255 }, C_ERR = { 255, 100, 100, 255 };

/* ------------------------------------------------------------------ drawing */
static void setc(SDL_Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }
static void fill(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderFillRect(ren, &r); }
static void frame(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderDrawRect(ren, &r); }

static int text_w(const char *s, int scale) {
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void text(const char *s, int x, int y, int scale, SDL_Color c) {
    setc(c);
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *rows = win_font_rows((unsigned char)*s);
        for (int ry = 0; ry < 7; ry++)
            for (int cx = 0; cx < 5; cx++)
                if (rows[ry] >> (4 - cx) & 1) {
                    SDL_Rect q = { x + cx * scale, y + ry * scale, scale, scale };
                    SDL_RenderFillRect(ren, &q);
                }
    }
}

static void text_center(const char *s, SDL_Rect r, int scale, SDL_Color c) {
    text(s, r.x + (r.w - text_w(s, scale)) / 2, r.y + (r.h - 7 * scale) / 2, scale, c);
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* ------------------------------------------------------------------ layout */
static void add(const char *id, WType t, int x, int y, int w, int h, const char *label) {
    if (nw >= MAX_W) return;
    Widget *k = &wd[nw++];
    memset(k, 0, sizeof *k);
    k->id = id; k->type = t; k->r.x = x; k->r.y = y; k->r.w = w; k->r.h = h;
    k->enabled = 1;
    strncpy(k->label, label, sizeof k->label - 1);
}

static void layout(void) {
    static const char *const size_ids[6] = { "size1", "size2", "size3", "size4", "size5", "size6" };
    static const char *const size_lbl[6] = { "1X", "2X", "3X", "4X", "5X", "6X" };
    nw = 0;
    if (page) {                               /* info page: only a Back button */
        add("back", W_BUTTON, 235, 360, 170, 36, "BACK");
        wd[0].primary = 1;
        return;
    }
    add("selectrom", W_BUTTON, 432, 40, 188, 26, "SELECT ROM...");
    for (int i = 0; i < 6; i++) {
        add(size_ids[i], W_RADIO, 270 + i * 56, 158, 50, 26, size_lbl[i]);
        wd[nw - 1].on = cfg.scale == i + 1;
    }
    add("fullscreen", W_CHECK, 120, 200, 22, 22, "FULLSCREEN");
    wd[nw - 1].on = cfg.fullscreen;
    add("integer", W_CHECK, 340, 200, 22, 22, "INTEGER SCALING");
    wd[nw - 1].on = cfg.integer_scale;
    add("volume", W_SLIDER, 270, 250, 260, 22, "");
    add("mute", W_CHECK, 120, 290, 22, 22, "MUTE");
    wd[nw - 1].on = cfg.mute;
    add("keyboard", W_BUTTON, 20, 338, 170, 28, "KEYBOARD...");
    add("controller", W_BUTTON, 198, 338, 190, 28, "CONTROLLER...");
    add("reset", W_BUTTON, 396, 338, 224, 28, "RESET TO DEFAULTS");
    add("play", W_BUTTON, 120, 374, 190, 36, "PLAY");
    wd[nw - 1].primary = 1;
    wd[nw - 1].enabled = st.state != 1;       /* disabled while the fast mode is being prepared */
    add("quit", W_BUTTON, 330, 374, 190, 36, "QUIT");
}

static int find(const char *id) {
    for (int i = 0; i < nw; i++) if (!strcmp(wd[i].id, id)) return i;
    return -1;
}

/* ------------------------------------------------------------------ settings */
static void save_cfg(void) { cfg_save(ini, &cfg); }

static void set_note(const char *s) {
    snprintf(note, sizeof note, "%s", s);
    note_until = SDL_GetTicks() + 2500;
}

static void set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v != cfg.volume) { cfg.volume = v; save_cfg(); }
}

static void activate(int i) {
    if (i < 0 || i >= nw || !wd[i].enabled) return;
    const char *id = wd[i].id;
    if (!strncmp(id, "size", 4)) {
        cfg.scale = id[4] - '0'; save_cfg();
    } else if (!strcmp(id, "fullscreen")) {
        cfg.fullscreen = !cfg.fullscreen; save_cfg();
    } else if (!strcmp(id, "integer")) {
        cfg.integer_scale = !cfg.integer_scale; save_cfg();
    } else if (!strcmp(id, "mute")) {
        cfg.mute = !cfg.mute; save_cfg();
    } else if (!strcmp(id, "keyboard")) {
        page = 1; focus = 0;
    } else if (!strcmp(id, "controller")) {
        page = 2; focus = 0;
    } else if (!strcmp(id, "back")) {
        page = 0; focus = 0;
    } else if (!strcmp(id, "reset")) {
        cfg_reset_keyboard(&cfg); cfg_reset_controller(&cfg); save_cfg();
        set_note("Input settings reset to defaults");
    } else if (!strcmp(id, "selectrom")) {
        result = KL_PICK_ROM; running = 0;
    } else if (!strcmp(id, "play")) {
        if (st.state == 0) result = KL_PICK_ROM;      /* nothing to play yet: ask for a ROM */
        else result = KL_PLAY;
        running = 0;
    } else if (!strcmp(id, "quit")) {
        result = KL_QUIT; running = 0;
    }
    layout();
}

static void focus_move(int d) {
    if (nw == 0) return;
    for (int k = 0; k < nw; k++) {
        focus = (focus + d + nw) % nw;
        if (wd[focus].enabled) return;
    }
}

static int hit(int lx, int ly) {
    for (int i = nw - 1; i >= 0; i--) {
        SDL_Rect r = wd[i].r;
        if (wd[i].type == W_CHECK) r.w = 22 + 10 + text_w(wd[i].label, 2);   /* label is clickable too */
        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) return i;
    }
    return -1;
}

static void slider_from_x(int lx) {
    int i = find("volume");
    if (i < 0) return;
    int v = (lx - wd[i].r.x) * 100 / wd[i].r.w;
    set_volume(v);
}

/* ------------------------------------------------------------------ rendering */
static void draw_widget(int i) {
    Widget *k = &wd[i];
    int is_focus = i == focus, is_hover = i == hover && k->enabled;
    SDL_Color fg = k->enabled ? C_TEXT : C_DIM;
    switch (k->type) {
    case W_BUTTON: {
        SDL_Color bg = !k->enabled ? C_PANEL : k->primary ? C_ACC : C_PANEL;
        if (is_hover) { bg.r = (Uint8)(bg.r + 25 > 255 ? 255 : bg.r + 25); bg.g = (Uint8)(bg.g + 25); bg.b = (Uint8)(bg.b + 25); }
        fill(k->r, bg);
        frame(k->r, k->enabled ? C_TEXT : C_DIM);
        text_center(k->label, k->r, k->r.h >= 36 ? 3 : 2, k->primary && k->enabled ? C_BG : fg);
        break;
    }
    case W_RADIO:
        fill(k->r, k->on ? C_ACC : C_PANEL);
        frame(k->r, C_TEXT);
        text_center(k->label, k->r, 2, k->on ? C_BG : fg);
        break;
    case W_CHECK: {
        fill(k->r, C_PANEL);
        frame(k->r, C_TEXT);
        if (k->on) { SDL_Rect in = { k->r.x + 5, k->r.y + 5, k->r.w - 10, k->r.h - 10 }; fill(in, C_ACC); }
        text(k->label, k->r.x + k->r.w + 10, k->r.y + 4, 2, fg);
        break;
    }
    case W_SLIDER: {
        SDL_Rect track = { k->r.x, k->r.y + 8, k->r.w, 6 };
        fill(track, C_PANEL);
        frame(track, C_DIM);
        SDL_Rect fl = { k->r.x, k->r.y + 8, k->r.w * cfg.volume / 100, 6 };
        fill(fl, C_ACC);
        SDL_Rect knob = { k->r.x + k->r.w * cfg.volume / 100 - 4, k->r.y + 2, 8, 18 };
        fill(knob, C_TEXT);
        char v[16];
        snprintf(v, sizeof v, "%d%%", cfg.volume);
        text(v, k->r.x + k->r.w + 12, k->r.y + 4, 2, cfg.mute ? C_DIM : C_TEXT);
        break;
    }
    }
    if (is_focus) {
        SDL_Rect f = k->r;
        if (k->type == W_CHECK) f.w = 22 + 10 + text_w(k->label, 2);
        f.x -= 3; f.y -= 3; f.w += 6; f.h += 6;
        frame(f, C_FOCUS);
        f.x++; f.y++; f.w -= 2; f.h -= 2;
        frame(f, C_FOCUS);
    }
}

static const char *state_text(void) {
    static char buf[160];
    switch (st.state) {
    case 0: return "Status: Waiting for a ROM";
    case 1: snprintf(buf, sizeof buf, "Status: Preparing... %d%%", st.percent); return buf;
    case 2: snprintf(buf, sizeof buf, "Status: Building %d%% (you can play now)", st.percent); return buf;
    case 3: return "Status: Compiled version ready";
    default: return "Status: Error";
    }
}

static void draw_main(void) {
    text("KIRBY RECOMP", 20, 14, 3, C_ACC);
    /* ROM line */
    char rom[96];
    if (st.rom[0]) {
        const char *b = basename_of(st.rom);
        char name[40];
        size_t n = strlen(b);
        if (n > 30) snprintf(name, sizeof name, "...%.27s", b + n - 27); else snprintf(name, sizeof name, "%.30s", b);
        snprintf(rom, sizeof rom, "ROM: %s %s", name, st.rom_ok ? "OK" : "(unverified)");
    } else {
        snprintf(rom, sizeof rom, "ROM: none - drop a file here");
    }
    text(rom, 620 - text_w(rom, 1) , 22, 1, st.rom[0] ? (st.rom_ok ? C_OK : C_ERR) : C_DIM);
    /* status panel */
    SDL_Rect panel = { 20, 78, 600, 62 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    text(state_text(), 32, 88, 2, st.state == 4 ? C_ERR : C_TEXT);
    if (st.text[0]) text(st.text, 32, 108, 1, C_DIM);
    if (st.state == 1 || st.state == 2) {
        SDL_Rect bar = { 32, 122, 576, 10 };
        frame(bar, C_TEXT);
        int p = st.percent < 0 ? 0 : st.percent > 100 ? 100 : st.percent;
        SDL_Rect f = { 33, 123, 574 * p / 100, 8 };
        if (f.w > 0) fill(f, C_ACC);
    }
    /* groups */
    text("VIDEO", 20, 164, 2, C_ACC);
    text("WINDOW SIZE", 120, 164, 2, C_TEXT);
    text("AUDIO", 20, 252, 2, C_ACC);
    text("VOLUME", 120, 252, 2, cfg.mute ? C_DIM : C_TEXT);
    text("INPUT", 20, 344, 2, C_ACC);
    /* footer */
    if (note[0] && SDL_GetTicks() < note_until) text(note, 20, 408, 1, C_OK);
    else text("TAB/ARROWS: MOVE   ENTER/SPACE: SELECT   ESC: QUIT", 20, 408, 1, C_DIM);
}

static void draw_info(void) {
    text(page == 1 ? "KEYBOARD" : "CONTROLLER", 20, 14, 3, C_ACC);
    SDL_Rect panel = { 20, 56, 600, 290 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    static const char *const names[CFG_BTN_COUNT] = { "A", "B", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT" };
    for (int i = 0; i < CFG_BTN_COUNT; i++) {
        int col = i / 5, row = i % 5;
        int x = 40 + col * 290, y = 70 + row * 34;
        text(names[i], x, y, 2, C_TEXT);
        text(page == 1 ? cfg.key[i] : cfg.pad[i], x + 130, y, 2, C_ACC);
    }
    if (page == 1) {
        text("RESERVED: ESC (QUIT)  F11 (FULLSCREEN)  F1 (LAUNCHER)", 40, 258, 1, C_DIM);
        text("MAPPED BY POSITION: GBA A = X, GBA B = Z", 40, 274, 1, C_DIM);
    } else {
        char s[64];
        snprintf(s, sizeof s, "STICK AS D-PAD: %s   DEADZONE: %d", cfg.stick_as_dpad ? "ON" : "OFF", cfg.deadzone);
        text(s, 40, 258, 1, C_DIM);
        text("GBA A = EAST BUTTON, GBA B = SOUTH BUTTON", 40, 274, 1, C_DIM);
    }
    text("EDITING THESE BINDINGS IS NOT AVAILABLE YET", 40, 306, 1, C_DIM);
    text("ESC OR ENTER: BACK", 20, 408, 1, C_DIM);
}

static void render(void) {
    setc(C_BG);
    SDL_RenderClear(ren);
    if (page) draw_info(); else draw_main();
    for (int i = 0; i < nw; i++) draw_widget(i);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ events */
static void to_logical(int wx, int wy, int *lx, int *ly) {
    float fx, fy;
    SDL_RenderWindowToLogical(ren, wx, wy, &fx, &fy);
    *lx = (int)fx; *ly = (int)fy;
}

static void on_key(SDL_Keycode k, int shift) {
    if (k == SDLK_ESCAPE) {
        if (page) { page = 0; focus = 0; layout(); }
        else { result = KL_QUIT; running = 0; }
        return;
    }
    Widget *f = &wd[focus];
    if (k == SDLK_TAB) { focus_move(shift ? -1 : 1); return; }
    if (k == SDLK_DOWN) { focus_move(1); return; }
    if (k == SDLK_UP) { focus_move(-1); return; }
    if (k == SDLK_LEFT || k == SDLK_RIGHT) {
        int d = k == SDLK_RIGHT ? 1 : -1;
        if (f->type == W_SLIDER) { set_volume(cfg.volume + 5 * d); return; }
        if (f->type == W_RADIO) {
            int ni = focus + d;
            if (ni >= 0 && ni < nw && wd[ni].type == W_RADIO) { focus = ni; activate(ni); }
            return;
        }
        focus_move(d);
        return;
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
        if (page) { page = 0; focus = 0; layout(); return; }
        activate(focus);
    }
}

static void handle(SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT:
        result = KL_QUIT; running = 0; break;
    case SDL_KEYDOWN:
        if (!e->key.repeat || e->key.keysym.sym == SDLK_LEFT || e->key.keysym.sym == SDLK_RIGHT)
            on_key(e->key.keysym.sym, (e->key.keysym.mod & KMOD_SHIFT) != 0);
        break;
    case SDL_MOUSEMOTION: {
        int lx, ly;
        to_logical(e->motion.x, e->motion.y, &lx, &ly);
        hover = hit(lx, ly);
        if (dragging) slider_from_x(lx);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            int lx, ly;
            to_logical(e->button.x, e->button.y, &lx, &ly);
            int i = hit(lx, ly);
            if (i >= 0 && wd[i].enabled) {
                focus = i;
                if (wd[i].type == W_SLIDER) { dragging = 1; slider_from_x(lx); }
                else activate(i);
            }
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) dragging = 0;
        break;
    case SDL_DROPFILE:
        if (e->drop.file) {
            snprintf(dropped, sizeof dropped, "%s", e->drop.file);
            SDL_free(e->drop.file);
            result = KL_PICK_ROM; running = 0;
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ public API */
KL_API const char *kl_dropped_path(void) { return dropped[0] ? dropped : NULL; }
KL_API void kl_set_poll_interval(int ms) { poll_ms = ms < 1 ? 1 : ms; }

KL_API int kl_test_widget(const char *id, int *x, int *y, int *w, int *h) {
    int i = find(id);
    if (i < 0) return 0;
    if (x) *x = wd[i].r.x;
    if (y) *y = wd[i].r.y;
    if (w) *w = wd[i].r.w;
    if (h) *h = wd[i].r.h;
    return 1;
}
KL_API const char *kl_test_focus(void) { return nw ? wd[focus].id : ""; }
KL_API void kl_test_set_focus(const char *id) { int i = find(id); if (i >= 0) focus = i; }
KL_API const char *kl_test_page(void) { return page == 0 ? "main" : page == 1 ? "keyboard" : "controller"; }
KL_API int kl_test_snapshot(uint32_t *out, int cap, int *w, int *h) {
    if (!ren || cap < KL_W * KL_H) return -1;
    if (w) *w = KL_W;
    if (h) *h = KL_H;
    return SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, out, KL_W * 4);
}

KL_API int kl_run(const char *ini_path, KlPoll poll) {
    snprintf(ini, sizeof ini, "%s", ini_path ? ini_path : "kirby.ini");
    cfg_load(ini, &cfg);
    memset(&st, 0, sizeof st);
    dropped[0] = 0; note[0] = 0; page = 0; focus = 0; hover = -1; dragging = 0;
    result = KL_QUIT; running = 1;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "kirby_ui: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return KL_QUIT;
    }
    win = SDL_CreateWindow(WIN_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, KL_W, KL_H, SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "kirby_ui: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return KL_QUIT;
    }
    {
        static uint32_t icon[32 * 32];
        win_icon_pixels(icon);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(icon, 32, 32, 32, 128, SDL_PIXELFORMAT_ARGB8888);
        if (s) { SDL_SetWindowIcon(win, s); SDL_FreeSurface(s); }
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "kirby_ui: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); win = NULL;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return KL_QUIT;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(ren, KL_W, KL_H);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    if (poll) poll(&st);
    layout();
    Uint32 next_poll = SDL_GetTicks() + (Uint32)poll_ms;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 16)) {
            do handle(&e); while (running && SDL_PollEvent(&e));
        }
        if (!running) break;
        if (poll && SDL_GetTicks() >= next_poll) {
            KlStatus ns = st;
            memset(&ns, 0, sizeof ns);
            poll(&ns);
            ns.text[sizeof ns.text - 1] = 0;
            ns.rom[sizeof ns.rom - 1] = 0;
            int was = st.state;
            st = ns;
            if (was != st.state || page == 0) layout();
            next_poll = SDL_GetTicks() + (Uint32)poll_ms;
        }
        if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) render();
    }
    save_cfg();
    SDL_DestroyRenderer(ren); ren = NULL;
    SDL_DestroyWindow(win); win = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result;
}
