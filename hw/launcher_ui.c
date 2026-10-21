/* launcher_ui.c - see launcher_ui.h. Pixel-art immediate-mode UI on SDL2 (v0.8 design: dark navy cards, pink accent).
 * Logical canvas 960x600; the window follows the video options (see launcher_ui.h). Text uses the baked Pixelify Sans /
 * Silkscreen bitmaps of ui_font_data.c (accents included).
 * Pages: HOME (status, PLAY, three cards), SETTINGS (video, audio, saves), INPUT (keyboard / controller on a GBA outline)
 * and ONLINE (connection + lobby). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "launcher_ui.h"
#include "config_ini.h"
#include "ui_font.h"
#include "window_sdl.h"      /* WIN_TITLE, win_icon_pixels */

/* ------------------------------------------------------------------ widgets */
typedef enum { W_BUTTON, W_RADIO, W_CHECK, W_SLIDER, W_SHAPE, W_FIELD, W_ROW, W_NAV } WType;
enum { SH_RECT, SH_CIRCLE };
enum { ST_NORMAL, ST_PRIMARY, ST_GHOST, ST_CHIP, ST_BIG };

typedef struct {
    const char *id;
    WType type;
    SDL_Rect r;
    char label[48];         /* W_NAV: "Title|description" */
    int enabled;
    int on;                 /* radio selected / switch on */
    int style;              /* ST_* (buttons) */
    int seg;                /* W_RADIO: 1 = first of its group, 2 = last (both bits for a lone one) */
    int btn;                /* W_SHAPE: CFG_BTN_* index */
    int shape;              /* W_SHAPE: SH_* */
    int slider;             /* W_SLIDER: 0 = volume, 1 = deadzone, 2 = link delay */
    int fld;                /* W_FIELD: 0 name, 1 address, 2 port; W_ROW: address index */
} Widget;

#define MAX_W 40
static Widget wd[MAX_W];
static int nw;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *canvas;          /* the UI is drawn here at KL_W x KL_H and then scaled to the window by us */
static SDL_Texture *atlas[UF_COUNT]; /* one white glyph atlas per font (tinted with the colour mod) */
static int cell_w[UF_COUNT], cell_h[UF_COUNT];
static SDL_Rect dst;                 /* where the canvas lands in the window output (pixels) */
static KirbyConfig cfg;
static KlStatus st;
static char ini[512];
static char dropped[1024];
static int poll_ms = 100;
static int focus;                    /* index into wd[] */
static int hover = -1;
static int dragging;                 /* slider drag: 0 none, else slider kind + 1 */
enum { PG_MAIN = 0, PG_KEYBOARD = 1, PG_CONTROLLER = 2, PG_ONLINE = 3, PG_SETTINGS = 4 };
static int page;
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
static int addr_sel;                 /* selected entry of the HOSTING address list */
static int role;                     /* ONLINE: 0 host, 1 join (which button the big action button is) */
static int last_delay = 2;           /* delay shown again when AUTO is switched off */
static void layout(void);
static void focus_move(int d);
static void nav(int dir);
static int port_ok(void) { int n = atoi(fbuf[2]); return fbuf[2][0] && n >= 1 && n <= 65535; }
static int online_busy(void) { return onl.state != KLO_OFFLINE; }
static int addr_count(void) {
    int n = 0;
    while (n < 4 && onl.addresses[n][0]) n++;
    return n ? n : (onl.my_address[0] ? 1 : 0);
}
static const char *addr_get(int i) { return onl.addresses[0][0] ? onl.addresses[i] : onl.my_address; }
static int show_addrs(void) { return onl.state != KLO_PLAYING && addr_count() > 0; }
static int self_ready(void) { return onl.self_slot >= 0 && onl.self_slot < 4 && onl.slots[onl.self_slot] == KLS_READY && onl.state != KLO_PLAYING; }
static int all_ready(void) { for (int i = 0; i < 4; i++) if (onl.slots[i] == KLS_CONNECTED) return 0; return 1; }
static int slots_up(void) { int n = 0; for (int i = 0; i < 4; i++) if (onl.slots[i] == KLS_CONNECTED || onl.slots[i] == KLS_READY) n++; return n; }

/* texts and the progress bar drawn on the last frame (layout checks in the tests) */
#define MAX_TEXTS 80
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

#define RGB(r, g, b) { r, g, b, 255 }
static const SDL_Color C_BG = RGB(18, 18, 42), C_CARD = RGB(28, 29, 58), C_CARDB = RGB(46, 48, 96), C_LINE = RGB(38, 40, 79),
                       C_BTN = RGB(36, 37, 74), C_BTNB = RGB(58, 60, 110), C_SHADOW = RGB(9, 9, 23), C_TEXT = RGB(232, 233, 255),
                       C_DIM = RGB(154, 157, 201), C_OFF = RGB(93, 97, 153), C_ACC = RGB(255, 126, 182), C_ACCH = RGB(255, 150, 197),
                       C_ONTXT = RGB(21, 21, 42), C_FOCUS = RGB(255, 210, 63), C_OK = RGB(126, 224, 160),
                       C_OKBG = RGB(23, 58, 52), C_OKB = RGB(42, 106, 85), C_ERR = RGB(255, 107, 107),
                       C_FIELD = RGB(15, 16, 36), C_SEG = RGB(18, 19, 43), C_SEGTXT = RGB(201, 203, 242),
                       C_GBA = RGB(31, 32, 69), C_GBAB = RGB(52, 55, 106), C_GBAIN = RGB(24, 25, 56), C_SCREEN = RGB(12, 12, 28),
                       C_WARN = RGB(255, 210, 63), C_NAVH = RGB(34, 35, 74), C_ERRBG = RGB(70, 30, 40);

/* ------------------------------------------------------------------ drawing */
static void setc(SDL_Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }
static void fill(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderFillRect(ren, &r); }
static void frame(SDL_Rect r, SDL_Color c) { setc(c); SDL_RenderDrawRect(ren, &r); }
static void frame2(SDL_Rect r, SDL_Color c) {            /* 2 px border (the design's 2px solid borders) */
    frame(r, c);
    r.x++; r.y++; r.w -= 2; r.h -= 2;
    if (r.w > 0 && r.h > 0) frame(r, c);
}
static SDL_Rect R(int x, int y, int w, int h) { SDL_Rect r = { x, y, w, h }; return r; }

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

/* ---- text: UTF-8 in, baked bitmap fonts out ---- */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s;
    if (!c) return 0;
    if (c < 0x80) { *p += 1; return c; }
    if (c >= 0xC2 && c <= 0xDF && (s[1] & 0xC0) == 0x80) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 1;                   /* skip the rest of a sequence we do not draw */
    int k = 1;
    while (k < n && (s[k] & 0xC0) == 0x80) k++;
    *p += k;
    return '?';
}
static int utf8_count(const char *s) { int n = 0; while (utf8_next(&s)) n++; return n; }
/* copy at most max_cp characters of s (never half of one) */
static void utf8_cut(char *dst_, size_t cap, const char *s, int max_cp) {
    const char *p = s;
    for (int i = 0; i < max_cp && utf8_next(&p); i++) {}
    size_t n = (size_t)(p - s);
    if (n >= cap) n = cap - 1;
    memcpy(dst_, s, n);
    dst_[n] = 0;
}

static const UiGlyph *glyph_of(int f, int cp, int *idx) {
    int i = ui_glyph_index(cp);
    if (idx) *idx = i;
    return &ui_fonts[f].glyphs[i];
}

static int tw(int f, const char *s) {                    /* width in pixels */
    int w = 0;
    for (int cp; (cp = utf8_next(&s)) != 0;) w += glyph_of(f, cp, NULL)->adv;
    return w;
}
static int cap_h(int f) { return glyph_of(f, 'H', NULL)->h; }

