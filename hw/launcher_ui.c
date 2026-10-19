/* launcher_ui.c - see launcher_ui.h. Retro-style immediate-mode UI on SDL2, English only.
 * Logical canvas 480x320; the window follows the video options (see launcher_ui.h).
 * Pages: main (status/video/audio/input) and the editable KEYBOARD / CONTROLLER pages with a GBA outline. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "launcher_ui.h"
#include "config_ini.h"
#include "window_sdl.h"      /* WIN_TITLE, win_icon_pixels, win_font_rows */

/* ------------------------------------------------------------------ widgets */
typedef enum { W_BUTTON, W_RADIO, W_CHECK, W_SLIDER, W_SHAPE, W_FIELD } WType;
enum { SH_RECT, SH_CIRCLE };

typedef struct {
    const char *id;
    WType type;
    SDL_Rect r;
    char label[40];
    int enabled;
    int on;                 /* radio selected / checkbox checked */
    int primary;            /* highlighted button (Play) */
    int btn;                /* W_SHAPE: CFG_BTN_* index */
    int shape;              /* W_SHAPE: SH_* */
    int slider;             /* W_SLIDER: 0 = volume, 1 = deadzone, 2 = link delay */
    int fld;                /* W_FIELD: 0 name, 1 address, 2 port */
} Widget;

#define MAX_W 32
static Widget wd[MAX_W];
static int nw;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *canvas;          /* the UI is drawn here at 480x320 and then scaled to the window by us */
static SDL_Rect dst;                 /* where the canvas lands in the window output (pixels) */
static KirbyConfig cfg;
static KlStatus st;
static char ini[512];
static char dropped[1024];
static int poll_ms = 100;
static int focus;                    /* index into wd[] */
static int hover = -1;
static int dragging;                 /* slider drag: 0 none, 1 volume, 2 deadzone */
static int page;                     /* 0 main, 1 keyboard, 2 controller */
static int capture = -1;             /* CFG_BTN_* being remapped, or -1 */
static int confirm;                  /* 1 = the "overwrite current save?" dialog is open */

/* ONLINE page */
static char fbuf[3][64];             /* edit buffers: name, address, port */
static const int fcap[3] = { 15, 45, 5 };
static int editing = -1;             /* text field being typed in, -1 = none */
static int keypad_field = -1;        /* field being edited with the on-screen keypad, -1 = closed */
static char kp_backup[64];
static int via_pad;                  /* the current activate() was triggered by the controller */
static int cur_mod;                  /* keyboard modifiers of the key event being handled */
static int start_page;
static char want_focus[24];          /* widget id to focus after the next layout() */
static KlOnlinePoll online_poll;
static KlOnline onl;
static void layout(void);
static void focus_move(int d);
static void nav(int dir);
static int port_ok(void) { int n = atoi(fbuf[2]); return fbuf[2][0] && n >= 1 && n <= 65535; }
static int online_busy(void) { return onl.state != KLO_OFFLINE; }
static int slots_up(void) { int n = 0; for (int i = 0; i < 4; i++) if (onl.slots[i] == KLS_CONNECTED || onl.slots[i] == KLS_READY) n++; return n; }

/* texts and the progress bar drawn on the last frame (layout checks in the tests) */
#define MAX_TEXTS 40
static SDL_Rect texts[MAX_TEXTS];
static int ntexts, rec_texts;
static SDL_Rect bar_rect;
static int bar_drawn;
static char note[96];
static Uint32 note_until;
static int note_err;
static int result;
static int running;

/* game controller: only the first connected one */
static SDL_GameController *pad;
static int fake_pad = -1, fake_ax, fake_ay;      /* test override: -1 = use the real controller */

static const SDL_Color C_BG = { 20, 20, 36, 255 }, C_PANEL = { 34, 34, 58, 255 }, C_TEXT = { 230, 230, 240, 255 },
                       C_ACC = { 255, 120, 170, 255 }, C_DIM = { 110, 110, 135, 255 },
                       C_FOCUS = { 255, 220, 80, 255 }, C_OK = { 120, 220, 150, 255 }, C_ERR = { 255, 100, 100, 255 },
                       C_BODY = { 52, 52, 86, 255 }, C_SCREEN = { 10, 10, 20, 255 };

/* ------------------------------------------------------------------ drawing */
static void setc(SDL_Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }
static void fill(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderFillRect(ren, &r); }
static void frame(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderDrawRect(ren, &r); }

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

static void fill_circle(int cx, int cy, int r, SDL_Color c) {
    setc(c);
    for (int dy = -r; dy <= r; dy++) {
        int dx = isqrt(r * r - dy * dy);
        SDL_Rect q = { cx - dx, cy + dy, 2 * dx + 1, 1 };
        SDL_RenderFillRect(ren, &q);
    }
}

static void ring(int cx, int cy, int r, SDL_Color c) {      /* midpoint circle outline */
    if (r < 1) { SDL_RenderDrawPoint(ren, cx, cy); return; }
    setc(c);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        SDL_RenderDrawPoint(ren, cx + x, cy + y); SDL_RenderDrawPoint(ren, cx - x, cy + y);
        SDL_RenderDrawPoint(ren, cx + x, cy - y); SDL_RenderDrawPoint(ren, cx - x, cy - y);
        SDL_RenderDrawPoint(ren, cx + y, cy + x); SDL_RenderDrawPoint(ren, cx - y, cy + x);
        SDL_RenderDrawPoint(ren, cx + y, cy - x); SDL_RenderDrawPoint(ren, cx - y, cy - x);
        y++;
        if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
    }
}

static void round_rect(SDL_Rect r, int rad, SDL_Color c) {  /* filled rectangle with rounded corners */
    setc(c);
    for (int y = 0; y < r.h; y++) {
        int d = y < rad ? rad - y : (y >= r.h - rad ? y - (r.h - rad - 1) : 0);
        int inset = d ? rad - isqrt(rad * rad - d * d) : 0;
        SDL_Rect q = { r.x + inset, r.y + y, r.w - 2 * inset, 1 };
        SDL_RenderFillRect(ren, &q);
    }
}

