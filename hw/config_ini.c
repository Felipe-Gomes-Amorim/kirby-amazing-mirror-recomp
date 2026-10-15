/* config_ini.c - ver config_ini.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config_ini.h"

void cfg_defaults(KirbyConfig *c) {
    c->scale = 3; c->mute = 0; c->fullscreen = 0; c->vsync = 0;
}

static int parse_int(const char *v, int lo, int hi, int def) {
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v) return def;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != 0) return def;               /* "3x" nao vale */
    if (n < lo || n > hi) return def;
    return (int)n;
}

int cfg_load(const char *ini_path, KirbyConfig *c) {
    cfg_defaults(c);
    FILE *f = fopen(ini_path, "rb");
    if (!f) return 1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == ';' || *p == '#' || *p == '[' || *p == '\n' || *p == '\r') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        for (char *k = key + strlen(key); k > key && (k[-1] == ' ' || k[-1] == '\t'); k--) k[-1] = 0;
        for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        while (*val == ' ' || *val == '\t') val++;
        for (char *k = val; *k; k++) if (*k == ';' || *k == '#' || *k == '\r' || *k == '\n') { *k = 0; break; }
        for (char *k = val + strlen(val); k > val && (k[-1] == ' ' || k[-1] == '\t'); k--) k[-1] = 0;
        if (!strcmp(key, "scale")) c->scale = parse_int(val, 1, 6, 3);
        else if (!strcmp(key, "mute")) c->mute = parse_int(val, 0, 1, 0);
        else if (!strcmp(key, "fullscreen")) c->fullscreen = parse_int(val, 0, 1, 0);
        else if (!strcmp(key, "vsync")) c->vsync = parse_int(val, 0, 1, 0);
    }
    fclose(f);
    return 0;
}

int cfg_save(const char *ini_path, const KirbyConfig *c) {
    FILE *f = fopen(ini_path, "wb");
    if (!f) return -1;
    int sc = c->scale < 1 ? 1 : c->scale > 6 ? 6 : c->scale;
    fprintf(f, "[kirby]\r\n"
               "; scale: 1..6 (tamanho da janela = 240x160 * scale)\r\n"
               "scale=%d\r\n"
               "; mute: 1 = sem audio\r\n"
               "mute=%d\r\n"
               "; fullscreen: 1 = tela cheia (F11 alterna no jogo)\r\n"
               "fullscreen=%d\r\n"
               "; vsync: 1 = sincroniza o present com o monitor\r\n"
               "vsync=%d\r\n",
            sc, c->mute ? 1 : 0, c->fullscreen ? 1 : 0, c->vsync ? 1 : 0);
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
