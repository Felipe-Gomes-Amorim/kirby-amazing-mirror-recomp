/* config_ini.c - see config_ini.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config_ini.h"

static const char *const BTN[CFG_BTN_COUNT] = { "a", "b", "l", "r", "start", "select", "up", "down", "left", "right" };
static const char *const DEF_KEY[CFG_BTN_COUNT] = { "X", "Z", "A", "S", "Return", "Backspace", "Up", "Down", "Left", "Right" };
static const char *const DEF_PAD[CFG_BTN_COUNT] = { "b", "a", "leftshoulder", "rightshoulder", "start", "back",
                                                    "dpup", "dpdown", "dpleft", "dpright" };

const char *cfg_button_name(int btn) { return btn >= 0 && btn < CFG_BTN_COUNT ? BTN[btn] : ""; }

static const char *const SCALING[3] = { "integer", "fit", "stretch" };
const char *cfg_scaling_name(int s) { return s >= 0 && s < 3 ? SCALING[s] : "fit"; }

static void set_str(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}
static void set_name(char *dst, const char *src) { set_str(dst, CFG_NAME_MAX, src); }

void cfg_reset_keyboard(KirbyConfig *c) {
    for (int i = 0; i < CFG_BTN_COUNT; i++) set_name(c->key[i], DEF_KEY[i]);
}

void cfg_reset_controller(KirbyConfig *c) {
    for (int i = 0; i < CFG_BTN_COUNT; i++) set_name(c->pad[i], DEF_PAD[i]);
    c->stick_as_dpad = 1;
    c->deadzone = 8000;
}

void cfg_defaults(KirbyConfig *c) {
    memset(c, 0, sizeof *c);
    c->scale = 3; c->mute = 0; c->fullscreen = 0; c->vsync = 0;
    c->scaling = CFG_SCALING_FIT; c->volume = 100;
    cfg_reset_keyboard(c);
    cfg_reset_controller(c);
    c->launcher_show = 1;
    set_str(c->language, sizeof c->language, "en");
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int cfg_key_reserved(const char *k) { return ieq(k, "Escape") || ieq(k, "F11") || ieq(k, "F1"); }

static int name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= CFG_NAME_MAX) return 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126 || ch == '=' || ch == ';' || ch == '#' || ch == '[' || ch == ']') return 0;
    }
    return 1;
}

static int clampi(int *v, int lo, int hi, int def) {
    if (*v < lo || *v > hi) { *v = def; return 1; }
    return 0;
}

int cfg_validate(KirbyConfig *c) {
    int fixes = 0;
    fixes += clampi(&c->scale, 1, 6, 3);
    fixes += clampi(&c->mute, 0, 1, 0);
    fixes += clampi(&c->fullscreen, 0, 1, 0);
    fixes += clampi(&c->vsync, 0, 1, 0);
    fixes += clampi(&c->scaling, 0, 2, CFG_SCALING_FIT);
    fixes += clampi(&c->volume, 0, 100, 100);
    fixes += clampi(&c->stick_as_dpad, 0, 1, 1);
    fixes += clampi(&c->deadzone, 0, 32767, 8000);
    fixes += clampi(&c->launcher_show, 0, 1, 1);
    c->key[0][CFG_NAME_MAX - 1] = 0;
    for (int i = 0; i < CFG_BTN_COUNT; i++) {
        c->key[i][CFG_NAME_MAX - 1] = 0;
        c->pad[i][CFG_NAME_MAX - 1] = 0;
        if (!name_ok(c->key[i]) || cfg_key_reserved(c->key[i])) { set_name(c->key[i], DEF_KEY[i]); fixes++; }
        if (!name_ok(c->pad[i])) { set_name(c->pad[i], DEF_PAD[i]); fixes++; }
    }
    int dup = 0;
    for (int i = 0; i < CFG_BTN_COUNT && !dup; i++)
        for (int j = i + 1; j < CFG_BTN_COUNT; j++) if (ieq(c->key[i], c->key[j])) { dup = 1; break; }
    if (dup) { cfg_reset_keyboard(c); fixes++; }
    dup = 0;
    for (int i = 0; i < CFG_BTN_COUNT && !dup; i++)
        for (int j = i + 1; j < CFG_BTN_COUNT; j++) if (ieq(c->pad[i], c->pad[j])) { dup = 1; break; }
    if (dup) {
        int sd = c->stick_as_dpad, dz = c->deadzone;
        cfg_reset_controller(c);
        c->stick_as_dpad = sd; c->deadzone = dz;
        fixes++;
    }
    if (!name_ok(c->language) || strlen(c->language) > 7) { set_str(c->language, sizeof c->language, "en"); fixes++; }
    return fixes;
}

static int parse_int(const char *v, int *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != 0) return 0;                   /* "3x" is not a number */
    if (n < -1000000 || n > 1000000) return 0;
    *out = (int)n;
    return 1;
}

