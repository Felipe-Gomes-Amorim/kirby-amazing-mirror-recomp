/* test_kirby_ui_online.c - the ONLINE page of the launcher (kirby_ui.dll), headless: text fields (SDL text input),
 * the on-screen keypad (controller), HOST / JOIN / START / LEAVE return codes, the live online poll and layout
 * checks (nothing overlaps). Every simulated event carries the SDL window id. */
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
static int step;
typedef void (*Script)(int step);
static Script script;
static KlOnline cur_on;

static void to_window(int lx, int ly, int *wx, int *wy) {
    int ww, wh, fs;
    kl_test_video(&ww, &wh, &fs);
    float s = (float)ww / KL_W < (float)wh / KL_H ? (float)ww / KL_W : (float)wh / KL_H;
    *wx = (int)((ww - KL_W * s) / 2 + lx * s);
    *wy = (int)((wh - KL_H * s) / 2 + ly * s);
}

static void click(const char *id) {
    int x, y, w, h, wx, wy;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); fails++; return; }
    to_window(x + w / 2, y + h / 2, &wx, &wy);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = kl_test_window_id();
    e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED; e.button.x = wx; e.button.y = wy; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void key(SDL_Keycode k) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.windowID = kl_test_window_id(); e.key.state = SDL_PRESSED; e.key.keysym.sym = k;
    SDL_PushEvent(&e);
}

static void typed(const char *t) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_TEXTINPUT; e.text.windowID = kl_test_window_id();
    snprintf(e.text.text, sizeof e.text.text, "%s", t);
    SDL_PushEvent(&e);
}

static void pad(int b) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.which = 0; e.cbutton.button = (Uint8)b; e.cbutton.state = SDL_PRESSED;
    SDL_PushEvent(&e);
}

static void quit_now(void) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }

static void status_cb(KlStatus *o) {
    memset(o, 0, sizeof *o);
    o->state = 3; o->percent = 100; o->rom_ok = 1;
    snprintf(o->rom, sizeof o->rom, "C:\\roms\\Kirby.gba");
    script(step++);
    if (step > 200) quit_now();
}

static void online_cb(KlOnline *o) { *o = cur_on; }

static int run_script(Script s, int start_online) {
    step = 0; script = s;
    kl_set_start_page(start_online ? KL_PAGE_ONLINE : KL_PAGE_MAIN);
    return kl_run(ini, status_cb);
}

static void reset_online(void) {
    memset(&cur_on, 0, sizeof cur_on);
    for (int i = 0; i < 4; i++) cur_on.ping_ms[i] = -1;
    cur_on.self_slot = -1;
}

