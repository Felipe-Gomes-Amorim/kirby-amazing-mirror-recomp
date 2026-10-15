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

/* ---- autoteste do interpretador ----
 * (1) corpos SRAM copiados p/ 3 enderecos da pilha, alias desligado: o fallback deve dar o
 *     mesmo resultado que a funcao nativa da ROM;
 * (2) diferencial: N funcoes recompiladas da ROM x M estados aleatorios, compilado x interpretado
 *     (r0-r14, NZCV, pc, modo, EWRAM+IWRAM). */
extern int rt_no_alias, rt_quiet;
#include <setjmp.h>
extern jmp_buf *rt_abort_jmp;
extern int64_t rt_test_budget;
extern uint64_t rt_interp_steps, rt_interp_entries;
int rt_interp_run(Cpu *c, uint32_t skip_lo, uint32_t skip_hi, uint64_t max_steps);

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 16);
}
static uint32_t fnv(const uint8_t *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

static int selftest_fallback_sram(Cpu *c) {
    int fails = 0;
    static const uint32_t sps[] = {0x03007CECu, 0x03007B80u, 0x02002340u};
    rt_no_alias = 1;
    for (unsigned k = 0; k < 3; k++) {
        uint32_t at = sps[k];
        for (uint32_t i = 0; i < 0x30; i++) gba_write8(at + i, gba_read8(0x08159220u + i));
        for (uint32_t i = 0; i < 8; i++) {
            gba_write8(0x03007F00u + i, 'A' + i);
            gba_write8(0x0E000018u + i, 'A' + i);
        }
        for (int diff = 0; diff < 2; diff++) {
            if (diff) gba_write8(0x0E00001Bu, 'x');
            uint32_t res[2][3];
            for (int native = 0; native < 2; native++) { /* 0 = fallback (RAM), 1 = ROM */
                rt_cpu_reset(c);
                c->thumb = 1;
                c->pc = native ? 0x08159220u : at;
                c->r[0] = 0x03007F00u; c->r[1] = 0x0E000018u; c->r[2] = 8;
                c->r[13] = 0x03007E00u; c->r[14] = RT_IRQ_RET | 1;
                rt_run_until(c, RT_IRQ_RET);
                res[native][0] = c->r[0]; res[native][1] = c->r[13]; res[native][2] = c->r[4];
            }
            if (memcmp(res[0], res[1], sizeof res[0])) {
                fails++;
                fprintf(stderr, "[selftest-interp] Verify sp=%08X diff=%d: fallback r0=%08X nativo r0=%08X\n",
                        at, diff, res[0][0], res[1][0]);
            }
        }
    }
    rt_no_alias = 0;
    return fails;
}

static int selftest_differential(Cpu *c, unsigned nfuncs, unsigned nstates) {
    static uint8_t ew0[0x40000], iw0[0x8000], ew1[0x40000], iw1[0x8000];
    int fails = 0, ran = 0, limited = 0;
    uint64_t steps0 = rt_interp_steps;
    for (uint32_t i = 0; i < 0x40000; i += 4) ew0[i] = (uint8_t)rnd(), ew0[i + 1] = (uint8_t)rnd(),
        ew0[i + 2] = (uint8_t)rnd(), ew0[i + 3] = (uint8_t)rnd();
    for (uint32_t i = 0; i < 0x8000; i++) iw0[i] = (uint8_t)rnd();
    unsigned stride = rc_table_n / nfuncs ? rc_table_n / nfuncs : 1;
    for (unsigned fi = 0; fi < rc_table_n && ran < (int)(nfuncs * nstates); fi += stride) {
        const RcFunc *f = &rc_table[fi];
        if (f->lo < 0x08000000u || f->hi - f->lo > 0x100) continue;
        for (unsigned st = 0; st < nstates; st++) {
            uint32_t r0[13], fl[4];
            for (int i = 0; i < 13; i++) {
                uint32_t k = rnd() % 10;
                r0[i] = k < 3 ? 0x03000000u + (rnd() % 0x7000u & ~3u) : k < 5 ? 0x02000000u + (rnd() % 0x3F000u & ~3u)
                        : k < 6 ? 0x08000000u + (rnd() % 0x100000u) : k < 8 ? rnd() % 40 : rnd();
            }
            for (int i = 0; i < 4; i++) fl[i] = rnd() & 1;
            struct { uint32_t r[16], n, z, cc, v, thumb, pc; uint8_t *mem; } out[2];
            for (int mode = 0; mode < 2; mode++) {      /* 0 = compilado, 1 = interpretado */
                memcpy(gba_ewram(), ew0, sizeof ew0);
                memcpy(gba_iwram(), iw0, sizeof iw0);
                rt_cpu_reset(c);
                for (int i = 0; i < 13; i++) c->r[i] = r0[i];
                c->r[13] = 0x03007E00u;
                c->r[14] = RT_IRQ_RET | f->thumb;
                c->n = fl[0]; c->z = fl[1]; c->c = fl[2]; c->v = fl[3];
                c->mode = 0x1F;
                c->thumb = f->thumb;
                c->pc = f->lo;
                rt_depth = 0;
                rt_rets[rt_depth++] = RT_IRQ_RET;
                rt_test_budget = 3000;
                jmp_buf jb;
                rt_abort_jmp = &jb;
                rt_quiet = 1;
                if (setjmp(jb) == 0) {
                    if (mode == 0) {
                        rt_call(c);
                        if (c->pc != RT_IRQ_RET && !c->stop) rt_run_until(c, RT_IRQ_RET);
                    } else {
                        rt_interp_run(c, f->lo, f->hi, 5000000);
                    }
                } else {
                    c->stop = RT_STOP_ERROR;   /* abort (salto para alvo sem funcao etc.) */
                }
                rt_abort_jmp = 0;
                rt_quiet = 0;
                rt_depth = 0;
                memcpy(out[mode].r, c->r, sizeof out[mode].r);
                out[mode].n = c->n; out[mode].z = c->z; out[mode].cc = c->c; out[mode].v = c->v;
                out[mode].thumb = c->thumb; out[mode].pc = c->pc;
                out[mode].r[15] = c->stop ? 0xDEAD : 0;   /* limite de laco */
                static uint8_t keep[2][0x48000];
                memcpy(keep[mode], gba_ewram(), 0x40000);
                memcpy(keep[mode] + 0x40000, gba_iwram(), 0x8000);
                out[mode].mem = keep[mode];
                c->stop = 0;
            }
            ran++;
            if (out[0].r[15] || out[1].r[15]) { limited++; continue; }
            int bad = out[0].n != out[1].n || out[0].z != out[1].z || out[0].cc != out[1].cc ||
                      out[0].v != out[1].v || out[0].pc != out[1].pc || out[0].thumb != out[1].thumb ||
                      memcmp(out[0].r, out[1].r, 15 * 4) || fnv(out[0].mem, 0x48000) != fnv(out[1].mem, 0x48000);
            if (bad) {
                fails++;
                if (fails <= 8) {
                    fprintf(stderr, "[selftest-interp] DIVERGE em %08X (%s) estado %u: pc %08X/%08X NZCV %u%u%u%u/%u%u%u%u\n",
                            f->lo, f->thumb ? "thumb" : "arm", st, out[0].pc, out[1].pc, out[0].n, out[0].z,
                            out[0].cc, out[0].v, out[1].n, out[1].z, out[1].cc, out[1].v);
                    for (int i = 0; i < 15; i++)
                        if (out[0].r[i] != out[1].r[i])
                            fprintf(stderr, "    r%d compilado=%08X interpretado=%08X\n", i, out[0].r[i], out[1].r[i]);
                    if (fnv(out[0].mem, 0x48000) != fnv(out[1].mem, 0x48000)) fprintf(stderr, "    memoria difere\n");
                }
            }
        }
    }
    rt_test_budget = 0;
    fprintf(stderr, "[selftest-interp] diferencial: %d execucoes comparadas (%d com limite de laco, ignoradas), "
            "%llu instrucoes interpretadas, %d divergencias\n", ran - limited, limited,
            (unsigned long long)(rt_interp_steps - steps0), fails);
    return fails;
}

int rt_selftest_interp(Cpu *c) {
    int f = selftest_fallback_sram(c);
    fprintf(stderr, "[selftest-interp] corpos SRAM em 3 enderecos da pilha: %s\n", f ? "FALHOU" : "OK");
    f += selftest_differential(c, 150, 6);
    return f;
}