static int text_w(const char *s, int scale) {
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void text(const char *s, int x, int y, int scale, SDL_Color c) {
    if (rec_texts && ntexts < MAX_TEXTS && *s) {
        SDL_Rect r = { x, y, (int)strlen(s) * 6 * scale - scale, 7 * scale };
        texts[ntexts++] = r;
    }
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

/* text clipped to max_px at scale 1 (ellipsis dropped: just cut) */
static void clip(const char *s, int max_px, char *out, size_t cap) {
    size_t n = strlen(s);
    while (n > 0 && (int)(n * 6 - 1) > max_px) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ------------------------------------------------------------------ video options -> window */
static int want_w(void) { int w = 240 * cfg.scale; return w < KL_W ? KL_W : w; }
static int want_h(void) { int h = 160 * cfg.scale; return h < KL_H ? KL_H : h; }

/* The canvas -> window mapping is done by compute_dst()/render() and to_logical() below. SDL's own
 * SDL_RenderSetLogicalSize is deliberately not used: it converts the coordinates of real mouse events in
 * place (when they carry the window id), which made clicks miss in windows that are not 480x320. */
static void apply_scaling(void) {}

static void compute_dst(void) {
    int ow = KL_W, oh = KL_H;
    if (ren) SDL_GetRendererOutputSize(ren, &ow, &oh);
    if (cfg.scaling == CFG_SCALING_STRETCH) { dst.x = 0; dst.y = 0; dst.w = ow; dst.h = oh; return; }
    float s = (float)ow / KL_W < (float)oh / KL_H ? (float)ow / KL_W : (float)oh / KL_H;
    if (cfg.scaling == CFG_SCALING_INTEGER && s >= 1.0f) s = (float)(int)s;
    dst.w = (int)(KL_W * s + 0.5f);
    dst.h = (int)(KL_H * s + 0.5f);
    dst.x = (ow - dst.w) / 2;
    dst.y = (oh - dst.h) / 2;
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

/* ------------------------------------------------------------------ controller */
static void pad_open_first(void) {
    if (pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) return; }
}

static int pad_present(void) { return fake_pad >= 0 ? fake_pad : pad != NULL; }

static void pad_axes(int *ax, int *ay) {
    *ax = *ay = 0;
    if (fake_pad >= 0) { *ax = fake_ax; *ay = fake_ay; return; }
    if (!pad) return;
    *ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    *ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
}

/* ------------------------------------------------------------------ layout */
static void add(const char *id, WType t, int x, int y, int w, int h, const char *label) {
    if (nw >= MAX_W) return;
    Widget *k = &wd[nw++];
    memset(k, 0, sizeof *k);
    k->id = id; k->type = t; k->r.x = x; k->r.y = y; k->r.w = w; k->r.h = h;
    k->enabled = 1;
    k->btn = -1;
    strncpy(k->label, label, sizeof k->label - 1);
}

static void add_shape(const char *id, int btn, int shape, int x, int y, int w, int h) {
    add(id, W_SHAPE, x, y, w, h, "");
    wd[nw - 1].btn = btn;
    wd[nw - 1].shape = shape;
}

static int rom_missing(void) { return st.state == 0 || !st.rom_ok; }
static int rom_loaded(void) { return st.rom[0] && st.rom_ok; }      /* a verified ROM: the save can be managed */

static void layout(void) {
    static const char *const size_ids[6] = { "size1", "size2", "size3", "size4", "size5", "size6" };
    static const char *const size_lbl[6] = { "1X", "2X", "3X", "4X", "5X", "6X" };
    char fid[32] = "";
    if (nw && focus >= 0 && focus < nw) snprintf(fid, sizeof fid, "%s", wd[focus].id);
    nw = 0;
    if (keypad_field >= 0) {                  /* on-screen keypad (controller): digits, '.', DEL, CLR, OK, CANCEL */
        static const char *const kid[10] = { "kp_0", "kp_1", "kp_2", "kp_3", "kp_4", "kp_5", "kp_6", "kp_7", "kp_8", "kp_9" };
        for (int d = 1; d <= 9; d++) {
            char lab[2] = { (char)('0' + d), 0 };
            add(kid[d], W_BUTTON, 108 + ((d - 1) % 3) * 66, 108 + ((d - 1) / 3) * 36, 60, 28, lab);
        }
        add("kp_del", W_BUTTON, 306, 108, 60, 28, "DEL");
        add("kp_clr", W_BUTTON, 306, 144, 60, 28, "CLR");
        add("kp_dot", W_BUTTON, 306, 180, 60, 28, ".");
        wd[nw - 1].enabled = keypad_field == 1;        /* a port has no dots */
        add("kp_0", W_BUTTON, 108, 216, 60, 28, "0");
        add("kp_ok", W_BUTTON, 174, 216, 126, 28, "OK");
        wd[nw - 1].primary = 1;
        add("kp_cancel", W_BUTTON, 306, 216, 72, 28, "CANCEL");
    } else if (confirm) {                            /* modal: only YES / NO are reachable */
        add("confirm_yes", W_BUTTON, 120, 176, 100, 24, "YES");
        add("confirm_no", W_BUTTON, 260, 176, 100, 24, "NO");
        wd[1].primary = 1;
    } else if (page == KL_PAGE_ONLINE) {
        int off = !online_busy();
        add("online_name", W_FIELD, 80, 34, 190, 22, "");   wd[nw - 1].fld = 0; wd[nw - 1].enabled = off;
        for (int n = 2; n <= 4; n++) {
            static const char *const pid[3] = { "online_p2", "online_p3", "online_p4" };
            char lab[2] = { (char)('0' + n), 0 };
            add(pid[n - 2], W_RADIO, 372 + (n - 2) * 32, 34, 30, 22, lab);
            wd[nw - 1].on = cfg.link_players == n; wd[nw - 1].enabled = off;
        }
        add("online_addr", W_FIELD, 100, 62, 368, 22, "");  wd[nw - 1].fld = 1; wd[nw - 1].enabled = off;
        add("online_port", W_FIELD, 80, 90, 70, 22, "");   wd[nw - 1].fld = 2; wd[nw - 1].enabled = off;
        add("online_delay", W_SLIDER, 236, 90, 150, 22, ""); wd[nw - 1].slider = 2; wd[nw - 1].enabled = off;
        add("online_host", W_BUTTON, 12, 118, 100, 24, "HOST");
        wd[nw - 1].enabled = off && port_ok(); wd[nw - 1].primary = 1;
        add("online_join", W_BUTTON, 118, 118, 100, 24, "JOIN");
        wd[nw - 1].enabled = off && port_ok() && fbuf[1][0];
        add("online_start", W_BUTTON, 224, 118, 100, 24, "START");
        wd[nw - 1].enabled = onl.state == KLO_HOSTING && slots_up() >= 2;
        add("online_leave", W_BUTTON, 330, 118, 100, 24, "LEAVE");
        wd[nw - 1].enabled = online_busy();
        add("online_copy", W_BUTTON, 396, 144, 72, 20, "COPY");
        wd[nw - 1].enabled = onl.my_address[0] != 0;
        add("online_back", W_BUTTON, 380, 6, 88, 22, "BACK");
    } else if (page) {
        /* editable pages: tabs + reset + back on top, the GBA outline in the middle */
        add("tab_keyboard", W_RADIO, 12, 6, 130, 22, "KEYBOARD");
        wd[nw - 1].on = page == 1;
        add("tab_controller", W_RADIO, 148, 6, 150, 22, "CONTROLLER");
        wd[nw - 1].on = page == 2;
        add("reset", W_BUTTON, 304, 6, 70, 22, "RESET");
        add("back", W_BUTTON, 380, 6, 88, 22, "BACK");
        add_shape("btn_l", CFG_BTN_L, SH_RECT, 62, 44, 96, 20);
        add_shape("btn_r", CFG_BTN_R, SH_RECT, 322, 44, 96, 20);
        add_shape("btn_up", CFG_BTN_UP, SH_RECT, 88, 104, 24, 24);
        add_shape("btn_left", CFG_BTN_LEFT, SH_RECT, 64, 128, 24, 24);
        add_shape("btn_right", CFG_BTN_RIGHT, SH_RECT, 112, 128, 24, 24);
        add_shape("btn_down", CFG_BTN_DOWN, SH_RECT, 88, 152, 24, 24);
        add_shape("btn_select", CFG_BTN_SELECT, SH_RECT, 166, 190, 66, 16);
        add_shape("btn_start", CFG_BTN_START, SH_RECT, 248, 190, 66, 16);
        add_shape("btn_b", CFG_BTN_B, SH_CIRCLE, 355, 131, 34, 34);
        add_shape("btn_a", CFG_BTN_A, SH_CIRCLE, 393, 111, 34, 34);
        if (page == 2) {
            add("stick", W_CHECK, 12, 262, 22, 22, "LEFT STICK AS D-PAD");
            wd[nw - 1].on = cfg.stick_as_dpad;
            add("deadzone", W_SLIDER, 120, 288, 150, 22, "");
            wd[nw - 1].slider = 1;
        }
    } else {
        if (rom_missing()) add("selectrom", W_BUTTON, 298, 34, 170, 22, "SELECT ROM...");
        else add("online", W_BUTTON, 298, 34, 170, 22, "ONLINE...");            /* the same slot: a ROM is loaded */
        for (int i = 0; i < 6; i++) {
            add(size_ids[i], W_RADIO, 220 + i * 42, 106, 40, 22, size_lbl[i]);
            wd[nw - 1].on = cfg.scale == i + 1;
            wd[nw - 1].enabled = !cfg.fullscreen;          /* the size is irrelevant in full screen */
        }
        add("fullscreen", W_CHECK, 84, 132, 22, 22, "FULLSCREEN");
        wd[nw - 1].on = cfg.fullscreen;
        add("scale_integer", W_RADIO, 220, 158, 96, 22, "INTEGER");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_INTEGER;
        add("scale_fit", W_RADIO, 322, 158, 50, 22, "FIT");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_FIT;
        add("scale_stretch", W_RADIO, 378, 158, 94, 22, "STRETCH");
        wd[nw - 1].on = cfg.scaling == CFG_SCALING_STRETCH;
        add("volume", W_SLIDER, 220, 184, 190, 22, "");
        add("mute", W_CHECK, 84, 210, 22, 22, "MUTE");
        add("save_open", W_BUTTON, 84, 236, 140, 22, "OPEN FOLDER");
        add("save_export", W_BUTTON, 230, 236, 116, 22, "EXPORT...");
        add("save_import", W_BUTTON, 352, 236, 116, 22, "IMPORT...");
        for (int i = nw - 3; i < nw; i++) wd[i].enabled = rom_loaded();   /* no ROM, no save */
        wd[nw - 1].on = cfg.mute;
        add("keyboard", W_BUTTON, 84, 262, 140, 22, "KEYBOARD...");
        add("controller", W_BUTTON, 230, 262, 166, 22, "CONTROLLER...");
        add("reset", W_BUTTON, 402, 262, 66, 22, "RESET");
        add("play", W_BUTTON, 80, 290, 180, 20, "PLAY");
        wd[nw - 1].primary = 1;
        wd[nw - 1].enabled = st.state != 1;       /* disabled while the fast mode is being prepared */
        add("quit", W_BUTTON, 280, 290, 180, 20, "QUIT");
    }
    focus = 0;
    for (int i = 0; i < nw; i++) if (fid[0] && !strcmp(wd[i].id, fid)) focus = i;
    if (want_focus[0]) {
        for (int i = 0; i < nw; i++) if (!strcmp(wd[i].id, want_focus)) focus = i;
        want_focus[0] = 0;
    }
}

static int find(const char *id) {
    for (int i = 0; i < nw; i++) if (!strcmp(wd[i].id, id)) return i;
    return -1;
}

/* ------------------------------------------------------------------ settings */
static void save_cfg(void) { cfg_save(ini, &cfg); }

static void set_note(const char *s, int err) {
    snprintf(note, sizeof note, "%s", s);
    note_until = SDL_GetTicks() + 3000;
    note_err = err;
}

static int *slider_ref(int kind) { return kind == 1 ? &cfg.deadzone : kind == 2 ? &cfg.link_delay : &cfg.volume; }
static int slider_min(int kind) { return kind == 2 ? -1 : 0; }           /* link delay: -1 = auto */
/* link delay: the game tolerates K<=2 with 2 players and K<=1 with 3-4 (NOTES 'Cabo link P2c') */
static int slider_max(int kind) { return kind == 1 ? 32767 : kind == 2 ? (cfg.link_players >= 3 ? 1 : 2) : 100; }
static int slider_step(int kind) { return kind == 1 ? 1000 : kind == 2 ? 1 : 5; }

static void set_slider(int kind, int lx, const Widget *k) {
    int span = k->r.w > 0 ? k->r.w : 1, lo = slider_min(kind), hi = slider_max(kind);
    int v = lo + ((lx - k->r.x) * (hi - lo) + span / 2) / span;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int *ref = slider_ref(kind);
    if (v != *ref) { *ref = v; save_cfg(); }
}

static void bump_slider(const Widget *k, int dir) {
    int *ref = slider_ref(k->slider);
    int v = *ref + dir * slider_step(k->slider);
    if (v < slider_min(k->slider)) v = slider_min(k->slider);
    if (v > slider_max(k->slider)) v = slider_max(k->slider);
    if (v != *ref) { *ref = v; save_cfg(); }
}

static const char *const BTN_NAME[CFG_BTN_COUNT] = { "A", "B", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT" };

static char *binding(int btn) { return page == 1 ? cfg.key[btn] : cfg.pad[btn]; }

static void start_capture(int btn) {
    if (page == 2 && !pad_present()) { set_note("NO CONTROLLER CONNECTED", 1); return; }
    capture = btn;
    note[0] = 0;
}

/* assign a key/button name to a GBA button; a name already used elsewhere is swapped */
static void assign(int btn, const char *name) {
    int other = -1;
    for (int i = 0; i < CFG_BTN_COUNT; i++) if (i != btn && ieq(binding(i), name)) other = i;
    char old[CFG_NAME_MAX];
    snprintf(old, sizeof old, "%s", binding(btn));
    if (other >= 0) snprintf(binding(other), CFG_NAME_MAX, "%s", old);
    snprintf(binding(btn), CFG_NAME_MAX, "%s", name);
    save_cfg();
    capture = -1;
    char m[96];
    if (other >= 0) snprintf(m, sizeof m, "%s = %s  (SWAPPED WITH %s)", BTN_NAME[btn], name, BTN_NAME[other]);
    else snprintf(m, sizeof m, "%s = %s", BTN_NAME[btn], name);
    set_note(m, 0);
}

static void capture_key(SDL_Keycode sym) {
    if (sym == SDLK_ESCAPE) { capture = -1; set_note("CANCELLED", 0); return; }
    const char *name = SDL_GetKeyName(sym);
    if (!name || !*name || strlen(name) >= CFG_NAME_MAX || strpbrk(name, "=;#[]")) {
        set_note("THAT KEY CANNOT BE USED - PRESS ANOTHER", 1);
        return;
    }
    if (cfg_key_reserved(name)) {
        char m[96];
        snprintf(m, sizeof m, "%s IS RESERVED - PRESS ANOTHER KEY", name);
        set_note(m, 1);
        return;
    }
    assign(capture, name);
}

static void capture_button(int sdl_button) {
    const char *name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)sdl_button);
    if (!name || !*name) { set_note("UNKNOWN BUTTON - PRESS ANOTHER", 1); return; }
    assign(capture, name);
}

/* ------------------------------------------------------------------ ONLINE page: fields and keypad */
static void sync_fields(void) {
    snprintf(fbuf[0], sizeof fbuf[0], "%s", cfg.link_name);
    snprintf(fbuf[1], sizeof fbuf[1], "%s", cfg.link_host);
    snprintf(fbuf[2], sizeof fbuf[2], "%d", cfg.link_port);
}

static void commit_fields(void) {
    snprintf(cfg.link_name, sizeof cfg.link_name, "%.15s", fbuf[0][0] ? fbuf[0] : "Player");
    if (fbuf[1][0]) snprintf(cfg.link_host, sizeof cfg.link_host, "%s", fbuf[1]);
    if (port_ok()) cfg.link_port = atoi(fbuf[2]);
    save_cfg();
}

static int field_char_ok(int f, int c) {
    if (c < 32 || c > 126) return 0;
    if (f == 0) return isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.';
    if (f == 1) return isalnum(c) || c == '.' || c == '-' || c == ':' || c == '_';
    return isdigit(c);
}

static void field_append(int f, int c) {
    size_t n = strlen(fbuf[f]);
    if ((int)n >= fcap[f] || !field_char_ok(f, c)) return;
    fbuf[f][n] = (char)c;
    fbuf[f][n + 1] = 0;
}

static void field_backspace(int f) { size_t n = strlen(fbuf[f]); if (n) fbuf[f][n - 1] = 0; }
static void begin_edit(int f) { editing = f; SDL_StartTextInput(); }
static void end_edit(void) {
    if (editing < 0) return;
    commit_fields();
    SDL_StopTextInput();
    editing = -1;
}

static void field_paste(int f) {
    char *t = SDL_GetClipboardText();
    if (!t) return;
    for (const char *p = t; *p; p++) field_append(f, (unsigned char)*p);
    SDL_free(t);
}

static void field_key(SDL_Keycode k, int shift) {                 /* a key while a field is being edited */
    if (k == SDLK_BACKSPACE) field_backspace(editing);
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) end_edit();
    else if (k == SDLK_TAB) { end_edit(); focus_move(shift ? -1 : 1); }
    else if (k == SDLK_UP || k == SDLK_DOWN) { end_edit(); nav(k == SDLK_UP ? 0 : 1); }
    else if (k == SDLK_v && (cur_mod & KMOD_CTRL)) field_paste(editing);
}

static void field_activate(int f) {
    end_edit();
    if (via_pad) {                                                  /* controller: numbers on screen */
        if (f == 0) { set_note("TYPE THE NAME ON A KEYBOARD", 1); return; }
        keypad_field = f;
        snprintf(kp_backup, sizeof kp_backup, "%s", fbuf[f]);
        snprintf(want_focus, sizeof want_focus, "kp_5");
        return;
    }
    begin_edit(f);
}

static void keypad_close(int keep) {
    int f = keypad_field;
    if (f < 0) return;
    if (keep) commit_fields(); else snprintf(fbuf[f], sizeof fbuf[f], "%s", kp_backup);
    keypad_field = -1;
    snprintf(want_focus, sizeof want_focus, "%s", f == 1 ? "online_addr" : "online_port");
}

/* returns 1 when `id` was an ONLINE / keypad control */
static int online_activate(const char *id) {
    if (!strcmp(id, "online")) { page = KL_PAGE_ONLINE; capture = -1; focus = 0; sync_fields(); return 1; }
    if (!strcmp(id, "online_back")) { end_edit(); commit_fields(); page = 0; focus = 0; return 1; }
    if (!strncmp(id, "online_p", 8) && id[8] >= '2' && id[8] <= '4') {
        cfg.link_players = id[8] - '0';
        if (cfg.link_delay > slider_max(2)) cfg.link_delay = slider_max(2);
        save_cfg(); return 1;
    }
    if (!strcmp(id, "online_host") || !strcmp(id, "online_join")) {
        int join = !strcmp(id, "online_join");
        end_edit();
        commit_fields();
        if (!port_ok()) { set_note("PORT MUST BE 1-65535", 1); return 1; }
        if (join && !fbuf[1][0]) { set_note("TYPE THE HOST ADDRESS TO JOIN", 1); return 1; }
        snprintf(cfg.link_mode, sizeof cfg.link_mode, "%s", join ? "join" : "host");
        save_cfg();
        result = join ? KL_ONLINE_JOIN : KL_ONLINE_HOST; running = 0;
        return 1;
    }
    if (!strcmp(id, "online_start")) { end_edit(); result = KL_ONLINE_START; running = 0; return 1; }
    if (!strcmp(id, "online_leave")) {
        end_edit();
        snprintf(cfg.link_mode, sizeof cfg.link_mode, "off");
        save_cfg();
        result = KL_ONLINE_LEAVE; running = 0;
        return 1;
    }
    if (!strcmp(id, "online_copy")) {
        if (onl.my_address[0]) { SDL_SetClipboardText(onl.my_address); set_note("ADDRESS COPIED", 0); }
        return 1;
    }
    if (!strncmp(id, "kp_", 3) && keypad_field >= 0) {
        int f = keypad_field;
        if (id[3] >= '0' && id[3] <= '9' && !id[4]) field_append(f, id[3]);
        else if (!strcmp(id, "kp_dot")) field_append(f, '.');
        else if (!strcmp(id, "kp_del")) field_backspace(f);
        else if (!strcmp(id, "kp_clr")) fbuf[f][0] = 0;
        else if (!strcmp(id, "kp_ok")) keypad_close(1);
        else if (!strcmp(id, "kp_cancel")) keypad_close(0);
        return 1;
    }
    return 0;
}

static void activate(int i) {
    if (i < 0 || i >= nw || !wd[i].enabled) return;
    const char *id = wd[i].id;
    if (wd[i].type == W_SHAPE) {
        start_capture(wd[i].btn);
        return;
    }
    if (wd[i].type == W_FIELD) { field_activate(wd[i].fld); layout(); return; }
    if (online_activate(id)) { layout(); return; }
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
    } else if (!strcmp(id, "stick")) {
        cfg.stick_as_dpad = !cfg.stick_as_dpad; save_cfg();
    } else if (!strcmp(id, "keyboard") || !strcmp(id, "tab_keyboard")) {
        page = 1; capture = -1; focus = 0;
    } else if (!strcmp(id, "controller") || !strcmp(id, "tab_controller")) {
        page = 2; capture = -1; focus = 0;
    } else if (!strcmp(id, "back")) {
        page = 0; capture = -1; focus = 0;
    } else if (!strcmp(id, "reset")) {
        if (page == 1) { cfg_reset_keyboard(&cfg); set_note("KEYBOARD RESET TO DEFAULTS", 0); }
        else if (page == 2) { cfg_reset_controller(&cfg); set_note("CONTROLLER RESET TO DEFAULTS", 0); }
        else { cfg_reset_keyboard(&cfg); cfg_reset_controller(&cfg); set_note("Input settings reset to defaults", 0); }
        save_cfg();
        capture = -1;
    } else if (!strcmp(id, "save_open")) {
        result = KL_OPEN_SAVE_DIR; running = 0;
    } else if (!strcmp(id, "save_export")) {
        result = KL_EXPORT_SAVE; running = 0;
    } else if (!strcmp(id, "save_import")) {
        confirm = 1; layout(); focus = 1;           /* focus NO: the safe answer */
        return;
    } else if (!strcmp(id, "confirm_yes")) {
        confirm = 0; result = KL_IMPORT_SAVE; running = 0;
    } else if (!strcmp(id, "confirm_no")) {
        confirm = 0; layout(); focus = 0;
        return;
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

/* spatial navigation (edit pages): nearest enabled widget in the given direction */
static void focus_dir(int dx, int dy) {
    if (nw == 0) return;
    int cx = wd[focus].r.x + wd[focus].r.w / 2, cy = wd[focus].r.y + wd[focus].r.h / 2;
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < nw; i++) {
        if (i == focus || !wd[i].enabled) continue;
        int x = wd[i].r.x + wd[i].r.w / 2, y = wd[i].r.y + wd[i].r.h / 2;
        int ddx = x - cx, ddy = y - cy;
        int along = dx ? ddx * dx : ddy * dy;
        int across = dx ? ddy : ddx;
        if (along <= 0) continue;
        if (across < 0) across = -across;
        int score = along + 2 * across;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) focus = best;
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

/* ------------------------------------------------------------------ rendering */
static void draw_binding_label(const Widget *k) {
    char s[CFG_NAME_MAX + 4];
    int inside = k->btn == CFG_BTN_L || k->btn == CFG_BTN_R || k->btn == CFG_BTN_SELECT || k->btn == CFG_BTN_START;
    clip(binding(k->btn), inside ? k->r.w - 6 : 60, s, sizeof s);
    int w = text_w(s, 1), cx = k->r.x + k->r.w / 2, cy = k->r.y + k->r.h / 2;
    SDL_Color c = capture == k->btn ? C_BG : C_ACC;
    switch (k->btn) {
    case CFG_BTN_L: case CFG_BTN_R: case CFG_BTN_SELECT: case CFG_BTN_START:
        text(s, cx - w / 2, cy - 3, 1, capture == k->btn ? C_BG : C_TEXT);
        text(BTN_NAME[k->btn], k->r.x + (k->btn == CFG_BTN_L || k->btn == CFG_BTN_R ? 4 : 3), k->r.y - 9, 1, C_DIM);
        break;
    case CFG_BTN_UP:    text(s, cx - w / 2, k->r.y - 10, 1, C_ACC); break;
    case CFG_BTN_DOWN:  text(s, cx - w / 2, k->r.y + k->r.h + 4, 1, C_ACC); break;
    case CFG_BTN_LEFT:  text(s, k->r.x - 2 - w, cy - 3, 1, C_ACC); break;
    case CFG_BTN_RIGHT: text(s, k->r.x + k->r.w + 4, cy - 3, 1, C_ACC); break;
    case CFG_BTN_A: case CFG_BTN_B:
        text(BTN_NAME[k->btn], cx - 3, cy - 3, 1, capture == k->btn ? C_BG : C_TEXT);
        text(s, cx - w / 2, k->r.y + k->r.h + 4, 1, c);
        break;
    default: break;
    }
}

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
        int v = *slider_ref(k->slider), vmin = slider_min(k->slider), vmax = slider_max(k->slider);
        int px = (int)((long long)k->r.w * (v - vmin) / (vmax - vmin));
        SDL_Rect track = { k->r.x, k->r.y + 8, k->r.w, 6 };
        fill(track, C_PANEL);
        frame(track, k->enabled ? C_DIM : (SDL_Color){ 70, 70, 90, 255 });
        SDL_Rect fl = { k->r.x, k->r.y + 8, px, 6 };
        fill(fl, k->enabled ? C_ACC : C_DIM);
        SDL_Rect knob = { k->r.x + px - 4, k->r.y + 2, 8, 18 };
        fill(knob, k->enabled ? C_TEXT : C_DIM);
        char sv[16];
        if (k->slider == 2 && v < 0) snprintf(sv, sizeof sv, "AUTO");
        else if (k->slider == 1 || k->slider == 2) snprintf(sv, sizeof sv, "%d", v);
        else snprintf(sv, sizeof sv, "%d%%", v);
        text(sv, k->r.x + k->r.w + 12, k->r.y + 4, 2, !k->enabled || (!k->slider && cfg.mute) ? C_DIM : C_TEXT);
        break;
    }
    case W_FIELD: {
        int ed = editing == k->fld;
        fill(k->r, ed ? (SDL_Color){ 10, 10, 24, 255 } : C_PANEL);
        frame(k->r, ed ? C_ACC : (k->enabled ? C_TEXT : C_DIM));
        const char *b = fbuf[k->fld];
        int maxc = (k->r.w - 8) / 12 - 1;                       /* one cell is kept for the caret */
        int n = (int)strlen(b);
        char line[64];
        snprintf(line, sizeof line, "%s%s", n > maxc ? b + n - maxc : b, ed && (SDL_GetTicks() / 400) % 2 == 0 ? "_" : "");
        text(line, k->r.x + 4, k->r.y + 4, 2, !k->enabled ? C_DIM : (k->fld == 2 && !port_ok()) ? C_ERR : C_TEXT);
        break;
    }
    case W_SHAPE: {
        int active = capture == k->btn;
        SDL_Color bg = active ? C_ACC : (is_hover ? (SDL_Color){ 90, 90, 140, 255 } : (SDL_Color){ 70, 70, 112, 255 });
        if (k->shape == SH_CIRCLE) {
            int cx = k->r.x + k->r.w / 2, cy = k->r.y + k->r.h / 2, r = k->r.w / 2;
            fill_circle(cx, cy, r, bg);
            ring(cx, cy, r, C_TEXT);
        } else {
            fill(k->r, bg);
            frame(k->r, C_TEXT);
        }
        draw_binding_label(k);
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
        if (n > 22) snprintf(name, sizeof name, "...%.19s", b + n - 19); else snprintf(name, sizeof name, "%.22s", b);
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
    SDL_Rect panel = { 12, 58, 456, 42 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    text("STATUS", 20, 61, 1, C_DIM);
    text(state_text(), 20, 70, 2, st.state == 4 ? C_ERR : C_TEXT);
    if (st.text[0]) {                         /* clipped so it never runs under the progress bar */
        char t[40];
        clip(st.text, 170, t, sizeof t);
        text(t, 20, 88, 1, C_DIM);
    }
    bar_drawn = 0;
    if (st.state == 1 || st.state == 2) {
        SDL_Rect bar = { 200, 87, 260, 9 };
        frame(bar, C_TEXT);
        int p = st.percent < 0 ? 0 : st.percent > 100 ? 100 : st.percent;
        SDL_Rect f = { 201, 88, 258 * p / 100, 7 };
        if (f.w > 0) fill(f, C_ACC);
        bar_rect = bar;
        bar_drawn = 1;
    }
    /* groups and row labels */
    text("VIDEO", 12, 110, 2, C_ACC);
    text("WINDOW SIZE", 84, 110, 2, cfg.fullscreen ? C_DIM : C_TEXT);
    text("SCALING", 84, 162, 2, C_TEXT);
    text("AUDIO", 12, 188, 2, C_ACC);
    text("VOLUME", 84, 188, 2, cfg.mute ? C_DIM : C_TEXT);
    text("SAVE", 12, 240, 2, rom_loaded() ? C_ACC : C_DIM);
    text("INPUT", 12, 266, 2, C_ACC);
    /* footer */
    if (note[0] && SDL_GetTicks() < note_until) text(note, 12, 313, 1, note_err ? C_ERR : C_OK);
    else text("TAB/ARROWS: MOVE   ENTER/SPACE: SELECT   ESC: QUIT", 12, 313, 1, C_DIM);
}

/* live view of the left stick: ring = dead zone, dot = current position */
static void draw_stick_indicator(void) {
    const int cx = 414, cy = 276, R = 34;
    ring(cx, cy, R, C_DIM);
    if (!pad_present()) {
        text("NO CONTROLLER", cx - text_w("NO CONTROLLER", 1) / 2, 314, 1, C_DIM);
        return;
    }
    int ax, ay;
    pad_axes(&ax, &ay);
    int dzr = (int)((long long)R * cfg.deadzone / 32767);
    ring(cx, cy, dzr, C_ACC);
    int px = (int)((long long)ax * R / 32767), py = (int)((long long)ay * R / 32767);
    long long mag2 = (long long)ax * ax + (long long)ay * ay;
    int outside = mag2 > (long long)cfg.deadzone * cfg.deadzone;
    SDL_Rect dot = { cx + px - 2, cy + py - 2, 5, 5 };
    fill(dot, outside ? C_OK : C_TEXT);
    text("LEFT STICK", cx - text_w("LEFT STICK", 1) / 2, 314, 1, C_DIM);
}

static void draw_edit(void) {
    /* GBA outline (own vector art): body, shoulders, screen, d-pad cross, A/B, select/start */
    SDL_Rect body = { 40, 62, 400, 150 };
    round_rect((SDL_Rect){ 62, 44, 96, 20 }, 6, C_BODY);          /* L shoulder base */
    round_rect((SDL_Rect){ 322, 44, 96, 20 }, 6, C_BODY);         /* R shoulder base */
    round_rect(body, 26, C_PANEL);
    round_rect((SDL_Rect){ body.x + 2, body.y + 2, body.w - 4, body.h - 4 }, 24, C_BODY);
    SDL_Rect scr = { 186, 76, 108, 100 };
    fill(scr, C_SCREEN);
    frame(scr, C_DIM);
    text("GBA", 240 - text_w("GBA", 2) / 2, 118, 2, C_DIM);
    SDL_Rect cross = { 88, 128, 24, 24 };                        /* d-pad centre */
    fill(cross, (SDL_Color){ 70, 70, 112, 255 });
    /* prompt / hints */
    char s[96];
    if (capture >= 0) {
        snprintf(s, sizeof s, page == 1 ? "PRESS A KEY FOR %s..." : "PRESS A BUTTON FOR %s...", BTN_NAME[capture]);
        text(s, 12, 222, 2, C_FOCUS);
        text("ESC CANCELS", 12, 241, 1, C_DIM);
    } else {
        text("CLICK A BUTTON TO REMAP IT", 12, 222, 2, C_TEXT);
    }
    if (note[0] && SDL_GetTicks() < note_until) text(note, 12, 241 + (capture >= 0 ? 12 : 0), 1, note_err ? C_ERR : C_OK);
    if (page == 1) text("RESERVED: ESC, F11, F1   KEYS ARE BOUND BY POSITION (A = X, B = Z)", 12, 268, 1, C_DIM);
    if (page == 2) {
        draw_stick_indicator();
        text("DEADZONE", 12, 291, 2, C_TEXT);
    }
    text("ARROWS: MOVE  ENTER: REMAP  ESC: BACK  RESET: THIS TAB", 12, 313, 1, C_DIM);
}

static const char *slot_state_text(int st_) {
    switch (st_) {
    case KLS_CONNECTING: return "JOINING";
    case KLS_CONNECTED: return "CONNECTED";
    case KLS_READY: return "READY";
    case KLS_LOST: return "LOST";
    default: return "EMPTY";
    }
}

static const char *online_state_text(void) {
    static char buf[48];
    switch (onl.state) {
    case KLO_HOSTING: return "WAITING FOR PLAYERS...";
    case KLO_CONNECTING: return "CONNECTING...";
    case KLO_CONNECTED: snprintf(buf, sizeof buf, "CONNECTED (%d PLAYERS)", slots_up() > 0 ? slots_up() : 2); return buf;
    case KLO_PLAYING: return "IN GAME";
    case KLO_DESYNC: return "DESYNC DETECTED";
    case KLO_LOST: return "CONNECTION LOST";
    default: return "NOT CONNECTED";
    }
}

static void draw_online(void) {
    int off = !online_busy();
    SDL_Color lab = off ? C_TEXT : C_DIM;
    text("ONLINE (CABLE LINK)", 12, 10, 2, C_ACC);
    text("NAME", 12, 38, 2, lab);
    text("PLAYERS", 284, 38, 2, lab);
    text("ADDRESS", 12, 66, 2, lab);
    text("PORT", 12, 94, 2, lab);
    text("DELAY", 170, 94, 2, lab);
    if (onl.my_address[0]) {
        char a[48];
        text("YOUR ADDRESS", 12, 150, 1, C_DIM);
        clip(onl.my_address, 290, a, sizeof a);
        text(a, 96, 150, 1, C_OK);
    }
    /* slots */
    SDL_Rect panel = { 12, 170, 456, 92 };
    fill(panel, C_PANEL);
    frame(panel, C_DIM);
    text("SLOT", 20, 173, 1, C_DIM);
    text("NAME", 112, 173, 1, C_DIM);
    text("STATE", 264, 173, 1, C_DIM);
    text("PING", 440, 173, 1, C_DIM);
    for (int i = 0; i < 4; i++) {
        int y = 186 + i * 18, sst = onl.slots[i];
        SDL_Color c = sst == KLS_EMPTY ? C_DIM : sst == KLS_LOST ? C_ERR : (i == onl.self_slot ? C_ACC : C_TEXT);
        char nm[16];
        text(i == 0 ? "P1 HOST" : (const char *[]){ "", "P2", "P3", "P4" }[i], 20, y, 2, c);
        snprintf(nm, sizeof nm, "%.12s", onl.names[i][0] ? onl.names[i] : (sst == KLS_EMPTY ? "-" : "?"));
        text(nm, 112, y, 2, c);
        text(slot_state_text(sst), 264, y, 2, sst == KLS_READY ? C_OK : c);
        if ((sst == KLS_CONNECTED || sst == KLS_READY) && onl.ping_ms[i] >= 0) {
            char pg[16];
            snprintf(pg, sizeof pg, "%d MS", onl.ping_ms[i] > 999 ? 999 : onl.ping_ms[i]);
            SDL_Color pc = onl.ping_ms[i] < 60 ? C_OK : onl.ping_ms[i] < 120 ? C_FOCUS : C_ERR;
            text(pg, 468 - text_w(pg, 2), y, 2, pc);
        }
    }
    /* state line and hint */
    char sl[64];
    snprintf(sl, sizeof sl, "STATE: %s", online_state_text());
    text(sl, 12, 268, 2, onl.state == KLO_DESYNC || onl.state == KLO_LOST ? C_ERR : (onl.state >= KLO_CONNECTED ? C_OK : C_TEXT));
    {
        char hint[96] = "";
        SDL_Color hc = C_DIM;
        if (onl.message[0]) { snprintf(hint, sizeof hint, "%s", onl.message); hc = C_ACC; }
        else if (off && !port_ok()) { snprintf(hint, sizeof hint, "PORT MUST BE 1-65535"); hc = C_ERR; }
        else if (off && !fbuf[1][0]) snprintf(hint, sizeof hint, "TYPE THE HOST ADDRESS TO JOIN, OR PRESS HOST");
        else if (onl.state == KLO_HOSTING && slots_up() < 2) snprintf(hint, sizeof hint, "START NEEDS AT LEAST 2 PLAYERS");
        char h2[96];
        clip(hint, 450, h2, sizeof h2);
        if (h2[0]) text(h2, 12, 286, 1, hc);
    }
    if (note[0] && SDL_GetTicks() < note_until) { char n2[96]; clip(note, 450, n2, sizeof n2); text(n2, 12, 298, 1, note_err ? C_ERR : C_OK); }
    text("TAB/ARROWS: MOVE   ENTER: EDIT/SELECT   ESC: BACK", 12, 313, 1, C_DIM);
}

static void dim_screen(void) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 190);
    SDL_Rect all = { 0, 0, KL_W, KL_H };
    SDL_RenderFillRect(ren, &all);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

static void draw_keypad(void) {
    dim_screen();
    SDL_Rect box = { 96, 70, 288, 188 };
    fill(box, C_PANEL);
    frame(box, C_ACC);
    char l[64];
    snprintf(l, sizeof l, "%s", keypad_field == 1 ? "ADDRESS" : "PORT");
    text(l, 108, 78, 1, C_DIM);
    const char *b = fbuf[keypad_field];
    int n = (int)strlen(b);
    snprintf(l, sizeof l, "%s_", n > 20 ? b + n - 20 : b);
    text(l, 108, 90, 2, C_TEXT);
}

static void draw_confirm(void) {                 /* modal dialog over a dimmed screen */
    dim_screen();
    SDL_Rect box = { 70, 104, 340, 110 };
    fill(box, C_PANEL);
    frame(box, C_ACC);
    text_center("OVERWRITE CURRENT SAVE?", (SDL_Rect){ 70, 118, 340, 14 }, 2, C_TEXT);
    text_center("A BACKUP IS KEPT", (SDL_Rect){ 70, 142, 340, 14 }, 2, C_OK);
}

static void draw_canvas(void) {
    ntexts = 0; bar_drawn = 0;
    setc(C_BG);
    SDL_RenderClear(ren);
    rec_texts = 1;
    if (page == KL_PAGE_ONLINE) draw_online(); else if (page) draw_edit(); else draw_main();
    rec_texts = 0;      /* texts inside widgets are covered by the widget rectangles */
    if (confirm) draw_confirm();
    if (keypad_field >= 0) draw_keypad();
    for (int i = 0; i < nw; i++) draw_widget(i);
}

static void render(void) {
    SDL_SetRenderTarget(ren, canvas);
    draw_canvas();
    SDL_SetRenderTarget(ren, NULL);
    setc(C_BG);
    SDL_RenderClear(ren);                     /* the letterbox bars */
    compute_dst();
    SDL_RenderCopy(ren, canvas, NULL, &dst);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ events */
/* window coordinates (as in SDL mouse events) -> canvas coordinates */
static void to_logical(int wx, int wy, int *lx, int *ly) {
    int ww = 1, wh = 1, ow = 1, oh = 1;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    int px = ww > 0 ? wx * ow / ww : wx, py = wh > 0 ? wy * oh / wh : wy;     /* high-DPI: window -> pixels */
    compute_dst();
    *lx = dst.w > 0 ? (px - dst.x) * KL_W / dst.w : px;
    *ly = dst.h > 0 ? (py - dst.y) * KL_H / dst.h : py;
}

/* directions: 0 up, 1 down, 2 left, 3 right (keyboard arrows, d-pad and stick all end here) */
static void nav(int dir) {
    if (nw == 0) return;
    Widget *f = &wd[focus];
    if (page) {
        if ((dir == 2 || dir == 3) && f->type == W_SLIDER) { bump_slider(f, dir == 3 ? 1 : -1); return; }
        focus_dir(dir == 3 ? 1 : dir == 2 ? -1 : 0, dir == 1 ? 1 : dir == 0 ? -1 : 0);
        return;
    }
    if (dir == 1) { focus_move(1); return; }
    if (dir == 0) { focus_move(-1); return; }
    int d = dir == 3 ? 1 : -1;
    if (f->type == W_SLIDER) { bump_slider(f, d); return; }
    if (f->type == W_RADIO) {
        int ni = focus + d;
        if (ni >= 0 && ni < nw && wd[ni].type == W_RADIO && wd[ni].enabled) { focus = ni; activate(ni); }
        return;
    }
    focus_move(d);
}

static void go_back(void) {
    if (keypad_field >= 0) { keypad_close(0); layout(); return; }
    if (editing >= 0) { end_edit(); return; }
    if (confirm) { confirm = 0; layout(); focus = 0; return; }
    if (page) { page = 0; capture = -1; focus = 0; layout(); }
}

static void on_key(SDL_Keycode k, int shift) {
    if (editing >= 0) { field_key(k, shift); return; }       /* typing in a field */
    if (capture >= 0) {                          /* remapping: the next key is the binding */
        if (page == 1) capture_key(k);
        else if (k == SDLK_ESCAPE) { capture = -1; set_note("CANCELLED", 0); }   /* controller pages ignore other keys */
        return;
    }
    if (k == SDLK_ESCAPE) {
        if (keypad_field >= 0 || confirm || page) go_back();
        else { result = KL_QUIT; running = 0; }
        return;
    }
    if (nw == 0) return;
    if (k == SDLK_TAB) { focus_move(shift ? -1 : 1); return; }
    if (k == SDLK_UP) { nav(0); return; }
    if (k == SDLK_DOWN) { nav(1); return; }
    if (k == SDLK_LEFT) { nav(2); return; }
    if (k == SDLK_RIGHT) { nav(3); return; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) activate(focus);
}

static void on_pad_button(int b) {
    if (capture >= 0 && page == 2) { capture_button(b); return; }
    if (keypad_field >= 0 && b == SDL_CONTROLLER_BUTTON_B) { field_backspace(keypad_field); return; }   /* east = DEL on the keypad */
    if (capture >= 0) return;                    /* keyboard capture: ignore the pad */
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: nav(0); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: nav(2); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: nav(3); break;
    case SDL_CONTROLLER_BUTTON_A: if (nw) { via_pad = 1; activate(focus); via_pad = 0; } break;       /* south = OK */
    case SDL_CONTROLLER_BUTTON_B: go_back(); break;                      /* east = back (no-op on the main page) */
    default: break;
    }
}

static void on_pad_axis(int axis, int value) {
    static int dir[2];
    if (axis != SDL_CONTROLLER_AXIS_LEFTX && axis != SDL_CONTROLLER_AXIS_LEFTY) return;
    int i = axis == SDL_CONTROLLER_AXIS_LEFTY;
    int nd = value > 20000 ? 1 : value < -20000 ? -1 : 0;
    if (nd != 0 && nd != dir[i] && capture < 0) nav(i ? (nd > 0 ? 1 : 0) : (nd > 0 ? 3 : 2));
    dir[i] = nd;
}

static int for_us(Uint32 wid) { return wid == 0 || !win || wid == SDL_GetWindowID(win); }

static void handle(SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT:
        result = KL_QUIT; running = 0; break;
    case SDL_KEYDOWN:
        if (!for_us(e->key.windowID)) break;
        cur_mod = e->key.keysym.mod;
        if (!e->key.repeat || e->key.keysym.sym == SDLK_LEFT || e->key.keysym.sym == SDLK_RIGHT || e->key.keysym.sym == SDLK_BACKSPACE)
            on_key(e->key.keysym.sym, (e->key.keysym.mod & KMOD_SHIFT) != 0);
        break;
    case SDL_TEXTINPUT:
        if (!for_us(e->text.windowID)) break;
        if (editing >= 0) for (const char *p = e->text.text; *p; p++) field_append(editing, (unsigned char)*p);
        else if (keypad_field >= 0) for (const char *p = e->text.text; *p; p++) field_append(keypad_field, (unsigned char)*p);
        break;
    case SDL_MOUSEMOTION: {
        if (!for_us(e->motion.windowID)) break;
        int lx, ly;
        to_logical(e->motion.x, e->motion.y, &lx, &ly);
        hover = hit(lx, ly);
        if (dragging) {
            int i = find(dragging == 3 ? "online_delay" : dragging == 2 ? "deadzone" : "volume");
            if (i >= 0) set_slider(dragging - 1, lx, &wd[i]);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (!for_us(e->button.windowID)) break;
        if (e->button.button == SDL_BUTTON_LEFT) {
            int lx, ly;
            to_logical(e->button.x, e->button.y, &lx, &ly);
            if (capture >= 0) { capture = -1; set_note("CANCELLED", 0); }   /* a click elsewhere cancels the capture */
            if (editing >= 0) end_edit();                                  /* ...and ends a text edit (keeps the text) */
            int i = hit(lx, ly);
            if (i >= 0 && wd[i].enabled) {
                focus = i;
                if (wd[i].type == W_SLIDER) { dragging = wd[i].slider + 1; set_slider(wd[i].slider, lx, &wd[i]); }
                else activate(i);
            }
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (!for_us(e->button.windowID)) break;
        if (e->button.button == SDL_BUTTON_LEFT) dragging = 0;
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        on_pad_button(e->cbutton.button);
        break;
    case SDL_CONTROLLERAXISMOTION:
        on_pad_axis(e->caxis.axis, e->caxis.value);
        break;
    case SDL_CONTROLLERDEVICEADDED:
        pad_open_first();
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == e->cdevice.which) {
            SDL_GameControllerClose(pad);
            pad = NULL;
            pad_open_first();
        }
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
KL_API void kl_set_online_poll(KlOnlinePoll poll) { online_poll = poll; }
KL_API void kl_set_start_page(int p) { start_page = p == KL_PAGE_ONLINE ? KL_PAGE_ONLINE : KL_PAGE_MAIN; }

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
KL_API const char *kl_test_page(void) { return page == 0 ? "main" : page == 1 ? "keyboard" : page == 2 ? "controller" : "online"; }
KL_API unsigned kl_test_window_id(void) { return win ? SDL_GetWindowID(win) : 0; }
KL_API int kl_test_capture(void) { return capture; }
KL_API const char *kl_test_note(void) { return note[0] && SDL_GetTicks() < note_until ? note : ""; }
KL_API void kl_test_fake_pad(int present, int ax, int ay) { fake_pad = present; fake_ax = ax; fake_ay = ay; }
KL_API int kl_test_pad_present(void) { return pad_present(); }
KL_API int kl_test_confirm(void) { return confirm; }
KL_API int kl_test_editing(void) { return editing; }
KL_API int kl_test_keypad(void) { return keypad_field; }
KL_API const char *kl_test_field(int f) { return f >= 0 && f < 3 ? fbuf[f] : ""; }
KL_API int kl_test_text_count(void) { return ntexts; }
KL_API int kl_test_text(int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= ntexts) return 0;
    if (x) *x = texts[i].x;
    if (y) *y = texts[i].y;
    if (w) *w = texts[i].w;
    if (h) *h = texts[i].h;
    return 1;
}
KL_API int kl_test_bar(int *x, int *y, int *w, int *h) {
    if (!bar_drawn) return 0;
    if (x) *x = bar_rect.x;
    if (y) *y = bar_rect.y;
    if (w) *w = bar_rect.w;
    if (h) *h = bar_rect.h;
    return 1;
}

/* The last frame of the canvas, exactly KL_W x KL_H (read back from the canvas texture). */
KL_API int kl_test_snapshot(uint32_t *out, int cap, int *w, int *h) {
    if (!ren || !canvas || cap < KL_W * KL_H) return -1;
    if (w) *w = KL_W;
    if (h) *h = KL_H;
    SDL_Texture *old = SDL_GetRenderTarget(ren);
    SDL_SetRenderTarget(ren, canvas);
    int rc = SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, out, KL_W * 4);
    SDL_SetRenderTarget(ren, old);
    return rc;
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
    sync_fields();
    memset(&st, 0, sizeof st);
    dropped[0] = 0; note[0] = 0; page = start_page; focus = 0; hover = -1; dragging = 0; nw = 0; capture = -1; confirm = 0;
    editing = -1; keypad_field = -1; via_pad = 0; want_focus[0] = 0; memset(&onl, 0, sizeof onl);
    fake_pad = -1; pad = NULL;
    result = KL_QUIT; running = 1;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "kirby_ui: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return KL_QUIT;
    }
    int have_pad_sys = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;   /* optional: keyboard/mouse still work */
    win = SDL_CreateWindow(WIN_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, want_w(), want_h(),
                           SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "kirby_ui: SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (have_pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
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
        if (have_pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return KL_QUIT;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, KL_W, KL_H);
    if (!canvas) {
        fprintf(stderr, "kirby_ui: SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren); ren = NULL;
        SDL_DestroyWindow(win); win = NULL;
        if (have_pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return KL_QUIT;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    if (have_pad_sys) pad_open_first();
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
            if (online_poll) {
                KlOnline no;
                memset(&no, 0, sizeof no);
                for (int q = 0; q < 4; q++) no.ping_ms[q] = -1;
                no.self_slot = -1;
                online_poll(&no);
                no.my_address[sizeof no.my_address - 1] = 0;
                no.message[sizeof no.message - 1] = 0;
                for (int q = 0; q < 4; q++) no.names[q][sizeof no.names[q] - 1] = 0;
                onl = no;
            }
            layout();
            next_poll = SDL_GetTicks() + (Uint32)poll_ms;
        }
        if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) render();
    }
    end_edit();
    commit_fields();
    if (pad) { SDL_GameControllerClose(pad); pad = NULL; }
    SDL_DestroyTexture(canvas); canvas = NULL;
    SDL_DestroyRenderer(ren); ren = NULL;
    SDL_DestroyWindow(win); win = NULL;
    if (have_pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result;
}
