/* test_link_ini.c - the [link] section of kirby.ini: round trip, validation, and PARITY between hw/config_ini.c
 * (launcher UI) and the game runtime's own reader (rt_sio_read_ini, extracted from recomp/runtime/rt_sio.c by
 * extract_rt_ini.py and compiled in). Same ini text -> same mode/players/port/host. No SDL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt_sio_ini_extract.c"          /* trim_ + rt_sio_read_ini from the runtime (generated into build/hw) */
#include "../config_ini.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static char path[700];

static void put(const char *txt) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

static int valid_mode(const char *m) { return !strcmp(m, "off") || !strcmp(m, "host") || !strcmp(m, "join") || !strcmp(m, "solo"); }

/* run both readers on `txt`; the runtime keeps the documented differences:
 *  - an unknown mode string is kept verbatim by the runtime (it then logs an error) and is "off" in config_ini
 *  - an empty host is kept by the runtime and reads as the default 127.0.0.1 in config_ini */
static void parity(const char *name, const char *txt) {
    put(txt);
    char mode[16] = "off", host[64] = "127.0.0.1";
    int players = 2, port = 47900, delay = -1;
    rt_sio_read_ini(path, mode, &players, &port, host, &delay);
    KirbyConfig c;
    cfg_load(path, &c);
    const char *want_mode = valid_mode(mode) ? mode : "off";
    const char *want_host = host[0] ? host : "127.0.0.1";
    int ok = !strcmp(c.link_mode, want_mode) && c.link_players == players && c.link_port == port && !strcmp(c.link_host, want_host) &&
             c.link_delay == delay;
    if (!ok)
        printf("  [%s] runtime: mode=%s players=%d port=%d host=%s | config_ini: mode=%s players=%d port=%d host=%s\n", name, mode,
               players, port, host, c.link_mode, c.link_players, c.link_port, c.link_host);
    if (c.link_delay != delay) printf("  [%s] delay: runtime=%d config_ini=%d" "\n", name, delay, c.link_delay);
    CHECK(ok, name);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    snprintf(path, sizeof path, "%s/test_link_ini.ini", dir);
    KirbyConfig c, d;

    /* ---- defaults match the runtime's (rt_main.c) ---- */
    cfg_defaults(&d);
    CHECK(!strcmp(d.link_mode, "off") && !strcmp(d.link_host, "127.0.0.1") && d.link_port == 47900 && d.link_players == 2,
          "defaults: mode=off host=127.0.0.1 port=47900 players=2 (as rt_main.c)");
    CHECK(d.link_delay == -1 && !strcmp(d.link_name, "Player"), "defaults: delay=auto name=Player");
    CHECK(cfg_validate(&d) == 0, "defaults need no fixes");

    /* ---- round trip ---- */
    KirbyConfig w;
    cfg_defaults(&w);
    snprintf(w.link_mode, sizeof w.link_mode, "join");
    snprintf(w.link_host, sizeof w.link_host, "192.168.1.20");
    w.link_port = 5123; w.link_players = 4; w.link_delay = 6;
    snprintf(w.link_name, sizeof w.link_name, "Kirby One");
    CHECK(cfg_save(path, &w) == 0, "save [link]");
    CHECK(cfg_load(path, &c) == 0 && cfg_equal(&c, &w), "[link] round trip (every field)");
    {
        FILE *f = fopen(path, "rb");
        char buf[4096] = "";
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        const char *l = strstr(buf, "[link]"), *la = strstr(buf, "[launcher]");
        CHECK(l && la && la < l, "[link] comes after [launcher]");
        CHECK(strstr(buf, "mode=join") && strstr(buf, "host=192.168.1.20") && strstr(buf, "port=5123") &&
              strstr(buf, "players=4") && strstr(buf, "delay=6") && strstr(buf, "name=Kirby One"), "keys written as in the contract");
    }
    /* the runtime reads what the UI wrote */
    {
        char mode[16] = "off", host[64] = "127.0.0.1";
        int players = 2, port = 47900, delay = -1;
        rt_sio_read_ini(path, mode, &players, &port, host, &delay);
        CHECK(!strcmp(mode, "join") && players == 4 && port == 5123 && !strcmp(host, "192.168.1.20") && delay == 6, "runtime reads the file the UI saved");
        w.link_delay = -1;
        cfg_save(path, &w);
        delay = 3;
        rt_sio_read_ini(path, mode, &players, &port, host, &delay);
        CHECK(delay == -1, "delay=auto written by the UI is read as auto by the runtime");
    }

    /* ---- validation of the struct (cfg_save normalises) ---- */
    {
        KirbyConfig z;
        memset(&z, 0, sizeof z);
        cfg_save(path, &z);
        cfg_load(path, &c);
        CHECK(!strcmp(c.link_mode, "off") && !strcmp(c.link_host, "127.0.0.1") && c.link_port == 47900 && c.link_players == 2 &&
              c.link_delay == 0 && !strcmp(c.link_name, "Player"),
              "a zeroed struct is written as the [link] defaults (delay 0 is a valid value and is kept)");
        KirbyConfig g;
        cfg_defaults(&g);
        snprintf(g.link_mode, sizeof g.link_mode, "weird");
        snprintf(g.link_host, sizeof g.link_host, "bad host;x");
        g.link_port = 70000; g.link_players = 9; g.link_delay = 9;
        snprintf(g.link_name, sizeof g.link_name, "a=b;c");
        CHECK(cfg_validate(&g) >= 6, "garbage [link] fields are all fixed");
        CHECK(!strcmp(g.link_mode, "off") && !strcmp(g.link_host, "127.0.0.1") && g.link_port == 47900 && g.link_players == 2 &&
              g.link_delay == -1 && !strcmp(g.link_name, "Player"), "...to their defaults");
    }

    /* ---- parity with the runtime parser ---- */
    parity("empty file", "");
    parity("full valid", "[link]\r\nmode=host\r\nplayers=3\r\nport=5000\r\nhost=10.0.0.5\r\n");
    parity("LF endings, no trailing newline", "[link]\nmode=join\nhost=example.org\nport=1234");
    parity("section case", "[LINK]\r\nmode=join\r\nport=2222\r\n");
    parity("key case", "[link]\r\nMODE=solo\r\nPLAYERS=4\r\nPORT=3333\r\nHOST=h1\r\n");
    parity("spaces around key and value", "[link]\r\n  port  =  5000  \r\n  mode = join  \r\n");
    parity("unknown mode is not a playable mode", "[link]\r\nmode=Host\r\n");
    parity("players out of range keeps the default", "[link]\r\nplayers=5\r\n");
    parity("players=1", "[link]\r\nplayers=1\r\n");
    parity("players=0", "[link]\r\nplayers=0\r\n");
    parity("players=3x (atoi reads 3)", "[link]\r\nplayers=3x\r\n");
    parity("players=abc", "[link]\r\nplayers=abc\r\n");
    parity("port=0", "[link]\r\nport=0\r\n");
    parity("port=70000", "[link]\r\nport=70000\r\n");
    parity("port=65535", "[link]\r\nport=65535\r\n");
    parity("port=47x (atoi reads 47)", "[link]\r\nport=47x\r\n");
    parity("port=-5", "[link]\r\nport=-5\r\n");
    parity("port=abc", "[link]\r\nport=abc\r\n");
    parity("host with ; comment", "[link]\r\nhost=10.0.0.5 ; at home\r\n");
    parity("host with # cut", "[link]\r\nhost=a#b\r\n");
    parity("whole line comment", "[link]\r\n; port=1111\r\n# port=2222\r\nport=3333\r\n");
    parity("duplicate keys: later valid wins, later invalid keeps the earlier", "[link]\r\nport=1000\r\nport=99999\r\nplayers=3\r\nplayers=7\r\n");
    parity("later valid duplicate", "[link]\r\nport=1000\r\nport=2000\r\n");
    parity("[link] after other sections", "[video]\r\nscale=2\r\nport=1\r\n[audio]\r\nvolume=50\r\n[link]\r\nport=4444\r\nmode=join\r\n");
    parity("keys of other sections do not leak into [link]", "[link]\r\nmode=host\r\n[video]\r\nport=1\r\nplayers=4\r\nhost=zzz\r\n");
    parity("[link] keys outside the section are ignored", "port=1111\r\nmode=host\r\n[video]\r\nscale=3\r\n");
    parity("section with inner spaces is not [link]", "[ link ]\r\nport=1234\r\n");
    parity("very long host is cut to 63 characters",
           "[link]\r\nhost=aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffgggggggggghhhhhhhhhh\r\n");
    parity("empty host (runtime keeps it; config_ini reads the default)", "[link]\r\nhost=\r\n");
    parity("delay=auto", "[link]\r\ndelay=auto\r\n");
    parity("delay=0", "[link]\r\ndelay=0\r\n");
    parity("delay=8", "[link]\r\ndelay=8\r\n");
    parity("delay=9 (out of range keeps the default)", "[link]\r\ndelay=9\r\n");
    parity("delay=-1 (not accepted; auto is spelled out)", "[link]\r\ndelay=-1\r\n");
    parity("delay=3x (atoi reads 3)", "[link]\r\ndelay=3x\r\n");
    parity("delay=Auto (case matters in both)", "[link]\r\ndelay=Auto\r\n");
    parity("mode=off", "[link]\r\nmode=off\r\nport=1500\r\n");
    parity("v1 flat file has no [link]", "scale=3\r\nmute=1\r\n");

    /* ---- config_ini only: BOM and the keys the runtime does not read yet ---- */
    {
        FILE *f = fopen(path, "wb");
        if (f) {
            fputc(0xEF, f); fputc(0xBB, f); fputc(0xBF, f);
            fputs("[link]\r\nmode=host\r\nport=6000\r\ndelay=4\r\nname=Pink\r\n", f);
            fclose(f);
        }
        cfg_load(path, &c);
        CHECK(!strcmp(c.link_mode, "host") && c.link_port == 6000 && c.link_delay == 4 && !strcmp(c.link_name, "Pink"),
              "config_ini skips a UTF-8 BOM before [link]; delay and name are read");
    }
    put("[link]\r\ndelay=150\r\nname=ok\r\n");
    cfg_load(path, &c);
    CHECK(c.link_delay == -1 && !strcmp(c.link_name, "ok"), "delay out of range is ignored (default stays)");
    put("[link]\r\ndelay=0\r\n");
    cfg_load(path, &c);
    CHECK(c.link_delay == 0, "delay=0 is valid");
    put("[link]\r\nname=Averyveryverylongname\r\n");
    cfg_load(path, &c);
    CHECK(strlen(c.link_name) == 15, "name is cut to 15 characters");
    put("[link]\r\nhost=my host\r\n");
    cfg_load(path, &c);
    CHECK(!strcmp(c.link_host, "127.0.0.1"), "a host with a space is rejected (the runtime would keep it)");

    remove(path);
    printf(fails ? "RESULT: %d failure(s)\n" : "RESULT: OK\n", fails);
    return fails != 0;
}
