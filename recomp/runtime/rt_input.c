/* rt_input.c - configuracao (kirby.ini v2) e entrada unificada do kirby.exe: teclado +
 * SDL_GameController, hot-plug, zona morta, analogico opcional como direcional.
 *
 * Mapeamento por POSICAO fisica: GBA A = botao leste (SDL "b"), GBA B = botao sul (SDL "a").
 * Reservadas (nao remapeaveis): Esc (sair), F11 (tela cheia), F1 (voltar ao launcher: codigo 77).
 *
 * kirby.ini (UTF-8; ';' ou '#' comentam). Secoes v2:
 *   [video]      scale=3 fullscreen=0 integer_scale=1 vsync=0
 *   [audio]      volume=100 mute=0
 *   [keyboard]   a=X b=Z l=A r=S start=Return select=Backspace up=Up down=Down left=Left right=Right
 *   [controller] a=b b=a l=leftshoulder r=rightshoulder start=start select=back
 *                up=dpup down=dpdown left=dpleft right=dpright stick_as_dpad=1 deadzone=8000
 * Tambem aceita o v1 (chaves soltas ou [kirby]: scale, mute, fullscreen, vsync). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RT_WINDOW
#include <SDL.h>
#endif

/* parser compartilhado com a UI do launcher (hw/config_ini.c, sem SDL): uma TU so, para que o
 * harness de testes que compila apenas este arquivo continue funcionando */
#include "../../hw/config_ini.c"

enum { B_A, B_B, B_SELECT, B_START, B_RIGHT, B_LEFT, B_UP, B_DOWN, B_R, B_L, NBTN };
enum { SCALING_INTEGER, SCALING_FIT, SCALING_STRETCH };
/* ordem GBA deste arquivo (KEYINPUT) -> indice CFG_BTN_* do config_ini.h */
static const int CFG_IDX[NBTN] = {CFG_BTN_A, CFG_BTN_B, CFG_BTN_SELECT, CFG_BTN_START, CFG_BTN_RIGHT,
                                  CFG_BTN_LEFT, CFG_BTN_UP, CFG_BTN_DOWN, CFG_BTN_R, CFG_BTN_L};

typedef struct {
    int scale, fullscreen, integer_scale, vsync, volume, mute, stick_as_dpad, deadzone;
    char kb[NBTN][32];     /* nomes SDL de teclas */
    char pad[NBTN][32];    /* nomes SDL de botoes de controle */
    int scaling;           /* SCALING_*: [video] scaling=integer|fit|stretch (padrao fit) */
} RtCfg;

RtCfg rt_cfg;

void rt_cfg_defaults(void) {
    KirbyConfig c;
    cfg_defaults(&c);
    memset(&rt_cfg, 0, sizeof rt_cfg);
    rt_cfg.scale = c.scale; rt_cfg.fullscreen = c.fullscreen; rt_cfg.integer_scale = c.scaling == CFG_SCALING_INTEGER;
    rt_cfg.vsync = c.vsync; rt_cfg.volume = c.volume; rt_cfg.mute = c.mute;
    rt_cfg.stick_as_dpad = c.stick_as_dpad; rt_cfg.deadzone = c.deadzone;
    for (int i = 0; i < NBTN; i++) {
        snprintf(rt_cfg.kb[i], sizeof rt_cfg.kb[i], "%s", c.key[CFG_IDX[i]]);
        snprintf(rt_cfg.pad[i], sizeof rt_cfg.pad[i], "%s", c.pad[CFG_IDX[i]]);
    }
    rt_cfg.scaling = c.scaling;   /* CFG_SCALING_*: 0 integer, 1 fit, 2 stretch */
}

