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
static int alive_checks;
typedef void (*Script)(KlStatus *out, int step);
static Script script;

static void click_at(int x, int y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED;
    e.button.x = x; e.button.y = y; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static int click(const char *id) {
    int x, y, w, h;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); return 0; }
    click_at(x + w / 2, y + h / 2);
    return 1;
}

static void key(SDL_Keycode k, int shift) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED; e.key.keysym.sym = k;
    e.key.keysym.mod = shift ? KMOD_LSHIFT : 0;
    SDL_PushEvent(&e);
}

static KirbyConfig now_cfg(void) { KirbyConfig c; cfg_load(ini, &c); return c; }

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

static int run_script(Script s) {
    step = 0; script = s;
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

/* ---------------------------------------------------------------- scenario A: settings, Play disabled, Play */
static void scen_a(KlStatus *o, int s) {
    int x, y, w, h;
    const uint32_t ACC = 0xFFFF78AAu;
    if (s >= 5) set_status(o, 3, 100, "", "x.gba", 1);   /* the status persists until the script changes it */
    switch (s) {
    case 0: set_status(o, 1, 20, "Analyzing...", "C:\\roms\\Kirby & The Amazing Mirror (USA).gba", 1); break;
    case 1:
        set_status(o, 1, 20, "Analyzing...", "C:\\roms\\Kirby & The Amazing Mirror (USA).gba", 1);
        click("size4");
        break;
    case 2: {
        KirbyConfig c = now_cfg();
        CHECK(c.scale == 4, "clicking 4X saves scale=4");
        set_status(o, 1, 20, "Analyzing...", "x.gba", 1);
        kl_test_widget("play", &x, &y, &w, &h);
        CHECK(!pixel_is(x + 6, y + 6, ACC), "Play is drawn disabled while state==1");
        click("play");                                    /* must be ignored */
        click("fullscreen");
        click("integer");
        break;
    }
    case 3: {
        KirbyConfig c = now_cfg();
        CHECK(c.fullscreen == 1 && c.integer_scale == 0, "fullscreen on, integer scaling off saved");
        set_status(o, 2, 40, "Compiling 4/16...", "x.gba", 1);
        click("mute");
        kl_test_widget("volume", &x, &y, &w, &h);
        click_at(x + w / 2, y + h / 2);                    /* 50% */
        break;
    }
    case 4: {
        KirbyConfig c = now_cfg();
        CHECK(c.mute == 1, "mute saved");
        CHECK(c.volume >= 47 && c.volume <= 53, "volume slider click ~50%");
        set_status(o, 2, 40, "Compiling 4/16...", "x.gba", 1);
        CHECK(count_color(ACC, 30, 118, 610, 138) > 1000, "progress bar filled (state 2, 40%)");
        kl_test_widget("play", &x, &y, &w, &h);
        CHECK(pixel_is(x + 6, y + 6, ACC), "Play is drawn enabled in state 2");
        {   /* dump the frame for a visual check */
            static uint32_t px[KL_W * KL_H];
            int iw, ih;
            char path[600];
            snprintf(path, sizeof path, "%s/ui_launcher.ppm", shot_dir);
            FILE *f = kl_test_snapshot(px, KL_W * KL_H, &iw, &ih) == 0 ? fopen(path, "wb") : NULL;
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", iw, ih);
                for (int i = 0; i < iw * ih; i++) { fputc(px[i] >> 16 & 255, f); fputc(px[i] >> 8 & 255, f); fputc(px[i] & 255, f); }
                fclose(f);
            }
        }
        /* keyboard: focus the 3X radio, Right moves to 4X and selects it, Left twice -> 2X */
        kl_test_set_focus("size3");
        key(SDLK_RIGHT, 0);
        break;
    }
    case 5: {
        CHECK(!strcmp(kl_test_focus(), "size4"), "Right moves the focus to the next radio");
        CHECK(now_cfg().scale == 4, "Right on a radio selects it");
        key(SDLK_LEFT, 0);
        key(SDLK_LEFT, 0);
        set_status(o, 3, 100, "", "x.gba", 1);
        break;
    }
    case 6: {
        CHECK(now_cfg().scale == 2, "Left twice -> 2X");
        set_status(o, 3, 100, "", "x.gba", 1);
        kl_test_set_focus("volume");
        key(SDLK_RIGHT, 0);
        key(SDLK_RIGHT, 0);
        break;
    }
    case 7: {
        int v = now_cfg().volume;
        CHECK(v >= 57 && v <= 63, "Right on the slider adds 5 per press");
        kl_test_set_focus("quit");
        key(SDLK_TAB, 0);                                  /* quit -> wraps to selectrom */
        break;
    }
    case 8:
        CHECK(!strcmp(kl_test_focus(), "selectrom"), "Tab wraps around");
        key(SDLK_TAB, 1);                                  /* Shift+Tab goes back to quit */
        break;
    case 9:
        CHECK(!strcmp(kl_test_focus(), "quit"), "Shift+Tab goes back");
        kl_test_set_focus("play");
        key(SDLK_RETURN, 0);
        break;
    default:
        alive_checks++;                                    /* should not get here: Play already returned */
        break;
    }
}

