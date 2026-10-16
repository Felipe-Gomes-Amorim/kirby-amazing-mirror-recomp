/* rt_save.c - save da SRAM (32 KiB crus) gravado de forma ATOMICA.
 *
 * - O barramento conta cada escrita que MUDA a SRAM (gba_sram_gen). Por quadro, rt_save_tick() ve se
 *   ha escrita nova; ~1 s depois da ULTIMA escrita (ou, no maximo, 5 s depois da primeira, se o jogo
 *   escreve sem parar) grava.
 * - Gravar = escrever <save>.tmp (fflush + FlushFileBuffers) e RENOMEAR sobre o .sav
 *   (MoveFileExW REPLACE_EXISTING|WRITE_THROUGH; rename() fora do Windows). O .sav nunca e escrito
 *   diretamente: morrer no meio deixa o save antigo ou o novo, nunca um arquivo parcial.
 * - Tambem grava no atexit (inclui F1/janela fechada), em CTRL_CLOSE/CTRL_C/LOGOFF/SHUTDOWN e SIGTERM.
 * - Falha: avisa 1x no kirby.log por sequencia de falhas e tenta de novo no proximo ciclo. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif
#include "gba_bus.h"

double rt_now_us(void);

#define SAVE_BYTES 0x8000
#define DEBOUNCE_US 1000000.0
#define MAX_DELAY_US 5000000.0

static char sav_path[1100], tmp_path[1110], log_path[1100];
static uint32_t flushed_gen;
static double dirty_since, last_change, retry_at;
static uint32_t seen_gen;
static int failing;
static double (*clock_fn)(void);
uint64_t rt_save_flushes, rt_save_failures;
double rt_save_flush_us, rt_save_tick_us;
#ifdef _WIN32
static CRITICAL_SECTION cs;
static int cs_ready;
#endif

static double now(void) { return clock_fn ? clock_fn() : rt_now_us(); }

void rt_log_set(const char *path) { snprintf(log_path, sizeof log_path, "%s", path ? path : ""); }

void rt_log(const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
    if (log_path[0]) {
        FILE *f = fopen(log_path, "ab");
        if (f) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            char ts[32] = "";
            if (tm) strftime(ts, sizeof ts, "%H:%M:%S", tm);
            fprintf(f, "%s %s\n", ts, msg);
            fclose(f);
        }
    }
}

#ifdef _WIN32
static int to_wide(const char *u8, wchar_t *w, int cap) {
    return MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, cap) > 0;
}
#endif

/* 0 = ok */
static int write_atomic(const uint8_t *buf) {
#ifdef _WIN32
    wchar_t wt[1200], ws[1200];
    if (!to_wide(tmp_path, wt, 1200) || !to_wide(sav_path, ws, 1200)) {
        /* nao e UTF-8 valido (ex.: ANSI do argv): usa a pagina de codigo ativa */
        if (!MultiByteToWideChar(CP_ACP, 0, tmp_path, -1, wt, 1200) ||
            !MultiByteToWideChar(CP_ACP, 0, sav_path, -1, ws, 1200))
            return -1;
    }
    FILE *f = _wfopen(wt, L"wb");
    if (!f) return -2;
    int ok = fwrite(buf, 1, SAVE_BYTES, f) == SAVE_BYTES && fflush(f) == 0 && _commit(_fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { _wremove(wt); return -3; }
    if (!MoveFileExW(wt, ws, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        _wremove(wt);
        return -4;
    }
    return 0;
#else
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -2;
    int ok = fwrite(buf, 1, SAVE_BYTES, f) == SAVE_BYTES && fflush(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp_path); return -3; }
    if (rename(tmp_path, sav_path) != 0) { remove(tmp_path); return -4; }
    return 0;
#endif
}

/* grava ja (se houver mudanca ou force). Seguro para chamar de qualquer thread/ponto. */
void rt_save_flush(const char *why, int force) {
    if (!sav_path[0]) return;
#ifdef _WIN32
    if (cs_ready) EnterCriticalSection(&cs);
#endif
    uint32_t gen = gba_sram_gen;
    if (force || gen != flushed_gen) {
        static uint8_t snap[SAVE_BYTES];
        gba_sram_get(snap);
        double t0 = rt_now_us();
        int rc = write_atomic(snap);
        rt_save_flush_us += rt_now_us() - t0;
        if (rc == 0) {
            flushed_gen = gen;
            dirty_since = 0;
            rt_save_flushes++;
            if (failing) rt_log("[save] write recovered (%s)", why);
            failing = 0;
        } else {
            rt_save_failures++;
            retry_at = now() + DEBOUNCE_US;   /* nova tentativa no proximo ciclo; o jogo segue */
            if (!failing) rt_log("[save] could not write %s (error %d, %s); will retry", sav_path, rc, why);
            failing = 1;
        }
    }
#ifdef _WIN32
    if (cs_ready) LeaveCriticalSection(&cs);
#endif
}

/* chamado 1x por quadro (VBlankIntrWait) */
static uint64_t frame_no, v_flushes, v_first, v_last;
static uint32_t v_seen;
static int v_dirty;

void rt_save_tick(void) {
    if (!sav_path[0]) return;
    uint32_t gen = gba_sram_gen;
    /* politica simulada em TEMPO DE JOGO (60 quadros = 1 s), p/ medir flushes/min mesmo no --bench */
    frame_no++;
    if (gen != v_seen) { v_seen = gen; v_last = frame_no; if (!v_dirty) { v_dirty = 1; v_first = frame_no; } }
    if (v_dirty && (frame_no - v_last >= 60 || frame_no - v_first >= 300)) { v_flushes++; v_dirty = 0; }
    if (gen == flushed_gen) return;   /* caminho comum: so a comparacao */
    double t0 = rt_now_us();
    double n = now();
    if (gen != seen_gen) {            /* escrita nova desde o ultimo quadro */
        seen_gen = gen;
        last_change = n;
        if (dirty_since == 0) dirty_since = n;
    }
    if (n >= retry_at && (n - last_change >= DEBOUNCE_US || n - dirty_since >= MAX_DELAY_US))
        rt_save_flush("debounce", 0);
    rt_save_tick_us += rt_now_us() - t0;
}

static void atexit_flush(void) { rt_save_flush("exit", 0); }

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;   /* CLOSE / C / BREAK / LOGOFF / SHUTDOWN: o handler roda em outra thread */
    rt_save_flush("console control event", 0);
    return FALSE;   /* deixa o tratamento padrao encerrar o processo */
}
#endif
static void sig_handler(int sig) {
    (void)sig;
    rt_save_flush("SIGTERM", 0);
    _Exit(143);
}

