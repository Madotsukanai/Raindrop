#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log = NULL;
static const int g_LogAll = 0;
static const int g_StripAllowModeX = 1;
static const int g_RetryCoopLevel = 1;
static const int g_RetryDisplayMode = 1;
#ifndef SMD_LOWLATENCY
#define SMD_LOWLATENCY 1
#endif
#ifndef SMD_DWMFLUSH
#define SMD_DWMFLUSH 1
#endif
static const int g_LowLatency = SMD_LOWLATENCY;
static const int g_UseDwmFlush = SMD_DWMFLUSH;
static const int g_StatsIntervalMs = 5000;
static const int g_WaitTimeoutMs = 50;

static void ExeDirFile(const char *name, char *out) {
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(out, name);
}

static void LogOpen(void) {
    if (g_log) return;
    char path[MAX_PATH];
    ExeDirFile("Smd_ddraw.log", path);
    g_log = fopen(path, "a");
}

static void Log(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

typedef HRESULT (STDMETHODCALLTYPE *PF_Blt)(IDirectDrawSurface7*, LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (STDMETHODCALLTYPE *PF_Flip)(IDirectDrawSurface7*, LPDIRECTDRAWSURFACE7, DWORD);
typedef HRESULT (WINAPI *PF_DwmFlush)(void);

static IDirectDrawSurface7 *g_primary = NULL;
static PF_Blt g_origBlt = NULL;
static PF_Flip g_origFlip = NULL;
static PF_DwmFlush g_pDwmFlush = NULL;
static LARGE_INTEGER g_qpcFreq;

typedef struct { LONGLONG n; double sum, max; } Stat;
typedef struct { LONGLONG t; int counted; } Press;

#define MAX_PENDING 16

static CRITICAL_SECTION g_cs;
static int g_measureReady = 0;
static Press g_pend[MAX_PENDING];
static int g_pendN = 0;
static Stat g_stInterval, g_stGame, g_stOrig, g_stWait, g_stDwm, g_stLatFirst, g_stLatGuar;
static LONGLONG g_frames = 0, g_timeouts = 0, g_presses = 0;
static LONGLONG g_prevDone = 0, g_winStart = 0;
static int g_lastFlip = 0;

static void StatAdd(Stat *s, double v) {
    s->n++;
    s->sum += v;
    if (v > s->max) s->max = v;
}

static double StatAvg(const Stat *s) {
    return s->n ? s->sum / (double)s->n : 0.0;
}

static double TicksToMs(LONGLONG t) {
    return (double)t * 1000.0 / (double)g_qpcFreq.QuadPart;
}

static void AddPress(LONGLONG t) {
    EnterCriticalSection(&g_cs);
    g_presses++;
    if (g_pendN < MAX_PENDING) {
        g_pend[g_pendN].t = t;
        g_pend[g_pendN].counted = 0;
        g_pendN++;
    }
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI KeyThread(LPVOID unused) {
    typedef UINT (WINAPI *PF_TBP)(UINT);
    unsigned char prev[256];
    memset(prev, 0, sizeof(prev));
    HMODULE mm = LoadLibraryA("winmm.dll");
    if (mm) {
        PF_TBP tbp = (PF_TBP)GetProcAddress(mm, "timeBeginPeriod");
        if (tbp) tbp(1);
    }
    for (;;) {
        Sleep(1);
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &pid);
        int active = (pid == GetCurrentProcessId());
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        for (int vk = 8; vk < 255; vk++) {
            if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
            int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (active && down && !prev[vk]) AddPress(t.QuadPart);
            prev[vk] = (unsigned char)down;
        }
    }
    return 0;
}

static void FlushStats(LONGLONG now) {
    double sec = TicksToMs(now - g_winStart) / 1000.0;
    Log("stats %.1fs mode=%s lowlatency=%d dwmflush=%d frames=%lld fps=%.1f timeouts=%lld",
        sec, g_lastFlip ? "flip" : "blt", g_LowLatency, (g_LowLatency && g_UseDwmFlush && g_pDwmFlush) ? 1 : 0,
        (long long)g_frames, sec > 0 ? (double)g_frames / sec : 0.0, (long long)g_timeouts);
    Log("  frame interval avg=%.2f max=%.2f ms | game cpu avg=%.2f max=%.2f | present call avg=%.2f max=%.2f | status wait avg=%.2f max=%.2f | dwmflush avg=%.2f max=%.2f",
        StatAvg(&g_stInterval), g_stInterval.max, StatAvg(&g_stGame), g_stGame.max,
        StatAvg(&g_stOrig), g_stOrig.max, StatAvg(&g_stWait), g_stWait.max,
        StatAvg(&g_stDwm), g_stDwm.max);
    Log("  key presses=%lld | press->next present done avg=%.2f max=%.2f ms (n=%lld) | press->frame started after press done avg=%.2f max=%.2f ms (n=%lld)",
        (long long)g_presses, StatAvg(&g_stLatFirst), g_stLatFirst.max, (long long)g_stLatFirst.n,
        StatAvg(&g_stLatGuar), g_stLatGuar.max, (long long)g_stLatGuar.n);
    memset(&g_stInterval, 0, sizeof(Stat));
    memset(&g_stGame, 0, sizeof(Stat));
    memset(&g_stOrig, 0, sizeof(Stat));
    memset(&g_stWait, 0, sizeof(Stat));
    memset(&g_stDwm, 0, sizeof(Stat));
    memset(&g_stLatFirst, 0, sizeof(Stat));
    memset(&g_stLatGuar, 0, sizeof(Stat));
    g_frames = 0;
    g_timeouts = 0;
    g_presses = 0;
}

static void OnPresent(LONGLONG a, LONGLONG b, LONGLONG c, LONGLONG d, int flip, int timedOut) {
    EnterCriticalSection(&g_cs);
    LONGLONG frameStart = g_prevDone;
    if (frameStart) {
        StatAdd(&g_stInterval, TicksToMs(d - frameStart));
        StatAdd(&g_stGame, TicksToMs(a - frameStart));
    }
    StatAdd(&g_stOrig, TicksToMs(b - a));
    StatAdd(&g_stWait, TicksToMs(c - b));
    StatAdd(&g_stDwm, TicksToMs(d - c));
    g_frames++;
    if (timedOut) g_timeouts++;
    g_lastFlip = flip;
    for (int i = 0; i < g_pendN; i++) {
        Press *p = &g_pend[i];
        if (!p->counted && d > p->t) {
            StatAdd(&g_stLatFirst, TicksToMs(d - p->t));
            p->counted = 1;
        }
        int done = (TicksToMs(d - p->t) > 1000.0);
        if (frameStart && frameStart >= p->t) {
            StatAdd(&g_stLatGuar, TicksToMs(d - p->t));
            done = 1;
        }
        if (done) {
            g_pend[i] = g_pend[g_pendN - 1];
            g_pendN--;
            i--;
        }
    }
    if (!g_winStart) {
        g_winStart = d;
    } else if (TicksToMs(d - g_winStart) >= (double)g_StatsIntervalMs) {
        FlushStats(d);
        g_winStart = d;
    }
    g_prevDone = d;
    LeaveCriticalSection(&g_cs);
}

static void FinishPresent(IDirectDrawSurface7 *s, int flip, LARGE_INTEGER a, LARGE_INTEGER b) {
    LARGE_INTEGER c, d, t;
    int timedOut = 0;
    if (g_LowLatency) {
        LONGLONG limit = g_qpcFreq.QuadPart * g_WaitTimeoutMs / 1000;
        for (;;) {
            HRESULT hr = flip ? IDirectDrawSurface7_GetFlipStatus(s, DDGFS_ISFLIPDONE)
                              : IDirectDrawSurface7_GetBltStatus(s, DDGBS_ISBLTDONE);
            if (hr != DDERR_WASSTILLDRAWING) break;
            QueryPerformanceCounter(&t);
            if (t.QuadPart - b.QuadPart > limit) {
                timedOut = 1;
                break;
            }
        }
    }
    QueryPerformanceCounter(&c);
    if (g_LowLatency && !flip && g_UseDwmFlush && g_pDwmFlush) g_pDwmFlush();
    QueryPerformanceCounter(&d);
    OnPresent(a.QuadPart, b.QuadPart, c.QuadPart, d.QuadPart, flip, timedOut);
}

static void MeasureInit(void) {
    InitializeCriticalSection(&g_cs);
    if (g_LowLatency && g_UseDwmFlush) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm) g_pDwmFlush = (PF_DwmFlush)GetProcAddress(dwm, "DwmFlush");
    }
    HANDLE th = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    g_measureReady = 1;
    Log("Measure: lowlatency=%d dwmflush=%d dwmapi=%s keythread=%s interval=%dms",
        g_LowLatency, g_UseDwmFlush, g_pDwmFlush ? "ok" : "none", th ? "ok" : "failed", g_StatsIntervalMs);
}