static int button_index(const char *k) {
    for (int i = 0; i < CFG_BTN_COUNT; i++) if (!strcmp(k, BTN[i])) return i;
    return -1;
}

/* v1/v2 compatibility: integer_scale seen without a valid scaling= */
static int g_have_scaling, g_have_int, g_int_val;

static void apply(KirbyConfig *c, const char *sec, const char *key, const char *val) {
    int n;
    int in_video = !*sec || !strcmp(sec, "kirby") || !strcmp(sec, "video");
    int in_audio = !*sec || !strcmp(sec, "kirby") || !strcmp(sec, "audio");
    /* an invalid number leaves the field untouched: validation/defaults already hold */
    if (in_video && !strcmp(key, "scale")) { if (parse_int(val, &n)) c->scale = n; return; }
    if (in_video && !strcmp(key, "fullscreen")) { if (parse_int(val, &n)) c->fullscreen = n; return; }
    if (in_video && !strcmp(key, "vsync")) { if (parse_int(val, &n)) c->vsync = n; return; }
    if (in_video && !strcmp(key, "integer_scale")) {
        if (parse_int(val, &n) && (n == 0 || n == 1)) { g_have_int = 1; g_int_val = n; }
        return;
    }
    if (in_video && !strcmp(key, "scaling")) {
        for (int i = 0; i < 3; i++) if (ieq(val, SCALING[i])) { c->scaling = i; g_have_scaling = 1; }
        return;
    }
    if (in_audio && !strcmp(key, "mute")) { if (parse_int(val, &n)) c->mute = n; return; }
    if (in_audio && !strcmp(key, "volume")) { if (parse_int(val, &n)) c->volume = n; return; }
    if (!strcmp(sec, "keyboard")) {
        int b = button_index(key);
        if (b >= 0 && name_ok(val)) set_name(c->key[b], val);
        return;
    }
    if (!strcmp(sec, "controller")) {
        int b = button_index(key);
        if (b >= 0) { if (name_ok(val)) set_name(c->pad[b], val); return; }
        if (!strcmp(key, "stick_as_dpad")) { if (parse_int(val, &n)) c->stick_as_dpad = n; return; }
        if (!strcmp(key, "deadzone")) { if (parse_int(val, &n)) c->deadzone = n; return; }
        return;
    }
    if (!strcmp(sec, "launcher")) {
        if (!strcmp(key, "show")) { if (parse_int(val, &n)) c->launcher_show = n; return; }
        if (!strcmp(key, "language")) { if (name_ok(val) && strlen(val) < 8) set_str(c->language, sizeof c->language, val); return; }
    }
}

