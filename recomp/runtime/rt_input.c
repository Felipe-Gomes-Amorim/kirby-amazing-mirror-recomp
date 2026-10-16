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
#include <time.h>
#include <stdarg.h>
static SDL_Scancode kb_sc[NBTN];
static SDL_GameControllerButton pad_btn[NBTN];
static SDL_GameController *active;          /* UM controle: o primeiro; se sair, passa ao seguinte */
static int stick_x, stick_y;                 /* direcao com histerese: -1, 0, +1 */
static int input_ready, scan_countdown;
static int only_virtual;   /* teste: ignora controles reais da maquina */
static char log_path[1100];

void rt_input_set_log(const char *path) {
    snprintf(log_path, sizeof log_path, "%s", path ? path : "");
}

/* log do jogo: stderr + arquivo (kirby.exe e app grafico: sem console) */
static void ilog(const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[input] %s\n", msg);
    if (log_path[0]) {
        FILE *f = fopen(log_path, "ab");
        if (f) {
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            char ts[32] = "";
            if (tm) strftime(ts, sizeof ts, "%H:%M:%S", tm);
            fprintf(f, "%s [input] %s\n", ts, msg);
            fclose(f);
        }
    }
}

static int reserved(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F1;
}

static SDL_Scancode key_scancode(const char *name) {
    SDL_Keycode k = SDL_GetKeyFromName(name);
    SDL_Scancode sc = k != SDLK_UNKNOWN ? SDL_GetScancodeFromKey(k) : SDL_SCANCODE_UNKNOWN;
    if (sc == SDL_SCANCODE_UNKNOWN) sc = SDL_GetScancodeFromName(name);
    return sc;
}