/* 0 = lido; 1 = arquivo inexistente (padroes); <0 = erro. Sempre validado (cfg_validate). */
int rt_cfg_load(const char *path) {
    KirbyConfig c;
    int rc = cfg_load(path, &c);
    rt_cfg_defaults();
    rt_cfg.scale = c.scale; rt_cfg.fullscreen = c.fullscreen; rt_cfg.integer_scale = c.scaling == CFG_SCALING_INTEGER;
    rt_cfg.vsync = c.vsync; rt_cfg.volume = c.volume; rt_cfg.mute = c.mute;
    rt_cfg.stick_as_dpad = c.stick_as_dpad; rt_cfg.deadzone = c.deadzone;
    for (int i = 0; i < NBTN; i++) {
        snprintf(rt_cfg.kb[i], sizeof rt_cfg.kb[i], "%s", c.key[CFG_IDX[i]]);
        snprintf(rt_cfg.pad[i], sizeof rt_cfg.pad[i], "%s", c.pad[CFG_IDX[i]]);
    }
    rt_cfg.scaling = c.scaling;
    return rc;
}

/* which: 0 scale, 1 fullscreen, 2 integer_scale, 3 vsync, 4 volume, 5 mute, 6 scaling */
int rt_cfg_get(int which) {
    switch (which) {
    case 0: return rt_cfg.scale;
    case 1: return rt_cfg.fullscreen;
    case 2: return rt_cfg.integer_scale;
    case 3: return rt_cfg.vsync;
    case 4: return rt_cfg.volume;
    case 6: return rt_cfg.scaling;
    case 7: rt_cfg.scaling = SCALING_FIT; return 0;   /* nao usado */
    default: return rt_cfg.mute;
    }
}

#ifdef RT_WINDOW
static SDL_Scancode kb_sc[NBTN];
static SDL_GameControllerButton pad_btn[NBTN];
static SDL_GameController *pads[4];
static int input_ready, scan_countdown;

static int reserved(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F1;
}

void rt_input_init(void) {
    if (input_ready) return;
    for (int i = 0; i < NBTN; i++) {
        SDL_Keycode k = SDL_GetKeyFromName(rt_cfg.kb[i]);
        SDL_Scancode sc = k != SDLK_UNKNOWN ? SDL_GetScancodeFromKey(k) : SDL_SCANCODE_UNKNOWN;
        if (sc == SDL_SCANCODE_UNKNOWN) sc = SDL_GetScancodeFromName(rt_cfg.kb[i]);
        kb_sc[i] = reserved(sc) ? SDL_SCANCODE_UNKNOWN : sc;   /* reservadas nao remapeiam */
        pad_btn[i] = SDL_GameControllerGetButtonFromString(rt_cfg.pad[i]);
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "input: game controller support unavailable: %s\n", SDL_GetError());
    input_ready = 1;
    scan_countdown = 0;
}

/* hot-plug: reabre/fecha de tempos em tempos (win_poll consome os eventos do SDL) */
static void scan_pads(void) {
    for (int p = 0; p < 4; p++)
        if (pads[p] && !SDL_GameControllerGetAttached(pads[p])) {
            SDL_GameControllerClose(pads[p]);
            pads[p] = NULL;
        }
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (!c) continue;
        int dup = 0;
        for (int p = 0; p < 4; p++) if (pads[p] == c) dup = 1;   /* Open devolve o mesmo se ja aberto */
        if (dup) continue;
        for (int p = 0; p < 4; p++)
            if (!pads[p]) { pads[p] = c; break; }
    }
}

uint16_t rt_input_poll(void) {
    uint16_t v = 0x03FF;
    if (!input_ready) rt_input_init();
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < NBTN; i++)
        if (kb_sc[i] != SDL_SCANCODE_UNKNOWN && k[kb_sc[i]]) v &= (uint16_t)~(1u << i);
    if (scan_countdown-- <= 0) { scan_pads(); scan_countdown = 30; }
    for (int p = 0; p < 4; p++) {
        SDL_GameController *c = pads[p];
        if (!c) continue;
        for (int i = 0; i < NBTN; i++)
            if (pad_btn[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(c, pad_btn[i]))
                v &= (uint16_t)~(1u << i);
        if (rt_cfg.stick_as_dpad) {
            int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            int dz = rt_cfg.deadzone;
            if (x > dz) v &= (uint16_t)~(1u << B_RIGHT);
            if (x < -dz) v &= (uint16_t)~(1u << B_LEFT);
            if (y > dz) v &= (uint16_t)~(1u << B_DOWN);
            if (y < -dz) v &= (uint16_t)~(1u << B_UP);
        }
    }
    /* direcionais opostos ao mesmo tempo nao existem no GBA: cancela o par */
    if (!(v & (1u << B_LEFT)) && !(v & (1u << B_RIGHT))) v |= (1u << B_LEFT) | (1u << B_RIGHT);
    if (!(v & (1u << B_UP)) && !(v & (1u << B_DOWN))) v |= (1u << B_UP) | (1u << B_DOWN);
    return v;
}