int cfg_load(const char *ini_path, KirbyConfig *c) {
    cfg_defaults(c);
    FILE *f = fopen(ini_path, "rb");
    if (!f) return 1;
    char line[256], sec[32] = "";
    g_have_scaling = g_have_int = g_int_val = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#' || *p == '\n' || *p == '\r') continue;
        if (*p == '[') {                                   /* [section] */
            char *e = strchr(p, ']');
            sec[0] = 0;
            if (e) {
                *e = 0;
                size_t n = strlen(p + 1);
                if (n < sizeof sec) {
                    for (size_t i = 0; i <= n; i++) sec[i] = (char)tolower((unsigned char)p[1 + i]);
                }
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        for (char *k = key + strlen(key); k > key && (k[-1] == ' ' || k[-1] == '\t'); k--) k[-1] = 0;
        for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        while (*val == ' ' || *val == '\t') val++;
        for (char *k = val; *k; k++) if (*k == ';' || *k == '#' || *k == '\r' || *k == '\n') { *k = 0; break; }
        for (char *k = val + strlen(val); k > val && (k[-1] == ' ' || k[-1] == '\t'); k--) k[-1] = 0;
        apply(c, sec, key, val);
    }
    fclose(f);
    if (!g_have_scaling && g_have_int) c->scaling = g_int_val ? CFG_SCALING_INTEGER : CFG_SCALING_FIT;
    cfg_validate(c);
    return 0;
}

int cfg_save(const char *ini_path, const KirbyConfig *in) {
    KirbyConfig c = *in;
    cfg_validate(&c);
    FILE *f = fopen(ini_path, "wb");
    if (!f) return -1;
    fprintf(f, "; kirby.ini - settings for the Kirby & The Amazing Mirror recompilation (v2)\r\n"
               "\r\n[video]\r\n"
               "; scale: window size = 240x160 * scale (1..6)\r\n"
               "scale=%d\r\n"
               "; fullscreen: 1 = full screen (F11 toggles in game)\r\n"
               "fullscreen=%d\r\n"
               "; scaling: integer = whole-number scale, fit = keep aspect ratio, stretch = fill the window\r\n"
               "scaling=%s\r\n"
               "; vsync: 1 = sync presents with the monitor\r\n"
               "vsync=%d\r\n",
            c.scale, c.fullscreen, SCALING[c.scaling], c.vsync);
    fprintf(f, "\r\n[audio]\r\n"
               "; volume: 0..100\r\n"
               "volume=%d\r\n"
               "mute=%d\r\n",
            c.volume, c.mute);
    fprintf(f, "\r\n[keyboard]\r\n"
               "; SDL key names, by physical position (A = X, B = Z). Esc, F11 and F1 are reserved.\r\n");
    for (int i = 0; i < CFG_BTN_COUNT; i++) fprintf(f, "%s=%s\r\n", BTN[i], c.key[i]);
    fprintf(f, "\r\n[controller]\r\n"
               "; SDL game controller button names (a/b follow the Xbox layout: GBA A = east button)\r\n");
    for (int i = 0; i < CFG_BTN_COUNT; i++) fprintf(f, "%s=%s\r\n", BTN[i], c.pad[i]);
    fprintf(f, "stick_as_dpad=%d\r\ndeadzone=%d\r\n", c.stick_as_dpad, c.deadzone);
    fprintf(f, "\r\n[launcher]\r\n"
               "; show: 0 = start the game directly (hold Shift to force the launcher)\r\n"
               "show=%d\r\n"
               "language=%s\r\n",
            c.launcher_show, c.language);
    int rc = ferror(f) ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

void cfg_path_beside(const char *file_path, char *out, size_t cap) {
    if (cap == 0) return;
    const char *slash = NULL;
    for (const char *p = file_path; *p; p++) if (*p == '/' || *p == '\\') slash = p;
    size_t dir = slash ? (size_t)(slash - file_path) + 1 : 0;
    const char *name = "kirby.ini";
    if (dir + strlen(name) + 1 > cap) { out[0] = 0; return; }
    memcpy(out, file_path, dir);
    strcpy(out + dir, name);
}

int cfg_equal(const KirbyConfig *a, const KirbyConfig *b) {
    if (a->scale != b->scale || a->mute != b->mute || a->fullscreen != b->fullscreen || a->vsync != b->vsync ||
        a->scaling != b->scaling || a->volume != b->volume || a->stick_as_dpad != b->stick_as_dpad ||
        a->deadzone != b->deadzone || a->launcher_show != b->launcher_show || strcmp(a->language, b->language))
        return 0;
    for (int i = 0; i < CFG_BTN_COUNT; i++)
        if (strcmp(a->key[i], b->key[i]) || strcmp(a->pad[i], b->pad[i])) return 0;
    return 1;
}
