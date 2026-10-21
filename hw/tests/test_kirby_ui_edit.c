/* test_kirby_ui_edit.c - the editable KEYBOARD / CONTROLLER pages of the launcher (kirby_ui.dll), headless.
 * Every simulated event carries the SDL window id (like real events) and one test sends events with a
 * foreign id to make sure they are ignored. Controller events are simulated (no device needed). */
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
static int real_pad;               /* whether this machine really has a controller connected */
typedef void (*Script)(KlStatus *out, int step);
static Script script;

static KirbyConfig now_cfg(void) { KirbyConfig c; cfg_load(ini, &c); return c; }

static void to_window(int lx, int ly, int *wx, int *wy) {      /* fit mode: s = min(ww/480, wh/320) */
    int ww, wh, fs;
    kl_test_video(&ww, &wh, &fs);
    float s = (float)ww / KL_W < (float)wh / KL_H ? (float)ww / KL_W : (float)wh / KL_H;
    *wx = (int)((ww - KL_W * s) / 2 + lx * s);
    *wy = (int)((wh - KL_H * s) / 2 + ly * s);
}

static void click_win(int x, int y, unsigned wid) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = wid; e.button.which = 0;
    e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED; e.button.x = x; e.button.y = y; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static int click_to(const char *id, unsigned wid) {
    int x, y, w, h, wx, wy;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); return 0; }
    to_window(x + w / 2, y + h / 2, &wx, &wy);
    click_win(wx, wy, wid);
    return 1;
}
static int click(const char *id) { return click_to(id, kl_test_window_id()); }

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

static void pad_axis(int axis, int value) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_CONTROLLERAXISMOTION; e.caxis.which = 0; e.caxis.axis = (Uint8)axis; e.caxis.value = (Sint16)value;
    SDL_PushEvent(&e);
}

static void set_status(KlStatus *o) {
    memset(o, 0, sizeof *o);
    o->state = 3; o->percent = 100; o->rom_ok = 1;
    snprintf(o->rom, sizeof o->rom, "%s", "x.gba");
}

static void poll_cb(KlStatus *out) {
    set_status(out);
    script(out, step++);
    if (step > 200) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }
}

static int run_script(Script s, int pg) { step = 0; script = s; kl_set_start_page(pg); return kl_run(ini, poll_cb); }

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

