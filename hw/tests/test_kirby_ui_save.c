/* test_kirby_ui_save.c - the SAVE section of the launcher (kirby_ui.dll), headless: OPEN FOLDER / EXPORT... /
 * IMPORT... with their return codes, the "overwrite current save?" confirmation, keyboard and controller
 * navigation, and layout checks (nothing overlaps: widgets, free texts, the progress bar, the dialog).
 * Every simulated event carries the SDL window id. */
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
static int cur_state = 3, cur_pct = 100, cur_ok = 1;
static const char *cur_rom = "C:\\roms\\Kirby.gba";
static const char *cur_text = "";

static void to_window(int lx, int ly, int *wx, int *wy) {      /* fit mode */
    int ww, wh, fs;
    kl_test_video(&ww, &wh, &fs);
    float s = (float)ww / KL_W < (float)wh / KL_H ? (float)ww / KL_W : (float)wh / KL_H;
    *wx = (int)((ww - KL_W * s) / 2 + lx * s);
    *wy = (int)((wh - KL_H * s) / 2 + ly * s);
}

static void click_win(int x, int y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = kl_test_window_id();
    e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED; e.button.x = x; e.button.y = y; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static int click(const char *id) {
    int x, y, w, h, wx, wy;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); return 0; }
    to_window(x + w / 2, y + h / 2, &wx, &wy);
    click_win(wx, wy);
    return 1;
}

static void key(SDL_Keycode k) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.windowID = kl_test_window_id(); e.key.state = SDL_PRESSED; e.key.keysym.sym = k;
    SDL_PushEvent(&e);
}

static void pad_button(int b) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.which = 0; e.cbutton.button = (Uint8)b; e.cbutton.state = SDL_PRESSED;
    SDL_PushEvent(&e);
}

static void set_status(KlStatus *o) {
    memset(o, 0, sizeof *o);
    o->state = cur_state; o->percent = cur_pct; o->rom_ok = cur_ok;
    snprintf(o->rom, sizeof o->rom, "%s", cur_rom);
    snprintf(o->text, sizeof o->text, "%s", cur_text);
}

static void poll_cb(KlStatus *out) {
    set_status(out);
    script(out, step++);
    if (step > 200) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }
}

static int run_script(Script s, int pg) { step = 0; script = s; kl_set_start_page(pg); return kl_run(ini, poll_cb); }
static void quit_now(void) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }

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

/* ---------------------------------------------------------------- layout checks */
static const char *const HOME_IDS[] = { "selectrom", "play", "settings", "input", "online", "quit" };
static const char *const SET_IDS[] = { "back", "size1", "size2", "size3", "size4", "size5", "size6", "fullscreen", "scale_integer",
    "scale_fit", "scale_stretch", "volume", "mute", "save_open", "save_export", "save_import", "done" };
static const char *const *page_ids = HOME_IDS;
static int page_n = 6;
#define NMAIN page_n
#define MAIN_IDS page_ids

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* returns the number of problems found on the current main-page frame; prints each one */
static int layout_problems(const char *tag) {
    int bad = 0, nr = 0;
    int rx[40], ry[40], rw[40], rh[40];
    const char *rn[40];
    for (int i = 0; i < NMAIN; i++) {
        int x, y, w, h;
        if (!kl_test_widget(MAIN_IDS[i], &x, &y, &w, &h)) continue;
        rx[nr] = x; ry[nr] = y; rw[nr] = w; rh[nr] = h; rn[nr] = MAIN_IDS[i]; nr++;
        if (x < 0 || y < 0 || x + w > KL_W || y + h > KL_H) { printf("  [%s] widget %s outside the canvas\n", tag, MAIN_IDS[i]); bad++; }
    }
    int bx, by, bw, bh, has_bar = kl_test_bar(&bx, &by, &bw, &bh);
    for (int i = 0; i < nr; i++) {
        for (int j = i + 1; j < nr; j++)
            if (overlap(rx[i], ry[i], rw[i], rh[i], rx[j], ry[j], rw[j], rh[j])) {
                printf("  [%s] widgets %s and %s overlap\n", tag, rn[i], rn[j]); bad++;
            }
        if (has_bar && overlap(rx[i], ry[i], rw[i], rh[i], bx, by, bw, bh)) {
            printf("  [%s] widget %s overlaps the progress bar\n", tag, rn[i]); bad++;
        }
    }
    int nt = kl_test_text_count();
    for (int i = 0; i < nt; i++) {
        int tx, ty, tw, th;
        kl_test_text(i, &tx, &ty, &tw, &th);
        if (tx < 0 || ty < 0 || tx + tw > KL_W || ty + th > KL_H) {
            printf("  [%s] text %d (%d,%d %dx%d) leaves the canvas\n", tag, i, tx, ty, tw, th); bad++;
        }
        for (int j = 0; j < nr; j++)
            if (overlap(tx, ty, tw, th, rx[j], ry[j], rw[j], rh[j])) {
                printf("  [%s] text %d (%d,%d %dx%d) is behind widget %s\n", tag, i, tx, ty, tw, th, rn[j]); bad++;
            }
        if (has_bar && overlap(tx, ty, tw, th, bx, by, bw, bh)) {
            printf("  [%s] text %d (%d,%d %dx%d) is behind the progress bar\n", tag, i, tx, ty, tw, th); bad++;
        }
        for (int j = i + 1; j < nt; j++) {
            int ux, uy, uw, uh;
            kl_test_text(j, &ux, &uy, &uw, &uh);
            if (overlap(tx, ty, tw, th, ux, uy, uw, uh)) {
                printf("  [%s] texts %d (%d,%d %dx%d) and %d (%d,%d %dx%d) overlap\n", tag, i, tx, ty, tw, th, j, ux, uy, uw, uh); bad++;
            }
        }
    }
    return bad;
}

