/* Kernel benchmark: old C upload loop (verbatim from the pre-Rust Smd_d3d9.c)
 * vs the Rust fearless_simd kernels. 32-bit, -O2, run under Wine.
 * Old code faithfully copied from git HEAD (D7_ToArgb / D7_Lut16 / the row loops). */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "raindrop_gpu.h"

/* ---------------- old implementation (verbatim semantics) ---------------- */

typedef struct { DWORD m[4]; int sh[4], nb[4]; } D7Fmt;
static unsigned char g_d7Exp[9][256];
static int g_d7ExpInit = 0;

static void D7_ExpInit(void) {
    int nb, v;
    if (g_d7ExpInit) return;
    for (nb = 1; nb <= 8; nb++)
        for (v = 0; v < (1 << nb); v++)
            g_d7Exp[nb][v] = (unsigned char)((v * 255) / ((1 << nb) - 1));
    g_d7ExpInit = 1;
}

static void D7_FmtInit(D7Fmt *f, const DWORD m[4]) {
    int i;
    D7_ExpInit();
    memset(f, 0, sizeof(*f));
    for (i = 0; i < 4; i++) {
        if (m[i]) {
            DWORD t = m[i]; int sh = 0, nb = 0;
            while (!(t & 1)) { t >>= 1; sh++; }
            t = m[i];
            while (t) { nb += (int)(t & 1); t >>= 1; }
            f->m[i] = m[i]; f->sh[i] = sh; f->nb[i] = nb;
        }
    }
}

static DWORD D7_ToArgb(const D7Fmt *f, DWORD raw) {
    DWORD c[4];
    int i;
    for (i = 0; i < 4; i++) {
        if (f->nb[i]) {
            DWORD v = (raw & f->m[i]) >> f->sh[i];
            c[i] = (f->nb[i] <= 8) ? (DWORD)g_d7Exp[f->nb[i]][v & 255u] : (v * 255u) / ((1u << f->nb[i]) - 1u);
        } else {
            c[i] = (i == 3) ? 255u : 0u;
        }
    }
    return (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
}

static const DWORD *D7_Lut16(const D7Fmt *f) {
    static struct { DWORD m[4]; DWORD *t; } cache[4];
    static int ncache = 0;
    int i;
    DWORD *t, v;
    for (i = 0; i < ncache; i++)
        if (cache[i].m[0] == f->m[0] && cache[i].m[1] == f->m[1] &&
            cache[i].m[2] == f->m[2] && cache[i].m[3] == f->m[3])
            return cache[i].t;
    if (ncache >= 4) return NULL;
    t = (DWORD*)malloc(65536 * sizeof(DWORD));
    for (v = 0; v < 65536; v++) t[v] = D7_ToArgb(f, v);
    for (i = 0; i < 4; i++) cache[ncache].m[i] = f->m[i];
    cache[ncache].t = t;
    ncache++;
    return t;
}

typedef struct { int bpp, pitch; unsigned char *bits; DWORD keyLo; int hasKey; D7Fmt fmt; } Sur;

/* Old upload loop (the pre-Rust D7_GetTex inner body, minus memcpy fast path). */
static void old_upload(Sur *s, uint32_t *dp, int n, int rows) {
    int x, y, bytes = s->bpp / 8;
    const DWORD *lut = (s->bpp == 16) ? D7_Lut16(&s->fmt) : NULL;
    for (y = 0; y < rows; y++) {
        const unsigned char *sp = s->bits + (size_t)y * (size_t)s->pitch;
        if (s->bpp == 32 && s->fmt.m[0] == 0x00FF0000u && s->fmt.m[1] == 0x0000FF00u &&
            s->fmt.m[2] == 0x000000FFu && (s->fmt.m[3] == 0 || s->fmt.m[3] == 0xFF000000u)) {
            DWORD m3 = s->fmt.m[3], addA = m3 ? 0u : 0xFF000000u, keyv = s->keyLo & ~m3;
            const DWORD *sd = (const DWORD*)sp;
            for (x = 0; x < n; x++) {
                DWORD raw = sd[x];
                DWORD argb = raw | addA;
                if (s->hasKey && (raw & ~m3) == keyv) argb &= 0x00FFFFFFu;
                dp[x] = argb;
            }
        } else {
            for (x = 0; x < n; x++) {
                DWORD raw;
                if (s->bpp == 32) raw = *(const DWORD*)(sp + x * 4);
                else raw = *(const WORD*)(sp + x * 2);
                DWORD argb = lut ? lut[raw & 0xFFFFu] : D7_ToArgb(&s->fmt, raw);
                if (s->hasKey && (raw & ~s->fmt.m[3]) == (s->keyLo & ~s->fmt.m[3]))
                    argb &= 0x00FFFFFFu;
                dp[x] = argb;
            }
        }
        dp += n;
    }
}

/* ---------------- harness ---------------- */

static uint64_t lcg_state = 0x243F6A8885A308D3ull;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(lcg_state >> 17);
}

