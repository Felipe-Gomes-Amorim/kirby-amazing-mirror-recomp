/* test_config_ini.c - kirby.ini v2: defaults, round trip, v1 compatibility, validation. No SDL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../config_ini.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void write_text(const char *path, const char *txt) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char ini[600];
    snprintf(ini, sizeof ini, "%s/test_kirby_v2.ini", dir);
    KirbyConfig c, d;

    /* ---- contract defaults ---- */
    cfg_defaults(&d);
    CHECK(d.scale == 3 && d.fullscreen == 0 && d.scaling == CFG_SCALING_FIT && d.vsync == 0, "[video] defaults (scaling=fit)");
    CHECK(d.volume == 100 && d.mute == 0, "[audio] defaults");
    CHECK(!strcmp(d.key[CFG_BTN_A], "X") && !strcmp(d.key[CFG_BTN_B], "Z"), "keyboard by position: A=X B=Z");
    CHECK(!strcmp(d.key[CFG_BTN_L], "A") && !strcmp(d.key[CFG_BTN_R], "S"), "keyboard: L=A R=S");
    CHECK(!strcmp(d.key[CFG_BTN_START], "Return") && !strcmp(d.key[CFG_BTN_SELECT], "Backspace"), "keyboard: Start/Select");
    CHECK(!strcmp(d.key[CFG_BTN_UP], "Up") && !strcmp(d.key[CFG_BTN_RIGHT], "Right"), "keyboard: d-pad");
    CHECK(!strcmp(d.pad[CFG_BTN_A], "b") && !strcmp(d.pad[CFG_BTN_B], "a"), "controller by position: GBA A = east (SDL b)");
    CHECK(!strcmp(d.pad[CFG_BTN_L], "leftshoulder") && !strcmp(d.pad[CFG_BTN_SELECT], "back") &&
          !strcmp(d.pad[CFG_BTN_UP], "dpup"), "controller defaults");
    CHECK(d.stick_as_dpad == 1 && d.deadzone == 8000, "controller: stick_as_dpad=1 deadzone=8000");
    CHECK(d.launcher_show == 1 && !strcmp(d.language, "en"), "[launcher] defaults");
    CHECK(cfg_validate(&d) == 0, "defaults need no fixes");

    /* ---- missing file ---- */
    remove(ini);
    CHECK(cfg_load(ini, &c) == 1, "missing file returns 1");
    CHECK(cfg_equal(&c, &d), "missing file keeps the defaults");

    /* ---- round trip: defaults and a fully customised config ---- */
    CHECK(cfg_save(ini, &d) == 0, "save defaults");
    CHECK(cfg_load(ini, &c) == 0 && cfg_equal(&c, &d), "defaults round trip");
    KirbyConfig w;
    cfg_defaults(&w);
    w.scale = 5; w.fullscreen = 1; w.scaling = CFG_SCALING_STRETCH; w.vsync = 1; w.volume = 37; w.mute = 1;
    strcpy(w.key[CFG_BTN_A], "Q"); strcpy(w.key[CFG_BTN_B], "E"); strcpy(w.key[CFG_BTN_START], "Space");
    strcpy(w.key[CFG_BTN_UP], "W"); strcpy(w.key[CFG_BTN_DOWN], "Keypad 2");   /* names with a space */
    strcpy(w.pad[CFG_BTN_A], "x"); strcpy(w.pad[CFG_BTN_B], "y"); strcpy(w.pad[CFG_BTN_L], "lefttrigger");
    w.stick_as_dpad = 0; w.deadzone = 12000; w.launcher_show = 0;
    CHECK(cfg_save(ini, &w) == 0, "save custom");
    CHECK(cfg_load(ini, &c) == 0 && cfg_equal(&c, &w), "custom round trip (every field)");
    CHECK(cfg_save(ini, &c) == 0, "save again");
    KirbyConfig c2;
    CHECK(cfg_load(ini, &c2) == 0 && cfg_equal(&c2, &w), "second round trip is stable");

    /* the file really has the v2 sections, in order */
    {
        FILE *f = fopen(ini, "rb");
        char buf[4096] = "";
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        const char *v = strstr(buf, "[video]"), *a = strstr(buf, "[audio]"), *k = strstr(buf, "[keyboard]"),
                   *p = strstr(buf, "[controller]"), *l = strstr(buf, "[launcher]");
        CHECK(v && a && k && p && l && v < a && a < k && k < p && p < l, "sections [video][audio][keyboard][controller][launcher]");
        CHECK(strstr(buf, "scaling=stretch") && !strstr(buf, "integer_scale") && strstr(buf, "volume=37") && strstr(buf, "a=Q") && strstr(buf, "stick_as_dpad=0"),
              "keys written as in the contract");
    }

    /* ---- v1 compatibility ---- */
    write_text(ini, "scale=5\r\nmute=1\r\nfullscreen=1\r\nvsync=1\r\n");
    CHECK(cfg_load(ini, &c) == 0, "v1 flat file loads");
    CHECK(c.scale == 5 && c.mute == 1 && c.fullscreen == 1 && c.vsync == 1, "v1 flat values land in video/audio");
    CHECK(c.volume == 100 && c.scaling == CFG_SCALING_FIT && !strcmp(c.key[CFG_BTN_A], "X"), "v1: new fields take defaults");
    write_text(ini, "[kirby]\r\n; comment\r\nscale=2\r\nmute=1\r\nfullscreen=0\r\nvsync=1\r\n");
    cfg_load(ini, &c);
    CHECK(c.scale == 2 && c.mute == 1 && c.vsync == 1, "v1 [kirby] section loads");
    write_text(ini, "[kirby]\r\nscale=2\r\nmute=1\r\n");
    cfg_load(ini, &c);
    CHECK(cfg_save(ini, &c) == 0, "v1 file can be re-saved");
    cfg_load(ini, &d);
    CHECK(cfg_equal(&c, &d) && d.scale == 2 && d.mute == 1, "v1 -> v2 upgrade keeps the values");

    /* ---- parsing rules ---- */
    write_text(ini, "; header\r\n# another\r\n[VIDEO]\r\n  SCALE = 4 ; trailing comment\r\n"
                    "[Audio]\r\nVolume=65 # x\r\n[keyboard]\r\nA = Return ;x\r\nb=Z\r\nunknown=1\r\n"
                    "[mystery]\r\nscale=6\r\n[controller]\r\nDeadzone=5000\r\nzzz=1\r\nno equals line\r\n");
    CHECK(cfg_load(ini, &c) == 0, "mixed-case file loads");
    CHECK(c.scale == 4, "case-insensitive section and key, trailing comment");
    CHECK(c.volume == 65, "[Audio] volume");
    CHECK(c.deadzone == 5000, "controller deadzone");
    CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_START], "Return"),
          "A=Return duplicates Start=Return -> keyboard map reset to defaults");

    /* ---- validation ---- */
    write_text(ini, "[video]\r\nscale=9\r\nfullscreen=2\r\nintegerscale=1\r\nscaling=zoom\r\nvsync=-1\r\n"
                    "[audio]\r\nvolume=250\r\nmute=maybe\r\n[controller]\r\ndeadzone=99999\r\nstick_as_dpad=7\r\n"
                    "[launcher]\r\nshow=5\r\nlanguage=this-is-way-too-long\r\n");
    cfg_load(ini, &c);
    CHECK(c.scale == 3 && c.fullscreen == 0 && c.vsync == 0, "video out of range -> defaults");
    CHECK(c.scaling == CFG_SCALING_FIT, "unknown scaling value -> default (fit)");
    CHECK(c.volume == 100 && c.mute == 0, "audio out of range -> defaults");
    CHECK(c.deadzone == 8000 && c.stick_as_dpad == 1, "controller out of range -> defaults");
    CHECK(c.launcher_show == 1 && !strcmp(c.language, "en"), "launcher invalid -> defaults");

    write_text(ini, "[keyboard]\r\na=Escape\r\nb=F11\r\nl=F1\r\nr=f1\r\nstart=Return\r\n");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z") && !strcmp(c.key[CFG_BTN_L], "A") &&
          !strcmp(c.key[CFG_BTN_R], "S"), "reserved keys (Escape, F11, F1; any case) cannot be bound");
    CHECK(cfg_key_reserved("escape") && cfg_key_reserved("F11") && cfg_key_reserved("f1") && !cfg_key_reserved("F2") &&
          !cfg_key_reserved("F10"), "cfg_key_reserved");

    write_text(ini, "[keyboard]\r\na=Q\r\nb=Q\r\n");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z"), "duplicate bindings reset the keyboard map");

    write_text(ini, "[controller]\r\na=x\r\nb=x\r\nstick_as_dpad=0\r\ndeadzone=3000\r\n");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.pad[CFG_BTN_A], "b") && !strcmp(c.pad[CFG_BTN_B], "a"), "duplicate pad buttons reset the pad map");
    CHECK(c.stick_as_dpad == 0 && c.deadzone == 3000, "...but stick/deadzone are kept");

    write_text(ini, "[keyboard]\r\na=\r\nb=bad[name\r\nl=Left Shift\r\nr=Tab;comment\r\n");
    cfg_load(ini, &c);
    CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.key[CFG_BTN_B], "Z"), "empty / illegal names -> defaults");
    CHECK(!strcmp(c.key[CFG_BTN_L], "Left Shift"), "SDL names with a space are accepted");
    CHECK(!strcmp(c.key[CFG_BTN_R], "Tab"), "trailing ; comment is cut from a value");

    /* ---- scaling = integer | fit | stretch, and the integer_scale compatibility rules ---- */
    CHECK(!strcmp(cfg_scaling_name(CFG_SCALING_INTEGER), "integer") && !strcmp(cfg_scaling_name(CFG_SCALING_FIT), "fit") &&
          !strcmp(cfg_scaling_name(CFG_SCALING_STRETCH), "stretch"), "scaling names");
    for (int sc = 0; sc < 3; sc++) {
        KirbyConfig t;
        cfg_defaults(&t);
        t.scaling = sc;
        cfg_save(ini, &t);
        KirbyConfig u;
        cfg_load(ini, &u);
        CHECK(u.scaling == sc, "scaling round trip");
    }
    write_text(ini, "[video]\r\nscaling=Integer");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_INTEGER, "scaling=Integer (any case)");
    write_text(ini, "[video]\r\nscaling = STRETCH ; comment");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_STRETCH, "scaling=STRETCH with spaces and comment");
    write_text(ini, "[video]\r\nscale=4");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_FIT, "no scaling key -> fit (default)");
    write_text(ini, "[video]\r\ninteger_scale=1");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_INTEGER, "v1/v2: only integer_scale=1 -> integer");
    write_text(ini, "[video]\r\ninteger_scale=0");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_FIT, "v1/v2: only integer_scale=0 -> fit");
    write_text(ini, "integer_scale=1\r\nscale=2");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_INTEGER && c.scale == 2, "v1 flat file with integer_scale=1 -> integer");
    write_text(ini, "[video]\r\ninteger_scale=1\r\nscaling=stretch");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_STRETCH, "explicit scaling wins over integer_scale (any order)");
    write_text(ini, "[video]\r\nscaling=stretch\r\ninteger_scale=1");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_STRETCH, "explicit scaling wins over integer_scale (reverse order)");
    write_text(ini, "[video]\r\nscaling=zoom\r\ninteger_scale=1");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_INTEGER, "invalid scaling falls back to integer_scale");
    write_text(ini, "[video]\r\nscaling=zoom\r\ninteger_scale=7");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_FIT, "invalid scaling + invalid integer_scale -> fit");
    write_text(ini, "[audio]\r\nscaling=stretch");
    cfg_load(ini, &c);
    CHECK(c.scaling == CFG_SCALING_FIT, "scaling outside [video] is ignored");
    KirbyConfig oob;
    cfg_defaults(&oob);
    oob.scaling = 9;
    CHECK(cfg_validate(&oob) > 0 && oob.scaling == CFG_SCALING_FIT, "out-of-range scaling -> fit");
    /* a v1 file saved by v2 no longer carries integer_scale but keeps the meaning */
    write_text(ini, "integer_scale=1");
    cfg_load(ini, &c);
    cfg_save(ini, &c);
    cfg_load(ini, &d);
    CHECK(d.scaling == CFG_SCALING_INTEGER, "integer_scale=1 survives a re-save as scaling=integer");

    /* ---- UTF-8 BOM (Notepad) must not hide the first key ---- */
    {
        FILE *f = fopen(ini, "wb");
        if (f) {
            fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);
            fputs("[video]\r\nscale=5\r\n[audio]\r\nvolume=40\r\n", f);
            fclose(f);
        }
        cfg_load(ini, &c);
        CHECK(c.scale == 5 && c.volume == 40, "BOM at the start of the file is skipped");
        f = fopen(ini, "wb");
        if (f) { fputs("scale=4\r\n", f); fclose(f); }
        cfg_load(ini, &c);
        CHECK(c.scale == 4, "file shorter than 3 bytes of BOM is read normally");
    }

    /* ---- save normalises garbage ---- */
    KirbyConfig bad;
    memset(&bad, 0, sizeof bad);
    bad.scale = 99; bad.mute = 7; bad.volume = -4; bad.fullscreen = -1; bad.deadzone = 70000;
    CHECK(cfg_save(ini, &bad) == 0, "save of a zeroed/garbage struct");
    cfg_load(ini, &c);
    CHECK(c.scale == 3 && c.mute == 0 && c.volume == 100 && c.fullscreen == 0 && c.deadzone == 8000,
          "out-of-range numbers are written as their defaults");
    CHECK(!strcmp(c.key[CFG_BTN_A], "X") && !strcmp(c.pad[CFG_BTN_B], "a") && !strcmp(c.language, "en"),
          "empty names are written as defaults");
    CHECK(c.scaling == CFG_SCALING_INTEGER && c.stick_as_dpad == 0 && c.launcher_show == 0, "valid zeros are kept");

    /* old v1-style positional initializer still compiles and means scale/mute/fullscreen/vsync */
    KirbyConfig legacy = { 5, 1, 1, 1 };
    cfg_save(ini, &legacy);
    cfg_load(ini, &c);
    CHECK(c.scale == 5 && c.mute == 1 && c.fullscreen == 1 && c.vsync == 1, "v1 field order preserved");

    /* ---- path helper ---- */
    char beside[300];
    cfg_path_beside("C:\\games\\Kirby & The Amazing Mirror (USA).sav", beside, sizeof beside);
    CHECK(!strcmp(beside, "C:\\games\\kirby.ini"), "kirby.ini beside the save (backslash)");
    cfg_path_beside("/home/u/roms/baserom.gba", beside, sizeof beside);
    CHECK(!strcmp(beside, "/home/u/roms/kirby.ini"), "kirby.ini beside the file (slash)");
    cfg_path_beside("baserom.sav", beside, sizeof beside);
    CHECK(!strcmp(beside, "kirby.ini"), "no directory -> kirby.ini");
    CHECK(!strcmp(cfg_button_name(CFG_BTN_START), "start") && !strcmp(cfg_button_name(CFG_BTN_COUNT), ""), "button names");

    remove(ini);
    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    return fails != 0;
}