/* ---------------------------------------------------------------- scenario B: Esc quits */
static void scen_esc(KlStatus *o, int s) {
    set_status(o, 3, 100, "", "x.gba", 1);
    if (s == 2) key(SDLK_ESCAPE, 0);
}

/* ---------------------------------------------------------------- scenario C: Select ROM... */
static void scen_pick(KlStatus *o, int s) {
    set_status(o, 0, 0, "", "", 0);
    if (s == 2) click("selectrom");
}

/* ---------------------------------------------------------------- scenario D: drop a file */
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

/* ---------------------------------------------------------------- scenario E: Play with state 0 asks for a ROM */
static void scen_play0(KlStatus *o, int s) {
    set_status(o, 0, 0, "", "", 0);
    if (s == 2) click("play");
}

/* ---------------------------------------------------------------- scenario F: Reset + info pages + Quit */
static void scen_pages(KlStatus *o, int s) {
    set_status(o, 3, 100, "", "x.gba", 1);
    switch (s) {
    case 2: {
        KirbyConfig c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "Q") && !strcmp(c.pad[CFG_BTN_B], "y"), "preset bindings present");
        click("reset");
        break;
    }
    case 3: {
        KirbyConfig c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z") && !strcmp(c.pad[CFG_BTN_A], "b"),
              "Reset to defaults restores keyboard and controller maps");
        CHECK(c.scale == 6, "Reset does not touch video settings");
        click("keyboard");
        break;
    }
    case 4:
        CHECK(!strcmp(kl_test_page(), "keyboard"), "Keyboard... opens the bindings page");
        CHECK(kl_test_widget("back", NULL, NULL, NULL, NULL) && !kl_test_widget("play", NULL, NULL, NULL, NULL),
              "info page has only Back");
        key(SDLK_ESCAPE, 0);                              /* closes the page, does NOT quit */
        break;
    case 5:
        CHECK(!strcmp(kl_test_page(), "main"), "Esc closes the info page");
        click("controller");
        break;
    case 6:
        CHECK(!strcmp(kl_test_page(), "controller"), "Controller... opens the controller page");
        click("back");
        break;
    case 7:
        CHECK(!strcmp(kl_test_page(), "main"), "Back returns to the main page");
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

    remove(ini);
    rc = run_script(scen_a);
    CHECK(rc == KL_PLAY, "Enter on Play returns KL_PLAY");
    CHECK(alive_checks == 0, "kl_run returned right after Play");
    {
        KirbyConfig c = now_cfg();
        CHECK(c.scale == 2 && c.fullscreen == 1 && c.integer_scale == 0 && c.mute == 1 && c.volume >= 57,
              "final ini holds every change");
    }
    printf("scenario A (settings/Play): rc=%d, polls=%d\n", rc, step);

    rc = run_script(scen_esc);
    CHECK(rc == KL_QUIT, "Esc returns KL_QUIT");
    rc = run_script(scen_pick);
    CHECK(rc == KL_PICK_ROM, "Select ROM... returns KL_PICK_ROM");
    rc = run_script(scen_drop);
    CHECK(rc == KL_PICK_ROM, "dropping a file returns KL_PICK_ROM");
    CHECK(kl_dropped_path() && !strcmp(kl_dropped_path(), "D:\\roms\\Kirby.gba"), "kl_dropped_path has the path");
    rc = run_script(scen_play0);
    CHECK(rc == KL_PICK_ROM, "Play with no ROM (state 0) asks for a ROM");
    CHECK(kl_dropped_path() == NULL, "no stale dropped path");

    write_text(ini, "[video]\r\nscale=6\r\n[keyboard]\r\na=Q\r\n[controller]\r\nb=y\r\n");
    rc = run_script(scen_pages);
    CHECK(rc == KL_QUIT, "Quit button returns KL_QUIT");

    /* snapshot of the main screen for a visual check */
    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    remove(ini);
    return fails != 0;
}