/* ---------------------------------------------------------------- A: keyboard remapping */
static void scen_keyboard(KlStatus *o, int s) {
    (void)o;
    KirbyConfig c;
    switch (s) {
    case 2:
        CHECK(!strcmp(kl_test_page(), "keyboard"), "the keyboard page opens");
        CHECK(kl_test_widget("btn_a", NULL, NULL, NULL, NULL) && kl_test_widget("btn_start", NULL, NULL, NULL, NULL) &&
              kl_test_widget("btn_l", NULL, NULL, NULL, NULL) && kl_test_widget("btn_right", NULL, NULL, NULL, NULL),
              "the GBA outline has A B L R Start Select and the d-pad as buttons");
        dump_frame("ui_keyboard.ppm");
        click("btn_a");
        break;
    case 3:
        CHECK(kl_test_capture() == CFG_BTN_A, "clicking A starts capturing for A");
        key(SDLK_q);
        break;
    case 4:
        c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "Q"), "Q saved to [keyboard] a=Q immediately");
        CHECK(kl_test_capture() == -1, "capture ends after the key");
        click("btn_b");
        break;
    case 5:
        CHECK(kl_test_capture() == CFG_BTN_B, "capturing for B");
        key(SDLK_q);                                       /* Q is A's key: swap */
        break;
    case 6:
        c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_B], "Q") && !strcmp(c.key[CFG_BTN_A], "Z"), "a used key swaps the two bindings (A gets B's old Z)");
        CHECK(strstr(kl_test_note(), "swapped") != NULL, "the swap is announced");
        click("btn_start");
        break;
    case 7:
        key(SDLK_F11);
        break;
    case 8:
        CHECK(kl_test_capture() == CFG_BTN_START, "a reserved key keeps capturing");
        CHECK(strstr(kl_test_note(), "reserved") != NULL, "F11 is refused with a warning");
        CHECK(!strcmp(now_cfg().key[CFG_BTN_START], "Return"), "binding unchanged after the refusal");
        key(SDLK_F1);
        break;
    case 9:
        CHECK(kl_test_capture() == CFG_BTN_START && strstr(kl_test_note(), "F1") != NULL, "F1 is refused too");
        key(SDLK_ESCAPE);                                  /* cancels the capture, does not leave the page */
        break;
    case 10:
        CHECK(kl_test_capture() == -1 && !strcmp(kl_test_page(), "keyboard"), "Esc cancels the capture and stays on the page");
        CHECK(!strcmp(now_cfg().key[CFG_BTN_START], "Return"), "cancelled: binding unchanged");
        kl_test_set_focus("btn_l");
        key(SDLK_RETURN);
        break;
    case 11:
        CHECK(kl_test_capture() == CFG_BTN_L, "Enter on the focused button starts capturing");
        key(SDLK_SPACE);
        break;
    case 12:
        CHECK(!strcmp(now_cfg().key[CFG_BTN_L], "Space"), "Space saved as L");
        click("reset");
        break;
    case 13:
        c = now_cfg();
        CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z") && !strcmp(c.key[CFG_BTN_L], "A") &&
              !strcmp(c.key[CFG_BTN_R], "S") && !strcmp(c.key[CFG_BTN_START], "Return") &&
              !strcmp(c.key[CFG_BTN_SELECT], "Backspace") && !strcmp(c.key[CFG_BTN_UP], "Up"),
              "RESET restores the positional defaults (X=A Z=B A=L S=R Enter Backspace arrows)");
        /* spatial navigation: from A, Left reaches B; from B, Up reaches R */
        kl_test_set_focus("btn_a");
        key(SDLK_LEFT);
        break;
    case 14:
        CHECK(!strcmp(kl_test_focus(), "btn_b"), "Left from A lands on B (spatial navigation)");
        kl_test_set_focus("btn_a");
        key(SDLK_UP);
        break;
    case 15:
        CHECK(!strcmp(kl_test_focus(), "btn_r"), "Up from A lands on the nearest widget above (R)");
        key(SDLK_LEFT);
        break;
    case 16:
        CHECK(strcmp(kl_test_focus(), "btn_r") != 0, "Left from R moves the focus");
        kl_test_set_focus("btn_up");
        key(SDLK_DOWN);
        break;
    case 17:
        CHECK(!strcmp(kl_test_focus(), "btn_down") || !strcmp(kl_test_focus(), "btn_select"), "Down from the d-pad up arm goes down");
        /* events from another window id are ignored */
        click_to("tab_controller", kl_test_window_id() + 1000);
        break;
    case 18:
        CHECK(!strcmp(kl_test_page(), "keyboard"), "a click with a foreign window id is ignored");
        click("tab_controller");
        break;
    case 19:
        CHECK(!strcmp(kl_test_page(), "controller"), "the CONTROLLER tab switches the page");
        click("tab_keyboard");
        break;
    case 20:
        CHECK(!strcmp(kl_test_page(), "keyboard"), "the KEYBOARD tab switches back");
        key(SDLK_ESCAPE);
        break;
    case 21:
        CHECK(!strcmp(kl_test_page(), "main"), "Esc (not capturing) leaves the editor");
        click("input");
        break;
    case 22:
        click("btn_select");                               /* then click elsewhere: cancels */
        break;
    case 23:
        CHECK(kl_test_capture() == CFG_BTN_SELECT, "capturing for Select");
        click("btn_up");
        break;
    case 24:
        CHECK(kl_test_capture() == -1 || kl_test_capture() == CFG_BTN_UP, "a click on another button moves/cancels the capture");
        key(SDLK_ESCAPE);
        break;
    default:
        if (s >= 25) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }
        break;
    }
}

