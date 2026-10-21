/* test_kirby_ui.c - drives the launcher UI (kl_run) headless with simulated mouse/keyboard/drop events.
 * Runs with SDL_VIDEODRIVER=dummy (set here). Linked against kirby_ui.dll (checks the exports) or
 * statically. Each scenario is a script executed from the poll callback, which runs on the UI thread. */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "../launcher_ui.h"
#include "../config_ini.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static char ini[600];
static char shot_dir[500];
static int step;
typedef void (*Script)(KlStatus *out, int step);
static Script script;

static KirbyConfig now_cfg(void) { KirbyConfig c; cfg_load(ini, &c); return c; }

/* logical canvas coords -> window coords, following the scaling mode currently saved in the ini */
static void to_window(int lx, int ly, int *wx, int *wy) {
    int ww, wh, fs;
    kl_test_video(&ww, &wh, &fs);
    KirbyConfig c = now_cfg();
    float sx, sy, ox = 0, oy = 0;
    if (c.scaling == CFG_SCALING_STRETCH) {
        sx = (float)ww / KL_W; sy = (float)wh / KL_H;
    } else {
        float s = (float)ww / KL_W < (float)wh / KL_H ? (float)ww / KL_W : (float)wh / KL_H;
        if (c.scaling == CFG_SCALING_INTEGER && s >= 1.0f) s = (float)(int)s;
        sx = sy = s;
        ox = (ww - KL_W * s) / 2; oy = (wh - KL_H * s) / 2;
    }
    *wx = (int)(ox + lx * sx);
    *wy = (int)(oy + ly * sy);
}

static void click_win(int x, int y) {
    SDL_Event e;
    SDL_zero(e);
    /* A REAL mouse event carries the window id, and SDL (SDL_RendererEventWatch) then converts x,y to the
     * renderer's logical coordinates before the application sees them. Without windowID the event is
     * left alone, which is how this test used to mask a double conversion in the launcher (clicks off
     * by the scale factor, unusable in fullscreen). Mirror the real thing: first window has id 1.
     * The test therefore pushes WINDOW coordinates and expects the UI to see logical ones. */
    e.button.windowID = 1;
    e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED;
    e.button.x = x; e.button.y = y; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void click_logical(int lx, int ly) {
    int wx, wy;
    to_window(lx, ly, &wx, &wy);
    click_win(wx, wy);
}

static int click(const char *id) {
    int x, y, w, h;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); return 0; }
    click_logical(x + w / 2, y + h / 2);
    return 1;
}

static void key(SDL_Keycode k, int shift) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED; e.key.keysym.sym = k;
    e.key.keysym.mod = shift ? KMOD_LSHIFT : 0;
    SDL_PushEvent(&e);
}

static void set_status(KlStatus *o, int state, int pct, const char *text, const char *rom, int ok) {
    memset(o, 0, sizeof *o);
    o->state = state; o->percent = pct; o->rom_ok = ok;
    snprintf(o->text, sizeof o->text, "%s", text);
    snprintf(o->rom, sizeof o->rom, "%s", rom);
}

static void poll_cb(KlStatus *out) {
    script(out, step++);
    if (step > 200) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }   /* safety net */
}

static int run_script(Script s, int pg) {
    step = 0; script = s;
    kl_set_start_page(pg);
    return kl_run(ini, poll_cb);
}

static int pixel_is(int x, int y, uint32_t argb) {
    static uint32_t px[KL_W * KL_H];
    int w, h;
    if (kl_test_snapshot(px, KL_W * KL_H, &w, &h) != 0) return 0;
    return px[y * KL_W + x] == argb;
}

static int count_color(uint32_t argb, int x0, int y0, int x1, int y1) {
    static uint32_t px[KL_W * KL_H];
    int w, h, n = 0;
    if (kl_test_snapshot(px, KL_W * KL_H, &w, &h) != 0) return -1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (px[y * KL_W + x] == argb) n++;
    return n;
}

static void dump_frame(const char *name) {
    static uint32_t px[KL_W * KL_H];
    int w, h;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", shot_dir, name);
    FILE *f = kl_test_snapshot(px, KL_W * KL_H, &w, &h) == 0 ? fopen(path, "wb") : NULL;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc(px[i] >> 16 & 255, f); fputc(px[i] >> 8 & 255, f); fputc(px[i] & 255, f); }
    fclose(f);
}

