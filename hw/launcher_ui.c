/* launcher_ui.c - see launcher_ui.h. Retro-style immediate-mode UI on SDL2, English only.
 * Logical canvas 480x320; the window follows the video options (see launcher_ui.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "launcher_ui.h"
#include "config_ini.h"
#include "window_sdl.h"      /* WIN_TITLE, win_icon_pixels, win_font_rows */

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

/* ------------------------------------------------------------------ video options -> window */
static int want_w(void) { int w = 240 * cfg.scale; return w < KL_W ? KL_W : w; }
static int want_h(void) { int h = 160 * cfg.scale; return h < KL_H ? KL_H : h; }

static void apply_scaling(void) {
    if (!ren) return;
    if (cfg.scaling == CFG_SCALING_STRETCH) {
        SDL_RenderSetIntegerScale(ren, SDL_FALSE);
        SDL_RenderSetLogicalSize(ren, 0, 0);          /* no letterbox: the canvas is stretched per frame */
    } else {
        SDL_RenderSetLogicalSize(ren, KL_W, KL_H);    /* aspect preserved (black bars) */
        SDL_RenderSetIntegerScale(ren, cfg.scaling == CFG_SCALING_INTEGER ? SDL_TRUE : SDL_FALSE);
    }
}

static void apply_video(int first) {
    if (!win) return;
    if (cfg.fullscreen) {
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowSize(win, want_w(), want_h());
        if (!first) SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    apply_scaling();
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

static int rom_missing(void) { return st.state == 0 || !st.rom_ok; }

static void layout(void) {
    static const char *const size_ids[6] = { "size1", "size2", "size3", "size4", "size5", "size6" };
    static const char *const size_lbl[6] = { "1X", "2X", "3X", "4X", "5X", "6X" };
    char fid[32] = "";
    if (nw && focus >= 0 && focus < nw) snprintf(fid, sizeof fid, "%s", wd[focus].id);
    nw = 0;
    if (page) {                               /* info page: only a Back button */
        add("back", W_BUTTON, 155, 278, 170, 32, "BACK");
        wd[0].primary = 1;
    } else {
        if (rom_missing()) add("selectrom", W_BUTTON, 298, 34, 170, 22, "SELECT ROM...");
        for (int i = 0; i < 6; i++) {
            add(size_ids[i], W_RADIO, 220 + i * 42, 116, 40, 24, size_lbl[i]);
            wd[nw - 1].on = cfg.scale == i + 1;
            wd[nw - 1].enabled = !cfg.fullscreen;          /* the size is irrelevant in full screen */
        }
        add("fullscreen", W_CHECK, 84, 146, 22, 22, "FULLSCREEN");
        wd[nw - 1].on = cfg.fullscreen;
        add("scale_integer", W_RADIO, 220, 174, 96, 24, "INTEGER");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_INTEGER;
        add("scale_fit", W_RADIO, 322, 174, 50, 24, "FIT");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_FIT;
        add("scale_stretch", W_RADIO, 378, 174, 94, 24, "STRETCH");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_STRETCH;
        add("volume", W_SLIDER, 220, 206, 190, 22, "");
        add("mute", W_CHECK, 84, 234, 22, 22, "MUTE");
        wd[nw - 1].on = cfg.mute;
        add("keyboard", W_BUTTON, 84, 262, 140, 24, "KEYBOARD...");
        add("controller", W_BUTTON, 230, 262, 166, 24, "CONTROLLER...");
        add("reset", W_BUTTON, 402, 262, 66, 24, "RESET");
        add("play", W_BUTTON, 80, 290, 180, 20, "PLAY");
        wd[nw - 1].primary = 1;
        wd[nw - 1].enabled = st.state != 1;       /* disabled while the fast mode is being prepared */
        add("quit", W_BUTTON, 280, 290, 180, 20, "QUIT");
    }
    focus = 0;
    for (int i = 0; i < nw; i++) if (fid[0] && !strcmp(wd[i].id, fid)) focus = i;
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
        cfg.scale = id[4] - '0'; save_cfg(); apply_video(0);
    } else if (!strcmp(id, "fullscreen")) {
        cfg.fullscreen = !cfg.fullscreen; save_cfg(); apply_video(0);
    } else if (!strcmp(id, "scale_integer")) {
        cfg.scaling = CFG_SCALING_INTEGER; save_cfg(); apply_scaling();
    } else if (!strcmp(id, "scale_fit")) {
        cfg.scaling = CFG_SCALING_FIT; save_cfg(); apply_scaling();
    } else if (!strcmp(id, "scale_stretch")) {
        cfg.scaling = CFG_SCALING_STRETCH; save_cfg(); apply_scaling();
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

static int widget_w(const Widget *k) {
    return k->type == W_CHECK ? 22 + 10 + text_w(k->label, 2) : k->r.w;   /* the label is clickable too */
}

static int hit(int lx, int ly) {
    for (int i = nw - 1; i >= 0; i--) {
        SDL_Rect r = wd[i].r;
        r.w = widget_w(&wd[i]);
        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) return i;
    }
    return -1;
}

static void slider_from_x(int lx) {
    int i = find("volume");
    if (i < 0) return;
    set_volume((lx - wd[i].r.x) * 100 / wd[i].r.w);
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
        text_center(k->label, k->r, k->r.h >= 30 ? 3 : 2, k->primary && k->enabled ? C_BG : fg);
        break;
    }
    case W_RADIO:
        fill(k->r, k->on && k->enabled ? C_ACC : C_PANEL);
        frame(k->r, k->enabled ? C_TEXT : C_DIM);
        text_center(k->label, k->r, 2, k->on && k->enabled ? C_BG : fg);
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
        f.w = widget_w(k);
        f.x -= 3; f.y -= 3; f.w += 6; f.h += 6;
        frame(f, C_FOCUS);
        f.x++; f.y++; f.w -= 2; f.h -= 2;
        frame(f, C_FOCUS);
    }
}