static void fonts_init(void) {
    for (int f = 0; f < UF_COUNT; f++) {
        const UiFont *F = &ui_fonts[f];
        int mw = 1, mh = 1;
        for (int i = 0; i < UI_GLYPHS; i++) {
            if (F->glyphs[i].w > mw) mw = F->glyphs[i].w;
            if (F->glyphs[i].h > mh) mh = F->glyphs[i].h;
        }
        cell_w[f] = mw + 1; cell_h[f] = mh + 1;
        int cols = 16, rows = (UI_GLYPHS + cols - 1) / cols;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, cols * cell_w[f], rows * cell_h[f], 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) { atlas[f] = NULL; continue; }
        SDL_FillRect(s, NULL, 0);
        Uint32 *px = (Uint32 *)s->pixels;
        int pitch = s->pitch / 4;
        for (int i = 0; i < UI_GLYPHS; i++) {
            const UiGlyph *g = &F->glyphs[i];
            int gx = (i % cols) * cell_w[f], gy = (i / cols) * cell_h[f], nb = (g->w + 7) / 8;
            for (int y = 0; y < g->h; y++)
                for (int x = 0; x < g->w; x++)
                    if (F->bits[g->off + y * nb + x / 8] >> (x % 8) & 1) px[(gy + y) * pitch + gx + x] = 0xFFFFFFFFu;
        }
        atlas[f] = SDL_CreateTextureFromSurface(ren, s);
        if (atlas[f]) SDL_SetTextureBlendMode(atlas[f], SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(s);
    }
}
static void fonts_free(void) {
    for (int f = 0; f < UF_COUNT; f++) if (atlas[f]) { SDL_DestroyTexture(atlas[f]); atlas[f] = NULL; }
}

/* y = top of the line box (ascent + descent) */
static void tx(int f, const char *s, int x, int y, SDL_Color c) {
    const UiFont *F = &ui_fonts[f];
    if (rec_texts && ntexts < MAX_TEXTS && *s) {
        SDL_Rect r = { x, y, tw(f, s), F->asc + F->desc };
        texts[ntexts++] = r;
    }
    if (!atlas[f]) return;
    SDL_SetTextureColorMod(atlas[f], c.r, c.g, c.b);
    int base = y + F->asc, cols = 16;
    for (int cp; (cp = utf8_next(&s)) != 0;) {
        int idx;
        const UiGlyph *g = glyph_of(f, cp, &idx);
        if (g->w) {
            SDL_Rect src = { (idx % cols) * cell_w[f], (idx / cols) * cell_h[f], g->w, g->h };
            SDL_Rect d = { x + g->dx, base + g->dy, g->w, g->h };
            SDL_RenderCopy(ren, atlas[f], &src, &d);
        }
        x += g->adv;
    }
}
static void tx_r(int f, const char *s, int xr, int y, SDL_Color c) { tx(f, s, xr - tw(f, s), y, c); }
/* centred in r (both ways: the capitals sit on the middle line) */
static void tx_c(int f, const char *s, SDL_Rect r, SDL_Color c) {
    const UiFont *F = &ui_fonts[f];
    int base = r.y + (r.h + cap_h(f)) / 2;
    tx(f, s, r.x + (r.w - tw(f, s)) / 2, base - F->asc, c);
}
/* vertically centred, left aligned at x */
static void tx_l(int f, const char *s, int x, SDL_Rect r, SDL_Color c) {
    const UiFont *F = &ui_fonts[f];
    int base = r.y + (r.h + cap_h(f)) / 2;
    tx(f, s, x, base - F->asc, c);
}