static double qpf;
static LARGE_INTEGER t0;
static void tick(void) { QueryPerformanceCounter(&t0); }
static double tock(void) { LARGE_INTEGER e; QueryPerformanceCounter(&e); return (e.QuadPart - t0.QuadPart) / qpf; }

#define W 640
#define H 480
#define FRAMES 120
#define BYTES_MB ((double)(W * H * 4) * FRAMES / (1024.0 * 1024.0))

int main(void) {
    LARGE_INTEGER f;
    static uint16_t src16[W * H];
    static uint32_t src32[W * H];
    static uint32_t dstOld[W * H], dstNew[W * H];
    static const struct { const char *name; int bpp; DWORD m[4]; int key; DWORD keyLo; int fmtcode; } cases[] = {
        { "565            ", 16, { 0xF800, 0x07E0, 0x001F, 0 }, 0, 0, RAINDROP_FMT_565 },
        { "565 + key      ", 16, { 0xF800, 0x07E0, 0x001F, 0 }, 1, 0x07E0, RAINDROP_FMT_565 },
        { "5551 + key     ", 16, { 0x7C00, 0x03E0, 0x001F, 0x8000 }, 1, 0x7C01, RAINDROP_FMT_5551 },
        { "4444 + key     ", 16, { 0x0F00, 0x00F0, 0x000F, 0xF000 }, 1, 0xF123, RAINDROP_FMT_4444 },
        { "X8R8G8B8       ", 32, { 0xFF0000, 0xFF00, 0xFF, 0 }, 0, 0, -2 },
        { "A8R8G8B8 + key ", 32, { 0xFF0000, 0xFF00, 0xFF, 0xFF000000 }, 1, 0x37F327, -2 },
    };
    int ci;
    QueryPerformanceFrequency(&f); qpf = (double)f.QuadPart;

    printf("kernel bench 640x480, %d frames (wine, i686, -O2, old C vs Rust)\n", FRAMES);
    for (ci = 0; ci < (int)(sizeof(cases)/sizeof(cases[0])); ci++) {
        const DWORD *m = cases[ci].m;
        int bpp = cases[ci].bpp, key = cases[ci].key;
        DWORD keyLo = cases[ci].keyLo;
        int it;
        double to, tn;
        Sur s;

        for (it = 0; it < W * H; it++)
            src16[it] = (uint16_t)lcg();
        for (it = 0; it < W * H; it++)
            src32[it] = lcg() & 0x00FFFFFFu;

        memset(&s, 0, sizeof(s));
        s.bpp = bpp; s.pitch = W * (bpp / 8);
        s.bits = bpp == 16 ? (unsigned char*)src16 : (unsigned char*)src32;
        s.hasKey = key; s.keyLo = keyLo;
        D7_FmtInit(&s.fmt, m);

        /* correctness (old vs new) */
        old_upload(&s, dstOld, W, H);
        if (bpp == 16) {
            raindrop_upload_conv16(dstNew, W, src16, W, W, H, cases[ci].fmtcode,
                                   key, (uint16_t)(keyLo & ~m[3]), (uint16_t)~(uint32_t)(WORD)m[3]);
        } else {
            DWORD m3 = m[3];
            raindrop_upload_xrgb32(dstNew, W, src32, W, W, H,
                                   m3 ? 0u : 0xFF000000u, key, keyLo & ~m3, ~m3);
        }
        if (memcmp(dstOld, dstNew, (size_t)W * H * 4) != 0) {
            printf("%s  MISMATCH old=%08x new=%08x\n", cases[ci].name, dstOld[0], dstNew[0]);
            continue;
        }

        /* timing old */
        old_upload(&s, dstOld, W, H); /* warm */
        tick();
        for (it = 0; it < FRAMES; it++) old_upload(&s, dstOld, W, H);
        to = tock();

        /* timing new */
        tick();
        for (it = 0; it < FRAMES; it++) {
            if (bpp == 16) {
                raindrop_upload_conv16(dstNew, W, src16, W, W, H, cases[ci].fmtcode,
                                       key, (uint16_t)(keyLo & ~m[3]), (uint16_t)~(uint32_t)(WORD)m[3]);
            } else {
                DWORD m3 = m[3];
                raindrop_upload_xrgb32(dstNew, W, src32, W, W, H,
                                       m3 ? 0u : 0xFF000000u, key, keyLo & ~m3, ~m3);
            }
        }
        tn = tock();
        printf("%s old %7.1f us/frame | new %7.1f us/frame | %.2fx  (%.0f -> %.0f MB/s)\n",
               cases[ci].name, to / FRAMES * 1e6, tn / FRAMES * 1e6, to / tn,
               BYTES_MB / to, BYTES_MB / tn);
    }
    return 0;
}
