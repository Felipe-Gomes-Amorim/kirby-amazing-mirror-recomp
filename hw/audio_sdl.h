/* audio_sdl.h - saida de audio via SDL2 para o hw/ (opcional; so este arquivo depende de SDL).
 * Uso por quadro:
 *     audio_open();                                     // 48000 Hz, estereo, int16
 *     ... hw_step(...);
 *     int16_t tmp[2 * 2048]; int n = hw_audio_read(tmp, 2048);
 *     audio_push(tmp, n);
 *     ...
 *     audio_close();
 * A fila guarda no maximo ~200 ms: se passar, o mais antigo e descartado (latencia nao acumula);
 * se faltar dado, o callback toca silencio. Falhas sao reportadas por retorno + mensagem em stderr. */
#ifndef AUDIO_SDL_H
#define AUDIO_SDL_H
#include <stdint.h>

int audio_open(void);                                  /* 0 = ok, <0 = erro (sem placa de som etc.) */
void audio_push(const int16_t *lr, int frames);        /* L,R intercalado; frames = pares */
int audio_queued(void);                                /* quadros ainda na fila */
int audio_underruns(void);                             /* vezes que o callback ficou sem dados */
int audio_dropped(void);                               /* quadros descartados por excesso de latencia */
void audio_close(void);

#endif