/* the longest prefix of s that fits max_px (ellipsis dropped: just cut) */
static void fit(int f, const char *s, int max_px, char *out, size_t cap) {
    int n = utf8_count(s);
    for (;; n--) {
        utf8_cut(out, cap, s, n);
        if (n <= 0 || tw(f, out) <= max_px) return;
    }
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
 * place (when they carry the window id), which made clicks miss in windows that are not KL_W x KL_H. */
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
static Widget *add(const char *id, WType t, int x, int y, int w, int h, const char *label) {
    static Widget dummy;
    if (nw >= MAX_W) return &dummy;
    Widget *k = &wd[nw++];
    memset(k, 0, sizeof *k);
    k->id = id; k->type = t; k->r.x = x; k->r.y = y; k->r.w = w; k->r.h = h;
    k->enabled = 1;
    k->btn = -1;
    strncpy(k->label, label, sizeof k->label - 1);
    return k;
}

static Widget *add_btn(const char *id, int x, int y, int w, int h, const char *label, int style) {
    Widget *k = add(id, W_BUTTON, x, y, w, h, label);
    k->style = style;
    return k;
}

/* a segmented group: n radios side by side starting at x */
static Widget *add_seg(const char *id, int x, int y, int w, int h, const char *label, int on, int pos, int n) {
    Widget *k = add(id, W_RADIO, x, y, w, h, label);
    k->on = on;
    k->seg = (pos == 0 ? 1 : 0) | (pos == n - 1 ? 2 : 0);
    return k;
}

static Widget *add_shape(const char *id, int btn, int shape, int x, int y, int w, int h) {
    Widget *k = add(id, W_SHAPE, x, y, w, h, "");
    k->btn = btn;
    k->shape = shape;
    return k;
}

static int rom_missing(void) { return st.state == 0 || !st.rom_ok; }
static int rom_loaded(void) { return st.rom[0] && st.rom_ok; }      /* a verified ROM: the save can be managed */

/* the page chrome shared by SETTINGS / INPUT / ONLINE: Back button in the header */
static void add_back(void) { add_btn("back", 28, 16, 96, 32, "< Back", ST_NORMAL); }

static void layout(void) {
    static const char *const size_ids[6] = { "size1", "size2", "size3", "size4", "size5", "size6" };
    static const char *const size_lbl[6] = { "1x", "2x", "3x", "4x", "5x", "6x" };
    char fid[32] = "";
    if (nw && focus >= 0 && focus < nw) snprintf(fid, sizeof fid, "%s", wd[focus].id);
    nw = 0;
    if (keypad_field >= 0) {                  /* on-screen keypad (controller): digits, '.', DEL, CLR, OK, CANCEL */
        static const char *const kid[10] = { "kp_0", "kp_1", "kp_2", "kp_3", "kp_4", "kp_5", "kp_6", "kp_7", "kp_8", "kp_9" };
        for (int d = 1; d <= 9; d++) {
            char lab[2] = { (char)('0' + d), 0 };
            add_btn(kid[d], 276 + ((d - 1) % 3) * 100, 214 + ((d - 1) / 3) * 56, 92, 48, lab, ST_NORMAL);
        }
        add_btn("kp_del", 576, 214, 108, 48, "Del", ST_NORMAL);
        add_btn("kp_clr", 576, 270, 108, 48, "Clear", ST_NORMAL);
        add_btn("kp_dot", 576, 326, 108, 48, ".", ST_NORMAL)->enabled = keypad_field == 1;   /* a port has no dots */
        add_btn("kp_0", 276, 382, 92, 48, "0", ST_NORMAL);
        add_btn("kp_ok", 376, 382, 192, 48, "OK", ST_PRIMARY);
        add_btn("kp_cancel", 576, 382, 108, 48, "Cancel", ST_NORMAL);
    } else if (confirm) {                            /* modal: only YES / NO are reachable */
        add_btn("confirm_yes", 300, 330, 150, 40, "Yes", ST_NORMAL);
        add_btn("confirm_no", 510, 330, 150, 40, "No", ST_PRIMARY);
    } else if (page == PG_ONLINE) {
        int off = !online_busy();
        add_back();
        /* connection card: x 28..503 */
        add("online_name", W_FIELD, 46, 152, 439, 34, "")->fld = 0;
        wd[nw - 1].enabled = off;
        add("online_addr", W_FIELD, 46, 216, 335, 34, "")->fld = 1;
        wd[nw - 1].enabled = off;
        add("online_port", W_FIELD, 393, 216, 92, 34, "")->fld = 2;
        wd[nw - 1].enabled = off;
        for (int n = 2; n <= 4; n++) {
            static const char *const pid[3] = { "online_p2", "online_p3", "online_p4" };
            char lab[2] = { (char)('0' + n), 0 };
            add_seg(pid[n - 2], 46 + (n - 2) * 42, 282, 42, 30, lab, cfg.link_players == n, n - 2, 3)->enabled = off;
        }
        add("online_delay", W_SLIDER, 290, 290, 195, 22, "")->slider = 2;
        wd[nw - 1].enabled = off && cfg.link_delay >= 0;
        add("online_auto", W_CHECK, 443, 262, 42, 22, "")->on = cfg.link_delay < 0;
        wd[nw - 1].enabled = off;
        {   /* transport */
            const char *t = cfg.link_transport;
            int a = !strcmp(t, "auto"), u = !strcmp(t, "udp"), c = !strcmp(t, "tcp");
            add_seg("online_net_auto", 46, 342, 64, 30, "Auto", a, 0, 3)->enabled = off;
            add_seg("online_net_udp", 110, 342, 56, 30, "UDP", u, 1, 3)->enabled = off;
            add_seg("online_net_tcp", 166, 342, 56, 30, "TCP", c, 2, 3)->enabled = off;
        }
        add_seg("role_host", 46, 408, 56, 32, "Host", role == 0, 0, 2)->enabled = off;
        add_seg("role_join", 102, 408, 56, 32, "Join", role == 1, 1, 2)->enabled = off;
        if (role == 0) {
            add_btn("online_host", 170, 408, 132, 32, "Host game", ST_PRIMARY)->enabled = off && port_ok();
        } else {
            add_btn("online_join", 170, 408, 132, 32, "Join game", ST_PRIMARY)->enabled = off && port_ok() && fbuf[1][0];
        }
        add_btn("online_copy", 419, 408, 66, 32, "Copy", ST_NORMAL)->enabled = show_addrs();
        add_btn("online_ready", 46, 450, 96, 32, self_ready() ? "Cancel" : "Ready", ST_NORMAL)->enabled =
            onl.state == KLO_HOSTING || onl.state == KLO_CONNECTED;
        add_btn("online_start", 150, 450, 96, 32, "Start", ST_NORMAL)->enabled =
            onl.state == KLO_HOSTING && slots_up() >= 2 && all_ready();
        add_btn("online_leave", 254, 450, 96, 32, "Leave", ST_NORMAL)->enabled = online_busy();
        if (show_addrs())
            for (int a = 0; a < addr_count(); a++) {
                static const char *const rid[4] = { "online_ip0", "online_ip1", "online_ip2", "online_ip3" };
                add(rid[a], W_ROW, 537, 436 + a * 20, 377, 20, "")->fld = a;
            }
    } else if (page == PG_SETTINGS) {
        add_back();
        for (int i = 0; i < 6; i++) {
            add_seg(size_ids[i], 290 + i * 40, 122, 40, 28, size_lbl[i], cfg.scale == i + 1, i, 6)->enabled = !cfg.fullscreen;
        }
        add("fullscreen", W_CHECK, 491, 205, 42, 22, "")->on = cfg.fullscreen;
        add_seg("scale_integer", 331, 274, 76, 28, "Integer", cfg.scaling == CFG_SCALING_INTEGER, 0, 3);
        add_seg("scale_fit", 407, 274, 52, 28, "Fit", cfg.scaling == CFG_SCALING_FIT, 1, 3);
        add_seg("scale_stretch", 459, 274, 74, 28, "Stretch", cfg.scaling == CFG_SCALING_STRETCH, 2, 3);
        add("volume", W_SLIDER, 597, 142, 315, 22, "")->slider = 0;
        add("mute", W_CHECK, 870, 188, 42, 22, "")->on = cfg.mute;
        add_btn("save_open", 597, 296, 112, 32, "Open folder", ST_NORMAL)->enabled = rom_loaded();   /* no ROM, no save */
        add_btn("save_export", 717, 296, 90, 32, "Export...", ST_NORMAL)->enabled = rom_loaded();
        add_btn("save_import", 815, 296, 97, 32, "Import...", ST_NORMAL)->enabled = rom_loaded();
        add_btn("done", 832, 556, 100, 32, "Done", ST_PRIMARY);
    } else if (page == PG_KEYBOARD || page == PG_CONTROLLER) {
        add_back();
        {
            const char *rl = page == PG_KEYBOARD ? "Reset keyboard" : "Reset controller";
            int w = tw(UF_B13, rl) + 32;
            add_btn("reset", 932 - w, 16, w, 32, rl, ST_NORMAL);
        }
        add_seg("tab_keyboard", 387, 84, 100, 30, "Keyboard", page == PG_KEYBOARD, 0, 2);
        add_seg("tab_controller", 487, 84, 106, 30, "Controller", page == PG_CONTROLLER, 1, 2);
        const int ox = 160, oy = 124;
        add_shape("btn_l", CFG_BTN_L, SH_RECT, ox + 36, oy + 2, 150, 40)->style = 1;
        add_shape("btn_r", CFG_BTN_R, SH_RECT, ox + 454, oy + 2, 150, 40)->style = 1;
        add_shape("btn_up", CFG_BTN_UP, SH_RECT, ox + 90, oy + 72, 56, 40);
        add_shape("btn_left", CFG_BTN_LEFT, SH_RECT, ox + 28, oy + 118, 56, 40);
        add_shape("btn_right", CFG_BTN_RIGHT, SH_RECT, ox + 152, oy + 118, 56, 40);
        add_shape("btn_down", CFG_BTN_DOWN, SH_RECT, ox + 90, oy + 164, 56, 40);
        add_shape("btn_select", CFG_BTN_SELECT, SH_RECT, ox + 228, oy + 216, 88, 40);
        add_shape("btn_start", CFG_BTN_START, SH_RECT, ox + 324, oy + 216, 88, 40);
        add_shape("btn_b", CFG_BTN_B, SH_CIRCLE, ox + 468, oy + 140, 58, 58);
        add_shape("btn_a", CFG_BTN_A, SH_CIRCLE, ox + 538, oy + 100, 58, 58);
        if (page == PG_CONTROLLER) {
            add("stick", W_CHECK, 28, 502, 42, 22, "")->on = cfg.stick_as_dpad;
            add("deadzone", W_SLIDER, 398, 502, 210, 22, "")->slider = 1;
        }
        add_btn("done", 832, 556, 100, 32, "Done", ST_PRIMARY);
    } else {
        add_btn("play", 350, 358, 260, 46, rom_missing() ? "SELECT ROM" : "PLAY", ST_BIG)->style = ST_BIG;
        wd[nw - 1].enabled = st.state != 1;       /* disabled while the fast mode is being prepared */
        if (rom_missing()) add_btn("selectrom", 700, 16, 232, 26, "No ROM - select one...", ST_CHIP);
        add("settings", W_NAV, 220, 432, 167, 62, "Settings|Video, audio, saves");
        add("input", W_NAV, 397, 432, 167, 62, "Input|Keyboard and controller");
        add("online", W_NAV, 574, 432, 166, 62, "Online|Cable link multiplayer")->enabled = rom_loaded();
        add_btn("quit", 862, 556, 70, 32, "Quit", ST_GHOST);
    }
    focus = 0;
    for (int i = 0; i < nw; i++) if (fid[0] && !strcmp(wd[i].id, fid)) focus = i;
    if (want_focus[0]) {
        for (int i = 0; i < nw; i++) if (!strcmp(wd[i].id, want_focus)) focus = i;
        want_focus[0] = 0;
    }
    if (addr_sel >= addr_count()) addr_sel = 0;
    if (page == PG_ONLINE && focus < nw && wd[focus].type == W_ROW) addr_sel = wd[focus].fld;   /* arrows pick the address */
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
static int slider_min(int kind) { (void)kind; return 0; }
/* link delay: the game tolerates K<=2 with 2 players and K<=1 with 3-4 (NOTES 'Cabo link P2c') */
static int slider_max(int kind) { return kind == 1 ? 32767 : kind == 2 ? (cfg.link_players >= 3 ? 1 : 2) : 100; }
static int slider_step(int kind) { return kind == 1 ? 1000 : kind == 2 ? 1 : 5; }

static void set_slider(int kind, int lx, const Widget *k) {
    if (!k->enabled) return;
    int span = k->r.w > 0 ? k->r.w : 1, lo = slider_min(kind), hi = slider_max(kind);
    int v = lo + ((lx - k->r.x) * (hi - lo) + span / 2) / span;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int *ref = slider_ref(kind);
    if (v != *ref) { *ref = v; if (kind == 2) last_delay = v; save_cfg(); }
}

static void bump_slider(const Widget *k, int dir) {
    if (!k->enabled) return;
    int *ref = slider_ref(k->slider);
    int v = *ref + dir * slider_step(k->slider);
    if (v < slider_min(k->slider)) v = slider_min(k->slider);
    if (v > slider_max(k->slider)) v = slider_max(k->slider);
    if (v != *ref) { *ref = v; if (k->slider == 2) last_delay = v; save_cfg(); }
}

static const char *const BTN_NAME[CFG_BTN_COUNT] = { "A", "B", "L", "R", "Start", "Select", "Up", "Down", "Left", "Right" };

static char *binding(int btn) { return page == PG_KEYBOARD ? cfg.key[btn] : cfg.pad[btn]; }

/* the controller names of kirby.ini are SDL's ("leftshoulder", "dpup"): show them the way a pad prints them */
static const char *pretty_binding(const char *b) {
    static const char *const map[][2] = {
        { "leftshoulder", "LB" }, { "rightshoulder", "RB" }, { "lefttrigger", "LT" }, { "righttrigger", "RT" },
        { "leftstick", "LS" }, { "rightstick", "RS" }, { "dpup", "Up" }, { "dpdown", "Down" },
        { "dpleft", "Left" }, { "dpright", "Right" }, { "back", "Back" }, { "start", "Start" },
        { "guide", "Guide" }, { "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
    };
    if (page == PG_CONTROLLER)
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (ieq(b, map[i][0])) return map[i][1];
    return b;
}

static void start_capture(int btn) {
    if (page == PG_CONTROLLER && !pad_present()) { set_note("No controller connected", 1); return; }
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
    if (other >= 0) snprintf(m, sizeof m, "%s = %s  (swapped with %s)", BTN_NAME[btn], pretty_binding(name), BTN_NAME[other]);
    else snprintf(m, sizeof m, "%s = %s", BTN_NAME[btn], pretty_binding(name));
    set_note(m, 0);
}

static void capture_key(SDL_Keycode sym) {
    if (sym == SDLK_ESCAPE) { capture = -1; set_note("Cancelled", 0); return; }
    const char *name = SDL_GetKeyName(sym);
    if (!name || !*name || strlen(name) >= CFG_NAME_MAX || strpbrk(name, "=;#[]")) {
        set_note("That key cannot be used - press another", 1);
        return;
    }
    if (cfg_key_reserved(name)) {
        char m[96];
        snprintf(m, sizeof m, "%s is reserved - press another key", name);
        set_note(m, 1);
        return;
    }
    assign(capture, name);
}

static void capture_button(int sdl_button) {
    const char *name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)sdl_button);
    if (!name || !*name) { set_note("Unknown button - press another", 1); return; }
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

/* a code point typed or pasted: the NAME field also takes the Latin-1 letters (stored as UTF-8: 2 bytes) */
static void field_append_cp(int f, int cp) {
    if (cp < 0x80) { field_append(f, cp); return; }
    if (f != 0 || cp < 0xC0 || cp > 0xFF || cp == 0xD7 || cp == 0xF7) return;
    size_t n = strlen(fbuf[f]);
    if ((int)n + 2 > fcap[f]) return;
    fbuf[f][n] = (char)(0xC0 | (cp >> 6));
    fbuf[f][n + 1] = (char)(0x80 | (cp & 0x3F));
    fbuf[f][n + 2] = 0;
}
static void field_append_text(int f, const char *s) { for (int cp; (cp = utf8_next(&s)) != 0;) field_append_cp(f, cp); }

static void field_backspace(int f) {
    size_t n = strlen(fbuf[f]);
    while (n > 1 && ((unsigned char)fbuf[f][n - 1] & 0xC0) == 0x80) n--;   /* a whole UTF-8 character */
    if (n) fbuf[f][n - 1] = 0;
}
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
    field_append_text(f, t);
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
        if (f == 0) { set_note("Type the name on a keyboard", 1); return; }
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

static void set_transport(const char *t) { snprintf(cfg.link_transport, sizeof cfg.link_transport, "%s", t); save_cfg(); }

/* returns 1 when `id` was an ONLINE / keypad control */
static int online_activate(const char *id) {
    if (!strcmp(id, "online")) { page = PG_ONLINE; capture = -1; focus = 0; sync_fields(); role = !strcmp(cfg.link_mode, "join"); return 1; }
    if (!strcmp(id, "role_host")) { role = 0; return 1; }
    if (!strcmp(id, "role_join")) { role = 1; return 1; }
    if (!strncmp(id, "online_p", 8) && id[8] >= '2' && id[8] <= '4' && !id[9]) {
        cfg.link_players = id[8] - '0';
        if (cfg.link_delay > slider_max(2)) cfg.link_delay = slider_max(2);
        save_cfg(); return 1;
    }
    if (!strcmp(id, "online_net_auto")) { set_transport("auto"); return 1; }
    if (!strcmp(id, "online_net_udp")) { set_transport("udp"); return 1; }
    if (!strcmp(id, "online_net_tcp")) { set_transport("tcp"); return 1; }
    if (!strcmp(id, "online_auto")) {
        if (cfg.link_delay < 0) { cfg.link_delay = last_delay > slider_max(2) ? slider_max(2) : last_delay; }
        else { last_delay = cfg.link_delay; cfg.link_delay = -1; }
        save_cfg(); return 1;
    }
    if (!strcmp(id, "online_host") || !strcmp(id, "online_join")) {
        int join = !strcmp(id, "online_join");
        end_edit();
        commit_fields();
        if (!port_ok()) { set_note("Port must be 1-65535", 1); return 1; }
        if (join && !fbuf[1][0]) { set_note("Type the host address to join", 1); return 1; }
        snprintf(cfg.link_mode, sizeof cfg.link_mode, "%s", join ? "join" : "host");
        save_cfg();
        result = join ? KL_ONLINE_JOIN : KL_ONLINE_HOST; running = 0;
        return 1;
    }
    if (!strcmp(id, "online_start")) { end_edit(); result = KL_ONLINE_START; running = 0; return 1; }
    if (!strcmp(id, "online_ready")) { end_edit(); result = KL_ONLINE_READY; running = 0; return 1; }
    if (!strcmp(id, "online_leave")) {
        end_edit();
        snprintf(cfg.link_mode, sizeof cfg.link_mode, "off");
        save_cfg();
        result = KL_ONLINE_LEAVE; running = 0;
        return 1;
    }
    if (!strcmp(id, "online_copy")) {
        if (show_addrs()) { SDL_SetClipboardText(addr_get(addr_sel)); set_note("Address copied", 0); }
        return 1;
    }
    if (!strncmp(id, "online_ip", 9)) {
        int a = id[9] - '0';
        if (a >= 0 && a < addr_count()) addr_sel = a;
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

static void go_home(void) {
    end_edit();
    commit_fields();
    page = PG_MAIN; capture = -1; focus = 0;
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
    } else if (!strcmp(id, "settings")) {
        page = PG_SETTINGS; capture = -1; focus = 0;
    } else if (!strcmp(id, "input") || !strcmp(id, "tab_keyboard")) {
        page = PG_KEYBOARD; capture = -1; if (strcmp(id, "tab_keyboard")) focus = 0;
    } else if (!strcmp(id, "tab_controller")) {
        page = PG_CONTROLLER; capture = -1;
    } else if (!strcmp(id, "back") || !strcmp(id, "done")) {
        go_home();
    } else if (!strcmp(id, "reset")) {
        if (page == PG_KEYBOARD) { cfg_reset_keyboard(&cfg); set_note("Keyboard reset to defaults", 0); }
        else { cfg_reset_controller(&cfg); set_note("Controller reset to defaults", 0); }
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

/* spatial navigation: nearest enabled widget in the given direction */
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

static int hit(int lx, int ly) {
    for (int i = nw - 1; i >= 0; i--) {
        SDL_Rect r = wd[i].r;
        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ rendering: widgets */
static void shadowed(SDL_Rect r, SDL_Color body, SDL_Color border, int shadow) {
    if (shadow) fill(R(r.x, r.y + 2, r.w, r.h), C_SHADOW);
    fill(r, body);
    frame2(r, border);
}

static void draw_key(const Widget *k, int is_hover) {
    int active = capture == k->btn;
    SDL_Color bg = active ? C_ACC : C_BTN, bd = active ? C_ACC : (is_hover ? C_ACC : C_BTNB);
    int cx = k->r.x + k->r.w / 2;
    char val[CFG_NAME_MAX + 8];
    const char *pb = pretty_binding(binding(k->btn));
    int maxw = k->r.w - (k->shape == SH_CIRCLE ? 20 : 10);
    fit(UF_S12, pb, maxw, val, sizeof val);
    const char *lab = BTN_NAME[k->btn];
    SDL_Color lc = active ? C_ONTXT : C_DIM, vc = active ? C_ONTXT : C_TEXT;
    if (k->shape == SH_CIRCLE) {
        int cy = k->r.y + k->r.h / 2, r = k->r.w / 2;
        fill_circle(cx, cy, r, bd);
        fill_circle(cx, cy, r - 2, bg);
        char up[8];
        snprintf(up, sizeof up, "%s", lab);
        tx(UF_S18, up, cx - tw(UF_S18, up) / 2, cy - 18, vc);
        tx(UF_LBL, val, cx - tw(UF_LBL, val) / 2, cy + 5, active ? C_ONTXT : C_ACC);
        return;
    }
    SDL_Rect r = k->r;
    if (k->style == 1) {                       /* shoulder buttons: rounded top */
        round_rect(r, 8, bd);
        round_rect(R(r.x + 2, r.y + 2, r.w - 4, r.h - 4), 6, bg);
    } else {
        fill(r, bd);
        fill(R(r.x + 2, r.y + 2, r.w - 4, r.h - 4), bg);
    }
    char ul[16];
    int n = 0;
    for (const char *s = lab; *s && n < 15; s++) ul[n++] = (char)toupper((unsigned char)*s);
    ul[n] = 0;
    tx(UF_LBL, ul, cx - tw(UF_LBL, ul) / 2, r.y + 5, lc);
    tx(UF_S12, val, cx - tw(UF_S12, val) / 2, r.y + r.h - 8 - cap_h(UF_S12) - 3, vc);
}

static void draw_widget(int i) {
    Widget *k = &wd[i];
    int is_focus = i == focus, is_hover = i == hover && k->enabled;
    SDL_Color fg = k->enabled ? C_TEXT : C_OFF;
    switch (k->type) {
    case W_BUTTON: {
        if (k->style == ST_GHOST) {
            if (is_hover || is_focus) frame2(k->r, C_BTNB);
            tx_c(UF_B13, k->label, k->r, is_hover ? C_TEXT : C_DIM);
            break;
        }
        if (k->style == ST_CHIP) {
            fill(k->r, C_CARD);
            frame2(k->r, is_hover ? C_ACC : C_CARDB);
            tx_c(UF_S12, k->label, k->r, k->enabled ? C_TEXT : C_OFF);
            break;
        }
        SDL_Color bg = !k->enabled ? C_BTN : k->style == ST_PRIMARY || k->style == ST_BIG ? (is_hover ? C_ACCH : C_ACC) : C_BTN;
        SDL_Color bd = !k->enabled ? C_CARDB : k->style == ST_PRIMARY || k->style == ST_BIG ? bg : (is_hover ? C_ACC : C_BTNB);
        shadowed(k->r, bg, bd, k->enabled);
        int f = k->style == ST_BIG ? UF_S18 : k->style == ST_PRIMARY ? UF_S15 : UF_B13;
        tx_c(f, k->label, k->r, !k->enabled ? C_OFF : k->style == ST_PRIMARY || k->style == ST_BIG ? C_ONTXT : fg);
        break;
    }
    case W_NAV: {
        char t[48], d[48];
        const char *bar = strchr(k->label, '|');
        snprintf(t, sizeof t, "%.*s", bar ? (int)(bar - k->label) : (int)strlen(k->label), k->label);
        snprintf(d, sizeof d, "%s", bar ? bar + 1 : "");
        SDL_Color bg = is_hover ? C_NAVH : C_CARD;
        shadowed(k->r, k->enabled ? bg : C_CARD, k->enabled ? (is_hover ? C_ACC : C_CARDB) : C_LINE, k->enabled);
        tx(UF_S15, t, k->r.x + 14, k->r.y + 12, k->enabled ? C_TEXT : C_OFF);
        tx(UF_S11, d, k->r.x + 14, k->r.y + 34, k->enabled ? C_DIM : C_OFF);
        break;
    }
    case W_RADIO: {
        SDL_Color bg = k->on ? (k->enabled ? C_ACC : C_BTNB) : (is_hover && k->enabled ? C_BTN : C_SEG);
        fill(k->r, bg);
        fill(R(k->r.x, k->r.y, k->r.w, 2), C_BTNB);                     /* top and bottom edges */
        fill(R(k->r.x, k->r.y + k->r.h - 2, k->r.w, 2), C_BTNB);
        if (k->seg & 1) fill(R(k->r.x, k->r.y, 2, k->r.h), C_BTNB);
        fill(R(k->r.x + k->r.w - 2, k->r.y, 2, k->r.h), C_BTNB);
        tx_c(UF_B13, k->label, k->r, k->on && k->enabled ? C_ONTXT : (k->enabled ? C_SEGTXT : (k->on ? C_TEXT : C_OFF)));
        break;
    }
    case W_CHECK: {                                                     /* the design's switch */
        SDL_Color bg = k->on && k->enabled ? C_ACC : C_FIELD, bd = k->on && k->enabled ? C_ACC : C_BTNB;
        fill(k->r, bd);
        fill(R(k->r.x + 2, k->r.y + 2, k->r.w - 4, k->r.h - 4), bg);
        int kx = k->on ? k->r.x + k->r.w - 2 - 14 - 2 : k->r.x + 2 + 2;
        fill(R(kx, k->r.y + 4, 14, 14), k->on && k->enabled ? C_ONTXT : (k->enabled ? C_DIM : C_OFF));
        break;
    }
    case W_SLIDER: {
        int v = *slider_ref(k->slider), vmin = slider_min(k->slider), vmax = slider_max(k->slider);
        int inner = k->r.w - 6, fw = vmax > vmin ? (int)((long long)inner * (v - vmin) / (vmax - vmin)) : 0;
        int cy = k->r.y + k->r.h / 2;
        SDL_Rect track = R(k->r.x, cy - 5, k->r.w, 10);
        fill(track, C_FIELD);
        frame2(track, C_BTNB);
        if (k->slider == 0 && cfg.mute) fw = 0;
        if (fw > 0) fill(R(k->r.x + 3, cy - 2, fw, 4), k->enabled ? C_ACC : C_OFF);
        if (k->enabled) {
            SDL_Rect th = R(k->r.x + 3 + fw - 5, cy - 9, 10, 18);
            fill(th, C_ONTXT);
            fill(R(th.x + 2, th.y + 2, th.w - 4, th.h - 4), C_TEXT);
        }
        break;
    }
    case W_FIELD: {
        int ed = editing == k->fld;
        fill(k->r, C_FIELD);
        frame2(k->r, ed ? C_ACC : (is_hover && k->enabled ? C_ACC : C_BTNB));
        const char *b = fbuf[k->fld];
        int maxw = k->r.w - 24;
        const char *tail = b;
        while (*tail && tw(UF_B13, tail) > maxw) utf8_next(&tail);       /* show the end of a long value */
        SDL_Color tc = !k->enabled ? C_DIM : (k->fld == 2 && !port_ok()) ? C_ERR : C_TEXT;
        tx_l(UF_B13, tail, k->r.x + 12, k->r, tc);
        if (ed && (SDL_GetTicks() / 400) % 2 == 0) fill(R(k->r.x + 12 + tw(UF_B13, tail) + 1, k->r.y + 9, 2, k->r.h - 18), C_ACC);
        break;
    }
    case W_ROW: {
        int sel = k->fld == addr_sel;
        char a[64];
        if (sel) fill(k->r, C_LINE);
        fit(UF_S12, addr_get(k->fld), k->r.w - 30, a, sizeof a);
        tx_l(UF_S12, sel ? ">" : " ", k->r.x + 6, k->r, C_ACC);
        tx_l(UF_S12, a, k->r.x + 22, k->r, sel ? C_OK : C_TEXT);
        break;
    }
    case W_SHAPE:
        draw_key(k, is_hover);
        break;
    }
    if (is_focus) {                                                      /* outline 2px, offset 2px */
        SDL_Rect f = k->r;
        if (k->type == W_ROW) { frame2(f, C_FOCUS); return; }
        f.x -= 4; f.y -= 4; f.w += 8; f.h += 8;
        frame2(f, C_FOCUS);
    }
}

/* ------------------------------------------------------------------ rendering: pages */
static void card(SDL_Rect r) { fill(r, C_CARD); frame2(r, C_CARDB); }

static void header_rule(void) { fill(R(0, 63, KL_W, 2), C_LINE); }
static void footer_rule(void) { fill(R(0, 552, KL_W, 2), C_LINE); }
static void footer_hint(const char *s) {
    if (note[0] && SDL_GetTicks() < note_until) { tx(UF_LBL, note, 28, 570, note_err ? C_ERR : C_OK); return; }
    tx(UF_LBL, s, 28, 570, C_DIM);
}
static void page_title(const char *t) { tx_l(UF_T22, t, 140, R(0, 0, 0, 64), C_ACC); }

static const char *state_text(void) {
    static char buf[64];
    switch (st.state) {
    case 0: return "Waiting for a ROM";
    case 1: return "Preparing the fast mode...";
    case 2: snprintf(buf, sizeof buf, "Building compiled version..."); return buf;
    case 3: return "Compiled version ready";
    default: return "Error";
    }
}

static void draw_main(void) {
    /* header: logo square + label, ROM chip */
    fill(R(31, 31, 10, 10), C_BTNB);
    fill(R(28, 28, 10, 10), C_ACC);
    tx_l(UF_LBL, "RECOMP LAUNCHER", 48, R(0, 20, 0, 26), C_DIM);
    if (rom_missing()) {
        /* the SELECT ROM chip widget sits here */
    } else {
        char name[64], chip[96];
        fit(UF_S12, basename_of(st.rom), 200, name, sizeof name);
        snprintf(chip, sizeof chip, "%s  -  OK", name);
        int w = tw(UF_S12, chip) + 42, x = 932 - w;
        card(R(x, 16, w, 26));
        fill(R(x + 12, 25, 8, 8), C_OK);
        tx_l(UF_S12, chip, x + 28, R(0, 16, 0, 26), C_TEXT);
    }
    /* title */
    const char *title = "KIRBY RECOMP";
    tx(UF_HERO, title, (KL_W - tw(UF_HERO, title)) / 2, 100, C_ACC);
    const char *sub = "Kirby & The Amazing Mirror  -  GBA recompilation";
    tx(UF_B13, sub, (KL_W - tw(UF_B13, sub)) / 2, 192, C_DIM);
    /* build status card */
    SDL_Rect c = R(220, 236, 520, 98);
    card(c);
    tx(UF_LBL, "BUILD STATUS", c.x + 16, c.y + 14, C_DIM);
    {
        const char *tag = st.state == 0 ? "No ROM" : st.state == 1 ? "Preparing" : st.state == 2 ? "You can play now" :
                          st.state == 3 ? "Ready to play" : "Error";
        SDL_Color tbg = st.state == 4 ? C_ERRBG : st.state == 0 || st.state == 1 ? C_BTN : C_OKBG;
        SDL_Color tbd = st.state == 4 ? C_ERR : st.state == 0 || st.state == 1 ? C_BTNB : C_OKB;
        SDL_Color tc = st.state == 4 ? C_ERR : st.state == 0 || st.state == 1 ? C_DIM : C_OK;
        int w = tw(UF_S12, tag) + 20;
        SDL_Rect t = R(c.x + c.w - 16 - w, c.y + 9, w, 22);
        fill(t, tbd);
        fill(R(t.x + 2, t.y + 2, t.w - 4, t.h - 4), tbg);
        tx_c(UF_S12, tag, t, tc);
    }
    {
        char line[96];
        if (st.text[0] && st.state != 0) fit(UF_B13, st.text, 340, line, sizeof line);
        else snprintf(line, sizeof line, "%s", state_text());
        tx(UF_B13, line, c.x + 16, c.y + 44, st.state == 4 ? C_ERR : C_TEXT);
        if (st.state == 1 || st.state == 2 || st.state == 3) {
            char pc[16];
            int p = st.state == 3 ? 100 : st.percent < 0 ? 0 : st.percent > 100 ? 100 : st.percent;
            snprintf(pc, sizeof pc, "%d%%", p);
            tx_r(UF_B13, pc, c.x + c.w - 16, c.y + 44, C_ACC);
        }
    }
    bar_drawn = 0;
    if (st.state == 1 || st.state == 2) {
        SDL_Rect bar = R(c.x + 16, c.y + 72, c.w - 32, 10);
        fill(bar, C_FIELD);
        frame2(bar, C_BTNB);
        int p = st.percent < 0 ? 0 : st.percent > 100 ? 100 : st.percent;
        int fw = (bar.w - 6) * p / 100;
        if (fw > 0) fill(R(bar.x + 3, bar.y + 3, fw, 4), C_ACC);
        bar_rect = bar;
        bar_drawn = 1;
    }
    footer_rule();
    footer_hint("TAB / ARROWS MOVE  .  ENTER SELECT  .  ESC QUIT");
}

static void row_text(int x, int y, const char *title, const char *desc, int dim) {
    tx(UF_B13, title, x, y, dim ? C_OFF : C_TEXT);
    if (desc && *desc) tx(UF_S11, desc, x, y + 19, C_DIM);
}

static void draw_settings(void) {
    header_rule();
    page_title("Settings");
    tx_r(UF_LBL, "VIDEO . AUDIO . SAVES", 932, 25, C_DIM);
    SDL_Rect v = R(28, 86, 533, 256);
    card(v);
    tx(UF_S15, "Video", v.x + 18, v.y + 14, C_ACC);
    row_text(46, 124, "Window size", "Multiple of the 240x160 GBA screen", cfg.fullscreen);
    fill(R(v.x + 2, 178, v.w - 4, 2), C_LINE);
    row_text(46, 198, "Fullscreen", "Toggle any time with F11", 0);
    fill(R(v.x + 2, 252, v.w - 4, 2), C_LINE);
    row_text(46, 272, "Scaling", "Integer keeps pixels sharp", 0);
    SDL_Rect a = R(577, 86, 355, 140);
    card(a);
    tx(UF_S15, "Audio", a.x + 18, a.y + 14, C_ACC);
    tx(UF_B13, "Volume", 597, 123, C_TEXT);
    {
        char vs[16];
        if (cfg.mute) snprintf(vs, sizeof vs, "Muted"); else snprintf(vs, sizeof vs, "%d%%", cfg.volume);
        tx_r(UF_B13, vs, 912, 123, C_ACC);
    }
    fill(R(a.x + 2, 174, a.w - 4, 2), C_LINE);
    tx(UF_B13, "Mute", 597, 190, C_TEXT);
    SDL_Rect s = R(577, 242, 355, 100);
    card(s);
    tx(UF_S15, "Saves", s.x + 18, s.y + 12, rom_loaded() ? C_ACC : C_OFF);
    tx(UF_S11, rom_loaded() ? "Back up or move your save data." : "Load a ROM to manage its save.", 597, 274, C_DIM);
    footer_rule();
    footer_hint("TAB / ARROWS MOVE  .  ENTER SELECT  .  ESC BACK");
}

/* live view of the left stick: ring = dead zone, dot = current position */
static void draw_stick_indicator(void) {
    const int cx = 880, cy = 508, Rr = 28;
    ring(cx, cy, Rr, C_BTNB);
    if (!pad_present()) { tx_r(UF_LBL, "NO CONTROLLER", cx - Rr - 14, 502, C_OFF); return; }
    int ax, ay;
    pad_axes(&ax, &ay);
    int dzr = (int)((long long)Rr * cfg.deadzone / 32767);
    ring(cx, cy, dzr, C_ACC);
    int px = (int)((long long)ax * Rr / 32767), py = (int)((long long)ay * Rr / 32767);
    long long mag2 = (long long)ax * ax + (long long)ay * ay;
    int outside = mag2 > (long long)cfg.deadzone * cfg.deadzone;
    fill(R(cx + px - 3, cy + py - 3, 7, 7), outside ? C_OK : C_TEXT);
}

static void draw_edit(void) {
    header_rule();
    page_title("Input");
    /* GBA outline (design: 640x300 at (160,124)) */
    const int ox = 160, oy = 124;
    round_rect(R(ox, oy + 40, 640, 240), 44, C_GBAB);
    round_rect(R(ox + 2, oy + 42, 636, 236), 42, C_GBA);
    fill(R(ox + 90, oy + 118, 56, 40), C_GBAB);                         /* d-pad centre */
    fill(R(ox + 92, oy + 120, 52, 36), C_GBAIN);
    SDL_Rect scr = R(ox + 220, oy + 76, 200, 122);
    fill(scr, C_BTNB);
    fill(R(scr.x + 2, scr.y + 2, scr.w - 4, scr.h - 4), C_SCREEN);
    tx_c(UF_T22, "GBA", scr, C_OFF);
    /* prompt / hints */
    const char *dev = page == PG_KEYBOARD ? "key" : "button";
    char s[96];
    if (capture >= 0) {
        snprintf(s, sizeof s, "Press a %s for %s...", dev, BTN_NAME[capture]);
        tx(UF_S15, s, (KL_W - tw(UF_S15, s)) / 2, 440, C_ACC);
        tx(UF_S11, "Esc cancels", (KL_W - tw(UF_S11, "Esc cancels")) / 2, 464, C_DIM);
    } else {
        tx(UF_S15, "Click a button to remap it", (KL_W - tw(UF_S15, "Click a button to remap it")) / 2, 440, C_TEXT);
        const char *nt = page == PG_KEYBOARD ? "Reserved: Esc, F11, F1  -  keys are bound by position (A = X, B = Z)"
                                             : "Buttons are bound by position on your controller";
        tx(UF_S11, nt, (KL_W - tw(UF_S11, nt)) / 2, 464, C_DIM);
    }
    if (page == PG_CONTROLLER) {
        tx(UF_B13, "Left stick as D-pad", 82, 505, C_TEXT);
        tx(UF_B13, "Deadzone", 320, 505, C_TEXT);
        char dz[16];
        snprintf(dz, sizeof dz, "%d", cfg.deadzone);
        tx(UF_S12, dz, 626, 507, C_DIM);
        draw_stick_indicator();
    }
    footer_rule();
    footer_hint("ARROWS MOVE  .  ENTER REMAP  .  ESC BACK");
}

static const char *slot_state_text(int st_) {
    switch (st_) {
    case KLS_CONNECTING: return "Joining";
    case KLS_CONNECTED: return "Connected";
    case KLS_READY: return "Ready";
    case KLS_LOST: return "Lost";
    default: return "Empty";
    }
}

static const char *online_state_text(void) {
    static char buf[48];
    switch (onl.state) {
    case KLO_HOSTING: return "Waiting for players";
    case KLO_CONNECTING: return "Connecting...";
    case KLO_CONNECTED: snprintf(buf, sizeof buf, "Connected (%d players)", slots_up() > 0 ? slots_up() : 2); return buf;
    case KLO_PLAYING: return "In game";
    case KLO_DESYNC: return "Desync detected";
    case KLO_LOST: return "Connection lost";
    default: return "Not connected";
    }
}

static void draw_online(void) {
    int off = !online_busy();
    SDL_Color lab = off ? C_DIM : C_OFF;
    header_rule();
    page_title("Online");
    tx(UF_LBL, "CABLE LINK", 140 + tw(UF_T22, "Online") + 14, 25, C_DIM);
    {   /* connection chip */
        const char *t = online_state_text();
        int w = tw(UF_S12, t) + 42, x = 932 - w;
        card(R(x, 19, w, 26));
        fill(R(x + 12, 28, 8, 8), onl.state == KLO_DESYNC || onl.state == KLO_LOST ? C_ERR : onl.state == KLO_OFFLINE ? C_DIM : C_OK);
        tx_l(UF_S12, t, x + 28, R(0, 19, 0, 26), C_TEXT);
    }
    /* connection card */
    SDL_Rect c = R(28, 86, 475, 446);
    card(c);
    tx(UF_S15, "Connection", c.x + 18, c.y + 14, C_ACC);
    tx(UF_LBL, "NAME", 46, 134, lab);
    tx(UF_LBL, role == 0 ? "YOUR ADDRESS" : "HOST ADDRESS", 46, 198, lab);
    tx(UF_LBL, "PORT", 393, 198, lab);
    tx(UF_LBL, "PLAYERS", 46, 264, lab);
    {
        char dl[32];
        if (cfg.link_delay < 0) snprintf(dl, sizeof dl, "DELAY . AUTO"); else snprintf(dl, sizeof dl, "DELAY . %d FR", cfg.link_delay);
        tx(UF_LBL, dl, 290, 264, lab);
        tx_r(UF_LBL, "AUTO", 437, 264, lab);
    }
    tx(UF_LBL, "TRANSPORT", 46, 324, lab);
    fill(R(c.x + 18, 392, c.w - 36, 2), C_LINE);
    /* lobby card */
    SDL_Rect l = R(519, 86, 413, 446);
    card(l);
    tx(UF_S15, "Lobby", l.x + 18, l.y + 14, C_ACC);
    tx(UF_LBL, "SLOT", 537, 128, C_DIM);
    tx(UF_LBL, "NAME", 603, 128, C_DIM);
    tx(UF_LBL, "STATE", 745, 128, C_DIM);
    tx_r(UF_LBL, "PING", 914, 128, C_DIM);
    for (int i = 0; i < 4; i++) {
        int y = 152 + i * 34, sst = onl.slots[i];
        int dim = (sst == KLS_EMPTY && i >= cfg.link_players && off);
        SDL_Color c0 = sst == KLS_EMPTY ? (dim ? C_OFF : C_DIM) : sst == KLS_LOST ? C_ERR : (i == onl.self_slot ? C_ACC : C_TEXT);
        fill(R(l.x + 2, y - 8, l.w - 4, 2), C_LINE);
        char nm[16];
        char sl[8];
        snprintf(sl, sizeof sl, "P%d", i + 1);
        tx(UF_S12, sl, 537, y, c0);
        if (i == 0) {
            int tgw = tw(UF_LBL, "HOST") + 10;
            SDL_Rect t = R(537 + tw(UF_S12, sl) + 6, y - 1, tgw, 18);
            fill(t, C_CARDB);
            tx_c(UF_LBL, "HOST", t, C_DIM);
        }
        fit(UF_S12, onl.names[i][0] ? onl.names[i] : "-", 130, nm, sizeof nm);
        tx(UF_S12, nm, 603, y, c0);
        tx(UF_S12, slot_state_text(sst), 745, y, sst == KLS_READY ? C_OK : c0);
        if ((sst == KLS_CONNECTED || sst == KLS_READY) && onl.ping_ms[i] >= 0) {
            char pg[16];
            snprintf(pg, sizeof pg, "%d ms", onl.ping_ms[i] > 999 ? 999 : onl.ping_ms[i]);
            SDL_Color pc = onl.ping_ms[i] < 60 ? C_OK : onl.ping_ms[i] < 120 ? C_WARN : C_ERR;
            tx_r(UF_S12, pg, 914, y, pc);
        } else {
            tx_r(UF_S12, "-", 914, y, C_OFF);
        }
    }
    fill(R(l.x + 2, 152 + 4 * 34 - 8, l.w - 4, 2), C_LINE);
    /* status / hint */
    {
        char hint[160] = "";
        SDL_Color hc = C_DIM;
        if (onl.message[0]) { snprintf(hint, sizeof hint, "%s", onl.message); hc = C_ACC; }
        else if (off && !port_ok()) { snprintf(hint, sizeof hint, "Port must be 1-65535"); hc = C_ERR; }
        else if (off && !fbuf[1][0] && role == 1) snprintf(hint, sizeof hint, "Type the host address to join.");
        else if (off) snprintf(hint, sizeof hint, "Waiting for players. Share your address and port so others can join.");
        else if (onl.state == KLO_HOSTING && slots_up() < 2) snprintf(hint, sizeof hint, "Waiting for players. Everybody presses Ready.");
        else if ((onl.state == KLO_HOSTING || onl.state == KLO_CONNECTED) && !all_ready()) snprintf(hint, sizeof hint, "The game starts when everybody is ready.");
        /* wrap to the card width */
        int y = 306, x = 537, maxw = 377;
        const char *p = hint;
        while (*p && y < 372) {
            char line[96];
            int n = utf8_count(p);
            for (; n > 0; n--) { utf8_cut(line, sizeof line, p, n); if (tw(UF_S12, line) <= maxw) break; }
            if (n <= 0) break;
            if (p[strlen(line)] && n > 8) {                     /* break at the last space when we cut a word */
                char *sp = strrchr(line, ' ');
                if (sp) *sp = 0;
            }
            tx(UF_S12, line, x, y, hc);
            p += strlen(line);
            while (*p == ' ') p++;
            y += 18;
        }
    }
    if (onl.state == KLO_PLAYING) {                       /* connection quality */
        static const char *const qn[4] = { "Measuring...", "Good", "Fair", "Poor" };
        int q = onl.quality >= 0 && onl.quality <= 3 ? onl.quality : 0;
        SDL_Color qc = q == KLQ_GOOD ? C_OK : q == KLQ_FAIR ? C_WARN : q == KLQ_POOR ? C_ERR : C_DIM;
        char ql[64], sl2[48];
        if (q && onl.ping_p95_ms >= 0) snprintf(ql, sizeof ql, "Connection: %s (p95 %d ms)", qn[q], onl.ping_p95_ms > 999 ? 999 : onl.ping_p95_ms);
        else snprintf(ql, sizeof ql, "Connection: %s", qn[q]);
        tx(UF_B13, ql, 537, 384, qc);
        snprintf(sl2, sizeof sl2, "Pauses over 100 ms: %d", onl.stalls_100ms > 9999 ? 9999 : onl.stalls_100ms);
        tx(UF_S12, sl2, 537, 406, onl.stalls_100ms > 0 ? C_WARN : C_DIM);
    } else if (show_addrs()) {
        tx(UF_LBL, addr_count() > 1 ? "YOUR ADDRESSES  (UP/DOWN PICKS ONE)" : "YOUR ADDRESS", 537, 416, C_DIM);
    }
}

static void dim_screen(void) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 4, 4, 14, 205);
    SDL_Rect all = { 0, 0, KL_W, KL_H };
    SDL_RenderFillRect(ren, &all);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

static void draw_keypad(void) {
    dim_screen();
    SDL_Rect box = R(250, 120, 460, 330);
    card(box);
    char l[64];
    snprintf(l, sizeof l, "%s", keypad_field == 1 ? "ADDRESS" : "PORT");
    tx(UF_LBL, l, 276, 142, C_DIM);
    SDL_Rect f = R(276, 160, 408, 40);
    fill(f, C_FIELD);
    frame2(f, C_ACC);
    const char *b = fbuf[keypad_field];
    while (*b && tw(UF_S18, b) > f.w - 30) utf8_next(&b);
    snprintf(l, sizeof l, "%s_", b);
    tx_l(UF_S18, l, f.x + 12, f, C_TEXT);
}

static void draw_confirm(void) {                 /* modal dialog over a dimmed screen */
    dim_screen();
    SDL_Rect box = R(250, 226, 460, 168);
    card(box);
    tx_c(UF_S18, "Overwrite current save?", R(250, 246, 460, 30), C_TEXT);
    tx_c(UF_B13, "A backup is kept.", R(250, 286, 460, 24), C_OK);
}

static void draw_canvas(void) {
    ntexts = 0; bar_drawn = 0;
    setc(C_BG);
    SDL_RenderClear(ren);
    rec_texts = 1;
    if (page == PG_ONLINE) draw_online(); else if (page == PG_SETTINGS) draw_settings();
    else if (page == PG_KEYBOARD || page == PG_CONTROLLER) draw_edit(); else draw_main();
    rec_texts = 0;      /* texts inside widgets are covered by the widget rectangles */
    for (int i = 0; i < nw; i++) draw_widget(i);
    if (confirm) { draw_confirm(); for (int i = 0; i < nw; i++) draw_widget(i); }
    if (keypad_field >= 0) { draw_keypad(); for (int i = 0; i < nw; i++) draw_widget(i); }
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
    if ((dir == 2 || dir == 3) && f->type == W_SLIDER) { bump_slider(f, dir == 3 ? 1 : -1); return; }
    focus_dir(dir == 3 ? 1 : dir == 2 ? -1 : 0, dir == 1 ? 1 : dir == 0 ? -1 : 0);
}

static void go_back(void) {
    if (keypad_field >= 0) { keypad_close(0); layout(); return; }
    if (editing >= 0) { end_edit(); return; }
    if (confirm) { confirm = 0; layout(); focus = 0; return; }
    if (page) { go_home(); layout(); }
}

static void on_key(SDL_Keycode k, int shift) {
    if (editing >= 0) { field_key(k, shift); return; }       /* typing in a field */
    if (capture >= 0) {                          /* remapping: the next key is the binding */
        if (page == PG_KEYBOARD) capture_key(k);
        else if (k == SDLK_ESCAPE) { capture = -1; set_note("Cancelled", 0); }   /* controller page ignores other keys */
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
    if (capture >= 0 && page == PG_CONTROLLER) { capture_button(b); return; }
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
        if (editing >= 0) field_append_text(editing, e->text.text);
        else if (keypad_field >= 0) field_append_text(keypad_field, e->text.text);
        break;
    case SDL_MOUSEMOTION: {
        if (!for_us(e->motion.windowID)) break;
        int lx, ly;
        to_logical(e->motion.x, e->motion.y, &lx, &ly);
        hover = hit(lx, ly);
        if (dragging) {
            int kind = dragging - 1;
            for (int i = 0; i < nw; i++) if (wd[i].type == W_SLIDER && wd[i].slider == kind) set_slider(kind, lx, &wd[i]);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (!for_us(e->button.windowID)) break;
        if (e->button.button == SDL_BUTTON_LEFT) {
            int lx, ly;
            to_logical(e->button.x, e->button.y, &lx, &ly);
            if (capture >= 0) { capture = -1; set_note("Cancelled", 0); }   /* a click elsewhere cancels the capture */
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
KL_API void kl_set_start_page(int p) { start_page = p >= 0 && p <= PG_SETTINGS ? p : PG_MAIN; }

KL_API int kl_test_widget(const char *id, int *x, int *y, int *w, int *h) {
    int i = find(id);
    if (i < 0) return 0;
    if (x) *x = wd[i].r.x;
    if (y) *y = wd[i].r.y;
    if (w) *w = wd[i].r.w;
    if (h) *h = wd[i].r.h;
    return 1;
}
KL_API int kl_test_enabled(const char *id) { int i = find(id); return i >= 0 && wd[i].enabled; }
KL_API const char *kl_test_focus(void) { return nw ? wd[focus].id : ""; }
KL_API void kl_test_set_focus(const char *id) { int i = find(id); if (i >= 0) focus = i; }
KL_API const char *kl_test_page(void) {
    return page == PG_MAIN ? "main" : page == PG_KEYBOARD ? "keyboard" : page == PG_CONTROLLER ? "controller" :
           page == PG_SETTINGS ? "settings" : "online";
}
KL_API unsigned kl_test_window_id(void) { return win ? SDL_GetWindowID(win) : 0; }
KL_API int kl_test_capture(void) { return capture; }
KL_API const char *kl_test_note(void) { return note[0] && SDL_GetTicks() < note_until ? note : ""; }
KL_API void kl_test_fake_pad(int present, int ax, int ay) { fake_pad = present; fake_ax = ax; fake_ay = ay; }
KL_API int kl_test_pad_present(void) { return pad_present(); }
KL_API int kl_test_addr_sel(void) { return addr_sel; }
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
    editing = -1; keypad_field = -1; via_pad = 0; addr_sel = 0; want_focus[0] = 0; memset(&onl, 0, sizeof onl);
    role = !strcmp(cfg.link_mode, "join");
    last_delay = cfg.link_delay >= 0 ? cfg.link_delay : 2;
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
    SDL_SetWindowMinimumSize(win, KL_W / 2, KL_H / 2);
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
    fonts_init();
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
                for (int q = 0; q < 4; q++) no.addresses[q][sizeof no.addresses[q] - 1] = 0;
                if (no.quality < 0 || no.quality > 3) no.quality = 0;
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
    fonts_free();
    SDL_DestroyTexture(canvas); canvas = NULL;
    SDL_DestroyRenderer(ren); ren = NULL;
    SDL_DestroyWindow(win); win = NULL;
    if (have_pad_sys) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result;
}
