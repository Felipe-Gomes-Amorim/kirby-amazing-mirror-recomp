/* kirby_ui.dll - progress window for the launcher: thin exported wrappers over hw/window_sdl.c. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "../hw/window_sdl.h"
#define EXPORT __declspec(dllexport)
EXPORT int kui_open(const char *title) {
    SDL_SetMainReady();
    /* do not turn console/process signals (Ctrl+C, SIGTERM, ...) into SDL_QUIT: the build must not be
     * cancelled by an event the user did not cause */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    return win_open_progress(title);
}
EXPORT int kui_progress(int percent, const char *text) { return win_progress(percent, text); }
EXPORT void kui_close(void) { win_close_progress(); }
