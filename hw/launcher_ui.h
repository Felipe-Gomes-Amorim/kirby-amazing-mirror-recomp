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
 *
 *   SAVE section (SETTINGS page): OPEN FOLDER, EXPORT... and IMPORT... are enabled only while a verified ROM
 *   is loaded (rom[0] != 0 and rom_ok == 1). Each returns a code; the launcher performs the action and calls
 *   kl_run again:
 *     KL_OPEN_SAVE_DIR = 3   open the folder that holds the .sav in the file explorer
 *     KL_EXPORT_SAVE   = 4   ask for a destination and copy the .sav there
 *     KL_IMPORT_SAVE   = 5   ask for a .sav to import; the UI has ALREADY asked the user to confirm
 *                            ("OVERWRITE CURRENT SAVE? A BACKUP IS KEPT" YES / NO) - the launcher must back
 *                            up the current save before overwriting it and validate the size (32768 bytes).
 *
 *   ONLINE page (card ONLINE on the home screen, enabled while a verified ROM is loaded):
 *   fields NAME / ADDRESS / PORT, PLAYERS 2-4, DELAY (AUTO or 0-2), NET AUTO/UDP/TCP, HOST|JOIN + the big action button,
 *   READY / START / LEAVE, and the lobby (4 slots with name, state and ping). The values are saved to kirby.ini [link] (mode/host/port/players/delay/name).
 *   Return codes (the launcher acts, then calls kl_run again; call kl_set_start_page(KL_PAGE_ONLINE) first so the
 *   screen reopens on the ONLINE page):
 *     KL_ONLINE_HOST  = 6   start hosting with [link] players/port/delay/name ([link] mode is now host)
 *     KL_ONLINE_JOIN  = 7   join [link] host:port ([link] mode is now join)
 *     KL_ONLINE_LEAVE = 8   leave / stop hosting ([link] mode is now off)
 *     KL_ONLINE_START = 9   host only: start the game now with the players that are connected (all of them ready)
 *     KL_ONLINE_READY = 10  toggle this player's READY flag (the game starts when everybody in the room is ready)
 *   The live data (slots, ping, messages) comes from a second poll set with kl_set_online_poll(), called with
 *   the normal poll (~10x/s, UI thread).
 *
 * The window follows the video options: size = 240*scale x 160*scale (never below 960x600, the logical
 * canvas of the UI), "Fullscreen" applies to the launcher too, and "Scaling" picks how the canvas is
 * mapped to the window: integer (whole-number zoom, black bars), fit (aspect preserved) or stretch
 * (fills the window). Pages: HOME (status, PLAY, cards SETTINGS / INPUT / ONLINE), SETTINGS (video, audio, saves),
 * INPUT (KEYBOARD / CONTROLLER tabs, editable: click or Enter on a key of the GBA outline, then press the new
 * key/button; Esc cancels; used keys are swapped; Esc/F11/F1 are refused) and ONLINE.
 * The first connected game controller can also drive the whole UI (d-pad + south = OK, east = back).
 * Text is drawn with baked Pixelify Sans / Silkscreen bitmaps (accents included). All UI text is English. */
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

enum { KL_QUIT = 0, KL_PLAY = 1, KL_PICK_ROM = 2, KL_OPEN_SAVE_DIR = 3, KL_EXPORT_SAVE = 4, KL_IMPORT_SAVE = 5,
       KL_ONLINE_HOST = 6, KL_ONLINE_JOIN = 7, KL_ONLINE_LEAVE = 8, KL_ONLINE_START = 9,
       KL_ONLINE_READY = 10 };

/* ---- ONLINE page: live data from the launcher ---- */
enum {   /* KlOnline.state */
    KLO_OFFLINE = 0,        /* not connected: HOST / JOIN enabled, fields editable */
    KLO_HOSTING = 1,        /* hosting, waiting for players (START enabled with >= 2 connected) */
    KLO_CONNECTING = 2,     /* joining in progress */
    KLO_CONNECTED = 3,      /* joined a host */
    KLO_PLAYING = 4,        /* a game is running */
    KLO_DESYNC = 5,         /* desync detected */
    KLO_LOST = 6            /* connection lost (LEAVE to reset) */
};
enum { KLS_EMPTY = 0, KLS_CONNECTING = 1, KLS_CONNECTED = 2, KLS_READY = 3, KLS_LOST = 4 };   /* KlOnline.slots[i] */