void rt_save_set_clock(double (*fn)(void)) { clock_fn = fn; }

/* carrega (ou cria com 0xFF) e registra os ganchos de saida */
void rt_save_open(const char *path) {
    snprintf(sav_path, sizeof sav_path, "%s", path);
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    uint8_t buf[SAVE_BYTES];
    memset(buf, 0xFF, sizeof buf);
#ifdef _WIN32
    wchar_t ws[1200];
    FILE *f = NULL;
    if (to_wide(sav_path, ws, 1200) || MultiByteToWideChar(CP_ACP, 0, sav_path, -1, ws, 1200)) f = _wfopen(ws, L"rb");
#else
    FILE *f = fopen(sav_path, "rb");
#endif
    if (f) {
        size_t n = fread(buf, 1, sizeof buf, f);
        (void)n;
        fclose(f);
    }
    gba_sram_set(buf);
    flushed_gen = seen_gen = gba_sram_gen;
    dirty_since = last_change = retry_at = 0;
#ifdef _WIN32
    InitializeCriticalSection(&cs);
    cs_ready = 1;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#endif
    signal(SIGTERM, sig_handler);
    atexit(atexit_flush);
}

void rt_save_report(void) {
    if (!sav_path[0]) return;
    printf("SAVE: %llu flushes (%llu in game time = %.2f per game-minute over %llu frames), %llu failures, "
           "%.2f ms writing files, tick cost %.3f ms total\n",
           (unsigned long long)rt_save_flushes, (unsigned long long)v_flushes,
           frame_no ? (double)v_flushes * 3584.0 / (double)frame_no : 0.0, (unsigned long long)frame_no,
           (unsigned long long)rt_save_failures, rt_save_flush_us / 1000.0, rt_save_tick_us / 1000.0);
}

/* ------------------------------------------------------------------ testes */
/* --selftest-save-writer FILE: grava sem parar (para o teste que mata o processo). Cada geracao N
 * escreve a SRAM inteira com o byte N e grava na hora; o .sav tem que ser sempre uniforme. */
