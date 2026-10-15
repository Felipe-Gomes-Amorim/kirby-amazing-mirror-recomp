/* audio_sdl.c - ver audio_sdl.h. Anel + callback do SDL2 (travado com SDL_LockAudioDevice). */
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "hw.h"
#include "audio_sdl.h"

#define RING 16384                 /* quadros; potencia de 2 */
#define MAX_LATENCY_MS 200

static SDL_AudioDeviceID dev;
static int16_t ring[RING * 2];
static uint32_t rd, wr;            /* contadores de quadros */
static int rate, max_frames, underruns, dropped;

static void callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int want = len / 4, got = 0;
    while (got < want && rd != wr) {
        out[got * 2] = ring[(rd & (RING - 1)) * 2];
        out[got * 2 + 1] = ring[(rd & (RING - 1)) * 2 + 1];
        rd++; got++;
    }
    if (got < want) {
        memset(out + got * 2, 0, (size_t)(want - got) * 4);
        underruns++;
    }
}

int audio_open(void) {
    rate = hw_audio_rate();
    max_frames = rate * MAX_LATENCY_MS / 1000;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: SDL_InitSubSystem falhou: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* 0 = nao aceita formato diferente */
    if (!dev) {
        fprintf(stderr, "audio: nao foi possivel abrir a saida de audio (%d Hz estereo int16): %s\n"
                        "       (sem placa de som? tente SDL_AUDIODRIVER=dummy para rodar sem som)\n",
                rate, SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -2;
    }
    rd = wr = 0; underruns = dropped = 0;
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

void audio_push(const int16_t *lr, int frames) {
    if (!dev || frames <= 0) return;
    if (frames > max_frames) { dropped += frames - max_frames; lr += (frames - max_frames) * 2; frames = max_frames; }
    SDL_LockAudioDevice(dev);
    int have = (int)(wr - rd);
    int over = have + frames - max_frames;
    if (over > 0) { rd += (uint32_t)over; dropped += over; }       /* descarta o mais antigo */
    for (int i = 0; i < frames; i++) {
        ring[(wr & (RING - 1)) * 2] = lr[i * 2];
        ring[(wr & (RING - 1)) * 2 + 1] = lr[i * 2 + 1];
        wr++;
    }
    SDL_UnlockAudioDevice(dev);
}

int audio_queued(void) {
    if (!dev) return 0;
    SDL_LockAudioDevice(dev);
    int n = (int)(wr - rd);
    SDL_UnlockAudioDevice(dev);
    return n;
}
int audio_underruns(void) { return underruns; }
int audio_dropped(void) { return dropped; }

void audio_close(void) {
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