void rt_input_init(void) {
    if (input_ready) return;
    KirbyConfig defs;
    cfg_defaults(&defs);
    for (int i = 0; i < NBTN; i++) {
        SDL_Scancode sc = key_scancode(rt_cfg.kb[i]);
        if (sc == SDL_SCANCODE_UNKNOWN) {          /* a name SDL does not know (typed by hand): use the default */
            ilog("unknown key name '%s' for button %d; using the default '%s'", rt_cfg.kb[i], i,
                 defs.key[CFG_IDX[i]]);
            sc = key_scancode(defs.key[CFG_IDX[i]]);
        }
        kb_sc[i] = reserved(sc) ? SDL_SCANCODE_UNKNOWN : sc;   /* reservadas nao remapeiam */
        pad_btn[i] = SDL_GameControllerGetButtonFromString(rt_cfg.pad[i]);
        if (pad_btn[i] == SDL_CONTROLLER_BUTTON_INVALID) {
            ilog("unknown controller button name '%s' for button %d; using the default '%s'", rt_cfg.pad[i], i,
                 defs.pad[CFG_IDX[i]]);
            pad_btn[i] = SDL_GameControllerGetButtonFromString(defs.pad[CFG_IDX[i]]);
        }
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        ilog("game controller support unavailable: %s", SDL_GetError());
    ilog("settings loaded: scale=%d scaling=%s fullscreen=%d volume=%d mute=%d | keyboard A=%s B=%s L=%s R=%s "
         "Start=%s Select=%s | controller A=%s B=%s stick_as_dpad=%d deadzone=%d",
         rt_cfg.scale, rt_cfg.scaling == 0 ? "integer" : rt_cfg.scaling == 2 ? "stretch" : "fit",
         rt_cfg.fullscreen, rt_cfg.volume, rt_cfg.mute, rt_cfg.kb[B_A], rt_cfg.kb[B_B], rt_cfg.kb[B_L],
         rt_cfg.kb[B_R], rt_cfg.kb[B_START], rt_cfg.kb[B_SELECT], rt_cfg.pad[B_A], rt_cfg.pad[B_B],
         rt_cfg.stick_as_dpad, rt_cfg.deadzone);
    input_ready = 1;
    scan_countdown = 0;
    active = NULL;
    stick_x = stick_y = 0;
}

static const char *pad_name(SDL_GameController *c) {
    const char *n = c ? SDL_GameControllerName(c) : NULL;
    return n ? n : "controller";
}

/* Mantem UM controle ativo. Se o ativo saiu, passa ao proximo da lista; novos entram so
 * quando nao ha ativo (conectar um segundo controle nao rouba a entrada do primeiro). */
static void manage_pad(int rescan) {
    if (active && !SDL_GameControllerGetAttached(active)) {
        ilog("controller disconnected: %s", pad_name(active));
        SDL_GameControllerClose(active);
        active = NULL;
        stick_x = stick_y = 0;
        rescan = 1;
    }
    if (active || !rescan) return;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        if (only_virtual && !SDL_JoystickIsVirtual(i)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (!c) continue;
        active = c;
        stick_x = stick_y = 0;
        ilog("controller connected: %s (using it; others are ignored)", pad_name(c));
        break;
    }
}

/* zona morta com histerese: entra em 100% da zona morta, sai em 85% (nao oscila na borda) */
static int hyst(int dir, int v, int dz) {
    int exit_at = dz * 85 / 100;
    if (dir > 0) {
        if (v >= exit_at) return 1;
        return v <= -dz ? -1 : 0;
    }
    if (dir < 0) {
        if (v <= -exit_at) return -1;
        return v >= dz ? 1 : 0;
    }
    if (v >= dz) return 1;
    if (v <= -dz) return -1;
    return 0;
}

uint16_t rt_input_poll(void) {
    uint16_t v = 0x03FF;
    if (!input_ready) rt_input_init();
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < NBTN; i++)
        if (kb_sc[i] != SDL_SCANCODE_UNKNOWN && k[kb_sc[i]]) v &= (uint16_t)~(1u << i);
    if (scan_countdown-- <= 0) { manage_pad(1); scan_countdown = 30; }
    else manage_pad(0);
    SDL_GameController *c = active;
    if (c) {
        for (int i = 0; i < NBTN; i++)
            if (pad_btn[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(c, pad_btn[i]))
                v &= (uint16_t)~(1u << i);
        if (rt_cfg.stick_as_dpad) {   /* so o analogico ESQUERDO; 8 direcoes por combinacao de eixos */
            int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            int dz = rt_cfg.deadzone > 0 ? rt_cfg.deadzone : 1;
            stick_x = hyst(stick_x, x, dz);
            stick_y = hyst(stick_y, y, dz);
            if (stick_x > 0) v &= (uint16_t)~(1u << B_RIGHT);
            if (stick_x < 0) v &= (uint16_t)~(1u << B_LEFT);
            if (stick_y > 0) v &= (uint16_t)~(1u << B_DOWN);
            if (stick_y < 0) v &= (uint16_t)~(1u << B_UP);
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
    if (active) { SDL_GameControllerClose(active); active = NULL; }
}

/* ---- autoteste com controles VIRTUAIS do SDL (sem hardware): botoes, analogico com histerese,
 * 8 direcoes, direito ignorado, primeiro controle + failover, hot-plug ---- */
static SDL_Joystick *vjs[2];
static int vidx[2];

static int vpad_attach(int slot) {
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    if (idx < 0) return -1;
    SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(idx);
    char gs[64], map[600];
    SDL_JoystickGetGUIDString(g, gs, sizeof gs);
    snprintf(map, sizeof map,
             "%s,Virtual Pad %d,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
             "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
             "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5", gs, slot);
    SDL_GameControllerAddMapping(map);
    vjs[slot] = SDL_JoystickOpen(idx);
    vidx[slot] = idx;
    return vjs[slot] ? 0 : -1;
}

static void vpad_detach(int slot) {
    /* o indice do dispositivo muda quando outro some: resolve pelo instance id */
    int idx = -1;
    if (vjs[slot]) {
        SDL_JoystickID id = SDL_JoystickInstanceID(vjs[slot]);
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) idx = i;
        SDL_JoystickClose(vjs[slot]);
        vjs[slot] = NULL;
    }
    if (idx >= 0) SDL_JoystickDetachVirtual(idx);
}

static void settle(void) {   /* remocao/insercao de dispositivo e assincrona: deixa o SDL processar */
    for (int i = 0; i < 4; i++) {
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        SDL_Delay(5);
    }
}

int rt_selftest_input(void) {
    int bad = 0;
#define EXPECT(cond, what) do { if (!(cond)) { bad++; fprintf(stderr, "[selftest-input] FAIL: %s\n", what); } } while (0)
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[selftest-input] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    rt_cfg_defaults();
    rt_cfg.deadzone = 8000;
    only_virtual = 1;
    input_ready = 0;
    rt_input_init();
    #define BIT(b) (1u << (b))
    EXPECT(rt_input_poll() == 0x03FF, "idle without controller");

    if (vpad_attach(0) < 0) { fprintf(stderr, "[selftest-input] virtual joystick unavailable\n"); return 1; }
    settle();
    scan_countdown = 0;
    EXPECT(rt_input_poll() == 0x03FF, "idle with controller");
    EXPECT(active != NULL, "controller 1 became active (hot-plug)");

    /* botoes por posicao: SDL b (leste, indice 1) = GBA A; SDL a (sul, indice 0) = GBA B */
    SDL_JoystickSetVirtualButton(vjs[0], 1, 1); settle();
    EXPECT(!(rt_input_poll() & BIT(B_A)), "east button -> GBA A");
    SDL_JoystickSetVirtualButton(vjs[0], 1, 0);
    SDL_JoystickSetVirtualButton(vjs[0], 0, 1); settle();
    EXPECT(!(rt_input_poll() & BIT(B_B)), "south button -> GBA B");
    SDL_JoystickSetVirtualButton(vjs[0], 0, 0);
    SDL_JoystickSetVirtualButton(vjs[0], 9, 1); SDL_JoystickSetVirtualButton(vjs[0], 6, 1); settle();
    {
        uint16_t r = rt_input_poll();
        EXPECT(!(r & BIT(B_L)) && !(r & BIT(B_START)), "shoulder L + start");
    }
    SDL_JoystickSetVirtualButton(vjs[0], 9, 0); SDL_JoystickSetVirtualButton(vjs[0], 6, 0);
    SDL_JoystickSetVirtualButton(vjs[0], 11, 1); settle();
    EXPECT(!(rt_input_poll() & BIT(B_UP)), "physical d-pad up");
    SDL_JoystickSetVirtualButton(vjs[0], 11, 0); settle();
    EXPECT(rt_input_poll() == 0x03FF, "released");

    /* analogico esquerdo: zona morta + histerese (entra em 8000, sai abaixo de 6800) */
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 7900); settle();
    EXPECT(rt_input_poll() & BIT(B_RIGHT), "below dead zone: nothing");
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 8100); settle();
    EXPECT(!(rt_input_poll() & BIT(B_RIGHT)), "above dead zone: right");
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 7000); settle();
    EXPECT(!(rt_input_poll() & BIT(B_RIGHT)), "hysteresis: still right at 7000 (> 85%)");
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 6700); settle();
    EXPECT(rt_input_poll() & BIT(B_RIGHT), "hysteresis: released below 85%");
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 7500); settle();
    EXPECT(rt_input_poll() & BIT(B_RIGHT), "re-entering needs 100% again");
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 0);
    /* 8 direcoes: diagonais por combinacao */
    SDL_JoystickSetVirtualAxis(vjs[0], 0, -20000); SDL_JoystickSetVirtualAxis(vjs[0], 1, -20000); settle();
    {
        uint16_t r = rt_input_poll();
        EXPECT(!(r & BIT(B_LEFT)) && !(r & BIT(B_UP)) && (r & BIT(B_RIGHT)) && (r & BIT(B_DOWN)), "diagonal up-left");
    }
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 20000); SDL_JoystickSetVirtualAxis(vjs[0], 1, 20000); settle();
    {
        uint16_t r = rt_input_poll();
        EXPECT(!(r & BIT(B_RIGHT)) && !(r & BIT(B_DOWN)) && (r & BIT(B_LEFT)) && (r & BIT(B_UP)), "diagonal down-right");
    }
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 0); SDL_JoystickSetVirtualAxis(vjs[0], 1, 0);
    /* analogico direito nunca vale */
    SDL_JoystickSetVirtualAxis(vjs[0], 2, 30000); SDL_JoystickSetVirtualAxis(vjs[0], 3, -30000); settle();
    EXPECT(rt_input_poll() == 0x03FF, "right stick ignored");
    SDL_JoystickSetVirtualAxis(vjs[0], 2, 0); SDL_JoystickSetVirtualAxis(vjs[0], 3, 0);
    /* stick_as_dpad=0 */
    rt_cfg.stick_as_dpad = 0;
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 30000); settle();
    EXPECT(rt_input_poll() == 0x03FF, "stick_as_dpad=0: stick ignored");
    rt_cfg.stick_as_dpad = 1;
    SDL_JoystickSetVirtualAxis(vjs[0], 0, 0);

    /* segundo controle: ignorado enquanto o primeiro existe */
    if (vpad_attach(1) == 0) {
        settle();
        scan_countdown = 0;
        SDL_JoystickSetVirtualButton(vjs[1], 1, 1); settle();
        EXPECT(rt_input_poll() & BIT(B_A), "2nd controller ignored while the 1st is active");
        /* desconecta o primeiro: o segundo assume (failover) */
        SDL_JoystickSetVirtualButton(vjs[0], 1, 0);
        vpad_detach(0); settle();
        scan_countdown = 0;
        uint16_t r = rt_input_poll();
        EXPECT(!(r & BIT(B_A)), "failover: 2nd controller takes over (A held)");
        SDL_JoystickSetVirtualButton(vjs[1], 1, 0);
        vpad_detach(1); settle();
        scan_countdown = 0;
        {
            uint16_t r2 = rt_input_poll();
            if (r2 != 0x03FF || active != NULL)
                fprintf(stderr, "[selftest-input] debug: poll=%04X active=%p attached=%d njoy=%d\n", r2,
                        (void *)active, active ? SDL_GameControllerGetAttached(active) : -1, SDL_NumJoysticks());
            EXPECT(r2 == 0x03FF && active == NULL, "all controllers gone: idle, no crash");
        }
    } else {
        bad++;
        fprintf(stderr, "[selftest-input] FAIL: could not attach the 2nd virtual controller\n");
    }
    /* reconectar durante o jogo (hot-plug) */
    if (vpad_attach(0) == 0) {
        settle();
        scan_countdown = 0;
        rt_input_poll();
        EXPECT(active != NULL, "hot-plug reconnect");
        vpad_detach(0);
        settle();
    }
    rt_input_close();
    fprintf(stderr, "[selftest-input] %s\n", bad ? "FAILED" : "OK");
    return bad;
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
