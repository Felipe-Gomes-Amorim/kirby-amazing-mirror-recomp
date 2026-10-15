/* launcher_ui.h - Kirby Recomp launcher screen (SDL2). Built as kirby_ui.dll and driven by the
 * Python launcher through ctypes; it can also be linked statically (tests).
 *
 * The UI owns the modal loop; Python only answers poll(). Settings are read from / written to the
 * kirby.ini given to kl_run (see config_ini.h) as soon as the user changes something.
 *
 *   state: 0 none (no ROM / idle), 1 preparing fast mode (Play DISABLED),
 *          2 fast mode ready + full build running (you can play), 3 compiled version ready, 4 error.
 *   kl_run returns KL_PLAY, KL_QUIT or KL_PICK_ROM (the user clicked "Select ROM..." or dropped a file
 *   on the window; the dropped path, if any, is in kl_dropped_path()).
 * All UI text is English; the font has no accents. */
#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H
#include <stdint.h>

#if defined(_WIN32) && defined(KL_BUILD_DLL)
#define KL_API __declspec(dllexport)
#else
#define KL_API
#endif

typedef struct {
    int state;
    int percent;          /* 0..100, shown as a progress bar for states 1 and 2 */
    char text[96];        /* free text shown next to the status ("Analyzing...", "Compiling 4/16...") */
    char rom[260];        /* ROM path or file name shown in the header ("" = none) */
    int rom_ok;           /* 1 = ROM verified (SHA-1 matches the supported dump) */
} KlStatus;

typedef void (*KlPoll)(KlStatus *out);   /* called ~10x/s from the UI thread */

enum { KL_QUIT = 0, KL_PLAY = 1, KL_PICK_ROM = 2 };

KL_API int kl_run(const char *ini_path, KlPoll poll);
KL_API const char *kl_dropped_path(void);        /* path dropped on the window (valid until the next kl_run) */

/* ---- test hooks (headless tests drive the UI with simulated events) ---- */
KL_API void kl_set_poll_interval(int ms);         /* default 100 */
KL_API int kl_test_widget(const char *id, int *x, int *y, int *w, int *h);  /* 1 = found; logical coords */
KL_API const char *kl_test_focus(void);           /* id of the focused widget */
KL_API void kl_test_set_focus(const char *id);
KL_API int kl_test_snapshot(uint32_t *out, int cap, int *w, int *h);       /* ARGB of the last frame */
KL_API const char *kl_test_page(void);            /* "main" or "keyboard" / "controller" info page */

#define KL_W 640
#define KL_H 420

#endif