static const uint32_t ACC = 0xFFFF7EB6u, OKC = 0xFF7EE0A0u;
#define ROM1 "C:\\roms\\Kirby & The Amazing Mirror (USA).gba"


/* ---------------------------------------------------------------- A: window follows the options (SETTINGS page) */
static void scen_video(KlStatus *o, int s) {
    int ww, wh, fs;
    set_status(o, 1, 20, "Analyzing...", ROM1, 1);
    switch (s) {
    case 1:
        CHECK(!strcmp(kl_test_page(), "settings"), "the settings page opens");
        click("size4");
        break;
    case 2:
        CHECK(now_cfg().scale == 4, "4X saves scale=4");
        kl_test_video(&ww, &wh, &fs);
        CHECK(ww == 960 && wh == 640, "scale 4 -> window 960x640 (240*4 x 160*4)");
        click("size1");
        break;
    case 3:
        CHECK(now_cfg().scale == 1, "1X saves scale=1");
        kl_test_video(&ww, &wh, &fs);
        CHECK(ww == 960 && wh == 600, "scale 1 -> window clamped to the 960x600 minimum");
        click("size2");
        break;
    case 4:
        kl_test_video(&ww, &wh, &fs);
        CHECK(ww == 960 && wh == 600, "scale 2 -> window 960x600 (minimum)");
        dump_frame("ui_settings.ppm");                      /* native 960x600: no resampling */
        click("size5");
        break;
    case 5:
        kl_test_video(&ww, &wh, &fs);
        CHECK(ww == 1200 && wh == 800, "scale 5 -> window 1200x800");
        click("fullscreen");
        break;
    case 6: {
        KirbyConfig c = now_cfg();
        CHECK(c.fullscreen == 1, "fullscreen saved");
        kl_test_video(&ww, &wh, &fs);
        CHECK(fs == 1, "the launcher window is full screen too");
        CHECK(!kl_test_enabled("size1") && !kl_test_enabled("size3") && !kl_test_enabled("size6"),
              "window size radios are disabled while Fullscreen is on");
        click("size3");                                     /* must be ignored */
        break;
    }
    case 7:
        CHECK(now_cfg().scale == 5, "clicking a disabled size does nothing");
        kl_test_set_focus("fullscreen");
        key(SDLK_TAB, 0);                                   /* focus must skip disabled widgets */
        break;
    case 8:
        CHECK(strncmp(kl_test_focus(), "size", 4) != 0, "keyboard focus skips disabled radios");
        click("scale_integer");
        break;
    case 9:
        CHECK(now_cfg().scaling == CFG_SCALING_INTEGER, "Integer saved");
        click("scale_stretch");
        break;
    case 10:
        CHECK(now_cfg().scaling == CFG_SCALING_STRETCH, "Stretch saved");
        click("scale_fit");
        break;
    case 11:
        CHECK(now_cfg().scaling == CFG_SCALING_FIT, "Fit saved");
        click("fullscreen");
        break;
    case 12:
        CHECK(now_cfg().fullscreen == 0, "fullscreen off saved");
        kl_test_video(&ww, &wh, &fs);
        CHECK(fs == 0 && ww == 1200 && wh == 800, "leaving full screen restores 240*scale x 160*scale");
        CHECK(kl_test_enabled("size3"), "size radios are enabled again");
        click("size3");
        break;
    case 13:
        kl_test_video(&ww, &wh, &fs);
        CHECK(now_cfg().scale == 3 && ww == 960 && wh == 600, "applies immediately: 3X -> 720x480, clamped to 960x600");
        break;
    default: break;
    }
    if (s >= 14) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }
}