/* teclas reservadas (borda de subida): 1 = F1 (voltar ao launcher), 2 = F11 (tela cheia) */
int rt_input_hotkeys(void) {
    static int f1_prev, f11_prev;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int r = 0, f1 = k[SDL_SCANCODE_F1], f11 = k[SDL_SCANCODE_F11];
    if (f1 && !f1_prev) r |= 1;
    if (f11 && !f11_prev) r |= 2;
    f1_prev = f1;
    f11_prev = f11;
    return r;
}

void rt_input_close(void) {
    for (int p = 0; p < 4; p++)
        if (pads[p]) { SDL_GameControllerClose(pads[p]); pads[p] = NULL; }
}
#endif

/* ---- autoteste do parser de kirby.ini (sem SDL) ---- */
static int ini_case(const char *name, const char *text, int scale, int fs, int integer, int vol, int mute,
                    const char *kb_a, int dz, int stick) {
    FILE *f = fopen("kirby_selftest.ini", "wb");
    if (!f) return 1;
    fputs(text, f);
    fclose(f);
    rt_cfg_load("kirby_selftest.ini");
    remove("kirby_selftest.ini");
    int bad = rt_cfg.scale != scale || rt_cfg.fullscreen != fs || rt_cfg.integer_scale != integer ||
              rt_cfg.volume != vol || rt_cfg.mute != mute || strcmp(rt_cfg.kb[B_A], kb_a) ||
              rt_cfg.deadzone != dz || rt_cfg.stick_as_dpad != stick;
    if (bad)
        fprintf(stderr, "[selftest-ini] %s: scale=%d fs=%d int=%d vol=%d mute=%d a=%s dz=%d stick=%d\n", name,
                rt_cfg.scale, rt_cfg.fullscreen, rt_cfg.integer_scale, rt_cfg.volume, rt_cfg.mute,
                rt_cfg.kb[B_A], rt_cfg.deadzone, rt_cfg.stick_as_dpad);
    return bad;
}

int rt_selftest_ini(void) {
    int bad = 0;
    bad += ini_case("v2 completo",
                    "; comentario\n[video]\nscale=5\nfullscreen=1\ninteger_scale=0\n[audio]\nvolume=40\nmute=1\n"
                    "[keyboard]\na=Q # inline\n[controller]\ndeadzone=5000\nstick_as_dpad=0\n",
                    5, 1, 0, 40, 1, "Q", 5000, 0);
    bad += ini_case("valores invalidos -> padrao", "[video]\nscale=9\nfullscreen=x\n[audio]\nvolume=abc\n",
                    3, 0, 0, 100, 0, "X", 8000, 1);
    bad += ini_case("v1 solto", "scale=2\nmute=1\nfullscreen=1\n", 2, 1, 0, 100, 1, "X", 8000, 1);
    bad += ini_case("v1 [kirby]", "[kirby]\nscale=4\nvsync=1\n", 4, 0, 0, 100, 0, "X", 8000, 1);
    bad += ini_case("vazio / inexistente", "", 3, 0, 0, 100, 0, "X", 8000, 1);
    bad += ini_case("integer_scale=1 antigo", "[video]\ninteger_scale=1\n", 3, 0, 1, 100, 0, "X", 8000, 1);
    bad += ini_case("scaling=stretch", "[video]\nscaling=stretch\n", 3, 0, 0, 100, 0, "X", 8000, 1);
    bad += rt_cfg.scaling != CFG_SCALING_STRETCH;
    rt_cfg_load("nao_existe_kirby.ini");
    bad += rt_cfg.scale != 3;
    fprintf(stderr, "[selftest-ini] %s\n", bad ? "FAILED" : "OK");
    return bad;
}