static HRESULT STDMETHODCALLTYPE Hook_Blt(IDirectDrawSurface7 *self, LPRECT dr, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD flags, LPDDBLTFX fx) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origBlt(self, dr, src, sr, flags, fx);
    QueryPerformanceCounter(&b);
    if (hr >= 0 && self == g_primary && src && g_measureReady) FinishPresent(self, 0, a, b);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Flip(IDirectDrawSurface7 *self, LPDIRECTDRAWSURFACE7 target, DWORD flags) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origFlip(self, target, flags);
    QueryPerformanceCounter(&b);
    if (hr >= 0 && self == g_primary && g_measureReady) FinishPresent(self, 1, a, b);
    return hr;
}

static void HookPrimary(IDirectDrawSurface7 *s) {
    g_primary = s;
    if (g_origBlt) return;
    MeasureInit();
    IDirectDrawSurface7Vtbl *vt = s->lpVtbl;
    DWORD old;
    if (!VirtualProtect(vt, sizeof(*vt), PAGE_READWRITE, &old)) {
        Log("LowLatency: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    g_origBlt = (PF_Blt)vt->Blt;
    g_origFlip = (PF_Flip)vt->Flip;
    vt->Blt = (void*)Hook_Blt;
    vt->Flip = (void*)Hook_Flip;
    VirtualProtect(vt, sizeof(*vt), old, &old);
    Log("LowLatency: primary surface %p hooked (Blt/Flip)", (void*)s);
}

/*
 * Title screen: Cancel -> move cursor to Exit, Cancel again -> same as deciding Exit.
 *
 * Rain.exe title update function (0x422fa0, thiscall, ecx = title object):
 *   +0x2324c : selected menu index (0..6, 6 = Exit)
 *   +0x08    : state (8 = exit sequence)
 *   input object = *(void**)0x46bbec
 *   0x431540(input, id): button pressed (edge), 0x431500(input, vk): key pressed (edge)
 * Original code treats button 5 (cancel) as "ignore" and only button 7 moves the
 * cursor to Exit.  The hook below makes 5 / 7 / Esc all act as cancel, and when the
 * cursor is already on Exit it goes through the normal decide path (0x423447:
 * decide sound -> jump table -> state 8).
 */
static const int g_TitleCancelExit = 1;

typedef unsigned char (__attribute__((thiscall)) *PF_InputKey)(void *self, int id);

static int __cdecl TitleCancelHook(void *input) {
    PF_InputKey button = (PF_InputKey)(uintptr_t)0x431540;
    PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
    if (!input) return 0;
    if (button(input, 5)) return 1;
    if (button(input, 7)) return 1;
    if (rawkey(input, 0x1b)) return 1;
    return 0;
}

static void PatchTitleCancel(void) {
    static const unsigned char orig_a[42] = {
        0x8b,0x0d,0xec,0xbb,0x46,0x00, 0x6a,0x05, 0xe8,0x3d,0xe1,0x00,0x00, 0x84,0xc0,
        0x0f,0x85,0x4f,0x01,0x00,0x00,
        0x8b,0x0d,0xec,0xbb,0x46,0x00, 0x6a,0x07, 0xe8,0x28,0xe1,0x00,0x00, 0x84,0xc0,
        0x0f,0x84,0x3a,0x01,0x00,0x00
    };
    static const unsigned char orig_b[12] = {
        0x39,0xbe,0x4c,0x32,0x02,0x00, 0x0f,0x84,0x27,0x01,0x00,0x00
    };
    const DWORD addrA = 0x4233f6, addrB = 0x423420;
    unsigned char *pa = (unsigned char*)addrA;
    unsigned char *pb = (unsigned char*)addrB;
    DWORD old;
    unsigned char patch[42];
    DWORD rel;

    if (!g_TitleCancelExit) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("TitleCancel: unexpected image base, skipped");
        return;
    }
    if (memcmp(pa, orig_a, sizeof(orig_a)) != 0 || memcmp(pb, orig_b, sizeof(orig_b)) != 0) {
        Log("TitleCancel: code signature mismatch, skipped");
        return;
    }

    memset(patch, 0x90, sizeof(patch));
    /* mov ecx,[0x46bbec] */
    patch[0] = 0x8b; patch[1] = 0x0d; patch[2] = 0xec; patch[3] = 0xbb; patch[4] = 0x46; patch[5] = 0x00;
    /* push ecx */
    patch[6] = 0x51;
    /* call TitleCancelHook (at 0x4233fd) */
    patch[7] = 0xe8;
    rel = (DWORD)(uintptr_t)&TitleCancelHook - (addrA + 7 + 5);
    memcpy(&patch[8], &rel, 4);
    /* add esp,4 */
    patch[12] = 0x83; patch[13] = 0xc4; patch[14] = 0x04;
    /* test al,al */
    patch[15] = 0x84; patch[16] = 0xc0;
    /* je 0x42355a (at 0x423407, next ip 0x42340d) */
    patch[17] = 0x0f; patch[18] = 0x84;
    rel = 0x42355a - (addrA + 17 + 6);
    memcpy(&patch[19], &rel, 4);
    /* patch[23..41] stay NOP (up to 0x423420) */

    if (!VirtualProtect(pa, 0x40, PAGE_EXECUTE_READWRITE, &old)) {
        Log("TitleCancel: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    memcpy(pa, patch, sizeof(patch));
    /* cursor already on Exit: je 0x423553 (state 8 directly) -> je 0x423447 (decide path) */
    rel = 0x423447 - 0x42342c;
    memcpy((unsigned char*)0x423428, &rel, 4);
    VirtualProtect(pa, 0x40, old, &old);
    FlushInstructionCache(GetCurrentProcess(), pa, 0x40);
    Log("TitleCancel: patched (hook=%p)", (void*)&TitleCancelHook);
}

/*
 * Option screen: D key restores default settings (defaults = config.txt values).
 *
 * Option screen object = title object + 0x5730 (updated by 0x41b270 from title state 3).
 *   +0x3308 : page (0 top menu, 1 game options, 2 key config, 3 leaving)
 *   game options page: +0x3314 Rest (initial lives; the game's g_ScreenMode/0x46c7c4, used as
 *                      the start lives by the retry function 0x419d10), +0x3318 FPS-1,
 *                      +0x331c hitrect-off, +0x3320 BGM-off, +0x3324 BGM vol,
 *                      +0x3328 SE-off,  +0x332c SE vol
 *   key config page  : rows are set with 0x41bf60(this, row, pad, type, key), exactly like the
 *                      game's own "default" row.  0x4e9240 != 0 while waiting for a key.
 * The hook replaces the `call 0x41b270` at 0x423650 and calls the original afterwards.
 */
static const int g_OptionDefaultKey = 1;

typedef unsigned char (__attribute__((fastcall)) *PF_OptionUpdate)(void *opt);
typedef void (__attribute__((thiscall)) *PF_SetKeyRow)(void *opt, int row, int pad, int type, int key);
typedef void (__attribute__((thiscall)) *PF_PlaySe)(void *se, int a, int b);
typedef void (__cdecl *PF_SetVolume)(int v);

#define OPT_F(o, off) (*(int*)((char*)(o) + (off)))

static int OptionVolume(int off, int vol) {
    double v = off ? *(double*)(uintptr_t)0x453cc0 : (double)vol * *(double*)(uintptr_t)0x453cb8;
    return (int)v;
}

static unsigned char __attribute__((fastcall)) OptionUpdateHook(void *opt) {
    void *input = *(void**)(uintptr_t)0x46bbec;
    int page = OPT_F(opt, 0x3308);
    int capturing = *(volatile unsigned char*)(uintptr_t)0x4e9240;

    if (input && ((page == 1) || (page == 2 && !capturing))) {
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        if (rawkey(input, 0x44)) {
            PF_PlaySe playSe = (PF_PlaySe)(uintptr_t)0x412210;
            if (page == 1) {
                OPT_F(opt, 0x3314) = 4;    /* Rest=4 */
                OPT_F(opt, 0x3318) = 3;    /* FPS=4 (60fps) */
                OPT_F(opt, 0x331c) = 1;    /* ShowHitRect=off */
                OPT_F(opt, 0x3320) = 0;    /* EnableBGM=on */
                OPT_F(opt, 0x3324) = 100;  /* BGMVolume=100 */
                OPT_F(opt, 0x3328) = 0;    /* EnableSE=on */
                OPT_F(opt, 0x332c) = 80;   /* SEVolume=80 */
                ((PF_SetVolume)(uintptr_t)0x4117c0)(OptionVolume(OPT_F(opt, 0x3320), OPT_F(opt, 0x3324)));
                ((PF_SetVolume)(uintptr_t)0x4120e0)(OptionVolume(OPT_F(opt, 0x3328), OPT_F(opt, 0x332c)));
            } else {
                PF_SetKeyRow setRow = (PF_SetKeyRow)(uintptr_t)0x41bf60;
                int i;
                for (i = 0; i < 4; i++) setRow(opt, i, 0, i + 1, 0);      /* Up/Left/Down/Right */
                for (i = 4; i < 8; i++) setRow(opt, i, 0, 0, i - 4);      /* Button1..4 */
            }
            playSe((void*)(uintptr_t)0x4ea3a8, 0, 0xff);
        }
    }
    return ((PF_OptionUpdate)(uintptr_t)0x41b270)(opt);
}

static void PatchOptionDefaults(void) {
    static const unsigned char orig[11] = {
        0x8d,0x8e,0x30,0x57,0x00,0x00, 0xe8,0x1b,0x7c,0xff,0xff
    };
    const DWORD addr = 0x42364a;
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel;

    if (!g_OptionDefaultKey) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("OptionDefault: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("OptionDefault: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
        Log("OptionDefault: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    rel = (DWORD)(uintptr_t)&OptionUpdateHook - (addr + 6 + 5);
    memcpy(p + 7, &rel, 4);   /* call 0x41b270 -> call OptionUpdateHook */
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("OptionDefault: patched (hook=%p)", (void*)&OptionUpdateHook);
}

/*
 * Pause menu: Q -> back to title, R -> retry.
 *
 * In-game scene update (0x413ab0) opens the pause menu with button 7 and, while the menu is
 * active (DAT_004dae16 != 0), calls the menu update 0x419b30 (this = DAT_004dae08):
 *   +0x538 : menu state (6 = idle/open, 4 = "title" chosen, 5 = "retry" chosen,
 *            3 = closing, 1 = resumed).  States 4/5 fade out and then run the game's own
 *            title switch (FUN_004254d0(1,0)) / restart (FUN_00419d10).
 * The hook replaces the `call 0x419b30` at 0x413d26 and forces state 4/5 on Q/R.
 * (When a replay is playing the pause menu is never shown, so the keys do nothing there.)
 */
static const int g_PauseQRKey = 1;

typedef void (__attribute__((fastcall)) *PF_PauseUpdate)(void *menu);

static void __attribute__((fastcall)) PauseUpdateHook(void *menu) {
    void *input = *(void**)(uintptr_t)0x46bbec;
    if (input && menu && OPT_F(menu, 0x538) == 6) {
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        int next = 0;
        if (rawkey(input, 0x51)) next = 4;        /* Q: title */
        else if (rawkey(input, 0x52)) next = 5;   /* R: retry */
        if (next) {
            ((PF_PlaySe)(uintptr_t)0x412210)((void*)(uintptr_t)0x4ea3a8, 0, 0xff);
            OPT_F(menu, 0x538) = next;
        }
    }
    ((PF_PauseUpdate)(uintptr_t)0x419b30)(menu);
}

static void PatchPauseKeys(void) {
    static const unsigned char orig[14] = {
        0x8b,0x0d,0x08,0xae,0x4d,0x00, 0xe8,0x05,0x5e,0x00,0x00, 0xb0,0x01, 0xc3
    };
    const DWORD addr = 0x413d20;
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel;

    if (!g_PauseQRKey) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("PauseKeys: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("PauseKeys: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
        Log("PauseKeys: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    rel = (DWORD)(uintptr_t)&PauseUpdateHook - (addr + 6 + 5);
    memcpy(p + 7, &rel, 4);   /* call 0x419b30 -> call PauseUpdateHook */
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("PauseKeys: patched (hook=%p)", (void*)&PauseUpdateHook);
}

/*
 * Result screen: retry from the pause menu keeps the score.
 *
 * The stage clear result object (DAT_004dae0c, draw function 0x417e30, this = ecx) writes
 *   DAT_004dadf0 (score) = DAT_004dade8 (shown score) = score at result start + bonuses
 * on every draw while it is fully faded in (this+8 == 0x64).  The pause menu retry
 * (0x419d10) clears both and only *requests* a scene switch (DAT_004e93c0 = next scene,
 * DAT_004e93bc = current scene); the switch happens after this frame's draw, so the draw
 * writes the old score back and the restarted game inherits it.
 * The hook replaces the `call 0x417e30` at 0x413ec2 (scene draw 0x413d30) and, when a
 * scene switch is already pending, undoes what the draw wrote to the score variables.
 * (In the normal flow the final value was already committed by earlier frames and the
 * result object is faded out by the time it switches, so nothing changes there.)
 */
static const int g_ResultRetryFix = 1;

typedef void (__attribute__((fastcall)) *PF_ResultDraw)(void *obj);

static void __attribute__((fastcall)) ResultDrawHook(void *obj) {
    volatile int *score = (volatile int*)(uintptr_t)0x4dadf0;
    volatile int *shown = (volatile int*)(uintptr_t)0x4dade8;
    int cur = *(volatile int*)(uintptr_t)0x4e93bc;
    int next = *(volatile int*)(uintptr_t)0x4e93c0;
    int pending = (next != 0 && next != cur);
    int s0 = *score, d0 = *shown;
    ((PF_ResultDraw)(uintptr_t)0x417e30)(obj);
    if (pending && (*score != s0 || *shown != d0)) {
        *score = s0;
        *shown = d0;
        Log("ResultRetry: scene switch pending, kept score=%d shown=%d", s0, d0);
    }
}

static void PatchResultRetry(void) {
    static const unsigned char orig[11] = {
        0x8b,0x0d,0x0c,0xae,0x4d,0x00, 0xe8,0x69,0x3f,0x00,0x00
    };
    const DWORD addr = 0x413ebc;
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel;

    if (!g_ResultRetryFix) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ResultRetry: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ResultRetry: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
        Log("ResultRetry: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    rel = (DWORD)(uintptr_t)&ResultDrawHook - (addr + 6 + 5);
    memcpy(p + 7, &rel, 4);   /* call 0x417e30 -> call ResultDrawHook */
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("ResultRetry: patched (hook=%p)", (void*)&ResultDrawHook);
}

/*
 * Replay menu: invisible stage choices below stage 1-3.
 *
 * Replay menu object = title object + 0x8ad0 (updated by 0x421ee0 from title state 6).
 * After a replay is chosen from the list (+0x27f4 != 0) the stage select screen shows at most 3
 * stage rows, but the number of cursor positions is taken from the replay header copy at
 * +0x2700: BYTE +0x2711 (+1), or 1 when BYTE +0x271e (extra) is set.  A header value above 2
 * therefore creates invisible choices; confirming one starts the replay with stage 4+
 * ([+0x27e8]+1 is passed to 0x431fe0), which shows the ending etc.
 * The hook replaces the `call 0x421ee0` at 0x4236b4 and clamps the menu's header copy (and the
 * cursor) to the 3 visible stages before calling the original.  The replay file is not touched.
 */
static const int g_ReplayStageFix = 1;

typedef void (__attribute__((fastcall)) *PF_ReplayUpdate)(void *obj);

static void __attribute__((fastcall)) ReplayUpdateHook(void *obj) {
    unsigned char *o = (unsigned char*)obj;
    if (o[0x27f4] != 0 && o[0x271e] == 0) {          /* stage select, not extra */
        if (o[0x2711] > 2) {
            Log("ReplayStage: header stage byte %u -> 2", (unsigned)o[0x2711]);
            o[0x2711] = 2;
        }
        if (*(int*)(o + 0x27e8) < 0 || *(int*)(o + 0x27e8) > 2) {
            Log("ReplayStage: cursor %d -> 0", *(int*)(o + 0x27e8));
            *(int*)(o + 0x27e8) = 0;
        }
    }
    ((PF_ReplayUpdate)(uintptr_t)0x421ee0)(obj);
}

static void PatchReplayStage(void) {
    static const unsigned char orig[11] = {
        0x88,0x1d,0x49,0xb1,0x4d,0x00, 0xe8,0x27,0xe8,0xff,0xff
    };
    const DWORD addr = 0x4236ae;
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel;

    if (!g_ReplayStageFix) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ReplayStage: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ReplayStage: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
        Log("ReplayStage: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    rel = (DWORD)(uintptr_t)&ReplayUpdateHook - (addr + 6 + 5);
    memcpy(p + 7, &rel, 4);   /* call 0x421ee0 -> call ReplayUpdateHook */
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("ReplayStage: patched (hook=%p)", (void*)&ReplayUpdateHook);
}

/*
 * Practice start: stage 2 starts with power level 6 (6 boxes on the HUD), stage 3 with MAX.
 *
 * Power = DWORD 0x4dadd0 (0..7; the HUD draws power+1 boxes, 7 is MAX).  0x4dadd4 is the item
 * counter: power p is reached when the counter >= threshold T[p] (DWORD table at 0x45ae00 + 4*p,
 * T[0] is not used), and the HUD gauge is counter - T[p].
 * The title starts a game through 0x423880(stage) (stdcall, `ret 4`) from title state 0xb; it zeroes
 * power and counter and only requests the scene switch.  Practice start = BYTE 0x4dade0 != 0, set
 * by the stage select menu.  The hook replaces the `call 0x423880` at 0x42377f, calls the original and
 * then, for practice stage 2/3, writes the power and a matching counter.  The pause menu retry
 * (0x419d10, stdcall, stage on the stack, called from 0x419c9e/0x419cad/0x419cb9) is hooked the same
 * way, so retrying a practice stage 2/3 gives the same power.
 */
static const int g_PracticeStage2Power = 5;   /* 5 => 6 boxes on the HUD */
static const int g_PracticeStage3Power = 7;   /* 7 => MAX */

typedef void (__stdcall *PF_GameStart)(int stage);

/* Writes the practice power for `stage` (call after the game's own start/retry code has zeroed it). */
static void ApplyPracticePower(int stage, const char *what) {
    if ((stage == 2 || stage == 3) && *(volatile unsigned char*)(uintptr_t)0x4dade0 != 0) {
        int p = (stage == 2) ? g_PracticeStage2Power : g_PracticeStage3Power;
        if (p < 0) return;
        if (p > 7) p = 7;
        *(volatile int*)(uintptr_t)0x4dadd0 = p;
        *(volatile int*)(uintptr_t)0x4dadd4 =
            (p > 0) ? *(volatile int*)(uintptr_t)(0x45ae00 + 4 * p) : 0;
        Log("PracticePower: stage %d %s, power=%d counter=%d", stage, what, p,
            *(volatile int*)(uintptr_t)0x4dadd4);
    }
}

/* title -> game start: replaces `call 0x423880` at 0x42377f */
static void __stdcall GameStartHook(int stage) {
    ((PF_GameStart)(uintptr_t)0x423880)(stage);
    ApplyPracticePower(stage, "start");
}

/* pause menu retry: replaces the three `call 0x419d10` in 0x419b30 (stdcall, stage on the stack) */
static void __stdcall RetryHook(int stage) {
    ((PF_GameStart)(uintptr_t)0x419d10)(stage);
    ApplyPracticePower(stage, "retry");
}

/* Redirects the `call rel32` at callAddr (which must call `target`) to `hook`. */
static int HookCall(DWORD callAddr, DWORD target, void *hook) {
    unsigned char *p = (unsigned char*)callAddr;
    DWORD old, rel, cur;

    if (p[0] != 0xe8) {
        Log("PracticePower: 0x%08X is not a call, skipped", callAddr);
        return 0;
    }
    memcpy(&cur, p + 1, 4);
    if (callAddr + 5 + cur != target) {
        Log("PracticePower: call at 0x%08X does not target 0x%08X, skipped", callAddr, target);
        return 0;
    }
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("PracticePower: VirtualProtect failed at 0x%08X (err=%08X)", callAddr, GetLastError());
        return 0;
    }
    rel = (DWORD)(uintptr_t)hook - (callAddr + 5);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static void PatchPracticePower(void) {
    int n = 0;

    if (g_PracticeStage2Power < 0 && g_PracticeStage3Power < 0) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("PracticePower: unexpected image base, skipped");
        return;
    }
    n += HookCall(0x42377f, 0x423880, (void*)&GameStartHook);   /* title state 0xb */
    n += HookCall(0x419c9e, 0x419d10, (void*)&RetryHook);       /* pause retry, practice */
    n += HookCall(0x419cad, 0x419d10, (void*)&RetryHook);       /* pause retry, extra */
    n += HookCall(0x419cb9, 0x419d10, (void*)&RetryHook);       /* pause retry, normal */
    Log("PracticePower: patched %d/4 call sites", n);
}

/*
 * Pause menu: pause BGM and sound effects while the pause menu is open.
 *
 * Both BGM (streamed ogg) and SE live in DirectSound secondary buffers created through the game's
 * IDirectSound (global 0x4dada8, created by `call 0x450190` (DirectSoundCreate thunk) at 0x411f1c).
 * That call is hooked so that the IDirectSound vtable slots CreateSoundBuffer (+0x0c) and
 * DuplicateSoundBuffer (+0x14) can be wrapped; every non-primary buffer is recorded, and the
 * Release slot of the buffer vtable is wrapped to drop released buffers from the record.
 * A small thread watches BYTE 0x4dae16 (pause menu active):
 *   0 -> 1 : Stop() every buffer that is playing right now and remember it (and DSBSTATUS_LOOPING)
 *   1 -> 0 : Play() the remembered buffers again (the play cursor is kept by Stop/Play), unless the
 *            pause menu is closing for retry/title (pause object *0x4dae08, state at +0x538 is 4/5),
 *            where the scene change takes over and nothing is resumed.
 * Sounds started while the menu is open (cursor / decide SE) are not touched.
 */
static const int g_PauseAudio = 1;

#define AUDIO_MAX 1024
typedef struct { IDirectSoundBuffer *buf; int paused, looping; DWORD pos; } AudioBuf;
static AudioBuf g_audio[AUDIO_MAX];
static int g_audioN = 0;
static CRITICAL_SECTION g_audioCs;

typedef HRESULT (STDMETHODCALLTYPE *PF_DSCreateBuf)(IDirectSound*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *PF_DSDupBuf)(IDirectSound*, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER*);
typedef HRESULT (WINAPI *PF_DSCreate)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
typedef ULONG (STDMETHODCALLTYPE *PF_BufRelease)(void*);

static PF_DSCreateBuf g_origCreateBuf = NULL;
static PF_DSDupBuf g_origDupBuf = NULL;
static struct { void **vt; PF_BufRelease orig; } g_relTab[8];
static int g_relN = 0;

static int PatchVtblSlot(void **slot, void *hook, void **orig) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return 1;
}

static void AudioRemove(void *buf) {   /* caller holds g_audioCs */
    int i;
    for (i = 0; i < g_audioN; i++) {
        if ((void*)g_audio[i].buf == buf) {
            g_audio[i] = g_audio[g_audioN - 1];
            g_audioN--;
            return;
        }
    }
}

static ULONG STDMETHODCALLTYPE BufReleaseHook(void *self) {
    void **vt = *(void***)self;
    PF_BufRelease orig = NULL;
    ULONG r;
    int i;
    for (i = 0; i < g_relN; i++)
        if (g_relTab[i].vt == vt) { orig = g_relTab[i].orig; break; }
    if (!orig) return 0;
    EnterCriticalSection(&g_audioCs);
    r = orig(self);
    if (r == 0) AudioRemove(self);
    LeaveCriticalSection(&g_audioCs);
    return r;
}

static void AudioAdd(IDirectSoundBuffer *buf) {
    void **vt;
    int i, known = 0;
    if (!buf) return;
    EnterCriticalSection(&g_audioCs);
    for (i = 0; i < g_audioN; i++)
        if (g_audio[i].buf == buf) { known = 1; break; }
    if (!known && g_audioN < AUDIO_MAX) {
        g_audio[g_audioN].buf = buf;
        g_audio[g_audioN].paused = 0;
        g_audio[g_audioN].looping = 0;
        g_audioN++;
    }
    vt = *(void***)buf;
    known = 0;
    for (i = 0; i < g_relN; i++)
        if (g_relTab[i].vt == vt) { known = 1; break; }
    if (!known && g_relN < 8) {
        void *orig = NULL;
        if (PatchVtblSlot(&vt[2], (void*)&BufReleaseHook, &orig)) {
            g_relTab[g_relN].vt = vt;
            g_relTab[g_relN].orig = (PF_BufRelease)orig;
            g_relN++;
            Log("PauseAudio: buffer Release hooked (vtbl=%p)", (void*)vt);
        }
    }
    LeaveCriticalSection(&g_audioCs);
}

static HRESULT STDMETHODCALLTYPE DSCreateBufHook(IDirectSound *self, LPCDSBUFFERDESC desc,
                                                 LPDIRECTSOUNDBUFFER *pp, LPUNKNOWN unk) {
    HRESULT hr = g_origCreateBuf(self, desc, pp, unk);
    if (SUCCEEDED(hr) && pp && *pp && desc && !(desc->dwFlags & DSBCAPS_PRIMARYBUFFER))
        AudioAdd(*pp);
    return hr;
}

static HRESULT STDMETHODCALLTYPE DSDupBufHook(IDirectSound *self, LPDIRECTSOUNDBUFFER src,
                                              LPDIRECTSOUNDBUFFER *pp) {
    HRESULT hr = g_origDupBuf(self, src, pp);
    if (SUCCEEDED(hr) && pp && *pp) AudioAdd(*pp);
    return hr;
}

static HRESULT WINAPI DSCreateHook(LPCGUID guid, LPDIRECTSOUND *pp, LPUNKNOWN unk) {
    HRESULT hr = ((PF_DSCreate)(uintptr_t)0x450190)(guid, pp, unk);
    if (SUCCEEDED(hr) && pp && *pp) {
        void **vt = *(void***)*pp;
        void *o1 = NULL, *o2 = NULL;
        if (vt[3] != (void*)&DSCreateBufHook) {
            if (PatchVtblSlot(&vt[3], (void*)&DSCreateBufHook, &o1)) g_origCreateBuf = (PF_DSCreateBuf)o1;
            if (PatchVtblSlot(&vt[5], (void*)&DSDupBufHook, &o2)) g_origDupBuf = (PF_DSDupBuf)o2;
            Log("PauseAudio: IDirectSound %p hooked (CreateSoundBuffer=%p Duplicate=%p)",
                (void*)*pp, o1, o2);
        }
    }
    return hr;
}

static void AudioPause(void) {
    int i, n = 0;
    EnterCriticalSection(&g_audioCs);
    for (i = 0; i < g_audioN; i++) {
        DWORD st = 0;
        IDirectSoundBuffer *b = g_audio[i].buf;
        g_audio[i].paused = 0;
        if (SUCCEEDED(IDirectSoundBuffer_GetStatus(b, &st)) && (st & DSBSTATUS_PLAYING)) {
            g_audio[i].paused = 1;
            g_audio[i].looping = (st & DSBSTATUS_LOOPING) ? 1 : 0;
            IDirectSoundBuffer_Stop(b);
            IDirectSoundBuffer_GetCurrentPosition(b, &g_audio[i].pos, NULL);
            if (g_audio[i].looping)
                Log("PauseAudio: loop buffer %p stopped at play cursor %lu", (void*)b, (unsigned long)g_audio[i].pos);
            n++;
        }
    }
    LeaveCriticalSection(&g_audioCs);
    Log("PauseAudio: paused %d of %d buffers", n, g_audioN);
}

static void AudioResume(int doPlay) {
    int i, n = 0, fixed = 0;
    EnterCriticalSection(&g_audioCs);
    for (i = 0; i < g_audioN; i++) {
        if (!g_audio[i].paused) continue;
        g_audio[i].paused = 0;
        if (doPlay) {
            DWORD st = 0;
            IDirectSoundBuffer *b = g_audio[i].buf;
            if (SUCCEEDED(IDirectSoundBuffer_GetStatus(b, &st)) && !(st & DSBSTATUS_PLAYING)) {
                DWORD cur = 0, after = 0;
                if (st & DSBSTATUS_BUFFERLOST) {
                    IDirectSoundBuffer_Restore(b);
                } else {
                    IDirectSoundBuffer_GetCurrentPosition(b, &cur, NULL);
                    if (cur != g_audio[i].pos) {
                        IDirectSoundBuffer_SetCurrentPosition(b, g_audio[i].pos);
                        fixed++;
                    }
                }
                IDirectSoundBuffer_Play(b, 0, 0, g_audio[i].looping ? DSBPLAY_LOOPING : 0);
                if (g_audio[i].looping) {
                    IDirectSoundBuffer_GetCurrentPosition(b, &after, NULL);
                    Log("PauseAudio: loop buffer %p resumed: stopped=%lu before_play=%lu after_play=%lu",
                        (void*)b, (unsigned long)g_audio[i].pos, (unsigned long)cur, (unsigned long)after);
                }
                n++;
            }
        }
    }
    LeaveCriticalSection(&g_audioCs);
    Log("PauseAudio: %s %d buffers (cursor restored on %d)", doPlay ? "resumed" : "dropped (retry/title)", n, fixed);
}

static DWORD WINAPI PauseAudioThread(LPVOID unused) {
    int prev = 0;
    (void)unused;
    for (;;) {
        int cur;
        Sleep(5);
        cur = (*(volatile unsigned char*)(uintptr_t)0x4dae16) != 0;
        if (cur && !prev) {
            AudioPause();
        } else if (!cur && prev) {
            int doPlay = 1;
            void *pm = *(void* volatile*)(uintptr_t)0x4dae08;
            if (pm) {
                int st = *(volatile int*)((char*)pm + 0x538);
                if (st == 4 || st == 5) doPlay = 0;
            }
            AudioResume(doPlay);
        }
        prev = cur;
    }
    return 0;
}

static void PatchPauseAudio(void) {
    HANDLE th;
    if (!g_PauseAudio) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("PauseAudio: unexpected image base, skipped");
        return;
    }
    InitializeCriticalSection(&g_audioCs);
    if (!HookCall(0x411f1c, 0x450190, (void*)&DSCreateHook)) {
        Log("PauseAudio: DirectSoundCreate call not hooked, skipped");
        return;
    }
    th = CreateThread(NULL, 0, PauseAudioThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    Log("PauseAudio: patched (thread %s)", th ? "ok" : "failed");
}

/*
 * Sound effect playback fix.
 *
 * SE play function (0x412210, thiscall, ecx = SE object, stack: pan, volume, ret 8):
 *   +0x00 : IDirectSoundBuffer** (duplicated buffers of one SE, used round-robin)
 *   +0x04 : buffer count
 *   +0x08 : index of the buffer to use next
 *   +0x0c : per-SE volume byte
 *   0x4dad9c : global SE volume byte, 0x46c82c : EnableSoundStop
 * Original behaviour:
 *   - GetStatus failure returns without advancing the index, so one broken buffer
 *     blocks that SE forever.
 *   - DSBSTATUS_BUFFERLOST is never handled; Play() on a lost buffer is silent.
 *   - A buffer that is not playing is started with Play() only, without rewinding
 *     the play cursor (relies on the driver resetting it after the buffer ended).
 *     If the cursor is left at the end the SE finishes instantly and nothing is heard.
 * The replacement below always advances the index, restores lost buffers, rewinds
 * the cursor before every Play() and retries once when Play() reports a lost buffer.
 * Volume/pan are computed exactly like the original (constants are read from the exe).
 */
static const int g_SeFix = 1;
static const int g_SePanLimit = 0;   /* clamp |pan| of SE (0 = always centered, 10000 = original panning) */
static const int g_SeTrace = 1;   /* log rare SE calls / anomalies to Smd_ddraw.log */
static int g_seTraceCount = 0;
#define SE_TRACE_MAX 4000
#define SE_TRACE_RARE 20          /* log the first N calls of every SE object individually */

typedef struct { void *se; int count; unsigned calls, idle; } SeStat;
static SeStat g_seStat[64];
static int g_seStatN = 0;

static SeStat *SeStatFind(void *se, int count) {
    int i;
    for (i = 0; i < g_seStatN; i++)
        if (g_seStat[i].se == se) return &g_seStat[i];
    if (g_seStatN >= 64) return NULL;
    g_seStat[g_seStatN].se = se;
    g_seStat[g_seStatN].count = count;
    g_seStat[g_seStatN].calls = 0;
    g_seStat[g_seStatN].idle = 0;
    return &g_seStat[g_seStatN++];
}

static void SeSummary(void) {
    int i;
    for (i = 0; i < g_seStatN; i++)
        Log("SeSummary: se=%p buffers=%d calls=%u started_idle=%u",
            g_seStat[i].se, g_seStat[i].count, g_seStat[i].calls, g_seStat[i].idle);
}
static int g_seLogCount = 0;
#define SE_LOG_MAX 64

static void SeLog(const char *what, HRESULT hr) {
    if (g_seLogCount >= SE_LOG_MAX) return;
    g_seLogCount++;
    Log("SeFix: %s hr=%08X", what, (unsigned)hr);
}

static int SeVolumeToDb(unsigned v) {
    double x;
    int db;
    if (v == 0) return -10000;
    x = log10((double)v * *(double*)(uintptr_t)0x453510)
        * *(double*)(uintptr_t)0x453508 * *(double*)(uintptr_t)0x453500;
    db = (int)x;
    if (db > 0) return 0;
    if (db < -10000) return -10000;
    return db;
}

static void __attribute__((thiscall)) SePlayHook(void *se, int pan, int vol) {
    IDirectSoundBuffer **bufs = *(IDirectSoundBuffer***)se;
    int count = *(int*)((char*)se + 4);
    int idx = *(int*)((char*)se + 8);
    unsigned seVol = *(volatile unsigned char*)(uintptr_t)0x4dad9c;
    unsigned objVol = *((unsigned char*)se + 0xc);
    int stopFirst = *(volatile int*)(uintptr_t)0x46c82c;
    IDirectSoundBuffer *buf;
    DWORD status = 0;
    HRESULT hr;

    if (!bufs || count <= 0) return;
    if (idx < 0 || idx >= count) idx = 0;
    /* advance first so that a broken buffer can never block the round-robin */
    *(int*)((char*)se + 8) = (idx + 1 >= count) ? 0 : idx + 1;

    buf = bufs[idx];
    if (!buf) return;

    hr = IDirectSoundBuffer_GetStatus(buf, &status);
    if (FAILED(hr)) {
        SeLog("GetStatus failed", hr);
        return;
    }
    if (status & DSBSTATUS_BUFFERLOST) {
        hr = IDirectSoundBuffer_Restore(buf);
        SeLog("buffer lost, Restore", hr);
        if (FAILED(hr)) return;
        status = 0;
    }

    int db = SeVolumeToDb(seVol * objVol * (unsigned)(vol & 0xff));
    IDirectSoundBuffer_SetVolume(buf, db);
    if (pan > g_SePanLimit) pan = g_SePanLimit;
    if (pan < -g_SePanLimit) pan = -g_SePanLimit;
    IDirectSoundBuffer_SetPan(buf, pan);

    if ((status & DSBSTATUS_PLAYING) && stopFirst)
        IDirectSoundBuffer_Stop(buf);
    IDirectSoundBuffer_SetCurrentPosition(buf, 0);
    hr = IDirectSoundBuffer_Play(buf, 0, 0, 0);
    if (hr == DSERR_BUFFERLOST) {
        SeLog("Play: buffer lost, retry", hr);
        if (SUCCEEDED(IDirectSoundBuffer_Restore(buf))) {
            IDirectSoundBuffer_SetCurrentPosition(buf, 0);
            hr = IDirectSoundBuffer_Play(buf, 0, 0, 0);
        }
    }
    if (FAILED(hr)) SeLog("Play failed", hr);
    if (g_SeTrace) {
        static DWORD lastSummary = 0;
        DWORD st2 = 0, now = GetTickCount();
        SeStat *s = SeStatFind(se, count);
        IDirectSoundBuffer_GetStatus(buf, &st2);
        if (s) {
            s->calls++;
            if (!(status & DSBSTATUS_PLAYING)) s->idle++;
        }
        if (g_seTraceCount < SE_TRACE_MAX &&
            ((s && s->calls <= SE_TRACE_RARE) || !(st2 & DSBSTATUS_PLAYING) || FAILED(hr))) {
            g_seTraceCount++;
            Log("SeTrace: se=%p #%u buf=%d/%d before=%lX after=%lX pan=%d vol=%u gvol=%u ovol=%u db=%d hr=%08X",
                se, s ? s->calls : 0, idx, count, (unsigned long)status, (unsigned long)st2, pan,
                (unsigned)(vol & 0xff), seVol, objVol, db, (unsigned)hr);
        }
        if (now - lastSummary > 30000) {
            lastSummary = now;
            SeSummary();
        }
    }
}

static void PatchSoundFix(void) {
    static const unsigned char orig[10] = {
        0x51, 0x53, 0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x07, 0x85, 0xc0
    };
    const DWORD addr = 0x412210;
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel;

    if (!g_SeFix) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("SeFix: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("SeFix: code signature mismatch, skipped");
        return;
    }
    Log("SeFix: PanLimit=%d", g_SePanLimit);
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) {
        Log("SeFix: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    rel = (DWORD)(uintptr_t)&SePlayHook - (addr + 5);
    p[0] = 0xe9;              /* jmp SePlayHook (original body is never entered again) */
    memcpy(p + 1, &rel, 4);
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    Log("SeFix: patched (hook=%p)", (void*)&SePlayHook);
}

static HMODULE g_real = NULL;
static FARPROC p_DirectDrawCreate;
static FARPROC p_DirectDrawCreateClipper;
static FARPROC p_DirectDrawCreateEx;
static FARPROC p_DirectDrawEnumerateA;
static FARPROC p_DirectDrawEnumerateW;
static FARPROC p_DirectDrawEnumerateExA;
static FARPROC p_DirectDrawEnumerateExW;
static FARPROC p_DllCanUnloadNow;
static FARPROC p_DllGetClassObject;
static FARPROC p_DllRegisterServer;
static FARPROC p_DllUnregisterServer;

static void LoadReal(void) {
    if (g_real) return;
    LogOpen();
    QueryPerformanceFrequency(&g_qpcFreq);
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strcat(sys, "\\ddraw.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) {
        Log("[Error] LoadSystem32 ddraw.dll failed (err=%08X)", GetLastError());
        return;
    }
    p_DirectDrawCreate        = GetProcAddress(g_real, "DirectDrawCreate");
    p_DirectDrawCreateClipper = GetProcAddress(g_real, "DirectDrawCreateClipper");
    p_DirectDrawCreateEx      = GetProcAddress(g_real, "DirectDrawCreateEx");
    p_DirectDrawEnumerateA    = GetProcAddress(g_real, "DirectDrawEnumerateA");
    p_DirectDrawEnumerateW    = GetProcAddress(g_real, "DirectDrawEnumerateW");
    p_DirectDrawEnumerateExA  = GetProcAddress(g_real, "DirectDrawEnumerateExA");
    p_DirectDrawEnumerateExW  = GetProcAddress(g_real, "DirectDrawEnumerateExW");
    p_DllCanUnloadNow         = GetProcAddress(g_real, "DllCanUnloadNow");
    p_DllGetClassObject       = GetProcAddress(g_real, "DllGetClassObject");
    p_DllRegisterServer       = GetProcAddress(g_real, "DllRegisterServer");
    p_DllUnregisterServer     = GetProcAddress(g_real, "DllUnregisterServer");
    Log("Smd ddraw proxy loaded (real=%p lowlatency=%d dwmflush=%d)",
        (void*)g_real, g_LowLatency, g_UseDwmFlush);
}

typedef HRESULT (WINAPI *PF_DDC)(GUID*, LPDIRECTDRAW*, IUnknown*);
typedef HRESULT (WINAPI *PF_DDClip)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*);
typedef HRESULT (WINAPI *PF_DDCEx)(GUID*, LPVOID*, REFIID, IUnknown*);
typedef HRESULT (WINAPI *PF_DDEnum)(LPDDENUMCALLBACKA, LPVOID);
typedef HRESULT (WINAPI *PF_DDEnumEx)(LPDDENUMCALLBACKEXA, LPVOID, DWORD);
typedef HRESULT (WINAPI *PF_FPOCH)(void);

typedef struct { IDirectDraw7Vtbl *lpVtbl; IDirectDraw7 *orig; } DD7W;

static const GUID k_IID_IDirectDraw7 = {
    0x15E65EC0, 0x3B9C, 0x11D2,
    { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B }
};

static HRESULT STDMETHODCALLTYPE WDD7_QueryInterface(IDirectDraw7 *self, REFIID riid, void **out) {
    DD7W *w = (DD7W*)self;
    if (out) *out = NULL;
    if (!IsEqualCLSID(riid, &k_IID_IDirectDraw7)) {
        HRESULT hr = IDirectDraw7_QueryInterface(w->orig, riid, out);
        Log("DD7.QI other iid -> hr=%08X", hr);
        return hr;
    }
    IDirectDraw7_AddRef(w->orig);
    if (out) *out = self;
    return DD_OK;
}
static ULONG STDMETHODCALLTYPE WDD7_AddRef(IDirectDraw7 *self) {
    DD7W *w = (DD7W*)self;
    return IDirectDraw7_AddRef(w->orig);
}
static ULONG STDMETHODCALLTYPE WDD7_Release(IDirectDraw7 *self) {
    DD7W *w = (DD7W*)self;
    ULONG r = IDirectDraw7_Release(w->orig);
    Log("DD7.Release -> orig refs=%lu", r);
    if (r == 0) {
        free(w);
        Log("DD7 wrapper destroyed");
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE WDD7_Compact(IDirectDraw7 *self) {
    return IDirectDraw7_Compact(((DD7W*)self)->orig);
}
static HRESULT STDMETHODCALLTYPE WDD7_CreateClipper(IDirectDraw7 *self, DWORD f, LPDIRECTDRAWCLIPPER *c, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreateClipper(w->orig, f, c, u);
    Log("DD7.CreateClipper f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_CreatePalette(IDirectDraw7 *self, DWORD f, LPPALETTEENTRY t, LPDIRECTDRAWPALETTE *p, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreatePalette(w->orig, f, t, p, u);
    Log("DD7.CreatePalette f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_CreateSurface(IDirectDraw7 *self, LPDDSURFACEDESC2 d, LPDIRECTDRAWSURFACE7 *s, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreateSurface(w->orig, d, s, u);
    if (d && g_LogAll)
        Log("DD7.CreateSurface %lux%lux bpp=%d caps=%lX/%lX back=%d dwFlags=%lX -> hr=%08X",
            (unsigned long)d->dwWidth, (unsigned long)d->dwHeight,
            (d->dwFlags & DDSD_PIXELFORMAT) ? (int)d->ddpfPixelFormat.dwRGBBitCount : -1,
            (unsigned long)((d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0),
            (unsigned long)d->dwFlags, (int)d->dwBackBufferCount, hr);
    else if (hr < 0)
        Log("DD7.CreateSurface -> hr=%08X", hr);
    if (hr >= 0 && g_LowLatency && d && s && *s &&
        (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        HookPrimary(*s);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_DuplicateSurface(IDirectDraw7 *self, LPDIRECTDRAWSURFACE7 s, LPDIRECTDRAWSURFACE7 *o) {
    return IDirectDraw7_DuplicateSurface(((DD7W*)self)->orig, s, o);
}
static HRESULT STDMETHODCALLTYPE WDD7_EnumDisplayModes(IDirectDraw7 *self, DWORD f, LPDDSURFACEDESC2 d, LPVOID c, LPDDENUMMODESCALLBACK2 cb) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_EnumDisplayModes(w->orig, f, d, c, cb);
    Log("DD7.EnumDisplayModes f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_EnumSurfaces(IDirectDraw7 *self, DWORD f, LPDDSURFACEDESC2 d, LPVOID c, LPDDENUMSURFACESCALLBACK7 cb) {
    return IDirectDraw7_EnumSurfaces(((DD7W*)self)->orig, f, d, c, cb);
}
static HRESULT STDMETHODCALLTYPE WDD7_FlipToGDISurface(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_FlipToGDISurface(((DD7W*)self)->orig);
    Log("DD7.FlipToGDISurface -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetCaps(IDirectDraw7 *self, LPDDCAPS h, LPDDCAPS hel) {
    HRESULT hr = IDirectDraw7_GetCaps(((DD7W*)self)->orig, h, hel);
    if (g_LogAll) Log("DD7.GetCaps -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetDisplayMode(IDirectDraw7 *self, LPDDSURFACEDESC2 d) {
    HRESULT hr = IDirectDraw7_GetDisplayMode(((DD7W*)self)->orig, d);
    if (d && g_LogAll)
        Log("DD7.GetDisplayMode -> hr=%08X %lux%lux modex=%s bpp=%d", hr,
            (unsigned long)d->dwWidth, (unsigned long)d->dwHeight,
            (d->ddpfPixelFormat.dwFlags & DDPF_RGB) ? "RGB" : "NOT_RGB",
            (d->ddpfPixelFormat.dwFlags & DDPF_RGB) ? (int)d->ddpfPixelFormat.dwRGBBitCount : -1);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetFourCCCodes(IDirectDraw7 *self, LPDWORD n, LPDWORD l) {
    return IDirectDraw7_GetFourCCCodes(((DD7W*)self)->orig, n, l);
}
static HRESULT STDMETHODCALLTYPE WDD7_GetGDISurface(IDirectDraw7 *self, LPDIRECTDRAWSURFACE7 *s) {
    return IDirectDraw7_GetGDISurface(((DD7W*)self)->orig, s);
}
static HRESULT STDMETHODCALLTYPE WDD7_GetMonitorFrequency(IDirectDraw7 *self, LPDWORD f) {
    HRESULT hr = IDirectDraw7_GetMonitorFrequency(((DD7W*)self)->orig, f);
    if (g_LogAll) Log("DD7.GetMonitorFrequency -> hr=%08X freq=%lu", hr, f ? *f : 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetScanLine(IDirectDraw7 *self, LPDWORD s) {
    return IDirectDraw7_GetScanLine(((DD7W*)self)->orig, s);
}
static HRESULT STDMETHODCALLTYPE WDD7_GetVerticalBlankStatus(IDirectDraw7 *self, WINBOOL *b) {
    return IDirectDraw7_GetVerticalBlankStatus(((DD7W*)self)->orig, b);
}
static HRESULT STDMETHODCALLTYPE WDD7_Initialize(IDirectDraw7 *self, GUID *g) {
    return IDirectDraw7_Initialize(((DD7W*)self)->orig, g);
}
static HRESULT STDMETHODCALLTYPE WDD7_RestoreDisplayMode(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_RestoreDisplayMode(((DD7W*)self)->orig);
    Log("DD7.RestoreDisplayMode -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_SetCooperativeLevel(IDirectDraw7 *self, HWND hwnd, DWORD flags) {
    DD7W *w7 = (DD7W*)self;
    if (g_StripAllowModeX && (flags & DDSCL_ALLOWMODEX)) {
        flags &= ~DDSCL_ALLOWMODEX;
    }
    HRESULT hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, flags);
    Log("DD7.SetCooperativeLevel hwnd=%p flags=%X -> hr=%08X",
        (void*)hwnd, (unsigned)flags, hr);
    if (hr >= 0 || !g_RetryCoopLevel) return hr;

    if (flags & DDSCL_EXCLUSIVE) {
        DWORD f = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        Log("DD7.SetCooperativeLevel retry1 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
    }
    {
        DWORD f = DDSCL_NORMAL | DDSCL_NOWINDOWCHANGES;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        Log("DD7.SetCooperativeLevel retry2 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
    }
    if (flags & DDSCL_FULLSCREEN) {
        DWORD f = DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        Log("DD7.SetCooperativeLevel retry3 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
    }
    return DDERR_GENERIC;
}
static HRESULT STDMETHODCALLTYPE WDD7_SetDisplayMode(IDirectDraw7 *self, DWORD wd, DWORD ht, DWORD bpp, DWORD refresh, DWORD flags) {
    DD7W *w7 = (DD7W*)self;
    HRESULT hr = IDirectDraw7_SetDisplayMode(w7->orig, wd, ht, bpp, refresh, flags);
    Log("DD7.SetDisplayMode %lux%lux bpp=%lu refresh=%lu f=%X -> hr=%08X",
        (unsigned long)wd, (unsigned long)ht, (unsigned long)bpp, (unsigned long)refresh, (unsigned)flags, hr);
    if (hr < 0 && g_RetryDisplayMode && !(refresh == 0 && flags == 0)) {
        hr = IDirectDraw7_SetDisplayMode(w7->orig, wd, ht, bpp, 0, 0);
        Log("DD7.SetDisplayMode refresh/flags retry -> hr=%08X", hr);
    }
    if (hr < 0 && g_RetryDisplayMode) {
        DWORD b2 = (bpp == 32) ? 16 : 32;
        hr = IDirectDraw7_SetDisplayMode(w7->orig, wd, ht, b2, 0, 0);
        Log("DD7.SetDisplayMode bpp retry -> %lu -> hr=%08X", (unsigned long)b2, hr);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_WaitForVerticalBlank(IDirectDraw7 *self, DWORD f, HANDLE ev) {
    HRESULT hr = IDirectDraw7_WaitForVerticalBlank(((DD7W*)self)->orig, f, ev);
    if (g_LogAll) Log("DD7.WaitForVerticalBlank f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetAvailableVidMem(IDirectDraw7 *self, LPDDSCAPS2 c, LPDWORD t, LPDWORD freev) {
    return IDirectDraw7_GetAvailableVidMem(((DD7W*)self)->orig, c, t, freev);
}
static HRESULT STDMETHODCALLTYPE WDD7_GetSurfaceFromDC(IDirectDraw7 *self, HDC dc, LPDIRECTDRAWSURFACE7 *s) {
    return IDirectDraw7_GetSurfaceFromDC(((DD7W*)self)->orig, dc, s);
}
static HRESULT STDMETHODCALLTYPE WDD7_RestoreAllSurfaces(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_RestoreAllSurfaces(((DD7W*)self)->orig);
    Log("DD7.RestoreAllSurfaces -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_TestCooperativeLevel(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_TestCooperativeLevel(((DD7W*)self)->orig);
    if (hr < 0 || g_LogAll) Log("DD7.TestCooperativeLevel -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetDeviceIdentifier(IDirectDraw7 *self, LPDDDEVICEIDENTIFIER2 p, DWORD f) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_GetDeviceIdentifier(w->orig, p, f);
    Log("DD7.GetDeviceIdentifier -> hr=%08X dev=%lld desc=%.64s", hr,
        p ? p->liDriverVersion.QuadPart : 0LL, p ? p->szDescription : "");
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_StartModeTest(IDirectDraw7 *self, LPSIZE s, DWORD n, DWORD f) {
    return IDirectDraw7_StartModeTest(((DD7W*)self)->orig, s, n, f);
}
static HRESULT STDMETHODCALLTYPE WDD7_EvaluateMode(IDirectDraw7 *self, DWORD f, DWORD *t) {
    return IDirectDraw7_EvaluateMode(((DD7W*)self)->orig, f, t);
}

static IDirectDraw7Vtbl g_dd7Vtbl = {
    WDD7_QueryInterface,
    WDD7_AddRef,
    WDD7_Release,
    WDD7_Compact,
    WDD7_CreateClipper,
    WDD7_CreatePalette,
    WDD7_CreateSurface,
    WDD7_DuplicateSurface,
    WDD7_EnumDisplayModes,
    WDD7_EnumSurfaces,
    WDD7_FlipToGDISurface,
    WDD7_GetCaps,
    WDD7_GetDisplayMode,
    WDD7_GetFourCCCodes,
    WDD7_GetGDISurface,
    WDD7_GetMonitorFrequency,
    WDD7_GetScanLine,
    WDD7_GetVerticalBlankStatus,
    WDD7_Initialize,
    WDD7_RestoreDisplayMode,
    WDD7_SetCooperativeLevel,
    WDD7_SetDisplayMode,
    WDD7_WaitForVerticalBlank,
    WDD7_GetAvailableVidMem,
    WDD7_GetSurfaceFromDC,
    WDD7_RestoreAllSurfaces,
    WDD7_TestCooperativeLevel,
    WDD7_GetDeviceIdentifier,
    WDD7_StartModeTest,
    WDD7_EvaluateMode
};

static IDirectDraw7* WrapDD7(IDirectDraw7 *orig) {
    DD7W *w = (DD7W*)malloc(sizeof(DD7W));
    if (!w) return orig;
    w->lpVtbl = &g_dd7Vtbl;
    w->orig = orig;
    IDirectDraw7_AddRef(orig);
    return (IDirectDraw7*)w;
}

static HRESULT WINAPI Hook_DirectDrawCreateEx(GUID *guid, LPVOID *out, REFIID riid, IUnknown *unk) {
    LoadReal();
    if (!out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    if (!p_DirectDrawCreateEx) return DDERR_GENERIC;
    PF_DDCEx fn = (PF_DDCEx)p_DirectDrawCreateEx;
    if (!IsEqualCLSID(riid, &k_IID_IDirectDraw7)) {
        HRESULT hr = fn(guid, out, riid, unk);
        Log("DirectDrawCreateEx (other) -> hr=%08X", hr);
        return hr;
    }
    IDirectDraw7 *orig = NULL;
    HRESULT hr = fn(guid, (LPVOID*)&orig, riid, unk);
    Log("DirectDrawCreateEx(IID_IDirectDraw7) -> hr=%08X orig=%p", hr, (void*)orig);
    if (hr < 0 || !orig) return hr;
    IDirectDraw7 *wrapped = WrapDD7(orig);
    if (wrapped != orig) {
        Log("DD7 wrapped ok");
        IDirectDraw7_Release(orig);
    }
    *out = wrapped;
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID *g, LPDIRECTDRAW *out, IUnknown *u) {
    LoadReal();
    if (!p_DirectDrawCreate) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDC)p_DirectDrawCreate)(g, out, u);
    Log("DirectDrawCreate -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER *c, IUnknown *u) {
    LoadReal();
    if (!p_DirectDrawCreateClipper) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDClip)p_DirectDrawCreateClipper)(flags, c, u);
    Log("DirectDrawCreateClipper -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(GUID *g, LPVOID *o, REFIID i, IUnknown *u) {
    return Hook_DirectDrawCreateEx(g, o, i, u);
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, void *ctx) {
    LoadReal();
    if (!p_DirectDrawEnumerateA) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnum)p_DirectDrawEnumerateA)(cb, ctx);
    Log("DirectDrawEnumerateA -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, void *ctx) {
    typedef HRESULT (WINAPI *PF_DDEnumW)(LPDDENUMCALLBACKW, LPVOID);
    LoadReal();
    if (!p_DirectDrawEnumerateW) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumW)p_DirectDrawEnumerateW)(cb, ctx);
    Log("DirectDrawEnumerateW -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, void *ctx, DWORD flags) {
    LoadReal();
    if (!p_DirectDrawEnumerateExA) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumEx)p_DirectDrawEnumerateExA)(cb, ctx, flags);
    Log("DirectDrawEnumerateExA -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, void *ctx, DWORD flags) {
    typedef HRESULT (WINAPI *PF_DDEnumExW)(LPDDENUMCALLBACKEXW, LPVOID, DWORD);
    LoadReal();
    if (!p_DirectDrawEnumerateExW) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumExW)p_DirectDrawEnumerateExW)(cb, ctx, flags);
    Log("DirectDrawEnumerateExW -> hr=%08X", hr);
    return hr;
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) {
    LoadReal();
    if (!p_DllCanUnloadNow) return S_FALSE;
    return ((PF_FPOCH)p_DllCanUnloadNow)();
}
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID cls, REFIID iid, void **out) {
    typedef HRESULT (WINAPI *PF_DGCO)(REFCLSID, REFIID, void**);
    LoadReal();
    if (!p_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return ((PF_DGCO)p_DllGetClassObject)(cls, iid, out);
}
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) {
    LoadReal();
    if (!p_DllRegisterServer) return E_FAIL;
    return ((PF_FPOCH)p_DllRegisterServer)();
}
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) {
    LoadReal();
    if (!p_DllUnregisterServer) return E_FAIL;
    return ((PF_FPOCH)p_DllUnregisterServer)();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        LoadReal();
        Log("process attach (pid=%lu)", GetCurrentProcessId());
        PatchTitleCancel();
        PatchOptionDefaults();
        PatchPauseKeys();
        PatchResultRetry();
        PatchReplayStage();
        PatchPracticePower();
        PatchPauseAudio();
        PatchSoundFix();
        break;
    case DLL_PROCESS_DETACH:
        Log("process detach");
        SeSummary();
        if (g_log) {
            FILE *f = g_log;
            g_log = NULL;
            fclose(f);
        }
        break;
    default:
        break;
    }
    return TRUE;
}
