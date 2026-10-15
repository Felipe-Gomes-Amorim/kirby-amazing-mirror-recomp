/* test_launcher_ui.c - kirby.ini, soltar arquivo, tela de progresso, icone, titulo e tela cheia.
 * Roda sem tela (SDL_VIDEODRIVER=dummy e definido aqui). Independente de recomp/. */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include "../config_ini.h"
#include "../window_sdl.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (linha %d)\n", msg, __LINE__); fails++; } } while (0)

static void write_text(const char *path, const char *txt) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

static int count_color(const uint32_t *px, int w, int h, uint32_t argb) {
    int n = 0;
    for (int i = 0; i < w * h; i++) if (px[i] == argb) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char ini[600];
    snprintf(ini, sizeof ini, "%s/test_kirby.ini", dir);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);

    /* ---------------- kirby.ini ---------------- */
    KirbyConfig c, d;
    cfg_defaults(&d);
    CHECK(d.scale == 3 && d.mute == 0 && d.fullscreen == 0 && d.vsync == 0, "padroes: scale=3 mute=0 fullscreen=0 vsync=0");
    remove(ini);
    CHECK(cfg_load(ini, &c) == 1, "arquivo inexistente => 1");
    CHECK(c.scale == 3 && c.mute == 0, "inexistente: mantem os padroes");
    KirbyConfig w = { 5, 1, 1, 1 };
    CHECK(cfg_save(ini, &w) == 0, "cfg_save");
    CHECK(cfg_load(ini, &c) == 0, "cfg_load apos save");
    CHECK(c.scale == 5 && c.mute == 1 && c.fullscreen == 1 && c.vsync == 1, "ida e volta");
    write_text(ini, "; comentario\r\n[kirby]\r\n  SCALE = 9 ; fora da faixa\r\nmute=1 # silencio\r\nfullscreen=abc\r\nvsync=1\r\n"
                    "desconhecido=7\r\nlinha sem igual\r\n");
    CHECK(cfg_load(ini, &c) == 0, "arquivo manual");
    CHECK(c.scale == 3, "scale fora da faixa => padrao");
    CHECK(c.mute == 1 && c.vsync == 1, "mute/vsync lidos com comentarios");
    CHECK(c.fullscreen == 0, "valor invalido => padrao");
    write_text(ini, "scale=2x\r\nmute=\r\n");
    cfg_load(ini, &c);
    CHECK(c.scale == 3 && c.mute == 0, "'2x' e vazio => padrao");
    KirbyConfig big = { 99, 7, -1, 0 };
    cfg_save(ini, &big);
    cfg_load(ini, &c);
    CHECK(c.scale == 6 && c.mute == 1 && c.fullscreen == 1 && c.vsync == 0, "save normaliza (scale<=6, flags qualquer nao-zero => 1)");
    char beside[300];
    cfg_path_beside("C:\\jogos\\Kirby & The Amazing Mirror (USA).sav", beside, sizeof beside);
    CHECK(strcmp(beside, "C:\\jogos\\kirby.ini") == 0, "kirby.ini no diretorio do save (barra invertida)");
    cfg_path_beside("/home/u/roms/baserom.gba", beside, sizeof beside);
    CHECK(strcmp(beside, "/home/u/roms/kirby.ini") == 0, "kirby.ini no diretorio (barra normal)");
    cfg_path_beside("baserom.sav", beside, sizeof beside);
    CHECK(strcmp(beside, "kirby.ini") == 0, "sem diretorio => kirby.ini");
    remove(ini);

    /* ---------------- icone ---------------- */
    uint32_t icon[1024];
    CHECK(win_icon_pixels(icon) == 0, "icone gerado");
    int opaque = 0;
    for (int i = 0; i < 1024; i++) if (icon[i] >> 24 == 0xFF) opaque++;
    CHECK(icon[0] >> 24 == 0, "canto transparente");
    CHECK(icon[15 * 32 + 16] == 0xFFFF9FC8u, "centro rosa");
    CHECK(icon[12 * 32 + 12] == 0xFF1A1A3Au || icon[12 * 32 + 13] == 0xFF1A1A3Au, "olho escuro");
    CHECK(opaque > 450 && opaque < 900, "area opaca plausivel (corpo + pes)");
    printf("icone: %d pixels opacos de 1024\n", opaque);
    {
        char path[700];
        snprintf(path, sizeof path, "%s/ui_icon.ppm", dir);
        FILE *o = fopen(path, "wb");
        if (o) {
            fprintf(o, "P6\n32 32\n255\n");
            for (int i = 0; i < 1024; i++) {      /* fundo xadrez p/ ver a transparencia */
                uint32_t bg = (((i % 32) / 4 + (i / 32) / 4) & 1) ? 0xC8C8C8u : 0x909090u;
                uint32_t a8 = icon[i] >> 24, c = icon[i];
                int r = (int)(((c >> 16 & 255) * a8 + (bg >> 16 & 255) * (255 - a8)) / 255);
                int g = (int)(((c >> 8 & 255) * a8 + (bg >> 8 & 255) * (255 - a8)) / 255);
                int bl = (int)(((c & 255) * a8 + (bg & 255) * (255 - a8)) / 255);
                fputc(r, o); fputc(g, o); fputc(bl, o);
            }
            fclose(o);
        }
    }

    /* ---------------- janela do jogo: titulo, soltar arquivo, tela cheia ---------------- */
    CHECK(win_open(NULL, 2) == 0, "win_open(NULL) com driver dummy");
    CHECK(strcmp(win_get_title(), WIN_TITLE) == 0 && strcmp(WIN_TITLE, "Kirby & The Amazing Mirror - Recomp") == 0,
          "titulo padrao 'Kirby & The Amazing Mirror - Recomp'");
    CHECK(win_dropped_path() == NULL, "sem drop: NULL");
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_DROPFILE;
    ev.drop.file = SDL_strdup("C:\\Users\\x\\Documents\\Kirby & The Amazing Mirror (USA).gba");
    SDL_PushEvent(&ev);
    CHECK(win_poll() == 1, "win_poll segue depois do drop");
    const char *dp = win_dropped_path();
    CHECK(dp && strcmp(dp, "C:\\Users\\x\\Documents\\Kirby & The Amazing Mirror (USA).gba") == 0, "caminho solto devolvido");
    CHECK(win_dropped_path() == NULL, "devolvido uma vez so");
    SDL_zero(ev);
    ev.type = SDL_DROPFILE;
    ev.drop.file = SDL_strdup("D:\\outra.gba");
    SDL_PushEvent(&ev);
    win_poll();
    dp = win_dropped_path();
    CHECK(dp && strcmp(dp, "D:\\outra.gba") == 0, "segundo drop substitui");
    win_set_fullscreen(1);
    win_poll();
    int fs = win_is_fullscreen();
    printf("tela cheia (driver dummy): %s\n", fs ? "refletida" : "nao refletida pelo driver (sem teste)");
    win_set_fullscreen(0);
    win_poll();
    CHECK(!win_is_fullscreen(), "volta ao modo janela");

    /* ---------------- tela de progresso ---------------- */
    CHECK(win_open_progress("Kirby Recomp") == 0, "win_open_progress");
    static uint32_t px[480 * 140];
    int pw = 0, ph = 0;
    CHECK(win_progress(0, "ANALISANDO...") == 1, "win_progress(0) continua");
    CHECK(win_progress_read(px, 480 * 140, &pw, &ph) == 0 && pw == 480 && ph == 140, "leitura dos pixels da janela");
    const uint32_t FILL = 0xFFFF78AAu, TEXT = 0xFFE6E6F0u, TITLE = 0xFFFF9FC8u;
    int f0 = count_color(px, pw, ph, FILL);
    int title_px = count_color(px, pw, ph, TITLE);
    CHECK(f0 == 0, "0%: sem preenchimento");
    CHECK(title_px > 50, "titulo desenhado com a fonte 5x7");
    int bar_px = (480 - 44) * 20;
    win_progress(50, "COMPILANDO 4/16...");
    win_progress_read(px, 480 * 140, &pw, &ph);
    int f50 = count_color(px, pw, ph, FILL), t50 = count_color(px, pw, ph, TEXT);
    CHECK(f50 > bar_px * 49 / 100 && f50 < bar_px * 51 / 100, "50%: metade da barra preenchida");
    CHECK(t50 > 150, "texto 'Compilando 4/16...' desenhado");
    {   /* grava o quadro para conferir a olho */
        char path[700];
        snprintf(path, sizeof path, "%s/ui_progress.ppm", dir);
        FILE *o = fopen(path, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", pw, ph);
            for (int i = 0; i < pw * ph; i++) { fputc(px[i] >> 16 & 255, o); fputc(px[i] >> 8 & 255, o); fputc(px[i] & 255, o); }
            fclose(o);
        }
    }
    win_progress(100, "PRONTO");
    win_progress_read(px, 480 * 140, &pw, &ph);
    int f100 = count_color(px, pw, ph, FILL);
    CHECK(f100 == bar_px, "100%: barra cheia");
    win_progress(150, "");                               /* limita a 100 */
    win_progress_read(px, 480 * 140, &pw, &ph);
    CHECK(count_color(px, pw, ph, FILL) == bar_px, "percentual > 100 e limitado");
    win_progress(-5, "");
    win_progress_read(px, 480 * 140, &pw, &ph);
    CHECK(count_color(px, pw, ph, FILL) == 0, "percentual < 0 e limitado a 0");
    printf("barra: 0%%=%d 50%%=%d 100%%=%d (de %d), texto=%d px\n", f0, f50, f100, bar_px, t50);
    SDL_zero(ev);
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
    CHECK(win_progress(10, "x") == 0, "fechar a janela => win_progress devolve 0");
    win_close_progress();
    win_close();
    CHECK(win_progress(1, "x") == 0, "depois de fechar: 0");

    printf(fails ? "RESULTADO: %d falha(s)\n" : "RESULTADO: OK\n", fails);
    return fails != 0;
}
