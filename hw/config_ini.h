/* config_ini.h - configuracoes do jogo em kirby.ini (ao lado do save). Sem dependencia de SDL.
 *
 * Formato (texto, UTF-8/ASCII; `;` ou `#` iniciam comentario; `[kirby]` e opcional):
 *     scale=3        ; 1..6, tamanho da janela (multiplo de 240x160)
 *     mute=0         ; 1 = sem audio
 *     fullscreen=0   ; 1 = tela cheia (F11 alterna)
 *     vsync=0        ; 1 = PRESENTVSYNC (o relogio do audio ja paceia o jogo)
 * Chaves desconhecidas sao ignoradas; valores invalidos viram o padrao; cfg_save reescreve o arquivo. */
#ifndef CONFIG_INI_H
#define CONFIG_INI_H
#include <stddef.h>

typedef struct {
    int scale;        /* 1..6, padrao 3 */
    int mute;         /* 0/1, padrao 0 */
    int fullscreen;   /* 0/1, padrao 0 */
    int vsync;        /* 0/1, padrao 0 */
} KirbyConfig;

void cfg_defaults(KirbyConfig *c);
/* 0 = lido; 1 = arquivo inexistente (c ficou com os padroes); <0 = erro de leitura */
int cfg_load(const char *ini_path, KirbyConfig *c);
/* 0 = ok; <0 = nao conseguiu gravar */
int cfg_save(const char *ini_path, const KirbyConfig *c);
/* kirby.ini no mesmo diretorio do save (ou da ROM): "C:\\jogos\\kirby.sav" -> "C:\\jogos\\kirby.ini" */
void cfg_path_beside(const char *file_path, char *out, size_t cap);

#endif