/* ---------------------------------------------------------------- B: controller page, stick, live indicator */
static void scen_controller(KlStatus *o, int s) {
    (void)o;
    KirbyConfig c;
    switch (s) {
    case 2:
        CHECK(!strcmp(kl_test_page(), "controller"), "the controller page opens");
        real_pad = kl_test_pad_present();                  /* a real gamepad may be plugged in on a dev machine */
        kl_test_fake_pad(0, 0, 0);                         /* force "no controller" for the test */
        CHECK(!kl_test_pad_present(), "fake override: no controller");
        click("btn_a");
        break;
    case 3:
        CHECK(kl_test_capture() == -1 && strstr(kl_test_note(), "No controller") != NULL,
              "without a controller the capture is refused with a warning");
        dump_frame("ui_controller_none.ppm");
        kl_test_fake_pad(1, 0, 0);                         /* pretend one is connected */
        click("btn_a");
        break;
    case 4:
        CHECK(kl_test_capture() == CFG_BTN_A, "capturing a controller button for A");
        key(SDLK_q);                                       /* keyboard keys do not count here */
        break;
    case 5:
        CHECK(kl_test_capture() == CFG_BTN_A && !strcmp(now_cfg().pad[CFG_BTN_A], "b"), "keyboard keys are ignored while waiting for a button");
        pad_button(SDL_CONTROLLER_BUTTON_X);
        break;
    case 6:
        c = now_cfg();
        CHECK(!strcmp(c.pad[CFG_BTN_A], "x"), "button X saved to [controller] a=x");
        click("btn_b");
        break;
    case 7:
        pad_button(SDL_CONTROLLER_BUTTON_X);               /* used by A: swap */
        break;
    case 8:
        c = now_cfg();
        CHECK(!strcmp(c.pad[CFG_BTN_B], "x") && !strcmp(c.pad[CFG_BTN_A], "a"), "used button swaps (A gets B's old 'a')");
        click("btn_start");
        break;
    case 9:
        key(SDLK_ESCAPE);                                  /* Esc cancels a button capture too */
        break;
    case 10:
        CHECK(kl_test_capture() == -1 && !strcmp(kl_test_page(), "controller"), "Esc cancels the controller capture");
        click("stick");
        break;
    case 11:
        CHECK(now_cfg().stick_as_dpad == 0, "LEFT STICK AS D-PAD unchecked and saved");
        {
            int x, y, w, h, wx, wy;
            kl_test_widget("deadzone", &x, &y, &w, &h);
            to_window(x + w / 2, y + h / 2, &wx, &wy);
            click_win(wx, wy, kl_test_window_id());        /* 50% of 0..32767 */
        }
        break;
    case 12:
        c = now_cfg();
        CHECK(c.deadzone > 15500 && c.deadzone < 17300, "DEADZONE slider click ~50% of 32767 saved");
        kl_test_fake_pad(1, 30000, 0);                     /* far right: outside the dead zone */
        break;
    case 13: {
        /* indicator centre (880,508) radius 28; dot at +30000/32767*28 = +25 px, green when outside */
        CHECK(count_color(0xFF7EE0A0u, 897, 500, 915, 516) > 10, "live indicator: dot drawn green outside the dead zone");
        dump_frame("ui_controller.ppm");
        kl_test_fake_pad(1, 3000, 0);                      /* inside */
        break;
    }
    case 14:
        CHECK(count_color(0xFF7EE0A0u, 845, 475, 915, 540) == 0, "live indicator: dot is not green inside the dead zone");
        CHECK(count_color(0xFFE8E9FFu, 872, 500, 888, 516) > 5, "live indicator: dot near the centre");
        kl_test_fake_pad(-1, 0, 0);
        break;
    case 15:
        CHECK(kl_test_pad_present() == real_pad, "override removed: the real controller state is back");
        click("reset");
        break;
    case 16:
        c = now_cfg();
        CHECK(!strcmp(c.pad[CFG_BTN_A], "b") && !strcmp(c.pad[CFG_BTN_B], "a") && c.stick_as_dpad == 1 && c.deadzone == 8000,
              "RESET restores the controller defaults (A=east b, B=south a, stick on, deadzone 8000)");
        kl_test_fake_pad(1, 0, 0);
        break;
    /* ---- controller navigation (south = OK, east = back, d-pad moves) ---- */
    case 17:
        pad_button(SDL_CONTROLLER_BUTTON_B);               /* east: back to the main page */
        break;
    case 18:
        CHECK(!strcmp(kl_test_page(), "main"), "east button goes back");
        kl_test_set_focus("input");
        pad_button(SDL_CONTROLLER_BUTTON_A);               /* south: OK */
        break;
    case 19:
        CHECK(!strcmp(kl_test_page(), "keyboard"), "south button activates the focused card");
        pad_button(SDL_CONTROLLER_BUTTON_B);
        break;
    case 20:
        CHECK(!strcmp(kl_test_page(), "main"), "east returns from the input page");
        kl_test_set_focus("settings");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    case 21:
        CHECK(!strcmp(kl_test_page(), "settings"), "south opens SETTINGS");
        kl_test_set_focus("size3");
        pad_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        break;
    case 22:
        CHECK(!strcmp(kl_test_focus(), "size4") && now_cfg().scale == 3, "d-pad Right moves the focus without changing the size");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    case 23:
        CHECK(now_cfg().scale == 4, "south selects the size under the focus");
        pad_button(SDL_CONTROLLER_BUTTON_B);
        break;
    case 24:
        CHECK(!strcmp(kl_test_page(), "main"), "east returns from SETTINGS");
        kl_test_set_focus("play");
        pad_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        break;
    case 25:
        CHECK(strcmp(kl_test_focus(), "play") != 0, "d-pad Down moves the focus");
        pad_axis(SDL_CONTROLLER_AXIS_LEFTY, -30000);       /* stick up */
        break;
    case 26:
        pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
        pad_button(SDL_CONTROLLER_BUTTON_B);               /* east on the main page: nothing (does not quit) */
        break;
    case 27:
        CHECK(!strcmp(kl_test_page(), "main"), "east on the main page does not quit");
        kl_test_set_focus("quit");
        pad_button(SDL_CONTROLLER_BUTTON_A);
        break;
    default: break;
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(ini, sizeof ini, "%s/test_ui_edit.ini", dir);
    snprintf(shot_dir, sizeof shot_dir, "%s", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    kl_set_poll_interval(10);
    int rc;

    remove(ini);
    rc = run_script(scen_keyboard, KL_PAGE_KEYBOARD);
    CHECK(rc == KL_QUIT, "keyboard scenario ends with SDL_QUIT");

    remove(ini);
    rc = run_script(scen_controller, KL_PAGE_CONTROLLER);
    CHECK(rc == KL_QUIT, "south button on QUIT returns KL_QUIT (controller navigation)");

    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    remove(ini);
    return fails != 0;
}
