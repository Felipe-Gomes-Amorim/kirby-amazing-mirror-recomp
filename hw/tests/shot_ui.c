/* shot_ui.c - dev tool, not a test: renders every launcher page headless and writes build/hw/shot_*.ppm
 * (convert with Pillow to look at them). Build/run: hw/tests/run_ui.ps1 -Shot */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "../launcher_ui.h"
#include "../config_ini.h"

static char ini[600], outdir[500];
static int step, scenario;
typedef void (*Script)(int);
static Script script;
static KlOnline cur_on;
static int rom_state = 2, rom_pct = 95, rom_ok = 1;

static void to_window(int lx, int ly, int *wx, int *wy) {
    int ww, wh, fs;
    kl_test_video(&ww, &wh, &fs);
    float s = (float)ww / KL_W < (float)wh / KL_H ? (float)ww / KL_W : (float)wh / KL_H;
    *wx = (int)((ww - KL_W * s) / 2 + lx * s);
    *wy = (int)((wh - KL_H * s) / 2 + ly * s);
}
static void click(const char *id) {
    int x, y, w, h, wx, wy;
    if (!kl_test_widget(id, &x, &y, &w, &h)) { printf("  widget '%s' not found\n", id); return; }
    to_window(x + w / 2, y + h / 2, &wx, &wy);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = kl_test_window_id();
    e.button.button = SDL_BUTTON_LEFT; e.button.state = SDL_PRESSED; e.button.x = wx; e.button.y = wy; e.button.clicks = 1;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP; e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}
static void hover_on(const char *id) {
    int x, y, w, h, wx, wy;
    if (!kl_test_widget(id, &x, &y, &w, &h)) return;
    to_window(x + w / 2, y + h / 2, &wx, &wy);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEMOTION; e.motion.windowID = kl_test_window_id(); e.motion.x = wx; e.motion.y = wy;
    SDL_PushEvent(&e);
}
static void key(SDL_Keycode k) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.windowID = kl_test_window_id(); e.key.state = SDL_PRESSED; e.key.keysym.sym = k;
    SDL_PushEvent(&e);
}
static void quit_now(void) { SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e); }