static void fresh_ini(const char *txt) {
    FILE *f = fopen(ini, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

/* ---------------------------------------------------------------- layout */
static const char *const ONL_IDS[] = { "online_name", "online_p2", "online_p3", "online_p4", "online_addr", "online_port",
    "online_delay", "online_host", "online_join", "online_start", "online_leave", "online_copy", "online_back" };
static const char *const KP_IDS[] = { "kp_0", "kp_1", "kp_2", "kp_3", "kp_4", "kp_5", "kp_6", "kp_7", "kp_8", "kp_9",
    "kp_del", "kp_clr", "kp_dot", "kp_ok", "kp_cancel" };

static int overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int layout_problems(const char *tag, const char *const *ids, int n, int with_texts) {
    int bad = 0, nr = 0, rx[40], ry[40], rw[40], rh[40];
    const char *rn[40];
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!kl_test_widget(ids[i], &x, &y, &w, &h)) continue;
        rx[nr] = x; ry[nr] = y; rw[nr] = w; rh[nr] = h; rn[nr] = ids[i]; nr++;
        if (x < 0 || y < 0 || x + w > KL_W || y + h > KL_H) { printf("  [%s] widget %s outside the canvas\n", tag, ids[i]); bad++; }
    }
    for (int i = 0; i < nr; i++)
        for (int j = i + 1; j < nr; j++)
            if (overlap(rx[i], ry[i], rw[i], rh[i], rx[j], ry[j], rw[j], rh[j])) {
                printf("  [%s] widgets %s and %s overlap\n", tag, rn[i], rn[j]); bad++;
            }
    if (with_texts) {
        /* slider value text is drawn at x+w+12 */
        int dx, dy, dw, dh;
        if (kl_test_widget("online_delay", &dx, &dy, &dw, &dh)) {
            int tx = dx + dw + 12, ty = dy + 4, tw = 4 * 12, th = 14;
            if (tx + tw > KL_W) { printf("  [%s] delay value leaves the canvas\n", tag); bad++; }
            for (int j = 0; j < nr; j++)
                if (strcmp(rn[j], "online_delay") && overlap(tx, ty, tw, th, rx[j], ry[j], rw[j], rh[j])) {
                    printf("  [%s] delay value is behind %s\n", tag, rn[j]); bad++;
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
            for (int j = i + 1; j < nt; j++) {
                int ux, uy, uw, uh;
                kl_test_text(j, &ux, &uy, &uw, &uh);
                if (overlap(tx, ty, tw, th, ux, uy, uw, uh)) { printf("  [%s] texts %d and %d overlap\n", tag, i, j); bad++; }
            }
        }
    }
    return bad;
}

/* ---------------------------------------------------------------- A: mouse + keyboard, fields, HOST */
static void scen_fields(int s) {
    switch (s) {
    case 2:
        CHECK(!strcmp(kl_test_page(), "main") && kl_test_widget("online", NULL, NULL, NULL, NULL), "main page has ONLINE... with a ROM");
        click("online");
        break;
    case 4:
        CHECK(!strcmp(kl_test_page(), "online"), "ONLINE opens the online page");
        CHECK(kl_test_enabled("online_host") && kl_test_enabled("online_join"), "HOST and JOIN are enabled while offline");
        CHECK(!kl_test_enabled("online_start") && !kl_test_enabled("online_leave") && !kl_test_enabled("online_copy"),
              "START / LEAVE / COPY are disabled while offline");
        CHECK(!strcmp(kl_test_field(1), "127.0.0.1") && !strcmp(kl_test_field(2), "47900") && !strcmp(kl_test_field(0), "Player"),
              "fields show the [link] defaults");
        click("online_addr");
        break;
    case 6:
        CHECK(kl_test_editing() == 1, "clicking ADDRESS starts typing");
        for (int i = 0; i < 9; i++) key(SDLK_BACKSPACE);
        typed("192.168.1.5");
        typed("!?");                                          /* illegal characters are ignored */
        break;
    case 8:
        CHECK(!strcmp(kl_test_field(1), "192.168.1.5"), "typed text goes into the address (illegal characters dropped)");
        key(SDLK_RETURN);
        break;
    case 10:
        CHECK(kl_test_editing() == -1, "Enter ends the edit");
        click("online_port");
        break;
    case 12:
        CHECK(kl_test_editing() == 2, "clicking PORT starts typing");
        for (int i = 0; i < 6; i++) key(SDLK_BACKSPACE);
        typed("12ab3");
        break;
    case 14:
        CHECK(!strcmp(kl_test_field(2), "123"), "the port field takes digits only");
        key(SDLK_BACKSPACE); key(SDLK_BACKSPACE); key(SDLK_BACKSPACE);
        break;
    case 16:
        CHECK(!kl_test_enabled("online_host") && !kl_test_enabled("online_join"), "an empty port disables HOST and JOIN");
        typed("70000");
        break;
    case 18:
        CHECK(!kl_test_enabled("online_host"), "port 70000 disables HOST");
        for (int i = 0; i < 5; i++) key(SDLK_BACKSPACE);
        typed("5123");
        break;
    case 20:
        CHECK(kl_test_enabled("online_host") && kl_test_enabled("online_join"), "a valid port enables HOST and JOIN");
        key(SDLK_ESCAPE);                                     /* ends the edit, stays on the page */
        break;
    case 22:
        CHECK(kl_test_editing() == -1 && !strcmp(kl_test_page(), "online"), "Escape in a field only ends the edit");
        click("online_p4");
        break;
    case 23:
        kl_test_set_focus("online_delay");
        break;
    case 24:
        key(SDLK_RIGHT); key(SDLK_RIGHT); key(SDLK_RIGHT);          /* auto -> 0 -> 1 -> stops at 1 (4 players: max K is 1) */
        break;
    case 25:
        click("online_name");
        break;
    case 26:
        for (int i = 0; i < 6; i++) key(SDLK_BACKSPACE);
        typed("Pink Puff");
        key(SDLK_TAB);                                        /* Tab commits and moves on */
        break;
    case 28:
        CHECK(kl_test_editing() == -1, "Tab ends the edit");
        click("online_host");
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- B: keyboard-only focus */
static void scen_keys(int s) {
    switch (s) {
    case 2:
        kl_test_set_focus("online_name");
        key(SDLK_RETURN);
        break;
    case 4:
        CHECK(kl_test_editing() == 0, "Enter on a field starts typing");
        typed("Z");
        key(SDLK_DOWN);                                       /* commits and moves to the next row */
        break;
    case 6:
        CHECK(kl_test_editing() == -1, "Down ends the edit");
        key(SDLK_ESCAPE);
        break;
    case 8:
        CHECK(!strcmp(kl_test_page(), "main"), "Escape (not editing) goes back to the main page");
        click("online");
        break;
    case 10:
        CHECK(!strcmp(kl_test_page(), "online"), "ONLINE reopens");
        click("online_back");
        break;
    case 12:
        CHECK(!strcmp(kl_test_page(), "main"), "BACK goes to the main page");
        quit_now();
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- C: controller + keypad */
static void scen_pad(int s) {
    static char port0[16];
    switch (s) {
    case 2:
        kl_test_set_focus("online_addr");
        pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 4:
        CHECK(kl_test_keypad() == 1 && kl_test_editing() == -1, "A on ADDRESS opens the keypad (not the keyboard)");
        CHECK(kl_test_widget("kp_5", NULL, NULL, NULL, NULL) && !kl_test_widget("online_host", NULL, NULL, NULL, NULL),
              "the keypad is modal");
        CHECK(kl_test_enabled("kp_dot"), "the address keypad has a dot key");
        kl_test_set_focus("kp_clr"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 6:
        CHECK(!strcmp(kl_test_field(1), ""), "CLR empties the field");
        kl_test_set_focus("kp_1"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 7: kl_test_set_focus("kp_0"); pad(SDL_CONTROLLER_BUTTON_A); break;
    case 8: kl_test_set_focus("kp_dot"); pad(SDL_CONTROLLER_BUTTON_A); break;
    case 9: kl_test_set_focus("kp_9"); pad(SDL_CONTROLLER_BUTTON_A); break;
    case 10: kl_test_set_focus("kp_9"); pad(SDL_CONTROLLER_BUTTON_A); break;
    case 11:
        CHECK(!strcmp(kl_test_field(1), "10.99"), "keypad digits and dot build the address");
        pad(SDL_CONTROLLER_BUTTON_B);                         /* east = backspace on the keypad */
        break;
    case 12:
        CHECK(!strcmp(kl_test_field(1), "10.9"), "B deletes the last character");
        kl_test_set_focus("kp_ok"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 14:
        CHECK(kl_test_keypad() == -1 && !strcmp(kl_test_focus(), "online_addr"), "OK closes the keypad and returns to the field");
        kl_test_set_focus("online_port"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 16:
        snprintf(port0, sizeof port0, "%s", kl_test_field(2));
        CHECK(kl_test_keypad() == 2 && !kl_test_enabled("kp_dot"), "the port keypad has no dot");
        kl_test_set_focus("kp_clr"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 18:
        kl_test_set_focus("kp_7"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 20:
        kl_test_set_focus("kp_cancel"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 22:
        CHECK(kl_test_keypad() == -1 && !strcmp(kl_test_field(2), port0), "CANCEL restores the field");
        kl_test_set_focus("online_name"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 24:
        CHECK(kl_test_keypad() == -1 && kl_test_editing() == -1, "the name has no keypad on a controller");
        kl_test_set_focus("online_port"); pad(SDL_CONTROLLER_BUTTON_A);
        break;
    case 26:
        CHECK(kl_test_keypad() == 2, "keypad open again");
        key(SDLK_ESCAPE);
        break;
    case 28:
        CHECK(kl_test_keypad() == -1 && !strcmp(kl_test_page(), "online"), "Escape closes only the keypad");
        quit_now();
        break;
    default: break;
    }
}

/* ---------------------------------------------------------------- D: live data and codes */
static void scen_live(int s) {
    switch (s) {
    case 2:
        cur_on.state = KLO_HOSTING;
        cur_on.slots[0] = KLS_READY; cur_on.slots[1] = KLS_CONNECTING;
        cur_on.self_slot = 0;
        snprintf(cur_on.my_address, sizeof cur_on.my_address, "192.168.1.5:47900");
        snprintf(cur_on.names[0], sizeof cur_on.names[0], "Host");
        break;
    case 4:
        CHECK(!kl_test_enabled("online_host") && !kl_test_enabled("online_join") && kl_test_enabled("online_leave"),
              "while hosting: HOST / JOIN off, LEAVE on");
        CHECK(!kl_test_enabled("online_addr") && !kl_test_enabled("online_port") && !kl_test_enabled("online_name") &&
              !kl_test_enabled("online_delay") && !kl_test_enabled("online_p3"), "fields are locked while connected");
        CHECK(!kl_test_enabled("online_start"), "START needs 2 connected players (one is still joining)");
        CHECK(kl_test_enabled("online_copy"), "COPY is enabled when the poll gives an address");
        cur_on.slots[1] = KLS_CONNECTED; cur_on.ping_ms[1] = 35; snprintf(cur_on.names[1], 16, "Guest");
        break;
    case 6:
        CHECK(kl_test_enabled("online_start"), "START enabled with 2 connected players");
        click("online_copy");
        break;
    case 8:
        CHECK(!strcmp(kl_test_note(), "ADDRESS COPIED"), "COPY gives feedback");
        click("online_start");
        break;
    default: break;
    }
}

static void scen_leave(int s) {
    if (s == 2) { cur_on.state = KLO_CONNECTED; cur_on.slots[0] = KLS_READY; cur_on.slots[1] = KLS_CONNECTED; cur_on.self_slot = 1; }
    if (s == 4) {
        CHECK(kl_test_enabled("online_leave") && !kl_test_enabled("online_start"), "a joined client can LEAVE but not START");
        click("online_leave");
    }
}

static void scen_join(int s) {
    if (s == 2) {
        CHECK(kl_test_enabled("online_join"), "JOIN enabled with an address");
        click("online_join");
    }
}

static void scen_join_empty(int s) {
    switch (s) {
    case 2: click("online_addr"); break;
    case 4: for (int i = 0; i < 20; i++) key(SDLK_BACKSPACE); break;
    case 6:
        CHECK(!kl_test_field(1)[0], "address cleared");
        CHECK(!kl_test_enabled("online_join") && kl_test_enabled("online_host"), "an empty address disables JOIN but not HOST");
        quit_now();
        break;
    default: break;
    }
}

static void scen_states(int s) {
    static const int states[] = { KLO_OFFLINE, KLO_HOSTING, KLO_CONNECTING, KLO_CONNECTED, KLO_PLAYING, KLO_DESYNC, KLO_LOST };
    int n = (int)(sizeof states / sizeof states[0]);
    if (s >= 2 && s < 2 + n * 3) {
        int k = (s - 2) / 3, ph = (s - 2) % 3;
        if (ph == 0) {
            reset_online();
            cur_on.state = states[k];
            for (int i = 0; i < 4; i++) {
                cur_on.slots[i] = (i + k) % 5;
                cur_on.ping_ms[i] = 999 + k * 100;
                snprintf(cur_on.names[i], 16, "WWWWWWWWWWWWWWW");
            }
            cur_on.self_slot = k % 4;
            snprintf(cur_on.my_address, sizeof cur_on.my_address, "very-long-host-name.example.org:47900 and more more");
            snprintf(cur_on.message, sizeof cur_on.message,
                     "A very long status message that has to be clipped inside the canvas width and not overflow it ok");
        } else if (ph == 2) {
            char tag[40];
            snprintf(tag, sizeof tag, "online state %d", states[k]);
            if (layout_problems(tag, ONL_IDS, (int)(sizeof ONL_IDS / sizeof ONL_IDS[0]), 1)) fails++;
        }
    }
    if (s == 2 + n * 3) quit_now();
}

static void scen_keypad_geometry(int s) {
    if (s == 2) { kl_test_set_focus("online_addr"); pad(SDL_CONTROLLER_BUTTON_A); }
    if (s == 4) {
        CHECK(kl_test_keypad() == 1, "keypad opened for the geometry check");
        if (layout_problems("keypad", KP_IDS, (int)(sizeof KP_IDS / sizeof KP_IDS[0]), 0)) fails++;
        /* with 15 chars typed the keypad text must still fit */
        quit_now();
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(ini, sizeof ini, "%s/test_ui_online.ini", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    kl_set_poll_interval(10);
    kl_set_online_poll(online_cb);
    int rc;
    KirbyConfig c;

    remove(ini);
    reset_online();
    rc = run_script(scen_fields, 0);
    CHECK(rc == KL_ONLINE_HOST, "HOST returns KL_ONLINE_HOST (6)");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.link_mode, "host") && !strcmp(c.link_host, "192.168.1.5") && c.link_port == 5123 &&
          c.link_players == 4 && c.link_delay == 1 && !strcmp(c.link_name, "Pink Puff"),
          "HOST saved mode/host/port/players/delay/name into [link]");

    reset_online();
    rc = run_script(scen_keys, 1);
    CHECK(rc == KL_QUIT, "keyboard scenario ends with quit");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.link_name, "Pink PuffZ"), "the name typed and committed with Down was kept");

    reset_online();
    rc = run_script(scen_pad, 1);
    CHECK(rc == KL_QUIT, "controller scenario ends with quit");

    reset_online();
    rc = run_script(scen_live, 1);
    CHECK(rc == KL_ONLINE_START, "START returns KL_ONLINE_START (9)");

    reset_online();
    rc = run_script(scen_leave, 1);
    CHECK(rc == KL_ONLINE_LEAVE, "LEAVE returns KL_ONLINE_LEAVE (8)");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.link_mode, "off"), "LEAVE puts [link] mode back to off");

    reset_online();
    fresh_ini("[link]\r\nmode=off\r\nhost=10.1.1.1\r\nport=4000\r\n");
    rc = run_script(scen_join, 1);
    CHECK(rc == KL_ONLINE_JOIN, "JOIN returns KL_ONLINE_JOIN (7)");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.link_mode, "join") && !strcmp(c.link_host, "10.1.1.1") && c.link_port == 4000, "JOIN saved mode=join and the address");

    reset_online();
    rc = run_script(scen_states, 1);
    CHECK(rc == KL_QUIT, "geometry scenario ends with quit");
    reset_online();
    rc = run_script(scen_keypad_geometry, 1);
    CHECK(rc == KL_QUIT, "keypad geometry scenario ends with quit");

    /* no poll: the page still works and shows an offline state */
    kl_set_online_poll(NULL);
    reset_online();
    rc = run_script(scen_join_empty, 1);
    CHECK(rc == KL_QUIT, "works without an online poll");

    kl_set_start_page(KL_PAGE_MAIN);
    remove(ini);
    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    return fails != 0;
}