/* ---------------------------------------------------------------- B: settings page: mute / volume / keyboard navigation */
static void scen_a(KlStatus *o, int s) {
    int x, y, w, h;
    set_status(o, 3, 100, "", "x.gba", 1);
    switch (s) {
    case 1:
        click("mute");
        break;
    case 2:
        CHECK(now_cfg().mute == 1, "mute saved");
        kl_test_widget("volume", &x, &y, &w, &h);
        {
            int wx, wy;
            to_window(x + w / 2, y + h / 2, &wx, &wy);
            click_win(wx, wy);                            /* 50% */
        }
        break;
    case 3: {
        KirbyConfig c = now_cfg();
        CHECK(c.volume >= 47 && c.volume <= 53, "volume slider click ~50%");
        kl_test_set_focus("size3");
        key(SDLK_RIGHT, 0);
        break;
    }
    case 4:
        CHECK(!strcmp(kl_test_focus(), "size4"), "Right moves the focus to the next size");
        CHECK(now_cfg().scale == 3, "Right alone does not change the size");
        key(SDLK_RETURN, 0);
        break;
    case 5:
        CHECK(now_cfg().scale == 4, "Enter selects the focused size");
        kl_test_set_focus("size4");
        key(SDLK_LEFT, 0);
        key(SDLK_LEFT, 0);
        key(SDLK_RETURN, 0);
        break;
    case 6:
        CHECK(now_cfg().scale == 2, "Left twice + Enter -> 2X");
        kl_test_set_focus("volume");
        key(SDLK_RIGHT, 0);
        key(SDLK_RIGHT, 0);
        break;
    case 7: {
        int v = now_cfg().volume;
        CHECK(v >= 57 && v <= 63, "Right on the slider adds 5 per press");
        click("done");
        break;
    }
    case 8:
        CHECK(!strcmp(kl_test_page(), "main"), "Done returns to the home page");
        click("quit");
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- B2: home page: status, Play, focus order */
static void scen_home(KlStatus *o, int s) {
    int x, y, w, h;
    if (s >= 7) set_status(o, 3, 100, "", "x.gba", 1);
    else if (s >= 3) set_status(o, 2, 40, "Compiling 4/16...", "x.gba", 1);
    else set_status(o, 1, 20, "Analyzing...", "x.gba", 1);
    switch (s) {
    case 2:
        kl_test_widget("play", &x, &y, &w, &h);
        CHECK(!kl_test_enabled("play"), "Play is disabled while state==1");
        CHECK(!pixel_is(x + 6, y + 6, ACC), "Play is drawn disabled while state==1");
        click("play");                                    /* must be ignored */
        kl_test_set_focus("quit");
        key(SDLK_TAB, 0);
        break;
    case 4:
        CHECK(!strcmp(kl_test_focus(), "settings"), "Tab skips the disabled Play (state 1 -> still preparing)");
        kl_test_widget("play", &x, &y, &w, &h);
        CHECK(pixel_is(x + 6, y + 6, ACC), "Play is drawn enabled in state 2");
        {
            int bx, by, bw, bh;
            CHECK(kl_test_bar(&bx, &by, &bw, &bh), "the progress bar is drawn in state 2");
            CHECK(count_color(ACC, bx, by, bx + bw, by + bh) > 600, "progress bar filled (state 2, 40%)");
        }
        CHECK(!kl_test_widget("selectrom", NULL, NULL, NULL, NULL), "no SELECT ROM when a verified ROM is loaded");
        break;
    case 8:
        kl_test_set_focus("quit");
        key(SDLK_TAB, 0);                                  /* quit -> wraps to the first widget */
        break;
    case 9:
        CHECK(!strcmp(kl_test_focus(), "play"), "Tab wraps around to Play");
        key(SDLK_TAB, 1);                                  /* Shift+Tab goes back to quit */
        break;
    case 10:
        CHECK(!strcmp(kl_test_focus(), "quit"), "Shift+Tab goes back");
        kl_test_set_focus("play");
        key(SDLK_RETURN, 0);
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- C: SELECT ROM... visibility */
static void scen_rom_vis(KlStatus *o, int s) {
    switch (s) {
    case 0: set_status(o, 3, 100, "", ROM1, 1); break;
    case 2:
        set_status(o, 3, 100, "", ROM1, 1);
        CHECK(!kl_test_widget("selectrom", NULL, NULL, NULL, NULL), "verified ROM (state 3): only the ROM chip");
        CHECK(count_color(OKC, 560, 16, 940, 44) > 20, "the ROM chip shows a green dot");
        break;
    case 3: set_status(o, 3, 100, "", ROM1, 0); break;
    case 4:
        set_status(o, 3, 100, "", ROM1, 0);
        CHECK(kl_test_widget("selectrom", NULL, NULL, NULL, NULL), "rom_ok==0 shows SELECT ROM...");
        break;
    case 5: set_status(o, 0, 0, "", ROM1, 1); break;
    case 6:
        set_status(o, 0, 0, "", ROM1, 1);
        CHECK(kl_test_widget("selectrom", NULL, NULL, NULL, NULL), "state==0 shows SELECT ROM...");
        break;
    case 7: set_status(o, 2, 10, "", ROM1, 1); break;
    case 8:
        set_status(o, 2, 10, "", ROM1, 1);
        CHECK(!kl_test_widget("selectrom", NULL, NULL, NULL, NULL), "state 2 + ok hides SELECT ROM...");
        key(SDLK_ESCAPE, 0);
        break;
    default: set_status(o, 2, 10, "", ROM1, 1); break;
    }
}

/* ---------------------------------------------------------------- D: Esc quits */
static void scen_esc(KlStatus *o, int s) {
    set_status(o, 3, 100, "", "x.gba", 1);
    if (s == 2) key(SDLK_ESCAPE, 0);
}

/* ---------------------------------------------------------------- E: Select ROM... */
static void scen_pick(KlStatus *o, int s) {
    set_status(o, 0, 0, "", "", 0);
    if (s == 2) click("selectrom");
}

/* ---------------------------------------------------------------- F: drop a file */
static void scen_drop(KlStatus *o, int s) {
    set_status(o, 0, 0, "", "", 0);
    if (s == 2) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_DROPFILE;
        e.drop.file = SDL_strdup("D:\\roms\\Kirby.gba");
        SDL_PushEvent(&e);
    }
}

/* ---------------------------------------------------------------- G: Play with state 0 asks for a ROM */
static void scen_play0(KlStatus *o, int s) {
    set_status(o, 0, 0, "", "", 0);
    if (s == 2) click("play");
}

/* ---------------------------------------------------------------- H: stretch mode maps clicks over the window */
static void scen_stretch(KlStatus *o, int s) {
    int ww, wh, fs, x, y, w, h;
    set_status(o, 3, 100, "", "x.gba", 1);
    if (s == 2) {
        kl_test_video(&ww, &wh, &fs);
        CHECK(ww == 960 && wh == 640, "scale 4 window (stretch test)");
        CHECK(now_cfg().scaling == CFG_SCALING_STRETCH, "ini scaling=stretch loaded");
        /* the whole window is the canvas: the bottom-right corner pixel belongs to the QUIT area */
        kl_test_widget("quit", &x, &y, &w, &h);
        click_win((x + w / 2) * ww / KL_W, (y + h / 2) * wh / KL_H);
    }
}

/* ---------------------------------------------------------------- I: cards, input page (Reset), settings page, Quit */
static void scen_pages(KlStatus *o, int s) {
    set_status(o, 3, 100, "", "x.gba", 1);
    switch (s) {
    case 2:
        CHECK(kl_test_widget("settings", NULL, NULL, NULL, NULL) && kl_test_widget("input", NULL, NULL, NULL, NULL) &&
              kl_test_enabled("online"), "the home page has the SETTINGS / INPUT / ONLINE cards");
        click("input");
        break;
    case 3: {
        KirbyConfig c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "Q") && !strcmp(c.pad[CFG_BTN_B], "y"), "preset bindings present");
        CHECK(!strcmp(kl_test_page(), "keyboard"), "the INPUT card opens the keyboard page");
        CHECK(kl_test_widget("back", NULL, NULL, NULL, NULL) && !kl_test_widget("play", NULL, NULL, NULL, NULL),
              "the input page has Back and no Play");
        click("reset");
        break;
    }
    case 4: {
        KirbyConfig c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z"), "Reset restores the keyboard map");
        CHECK(!strcmp(c.pad[CFG_BTN_B], "y"), "Reset on the keyboard tab leaves the controller alone");
        CHECK(c.scale == 6, "Reset does not touch video settings");
        key(SDLK_ESCAPE, 0);                              /* closes the page, does NOT quit */
        break;
    }
    case 5:
        CHECK(!strcmp(kl_test_page(), "main"), "Esc closes the input page");
        click("input");
        break;
    case 6:
        click("tab_controller");
        break;
    case 7:
        CHECK(!strcmp(kl_test_page(), "controller"), "the Controller tab opens the controller page");
        click("reset");
        break;
    case 8:
        CHECK(!strcmp(now_cfg().pad[CFG_BTN_A], "b") && !strcmp(now_cfg().pad[CFG_BTN_B], "a"), "Reset restores the controller map");
        click("back");
        break;
    case 9:
        CHECK(!strcmp(kl_test_page(), "main"), "Back returns to the home page");
        click("settings");
        break;
    case 10:
        CHECK(!strcmp(kl_test_page(), "settings"), "the SETTINGS card opens the settings page");
        CHECK(kl_test_widget("volume", NULL, NULL, NULL, NULL) && kl_test_widget("save_open", NULL, NULL, NULL, NULL),
              "settings: volume and save buttons");
        key(SDLK_ESCAPE, 0);
        break;
    case 11:
        CHECK(!strcmp(kl_test_page(), "main"), "Esc closes the settings page");
        click("quit");
        break;
    default: break;
    }
}

static void write_text(const char *path, const char *txt) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(ini, sizeof ini, "%s/test_ui.ini", dir);
    snprintf(shot_dir, sizeof shot_dir, "%s", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    kl_set_poll_interval(10);
    int rc;

    /* A: window size / fullscreen / scaling apply immediately and persist */
    remove(ini);
    rc = run_script(scen_video, KL_PAGE_SETTINGS);
    CHECK(rc == KL_QUIT, "video scenario ends with SDL_QUIT");
    {
        KirbyConfig c = now_cfg();
        CHECK(c.scale == 3 && c.fullscreen == 0 && c.scaling == CFG_SCALING_FIT, "final ini: 3X, windowed, fit");
    }

    /* B: mute / volume / keyboard navigation on the settings page */
    remove(ini);
    rc = run_script(scen_a, KL_PAGE_SETTINGS);
    CHECK(rc == KL_QUIT, "settings scenario ends with Quit");
    {
        KirbyConfig c = now_cfg();
        CHECK(c.scale == 2 && c.mute == 1 && c.volume >= 57, "final ini holds every change");
    }
    /* B2: the home page: status, Play, focus */
    rc = run_script(scen_home, KL_PAGE_MAIN);
    CHECK(rc == KL_PLAY, "Enter on Play returns KL_PLAY");
    printf("scenario B (settings/Play): rc=%d, polls=%d\n", rc, step);

    rc = run_script(scen_rom_vis, KL_PAGE_MAIN);
    CHECK(rc == KL_QUIT, "ROM visibility scenario ends with Esc");
    rc = run_script(scen_esc, KL_PAGE_MAIN);
    CHECK(rc == KL_QUIT, "Esc returns KL_QUIT");
    rc = run_script(scen_pick, KL_PAGE_MAIN);
    CHECK(rc == KL_PICK_ROM, "SELECT ROM... returns KL_PICK_ROM");
    rc = run_script(scen_drop, KL_PAGE_MAIN);
    CHECK(rc == KL_PICK_ROM, "dropping a file returns KL_PICK_ROM");
    CHECK(kl_dropped_path() && !strcmp(kl_dropped_path(), "D:\\roms\\Kirby.gba"), "kl_dropped_path has the path");
    rc = run_script(scen_play0, KL_PAGE_MAIN);
    CHECK(rc == KL_PICK_ROM, "Play with no ROM (state 0) asks for a ROM");
    CHECK(kl_dropped_path() == NULL, "no stale dropped path");

    write_text(ini, "[video]\r\nscale=4\r\nscaling=stretch\r\n");
    rc = run_script(scen_stretch, KL_PAGE_MAIN);
    CHECK(rc == KL_QUIT, "stretch: a click mapped over the whole window hits QUIT");

    write_text(ini, "[video]\r\nscale=6\r\n[keyboard]\r\na=Q\r\n[controller]\r\nb=y\r\n");
    rc = run_script(scen_pages, KL_PAGE_MAIN);
    CHECK(rc == KL_QUIT, "Quit button returns KL_QUIT");

    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    remove(ini);
    return fails != 0;
}