static void dump(const char *name) {
    static uint32_t px[KL_W * KL_H];
    int w, h;
    char path[700];
    snprintf(path, sizeof path, "%s/shot_%s.ppm", outdir, name);
    FILE *f = kl_test_snapshot(px, KL_W * KL_H, &w, &h) == 0 ? fopen(path, "wb") : NULL;
    if (!f) { printf("cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { unsigned char c[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}

static void status_cb(KlStatus *o) {
    memset(o, 0, sizeof *o);
    o->state = rom_state; o->percent = rom_pct; o->rom_ok = rom_ok;
    if (rom_ok || rom_state) snprintf(o->rom, sizeof o->rom, "C:\\roms\\baserom.gba");
    if (rom_state == 2) snprintf(o->text, sizeof o->text, "Building compiled version... 12/16");
    script(step++);
    if (step > 400) quit_now();
}
static void online_cb(KlOnline *o) { *o = cur_on; }

static int run(Script s, int start_online) {
    step = 0; script = s;
    kl_set_start_page(start_online ? KL_PAGE_ONLINE : KL_PAGE_MAIN);
    return kl_run(ini, status_cb);
}

static void reset_online(void) {
    memset(&cur_on, 0, sizeof cur_on);
    for (int i = 0; i < 4; i++) cur_on.ping_ms[i] = -1;
    cur_on.self_slot = -1;
}

static void s_home(int s) {
    if (s == 5) dump("home");
    if (s == 6) { hover_on("play"); }
    if (s == 8) { dump("home_hover"); }
    if (s == 9) { rom_state = 1; rom_pct = 40; }
    if (s == 14) { dump("home_preparing"); rom_state = 3; }
    if (s == 19) { dump("home_ready"); rom_state = 0; rom_ok = 0; }
    if (s == 24) { dump("home_norom"); quit_now(); }
}
static void s_settings(int s) {
    if (s == 3) click("settings");
    if (s == 8) dump("settings");
    if (s == 9) { click("fullscreen"); }
    if (s == 14) { dump("settings_fs"); click("fullscreen"); click("mute"); }
    if (s == 19) { dump("settings_mute"); quit_now(); }
}
static void s_input(int s) {
    if (s == 3) click("input");
    if (s == 8) dump("keyboard");
    if (s == 9) click("btn_a");
    if (s == 14) { dump("keyboard_capture"); key(SDLK_ESCAPE); }
    if (s == 16) click("tab_controller");
    if (s == 20) { kl_test_fake_pad(1, 12000, -8000); }
    if (s == 24) { dump("controller"); click("btn_start"); }
    if (s == 30) { dump("controller_capture"); quit_now(); }
}
static void s_online(int s) {
    switch (s) {
    case 8: dump("online_off"); break;
    case 9: click("role_join"); break;
    case 14: dump("online_join"); click("role_host"); break;
    case 16:
        cur_on.state = KLO_HOSTING; cur_on.self_slot = 0;
        cur_on.slots[0] = KLS_READY; cur_on.slots[1] = KLS_CONNECTED; cur_on.slots[2] = KLS_CONNECTING;
        cur_on.ping_ms[1] = 38; cur_on.ping_ms[2] = 140;
        snprintf(cur_on.names[0], 16, "Bonezinho360"); snprintf(cur_on.names[1], 16, "Jos\xC3\xA9 \xC3\x87\xC3\xA3o");
        snprintf(cur_on.my_address, sizeof cur_on.my_address, "26.50.118.79:47900");
        snprintf(cur_on.addresses[0], 48, "26.50.118.79:47900"); snprintf(cur_on.addresses[1], 48, "192.168.1.5:47900");
        break;
    case 24: dump("online_hosting"); break;
    case 25:
        cur_on.state = KLO_PLAYING; cur_on.quality = KLQ_FAIR; cur_on.stalls_100ms = 7; cur_on.ping_p95_ms = 88;
        cur_on.slots[0] = cur_on.slots[1] = KLS_READY; cur_on.slots[2] = KLS_EMPTY; cur_on.ping_ms[1] = 85;
        break;
    case 32: dump("online_playing"); quit_now(); break;
    default: break;
    }
}
static void s_keypad(int s) {
    if (s == 3) { cur_on.state = KLO_OFFLINE; }
    if (s == 5) {
        SDL_Event e; SDL_zero(e);
        e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.which = 0; e.cbutton.button = SDL_CONTROLLER_BUTTON_A; e.cbutton.state = SDL_PRESSED;
        kl_test_fake_pad(1, 0, 0);
        kl_test_set_focus("online_addr");
        SDL_PushEvent(&e);
    }
    if (s == 12) { dump("keypad"); quit_now(); }
}
static void s_confirm(int s) {
    if (s == 3) click("settings");
    if (s == 8) click("save_import");
    if (s == 14) { dump("confirm"); quit_now(); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(outdir, sizeof outdir, "%s", dir);
    snprintf(ini, sizeof ini, "%s/shot_ui.ini", dir);
    remove(ini);
    FILE *f = fopen(ini, "wb");
    if (f) { fputs("[link]\r\nname=Bonezinho360\r\nhost=26.50.118.79\r\nport=7900\r\n[video]\r\nscale=4\r\n", f); fclose(f); }
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    kl_set_poll_interval(10);
    kl_set_online_poll(online_cb);
    reset_online();
    rom_state = 2; rom_pct = 95; rom_ok = 1;
    run(s_home, 0);
    rom_state = 2; rom_pct = 95; rom_ok = 1;
    run(s_settings, 0);
    run(s_input, 0);
    reset_online();
    run(s_online, 1);
    reset_online();
    run(s_keypad, 1);
    run(s_confirm, 0);
    (void)scenario;
    printf("done\n");
    return 0;
}
