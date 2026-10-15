/* Roteiro de teste do runtime: teclas por quadro, snapshots de video e arquivo de save.
 *   --keys "START@620,A@700+10"   tecla@quadro[+duracao em quadros, padrao 6]
 *   --snap 600,2000,3000          salva frames/frame_N.ppm e imprime registradores de video
 *   --frames-dir DIR              (padrao build/recomp/frames)
 *   --sav FILE                    save SRAM cru de 32 KiB (criado com 0xFF se nao existir)
 * Teclas: A B SELECT START RIGHT LEFT UP DOWN R L (KEYINPUT: bit=0 pressionado). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#include "gba_bus.h"
#include "gba_cpu.h"
#include "gba_rt.h"

const uint32_t *hw_get_framebuffer(void);
void rt_script_sram_save(void);

typedef struct { uint64_t at, until; uint16_t mask; } KeyEv;
static KeyEv keys[128];
static int nkeys;
static uint64_t snaps[128];
static int nsnaps;
static char frames_dir[400] = "build/recomp/frames";
static char sav_path[400];

static const char *KEYNAMES[10] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"};

int rt_script_keys(const char *s) {
    char buf[1024];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok && nkeys < 128; tok = strtok(NULL, ",")) {
        char *at = strchr(tok, '@');
        if (!at) return -1;
        *at++ = 0;
        int bit = -1;
        for (int i = 0; i < 10; i++)
            if (!strcmp(tok, KEYNAMES[i])) bit = i;
        if (bit < 0) return -1;
        uint64_t f = strtoull(at, &at, 10), hold = 6;
        if (*at == '+') hold = strtoull(at + 1, NULL, 10);
        keys[nkeys].at = f;
        keys[nkeys].until = f + hold;
        keys[nkeys].mask = (uint16_t)(1u << bit);
        nkeys++;
    }
    return 0;
}

int rt_script_snaps(const char *s) {
    char buf[1024];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok && nsnaps < 128; tok = strtok(NULL, ","))
        snaps[nsnaps++] = strtoull(tok, NULL, 10);
    return 0;
}

void rt_script_dir(const char *d) {
    strncpy(frames_dir, d, sizeof frames_dir - 1);
}

static void snapshot(uint64_t frame) {
    const uint32_t *fb = hw_get_framebuffer();
    unsigned nonblack = 0;
    char path[512];
    if (fb) {
        for (int i = 0; i < 240 * 160; i++)
            if (fb[i] & 0xFFFFFF) nonblack++;
        MKDIR(frames_dir);
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", frames_dir, (unsigned long long)frame);
        FILE *o = fopen(path, "wb");
        if (o) {
            fprintf(o, "P6\n240 160\n255\n");
            for (int i = 0; i < 240 * 160; i++) {
                fputc((fb[i] >> 16) & 255, o);
                fputc((fb[i] >> 8) & 255, o);
                fputc(fb[i] & 255, o);
            }
            fclose(o);
        }
    }
    fprintf(stderr,
            "[snap] frame=%llu DISPCNT=%04X DISPSTAT=%04X BG0..3CNT=%04X %04X %04X %04X "
            "BLDCNT=%04X BLDALPHA=%04X BLDY=%04X MOSAIC=%04X WININ=%04X WINOUT=%04X "
            "pal0=%04X IE=%04X IME=%04X nao-pretos=%u\n",
            (unsigned long long)frame, hw_read(0x04000000, 2), hw_read(0x04000004, 2),
            hw_read(0x04000008, 2), hw_read(0x0400000A, 2), hw_read(0x0400000C, 2),
            hw_read(0x0400000E, 2), hw_read(0x04000050, 2), hw_read(0x04000052, 2),
            hw_read(0x04000054, 2), hw_read(0x0400004C, 2), hw_read(0x04000048, 2),
            hw_read(0x0400004A, 2), hw_read(0x05000000, 2), hw_read(0x04000200, 2),
            hw_read(0x04000208, 2), nonblack);
}

/* chamado por VBlankIntrWait ao fechar cada quadro (frame = 1, 2, ...) */
static void frame_hook(uint64_t frame) {
    if (nkeys) {
        uint16_t k = 0x03FF;
        for (int i = 0; i < nkeys; i++)
            if (frame >= keys[i].at && frame < keys[i].until) k &= (uint16_t)~keys[i].mask;
        hw_set_keys(k);
    }
    for (int i = 0; i < nsnaps; i++)
        if (snaps[i] == frame) snapshot(frame);
}

