/* config_ini.h - kirby.ini (v2): the single settings file read by the launcher UI and by kirby.exe.
 * No SDL dependency (key/button names are stored as strings; the SDL side validates them).
 *
 *   [video]       scale=3   fullscreen=0   integer_scale=1   vsync=0
 *   [audio]       volume=100   mute=0
 *   [keyboard]    a=X b=Z l=A r=S start=Return select=Backspace up=Up down=Down left=Left right=Right
 *   [controller]  a=b b=a l=leftshoulder r=rightshoulder start=start select=back
 *                 up=dpup down=dpdown left=dpleft right=dpright stick_as_dpad=1 deadzone=8000
 *   [launcher]    show=1   language=en
 *
 * Rules: `;` or `#` start a comment; keys are case-insensitive; unknown keys/sections are ignored;
 * an invalid value falls back to its default. Key names are SDL key names (SDL_GetKeyName),
 * controller names are SDL_GameControllerGetStringForButton names. The keyboard buttons are mapped
 * by physical POSITION: GBA A = X, GBA B = Z (like mGBA). Reserved keys (Escape, F11, F1) cannot be
 * bound; duplicate bindings reset the whole keyboard map to the defaults.
 *
 * Compatibility with v1 (flat file or [kirby] section with scale/mute/fullscreen/vsync): those keys are
 * accepted in the root, in [kirby], and in their v2 section. The v1 field names are unchanged. */
#ifndef CONFIG_INI_H
#define CONFIG_INI_H
#include <stddef.h>

enum {   /* GBA button indexes for KirbyConfig.key[] / .pad[] */
    CFG_BTN_A, CFG_BTN_B, CFG_BTN_L, CFG_BTN_R, CFG_BTN_START, CFG_BTN_SELECT,
    CFG_BTN_UP, CFG_BTN_DOWN, CFG_BTN_LEFT, CFG_BTN_RIGHT, CFG_BTN_COUNT
};

#define CFG_NAME_MAX 32

typedef struct {
    /* [video] / [audio]  (v1 names kept) */
    int scale;            /* 1..6, default 3 */
    int mute;             /* 0/1, default 0 */
    int fullscreen;       /* 0/1, default 0 */
    int vsync;            /* 0/1, default 0 */
    int integer_scale;    /* 0/1, default 1 */
    int volume;           /* 0..100, default 100 */
    /* [keyboard] SDL key names, indexed by CFG_BTN_* */
    char key[CFG_BTN_COUNT][CFG_NAME_MAX];
    /* [controller] SDL game-controller button names, indexed by CFG_BTN_* */
    char pad[CFG_BTN_COUNT][CFG_NAME_MAX];
    int stick_as_dpad;    /* 0/1, default 1 */
    int deadzone;         /* 0..32767, default 8000 */
    /* [launcher] */
    int launcher_show;    /* 0/1, default 1 (0 = open the game directly; Shift forces the launcher) */
    char language[8];     /* "en" */
} KirbyConfig;

void cfg_defaults(KirbyConfig *c);
void cfg_reset_keyboard(KirbyConfig *c);       /* keyboard + controller maps back to the defaults */
void cfg_reset_controller(KirbyConfig *c);
/* Fixes out-of-range values, empty/illegal names, reserved keys and duplicate bindings.
 * Returns the number of fixes applied (0 = the config was already valid). */
int cfg_validate(KirbyConfig *c);
/* 0 = read; 1 = file does not exist (c holds the defaults); <0 = read error. Always validated. */
int cfg_load(const char *ini_path, KirbyConfig *c);
/* 0 = ok; <0 = could not write. Writes a validated copy (v2 layout, CRLF). */
int cfg_save(const char *ini_path, const KirbyConfig *c);
/* "<dir of file_path>kirby.ini": C:\\games\\kirby.sav -> C:\\games\\kirby.ini */
void cfg_path_beside(const char *file_path, char *out, size_t cap);
/* "a", "b", "l", "r", "start", "select", "up", "down", "left", "right" */
const char *cfg_button_name(int btn);
/* 1 if the key name is reserved (Escape, F11, F1; case-insensitive) */
int cfg_key_reserved(const char *key_name);
/* field-by-field comparison (used by the round-trip test) */
int cfg_equal(const KirbyConfig *a, const KirbyConfig *b);

#endif