/* ---------------------------------------------------------------- A: enabled / disabled by ROM */
static void scen_enabled(KlStatus *o, int s) {
    (void)o;
    switch (s) {
    case 2:
        CHECK(kl_test_enabled("save_open") && kl_test_enabled("save_export") && kl_test_enabled("save_import"),
              "SAVE buttons are enabled with a verified ROM");
        CHECK(count_color(0xFFE8E9FFu, 597, 296, 709, 328) > 20, "enabled button drawn with the bright label");
        dump_frame("ui_save.ppm");
        cur_state = 0; cur_ok = 0; cur_rom = "";
        break;
    case 4:
        CHECK(!kl_test_enabled("save_open") && !kl_test_enabled("save_export") && !kl_test_enabled("save_import"),
              "SAVE buttons are disabled with no ROM");
        CHECK(count_color(0xFFE8E9FFu, 597, 296, 709, 328) == 0, "disabled button is drawn greyed out (no bright pixels)");
        kl_test_set_focus("size1");
        click("save_import");
        key(SDLK_RETURN);
        break;
    case 6:
        CHECK(!kl_test_confirm(), "a disabled IMPORT does not open the confirmation");
        cur_state = 3; cur_ok = 0; cur_rom = "C:\\roms\\not_a_kirby.gba";
        break;
    case 8:
        CHECK(!kl_test_enabled("save_open") && !kl_test_enabled("save_export") && !kl_test_enabled("save_import"),
              "SAVE buttons are disabled when the ROM is not verified");
        cur_state = 3; cur_ok = 1; cur_rom = "C:\\roms\\Kirby.gba";
        break;
    case 10:
        CHECK(kl_test_enabled("save_import"), "enabled again once a verified ROM is loaded");
        quit_now();
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- B: return codes */
static void scen_open(KlStatus *o, int s) { (void)o; if (s == 2) click("save_open"); }
static void scen_export(KlStatus *o, int s) { (void)o; if (s == 2) click("save_export"); }

/* ---------------------------------------------------------------- C: import + confirmation (mouse) */
static void scen_import_yes(KlStatus *o, int s) {
    (void)o;
    switch (s) {
    case 2: click("save_import"); break;
    case 3:
        CHECK(kl_test_confirm(), "IMPORT opens the confirmation inside the UI");
        CHECK(kl_test_widget("confirm_yes", NULL, NULL, NULL, NULL) && kl_test_widget("confirm_no", NULL, NULL, NULL, NULL),
              "dialog has YES and NO");
        CHECK(!kl_test_widget("play", NULL, NULL, NULL, NULL) && !kl_test_widget("save_open", NULL, NULL, NULL, NULL),
              "the screen behind the dialog is not clickable (modal)");
        CHECK(!strcmp(kl_test_focus(), "confirm_no"), "focus starts on NO (the safe answer)");
        dump_frame("ui_save_confirm.ppm");
        click("confirm_no");
        break;
    case 4:
        CHECK(!kl_test_confirm() && kl_test_widget("save_import", NULL, NULL, NULL, NULL), "NO closes the dialog and keeps the screen");
        click("save_import");
        break;
    case 5:
        CHECK(kl_test_confirm(), "IMPORT opens the dialog again");
        click("confirm_yes");
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- D: dialog by keyboard */
static void scen_import_keys(KlStatus *o, int s) {
    (void)o;
    switch (s) {
    case 2: kl_test_set_focus("save_import"); key(SDLK_RETURN); break;
    case 3:
        CHECK(kl_test_confirm() && !strcmp(kl_test_focus(), "confirm_no"), "Enter on IMPORT opens the dialog on NO");
        key(SDLK_ESCAPE);                                  /* Esc closes the dialog, it does not quit */
        break;
    case 4:
        CHECK(!kl_test_confirm(), "Esc closes the dialog");
        kl_test_set_focus("save_import");
        key(SDLK_SPACE);
        break;
    case 5:
        CHECK(kl_test_confirm(), "Space opens it again");
        key(SDLK_RETURN);                                  /* Enter on NO = closes */
        break;
    case 6:
        CHECK(!kl_test_confirm(), "Enter on the default NO closes the dialog");
        kl_test_set_focus("save_import");
        key(SDLK_RETURN);
        break;
    case 7:
        key(SDLK_LEFT);                                    /* NO -> YES */
        break;
    case 8:
        CHECK(!strcmp(kl_test_focus(), "confirm_yes"), "Left moves to YES");
        key(SDLK_RETURN);
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- E: dialog by controller */
static void scen_import_pad(KlStatus *o, int s) {
    (void)o;
    switch (s) {
    case 2: kl_test_set_focus("save_import"); pad_button(SDL_CONTROLLER_BUTTON_A); break;       /* south */
    case 3:
        CHECK(kl_test_confirm(), "south on IMPORT opens the dialog");
        pad_button(SDL_CONTROLLER_BUTTON_B);                                                   /* east = NO / back */
        break;
    case 4:
        CHECK(!kl_test_confirm(), "east closes the dialog");
        kl_test_set_focus("save_import");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    case 5:
        pad_button(SDL_CONTROLLER_BUTTON_A);               /* south on the default NO */
        break;
    case 6:
        CHECK(!kl_test_confirm(), "south on the default NO closes the dialog");
        kl_test_set_focus("save_import");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    case 7: pad_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT); break;
    case 8:
        CHECK(!strcmp(kl_test_focus(), "confirm_yes"), "d-pad Left moves to YES");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- F: navigation through the SAVE row */
static void scen_nav(KlStatus *o, int s) {
    (void)o;
    switch (s) {
    case 2: kl_test_set_focus("mute"); key(SDLK_DOWN); break;
    case 3:
        CHECK(strncmp(kl_test_focus(), "save_", 5) == 0, "Down from MUTE reaches the save buttons");
        kl_test_set_focus("save_open");
        key(SDLK_RIGHT);
        break;
    case 4:
        CHECK(!strcmp(kl_test_focus(), "save_export"), "Right moves to EXPORT...");
        key(SDLK_TAB);
        break;
    case 5:
        CHECK(!strcmp(kl_test_focus(), "save_import"), "Tab moves to IMPORT...");
        key(SDLK_TAB);
        break;
    case 6:
        CHECK(!strcmp(kl_test_focus(), "done"), "Tab continues to DONE");
        pad_button(SDL_CONTROLLER_BUTTON_DPAD_UP);
        break;
    case 7:
        CHECK(!strcmp(kl_test_focus(), "save_import"), "controller d-pad Up goes back to IMPORT...");
        quit_now();
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- G: layout in many states */
typedef struct { int state, pct, ok, scale, fullscreen; const char *rom, *text; const char *tag; } Case;
static const Case CASES[] = {
    { 0, 0, 0, 3, 0, "", "", "no ROM" },
    { 1, 35, 1, 3, 0, "C:\\roms\\Kirby & The Amazing Mirror (USA).gba", "Analyzing the ROM", "preparing" },
    { 2, 63, 1, 3, 0, "C:\\roms\\Kirby & The Amazing Mirror (USA).gba", "Compiling 10/16 (this text is far too long to fit)", "building, long text" },
    { 3, 100, 1, 3, 0, "C:\\roms\\Kirby.gba", "", "ready" },
    { 3, 100, 0, 3, 0, "C:\\a\\very\\long\\folder\\name\\with\\many\\parts\\Kirby & The Amazing Mirror (USA) [hack v2].gba", "", "unverified, long name" },
    { 4, 0, 1, 3, 0, "x.gba", "Compilation failed: see build.log for details of the error", "error" },
    { 2, 100, 1, 1, 0, "x.gba", "Almost done", "scale 1" },
    { 2, 50, 1, 5, 1, "x.gba", "Fullscreen", "fullscreen" },
};
#define NCASES ((int)(sizeof CASES / sizeof CASES[0]))
static int case_idx, case_lo, case_hi;
static int layout_total;

static void scen_layout(KlStatus *o, int s) {
    (void)o;
    if (s == 0) { case_idx = case_lo; return; }
    const Case *c = &CASES[case_idx];
    cur_state = c->state; cur_pct = c->pct; cur_ok = c->ok; cur_rom = c->rom; cur_text = c->text;
    if (s == 3 + (case_idx - case_lo) * 3) {      /* the case was set 2 polls ago: it has been drawn */
        CHECK(kl_test_text_count() >= 6, "the layout check really sees the free texts of the frame");
        if (page_ids == HOME_IDS)
            CHECK(kl_test_bar(NULL, NULL, NULL, NULL) == (c->state == 1 || c->state == 2), "progress bar drawn only in states 1 and 2");
        int bad = layout_problems(c->tag);
        layout_total += bad;
        CHECK(bad == 0, c->tag);
        if (case_idx == 4) dump_frame("ui_save_long.ppm");
        if (case_idx == 2) dump_frame("ui_save_building.ppm");
        case_idx++;
        if (case_idx >= case_hi) quit_now();
    }
}

static void write_text(const char *path, const char *txt) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(ini, sizeof ini, "%s/test_ui_save.ini", dir);
    snprintf(shot_dir, sizeof shot_dir, "%s", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    kl_set_poll_interval(10);
    int rc;

    /* the overlap helper itself (a layout checker that cannot fail would prove nothing) */
    CHECK(overlap(0, 0, 10, 10, 5, 5, 10, 10) && !overlap(0, 0, 10, 10, 10, 0, 5, 5) && !overlap(0, 0, 10, 10, 0, 10, 5, 5) &&
          overlap(0, 0, 10, 10, 9, 9, 1, 1), "overlap() detects touching-by-area but not edge-to-edge");

    remove(ini);
    rc = run_script(scen_enabled, KL_PAGE_SETTINGS);
    CHECK(rc == KL_QUIT, "enabled/disabled scenario ends with SDL_QUIT");

    cur_state = 3; cur_ok = 1; cur_rom = "C:\\roms\\Kirby.gba"; cur_text = "";
    rc = run_script(scen_open, KL_PAGE_SETTINGS);
    CHECK(rc == KL_OPEN_SAVE_DIR && rc == 3, "OPEN FOLDER returns KL_OPEN_SAVE_DIR (3)");
    rc = run_script(scen_export, KL_PAGE_SETTINGS);
    CHECK(rc == KL_EXPORT_SAVE && rc == 4, "EXPORT... returns KL_EXPORT_SAVE (4)");
    rc = run_script(scen_import_yes, KL_PAGE_SETTINGS);
    CHECK(rc == KL_IMPORT_SAVE && rc == 5, "IMPORT... then YES returns KL_IMPORT_SAVE (5)");
    rc = run_script(scen_import_keys, KL_PAGE_SETTINGS);
    CHECK(rc == KL_IMPORT_SAVE, "IMPORT... then YES by keyboard returns KL_IMPORT_SAVE");
    rc = run_script(scen_import_pad, KL_PAGE_SETTINGS);
    CHECK(rc == KL_IMPORT_SAVE, "IMPORT... then YES by controller returns KL_IMPORT_SAVE");
    rc = run_script(scen_nav, KL_PAGE_SETTINGS);
    CHECK(rc == KL_QUIT, "navigation scenario ends with SDL_QUIT");

    /* layout in many states, on the home page and on the settings page */
    for (int pg = 0; pg < 2; pg++) {
        page_ids = pg ? SET_IDS : HOME_IDS;
        page_n = pg ? (int)(sizeof SET_IDS / sizeof SET_IDS[0]) : (int)(sizeof HOME_IDS / sizeof HOME_IDS[0]);
        int start = pg ? KL_PAGE_SETTINGS : KL_PAGE_MAIN;
        write_text(ini, "[video]\r\nscale=3\r\n");
        case_lo = 0; case_hi = 6;
        rc = run_script(scen_layout, start);
        CHECK(rc == KL_QUIT, "layout scenario (default video) ends with SDL_QUIT");
        write_text(ini, "[video]\r\nscale=1\r\n");
        case_lo = 6; case_hi = 7;
        rc = run_script(scen_layout, start);
        CHECK(rc == KL_QUIT, "layout scenario (1X) ends with SDL_QUIT");
        write_text(ini, "[video]\r\nscale=5\r\nfullscreen=1\r\nscaling=stretch\r\n");
        case_lo = 7; case_hi = 8;
        rc = run_script(scen_layout, start);
        CHECK(rc == KL_QUIT, "layout scenario (fullscreen, stretch) ends with SDL_QUIT");
    }
    CHECK(layout_total == 0, "no overlapping widgets / texts / progress bar in any state");

    printf("save section: codes 3/4/5 verified, %d layout cases checked\n", NCASES);
    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    remove(ini);
    return fails != 0;
}