void rt_script_install(void) {
    if (nkeys || nsnaps) rt_frame_hook = frame_hook;
}

/* ---- save SRAM ---- */
void rt_script_sram(const char *path) {
    strncpy(sav_path, path, sizeof sav_path - 1);
    FILE *f = fopen(sav_path, "rb");
    uint8_t buf[0x8000];
    memset(buf, 0xFF, sizeof buf);
    if (f) {
        size_t n = fread(buf, 1, sizeof buf, f);
        (void)n;
        fclose(f);
    }
    gba_sram_set(buf);
    atexit(rt_script_sram_save);
}

void rt_script_sram_save(void) {
    if (!sav_path[0]) return;
    uint8_t buf[0x8000];
    gba_sram_get(buf);
    FILE *f = fopen(sav_path, "wb");
    if (f) {
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
    }
}

/* ---- autoteste: rotinas SRAM copiadas para a pilha em sp arbitrario ---- */
extern uint64_t rt_sram_alias_hits;

int rt_selftest_sram(Cpu *c) {
    int fails = 0;
    static const uint32_t sps[] = {0x03007CECu, 0x03007DA0u, 0x03007B10u, 0x02001230u};
    for (unsigned k = 0; k < sizeof sps / sizeof sps[0]; k++) {
        uint32_t at = sps[k];
        /* VerifySram: corpo de 0x08159220 (48 bytes) copiado para `at` */
        for (uint32_t i = 0; i < 0x30; i++) gba_write8(at + i, gba_read8(0x08159220u + i));
        for (uint32_t i = 0; i < 8; i++) {
            gba_write8(0x03007F00u + i, 'A' + i);
            gba_write8(0x0E000018u + i, 'A' + i);
        }
        for (int diff = 0; diff < 2; diff++) {
            if (diff) gba_write8(0x0E00001Bu, 'x');
            rt_cpu_reset(c);
            c->thumb = 1;
            c->pc = at;
            c->r[0] = 0x03007F00u;
            c->r[1] = 0x0E000018u;
            c->r[2] = 8;
            c->r[13] = 0x03007E00u;
            c->r[14] = RT_IRQ_RET | 1;
            uint32_t sp0 = c->r[13];
            rt_run_until(c, RT_IRQ_RET);
            uint32_t want = diff ? 0x0E00001Bu : 0;
            if (c->r[0] != want || c->r[13] != sp0) {
                fprintf(stderr, "[selftest] Verify sp=%08X diff=%d: r0=%08X (esperado %08X) sp=%08X\n",
                        at, diff, c->r[0], want, c->r[13]);
                fails++;
            }
        }
        /* ReadSram: corpo de 0x08159158 (34 bytes) */
        for (uint32_t i = 0; i < 0x22; i++) gba_write8(at + i, gba_read8(0x08159158u + i));
        for (uint32_t i = 0; i < 8; i++) gba_write8(0x0E000040u + i, 0x30 + i);
        rt_cpu_reset(c);
        c->thumb = 1;
        c->pc = at;
        c->r[0] = 0x0E000040u;
        c->r[1] = 0x03007F40u;
        c->r[2] = 8;
        c->r[13] = 0x03007E00u;
        c->r[14] = RT_IRQ_RET | 1;
        rt_run_until(c, RT_IRQ_RET);
        for (uint32_t i = 0; i < 8; i++)
            if (gba_read8(0x03007F40u + i) != 0x30 + i) { fails++; fprintf(stderr, "[selftest] Read sp=%08X byte %u\n", at, i); break; }
    }
    fprintf(stderr, "[selftest] SRAM na pilha: %s (%d falhas, %llu usos do alias)\n", fails ? "FALHOU" : "OK",
            fails, (unsigned long long)rt_sram_alias_hits);
    return fails;
}