enum { KLQ_UNKNOWN = 0, KLQ_GOOD = 1, KLQ_FAIR = 2, KLQ_POOR = 3 };   /* KlOnline.quality */

typedef struct {
    int state;                /* KLO_* */
    int slots[4];             /* KLS_* for P1..P4 (P1 is the host) */
    int ping_ms[4];           /* round trip in ms; < 0 = unknown (shown only for connected/ready slots) */
    char names[4][16];        /* player names ('' = unknown) */
    int self_slot;            /* this player's slot 0..3, -1 = unknown (highlighted in the list) */
    char my_address[48];      /* host: address the others should use, e.g. "192.168.1.5:47900" (COPY button) */
    char message[96];         /* optional line under the state (errors, hints); '' = the UI shows its own hint */
    /* ---- added later, at the end (older launchers that zero the struct keep working) ---- */
    int quality;              /* KLQ_*: connection quality while PLAYING */
    int stalls_100ms;         /* pauses > 100 ms during this session */
    int ping_p95_ms;          /* round trip p95 in ms (< 0 or 0 with quality 0 = unknown) */
    char addresses[4][48];    /* HOSTING: up to 4 addresses of this PC ("ip:port"), '' ends the list; empty list -> my_address */
} KlOnline;
typedef void (*KlOnlinePoll)(KlOnline *out);   /* the UI zeroes *out before each call */

enum { KL_PAGE_MAIN = 0, KL_PAGE_KEYBOARD = 1, KL_PAGE_CONTROLLER = 2, KL_PAGE_ONLINE = 3, KL_PAGE_SETTINGS = 4 };

KL_API int kl_run(const char *ini_path, KlPoll poll);
KL_API const char *kl_dropped_path(void);        /* path dropped on the window (valid until the next kl_run) */
KL_API void kl_set_online_poll(KlOnlinePoll poll);   /* NULL = no live data (everything shows as offline/empty) */
KL_API void kl_set_start_page(int page);          /* KL_PAGE_* (default MAIN): the page the next kl_run calls open on */

/* ---- test hooks (headless tests drive the UI with simulated events) ---- */
KL_API void kl_set_poll_interval(int ms);         /* default 100 */
KL_API int kl_test_widget(const char *id, int *x, int *y, int *w, int *h);  /* 1 = found; logical coords */
KL_API int kl_test_enabled(const char *id);       /* 1 = exists and is enabled */
KL_API const char *kl_test_focus(void);           /* id of the focused widget */
KL_API void kl_test_set_focus(const char *id);
KL_API int kl_test_snapshot(uint32_t *out, int cap, int *w, int *h);       /* ARGB of the last frame (KL_W x KL_H) */
KL_API const char *kl_test_page(void);            /* "main" or "keyboard" / "controller" info page */
KL_API void kl_test_video(int *win_w, int *win_h, int *fullscreen);        /* actual window size / state */
KL_API unsigned kl_test_window_id(void);        /* SDL window id: simulated events must carry it */
KL_API int kl_test_capture(void);                 /* CFG_BTN_* being remapped, -1 = none */
KL_API const char *kl_test_note(void);            /* current warning/confirmation line ('' = none) */
KL_API void kl_test_fake_pad(int present, int axis_x, int axis_y);   /* -1 = use the real controller */
KL_API int kl_test_pad_present(void);
KL_API int kl_test_editing(void);                 /* text field being typed in: 0 name, 1 address, 2 port, -1 none */
KL_API int kl_test_keypad(void);                  /* field edited with the on-screen keypad (0..2), -1 = closed */
KL_API const char *kl_test_field(int f);          /* current text of field f (0 name, 1 address, 2 port) */
KL_API int kl_test_addr_sel(void);                /* selected address in the HOSTING list (0..3) */
KL_API int kl_test_confirm(void);                 /* 1 while the import confirmation is open */
KL_API int kl_test_text_count(void);              /* free texts drawn on the last frame (not inside buttons) */
KL_API int kl_test_text(int i, int *x, int *y, int *w, int *h);   /* bounding box of free text i */
KL_API int kl_test_bar(int *x, int *y, int *w, int *h);           /* progress bar rectangle (1 = drawn) */

#define KL_W 960    /* logical canvas */
#define KL_H 600

#endif