int rt_selftest_save_writer(const char *path) {
    rt_save_open(path);
    for (unsigned n = 0;; n++) {
        for (uint32_t i = 0; i < SAVE_BYTES; i++) gba_write8(0x0E000000u + i, n & 0xFF);
        rt_save_flush("writer", 1);
    }
    return 0;
}

static double fake_t;
static double fake_clock(void) { return fake_t; }

/* debounce/limites com relogio falso + custo por quadro quando o jogo escreve muito */
int rt_selftest_save(const char *dir) {
    int bad = 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/selftest.sav", dir);
    remove(path);
    rt_save_open(path);
    rt_save_set_clock(fake_clock);
    fake_t = 1e6;
#define EXPECT(c, what) do { if (!(c)) { bad++; fprintf(stderr, "[selftest-save] FAIL: %s\n", what); } } while (0)
    /* 1. uma escrita: nada ate 1 s depois da ultima; depois 1 flush */
    gba_write8(0x0E000010u, 0x42);
    rt_save_tick();
    EXPECT(rt_save_flushes == 0, "no flush on the same frame");
    fake_t += 500000; rt_save_tick();
    EXPECT(rt_save_flushes == 0, "no flush at 0.5 s");
    fake_t += 600000; rt_save_tick();
    EXPECT(rt_save_flushes == 1, "flush ~1 s after the write");
    {
        FILE *f = fopen(path, "rb");
        uint8_t b[SAVE_BYTES];
        size_t n = f ? fread(b, 1, sizeof b, f) : 0;
        if (f) fclose(f);
        EXPECT(n == SAVE_BYTES && b[0x10] == 0x42, ".sav has the written byte (32768 bytes)");
    }
    /* o .tmp nao fica para tras */
    {
        char t[1210];
        snprintf(t, sizeof t, "%s.tmp", path);
        FILE *f = fopen(t, "rb");
        EXPECT(f == NULL, "no .tmp left behind");
        if (f) fclose(f);
    }
    /* 2. escrita identica (mesmo valor) nao suja */
    gba_write8(0x0E000010u, 0x42);
    fake_t += 3e6; rt_save_tick();
    EXPECT(rt_save_flushes == 1, "same-value write does not dirty the SRAM");
    /* 3. escrita continua: grava no maximo a cada 5 s */
    uint64_t f0 = rt_save_flushes;
    double cpu0 = rt_now_us();
    unsigned frames = 3600;   /* 1 min de jogo (a 60 q/s) escrevendo 64 bytes por quadro */
    double tick_us = 0;
    for (unsigned fr = 0; fr < frames; fr++) {
        for (int k = 0; k < 64; k++) gba_write8(0x0E000100u + (uint32_t)((fr * 64 + k) & 0x7FF), (uint8_t)(fr + k));
        fake_t += 16743.0;
        double t0 = rt_now_us();
        rt_save_tick();
        tick_us += rt_now_us() - t0;
    }
    uint64_t fl = rt_save_flushes - f0;
    fprintf(stderr, "[selftest-save] heavy writing (64 B/frame, 3600 frames = 1 min): %llu flushes/min, "
            "tick cost %.2f us/frame avg (%.1f ms total, incl. %.1f ms writing files); run took %.1f ms\n",
            (unsigned long long)fl, tick_us / frames, tick_us / 1000.0, rt_save_flush_us / 1000.0,
            (rt_now_us() - cpu0) / 1000.0);
    EXPECT(fl >= 10 && fl <= 13, "continuous writing flushes every ~5 s (10..13 per minute)");
    /* 4. falha de gravacao: 1 aviso, tenta de novo, recupera */
    char sav_keep[1100];
    snprintf(sav_keep, sizeof sav_keep, "%s", sav_path);
    snprintf(sav_path, sizeof sav_path, "%s/no_such_dir/x.sav", dir);
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", sav_path);
    gba_write8(0x0E000200u, 0x99);
    uint64_t fails0 = rt_save_failures;
    for (int i = 0; i < 6; i++) { fake_t += 1.2e6; rt_save_tick(); }
    EXPECT(rt_save_failures - fails0 >= 2, "failed writes are retried each cycle");
    snprintf(sav_path, sizeof sav_path, "%s", sav_keep);
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", sav_keep);
    fake_t += 1.2e6; rt_save_tick(); fake_t += 1.2e6; rt_save_tick();
    EXPECT(failing == 0, "recovered after the path works again");
    rt_save_set_clock(NULL);
    fprintf(stderr, "[selftest-save] %s\n", bad ? "FAILED" : "OK");
    return bad;
}