static const char *state_text(void) {
    static char buf[64];
    switch (st.state) {
    case 0: return "Waiting for a ROM";
    case 1: snprintf(buf, sizeof buf, "Preparing... %d%%", st.percent); return buf;
    case 2: snprintf(buf, sizeof buf, "Building %d%% - you can play now", st.percent); return buf;
    case 3: return "Compiled version ready";
    default: return "Error";
    }
}

static void draw_main(void) {
    text("KIRBY RECOMP", 12, 8, 3, C_ACC);
    /* ROM line: "ROM: <name> OK" when a verified ROM is loaded; otherwise a hint + the SELECT ROM button */
    char rom[96];
    SDL_Color rc = C_DIM;
    if (st.rom[0]) {
        const char *b = basename_of(st.rom);
        char name[40];
        size_t n = strlen(b);
        if (n > 30) snprintf(name, sizeof name, "...%.27s", b + n - 27); else snprintf(name, sizeof name, "%.30s", b);
        snprintf(rom, sizeof rom, st.rom_ok ? "ROM: %s OK" : "ROM: %s (unverified)", name);
        rc = st.rom_ok ? C_OK : C_ERR;
    } else {
        snprintf(rom, sizeof rom, "ROM: none - drop a file here");
    }
    text(rom, 468 - text_w(rom, 1), 10, 1, rc);
    if (cfg.scale == 1) {
        const char *cap = "1X PREVIEW (UI AT 2X)";
        text(cap, 468 - text_w(cap, 1), 21, 1, C_DIM);
    }
    /* status panel */
    SDL_Rect panel = { 12, 62, 456, 46 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    text("STATUS", 20, 66, 1, C_DIM);
    text(state_text(), 20, 76, 2, st.state == 4 ? C_ERR : C_TEXT);
    if (st.text[0]) text(st.text, 20, 96, 1, C_DIM);
    if (st.state == 1 || st.state == 2) {
        SDL_Rect bar = { 200, 95, 260, 9 };
        frame(bar, C_TEXT);
        int p = st.percent < 0 ? 0 : st.percent > 100 ? 100 : st.percent;
        SDL_Rect f = { 201, 96, 258 * p / 100, 7 };
        if (f.w > 0) fill(f, C_ACC);
    }
    /* groups and row labels */
    text("VIDEO", 12, 121, 2, C_ACC);
    text("WINDOW SIZE", 84, 121, 2, cfg.fullscreen ? C_DIM : C_TEXT);
    text("SCALING", 84, 179, 2, C_TEXT);
    text("AUDIO", 12, 211, 2, C_ACC);
    text("VOLUME", 84, 211, 2, cfg.mute ? C_DIM : C_TEXT);
    text("INPUT", 12, 267, 2, C_ACC);
    /* footer */
    if (note[0] && SDL_GetTicks() < note_until) text(note, 12, 313, 1, C_OK);
    else text("TAB/ARROWS: MOVE   ENTER/SPACE: SELECT   ESC: QUIT", 12, 313, 1, C_DIM);
}

static void draw_info(void) {
    text(page == 1 ? "KEYBOARD" : "CONTROLLER", 12, 8, 3, C_ACC);
    SDL_Rect panel = { 12, 44, 456, 226 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    static const char *const names[CFG_BTN_COUNT] = { "A", "B", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT" };
    for (int i = 0; i < CFG_BTN_COUNT; i++) {
        int y = 50 + i * 19;
        text(names[i], 24, y, 2, C_TEXT);
        text(page == 1 ? cfg.key[i] : cfg.pad[i], 130, y, 2, C_ACC);
    }
    if (page == 1) {
        text("RESERVED: ESC (QUIT)", 300, 54, 1, C_DIM);
        text("F11 (FULLSCREEN)", 300, 66, 1, C_DIM);
        text("F1 (LAUNCHER)", 300, 78, 1, C_DIM);
        text("MAPPED BY POSITION:", 300, 98, 1, C_DIM);
        text("GBA A = X, GBA B = Z", 300, 110, 1, C_DIM);
    } else {
        char s[64];
        snprintf(s, sizeof s, "STICK AS D-PAD: %s", cfg.stick_as_dpad ? "ON" : "OFF");
        text(s, 300, 54, 1, C_DIM);
        snprintf(s, sizeof s, "DEADZONE: %d", cfg.deadzone);
        text(s, 300, 70, 1, C_DIM);
        text("GBA A = EAST BUTTON", 300, 90, 1, C_DIM);
        text("GBA B = SOUTH BUTTON", 300, 102, 1, C_DIM);
    }
    text("EDITING IS NOT AVAILABLE YET", 12, 278, 1, C_DIM);
    text("ESC OR ENTER: BACK", 12, 311, 1, C_DIM);
}

static void render(void) {
    if (cfg.scaling == CFG_SCALING_STRETCH) {      /* map the canvas onto the whole window (no letterbox) */
        int ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_RenderSetScale(ren, (float)ow / KL_W, (float)oh / KL_H);
    }   /* (integer/fit: SDL keeps the logical-size scale in the renderer; do not touch it) */
    setc(C_BG);
    SDL_RenderClear(ren);
    if (page) draw_info(); else draw_main();
    for (int i = 0; i < nw; i++) draw_widget(i);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ events */
static void to_logical(int wx, int wy, int *lx, int *ly) {
    if (cfg.scaling == CFG_SCALING_STRETCH) {
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        *lx = ww > 0 ? wx * KL_W / ww : wx;
        *ly = wh > 0 ? wy * KL_H / wh : wy;
        return;
    }
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
    if (nw == 0) return;
    Widget *f = &wd[focus];
    if (k == SDLK_TAB) { focus_move(shift ? -1 : 1); return; }
    if (k == SDLK_DOWN) { focus_move(1); return; }
    if (k == SDLK_UP) { focus_move(-1); return; }
    if (k == SDLK_LEFT || k == SDLK_RIGHT) {
        int d = k == SDLK_RIGHT ? 1 : -1;
        if (f->type == W_SLIDER) { set_volume(cfg.volume + 5 * d); return; }
        if (f->type == W_RADIO) {
            int ni = focus + d;
            if (ni >= 0 && ni < nw && wd[ni].type == W_RADIO && wd[ni].enabled) { focus = ni; activate(ni); }
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
    if (w) *w = widget_w(&wd[i]);
    if (h) *h = wd[i].r.h;
    return 1;
}
KL_API int kl_test_enabled(const char *id) { int i = find(id); return i >= 0 && wd[i].enabled; }
KL_API const char *kl_test_focus(void) { return nw ? wd[focus].id : ""; }
KL_API void kl_test_set_focus(const char *id) { int i = find(id); if (i >= 0) focus = i; }
KL_API const char *kl_test_page(void) { return page == 0 ? "main" : page == 1 ? "keyboard" : "controller"; }

/* Frame of the logical canvas (KL_W x KL_H). The renderer output may be larger/offset (zoom, letterbox):
 * the whole output is read back and the canvas region is sampled out of it. */
KL_API int kl_test_snapshot(uint32_t *out, int cap, int *w, int *h) {
    if (!ren || cap < KL_W * KL_H) return -1;
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (w) *w = KL_W;
    if (h) *h = KL_H;
    if (ow == KL_W && oh == KL_H) return SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, out, KL_W * 4);
    static uint32_t *big;
    static int big_cap;
    if (ow * oh > big_cap) { free(big); big = (uint32_t *)malloc((size_t)ow * oh * 4); big_cap = ow * oh; }
    if (!big || SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, big, ow * 4) != 0) return -1;
    float zx, zy;                                   /* output pixels per canvas pixel and the canvas origin */
    int ox = 0, oy = 0;
    if (cfg.scaling == CFG_SCALING_STRETCH) {
        zx = (float)ow / KL_W; zy = (float)oh / KL_H;
    } else {
        float s = (float)ow / KL_W < (float)oh / KL_H ? (float)ow / KL_W : (float)oh / KL_H;
        if (cfg.scaling == CFG_SCALING_INTEGER && s >= 1.0f) s = (float)(int)s;
        zx = zy = s;
        ox = (int)((ow - KL_W * s) / 2);
        oy = (int)((oh - KL_H * s) / 2);
    }
    for (int y = 0; y < KL_H; y++)
        for (int x = 0; x < KL_W; x++) {
            int px = ox + (int)((x + 0.5f) * zx), py = oy + (int)((y + 0.5f) * zy);
            if (px >= ow) px = ow - 1;
            if (py >= oh) py = oh - 1;
            out[y * KL_W + x] = big[py * ow + px];
        }
    return 0;
}

KL_API void kl_test_video(int *ww, int *wh, int *fs) {
    int w = 0, h = 0;
    if (win) SDL_GetWindowSize(win, &w, &h);
    if (ww) *ww = w;
    if (wh) *wh = h;
    if (fs) *fs = win && (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) ? 1 : 0;
}

KL_API int kl_run(const char *ini_path, KlPoll poll) {
    snprintf(ini, sizeof ini, "%s", ini_path ? ini_path : "kirby.ini");
    cfg_load(ini, &cfg);
    memset(&st, 0, sizeof st);
    dropped[0] = 0; note[0] = 0; page = 0; focus = 0; hover = -1; dragging = 0; nw = 0;
    result = KL_QUIT; running = 1;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "kirby_ui: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return KL_QUIT;
    }
    win = SDL_CreateWindow(WIN_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, want_w(), want_h(),
                           SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "kirby_ui: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return KL_QUIT;
    }
    SDL_SetWindowMinimumSize(win, KL_W, KL_H);
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
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    apply_video(1);

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
            KlStatus ns;
            memset(&ns, 0, sizeof ns);
            poll(&ns);
            ns.text[sizeof ns.text - 1] = 0;
            ns.rom[sizeof ns.rom - 1] = 0;
            st = ns;
            layout();
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
