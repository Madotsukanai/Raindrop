#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
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
static const int g_KeyLatencyStats = 0;
static const int g_WaitTimeoutMs = 50;
static const int g_ForceColorDepth32 = 1;
static const int g_AutoWindowFallback = 1;
static const int g_ForceWindowMode = 0;
static int g_fallbackInstance = 0;
static int g_fullscreenRequested = 0;
static int g_displayModeFailures = 0;

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
    {
        /* flushing every line costs a write syscall per log call; flush at most every 200 ms */
        static DWORD lastFlush = 0;
        DWORD now = GetTickCount();
        if (now - lastFlush >= 200) {
            fflush(g_log);
            lastFlush = now;
        }
    }
}

static void LogWindowsVersion(void) {
    typedef LONG (WINAPI *PF_RtlGetVersion)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    PF_RtlGetVersion fn = nt ? (PF_RtlGetVersion)(void*)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0)
        Log("Windows %lu.%lu build %lu", (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
            (unsigned long)vi.dwBuildNumber);
}

static void InitFallbackInstance(void) {
    char buf[32];
    DWORD n = GetEnvironmentVariableA("SMD_WND_FALLBACK", buf, sizeof(buf));
    DWORD ppid;
    if (n == 0 || n >= sizeof(buf)) return;
    g_fallbackInstance = 1;
    ppid = (DWORD)strtoul(buf, NULL, 10);
    SetEnvironmentVariableA("SMD_WND_FALLBACK", NULL);
    if (ppid && ppid != GetCurrentProcessId()) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, ppid);
        if (h) {
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
    Log("WindowFallback: this is the window mode fallback instance (parent pid=%lu)", (unsigned long)ppid);
}

static void TriggerWindowFallback(const char *why) {
    char exe[MAX_PATH], dir[MAX_PATH], pidbuf[32];
    char *cmd, *slash;
    const char *cl;
    size_t n;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (!g_AutoWindowFallback || g_fallbackInstance) return;
    Log("WindowFallback: %s -> relaunching in window mode", why);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    cl = GetCommandLineA();
    n = strlen(cl);
    cmd = (char*)malloc(n + 1);
    if (!cmd) return;
    memcpy(cmd, cl, n + 1);
    sprintf(pidbuf, "%lu", (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableA("SMD_WND_FALLBACK", pidbuf);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        DWORD newPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        free(cmd);
        Log("WindowFallback: new process started (pid=%lu), exiting", (unsigned long)newPid);
        if (g_log) fflush(g_log);
        ExitProcess(0);
    }
    Log("WindowFallback: CreateProcess failed (err=%08X)", (unsigned)GetLastError());
    SetEnvironmentVariableA("SMD_WND_FALLBACK", NULL);
    free(cmd);
}

static HRESULT TrySetDisplayMode(IDirectDraw7 *orig, DWORD wd, DWORD ht, DWORD bpp, DWORD refresh, DWORD flags) {
    DWORD tries[3] = { 0, 0, 60 };
    DEVMODEA dm;
    HRESULT hr = DDERR_GENERIC;
    int i;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        tries[1] = dm.dmDisplayFrequency;
    for (i = 0; i < 3; i++) {
        if (i == 1 && tries[1] == 0) continue;
        if (tries[i] == refresh && flags == 0) continue;
        hr = IDirectDraw7_SetDisplayMode(orig, wd, ht, bpp, tries[i], 0);
        Log("DD7.SetDisplayMode retry refresh=%lu -> hr=%08X", (unsigned long)tries[i], (unsigned)hr);
        if (hr >= 0) return hr;
    }
    return hr;
}

static void PatchColorDepth32(void) {
    static const unsigned char orig[11] = {
        0x83,0xf8,0x10, 0xa3,0x30,0xc8,0x46,0x00, 0x5e, 0x74,0x1c
    };
    const DWORD addr = 0x405951;
    unsigned char *p = (unsigned char*)addr;
    DWORD old;

    if (!g_ForceColorDepth32) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ColorDepth32: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ColorDepth32: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ColorDepth32: VirtualProtect failed (err=%08X)", (unsigned)GetLastError());
        return;
    }
    p[0] = 0x6a; p[1] = 0x20; p[2] = 0x58;
    VirtualProtect(p, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 3);
    Log("ColorDepth32: patched (ColorDepth in config.txt is ignored, always 32)");
}

static void PatchForceWindowMode(void) {
    static const unsigned char orig[10] = {
        0xe8,0x74,0x04,0x00,0x00, 0x25,0xff,0x00,0x00,0x00
    };
    const DWORD addr = 0x4055c7;
    unsigned char *p = (unsigned char*)addr;
    DWORD old;

    if (!g_ForceWindowMode && !g_fallbackInstance) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ForceWindow: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ForceWindow: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p + 5, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ForceWindow: VirtualProtect failed (err=%08X)", (unsigned)GetLastError());
        return;
    }
    p[5] = 0xb8; p[6] = 0x01; p[7] = 0x00; p[8] = 0x00; p[9] = 0x00;
    VirtualProtect(p + 5, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 10);
    Log("ForceWindow: patched (WndMode is forced on)");
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
static double g_fwSleptMs = 0.0;
static unsigned g_fwSleeps = 0, g_fwSpins = 0;
static double g_cpuPrevMs = 0.0;
static unsigned g_stQuads = 0, g_stBatchCalls = 0, g_stPass = 0, g_stTexApplied = 0, g_stTexSkipped = 0;
static int g_batchInstalled = 0;

static double ProcessCpuMs(void) {
    FILETIME c, e, k, u;
    ULARGE_INTEGER kk, uu;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (double)(kk.QuadPart + uu.QuadPart) / 10000.0;
}

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

static void CensusSummaryPeriodic(void);

static void FlushStats(LONGLONG now) {
    double sec = TicksToMs(now - g_winStart) / 1000.0;
    CensusSummaryPeriodic();
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
    {
        double cpu = ProcessCpuMs();
        Log("  process cpu=%.1f%% of one core (%.0f ms in %.1fs) | frame wait sleeps=%u slept=%.1f ms spins=%u",
            sec > 0 ? (cpu - g_cpuPrevMs) / (sec * 10.0) : 0.0, cpu - g_cpuPrevMs, sec,
            g_fwSleeps, g_fwSleptMs, g_fwSpins);
        g_cpuPrevMs = cpu;
        g_fwSleeps = 0;
        g_fwSpins = 0;
        g_fwSleptMs = 0.0;
        Log("  draw batching: %s quads=%u in %u draw calls, other draws=%u | texture sets applied=%u skipped=%u",
            g_batchInstalled ? "on" : "off", g_stQuads, g_stBatchCalls, g_stPass, g_stTexApplied,
            g_stTexSkipped);
        g_stQuads = g_stBatchCalls = g_stPass = g_stTexApplied = g_stTexSkipped = 0;
    }
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
    HANDLE th = NULL;
    {
        typedef UINT (WINAPI *PF_TBP2)(UINT);
        HMODULE mm = LoadLibraryA("winmm.dll");
        PF_TBP2 tbp = mm ? (PF_TBP2)(void*)GetProcAddress(mm, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
    }
    if (g_KeyLatencyStats) {
        th = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    g_measureReady = 1;
    Log("Measure: lowlatency=%d dwmflush=%d dwmapi=%s keythread=%s interval=%dms",
        g_LowLatency, g_UseDwmFlush, g_pDwmFlush ? "ok" : "none",
        th ? "ok" : (g_KeyLatencyStats ? "failed" : "off"), g_StatsIntervalMs);
}

static void BatchFlush(void);

static HRESULT STDMETHODCALLTYPE Hook_Blt(IDirectDrawSurface7 *self, LPRECT dr, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD flags, LPDDBLTFX fx) {
    LARGE_INTEGER a, b;
    BatchFlush();
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origBlt(self, dr, src, sr, flags, fx);
    QueryPerformanceCounter(&b);
    if (hr >= 0 && self == g_primary && src && g_measureReady) FinishPresent(self, 0, a, b);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Flip(IDirectDrawSurface7 *self, LPDIRECTDRAWSURFACE7 target, DWORD flags) {
    LARGE_INTEGER a, b;
    BatchFlush();
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

static const int g_TitleCancelExit = 1;
static int g_titleEscCarryover = 0;

typedef unsigned char (__attribute__((thiscall)) *PF_InputKey)(void *self, int id);

static int __cdecl TitleCancelHook(void *input) {
    static int cancelHeld = 0;
    PF_InputKey button = (PF_InputKey)(uintptr_t)0x431540;
    PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
    int buttonDown, escDown, down;
    if (!input) return 0;
    buttonDown = button(input, 5) || button(input, 7);
    escDown = rawkey(input, 0x1b);
    if (g_titleEscCarryover) {
        if (escDown) {
            if (!buttonDown) return 0;
            escDown = 0;
        } else {
            g_titleEscCarryover = 0;
        }
    }
    down = buttonDown || escDown;
    if (!down) {
        cancelHeld = 0;
        return 0;
    }
    if (cancelHeld) return 0;
    cancelHeld = 1;
    return 1;
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

    patch[0] = 0x8b; patch[1] = 0x0d; patch[2] = 0xec; patch[3] = 0xbb; patch[4] = 0x46; patch[5] = 0x00;

    patch[6] = 0x51;

    patch[7] = 0xe8;
    rel = (DWORD)(uintptr_t)&TitleCancelHook - (addrA + 7 + 5);
    memcpy(&patch[8], &rel, 4);

    patch[12] = 0x83; patch[13] = 0xc4; patch[14] = 0x04;

    patch[15] = 0x84; patch[16] = 0xc0;

    patch[17] = 0x0f; patch[18] = 0x84;
    rel = 0x42355a - (addrA + 17 + 6);
    memcpy(&patch[19], &rel, 4);

    if (!VirtualProtect(pa, 0x40, PAGE_EXECUTE_READWRITE, &old)) {
        Log("TitleCancel: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    memcpy(pa, patch, sizeof(patch));

    rel = 0x423447 - 0x42342c;
    memcpy((unsigned char*)0x423428, &rel, 4);
    VirtualProtect(pa, 0x40, old, &old);
    FlushInstructionCache(GetCurrentProcess(), pa, 0x40);
    Log("TitleCancel: patched (hook=%p)", (void*)&TitleCancelHook);
}

static const int g_OptionDefaultKey = 1;

typedef unsigned char (__attribute__((fastcall)) *PF_OptionUpdate)(void *opt);
typedef void (__attribute__((thiscall)) *PF_SetKeyRow)(void *opt, int row, int pad, int type, int key);
typedef void (__attribute__((thiscall)) *PF_PlaySe)(void *se, int a, int b);
typedef void (__cdecl *PF_SetVolume)(int v);
typedef unsigned char (__attribute__((thiscall)) *PF_InputHold)(void *self, int id, int delay);
typedef void (__attribute__((thiscall)) *PF_OptionDraw)(void *opt);
typedef void (__attribute__((thiscall)) *PF_OptionRowCtor)(void *row);
typedef void (__attribute__((thiscall)) *PF_OptionRowSetup)(void *row, void *tex, int x, int y, int a, int b, int w, int h, int c);
typedef void (__cdecl *PF_OptionRowText)(void *row, int a, int b, int c, int d, int e, const char *fmt, ...);
typedef void (__cdecl *PF_OptionRowDraw)(void *row, int flag);

#define OPT_F(o, off) (*(int*)((char*)(o) + (off)))

static int OptionVolume(int off, int vol);
static int HookCall(DWORD callAddr, DWORD target, void *hook);

static char g_OptionDefaultRow[0xe8] __attribute__((aligned(16)));
static void *g_OptionDefaultTex;    /* private 256x256 text surface of the Default row */
static void *g_OptionDefaultScene;  /* first texture of the option screen the row was built for */
static int g_OptionDefaultRowInit;
typedef void *(__cdecl *PF_CreateTexture)(int w, int h, const char *name, int flag);

static void OptionApplyDefaults(void *opt, int page) {
    PF_PlaySe playSe = (PF_PlaySe)(uintptr_t)0x412210;
    if (page == 1) {
        OPT_F(opt, 0x3314) = 4;
        OPT_F(opt, 0x3318) = 3;
        OPT_F(opt, 0x331c) = 1;
        OPT_F(opt, 0x3320) = 0;
        OPT_F(opt, 0x3324) = 100;
        OPT_F(opt, 0x3328) = 0;
        OPT_F(opt, 0x332c) = 80;
        ((PF_SetVolume)(uintptr_t)0x4117c0)(OptionVolume(OPT_F(opt, 0x3320), OPT_F(opt, 0x3324)));
        ((PF_SetVolume)(uintptr_t)0x4120e0)(OptionVolume(OPT_F(opt, 0x3328), OPT_F(opt, 0x332c)));
    } else if (page == 2) {
        PF_SetKeyRow setRow = (PF_SetKeyRow)(uintptr_t)0x41bf60;
        int i;
        for (i = 0; i < 4; i++) setRow(opt, i, 0, i + 1, 0);
        for (i = 4; i < 8; i++) setRow(opt, i, 0, 0, i - 4);
    }
    playSe((void*)(uintptr_t)0x4ea3a8, 0, 0xff);
}

/*
 * The Default row is a text row like those of the replay menu (0x421350): it needs a 256x256 text
 * surface created by the game's 0x4097b0 (width, height, name 0x45b1c4, flag 1).  Option object
 * layout: +0x00/+0x04/+0x08/+0x0c are the four label textures loaded from the archive and +0x10 is an
 * embedded font object (ctor 0x407ec0, init 0x407fa0, dtor 0x407f30) - not a texture, so it must never
 * be handed to the row setup (0x41ff10) / text (0x41ff80) functions.
 * The option screen is rebuilt (textures re-created by 0x41ad00) every time the title screen is built,
 * so the row and its surface are rebuilt whenever +0x00 changes.
 */
static void OptionDefaultDrawText(void *opt) {
    void *scene = *(void**)opt;
    int selected = OPT_F(opt, 0x3310) == 7;
    if (!scene) return;
    if (!g_OptionDefaultRowInit || g_OptionDefaultScene != scene) {
        void *tex = ((PF_CreateTexture)(uintptr_t)0x4097b0)(0x100, 0x100, (const char*)(uintptr_t)0x45b1c4, 1);
        if (!tex) {
            Log("OptionDefault: text surface creation failed");
            return;
        }
        g_OptionDefaultRowInit = 1;
        g_OptionDefaultScene = scene;
        g_OptionDefaultTex = tex;
        memset(g_OptionDefaultRow, 0, sizeof(g_OptionDefaultRow));
        ((PF_OptionRowCtor)(uintptr_t)0x41fdf0)(g_OptionDefaultRow);
        ((PF_OptionRowSetup)(uintptr_t)0x41ff10)(g_OptionDefaultRow, tex, 0xd4, 0x14c, 0, 0, 0x100, 0x100, 0);
        ((PF_OptionRowText)(uintptr_t)0x41ff80)(g_OptionDefaultRow, 0x10, 0xc, 8, 9, 0xb, "Default");
        *(int*)(g_OptionDefaultRow + 0xac) = 0x3f4ccccd;
        *(int*)(g_OptionDefaultRow + 0xb0) = 0x3f4ccccd;
        Log("OptionDefault: Default row built (scene=%p tex=%p)", scene, tex);
    }
    *(int*)(g_OptionDefaultRow + 0x20) = 0xff;   /* alpha: the option screen sets it per frame, 0xff once faded in */
    *(int*)(g_OptionDefaultRow + 0xd8) = selected ? 0xffffff : 0xa0a0a0;
    ((PF_OptionRowDraw)(uintptr_t)0x420270)(g_OptionDefaultRow, 0);
}

/*
 * Preferred way to show the Default row: the other labels of the option screen are cut out of one
 * 256x256 picture (option+4, 8 labels of 128x32 at x=0).  The right half of it is unused, so
 * "Default" is rendered there with GDI (Arial, white, anti-aliased) and drawn as a normal label row
 * (same blending, alpha and position handling as the others).  If the picture cannot be written the
 * game's own text row (OptionDefaultDrawText) is used instead.
 */
typedef void (__attribute__((thiscall)) *PF_OptionRowDrawT)(void *row);

static char g_OptionLabelRow[0xe8] __attribute__((aligned(16)));
static void *g_OptionLabelScene;
static int g_OptionLabelMode;     /* 0: not built, 1: label in the atlas, 2: fallback to the text row */
static int g_optPrevEnter;

static DWORD OptPackMask(DWORD mask, unsigned v8) {
    unsigned shift = 0, bits = 0;
    if (!mask) return 0;
    while (shift < 32 && !((mask >> shift) & 1)) shift++;
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1)) bits++;
    if (bits > 8) bits = 8;
    if (!bits) return 0;
    return ((DWORD)(v8 >> (8 - bits)) << shift) & mask;
}

static int OptionRenderDefaultLabel(void *tex, int rx, int ry) {
    enum { LW = 128, LH = 32 };
    IDirectDrawSurface7 *surf;
    DDSURFACEDESC2 d;
    BITMAPINFO bi;
    HDC dc;
    HBITMAP bmp, oldBmp;
    HFONT font, oldFont = NULL;
    unsigned char *bits = NULL;
    int x, y, bytes, bpp, ok = 0, nonEmpty = 0;
    DWORD rm, gm, bm, am;

    if (!tex || IsBadReadPtr(tex, 4)) return 0;
    surf = *(IDirectDrawSurface7**)tex;
    if (!surf || IsBadReadPtr(surf, 4)) return 0;

    dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = LW;
    bi.bmiHeader.biHeight = -LH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return 0;
    }
    oldBmp = (HBITMAP)SelectObject(dc, bmp);
    memset(bits, 0, LW * LH * 4);
    font = CreateFontA(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, "Arial");
    if (font) oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 1, 6, "Default", 7);
    GdiFlush();

    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(surf, NULL, &d, DDLOCK_WAIT, NULL))) {
        bpp = (int)d.ddpfPixelFormat.dwRGBBitCount;
        rm = d.ddpfPixelFormat.dwRBitMask;
        gm = d.ddpfPixelFormat.dwGBitMask;
        bm = d.ddpfPixelFormat.dwBBitMask;
        am = (d.ddpfPixelFormat.dwFlags & DDPF_ALPHAPIXELS) ? d.ddpfPixelFormat.dwRGBAlphaBitMask : 0;
        bytes = bpp / 8;
        if ((d.ddpfPixelFormat.dwFlags & DDPF_RGB) && (bpp == 16 || bpp == 32) && d.lpSurface &&
            (!d.dwWidth || d.dwWidth >= (DWORD)(rx + LW)) && (!d.dwHeight || d.dwHeight >= (DWORD)(ry + LH))) {
            for (y = 0; y < LH && !nonEmpty; y++) {
                unsigned char *p = (unsigned char*)d.lpSurface + (size_t)(ry + y) * d.lPitch + (size_t)rx * bytes;
                for (x = 0; x < LW; x++, p += bytes) {
                    DWORD v = bytes == 4 ? *(DWORD*)p : *(WORD*)p;
                    if (am ? (v & am) : v) { nonEmpty = 1; break; }
                }
            }
            if (!nonEmpty) {
                for (y = 0; y < LH; y++) {
                    unsigned char *p = (unsigned char*)d.lpSurface + (size_t)(ry + y) * d.lPitch + (size_t)rx * bytes;
                    for (x = 0; x < LW; x++, p += bytes) {
                        unsigned a = bits[(y * LW + x) * 4 + 1];
                        DWORD pix = 0;
                        if (a) {
                            if (am) pix = OptPackMask(am, a) | OptPackMask(rm, 255) | OptPackMask(gm, 255) | OptPackMask(bm, 255);
                            else    pix = OptPackMask(rm, a) | OptPackMask(gm, a) | OptPackMask(bm, a);
                        }
                        if (bytes == 4) *(DWORD*)p = pix;
                        else            *(WORD*)p = (WORD)pix;
                    }
                }
                ok = 1;
            } else {
                Log("OptionDefault: atlas region (%d,%d) is not empty", rx, ry);
            }
        } else {
            Log("OptionDefault: atlas surface not usable (bpp=%d flags=%08X %lux%lu)", bpp,
                (unsigned)d.ddpfPixelFormat.dwFlags, (unsigned long)d.dwWidth, (unsigned long)d.dwHeight);
        }
        IDirectDrawSurface7_Unlock(surf, NULL);
    } else {
        Log("OptionDefault: atlas surface Lock failed");
    }

    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}

static void OptionDefaultDraw(void *opt) {
    void *scene = *(void**)opt;
    void *atlas = *(void**)((char*)opt + 4);
    int selected = OPT_F(opt, 0x3310) == 7;

    if (!scene) return;
    if (g_OptionLabelScene != scene) {
        g_OptionLabelScene = scene;
        g_OptionLabelMode = 2;
        if (atlas && OptionRenderDefaultLabel(atlas, 0x80, 0)) {
            memset(g_OptionLabelRow, 0, sizeof(g_OptionLabelRow));
            ((PF_OptionRowCtor)(uintptr_t)0x41fdf0)(g_OptionLabelRow);
            ((PF_OptionRowSetup)(uintptr_t)0x41ff10)(g_OptionLabelRow, atlas, 0xe4, 0x152, 0x80, 0, 0x80, 0x20, 0);
            g_OptionLabelMode = 1;
            Log("OptionDefault: Default label rendered into the option atlas %p", atlas);
        } else {
            Log("OptionDefault: atlas label unavailable, using the game's text row");
        }
    }
    if (g_OptionLabelMode == 1) {
        OPT_F(g_OptionLabelRow, 0x20) = selected ? 0xff : 0x66;   /* same alphas as the other labels */
        ((PF_OptionRowDrawT)(uintptr_t)0x4200a0)(g_OptionLabelRow);
    } else {
        OptionDefaultDrawText(opt);
    }
}

static int OptionVolume(int off, int vol) {
    double v = off ? *(double*)(uintptr_t)0x453cc0 : (double)vol * *(double*)(uintptr_t)0x453cb8;
    return (int)v;
}

static unsigned char __attribute__((fastcall)) OptionUpdateHook(void *opt) {
    void *input = *(void**)(uintptr_t)0x46bbec;
    int page = OPT_F(opt, 0x3308);
    int capturing = *(volatile unsigned char*)(uintptr_t)0x4e9240;
    int virtualCursor = OPT_F(opt, 0x3310);
    int up = 0, down = 0, chooseDefault = 0, confirmHeld = 0, enterNow = 0;
    PF_InputHold hold = (PF_InputHold)(uintptr_t)0x431480;
    PF_InputKey edge = (PF_InputKey)(uintptr_t)0x431540;

    if (input) {
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        g_titleEscCarryover = rawkey(input, 0x1b) ? 1 : 0;
    }

    if (input && page == 1) {
        PF_InputKey rawkeyEnter = (PF_InputKey)(uintptr_t)0x431500;
        up = hold(input, 0, 0xf) ? 1 : 0;
        down = hold(input, 1, 0xf) ? 1 : 0;
        enterNow = rawkeyEnter(input, 0xd) ? 1 : 0;
        confirmHeld = (edge(input, 4) || enterNow) ? 1 : 0;
        chooseDefault = virtualCursor == 7 && (edge(input, 4) || (enterNow && !g_optPrevEnter));
        g_optPrevEnter = enterNow;
        /* virtual cursor: 0..6 as in the game, 7 = Default, 8 = Exit (the game's row 7).  While Default is
           selected the game is run with its cursor on the Exit row, which has no value to change with
           left/right and highlights nothing we show; on a confirm press it is run on row 6 instead, where
           confirm does nothing, so that the game does not leave the screen. */
        if (virtualCursor == 7) {
            int exitChoices = *(volatile int*)(uintptr_t)(0x453c84 + 7 * 4);
            OPT_F(opt, 0x3310) = (confirmHeld || exitChoices <= 0) ? 6 : 7;
        } else if (virtualCursor >= 8) {
            OPT_F(opt, 0x3310) = 7;
        }
        if (chooseDefault) OptionApplyDefaults(opt, 1);
    }

    if (input && ((page == 1) || (page == 2 && !capturing))) {
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        if (rawkey(input, 0x44)) {
            OptionApplyDefaults(opt, page);
        }
    }
    {
        unsigned char result = ((PF_OptionUpdate)(uintptr_t)0x41b270)(opt);
        if (input && page == 1) {
            int newReal = OPT_F(opt, 0x3310);
            int newV, i;
            if (virtualCursor == 7)
                newV = down ? 8 : (up ? newReal : 7);
            else if (virtualCursor >= 8)
                newV = up ? 7 : (down ? newReal : 8);
            else if (newReal == 7)
                newV = up ? 8 : 7;      /* down from SE Volume enters Default, up from the top wraps to Exit */
            else
                newV = newReal;
            OPT_F(opt, 0x3310) = newV;
            /* label highlight: rows 0..6 as the game, virtual 8 = Exit (row 7), Default is drawn separately */
            for (i = 0; i < 8; i++) OPT_F(opt, 0x3e8 + i * 0xe8 + 0x20) = 0x66;
            if (newV >= 0 && newV <= 6) OPT_F(opt, 0x3e8 + newV * 0xe8 + 0x20) = 0xff;
            else if (newV == 8) OPT_F(opt, 0x3e8 + 7 * 0xe8 + 0x20) = 0xff;
            /* the game's hold-left-on-Exit counter must not run while Default is selected */
            if (newV == 7) *(volatile unsigned char*)(uintptr_t)0x4e9244 = 0;
        }
        return result;
    }
}

static unsigned char __attribute__((fastcall)) OptionDrawHook(void *opt) {
    unsigned char result;
    if (OPT_F(opt, 0x3308) == 1) {
        /* Exit row (row 7, y double at +0x10) one slot lower: Default takes its place */
        *(double*)((char*)opt + 0x3e8 + 7 * 0xe8 + 0x10) = 378.0;
    }
    result = ((PF_OptionUpdate)(uintptr_t)0x41c1d0)(opt);
    if (OPT_F(opt, 0x3308) == 1 && OPT_F(opt, 0x3398) == 0xff)
        OptionDefaultDraw(opt);
    return result;
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
    memcpy(p + 7, &rel, 4);
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    if (!HookCall(0x423f0e, 0x41c1d0, (void*)&OptionDrawHook))
        Log("OptionDefault: draw hook failed");
    {
        /* the page-1 panel (0x4241d0 call at 0x41c2a2: push 0x140 = height 320) grows by one row (40) */
        static const unsigned char panelOrig[5] = { 0x68, 0x40, 0x01, 0x00, 0x00 };
        unsigned char *pp = (unsigned char*)0x41c2a2;
        DWORD oldp;
        if (memcmp(pp, panelOrig, sizeof(panelOrig)) == 0 &&
            VirtualProtect(pp + 1, 4, PAGE_EXECUTE_READWRITE, &oldp)) {
            DWORD h = 0x168;
            memcpy(pp + 1, &h, 4);
            VirtualProtect(pp + 1, 4, oldp, &oldp);
            FlushInstructionCache(GetCurrentProcess(), pp, 5);
            Log("OptionDefault: option panel height 0x140 -> 0x168");
        } else {
            Log("OptionDefault: panel height patch skipped");
        }
    }
    Log("OptionDefault: patched (hook=%p)", (void*)&OptionUpdateHook);
}

static const int g_PauseQRKey = 1;

static const int g_ReplayPause = 1;
static int g_pauseSeen = 0;
static int g_pauseReplay = 0;
static int g_pauseReplayHandled = 0;
static int g_replayFileId = -1;
static int g_replayStage = 1;
static int g_replayFileIdCaptured = 0;

typedef void (__attribute__((fastcall)) *PF_PauseUpdate)(void *menu);
typedef void (__attribute__((thiscall)) *PF_ReplayStop)(void *input);
typedef int (__attribute__((thiscall)) *PF_ReplayLoad)(void *input, int replayId, int stage);
typedef void (__cdecl *PF_SceneSwitch)(int a, int b);

static void PauseReplayRestore(void) {
    if (*(volatile unsigned char*)(uintptr_t)0x4dae16 != 0) return;
    if (g_pauseReplay && !g_pauseReplayHandled) {
        void *input = *(void**)(uintptr_t)0x46bbec;
        if (input) {
            *((volatile unsigned char*)input + 0x234) = 1;
            Log("ReplayPause: pause menu closed, replay playback restored");
        }
    }
    g_pauseSeen = 0;
    g_pauseReplay = 0;
    g_pauseReplayHandled = 0;
}

static void __attribute__((fastcall)) PauseUpdateHook(void *menu) {
    void *input = *(void**)(uintptr_t)0x46bbec;
    if (input && !g_pauseSeen) {
        g_pauseSeen = 1;
        if (g_ReplayPause && *((volatile unsigned char*)input + 0x234)) {
            g_pauseReplay = 1;
            g_pauseReplayHandled = 0;
            *((volatile unsigned char*)input + 0x234) = 0;
            Log("ReplayPause: pause menu opened during replay playback");
        }
    }
    /* menu+0xc is the command input mode flag (set when the 4th item is chosen); the typed letters must not
       act as the Q/R shortcuts while a command is being entered. */
    if (input && menu && OPT_F(menu, 0x538) == 6 && OPT_F(menu, 0xc) == 0) {
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        int next = 0;
        if (rawkey(input, 0x51)) next = 4;
        else if (rawkey(input, 0x52)) next = 5;
        if (next) {
            ((PF_PlaySe)(uintptr_t)0x412210)((void*)(uintptr_t)0x4ea3a8, 0, 0xff);
            OPT_F(menu, 0x538) = next;
        }
    }
    ((PF_PauseUpdate)(uintptr_t)0x419b30)(menu);
    if (g_pauseReplay && !g_pauseReplayHandled && input && menu) {
        int st = OPT_F(menu, 0x538);
        if (st == 4) {
            *((volatile unsigned char*)input + 0x234) = 1;
            ((PF_ReplayStop)(uintptr_t)0x432020)(input);
            g_pauseReplayHandled = 1;
            Log("ReplayPause: title chosen, replay stopped");
        } else if (st == 5) {
            g_pauseReplayHandled = 1;
        }
    }
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
    memcpy(p + 7, &rel, 4);
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("PauseKeys: patched (hook=%p)", (void*)&PauseUpdateHook);
}

static void PatchReplayPause(void) {
    static const unsigned char orig[10] = {
        0x8a,0x81,0x34,0x02,0x00,0x00, 0x84,0xc0, 0x74,0x13
    };
    const DWORD addr = 0x413ccf;
    unsigned char *p = (unsigned char*)addr;
    DWORD old;

    if (!g_ReplayPause) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ReplayPause: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ReplayPause: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p + 8, 1, PAGE_EXECUTE_READWRITE, &old)) {
        Log("ReplayPause: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    p[8] = 0xeb;
    VirtualProtect(p + 8, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("ReplayPause: patched (pause menu opens during replay playback)");
}

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
    memcpy(p + 7, &rel, 4);
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("ResultRetry: patched (hook=%p)", (void*)&ResultDrawHook);
}

static const int g_ReplayStageFix = 1;

typedef void (__attribute__((fastcall)) *PF_ReplayUpdate)(void *obj);

static void __attribute__((fastcall)) ReplayUpdateHook(void *obj) {
    unsigned char *o = (unsigned char*)obj;
    if (o[0x27f4] != 0 && o[0x271e] == 0) {
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
    memcpy(p + 7, &rel, 4);
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    Log("ReplayStage: patched (hook=%p)", (void*)&ReplayUpdateHook);
}

typedef unsigned char (__attribute__((fastcall)) *PF_ReplaySavePromptUpdate)(void *obj);
typedef void (__attribute__((thiscall)) *PF_ReplaySaveInputAction)(void *input);

static unsigned char __attribute__((fastcall)) ReplaySavePromptUpdateHook(void *obj) {
    unsigned char *o = (unsigned char*)obj;
    void *input = *(void**)(uintptr_t)0x46bbec;
    if (input && o[4] && *(int*)(o + 0x494) == 0) {
        PF_InputKey edge = (PF_InputKey)(uintptr_t)0x431540;
        PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
        if (edge(input, 5) || edge(input, 7) || rawkey(input, 0x1b)) {
            /* Run the game's No branch and its normal fade-out sequence. */
            *(int*)(o + 0x488) = 0;
            ((PF_ReplaySaveInputAction)(uintptr_t)0x431880)(input);
            ((PF_ReplaySaveInputAction)(uintptr_t)0x431900)(input);
            ((PF_PlaySe)(uintptr_t)0x412210)((void*)(uintptr_t)0x4ea3a8, 0, 0xff);
            *(int*)(o + 0x48c) = 0x19;
            *(int*)(o + 0x494) = 1;
            Log("ReplaySave: cancel skipped saving");
            return 1;
        }
    }
    return ((PF_ReplaySavePromptUpdate)(uintptr_t)0x41a120)(obj);
}

static void PatchReplaySaveCancel(void) {
    int n;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ReplaySave: unexpected image base, skipped");
        return;
    }
    n = HookCall(0x41a21e, 0x41a120, (void*)&ReplaySavePromptUpdateHook);
    Log("ReplaySave: cancel hook %s", n ? "installed" : "failed");
}

static const int g_ReplayFpuFix = 1;
static const unsigned short g_ReplayFpuCw = 0x007F;
static int g_fpuLogLeft = 40;

static unsigned short FpuGetCw(void) {
    unsigned short cw = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return cw;
}

static void FpuSetCw(unsigned short cw) {
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

static void FpuEnforce(const char *where, int frame) {
    unsigned short cur, want = g_ReplayFpuCw;
    if (!g_ReplayFpuFix) return;
    cur = FpuGetCw();
    if (frame == 0)
        Log("FpuFix: %s frame 0: x87 control word %04X (target %04X)", where, (unsigned)cur, (unsigned)want);
    if (cur != want) {
        FpuSetCw(want);
        if (g_fpuLogLeft > 0) {
            g_fpuLogLeft--;
            Log("FpuFix: %s: x87 control word %04X -> %04X", where, (unsigned)cur, (unsigned)want);
        }
    }
}

typedef unsigned char (__attribute__((thiscall)) *PF_ReplayInputQuery)(void *self, int id,
                                                                       int delay);
static PF_ReplayInputQuery g_replayInputQueryOrig = NULL;
static unsigned g_replayInputQueryLogRemaining = 0;

static unsigned char __attribute__((fastcall)) ReplayInputQueryHook(void *self, void *edx,
                                                                     int id, int delay) {
    unsigned char result;
    int playback = self && *(volatile unsigned char*)((char*)self + 0x234);
    unsigned buttons = playback ? *(volatile unsigned*)((char*)self + 4) : 0;
    unsigned index = playback ? *(volatile unsigned*)((char*)self + 0x11a8) : 0;

    (void)edx;
    FpuEnforce("input", -1);
    PauseReplayRestore();
    result = g_replayInputQueryOrig(self, id, delay);
    if (playback && g_replayInputQueryLogRemaining) {
        Log("ReplayInput: frame=%d cursor=%u mask=%04X query=%d delay=%d result=%u",
            *(volatile int*)(uintptr_t)0x4dadf8, index, buttons & 0xffff, id, delay,
            (unsigned)result);
        --g_replayInputQueryLogRemaining;
    }
    return result;
}

static void PatchReplayInputTrace(void) {
    static const unsigned char orig[5] = { 0x55,0x56,0x57,0x8b,0xf9 };
    const DWORD addr = 0x431480;
    unsigned char *p = (unsigned char*)(uintptr_t)addr;
    unsigned char *tramp;
    DWORD old, rel;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ReplayInput: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("ReplayInput: code signature mismatch, skipped");
        return;
    }
    tramp = (unsigned char*)VirtualAlloc(NULL, 10, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("ReplayInput: trampoline allocation failed (err=%08X)", GetLastError());
        return;
    }
    memcpy(tramp, orig, sizeof(orig));
    tramp[5] = 0xe9;
    rel = (DWORD)(addr + 5 - ((DWORD)(uintptr_t)tramp + 10));
    memcpy(tramp + 6, &rel, 4);
    g_replayInputQueryOrig = (PF_ReplayInputQuery)(uintptr_t)tramp;
    if (!VirtualProtect(p, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
        g_replayInputQueryOrig = NULL;
        VirtualFree(tramp, 0, MEM_RELEASE);
        Log("ReplayInput: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    p[0] = 0xe9;
    rel = (DWORD)(uintptr_t)&ReplayInputQueryHook - (addr + 5);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, sizeof(orig), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(orig));
    g_replayInputQueryLogRemaining = 500;
    Log("ReplayInput: query trace hook installed");
}

static const int g_PracticeStage2Power = 5;
static const int g_PracticeStage3Power = 7;
static int g_exArea = 0;
static int g_exAreaChoice = 0;
static int g_exRun = 0;
static int g_exBossSelected = 0;
static int g_exBossRun = 0;
static int g_exBoss2Kill = 0;         /* Boss 2 practice: defeat Boss 1 at once (see ExBoss2Step) */
static int g_exBoss2Transition = 0;   /* suppress dialogue until Boss 2's battle BGM starts */
static int g_exBoss2KillFrames = 0;
static int g_exBoss1Defeated = 0;
static int g_exBoss2BgmSeen = 0;      /* Boss 2's battle BGM has started (VM bgm op seen outside the fast-forward) */
static int g_exBoss2BgmFrame = 0;
static int g_exBoss2TransitionFastForwarded = 0;
static int g_exBoss1DonePending = 0;
static int g_exBoss2ScoreReset = 0;
static int g_exBossReturnFF = 0;      /* boss practice: the practice boss vanished; fast-forward to the clear op */
static int g_exBossClearSim = 0;      /* the stage clear opcode was called while simulating */
static int g_exBoss2Logged = 0;
static unsigned *g_exBoss1Obj = NULL;   /* the Boss 1 object being defeated (so that Boss 2 is never touched) */

typedef void (__stdcall *PF_GameStart)(int stage);

static void ApplyPracticePower(int stage, const char *what) {
    int p;
    if (stage == 2 || stage == 3) {
        if (*(volatile unsigned char*)(uintptr_t)0x4dade0 == 0) return;
        p = (stage == 2) ? g_PracticeStage2Power : g_PracticeStage3Power;
    } else if (stage == 4 && g_exRun && (g_exArea >= 1 || g_exBossSelected > 0)) {
        p = 7;
    } else {
        return;
    }
    if (p < 0) return;
    if (p > 7) p = 7;
    *(volatile int*)(uintptr_t)0x4dadd0 = p;
    *(volatile int*)(uintptr_t)0x4dadd4 =
        (p > 0) ? *(volatile int*)(uintptr_t)(0x45ae00 + 4 * p) : 0;
    Log("PracticePower: stage %d %s, power=%d counter=%d", stage, what, p,
        *(volatile int*)(uintptr_t)0x4dadd4);
}

#define EX_MAX_AREAS 16
#define EX_AREA_N 4
#define EX_BGM_IDS 16
#define EX_MAIN_WAIT_IP 810192
static const int g_ExAreaSelect = 1;

static const int g_ExBgMode = 1;

static const int g_ExBgDebugKeys = 0;
static const int g_exAreaBgm[EX_AREA_N] = { 2, 3, 4, 5 };
static const int g_exAreaDefaultFrame[EX_AREA_N] = { 362, 6612, 8962, 14192 };
static const int g_exAreaEndFrame[EX_AREA_N] = { 6612, 8962, 14192, 19074 };
static const int g_exBossBgm[2] = { 0, 1 };
static const int g_exBossDefaultFrame[2] = { 19074, 24017 };
static int g_exBgmTime[EX_BGM_IDS];
static int g_exBossBgmTime[2];
static int g_exAreaN = EX_AREA_N;
static int g_exStartReq = 0;
static int g_exPending = 0;
static int g_exSkipWait = 0;
static int g_exShorten = 0;

static int g_exSimulatingStageFrame = 0;
static int g_exBossRunLogged = 0;
static volatile int g_exMuteSe = 0;   /* 1 while a practice skip fast-forwards: sound effects are not played */
static int g_exBgmMuted = 0;
static int g_exBgmSavedVol = 0;

/* The BGM follows g_exMuteSe: it is silenced while a practice skip fast-forwards and the volume the
   option screen set (0x45ab14, written by 0x4117c0) is restored afterwards. */
static void ExBgmMuteSync(void) {
    if (g_exMuteSe && !g_exBgmMuted) {
        g_exBgmSavedVol = *(volatile unsigned char*)(uintptr_t)0x45ab14;
        ((PF_SetVolume)(uintptr_t)0x4117c0)(0);
        g_exBgmMuted = 1;
        Log("ExArea: BGM muted for the fast-forward (volume %d)", g_exBgmSavedVol);
    } else if (!g_exMuteSe && g_exBgmMuted) {
        g_exBgmMuted = 0;
        ((PF_SetVolume)(uintptr_t)0x4117c0)(g_exBgmSavedVol);
        Log("ExArea: BGM volume restored to %d", g_exBgmSavedVol);
    }
}

typedef unsigned char (__cdecl *PF_Op)(void *thread, void *args);
static PF_Op g_origOp2 = NULL;

static int g_exOpLog = 0;

static unsigned char __cdecl Op2Hook(void *thr, void *args) {
    unsigned char r = g_origOp2(thr, args);
    if (g_exOpLog > 0 && *(int*)((char*)thr + 0x14) >= EX_MAIN_WAIT_IP) {
        g_exOpLog--;
        Log("ExArea: op2 thread=%p ip=%d wait=%d frame=%d", thr, *(int*)((char*)thr + 0x14),
            *(int*)((char*)thr + 0x10), *(volatile int*)(uintptr_t)0x4dadf8);
    }
    if (g_exShorten) {
        int *w = (int*)((char*)thr + 0x10);
        if (*(int*)((char*)thr + 0x14) == EX_MAIN_WAIT_IP && *w == 300) {
            *(float*)((char*)thr + 0x50) = 362.0f;
            *(int*)((char*)thr + 0x14) = 810248 - 12;
            *w = 0;
            g_exShorten = 0;
            Log("ExArea: main timeline intro bypassed (ip -> 810248, thr+0x50 = 362)");
        }
    }
    return r;
}

static void ExArm(void) {
    void **tbl;
    DWORD old;

    memset(g_exBgmTime, 0, sizeof(g_exBgmTime));
    memset(g_exBossBgmTime, 0, sizeof(g_exBossBgmTime));
    g_exSkipWait = 0;
    g_exShorten = 0;
    g_exOpLog = g_exRun ? 40 : 0;
    if (!g_exRun || g_exArea < 0 || g_exArea >= EX_AREA_N) return;
    tbl = *(void***)(uintptr_t)0x4ea1fc;
    if (!tbl) {
        Log("ExArea: opcode table missing, area skip disabled");
        return;
    }
    if (!g_origOp2) {
        if (tbl[2] != (void*)(uintptr_t)0x42ec20) {
            Log("ExArea: opcode 2 handler is %p, area skip disabled", tbl[2]);
            return;
        }
        if (!VirtualProtect(&tbl[2], sizeof(void*), PAGE_READWRITE, &old)) {
            Log("ExArea: VirtualProtect failed (err=%08X), area skip disabled", GetLastError());
            return;
        }
        g_origOp2 = (PF_Op)tbl[2];
        tbl[2] = (void*)Op2Hook;
        VirtualProtect(&tbl[2], sizeof(void*), old, &old);
    }
    g_exShorten = 1;
    g_exSkipWait = 1;
}

static void ExDlgInstall(void);
static void ExClearHookInstall(void);

static void ExOnGameStart(int stage) {
    g_exRun = (stage == 4) && g_exStartReq;
    g_exBossRun = g_exRun ? g_exBossSelected : 0;
    g_exBoss2Kill = 0;
    g_exBoss2Transition = 0;
    g_exBoss1Defeated = 0;
    g_exBoss2BgmSeen = 0;
    g_exBoss2BgmFrame = 0;
    g_exBoss2TransitionFastForwarded = 0;
    g_exBoss1DonePending = 0;
    g_exBoss2ScoreReset = 0;
    g_exBossReturnFF = 0;
    g_exBossClearSim = 0;
    g_exStartReq = 0;
    /* hook the VM before the practice fast-forward so the dialogue is not shown while it runs */
    if (g_exBossRun >= 1) ExDlgInstall();
    if (g_exBossRun >= 1) ExClearHookInstall();
    if (stage == 4) {
        g_exPending = 1;
        ExArm();
    }
}

static void ExOnRetry(int stage) {
    if (stage == 4) {
        g_exPending = 1;
        ExArm();
    }
}

static void __stdcall GameStartHook(int stage) {
    ((PF_GameStart)(uintptr_t)0x423880)(stage);
    ExOnGameStart(stage);
    ApplyPracticePower(stage, "start");
}

static void __stdcall RetryHook(int stage) {
    if (g_pauseReplay) {
        void *input = *(void**)(uintptr_t)0x46bbec;
        if (input && g_replayFileIdCaptured) {
            int ok = ((PF_ReplayLoad)(uintptr_t)0x431fe0)(input, g_replayFileId, g_replayStage);
            Log("ReplayPause: retry reloaded replay id=%d from stage %d (ok=%d)",
                g_replayFileId, g_replayStage, ok);
        } else {
            Log("ReplayPause: retry could not reload replay (input=%p file_id=%d)",
                input, g_replayFileIdCaptured);
            ((PF_SceneSwitch)(uintptr_t)0x4254d0)(1, 0);
        }
        return;
    }
    ((PF_GameStart)(uintptr_t)0x419d10)(stage);
    if (stage == 4 && g_exRun)
        *(volatile int*)(uintptr_t)0x4dadd8 = *(volatile int*)(uintptr_t)0x46c7c4;
    ExOnRetry(stage);
    ApplyPracticePower(stage, "retry");
}

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

static unsigned char __attribute__((naked)) PracticeFlagStub(void) {
    __asm__ volatile(
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "movzbl 0x4dade0, %eax\n\t"
        "testl %eax, %eax\n\t"
        "je 1f\n\t"
        "cmpl $4, 0x4dade4\n\t"
        "jne 1f\n\t"
        "xorl %eax, %eax\n"
        "1:\n\t"
        "popl %edx\n\t"
        "popl %ecx\n\t"
        "ret");
}

static int PatchPracticeFlagRead(DWORD addr) {
    unsigned char *p = (unsigned char*)addr;
    DWORD old, rel, imm;

    memcpy(&imm, p + 1, 4);
    if (p[0] != 0xa0 || imm != 0x4dade0) {
        Log("PracticeFlag: unexpected bytes at 0x%08X, skipped", addr);
        return 0;
    }
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("PracticeFlag: VirtualProtect failed at 0x%08X (err=%08X)", addr, GetLastError());
        return 0;
    }
    rel = (DWORD)(uintptr_t)&PracticeFlagStub - (addr + 5);
    p[0] = 0xe8;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

typedef unsigned char (__cdecl *PF_FrameNoArg)(void);
typedef struct { LONGLONG ticks, maxTicks; unsigned calls; } PerfStat;
static PerfStat g_gamePerf[7];
static const char *g_gamePerfName[7] = {
    "events", "object_update", "effects", "object_checks",
    "animation", "stage_tasks", "game_state"
};

static unsigned char PerfCall(int slot, DWORD target) {
    LARGE_INTEGER a, b;
    unsigned char result;
    QueryPerformanceCounter(&a);
    result = ((PF_FrameNoArg)(uintptr_t)target)();
    QueryPerformanceCounter(&b);
    if (slot >= 0 && slot < 7) {
        LONGLONG dt = b.QuadPart - a.QuadPart;
        g_gamePerf[slot].ticks += dt;
        if (dt > g_gamePerf[slot].maxTicks) g_gamePerf[slot].maxTicks = dt;
        g_gamePerf[slot].calls++;
    }
    return result;
}

static void PerfFlush(int frame) {
    int i;
    if (frame < 0 || frame % 300 != 299) return;
    for (i = 0; i < 7; i++) {
        PerfStat *s = &g_gamePerf[i];
        if (s->calls) {
            Log("GamePerf: frames %d-%d %s calls=%u avg=%.3f max=%.3f ms",
                frame - 299, frame, g_gamePerfName[i], s->calls,
                (double)s->ticks * 1000.0 / (double)g_qpcFreq.QuadPart / s->calls,
                (double)s->maxTicks * 1000.0 / (double)g_qpcFreq.QuadPart);
        }
        memset(s, 0, sizeof(*s));
    }
}

#ifdef SMD_DEBUG_BUILD
/* The collision victim is the player body object *(0x4e9ac0) (created at 0x42a4e5).  Its update handler
   0x42ade0 keeps +0x109 (hitbox) off while the invincibility counter +0x2a8 is above 0 and restores it to 1
   every frame, so a one-shot poke at +0x109 never survives.  We hold +0x2a8 exactly at 0x78 every frame:
   above 0 (hitbox off for the whole frame) but not above 0x78, because the not-yet-spawned-ship branch in
   0x42ade0 (ship dead -> wait for the fire button) is skipped while +0x2a8 > 0x78 and the ship would never
   reappear.  F9 toggles the feature on the fly. */
static void DebugDisablePlayerHitbox(void) {
    static int on = 1, logged = 0, dumpN = 0;
    void *body = *(void **)(uintptr_t)0x4e9ac0;
    if (GetAsyncKeyState(VK_F9) & 1) {
        on = !on;
        Log("Debug: player invincibility now %s", on ? "ON" : "OFF");
    }
    if (on && body && !IsBadReadPtr(body, 0x2ac))
        *(int *)((char *)body + 0x2a8) = 0x78;
    if (!logged && body) {
        Log("Debug: player invincibility enabled (body=%p, +0x2a8=0x78, F9 toggles)", body);
        logged = 1;
    }
    if (body && !IsBadReadPtr(body, 0x2b0) && dumpN < 300 &&
        (*(int *)(uintptr_t)0x4dadf8 % 60) == 0) {
        dumpN++;
        Log("Debug: player state f=%d body=%p +0x109=%u +0x2a8=%d +0x234=%u +0x280=%d ship=%p "
            "sip=%d ship+0x109=%u", *(volatile int *)(uintptr_t)0x4dadf8, body,
            (unsigned)*((unsigned char *)body + 0x109), *(int *)((char *)body + 0x2a8),
            (unsigned)*((unsigned char *)body + 0x234), *(int *)((char *)body + 0x280),
            *(void **)(uintptr_t)0x4e9ad4,
            *(void **)(uintptr_t)0x4e9ad4 ? *(int *)((char *)*(void **)(uintptr_t)0x4e9ad4 + 0x8) : -1,
            *(void **)(uintptr_t)0x4e9ad4 ? (unsigned)*((unsigned char *)*(void **)(uintptr_t)0x4e9ad4 + 0x109) : 0u);
    }
}
#endif

static unsigned char __cdecl PerfObjectUpdate(void) { return PerfCall(1, 0x42daa0); }
static unsigned char __cdecl PerfEffects(void) {
#ifdef SMD_DEBUG_BUILD
    DebugDisablePlayerHitbox();
#endif
    return PerfCall(2, 0x42db70);
}
static unsigned char __cdecl PerfObjectChecks(void) {
#ifdef SMD_DEBUG_BUILD
    DebugDisablePlayerHitbox();
#endif
    return PerfCall(3, 0x42db30);
}static unsigned char __cdecl PerfAnimation(void)    { return PerfCall(4, 0x416540); }
static unsigned char __cdecl PerfStageTasks(void)   { return PerfCall(5, 0x414710); }
static unsigned char __cdecl PerfGameState(void) {
    int frame = *(volatile int*)(uintptr_t)0x4dadf8;
    unsigned char result = PerfCall(6, 0x434740);
    PerfFlush(frame);
    return result;
}

static void PatchGamePerf(void) {
    int n = 0;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("GamePerf: unexpected image base, skipped");
        return;
    }
    n += HookCall(0x413c66, 0x42daa0, (void*)&PerfObjectUpdate);
    n += HookCall(0x413c70, 0x42db70, (void*)&PerfEffects);
    n += HookCall(0x413c7a, 0x42db30, (void*)&PerfObjectChecks);
    n += HookCall(0x413c84, 0x416540, (void*)&PerfAnimation);
    n += HookCall(0x413c89, 0x414710, (void*)&PerfStageTasks);
    n += HookCall(0x413c8e, 0x434740, (void*)&PerfGameState);
    Log("GamePerf: patched %d/6 frame subsystems", n);
}

static int __attribute__((fastcall)) ReplayLoadCaptureHook(void *input, void *edx,
                                                            int replayId, int stage) {
    (void)edx;
    g_replayFileId = replayId;
    g_replayStage = (stage == 4) ? 4 : 1;
    g_replayFileIdCaptured = 1;
    Log("ReplayLoad: selected id=%d stage=%d", replayId, stage);
    return ((PF_ReplayLoad)(uintptr_t)0x431fe0)(input, replayId, stage);
}

static void PatchReplayLoadCapture(void) {
    int n;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ReplayLoad: unexpected image base, skipped");
        return;
    }
    n = HookCall(0x4224a1, 0x431fe0, (void*)&ReplayLoadCaptureHook);
    Log("ReplayLoad: selection hook %s", n ? "installed" : "failed");
}

static void PatchPracticePower(void) {
    int n = 0;

    if (g_PracticeStage2Power < 0 && g_PracticeStage3Power < 0) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("PracticePower: unexpected image base, skipped");
        return;
    }
    n += HookCall(0x42377f, 0x423880, (void*)&GameStartHook);
    n += PatchPracticeFlagRead(0x413b9a);
    n += PatchPracticeFlagRead(0x417df2);
    n += HookCall(0x419c9e, 0x419d10, (void*)&RetryHook);
    n += HookCall(0x419cad, 0x419d10, (void*)&RetryHook);
    n += HookCall(0x419cb9, 0x419d10, (void*)&RetryHook);
    Log("PracticePower: patched %d/4 call sites", n);
}

typedef unsigned char (__attribute__((thiscall)) *PF_InputHold)(void *self, int id, int delay);
typedef unsigned char (__attribute__((fastcall)) *PF_PracticeFn)(void *obj);
typedef void (__attribute__((thiscall)) *PF_RowCtor)(void *row);
typedef void (__attribute__((thiscall)) *PF_RowSetup)(void *row, void *tex, int x, int y, int a, int b,
                                                     int w, int h, int c);
typedef void (__cdecl *PF_RowText)(void *row, int a, int b, int c, int d, int e, const char *fmt, ...);
typedef void (__cdecl *PF_RowDraw)(void *row, int flag);
typedef unsigned char (__cdecl *PF_EventDispatch)(int frame);
typedef void (__cdecl *PF_Free)(void *p);

static char g_exRowBuf[EX_MAX_AREAS][0xe8] __attribute__((aligned(16)));
static char g_exLabelRow[0xe8] __attribute__((aligned(16)));
static int g_exRowInit[EX_MAX_AREAS];
static void *g_exRowTex[EX_MAX_AREAS];
static int g_exLabelInit;
static void *g_exLabelTex;
static int g_exLastFade = 0;

static void ExRowsReset(void) {
    memset(g_exRowInit, 0, sizeof(g_exRowInit));
    memset(g_exRowTex, 0, sizeof(g_exRowTex));
    g_exLabelInit = 0;
    g_exLabelTex = NULL;
}

static int ExRowAvailable(void) {
    return g_ExAreaSelect &&
           *(volatile int*)(uintptr_t)0x46bc2c == 3 &&
           *(volatile int*)(uintptr_t)0x46bc00 != 0;
}

static void ExRowEnsure(void *obj, int area) {
    char *row = g_exRowBuf[area];
    void *tex = *(void**)((char*)obj + 0x10);
    if (g_exRowInit[area] && g_exRowTex[area] == tex) return;
    g_exRowInit[area] = 1;
    g_exRowTex[area] = tex;
    memset(row, 0, 0xe8);
    ((PF_RowCtor)(uintptr_t)0x41fdf0)(row);
    ((PF_RowSetup)(uintptr_t)0x41ff10)(row, *(void**)((char*)obj + 0x10), 0x120, 0xf8, 0, 0, 0x100, 0x100, 0);
    if (area < EX_AREA_N)
        ((PF_RowText)(uintptr_t)0x41ff80)(row, 0x10, 0xc, 8, 9, 0xb, "< Area %d/%d >", area + 1, g_exAreaN);
    else
        ((PF_RowText)(uintptr_t)0x41ff80)(row, 0x10, 0xc, 8, 9, 0xb, "Boss %d/2", area - EX_AREA_N + 1);
    *(int*)(row + 0xac) = 0x3f4ccccd;
    *(int*)(row + 0xb0) = 0x3f4ccccd;
    *(int*)(row + 0xd8) = 0x505050;
}

static void ExLabelEnsure(void *obj) {
    void *tex = *(void**)((char*)obj + 0x10);
    if (g_exLabelInit && g_exLabelTex == tex) return;
    g_exLabelInit = 1;
    g_exLabelTex = tex;
    memset(g_exLabelRow, 0, sizeof(g_exLabelRow));
    ((PF_RowCtor)(uintptr_t)0x41fdf0)(g_exLabelRow);
    ((PF_RowSetup)(uintptr_t)0x41ff10)(g_exLabelRow, tex, 0x6c, 0xd8, 0, 0, 0x100, 0x100, 0);
    ((PF_RowText)(uintptr_t)0x41ff80)(g_exLabelRow, 0x10, 0xc, 8, 9, 0xb,
                                     "Extra      %3d     %3d", 0,
                                     *(volatile int*)(uintptr_t)0x46c7c4);
    *(int*)(g_exLabelRow + 0xac) = 0x3f4ccccd;
    *(int*)(g_exLabelRow + 0xb0) = 0x3f4ccccd;
    *(int*)(g_exLabelRow + 0xd8) = 0x505050;
}

static unsigned char __attribute__((fastcall)) PracticeUpdateHook(void *obj) {
    char *o = (char*)obj;
    int *cur = (int*)(o + 8), *fade = (int*)(o + 0xc);
    void *input = *(void**)(uintptr_t)0x46bbec;
    char *scene = *(char**)(uintptr_t)0x4e93c8;
    PF_InputKey edge = (PF_InputKey)(uintptr_t)0x431540;
    PF_InputKey rawkey = (PF_InputKey)(uintptr_t)0x431500;
    PF_InputHold hold = (PF_InputHold)(uintptr_t)0x431480;
    PF_PlaySe playSe = (PF_PlaySe)(uintptr_t)0x412210;
    const int rows = 4;
    int i, v;

    if (!ExRowAvailable())
        return ((PF_PracticeFn)(uintptr_t)0x420c90)(obj);
    if (*cur < 0 || *cur >= rows) *cur = 0;

    if (edge(input, 4) || rawkey(input, 0xd)) {
        *(int*)(scene + 0xc) = 0;
        if (*cur < 3) {
            *(int*)(scene + 0x10) = *cur + 1;
            *(volatile unsigned char*)(uintptr_t)0x4dade0 = 1;
            *(volatile int*)(uintptr_t)0x4dadd8 = *(int*)(uintptr_t)(0x46bc14 + 4 * *cur) - 1;
            g_exStartReq = 0;
            g_exBossSelected = 0;
        } else {
            *(int*)(scene + 0x10) = 4;
            *(volatile unsigned char*)(uintptr_t)0x4dade0 = 1;
            *(volatile int*)(uintptr_t)0x4dadd8 = *(volatile int*)(uintptr_t)0x46c7c4;
            g_exStartReq = 1;
            g_exBossSelected = (g_exAreaChoice >= EX_AREA_N) ? g_exAreaChoice - EX_AREA_N + 1 : 0;
            if (!g_exBossSelected) g_exArea = g_exAreaChoice;
            if (g_exBossSelected)
                Log("ExArea: Boss %d practice start", g_exBossSelected);
            else
                Log("ExArea: Extra practice start, area %d/%d", g_exArea + 1, g_exAreaN);
        }
        *(int*)(scene + 8) = 0xb;
    }
    if (hold(input, 5, 0) || hold(input, 7, 0))
        *(int*)(scene + 8) = 2;
    if (hold(input, 0, 0xf)) {
        *cur = (rows + *cur - 1) % rows;
        playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
    }
    if (hold(input, 1, 0xf)) {
        *cur = (*cur + 1) % rows;
        playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
    }
    if (*cur == 3) {
        if (hold(input, 2, 0xf)) {
            g_exAreaChoice = (g_exAreaChoice + g_exAreaN + 1) % (g_exAreaN + 2);
            if (g_exAreaChoice < g_exAreaN) g_exArea = g_exAreaChoice;
            playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
        }
        if (hold(input, 3, 0xf)) {
            g_exAreaChoice = (g_exAreaChoice + 1) % (g_exAreaN + 2);
            if (g_exAreaChoice < g_exAreaN) g_exArea = g_exAreaChoice;
            playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
        }
    }
    for (i = 0; i < 3; i++) *(int*)(o + 0x1d8 + 0xe8 * i) = 0x505050;
    if (*cur < 3) *(int*)(o + 0x1d8 + 0xe8 * *cur) = 0xffffff;
    v = *fade + 4;
    *fade = v > 0x80 ? 0x80 : v;
    return 1;
}

static unsigned char __attribute__((fastcall)) PracticeDrawHook(void *obj) {
    volatile int *count = (volatile int*)(uintptr_t)0x46bc2c;
    unsigned char r;
    int saved = *count;
    int fade = *(int*)((char*)obj + 0xc);

    if (fade < g_exLastFade) ExRowsReset();
    g_exLastFade = fade;
    if (!ExRowAvailable())
        return ((PF_PracticeFn)(uintptr_t)0x420de0)(obj);
    *count = 5;
    r = ((PF_PracticeFn)(uintptr_t)0x420de0)(obj);
    *count = saved;
    if (*(int*)((char*)obj + 0xc) * 6 > 0xff) {
        char *row;
        ExLabelEnsure(obj);
        ExRowEnsure(obj, g_exAreaChoice);
        row = g_exRowBuf[g_exAreaChoice];
        *(int*)(g_exLabelRow + 0xd8) = (*(int*)((char*)obj + 8) == 3) ? 0xffffff : 0x505050;
        *(int*)(row + 0xd8) = (*(int*)((char*)obj + 8) == 3) ? 0xffffff : 0x505050;
        ((PF_RowDraw)(uintptr_t)0x420270)(g_exLabelRow, 0);
        ((PF_RowDraw)(uintptr_t)0x420270)(row, 0);
    }
    return r;
}

static void ExLogQueue(void) {
    unsigned *n = *(unsigned**)(uintptr_t)0x4ea1d8;
    int cnt[8] = { 0 }, tmin[8], tmax[8], i;

    for (i = 0; i < 8; i++) { tmin[i] = 0x7fffffff; tmax[i] = -1; }
    Log("ExArea: --- Extra event queue (time type args) ---");
    for (; n; n = (unsigned*)n[0]) {
        int t = (int)n[1], ty = (int)n[2];
        unsigned *a = (unsigned*)n[3];
        if (ty < 0 || ty > 7) ty = 7;
        cnt[ty]++;
        if (t < tmin[ty]) tmin[ty] = t;
        if (t > tmax[ty]) tmax[ty] = t;
        if (ty != 0)
            Log("ExArea: ev t=%d type=%d args=%08X %08X %08X %08X %08X %08X", t, ty,
                a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0, a ? a[3] : 0, a ? a[4] : 0, a ? a[5] : 0);
    }
    for (i = 0; i < 8; i++)
        if (cnt[i])
            Log("ExArea: type %d: %d event(s), time %d .. %d%s", i, cnt[i], tmin[i], tmax[i],
                i == 0 ? " (spawns not listed)" : "");
}

static void ExSkipTo(int target, int area) {
    unsigned **headp = (unsigned**)(uintptr_t)0x4ea1d8;
    unsigned *prev = NULL, *n = *headp;
    const int dropOldBg = 0;
    int dropped = 0, bgDropped = 0, spawnLogged = 0;
    unsigned layerKey[8];
    int layerLast[8], nLayers = 0, li;

    for (n = *headp; n; n = (unsigned*)n[0]) {
        unsigned *a = (unsigned*)n[3];
        if (!dropOldBg || (int)n[2] != 2 || (int)n[1] >= target || !a) continue;
        for (li = 0; li < nLayers; li++)
            if (layerKey[li] == a[3]) break;
        if (li == nLayers) {
            if (nLayers >= 8) continue;
            layerKey[nLayers] = a[3];
            layerLast[nLayers] = (int)n[1];
            nLayers++;
        } else if ((int)n[1] > layerLast[li]) {
            layerLast[li] = (int)n[1];
        }
    }
    n = *headp;

    {
        /* If no BGM event starts exactly at the target (the boss area), keep the BGM that would be
           playing there: take the latest earlier BGM event and move it to the target frame. */
        unsigned *q, *keepBgm = NULL;
        int bestT = -1, exact = 0;
        for (q = *headp; q; q = (unsigned*)q[0]) {
            if ((int)q[2] != 6) continue;
            if ((int)q[1] == target) exact = 1;
            else if ((int)q[1] < target && (int)q[1] > bestT) { bestT = (int)q[1]; keepBgm = q; }
        }
        if (!exact && keepBgm) {
            Log("ExArea: BGM event at t=%d kept and moved to the target frame %d", bestT, target);
            keepBgm[1] = (unsigned)target;
        }
    }

    while (n) {
        unsigned *next = (unsigned*)n[0];
        int t = (int)n[1];
        int ty = (int)n[2];
        int drop = 0;

        if (t < target && (ty == 0 || ty == 6)) drop = 1;
        if (drop && ty == 0 && n[3] && !IsBadReadPtr((void*)(uintptr_t)n[3], 24) && spawnLogged < 800) {
            unsigned *sa = (unsigned*)(uintptr_t)n[3];
            spawnLogged++;
            Log("ExArea: dropped spawn t=%d args=%08X %08X %08X %08X %08X %08X", t,
                sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]);
        }

        if (!drop && ty == 2 && t < target && n[3]) {
            unsigned *a = (unsigned*)n[3];
            for (li = 0; li < nLayers; li++) {
                if (layerKey[li] == a[3]) {
                    if (t < layerLast[li]) {
                        drop = 1;
                        bgDropped++;
                        Log("ExArea: dropped old background push t=%d layer=%08X tex=%08X", t, a[3], a[5]);
                    }
                    break;
                }
            }
        }

        if (drop) {
            if (prev) prev[0] = (unsigned)(uintptr_t)next;
            else *headp = next;
            ((PF_Free)(uintptr_t)0x436b1d)(n);
            dropped++;
        } else {
            prev = n;
        }
        n = next;
    }
    Log("ExArea: skipped to frame %d (area %d), removed %d event(s) (%d old background push(es))",
        target, area + 1, dropped, bgDropped);
}

static void ExLogProgress(int frame) {
    unsigned *n = *(unsigned**)(uintptr_t)0x4ea1d8;
    int total = 0, bgm = 0;

    for (; n; n = (unsigned*)n[0]) {
        total++;
        if (n[2] == 6) bgm++;
    }
    Log("ExArea: waiting for BGM event of area %d: frame=%d queue=%d bgm_events=%d",
        g_exArea + 1, frame, total, bgm);
}

static int ExFindBgmTime(int bgmId) {
    unsigned *n = *(unsigned**)(uintptr_t)0x4ea1d8;
    for (; n; n = (unsigned*)n[0]) {
        if (n[2] == 6) {
            unsigned *a = (unsigned*)n[3];
            if (a && (int)a[3] == bgmId) return (int)n[1];
        }
    }
    return 0;
}

static int ExRunStageFrame(int frame) {
    typedef unsigned char (__cdecl *PF_Update)(void);
    unsigned char ok;

    *(volatile int*)(uintptr_t)0x4dadf8 = frame;
    g_exSimulatingStageFrame = 1;
    ok = ((PF_Update)(uintptr_t)0x413c39)();
    g_exSimulatingStageFrame = 0;
    return ok && *(volatile int*)(uintptr_t)0x4dadf8 == frame + 1;
}

static void ExFastForward(int from, int target) {
    int f;

    for (f = from; f < target; f++) {
        if (!ExRunStageFrame(f)) {
            Log("ExArea: normal frame update stopped at %d", f);
            break;
        }
    }
    *(volatile int*)(uintptr_t)0x4dadf8 = target;
    Log("ExArea: simulated frames %d..%d of events + object update", from, target - 1);
}

typedef struct { int type; unsigned w[6]; } ExBgEv;
typedef struct { int n; int time; ExBgEv ev[32]; } ExBgGroup;
static ExBgGroup g_exBgGroup;

typedef void (__cdecl *PF_LayerSetU)(int layer, unsigned v);
typedef void (__cdecl *PF_LayerSetU3)(int layer, unsigned argsP, unsigned tex);
typedef unsigned (__cdecl *PF_TexById)(unsigned id);
typedef void *PF_Void;

static int ExValInt(unsigned tag, unsigned payload, int *ok) {
    if (tag == 0) { float f; memcpy(&f, &payload, 4); return (int)f; }
    if (tag == 1) return (int)payload;
    *ok = 0;
    return 0;
}

static unsigned ExValFloatBits(unsigned tag, unsigned payload, int *ok) {
    if (tag == 0) return payload;
    if (tag == 1) { float f = (float)(int)payload; unsigned b; memcpy(&b, &f, 4); return b; }
    *ok = 0;
    return 0;
}

static int ExBgExtract(int area, ExBgGroup *g) {
    unsigned **headp = (unsigned**)(uintptr_t)0x4ea1d8;
    unsigned *n, *prev = NULL, *next;
    int times[16], nt = 0, i, j, tb;

    g->n = 0;
    g->time = 0;
    if (area < 1) return 0;
    for (n = *headp; n; n = (unsigned*)n[0]) {
        int t = (int)n[1];
        if ((int)n[2] != 2) continue;
        for (i = 0; i < nt && times[i] != t; i++) ;
        if (i == nt && nt < 16) times[nt++] = t;
    }
    for (i = 0; i < nt; i++)
        for (j = i + 1; j < nt; j++)
            if (times[j] < times[i]) { int t = times[i]; times[i] = times[j]; times[j] = t; }
    if (area >= nt) {
        Log("ExArea: background group %d not found (%d group(s) in queue)", area + 1, nt);
        return 0;
    }
    tb = times[area];
    g->time = tb;
    n = *headp;
    while (n) {
        int ty = (int)n[2];
        next = (unsigned*)n[0];
        if ((int)n[1] == tb && ty >= 2 && ty <= 5 && n[3] && g->n < 32) {
            unsigned *a = (unsigned*)n[3];
            g->ev[g->n].type = ty;
            for (i = 0; i < 6; i++) g->ev[g->n].w[i] = a[i];
            g->n++;
            if (prev) prev[0] = (unsigned)(uintptr_t)next;
            else *headp = next;
            ((PF_Free)(uintptr_t)0x436b1d)(n);
        } else {
            prev = n;
        }
        n = next;
    }
    Log("ExArea: background group of area %d: time %d, %d event(s) taken out of the queue", area + 1, tb, g->n);
    return g->n > 0;
}

static void ExQueueClear(int layer) {
    unsigned *sent = *(unsigned**)(uintptr_t)(0x4e9a94 + layer * 12);
    int *cnt = (int*)(uintptr_t)(0x4e9a98 + layer * 12);
    int guard = 0;

    if (!sent) return;
    while (*cnt > 0 && guard++ < 1000) {
        unsigned *node = (unsigned*)sent[0];
        if (!node || node == sent) break;
        ((unsigned*)node[1])[0] = node[0];
        ((unsigned*)node[0])[1] = node[1];
        ((PF_Free)(uintptr_t)0x434fad)(node);
        (*cnt)--;
    }
}

static int ExLayerObjectReset(int layer) {
    unsigned *node;
    int found = 0;

    for (node = *(unsigned**)(uintptr_t)0x4ea1ac; node; node = (unsigned*)node[1]) {
        unsigned *obj = (unsigned*)node[4];
        if (!obj || IsBadReadPtr(obj, 0x824)) continue;
        if ((obj[0x808 / 4] == 0x427e60 || obj[0x81c / 4] == 0x427e60) && (int)obj[2] == layer) {
            Log("ExArea: layer %d object %p: offset %08X slots %08X %08X %08X -> cleared", layer,
                (void*)obj, obj[3], obj[4], obj[5], obj[6]);
            obj[3] = 0;
            obj[4] = obj[5] = obj[6] = 0;
            found++;
        }
    }
    return found;
}

static void ExBgSeed(const ExBgGroup *g) {
    int layers[4], nl = 0, i, j, ok, L;

    for (i = 0; i < g->n; i++) {
        if (g->ev[i].type != 2) continue;
        ok = 1;
        L = ExValInt(g->ev[i].w[2], g->ev[i].w[3], &ok);
        if (!ok || L < 0 || L > 3) continue;
        for (j = 0; j < nl && layers[j] != L; j++) ;
        if (j == nl) layers[nl++] = L;
    }
    for (i = 0; i < nl; i++) {
        int objs;
        ExQueueClear(layers[i]);
        objs = ExLayerObjectReset(layers[i]);
        if (!objs) Log("ExArea: layer %d: update object not found, tile slots left as they are", layers[i]);
    }
    for (i = 0; i < g->n; i++) {
        const ExBgEv *e = &g->ev[i];
        ok = 1;
        L = ExValInt(e->w[2], e->w[3], &ok);
        if (!ok || L < 0 || L > 3) continue;
        if (e->type == 2) {
            int tex = ExValInt(e->w[4], e->w[5], &ok);
            if (ok) {
                ((PF_LayerSetU)(uintptr_t)0x427d40)(L, (unsigned)tex);
                Log("ExArea: layer %d: queued texture %02X", L, tex);
            }
        } else if (e->type == 3 || e->type == 4) {
            unsigned bits = ExValFloatBits(e->w[4], e->w[5], &ok);
            if (ok) ((PF_LayerSetU)(uintptr_t)(e->type == 3 ? 0x428030 : 0x428040))(L, bits);
        } else if (e->type == 5) {
            int v = ExValInt(e->w[4], e->w[5], &ok);
            if (ok) ((PF_LayerSetU)(uintptr_t)0x428050)(L, (unsigned)v);
        }
    }
    for (i = 0; i < nl; i++)
        Log("ExArea: layer %d queue now holds %d tile(s)", layers[i], *(int*)(uintptr_t)(0x4e9a98 + layers[i] * 12));
}

#define EX_BG_GROUPS 8
static const int g_exAreaBgGroup[EX_AREA_N] = { 0, 1, 2, 3 };
static ExBgGroup g_exBgGroups[EX_BG_GROUPS];
static int g_exBgGroupN = 0;
static int g_exBgCur = -1;

static int ExBgSaveAll(int sel) {
    unsigned **headp = (unsigned**)(uintptr_t)0x4ea1d8;
    unsigned *n, *prev = NULL, *next;
    int times[EX_BG_GROUPS], nt = 0, i, j, k;

    g_exBgGroupN = 0;
    g_exBgCur = -1;
    for (n = *headp; n; n = (unsigned*)n[0]) {
        int t = (int)n[1];
        if ((int)n[2] != 2) continue;
        for (i = 0; i < nt && times[i] != t; i++) ;
        if (i == nt && nt < EX_BG_GROUPS) times[nt++] = t;
    }
    for (i = 0; i < nt; i++)
        for (j = i + 1; j < nt; j++)
            if (times[j] < times[i]) { int t = times[i]; times[i] = times[j]; times[j] = t; }
    for (k = 0; k < nt; k++) {
        g_exBgGroups[k].n = 0;
        g_exBgGroups[k].time = times[k];
    }
    g_exBgGroupN = nt;
    n = *headp;
    while (n) {
        int ty = (int)n[2], t = (int)n[1];
        next = (unsigned*)n[0];
        if (ty >= 2 && ty <= 5 && n[3]) {
            for (k = 0; k < nt && times[k] != t; k++) ;
            if (k < nt && g_exBgGroups[k].n < 32) {
                unsigned *a = (unsigned*)n[3];
                ExBgEv *e = &g_exBgGroups[k].ev[g_exBgGroups[k].n++];
                e->type = ty;
                for (i = 0; i < 6; i++) e->w[i] = a[i];
                if (k == sel && sel > 0) {
                    if (prev) prev[0] = (unsigned)(uintptr_t)next;
                    else *headp = next;
                    ((PF_Free)(uintptr_t)0x436b1d)(n);
                    n = next;
                    continue;
                }
            }
        }
        prev = n;
        n = next;
    }
    for (k = 0; k < nt; k++)
        Log("ExArea: background group %d: time %d, %d event(s)%s", k + 1, g_exBgGroups[k].time,
            g_exBgGroups[k].n, (k == sel && sel > 0) ? " (taken out of the queue)" : "");
    return sel > 0 && sel < nt && g_exBgGroups[sel].n > 0;
}

static float ExBitsToFloat(unsigned bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static unsigned ExTextureTableN(void) {
    return *(volatile unsigned*)(uintptr_t)0x46c888;
}

static unsigned ExTextureLoadedN(void) {
    return *(volatile unsigned*)(uintptr_t)0x46c890;
}

static int ExTexId(unsigned p) {
    unsigned n = ExTextureTableN() / 4, i;
    unsigned volatile **tbl;

    if (!p) return -2;
    tbl = (unsigned volatile **)*(void *volatile *)(uintptr_t)0x46c884;
    if (!tbl || IsBadReadPtr(tbl, ExTextureTableN())) return -3;
    for (i = 0; i < n; i++)
        if ((unsigned)(uintptr_t)tbl[i] == p) return (int)i;
    return -1;
}

static char ExPtrLabel(unsigned *seen, int *nseen, unsigned p) {
    int i;
    if (!p) return '-';
    for (i = 0; i < *nseen; i++)
        if (seen[i] == p) return (char)('A' + i);
    if (*nseen >= 20) return '?';
    seen[*nseen] = p;
    return (char)('A' + (*nseen)++);
}

static void ExLogObjects(const char *tag, int frame) {
    unsigned keys[32][6];
    int cnt[32];
    int nk = 0, total = 0, guard = 0, j;
    unsigned *node;

    for (node = *(unsigned**)(uintptr_t)0x4ea1ac; node && guard < 8192; node = (unsigned*)node[1], guard++) {
        unsigned *o;
        unsigned k[6];
        if (IsBadReadPtr(node, 20)) break;
        o = (unsigned*)node[4];
        if (!o || IsBadReadPtr(o, 0x824)) continue;
        total++;
        k[0] = o[0x808 / 4]; k[1] = o[0x80c / 4]; k[2] = o[0x810 / 4];
        k[3] = o[0x814 / 4]; k[4] = o[0x81c / 4]; k[5] = o[0x820 / 4];
        for (j = 0; j < nk; j++)
            if (!memcmp(keys[j], k, sizeof(k))) break;
        if (j == nk) {
            if (nk >= 32) continue;
            memcpy(keys[nk], k, sizeof(k));
            cnt[nk] = 0;
            nk++;
        }
        cnt[j]++;
    }
    Log("ExObj[%s] frame=%d objects=%d kinds=%d", tag, frame, total, nk);
    for (j = 0; j < nk; j++)
        Log("ExObj[%s] frame=%d n=%d f808=%08X f80c=%08X f810=%08X f814=%08X f81c=%08X f820=%08X",
            tag, frame, cnt[j], keys[j][0], keys[j][1], keys[j][2], keys[j][3], keys[j][4], keys[j][5]);
}

static void ExLogLayerState(const char *tag, int frame) {
    int layer;

    for (layer = 0; layer < 4; layer++) {
        unsigned *node, *obj = NULL, *sent, *q;
        unsigned seen[20], qtex[4] = { 0, 0, 0, 0 };
        int nseen = 0, cnt, qn = 0, guard = 0;
        char qs[24];
        char s0, s1, s2;

        for (node = *(unsigned**)(uintptr_t)0x4ea1ac; node && guard < 4096; node = (unsigned*)node[1], guard++) {
            unsigned *o;
            if (IsBadReadPtr(node, 20)) break;
            o = (unsigned*)node[4];
            if (!o || IsBadReadPtr(o, 0x824)) continue;
            if ((o[0x808 / 4] == 0x427e60 || o[0x81c / 4] == 0x427e60) && (int)o[2] == layer) {
                obj = o;
                break;
            }
        }
        if (!obj) continue;
        s0 = ExPtrLabel(seen, &nseen, obj[4]);
        s1 = ExPtrLabel(seen, &nseen, obj[5]);
        s2 = ExPtrLabel(seen, &nseen, obj[6]);
        sent = *(unsigned**)(uintptr_t)(0x4e9a94 + layer * 12);
        cnt = *(int*)(uintptr_t)(0x4e9a98 + layer * 12);
        guard = 0;
        if (sent && !IsBadReadPtr(sent, 12)) {
            for (q = (unsigned*)sent[0]; q && q != sent && qn < 20 && guard++ < 64; q = (unsigned*)q[0]) {
                if (IsBadReadPtr(q, 12)) break;
                if (qn < 4) qtex[qn] = q[2];
                if (qn < (int)sizeof(qs) - 1) qs[qn] = ExPtrLabel(seen, &nseen, q[2]);
                qn++;
            }
        }
        if (qn > (int)sizeof(qs) - 1) qn = (int)sizeof(qs) - 1;
        qs[qn] = 0;
        Log("ExBg[%s] frame=%d layer %d: offset=%.3f slots=%c%c%c tid=%02X,%02X,%02X tex=%08X,%08X,%08X queue(%d)=%s qid=%02X,%02X,%02X,%02X qtex=%08X,%08X,%08X,%08X speed=%.4f accel=%.4f cycle=%u tbl=n%d/l%d",
            tag, frame, layer, ExBitsToFloat(obj[3]), s0, s1, s2,
            ExTexId(obj[4]), ExTexId(obj[5]), ExTexId(obj[6]), obj[4], obj[5], obj[6], cnt, qs,
            ExTexId(qtex[0]), ExTexId(qtex[1]), ExTexId(qtex[2]), ExTexId(qtex[3]),
            qtex[0], qtex[1], qtex[2], qtex[3],
            ExBitsToFloat(*(volatile unsigned*)(uintptr_t)(0x4e9a80 + layer * 4)),
            ExBitsToFloat(*(volatile unsigned*)(uintptr_t)(0x4e9a74 + layer * 4)),
            *(volatile unsigned*)(uintptr_t)(0x4e9a68 + layer * 4),
            ExTextureTableN(), ExTextureLoadedN());
    }
    ExLogObjects(tag, frame);
}

#define EX_SETTLE_MAX 6000
#define EX_NEW_MAX 8

typedef struct { unsigned p[EX_NEW_MAX]; int n; } ExTiles;

static unsigned *ExFindLayerObj(int layer) {
    unsigned *node;
    int guard = 0;
    for (node = *(unsigned**)(uintptr_t)0x4ea1ac; node && guard < 4096; node = (unsigned*)node[1], guard++) {
        unsigned *o;
        if (IsBadReadPtr(node, 20)) break;
        o = (unsigned*)node[4];
        if (!o || IsBadReadPtr(o, 0x824)) continue;
        if ((o[0x808 / 4] == 0x427e60 || o[0x81c / 4] == 0x427e60) && (int)o[2] == layer) return o;
    }
    return NULL;
}

static int ExTilesHas(const ExTiles *t, unsigned p) {
    int i;
    for (i = 0; i < t->n; i++)
        if (t->p[i] == p) return 1;
    return 0;
}

static void ExTilesCapture(ExTiles *t, int layer, int grown) {
    unsigned *sent = *(unsigned**)(uintptr_t)(0x4e9a94 + layer * 12);
    unsigned *q;
    int i;

    t->n = 0;
    if (!sent || IsBadReadPtr(sent, 12)) return;
    q = (unsigned*)sent[1];
    for (i = 0; i < grown && i < EX_NEW_MAX && q && q != sent; i++) {
        if (IsBadReadPtr(q, 12)) break;
        t->p[t->n++] = q[2];
        q = (unsigned*)q[1];
    }
}

static int ExLayerSettled(int layer, const ExTiles *t) {
    unsigned *obj = ExFindLayerObj(layer);
    unsigned *sent, *q;
    int k, nonNull = 0, guard = 0;

    if (!obj) return 1;
    for (k = 0; k < 3; k++) {
        if (!obj[4 + k]) continue;
        nonNull++;
        if (!ExTilesHas(t, obj[4 + k])) return 0;
    }
    if (!nonNull) return 0;
    sent = *(unsigned**)(uintptr_t)(0x4e9a94 + layer * 12);
    if (sent && !IsBadReadPtr(sent, 12)) {
        for (q = (unsigned*)sent[0]; q && q != sent && guard++ < 64; q = (unsigned*)q[0]) {
            if (IsBadReadPtr(q, 12)) break;
            if (!ExTilesHas(t, q[2])) return 0;
        }
    }
    return 1;
}

static int ExAllSettled(const ExTiles *tiles) {
    int layer;
    for (layer = 0; layer < 4; layer++)
        if (tiles[layer].n > 0 && !ExLayerSettled(layer, &tiles[layer])) return 0;
    return 1;
}

static void ExStep(int f, int gt, int *captured, ExTiles *tiles) {
    int c0[4], layer;

    for (layer = 0; layer < 4; layer++) c0[layer] = *(volatile int*)(uintptr_t)(0x4e9a98 + layer * 12);
    if (!ExRunStageFrame(f)) return;
    if (!*captured && f >= gt) {
        int any = 0;
        for (layer = 0; layer < 4; layer++) {
            int c1 = *(volatile int*)(uintptr_t)(0x4e9a98 + layer * 12);
            if (c1 > c0[layer]) {
                ExTilesCapture(&tiles[layer], layer, c1 - c0[layer]);
                any = 1;
            }
        }
        if (any) *captured = 1;
    }
}

static unsigned *ExMerge(unsigned *a, unsigned *b) {
    unsigned *head = NULL, *tail = NULL, *n;

    while (a || b) {
        if (a && (!b || (int)a[1] <= (int)b[1])) {
            n = a;
            a = (unsigned*)a[0];
        } else {
            n = b;
            b = (unsigned*)b[0];
        }
        n[0] = 0;
        if (tail) tail[0] = (unsigned)(uintptr_t)n;
        else head = n;
        tail = n;
    }
    return head;
}

static void ExSimulateSettled(int from, int target, int gt) {
    unsigned **headp = (unsigned**)(uintptr_t)0x4ea1d8;
    unsigned *n, *bgH = NULL, *bgT = NULL, *hH = NULL, *hT = NULL;
    ExTiles tiles[4];
    int captured = 0, f, extra, heldN = 0, layer;

    memset(tiles, 0, sizeof(tiles));
    for (f = from; f < target; f++) ExStep(f, gt, &captured, tiles);
    *(volatile int*)(uintptr_t)0x4dadf8 = target;
    Log("ExArea: simulated frames %d..%d of events + object update", from, target - 1);

    if (captured && ExAllSettled(tiles)) {
        Log("ExArea: background of the area (group time %d) is already on screen at frame %d", gt, target);
        return;
    }
    if (!captured && gt <= target) {
        Log("ExArea: background settle skipped: no tiles were pushed at group time %d", gt);
        return;
    }

    for (n = *headp; n; ) {
        unsigned *next = (unsigned*)n[0];
        n[0] = 0;
        if ((int)n[2] >= 2 && (int)n[2] <= 5) {
            if (bgT) bgT[0] = (unsigned)(uintptr_t)n;
            else bgH = n;
            bgT = n;
        } else {
            if (hT) hT[0] = (unsigned)(uintptr_t)n;
            else hH = n;
            hT = n;
            heldN++;
        }
        n = next;
    }
    *headp = bgH;
    for (extra = 0; extra < EX_SETTLE_MAX; extra++) {
        ExStep(target + extra, gt, &captured, tiles);
        if (captured && ExAllSettled(tiles)) {
            extra++;
            break;
        }
    }
    *headp = ExMerge(hH, *headp);
    *(volatile int*)(uintptr_t)0x4dadf8 = target;
    for (layer = 0; layer < 4; layer++)
        if (tiles[layer].n > 0)
            Log("ExArea: background settle: layer %d received %d tile(s)", layer, tiles[layer].n);
    Log("ExArea: background settle: ran %d extra frame(s) (group time %d, %d other event(s) held back)%s",
        extra, gt, heldN, (extra >= EX_SETTLE_MAX) ? " -- LIMIT REACHED, tiles not settled" : "");
}

typedef struct {
    int lives, gameOverCountdown, stageEnd;
    int miss, bombs;
    unsigned char continueUi, resultUi, cleared;
} ExPracticeSave;

static void ExPracticeSaveState(ExPracticeSave *s) {
    s->lives = *(volatile int*)(uintptr_t)0x4dadd8;
    s->gameOverCountdown = *(volatile int*)(uintptr_t)0x45ae00;
    s->stageEnd = *(volatile int*)(uintptr_t)0x45adfc;
    s->miss = *(volatile int*)(uintptr_t)0x4dadc0;
    s->bombs = *(volatile int*)(uintptr_t)0x4dadc4;
    s->resultUi = *(volatile unsigned char*)(uintptr_t)0x4dae14;
    s->continueUi = *(volatile unsigned char*)(uintptr_t)0x4dae15;
    s->cleared = *(volatile unsigned char*)(uintptr_t)0x4dae17;
}

static void ExPracticeRestoreState(const ExPracticeSave *s) {
    Log("ExArea: practice state after skip: lives=%d countdown=%d end=%d miss=%d c4=%d ui=%d/%d/%d -> restoring lives=%d countdown=%d",
        *(volatile int*)(uintptr_t)0x4dadd8, *(volatile int*)(uintptr_t)0x45ae00,
        *(volatile int*)(uintptr_t)0x45adfc, *(volatile int*)(uintptr_t)0x4dadc0,
        *(volatile int*)(uintptr_t)0x4dadc4, *(volatile unsigned char*)(uintptr_t)0x4dae14,
        *(volatile unsigned char*)(uintptr_t)0x4dae15, *(volatile unsigned char*)(uintptr_t)0x4dae17,
        s->lives, s->gameOverCountdown);
    *(volatile int*)(uintptr_t)0x4dadd8 = s->lives;
    *(volatile int*)(uintptr_t)0x45ae00 = s->gameOverCountdown;
    *(volatile int*)(uintptr_t)0x45adfc = s->stageEnd;
    *(volatile int*)(uintptr_t)0x4dadc0 = s->miss;
    *(volatile int*)(uintptr_t)0x4dadc4 = s->bombs;
    *(volatile unsigned char*)(uintptr_t)0x4dae14 = s->resultUi;
    *(volatile unsigned char*)(uintptr_t)0x4dae15 = s->continueUi;
    *(volatile unsigned char*)(uintptr_t)0x4dae17 = s->cleared;
}

static int ExFastForwardBoss2Transition(int from) {
    int f;
    int limit = from + 12000;
    ExPracticeSave sv;

    ExPracticeSaveState(&sv);
    g_exMuteSe = 1;
    Log("ExArea: fast-forwarding Boss 1 defeat transition from frame %d until Boss 2 BGM", from);
    for (f = from; f < limit && !g_exBoss2BgmSeen; f++) {
        if (!ExRunStageFrame(f)) {
            Log("ExArea: Boss 2 transition simulation stopped at frame %d", f);
            break;
        }
    }
    g_exMuteSe = 0;
    ExBgmMuteSync();
    ExPracticeRestoreState(&sv);
    {
        void *pl = *(void**)(uintptr_t)0x4e9ad4;
        if (pl) {
            *(int*)((char*)pl + 0x8) = 0;
            if (*(int*)((char*)pl + 0x2c) <= 0) *(int*)((char*)pl + 0x2c) = 5;
            *(int*)((char*)pl + 0x14) = 0;
            Log("ExArea: revived player after Boss 2 transition fast-forward (state=%d hp=%d)",
                *(int*)((char*)pl + 0x8), *(int*)((char*)pl + 0x2c));
        }
    }
    if (g_exBoss2BgmSeen) {
        int endFrame = *(volatile int*)(uintptr_t)0x4dadf8;
        Log("ExArea: Boss 2 BGM reached during transition fast-forward at frame %d; resuming at %d",
            g_exBoss2BgmFrame, endFrame);
        return endFrame;
    }
    Log("ExArea: Boss 2 BGM not reached within %d simulated frames; resuming at frame %d",
        limit - from, *(volatile int*)(uintptr_t)0x4dadf8);
    return *(volatile int*)(uintptr_t)0x4dadf8;
}

/* Boss practice return: the boss practice boss was defeated (0x4dae04 -> NULL).  The defeat script
   waits for the defeat dialogue confirmations, so waiting naturally stalls (Boss 2's BGM is never
   reached).  Simulate the defeat transition the same way the Boss 2 practice transition fast-forward
   does, until the stage clear opcode is called (ExStageClearOp sets g_exBossClearSim), then return to
   the Practice menu from the event dispatch. */
static int ExFastForwardBossReturn(int from) {
    int f;
    int limit = from + 12000;
    int endFrame;
    ExPracticeSave sv;

    ExPracticeSaveState(&sv);
    g_exMuteSe = 1;
    g_exBossClearSim = 0;
    Log("ExArea: fast-forwarding from frame %d until the stage clear opcode (boss practice %d)", from,
        g_exBossRun);
    for (f = from; f < limit && !g_exBossClearSim; f++) {
        if (!ExRunStageFrame(f)) {
            Log("ExArea: return simulation stopped at frame %d", f);
            break;
        }
    }
    g_exMuteSe = 0;
    ExBgmMuteSync();
    ExPracticeRestoreState(&sv);
    {
        void *pl = *(void**)(uintptr_t)0x4e9ad4;
        if (pl) {
            *(int*)((char*)pl + 0x8) = 0;
            if (*(int*)((char*)pl + 0x2c) <= 0) *(int*)((char*)pl + 0x2c) = 5;
            *(int*)((char*)pl + 0x14) = 0;
        }
    }
    endFrame = *(volatile int*)(uintptr_t)0x4dadf8;
    if (g_exBossClearSim)
        Log("ExArea: stage clear opcode reached during the boss return fast-forward at frame %d", endFrame);
    else
        Log("ExArea: stage clear opcode not reached within %d simulated frames (frame %d)", limit - from,
            endFrame);
    return endFrame;
}

/* Boss intro dialogue investigation: after a Boss practice jump, log the first few calls of every VM
   opcode handler (thread ip / wait / args) for EX_TR_FRAMES frames, to find the dialogue opcodes. */
#define EX_TR_N 40
#define EX_TR_FRAMES 2400
static const int g_ExDlgTrace = 0;      /* 1: log VM opcodes after a Boss practice jump */
/* Skip Boss 1 dialogue during boss practice and the Boss 2 transition fast-forward so the VM can advance
   through the script without waiting for player confirmation. */
static const int g_ExBossDlgSkip = 1;   /* 1: skip the Boss 1 intro dialogue in Boss practice */
static const int g_ExBossDlgDump = 0;   /* 1: dump the script bytes of the dialogue region to the log */
/* Boss 1 main thread (from ExTrace): the first dialogue line is op21 at ip 649740; the script resumes
   at the op0 at ip 650448 (right before the BGM op18). op21 is 20 bytes and the VM adds the size of the
   current instruction after the handler returns, so ip is set to END - 20 and the handler is not run. */
#define EX_B1_DLG_FIRST 649740
#define EX_B1_DLG_LO 649400
#define EX_B1_DLG_END 650448
static PF_Op g_trOrig[EX_TR_N];
static int g_trInstalled = 0;
static volatile int g_trOn = 0;
static int g_trEndFrame = 0;
static int g_trCnt[EX_TR_N];
#define EX_TR_SEEN 8192
static unsigned g_trSeen[EX_TR_SEEN];
static int g_trLines = 0;
static volatile int g_trDlgArmed = 0;

/* true the first time this (opcode, ip) pair is seen in the current trace */
static int ExTrFirst(int op, int ip) {
    unsigned key = ((unsigned)op << 24) ^ (unsigned)ip ^ 0x80000000u;
    unsigned h = (key * 2654435761u) >> 19, n;
    for (n = 0; n < EX_TR_SEEN; n++, h = (h + 1) % EX_TR_SEEN) {
        if (g_trSeen[h] == key) return 0;
        if (g_trSeen[h] == 0) { g_trSeen[h] = key; return 1; }
    }
    return 0;
}

static void ExDumpScript(void *args, int ip) {
    /* args points 4 bytes (instruction header) past the instruction start, ip is the instruction offset */
    const unsigned char *base = (const unsigned char*)args - 4 - ip;
    int from = 649600, to = 650800, a;

    if (IsBadReadPtr(base + from, to - from)) {
        Log("ExScript: range not readable (base=%p)", base);
        return;
    }
    Log("ExScript: dump ip %d..%d (base=%p)", from, to, base);
    for (a = from; a < to; a += 32) {
        const unsigned *w = (const unsigned*)(base + a);
        Log("ExScript: %d: %08X %08X %08X %08X %08X %08X %08X %08X", a,
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

static int g_dlgSkipLog = 0;
static int g_dlgShownLog = 0;

static unsigned char ExTrCommon(int i, void *thr, void *args) {
    unsigned char r;
    int ip = (i == 21) ? *(int*)((char*)thr + 0x14) : -1;
    int skipIntro = i == 21 && g_ExBossDlgSkip && g_exBossRun >= 1 &&
                    ip >= EX_B1_DLG_LO && ip < EX_B1_DLG_END;
    int hideBoss2Transition = i == 21 && g_ExBossDlgSkip && g_exBossRun == 2 &&
                              g_exBoss2Transition;
    if (i == 21 && g_exBossRun >= 1 && !g_exSimulatingStageFrame && g_dlgShownLog < 80) {
        const char *action = skipIntro ? "skipped" : (hideBoss2Transition ? "hidden" : "SHOWN");
        g_dlgShownLog++;
        Log("ExDlg: Boss %d op21 thr=%p ip=%d line=%d frame=%d -> %s", g_exBossRun, thr, ip,
            IsBadReadPtr(args, 8) ? -1 : (int)((unsigned*)args)[1],
            *(volatile int*)(uintptr_t)0x4dadf8, action);
    }
    if (skipIntro) {
        /* Boss 1 intro: each dialogue line is [op21 show line N][op6 loop until system var 0x1D == 0]
           [op2 wait 1][op4 jump back]. Not showing the line leaves var 0x1D at 0, so the loop is left at
           once, while the boss entrance ops between the lines still run. */
        if (g_dlgSkipLog < 12) {
            g_dlgSkipLog++;
            Log("ExArea: Boss 1 intro dialogue skipped (ip=%d line=%d frame=%d sim=%d)", ip,
                IsBadReadPtr(args, 8) ? -1 : (int)((unsigned*)args)[1],
                *(volatile int*)(uintptr_t)0x4dadf8, g_exSimulatingStageFrame);
        }
        return 1;
    }
    if (g_ExBossDlgDump && i == 21 && g_exBossRun >= 1 && g_trDlgArmed &&
        *(int*)((char*)thr + 0x14) == EX_B1_DLG_FIRST) {
        g_trDlgArmed = 0;
        ExDumpScript(args, EX_B1_DLG_FIRST);
    }
    r = g_trOrig[i](thr, args);
    if (hideBoss2Transition) {
        *(volatile unsigned char*)(uintptr_t)0x4ea548 = 0;
        if (g_dlgSkipLog < 24) {
            g_dlgSkipLog++;
            Log("ExArea: Boss 2 transition dialogue hidden after op21 (ip=%d line=%d frame=%d)", ip,
                IsBadReadPtr(args, 8) ? -1 : (int)((unsigned*)args)[1],
                *(volatile int*)(uintptr_t)0x4dadf8);
        }
    }
    if (g_trOn && !g_exSimulatingStageFrame && g_trLines < 3000 &&
        ExTrFirst(i, *(int*)((char*)thr + 0x14))) {
        unsigned *a = (unsigned*)args;
        int ok = a && !IsBadReadPtr(a, 16);
        g_trCnt[i]++;
        g_trLines++;
        Log("ExTrace: op%d thr=%p ip=%d wait=%d ret=%d frame=%d args=%08X %08X %08X %08X", i, thr,
            *(int*)((char*)thr + 0x14), *(int*)((char*)thr + 0x10), (int)r,
            *(volatile int*)(uintptr_t)0x4dadf8,
            ok ? a[0] : 0, ok ? a[1] : 0, ok ? a[2] : 0, ok ? a[3] : 0);
    }
    return r;
}
#define EX_TR_DEF(i) static unsigned char __cdecl ExTr##i(void *t, void *a) { return ExTrCommon(i, t, a); }
EX_TR_DEF(0) EX_TR_DEF(1) EX_TR_DEF(3) EX_TR_DEF(4) EX_TR_DEF(5) EX_TR_DEF(6) EX_TR_DEF(7)
EX_TR_DEF(8) EX_TR_DEF(9) EX_TR_DEF(10) EX_TR_DEF(11) EX_TR_DEF(12) EX_TR_DEF(13) EX_TR_DEF(14)
EX_TR_DEF(15) EX_TR_DEF(16) EX_TR_DEF(17) EX_TR_DEF(18) EX_TR_DEF(19) EX_TR_DEF(20) EX_TR_DEF(21)
EX_TR_DEF(22) EX_TR_DEF(23) EX_TR_DEF(24) EX_TR_DEF(25) EX_TR_DEF(26) EX_TR_DEF(27) EX_TR_DEF(28)
EX_TR_DEF(29) EX_TR_DEF(30) EX_TR_DEF(31) EX_TR_DEF(32) EX_TR_DEF(33) EX_TR_DEF(34) EX_TR_DEF(35)
EX_TR_DEF(36) EX_TR_DEF(37) EX_TR_DEF(38) EX_TR_DEF(39)

static void ExTraceStart(int target) {
    static PF_Op hooks[EX_TR_N] = {
        ExTr0, ExTr1, NULL, ExTr3, ExTr4, ExTr5, ExTr6, ExTr7, ExTr8, ExTr9, ExTr10, ExTr11, ExTr12,
        ExTr13, ExTr14, ExTr15, ExTr16, ExTr17, ExTr18, ExTr19, ExTr20, ExTr21, ExTr22, ExTr23, ExTr24,
        ExTr25, ExTr26, ExTr27, ExTr28, ExTr29, ExTr30, ExTr31, ExTr32, ExTr33, ExTr34, ExTr35, ExTr36,
        ExTr37, ExTr38, ExTr39
    };
    void **tbl = *(void***)(uintptr_t)0x4ea1fc;
    int i, n = 0;
    DWORD old;

    if ((!g_ExDlgTrace && !g_ExBossDlgSkip) || !tbl) return;
    g_trDlgArmed = g_ExBossDlgSkip;
    if (!g_trInstalled) {
        for (i = 0; i < EX_TR_N; i++) {
            DWORD p = (DWORD)(uintptr_t)tbl[i];
            if (!hooks[i] || p < 0x401000 || p >= 0x460000) continue;
            if (!g_ExDlgTrace && i != 21) continue;
            if (!VirtualProtect(&tbl[i], sizeof(void*), PAGE_READWRITE, &old)) continue;
            g_trOrig[i] = (PF_Op)tbl[i];
            Log("ExTrace: opcode %d handler at 0x%08X", i, p);
            tbl[i] = (void*)hooks[i];
            VirtualProtect(&tbl[i], sizeof(void*), old, &old);
            n++;
        }
        g_trInstalled = 1;
        Log("ExTrace: hooked %d opcode handler(s)", n);
    }
    if (target < 0) return;   /* install only */
    g_dlgSkipLog = 0;
    g_dlgShownLog = 0;
    memset(g_trCnt, 0, sizeof(g_trCnt));
    memset(g_trSeen, 0, sizeof(g_trSeen));
    g_trLines = 0;
    g_trEndFrame = target + EX_TR_FRAMES;
    g_trOn = g_ExDlgTrace;
    Log("ExTrace: logging opcodes from frame %d to %d", target, g_trEndFrame);
}

static void ExDlgInstall(void) {
    ExTraceStart(-1);
}

/* Boss 1 practice: no "Stage Clear" when Boss 1 is defeated.
   The stage clear screen is started only by the VM opcode whose handler is 0x42f200 (it sets the result UI
   flag 0x4dae14 and calls 0x417c00).  In the Boss 1 practice that opcode is skipped until Boss 2's battle BGM
   has started, so the script just goes on after Boss 1; the clear after Boss 2 is shown as usual.  The handler is
   looked up by address in the opcode table, so the opcode number does not matter.  Every call is logged. */
static PF_Op g_exClearOrig = NULL;
static int g_exClearHooked = 0;
static int g_exClearLog = 0;

static void ExReturnToPracticeMenu(const char *reason) {
    Log("ExArea: %s finished; returning to the Practice menu", reason);
    g_exRun = 0;
    g_exBossRun = 0;
    g_exStartReq = 0;
    g_exPending = 0;
    g_exSkipWait = 0;
    g_exBoss2Kill = 0;
    g_exBoss2Transition = 0;
    g_exBossReturnFF = 0;
    ((void (__cdecl *)(int, int))(uintptr_t)0x4254d0)(1, 0);
}

static unsigned char __cdecl ExStageClearOp(void *thr, void *args) {
    int transitionClear = g_exBossRun == 2 && !g_exBoss2BgmSeen;
    if (g_exClearLog < 20) {
        g_exClearLog++;
        Log("ExArea: stage clear opcode called (thr=%p ip=%d frame=%d boss=%p boss2bgm=%d sim=%d) -> %s", thr,
            thr && !IsBadReadPtr(thr, 0x18) ? *(int*)((char*)thr + 0x14) : -1,
            *(volatile int*)(uintptr_t)0x4dadf8, *(void**)(uintptr_t)0x4dae04, g_exBoss2BgmSeen,
            g_exSimulatingStageFrame ? 1 : 0, transitionClear ? "suppressed" : "return to Practice");
    }
    if (transitionClear) return 1;
    if (g_exSimulatingStageFrame && g_exRun && g_exBossRun >= 1) {
        /* inside the return fast-forward: stop the simulation; the actual return to the Practice
           menu happens back in the event dispatch (a scene switch inside the sim would corrupt it) */
        g_exBossClearSim = 1;
        return 1;
    }
    if (g_exRun && g_exBossRun == 1) {
        ExReturnToPracticeMenu("Boss 1");
        return 1;
    }
    if (g_exRun && g_exBossRun == 2 && g_exBoss2BgmSeen) {
        ExReturnToPracticeMenu("Boss 2");
        return 1;
    }
    return g_exClearOrig(thr, args);
}

static void ExClearHookInstall(void) {
    void **tbl = *(void***)(uintptr_t)0x4ea1fc;
    int i;
    DWORD old;

    g_exClearLog = 0;
    if (g_exClearHooked || !tbl) return;
    for (i = 0; i < 45; i++) {   /* the VM has opcodes 0..44 */
        if ((DWORD)(uintptr_t)tbl[i] != 0x42f200) continue;
        if (!VirtualProtect(&tbl[i], sizeof(void*), PAGE_READWRITE, &old)) continue;
        g_exClearOrig = (PF_Op)tbl[i];
        tbl[i] = (void*)&ExStageClearOp;
        VirtualProtect(&tbl[i], sizeof(void*), old, &old);
        g_exClearHooked = 1;
        Log("ExArea: stage clear opcode %d hooked for boss Practice completion", i);
        return;
    }
    Log("ExArea: stage clear opcode handler 0x42f200 not found in the opcode table");
}

/* Boss 2 practice.  Boss 2 appears after Boss 1 is defeated, at a frame that depends on how fast the player
   wins, so there is no fixed frame to jump to.  The practice jumps to the Boss 1 start like the Boss 1 practice
   and then defeats Boss 1 at once (its hit points are set to 0 every frame) so that the script goes on to
   Boss 2 by itself.  The Boss 2 BGM (g_exBossBgm[1], seen by ExVmBgmHook) ends the defeating.
   Boss 1 is found as the only enemy object with a large hit point value (+0x2c, as for the player object);
   candidates are logged so the rule can be adjusted. */
/* The game keeps the current boss in DWORD 0x4dae04: the enemy spawn (0x42fa70) stores the new object there
   when it is created with the boss flag, and the object removal (0x42ea40) clears it again.  The boss hit
   points are the FLOAT at object+0x48 (1.0 = full; the HUD gauge 0x414710 draws it): the hit handler
   (object+0x818 = 0x42a1a0, called as handler(self, shot)) subtracts the shot's +0x48 and, when the value is
   <= 0, runs the defeat (score, hit box off, thread flag +0x0c = 1).  The old rule (int at +0x2c, 500..2000000)
   never matched anything: +0x2c is a counter that starts at 0. */
static unsigned *ExFindBossCandidate(int logIt) {
    unsigned *boss = *(unsigned**)(uintptr_t)0x4dae04;

    if (!boss || IsBadReadPtr(boss, 0x824)) return NULL;
    if (logIt)
        Log("ExArea: boss object=%p type=%u hp(+0x48)=%.3f flags(+0x40)=%08X hitbox(+0x109)=%u", (void*)boss,
            boss[1], *(float*)((char*)boss + 0x48), boss[0x40 / 4], (unsigned)*((unsigned char*)boss + 0x109));
    return boss;
}

static void ExBoss2Step(int frame) {
    static unsigned char fakeShot[0x830] __attribute__((aligned(16)));
    unsigned *boss;
    float hp;

    g_exBoss2KillFrames++;
    if (g_exBoss2KillFrames > 3600) {
        g_exBoss2Kill = 0;
        g_exBoss2Transition = 0;
        Log("ExArea: Boss 2 practice: Boss 2 not seen within %d frames, giving up", g_exBoss2KillFrames);
        return;
    }
    boss = ExFindBossCandidate(g_exBoss2KillFrames % 120 == 1);
    if (!boss) {
        if (g_exBoss1Obj) {
            Log("ExArea: Boss 2 practice: Boss 1 object %p is gone (frame %d, %d frame(s) after the jump)",
                (void*)g_exBoss1Obj, frame, g_exBoss2KillFrames);
            g_exBoss1Obj = NULL;
        }
        return;
    }
    if (g_exBoss1Defeated) {
        if (!g_exBoss1Obj || boss != g_exBoss1Obj) {
            g_exBoss2Kill = 0;
            Log("ExArea: Boss 2 appeared (object %p, hp=%.3f) %d frame(s) after Boss 1 was defeated",
                (void*)boss, *(float*)((char*)boss + 0x48), g_exBoss2KillFrames);
            return;
        }
        return;
    }
    if (!g_exBoss1Obj) {
        g_exBoss1Obj = boss;
        Log("ExArea: Boss 2 practice: Boss 1 object %p found (hp=%.3f)", (void*)boss, *(float*)((char*)boss + 0x48));
    } else if (boss != g_exBoss1Obj) {
        /* a different boss object: this is Boss 2, leave it alone */
        g_exBoss2Kill = 0;
        Log("ExArea: Boss 2 appeared (object %p, hp=%.3f) %d frame(s) after the Boss 1 jump", (void*)boss,
            *(float*)((char*)boss + 0x48), g_exBoss2KillFrames);
        return;
    }
    hp = *(float*)((char*)boss + 0x48);
    if (hp > 0.0f) {
        *(float*)((char*)boss + 0x48) = 0.0f;
        if (g_exBoss2Logged < 20) {
            g_exBoss2Logged++;
            Log("ExArea: Boss 2 practice: boss object %p hp %.3f -> 0 (frame %d)", (void*)boss, (double)hp, frame);
        }
    }
    /* The defeat itself runs inside the hit handler; with hp <= 0 one call is enough (the shot only has to
       look like a player shot: type 2 at +0x04, damage at +0x48).  Not repeated once the hit box is off. */
    if (*((unsigned char*)boss + 0x109) != 0) {
        typedef void (__cdecl *PF_HitHandler)(void *self, void *other);
        PF_HitHandler hit = (PF_HitHandler)(uintptr_t)boss[0x818 / 4];
        if (hit && !IsBadCodePtr((FARPROC)hit)) {
            *(int*)(fakeShot + 0x04) = 2;
            *(float*)(fakeShot + 0x48) = 1.0f;
            hit(boss, fakeShot);
            g_exBoss1Defeated = 1;
            Log("ExArea: Boss 2 practice: Boss 1 hit handler %p called (frame %d, hitbox now %u)", (void*)hit, frame,
                (unsigned)*((unsigned char*)boss + 0x109));
        }
    }
}

static unsigned char __cdecl EventDispatchImpl(int frame) {
    static int f9prev = 0;
    FpuEnforce("stage", frame);
    PauseReplayRestore();
    ExBgmMuteSync();
    if (g_trOn && frame > g_trEndFrame) {
        g_trOn = 0;
        Log("ExTrace: finished at frame %d", frame);
    }
    if (g_exSimulatingStageFrame)
        return ((PF_EventDispatch)(uintptr_t)0x430020)(frame);
    int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9prev && *(volatile int*)(uintptr_t)0x4dade4 == 4)
        Log("ExArea: marker frame=%d", frame);
    f9prev = f9;
    if (*(volatile int*)(uintptr_t)0x4dade4 == 4 && !g_exRun) {
        int ai;
        for (ai = 0; ai < EX_AREA_N; ai++)
            if (frame == g_exAreaDefaultFrame[ai]) ExLogLayerState("natural", frame);
    }
    {
        static int f8prev = 0;
        int f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (g_ExBgDebugKeys && f8 && !f8prev && g_exBgGroupN > 0 && g_exRun && *(volatile int*)(uintptr_t)0x4dade4 == 4) {
            g_exBgCur = (g_exBgCur + 1) % g_exBgGroupN;
            ExBgSeed(&g_exBgGroups[g_exBgCur]);
            Log("ExArea: F8 background group %d/%d (time %d)", g_exBgCur + 1, g_exBgGroupN,
                g_exBgGroups[g_exBgCur].time);
        }
        f8prev = f8;
    }
    if (g_exBoss2Kill && !g_exSimulatingStageFrame)
        ExBoss2Step(frame);
    if (g_exBossRun == 2 && g_exBoss1Defeated && !g_exBoss2BgmSeen &&
        !g_exBoss2TransitionFastForwarded && !g_exSimulatingStageFrame) {
        g_exBoss2TransitionFastForwarded = 1;
        frame = ExFastForwardBoss2Transition(frame);
    }
    if (g_exBossReturnFF && !g_exSimulatingStageFrame) {
        int run = g_exBossRun;
        g_exBossReturnFF = 0;
        frame = ExFastForwardBossReturn(frame);
        ExReturnToPracticeMenu(run == 1 ? "Boss 1" : "Boss 2");
        return 1;
    }
    if (g_exRun && g_exBossRun == 1 && g_exBoss1DonePending) {
        ExReturnToPracticeMenu("Boss 1");
        return 1;
    }
    if (g_exRun && g_exBossRun == 0 && *(volatile int*)(uintptr_t)0x4dade4 == 4 &&
        g_exArea >= 0 && g_exArea < EX_AREA_N && frame >= g_exAreaEndFrame[g_exArea]) {
        ExReturnToPracticeMenu("Area");
        return 1;
    }
    if (g_exBossRun >= 1 && !g_exBoss2Kill && !g_exSimulatingStageFrame && g_exBossRunLogged < 400) {
        /* Boss practice (played normally): log when the current boss object changes or dies.  A
           "none" after a live boss means the practice boss was defeated: arm the return fast-forward
           (ExFastForwardBossReturn).  In Boss 2 practice only after Boss 2's BGM (the Boss 1 kill
           during the transition is handled by ExBoss2Step with g_exBoss2Kill). */
        static unsigned *lastBoss = NULL;
        unsigned *b = *(unsigned**)(uintptr_t)0x4dae04;
        if (b != lastBoss) {
            g_exBossRunLogged++;
            if (b && !IsBadReadPtr(b, 0x824))
                Log("ExDlg: Boss practice: current boss object -> %p type=%u hp=%.3f (frame %d)", (void*)b, b[1],
                    *(float*)((char*)b + 0x48), frame);
            else if (lastBoss) {
                Log("ExDlg: Boss practice: current boss object -> none (frame %d, was %p)", frame,
                    (void*)lastBoss);
                if (g_exBossRun == 1 || g_exBoss2BgmSeen) g_exBossReturnFF = 1;
            }
            lastBoss = b;
        }
    }
    if (g_exPending && frame == 0 && *(volatile int*)(uintptr_t)0x4dade4 == 4) {
        g_exPending = 0;
        ExLogQueue();
    }
    if (g_exSkipWait && *(volatile int*)(uintptr_t)0x4dade4 == 4) {
        int bossIndex = g_exBossRun - 1;
        int isBoss = bossIndex >= 0 && bossIndex < 2;
        int bgmId = isBoss ? g_exBossBgm[bossIndex] :
            ((g_exArea >= 0 && g_exArea < EX_AREA_N) ? g_exAreaBgm[g_exArea] : -1);
        if (bgmId < 0 || bgmId >= EX_BGM_IDS) {
            g_exSkipWait = 0;
            Log("ExArea: invalid practice selection (area=%d boss=%d), skip cancelled", g_exArea, g_exBossRun);
            return ((PF_EventDispatch)(uintptr_t)0x430020)(frame);
        }
        int target = g_exBgmTime[bgmId];
        int haveBg;
        if (isBoss && g_exBossBgmTime[bossIndex] > 0) {
            target = g_exBossBgmTime[bossIndex];
            Log("ExArea: Boss %d name-matched bgm frame %d", g_exBossRun, target);
        }
        if (isBoss && target <= 0) {
            target = g_exBossBgmTime[bossIndex];
            if (target <= 0) target = ExFindBgmTime(bgmId);
        }
        if (g_exBossRun == 2) {
            /* The BGM at frame 24017 happens after the Boss 1 script has already been running
               for several minutes. Simulating to that timestamp leaves its live boss and attacks
               on screen. Enter at Boss 1's intro, then use the real defeat path to advance the script. */
            target = g_exBossDefaultFrame[0];
        } else if (target <= 0 && isBoss) target = g_exBossDefaultFrame[bossIndex];
        if (target <= 0 && !isBoss && g_exArea >= 0 && g_exArea < EX_AREA_N && !g_exShorten)
            target = g_exAreaDefaultFrame[g_exArea];
        if (target > 0) {
            int startFrame = frame;
            int sel = isBoss ? 3 : ((g_exArea >= 0 && g_exArea < EX_AREA_N) ? g_exAreaBgGroup[g_exArea] : 0);
            int areaBgMode = g_ExBgMode;
            /* The fast-forward runs the real stage update with no input, so the player is hit over and
               over while the frames are simulated. Keep the state the run started with (lives, game-over
               countdown, game-over/continue flags) and put it back after the jump. */
            ExPracticeSave sv;
            ExPracticeSaveState(&sv);
            g_exMuteSe = 1;
            g_exSkipWait = 0;
            if (areaBgMode == 0) {
                haveBg = ExBgSaveAll(sel);
                ExSkipTo(target, g_exArea);
                ExFastForward(startFrame, target);
                if (haveBg) {
                    g_exBgCur = sel;
                    ExBgSeed(&g_exBgGroups[sel]);
                }
            } else {

                ExBgSaveAll(-1);
                ExSkipTo(target, g_exArea);
                if (areaBgMode == 2 && sel >= 0 && sel < g_exBgGroupN)
                    ExSimulateSettled(startFrame, target, g_exBgGroups[sel].time);
                else
                    ExFastForward(startFrame, target);
            }
            *(volatile int*)(uintptr_t)0x4dadf8 = target;
            g_exMuteSe = 0;
            ExPracticeRestoreState(&sv);
            if (g_exBossRun == 2) {
                g_exBoss2Kill = 1;
                g_exBoss2Transition = 1;
                g_exBoss2KillFrames = 0;
                g_exBoss2Logged = 0;
                g_exBoss1Obj = NULL;
                g_exBoss1Defeated = 0;
                g_exBoss2BgmSeen = 0;
                Log("ExArea: Boss 2 practice will defeat Boss 1 through the game's hit handler and continue the script");
            }
            if (isBoss) ExTraceStart(target);
            if (isBoss && g_ExBossDlgSkip && g_exBossRun >= 1) {
                Log("ExArea: closing the dialogue message left open by the simulation (active=%d line=%d)",
                    *(volatile unsigned char*)(uintptr_t)0x4ea548, *(volatile int*)(uintptr_t)0x4ea514);
                *(volatile unsigned char*)(uintptr_t)0x4ea548 = 0;
                g_dlgSkipLog = 0;
            }
            ExLogLayerState("practice", target);
            if (isBoss) {
                void *pl = *(void**)(uintptr_t)0x4e9ad4;
                if (pl) {
                    *(int*)((char*)pl + 0x8) = 0;
                    if (*(int*)((char*)pl + 0x2c) <= 0) *(int*)((char*)pl + 0x2c) = 5;
                    *(int*)((char*)pl + 0x14) = 0;
                    Log("ExArea: revived player (state=%d hp=%d)", *(int*)((char*)pl + 0x8), *(int*)((char*)pl + 0x2c));
                }
            }
            if (isBoss)
                Log("ExArea: Boss %d jump to frame %d from frame %d (bgmId=%d, background mode %d)",
                    g_exBossRun, target, startFrame, bgmId, areaBgMode);
            else
                Log("ExArea: Area %d/%d jump to frame %d from frame %d (bgmId=%d, background mode %d)",
                    g_exArea + 1, g_exAreaN, target, startFrame, bgmId, areaBgMode);
            frame = target;
        } else if (frame > 1500) {
            g_exSkipWait = 0;
            if (isBoss) Log("ExArea: BGM event for Boss %d not seen, no skip", g_exBossRun);
            else Log("ExArea: BGM event of area %d not seen, no skip", g_exArea + 1);
        } else if (frame % 60 == 0) {
            ExLogProgress(frame);
        }
    }
    return ((PF_EventDispatch)(uintptr_t)0x430020)(frame);
}

static unsigned char __cdecl EventDispatchHook(int frame) {
    LARGE_INTEGER a, b;
    unsigned char result;
    QueryPerformanceCounter(&a);
    result = EventDispatchImpl(frame);
    QueryPerformanceCounter(&b);
    LONGLONG dt = b.QuadPart - a.QuadPart;
    g_gamePerf[0].ticks += dt;
    if (dt > g_gamePerf[0].maxTicks) g_gamePerf[0].maxTicks = dt;
    g_gamePerf[0].calls++;
    return result;
}

typedef void (__cdecl *PF_EventInsert)(int time, void *node);

static void __cdecl BgPushHook(int layer, unsigned *argsP, unsigned tex) {
    if (*(volatile int*)(uintptr_t)0x4dade4 == 4) {
        union { unsigned b; float f; } v;
        unsigned texObj;
        v.b = tex;
        texObj = ((PF_TexById)(uintptr_t)0x409960)((unsigned)v.f);
        Log("ExArea: bg push frame=%d layer=%.1f args=%08X tex=%d texObj=%08X id=%d (tbl=n%d/l%d)",
            *(volatile int*)(uintptr_t)0x4dadf8, (double)v.f, (unsigned)(uintptr_t)argsP,
            (unsigned)v.f, texObj, ExTexId(texObj), ExTextureTableN(), ExTextureLoadedN());
    }
    ((PF_LayerSetU3)(uintptr_t)0x427d40)(layer, (unsigned)(uintptr_t)argsP, tex);
}

static void __cdecl EventInsertHook(int time, unsigned *node) {
    if (node && *(volatile int*)(uintptr_t)0x4dade4 == 4) {
        int ty = (int)node[2];
        if (ty == 6) {
            unsigned *b = (unsigned*)node[3];
            if (b && b[3] < EX_BGM_IDS) {
                g_exBgmTime[b[3]] = time;
                Log("ExArea: BGM id %u scheduled at frame %d", b[3], time);
            }
        }
        if (ty != 0) {
            unsigned *a = (unsigned*)node[3];
            Log("ExArea: +ev frame=%d t=%d type=%d args=%08X %08X %08X %08X %08X %08X",
                *(volatile int*)(uintptr_t)0x4dadf8, time, ty,
                a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0, a ? a[3] : 0, a ? a[4] : 0, a ? a[5] : 0);
        }
    }
    ((PF_EventInsert)(uintptr_t)0x42fe30)(time, node);
}

static int PatchEventInsertLog(void) {
    DWORD a;
    int n = 0;

    for (a = 0x42fe60; a < 0x430020; a++)
        if (*(unsigned char*)(uintptr_t)a == 0xe8 &&
            (DWORD)(a + 5 + *(DWORD*)(uintptr_t)(a + 1)) == 0x42fe30)
            n += HookCall(a, 0x42fe30, (void*)&EventInsertHook);
    return n;
}

static void PatchReplayFpu(void) {
    if (!g_ReplayFpuFix) return;
    Log("FpuFix: x87 control word at attach %04X, forcing %04X from the stage frame/input hooks",
        (unsigned)FpuGetCw(), (unsigned)g_ReplayFpuCw);
    if (g_ExAreaSelect) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("FpuFix: unexpected image base, stage hook skipped");
        return;
    }
    if (HookCall(0x413c45, 0x430020, (void*)&EventDispatchHook))
        Log("FpuFix: stage frame hook installed");
}

typedef int (__cdecl *PF_VmBgm)(int id);
static int g_exVmBgmLog = 1;
static int __cdecl ExVmBgmHook(int id);
static int __cdecl ExVmSpawnHook(void *a, void *b);

static void PatchExAreaSelect(void) {
    int n = 0;

    if (!g_ExAreaSelect) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ExArea: unexpected image base, skipped");
        return;
    }
    n += HookCall(0x423791, 0x420c90, (void*)&PracticeUpdateHook);
    n += HookCall(0x42407f, 0x420de0, (void*)&PracticeDrawHook);
    n += HookCall(0x413c45, 0x430020, (void*)&EventDispatchHook);
    Log("ExArea: patched %d/3 call sites", n);
    Log("ExArea: event insert log: %d call site(s) hooked", PatchEventInsertLog());
    if (HookCall(0x4300b4, 0x427d40, (void*)&BgPushHook))
        Log("ExArea: background push log hooked");
    if (HookCall(0x42f0c3, 0x433740, (void*)&ExVmBgmHook))
        Log("ExArea: VM bgm op hooked");
    if (HookCall(0x42f475, 0x42a030, (void*)&ExVmSpawnHook))
        Log("ExArea: VM spawn op hooked");
}

typedef int (__cdecl *PF_VmSpawnReg)(void *a, void *b);
static int __cdecl ExVmSpawnHook(void *a, void *b) {
    unsigned *p = (unsigned*)b;
    int frame = *(volatile int*)(uintptr_t)0x4dadf8;
    if (g_exVmBgmLog && frame >= 17000 && frame <= 26000)
        Log("ExArea: VM spawn frame=%d sim=%d s=%08X a=%08X a2=%08X a3=%08X a4=%08X",
            frame, g_exSimulatingStageFrame ? 1 : 0, (unsigned)(uintptr_t)b,
            p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0);
    return ((PF_VmSpawnReg)(uintptr_t)0x42a030)(a, b);
}

static int __cdecl ExVmBgmHook(int id) {
    int frame;
    if (id >= 0 && id < EX_BGM_IDS) {
        frame = *(volatile int*)(uintptr_t)0x4dadf8;
        if (frame < g_exBgmTime[id] || g_exBgmTime[id] <= 0)
            g_exBgmTime[id] = frame;
        if (id == g_exBossBgm[0]) {
            if (g_exBossBgmTime[0] <= 0 || frame < g_exBossBgmTime[0])
                g_exBossBgmTime[0] = frame;
        } else if (id == g_exBossBgm[1]) {
            if (g_exBossBgmTime[1] <= 0 || frame < g_exBossBgmTime[1])
                g_exBossBgmTime[1] = frame;
        }
        if (id == g_exBossBgm[1] && g_exBossRun >= 1) {
            if (g_exBossRun == 1) g_exBoss1DonePending = 1;
            if (g_exBossRun == 2) {
                g_exBoss2BgmSeen = 1;
                g_exBoss2BgmFrame = frame;
                if (!g_exBoss2ScoreReset) {
                    *(volatile int*)(uintptr_t)0x4dadf0 = 0;
                    *(volatile int*)(uintptr_t)0x4dade8 = 0;
                    g_exBoss2ScoreReset = 1;
                    Log("ExArea: Boss 2 started; score and displayed score reset to 0 (frame %d)", frame);
                }
            }
        }
        if (g_exBoss2Transition && id == g_exBossBgm[1] && g_exBossRun == 2) {
            g_exBoss2Kill = 0;
            g_exBoss2Transition = 0;
            Log("ExArea: Boss 2 started (BGM id=%d at frame %d, %d frame(s) after the Boss 1 jump)", id,
                frame, g_exBoss2KillFrames);
        }
        if (g_exVmBgmLog)
            Log("ExArea: VM bgm id=%d frame=%d (sim=%d)", id, frame,
                g_exSimulatingStageFrame ? 1 : 0);
    }
    if (g_exSimulatingStageFrame) {
        if (g_exBossRun == 2 && id == g_exBossBgm[1])
            return ((PF_VmBgm)(uintptr_t)0x433740)(id);
        return 0;
    }
    return ((PF_VmBgm)(uintptr_t)0x433740)(id);
}

static const int g_PauseAudio = 1;

#define AUDIO_MAX 1024
typedef struct { IDirectSoundBuffer *buf; int paused, looping, posValid; DWORD pos, settled; } AudioBuf;
static AudioBuf g_audio[AUDIO_MAX];
static int g_audioN = 0;
static IDirectSoundBuffer *g_chkBuf = NULL;
static DWORD g_chkPos = 0;
static DWORD g_chkTick = 0;
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

static void AudioRemove(void *buf) {
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
        g_audio[g_audioN].posValid = 0;
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
    g_chkBuf = NULL;
    for (i = 0; i < g_audioN; i++) {
        DWORD st = 0;
        IDirectSoundBuffer *b = g_audio[i].buf;
        g_audio[i].paused = 0;
        if (SUCCEEDED(IDirectSoundBuffer_GetStatus(b, &st)) && (st & DSBSTATUS_PLAYING)) {
            DWORD pos = 0;
            HRESULT posHr = IDirectSoundBuffer_GetCurrentPosition(b, &pos, NULL);
            g_audio[i].paused = 1;
            g_audio[i].looping = (st & DSBSTATUS_LOOPING) ? 1 : 0;
            g_audio[i].posValid = SUCCEEDED(posHr);
            if (g_audio[i].posValid) g_audio[i].pos = pos;
            IDirectSoundBuffer_Stop(b);
            g_audio[i].settled = g_audio[i].pos;
            if (g_audio[i].looping) {
                DWORD last = g_audio[i].pos, now = 0;
                int k, same = 0;
                for (k = 0; k < 20 && same < 3; k++) {
                    Sleep(1);
                    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(b, &now, NULL))) break;
                    if (k > 0 && now == last) same++; else same = 0;
                    last = now;
                }
                g_audio[i].settled = last;
                Log("PauseAudio: loop buffer %p saved play cursor %lu, settled at %lu after stop (hr=%08X)",
                    (void*)b, (unsigned long)g_audio[i].pos, (unsigned long)g_audio[i].settled, (unsigned)posHr);
            }
            n++;
        }
    }
    LeaveCriticalSection(&g_audioCs);
    Log("PauseAudio: paused %d of %d buffers", n, g_audioN);
}

static void AudioResume(int doPlay) {
    int i, n = 0;
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
                }
                IDirectSoundBuffer_GetCurrentPosition(b, &cur, NULL);
                IDirectSoundBuffer_Play(b, 0, 0, g_audio[i].looping ? DSBPLAY_LOOPING : 0);
                if (g_audio[i].looping) {
                    IDirectSoundBuffer_GetCurrentPosition(b, &after, NULL);
                    Log("PauseAudio: loop buffer %p resumed: saved=%lu settled=%lu before_play=%lu after_play=%lu",
                        (void*)b, (unsigned long)g_audio[i].pos, (unsigned long)g_audio[i].settled,
                        (unsigned long)cur, (unsigned long)after);
                    g_chkBuf = b;
                    g_chkPos = after;
                    g_chkTick = GetTickCount();
                }
                n++;
            }
        }
    }
    LeaveCriticalSection(&g_audioCs);
    Log("PauseAudio: %s %d buffers", doPlay ? "resumed" : "dropped (retry/title)", n);
}

static DWORD WINAPI PauseAudioThread(LPVOID unused) {
    int prev = 0;
    (void)unused;
    for (;;) {
        int cur;
        Sleep(5);
        if (g_chkBuf && GetTickCount() - g_chkTick >= 150) {
            int k;
            EnterCriticalSection(&g_audioCs);
            for (k = 0; k < g_audioN; k++) {
                if (g_audio[k].buf == g_chkBuf) {
                    DWORD now = 0, size = 0, adv, avg = 0;
                    DSBCAPS caps;
                    WAVEFORMATEX wf;
                    memset(&caps, 0, sizeof(caps));
                    caps.dwSize = sizeof(caps);
                    memset(&wf, 0, sizeof(wf));
                    IDirectSoundBuffer_GetCurrentPosition(g_chkBuf, &now, NULL);
                    if (SUCCEEDED(IDirectSoundBuffer_GetCaps(g_chkBuf, &caps))) size = caps.dwBufferBytes;
                    if (SUCCEEDED(IDirectSoundBuffer_GetFormat(g_chkBuf, &wf, sizeof(wf), NULL))) avg = wf.nAvgBytesPerSec;
                    adv = (now >= g_chkPos) ? now - g_chkPos : now + size - g_chkPos;
                    Log("PauseAudio: resume check: cursor advanced %lu bytes (%lu ms of audio) in %lu ms",
                        (unsigned long)adv, avg ? (unsigned long)((unsigned long long)adv * 1000 / avg) : 0UL,
                        (unsigned long)(GetTickCount() - g_chkTick));
                    break;
                }
            }
            LeaveCriticalSection(&g_audioCs);
            g_chkBuf = NULL;
        }
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

static const int g_SeFix = 1;
static const int g_SePanLimit = 0;
static const int g_SeTrace = 0;
static int g_seTraceCount = 0;
#define SE_TRACE_MAX 4000
#define SE_TRACE_RARE 20

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

    if (g_exMuteSe || g_exSimulatingStageFrame) return;   /* silent during the practice fast-forward */
    if (!bufs || count <= 0) return;
    if (idx < 0 || idx >= count) idx = 0;

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
    p[0] = 0xe9;
    memcpy(p + 1, &rel, 4);
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    Log("SeFix: patched (hook=%p)", (void*)&SePlayHook);
}

static const int g_BatchDraw = 1;
#define BATCH_MAX_QUADS 1024
#define BATCH_FVF 0x1c4
#define BATCH_VTX_DWORDS 8

typedef HRESULT (STDMETHODCALLTYPE *PF_DevDrawPrim)(void*, DWORD, DWORD, LPVOID, DWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PF_DevSetTexture)(void*, DWORD, void*);

static PF_DevDrawPrim g_devDrawPrim = NULL;
static PF_DevSetTexture g_devSetTexture = NULL;
static void *g_devSelf = NULL;
static unsigned g_bverts[BATCH_MAX_QUADS * 6 * BATCH_VTX_DWORDS];
static int g_bquads = 0;
static int g_batchFailed = 0;
static void *g_texApplied = NULL;
static int g_texKnown = 0;
static void *g_texPending = NULL;
static int g_texHasPending = 0;
static unsigned char *g_stubMem = NULL;
static unsigned g_stubUsed = 0;

static void BatchApplyTexture(void) {
    if (!g_texHasPending) return;
    g_texHasPending = 0;
    if (g_devSelf && g_devSetTexture && (!g_texKnown || g_texPending != g_texApplied)) {
        g_devSetTexture(g_devSelf, 0, g_texPending);
        g_texApplied = g_texPending;
        g_texKnown = 1;
        g_stTexApplied++;
    } else {
        g_stTexSkipped++;
    }
    if (g_texPending) {
        IDirectDrawSurface7_Release((IDirectDrawSurface7*)g_texPending);
        g_texPending = NULL;
    }
}

/* Draws the queued quads.  The count is cleared first so that a state change made while the draw is
   being issued (D7_BatchFlush -> here) cannot draw them twice. */
static void BatchFlushQuads(void) {
    int q = g_bquads;
    if (q > 0) {
        g_bquads = 0;
        if (g_devSelf && g_devDrawPrim) {
            g_devDrawPrim(g_devSelf, 4 , BATCH_FVF, g_bverts, (DWORD)q * 6, 0);
            g_stBatchCalls++;
        }
    }
}

static void BatchFlush(void) {
    BatchFlushQuads();
    BatchApplyTexture();
}

static HRESULT STDMETHODCALLTYPE BatchDrawPrimitive(void *self, DWORD type, DWORD fvf, LPVOID verts,
                                                    DWORD count, DWORD flags) {
    g_devSelf = self;
    if (type == 6  && fvf == BATCH_FVF && count == 4 && flags == 0 && verts) {
        const unsigned *s = (const unsigned*)verts;
        unsigned *d;
        if (g_texHasPending) {
            if (!g_texKnown || g_texPending != g_texApplied) BatchFlush();
            else BatchApplyTexture();
        }
        if (g_bquads >= BATCH_MAX_QUADS) BatchFlush();
        d = g_bverts + (size_t)g_bquads * 6 * BATCH_VTX_DWORDS;
        memcpy(d, s, 3 * BATCH_VTX_DWORDS * 4);
        memcpy(d + 3 * BATCH_VTX_DWORDS, s, BATCH_VTX_DWORDS * 4);
        memcpy(d + 4 * BATCH_VTX_DWORDS, s + 2 * BATCH_VTX_DWORDS, 2 * BATCH_VTX_DWORDS * 4);
        g_bquads++;
        g_stQuads++;
        return 0;
    }
    BatchFlush();
    g_stPass++;
    return g_devDrawPrim(self, type, fvf, verts, count, flags);
}

static HRESULT STDMETHODCALLTYPE BatchSetTexture(void *self, DWORD stage, void *tex) {
    g_devSelf = self;
    if (stage != 0) {
        BatchFlush();
        return g_devSetTexture(self, stage, tex);
    }
    if (g_texHasPending && g_texPending)
        IDirectDrawSurface7_Release((IDirectDrawSurface7*)g_texPending);
    if (tex) IDirectDrawSurface7_AddRef((IDirectDrawSurface7*)tex);
    g_texPending = tex;
    g_texHasPending = 1;
    return 0;
}

static void *BatchMakeStub(void *orig) {
    unsigned char *p;
    DWORD rel, holder;

    if (!g_stubMem) {
        g_stubMem = (unsigned char*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_stubUsed = 0;
    }
    if (!g_stubMem || g_stubUsed + 32 > 4096) return NULL;
    p = g_stubMem + g_stubUsed;
    g_stubUsed += 32;
    p[0] = 0x60; p[1] = 0x9c; p[2] = 0xe8;
    rel = (DWORD)(uintptr_t)&BatchFlush - (DWORD)(uintptr_t)(p + 7);
    memcpy(p + 3, &rel, 4);
    p[7] = 0x9d; p[8] = 0x61; p[9] = 0xff; p[10] = 0x25;
    holder = (DWORD)(uintptr_t)(p + 16);
    memcpy(p + 11, &holder, 4);
    memcpy(p + 16, &orig, 4);
    return p;
}

static int BatchStubSlots(void **vt, const unsigned char *slots, int n) {
    int i, ok = 0;
    for (i = 0; i < n; i++) {
        void *stub = BatchMakeStub(vt[slots[i]]);
        void *old = NULL;
        if (stub && PatchVtblSlot(&vt[slots[i]], stub, &old)) ok++;
    }
    return ok;
}

static int D7_EmuActive(void);

static void BatchTryInstall(void) {

    static const unsigned char devSlots[] = { 5, 6, 8, 10, 11, 13, 14, 16, 18, 20, 26, 29, 30, 31, 32,
                                              34, 37, 39, 43, 44, 45 };

    static const unsigned char surfSlots[] = { 6, 7, 17, 25, 26, 27, 32 };
    static const unsigned char surfBltFlip[] = { 5, 11 };
    void **obj, **vt, **svt;
    void *dev, *old = NULL;
    int nd, ns = 0;

    if (g_batchInstalled || g_batchFailed || !g_BatchDraw) return;
    obj = *(void***)(uintptr_t)0x46c85c;
    if (!obj || IsBadReadPtr(obj, 8)) return;
    dev = obj[1];
    if (!dev || IsBadReadPtr(dev, 4)) return;
    vt = *(void***)dev;
    if (!vt || IsBadReadPtr(vt, 48 * sizeof(void*))) { g_batchFailed = 1; return; }
    if (!g_primary) return;
    svt = *(void***)g_primary;
    if (!svt || IsBadReadPtr(svt, 40 * sizeof(void*))) { g_batchFailed = 1; return; }

    g_devSelf = dev;
    if (D7_EmuActive()) {
        /* In the Direct3D9 layer a render / texture-stage state change flushes the batch itself, and only
           when the value really changes, so the always-flush stubs on those two slots (20, 37) are left out:
           redundant state calls no longer split the sprite batches. */
        static const unsigned char devSlotsEmu[] = { 5, 6, 8, 10, 11, 13, 14, 16, 18, 26, 29, 30, 31, 32,
                                                     34, 39, 43, 44, 45 };
        nd = BatchStubSlots(vt, devSlotsEmu, (int)sizeof(devSlotsEmu));
    } else {
        nd = BatchStubSlots(vt, devSlots, (int)sizeof(devSlots));
    }
    ns = BatchStubSlots(svt, surfSlots, (int)sizeof(surfSlots));
    if (!g_origBlt) ns += BatchStubSlots(svt, surfBltFlip, (int)sizeof(surfBltFlip));
    if (!PatchVtblSlot(&vt[35], (void*)&BatchSetTexture, &old)) { g_batchFailed = 1; return; }
    g_devSetTexture = (PF_DevSetTexture)old;
    if (!PatchVtblSlot(&vt[25], (void*)&BatchDrawPrimitive, &old)) {
        g_batchFailed = 1;
        return;
    }
    g_devDrawPrim = (PF_DevDrawPrim)old;
    g_batchInstalled = 1;
    Log("Batch: device %p (vtbl %p) hooked: %d flush slots, %d surface slots, DrawPrimitive + SetTexture",
        dev, (void*)vt, nd, ns);
}

static const int g_FrameWaitSleep = 1;
static const double g_FrameWaitMarginMs = 2.0;
static const double g_FrameWaitMarginHiResMs = 0.7;

/* A high-resolution waitable timer (Windows 10 1803+) wakes up within ~0.5 ms, so the frame wait can sleep
   almost the whole remaining time instead of leaving 2 ms for busy-waiting.  Without it the 1 ms system
   timer resolution is requested and the old margin is kept. */
static HANDLE g_fwTimer = NULL;
static int g_fwTimerInit = 0;
static int g_fwHiRes = 0;

static void FrameWaitInit(void) {
    typedef HANDLE (WINAPI *PF_CWTEx)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    typedef UINT (WINAPI *PF_TBP)(UINT);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    PF_CWTEx cw = k ? (PF_CWTEx)(void*)GetProcAddress(k, "CreateWaitableTimerExW") : NULL;

    g_fwTimerInit = 1;
    if (cw) {
        g_fwTimer = cw(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (g_fwTimer) g_fwHiRes = 1;
    }
    if (!g_fwHiRes) {
        HMODULE mm = LoadLibraryA("winmm.dll");
        PF_TBP tbp = mm ? (PF_TBP)(void*)GetProcAddress(mm, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
    }
    Log("FrameWait: %s", g_fwHiRes ? "high-resolution waitable timer" : "MsgWait with 1 ms timer resolution");
}

typedef unsigned (__attribute__((fastcall)) *PF_FrameTimer)(int *ctx);

static unsigned __attribute__((fastcall)) FrameTimerHook(int *ctx) {
    unsigned r;
    BatchTryInstall();
    r = ((PF_FrameTimer)(uintptr_t)0x424f20)(ctx);
    if (g_FrameWaitSleep && (r & 0xff) == 0) {
        unsigned acc = (unsigned)ctx[3], dacc = (unsigned)ctx[4], per = (unsigned)ctx[1];
        double rem = 30000.0 - (double)acc;
        if (per > 0 && per < 1000000) {
            double d = (double)per - (double)dacc;
            if (d < rem) rem = d;
        }
        double margin;
        rem /= 1800.0;
        if (!g_fwTimerInit) FrameWaitInit();
        margin = g_fwHiRes ? g_FrameWaitMarginHiResMs : g_FrameWaitMarginMs;
        if (rem > margin + (g_fwHiRes ? 0.5 : 1.0) && rem < 100.0) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            if (g_fwHiRes) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((rem - margin) * 10000.0);
                if (SetWaitableTimer(g_fwTimer, &due, 0, NULL, NULL, FALSE))
                    MsgWaitForMultipleObjects(1, &g_fwTimer, FALSE, INFINITE, QS_ALLINPUT);
            } else {
                MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(rem - margin), QS_ALLINPUT);
            }
            QueryPerformanceCounter(&b);
            g_fwSleeps++;
            g_fwSleptMs += TicksToMs(b.QuadPart - a.QuadPart);
        } else {
            g_fwSpins++;
        }
    }
    return r;
}

static void PatchFrameWait(void) {
    DWORD a;
    int n = 0;

    if (!g_FrameWaitSleep && !g_BatchDraw) return;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("FrameWait: unexpected image base, skipped");
        return;
    }
    for (a = 0x425330; a < 0x425570; a++)
        if (*(unsigned char*)(uintptr_t)a == 0xe8 &&
            (DWORD)(a + 5 + *(DWORD*)(uintptr_t)(a + 1)) == 0x424f20)
            n += HookCall(a, 0x424f20, (void*)&FrameTimerHook);
    Log("FrameWait: %d call site(s) hooked (margin %.1f ms)", n, g_FrameWaitMarginMs);
}

static void PatchKeepRunningInactive(void) {
    static const unsigned char orig[5] = { 0xa2, 0x54, 0x8d, 0x45, 0x00 };
    static const unsigned char patch[5] = { 0xb0, 0x01, 0x90, 0x90, 0x90 };
    unsigned char *p = (unsigned char*)0x40638a;
    DWORD old;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("KeepActive: unexpected image base, skipped");
        return;
    }
    if (memcmp(p, orig, sizeof(orig)) != 0) {
        Log("KeepActive: code signature mismatch, skipped");
        return;
    }
    if (!VirtualProtect(p, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
        Log("KeepActive: VirtualProtect failed (err=%08X)", (unsigned)GetLastError());
        return;
    }
    memcpy(p, patch, sizeof(patch));
    VirtualProtect(p, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(patch));
    Log("KeepActive: game active-state flag is held while the window is unfocused");
}

/* ------------------------------------------------------------------------
   Modern controller support.  Rain.exe only speaks the legacy winmm joystick
   API (joyGetDevCapsA 0x4532c8, joyGetPosEx 0x4532c4).  Windows 8+ excludes
   XInput devices (Xbox 360/One/Series, modern pads) from that API, so they are
   invisible to the game.  We replace those two imports with shims backed by
   XInputGetState:

   - joyGetDevCapsA(id): succeeds only for connected XInput pads, id 0..3 ->
     XInput user 0..3, reporting 16 buttons and 6 axes in the 0..0xFFFF range
     the game's own thresholds (0x4315c0: center +/- 0x4e20) expect.
   - joyGetPosEx(id): polls XInput and fills JOYINFOEX.  X (right+) and Y
     (down+) use the winmm convention, POV holds the d-pad in winmm degrees,
     and buttons map to JOY_BUTTON1..16 bits as listed in the table below.

   The game already supports such regular joystick hardware, so no further
   patching is needed: its enumeration loop (0x431130), the frame poll
   (0x430eab) and the config UI all keep working unchanged.
   ------------------------------------------------------------------------ */
typedef unsigned (__stdcall *PF_XInputGetState)(unsigned id, void *state);
typedef MMRESULT (WINAPI *PF_JoyGetCapsA)(UINT id, LPJOYCAPSA caps, UINT cb);
typedef MMRESULT (WINAPI *PF_JoyGetPosEx)(UINT id, LPJOYINFOEX info);

#define XI_ID_VK_A            0x1000u
#define XI_ID_VK_B            0x2000u
#define XI_ID_VK_X            0x4000u
#define XI_ID_VK_Y            0x8000u
#define XI_ID_SHOULDER_L      0x0100u
#define XI_ID_SHOULDER_R      0x0200u
#define XI_ID_VK_BACK         0x0020u
#define XI_ID_VK_START        0x0010u
#define XI_ID_VK_STICK_L      0x0040u
#define XI_ID_VK_STICK_R      0x0080u
#define XI_ID_DPAD_UP         0x0001u
#define XI_ID_DPAD_DOWN       0x0002u
#define XI_ID_DPAD_LEFT       0x0004u
#define XI_ID_DPAD_RIGHT      0x0008u

static unsigned char g_joyIoInited = 0;
static unsigned char g_joyIoOk = 0;
static PF_XInputGetState g_xiGetState = NULL;
static PF_JoyGetCapsA g_winmmJoyGetCapsA = NULL;
static PF_JoyGetPosEx g_winmmJoyGetPosEx = NULL;
static unsigned g_joyCapsCount = 0;
static unsigned char g_joyPlugged[4];

static void JoyIoInit(void) {
    HMODULE xi = NULL;
    const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int i;

    g_joyIoInited = 1;
    for (i = 0; i < 3; i++) {
        xi = LoadLibraryA(names[i]);
        if (xi) break;
    }
    if (!xi) {
        Log("JoyBridge: XInput not found, leaving winmm untouched");
        return;
    }
    g_xiGetState = (PF_XInputGetState)(void*)GetProcAddress(xi, (LPCSTR)"XInputGetState");
    if (!g_xiGetState) {
        Log("JoyBridge: XInputGetState missing in %s, leaving winmm untouched", names[i]);
        g_xiGetState = NULL;
        return;
    }
    g_joyIoOk = 1;
    Log("JoyBridge: XInput ready via %s", names[i]);
}

/* id -> XInput user, refresh the connection state at most once per call */
static unsigned JoyConnected(unsigned id) {
    struct { unsigned reply; unsigned char pad[16]; } st;
    unsigned r;

    if (id >= 4) return 0;
    memset(&st, 0, sizeof(st));
    r = g_xiGetState(id, &st.pad);
    g_joyPlugged[id] = (r == 0);
    return (r == 0);
}

static unsigned __stdcall JoyGetCapsA(unsigned id, JOYCAPSA *jc, unsigned cb) {
    if (!g_joyIoInited) JoyIoInit();
    if (!g_joyIoOk || !jc || cb < sizeof(JOYCAPSA) || !JoyConnected(id))
        return g_winmmJoyGetCapsA ? g_winmmJoyGetCapsA(id, jc, cb) : JOYERR_UNPLUGGED;
    memset(jc, 0, sizeof(*jc));
    jc->wMid = 0x28E;                  /* Microsoft */
    jc->wPid = 0x2FF;                  /* Xbox 360 wired */
    strcpy(jc->szPname, "Modern Controller (XInput)");
    jc->wXmin = 0;      jc->wXmax = 0xFFFF;
    jc->wYmin = 0;      jc->wYmax = 0xFFFF;
    jc->wZmin = 0;      jc->wZmax = 0xFFFF;
    jc->wRmin = 0;      jc->wRmax = 0xFFFF;
    jc->wUmin = 0;      jc->wUmax = 0xFFFF;
    jc->wVmin = 0;      jc->wVmax = 0xFFFF;
    jc->wPeriodMin = 10000;
    jc->wPeriodMax = 10000;
    jc->wCaps = 0;                     /* plain stick, no rudder/POV special bits */
    jc->wMaxButtons = 32;
    jc->wNumButtons = 16;
    jc->wMaxAxes = 6;
    jc->wNumAxes = 6;
    return JOYERR_NOERROR;
}

static unsigned __stdcall JoyGetPosEx(unsigned id, JOYINFOEX *ji) {
    if (!g_joyIoInited) JoyIoInit();
    if (!g_joyIoOk || !ji || ji->dwSize < sizeof(JOYINFOEX) || !JoyConnected(id))
        return g_winmmJoyGetPosEx ? g_winmmJoyGetPosEx(id, ji) : JOYERR_UNPLUGGED;

    /* XINPUT_STATE: dwPacketNumber(+0), gamepad +4: wButtons(+4) bLTrigger(+6)
       bRTrigger(+7) sLX(+8) sLY(+A) sRX(+C) sRY(+E) -> 16 bytes total */
    unsigned char *st = (unsigned char*)(void*)(ji + 1);
    unsigned char buf[64];
    unsigned short btn;
    short sLX, sLY, sRX, sRY;
    unsigned char lt, rt;
    unsigned dwButtons;

    memset(buf, 0, sizeof(buf));
    if (g_xiGetState(id, buf + 4) != 0) {
        g_joyPlugged[id] = 0;
        return JOYERR_UNPLUGGED;
    }
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    st = buf;
    btn = (unsigned short)(unsigned)*(unsigned short*)(st + 4 + 0);
    lt  = st[4 + 2];
    rt  = st[4 + 3];
    sLX = *(short*)(st + 4 + 8);
    sLY = *(short*)(st + 4 + 0xA);
    sRX = *(short*)(st + 4 + 0xC);
    sRY = *(short*)(st + 4 + 0xE);
    g_joyPlugged[id] = 1;

    /* XInput button -> JOY_BUTTON1..16 bit order */
    dwButtons = 0;
    if (btn & XI_ID_VK_A)               dwButtons |= JOY_BUTTON1;
    if (btn & XI_ID_VK_B)               dwButtons |= JOY_BUTTON2;
    if (btn & XI_ID_VK_X)               dwButtons |= JOY_BUTTON3;
    if (btn & XI_ID_VK_Y)               dwButtons |= JOY_BUTTON4;
    if (btn & XI_ID_VK_START)           dwButtons |= JOY_BUTTON5;
    if (btn & XI_ID_VK_BACK)            dwButtons |= JOY_BUTTON6;
    if (btn & XI_ID_SHOULDER_L)         dwButtons |= JOY_BUTTON7;
    if (btn & XI_ID_SHOULDER_R)         dwButtons |= JOY_BUTTON8;
    if (btn & XI_ID_VK_STICK_L)         dwButtons |= JOY_BUTTON9;
    if (btn & XI_ID_VK_STICK_R)         dwButtons |= JOY_BUTTON10;
    if (btn & XI_ID_DPAD_UP)            dwButtons |= JOY_BUTTON11;
    if (btn & XI_ID_DPAD_DOWN)          dwButtons |= JOY_BUTTON12;
    if (btn & XI_ID_DPAD_LEFT)          dwButtons |= JOY_BUTTON13;
    if (btn & XI_ID_DPAD_RIGHT)         dwButtons |= JOY_BUTTON14;
    if (lt >= 0x40)                     dwButtons |= JOY_BUTTON15;
    if (rt >= 0x40)                     dwButtons |= JOY_BUTTON16;

    {   /* -32768..32767 -> 0..0xFFFF; X: right = large, Y: down = large */
        unsigned short sx = (unsigned short)(unsigned)sLX;
        unsigned short sy = (unsigned short)(unsigned)sLY;
        unsigned short sz = (unsigned short)(unsigned)sRX;
        unsigned short sr = (unsigned short)(unsigned)sRY;

        ji->dwXpos = (DWORD)sx + 0x8000u;
        ji->dwYpos = (DWORD)sy + 0x8000u;
        ji->dwZpos = (DWORD)sz + 0x8000u;
        ji->dwRpos = (DWORD)sr + 0x8000u;
    }
    ji->dwUpos = (DWORD)(lt ? (unsigned)lt * 0x101 : 0);
    ji->dwVpos = (DWORD)(rt ? (unsigned)rt * 0x101 : 0);
    ji->dwButtons = dwButtons;
    {
        int n = 0;
        while (dwButtons) { dwButtons &= (dwButtons - 1); n++; }
        ji->dwButtonNumber = (DWORD)n;
    }
    if (btn & XI_ID_DPAD_UP)            ji->dwPOV = 0;          /* north */
    else if (btn & XI_ID_DPAD_RIGHT)    ji->dwPOV = 9000;
    else if (btn & XI_ID_DPAD_DOWN)     ji->dwPOV = 18000;
    else if (btn & XI_ID_DPAD_LEFT)     ji->dwPOV = 27000;
    else                                ji->dwPOV = 0xFFFF;     /* centered */
    return JOYERR_NOERROR;
}

static void PatchModernController(void) {
    void **pPos = (void**)(uintptr_t)0x4532c4;
    void **pCaps = (void**)(uintptr_t)0x4532c8;
    DWORD old = 0, old2 = 0;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("JoyBridge: unexpected image base, skipped");
        return;
    }
    if (!pPos || !pCaps || !*pPos || !*pCaps) {
        Log("JoyBridge: winmm joystick entry points are unavailable");
        return;
    }
    if (*pPos == (void *)&JoyGetPosEx && *pCaps == (void *)&JoyGetCapsA) return;
    g_winmmJoyGetPosEx = (PF_JoyGetPosEx)*pPos;
    g_winmmJoyGetCapsA = (PF_JoyGetCapsA)*pCaps;
    if (!VirtualProtect(pPos, 8, PAGE_READWRITE, &old) ||
        !VirtualProtect(pCaps, 8, PAGE_READWRITE, &old2)) {
        Log("JoyBridge: VirtualProtect failed (err=%08X)", (unsigned)GetLastError());
        if (old) VirtualProtect(pPos, 8, old, &old);
        return;
    }
    *pPos = (void *)&JoyGetPosEx;
    *pCaps = (void *)&JoyGetCapsA;
    VirtualProtect(pPos, 8, old, &old);
    VirtualProtect(pCaps, 8, old2, &old2);
    Log("JoyBridge: dynamic XInput hotplug hooks installed; disconnected devices use winmm");
}

/* ------------------------------------------------------------------------
   API census (diagnostic).
   Windows 11 25H2's ddraw.dll does not expose IDirect3D7, so the game needs a
   Direct3D7 -> Direct3D9 layer.  Before writing one, record exactly which
   DirectDraw7 / Surface7 / Direct3D7 / Device7 methods (and which arguments)
   the game uses.  Every vtable slot (except IUnknown) is replaced with a small
   stub that logs the call and jumps to the original.  Run the game once on a
   system where D3D7 works (e.g. Wine) and read "Api[...]" / "ApiSummary[...]"
   lines in Smd_ddraw.log.
   ------------------------------------------------------------------------ */
#ifndef SMD_API_CENSUS
#define SMD_API_CENSUS 0
#endif
static const int g_ApiCensus = SMD_API_CENSUS;

enum { CEN_DD7 = 0, CEN_D3D7 = 1, CEN_DEV = 2, CEN_SURF = 3, CEN_IFACES = 4 };

static const char *const g_cenIfName[CEN_IFACES] = { "dd7", "d3d7", "dev", "surf" };
static const char *const g_cenNameDD7[30] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", "StartModeTest", "EvaluateMode"
};
static const char *const g_cenNameD3D7[8] = {
    "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateDevice", "CreateVertexBuffer",
    "EnumZBufferFormats", "EvictManagedTextures"
};
static const char *const g_cenNameDev[49] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats", "BeginScene", "EndScene",
    "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear", "SetTransform", "GetTransform",
    "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock", "PreLoad", "DrawPrimitive",
    "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus", "DrawPrimitiveStrided",
    "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice",
    "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load", "LightEnable",
    "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo"
};
static const char *const g_cenNameSurf[49] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch",
    "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock",
    "PageUnlock", "SetSurfaceDesc", "SetPrivateData", "GetPrivateData", "FreePrivateData",
    "GetUniquenessValue", "ChangeUniquenessValue", "SetPriority", "GetPriority", "SetLOD", "GetLOD"
};

static const char *CensusName(unsigned iface, unsigned slot) {
    switch (iface) {
    case CEN_DD7:  return slot < 30 ? g_cenNameDD7[slot] : "?";
    case CEN_D3D7: return slot < 8 ? g_cenNameD3D7[slot] : "?";
    case CEN_DEV:  return slot < 49 ? g_cenNameDev[slot] : "?";
    case CEN_SURF: return slot < 49 ? g_cenNameSurf[slot] : "?";
    }
    return "?";
}

#define CEN_SLOTS 64
#define CEN_SEEN_N 8192
typedef struct { unsigned tag, a[3]; unsigned char used; } CenSeen;

static CRITICAL_SECTION g_cenCs;
static unsigned g_cenCount[CEN_IFACES][CEN_SLOTS];
static unsigned g_cenLogged[CEN_IFACES][CEN_SLOTS];
static CenSeen g_cenSeen[CEN_SEEN_N];
static unsigned g_cenSeenUsed = 0;
static unsigned char *g_cenStubMem = NULL;
static unsigned g_cenStubUsed = 0;
static void **g_cenVt[16];
static int g_cenVtN = 0;

static void CensusInit(void) {
    InitializeCriticalSection(&g_cenCs);
}

/* Called from the stubs: tag = (iface << 8) | slot, argp -> {this, arg1, arg2, ...}. */
static void __cdecl CensusHit(unsigned tag, unsigned *argp) {
    unsigned iface = tag >> 8, slot = tag & 0xff;
    unsigned a1 = argp[1], a2 = argp[2], a3 = argp[3];
    unsigned cap, h, i;

    if (iface >= CEN_IFACES || slot >= CEN_SLOTS) return;
    EnterCriticalSection(&g_cenCs);
    g_cenCount[iface][slot]++;
    cap = (iface == CEN_DEV && (slot == 20 || slot == 37)) ? 512 : 48;
    if (g_cenLogged[iface][slot] < cap && g_cenSeenUsed < CEN_SEEN_N * 3 / 4) {
        h = (tag * 2654435761u) ^ (a1 * 40503u) ^ (a2 * 2246822519u) ^ (a3 * 3266489917u);
        for (i = h % CEN_SEEN_N; ; i = (i + 1) % CEN_SEEN_N) {
            CenSeen *s = &g_cenSeen[i];
            if (!s->used) {
                s->used = 1;
                s->tag = tag;
                s->a[0] = a1; s->a[1] = a2; s->a[2] = a3;
                g_cenSeenUsed++;
                g_cenLogged[iface][slot]++;
                Log("Api[%s] %s #%u this=%08X args=%08X %08X %08X %08X %08X", g_cenIfName[iface],
                    CensusName(iface, slot), g_cenCount[iface][slot], argp[0], a1, a2, a3, argp[4], argp[5]);
                break;
            }
            if (s->tag == tag && s->a[0] == a1 && s->a[1] == a2 && s->a[2] == a3) break;
        }
    }
    LeaveCriticalSection(&g_cenCs);
}

static void CensusSummary(void) {
    unsigned iface, slot;
    if (!g_ApiCensus) return;
    EnterCriticalSection(&g_cenCs);
    for (iface = 0; iface < CEN_IFACES; iface++)
        for (slot = 0; slot < CEN_SLOTS; slot++)
            if (g_cenCount[iface][slot])
                Log("ApiSummary[%s] %s calls=%u", g_cenIfName[iface], CensusName(iface, slot),
                    g_cenCount[iface][slot]);
    LeaveCriticalSection(&g_cenCs);
}

static void CensusSummaryPeriodic(void) {
    static int n = 0;
    if (g_ApiCensus && ++n % 6 == 0) CensusSummary();
}

/* pushad; pushfd; lea eax,[esp+28h]; push eax; push tag; call CensusHit;
   add esp,8; popfd; popad; jmp [orig] */
static void *CensusMakeStub(unsigned tag, void *orig) {
    unsigned char *p;
    DWORD rel, holder;

    if (!g_cenStubMem) {
        g_cenStubMem = (unsigned char*)VirtualAlloc(NULL, 16384, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE);
        g_cenStubUsed = 0;
    }
    if (!g_cenStubMem || g_cenStubUsed + 48 > 16384) return NULL;
    p = g_cenStubMem + g_cenStubUsed;
    g_cenStubUsed += 48;
    p[0] = 0x60; p[1] = 0x9c;
    p[2] = 0x8d; p[3] = 0x44; p[4] = 0x24; p[5] = 0x28;
    p[6] = 0x50;
    p[7] = 0x68; memcpy(p + 8, &tag, 4);
    p[12] = 0xe8;
    rel = (DWORD)(uintptr_t)&CensusHit - (DWORD)(uintptr_t)(p + 17);
    memcpy(p + 13, &rel, 4);
    p[17] = 0x83; p[18] = 0xc4; p[19] = 0x08;
    p[20] = 0x9d; p[21] = 0x61;
    p[22] = 0xff; p[23] = 0x25;
    holder = (DWORD)(uintptr_t)(p + 32);
    memcpy(p + 24, &holder, 4);
    memcpy(p + 32, &orig, 4);
    return p;
}

static int CensusVtblDone(void **vt) {
    int i;
    for (i = 0; i < g_cenVtN; i++)
        if (g_cenVt[i] == vt) return 1;
    if (g_cenVtN < 16) g_cenVt[g_cenVtN++] = vt;
    return 0;
}

/* Replace slots 3..n-1 of vt (except those set in skip) with logging stubs. */
static void CensusHookVtbl(void **vt, unsigned iface, unsigned n, unsigned long long skip) {
    unsigned slot;
    int ok = 0;

    if (!g_ApiCensus || !vt || IsBadReadPtr(vt, n * sizeof(void*))) return;
    if (CensusVtblDone(vt)) return;
    for (slot = 3; slot < n; slot++) {
        void *stub, *old = NULL;
        if (skip & (1ULL << slot)) continue;
        stub = CensusMakeStub((iface << 8) | slot, vt[slot]);
        if (stub && PatchVtblSlot(&vt[slot], stub, &old)) ok++;
    }
    Log("Api: %s vtable %p hooked (%d slots)", g_cenIfName[iface], (void*)vt, ok);
}

static void CensusHookSurface(void *surf) {
    if (g_ApiCensus && surf) CensusHookVtbl(*(void***)surf, CEN_SURF, 49, 0);
}

static void CensusHookDevice(void *dev) {
    if (g_ApiCensus && dev) CensusHookVtbl(*(void***)dev, CEN_DEV, 49, 0);
}

typedef HRESULT (STDMETHODCALLTYPE *PF_CenCreateDevice)(void*, const GUID*, void*, void**);
static PF_CenCreateDevice g_cenOrigCreateDevice = NULL;

static HRESULT STDMETHODCALLTYPE CensusCreateDevice(void *self, const GUID *cls, void *surf, void **dev) {
    HRESULT hr = g_cenOrigCreateDevice(self, cls, surf, dev);
    Log("Api[d3d7] CreateDevice clsid=%08lX-%04X-%04X-%02X%02X surf=%p -> hr=%08X dev=%p",
        cls ? (unsigned long)cls->Data1 : 0UL, cls ? (unsigned)cls->Data2 : 0U, cls ? (unsigned)cls->Data3 : 0U,
        cls ? (unsigned)cls->Data4[0] : 0U, cls ? (unsigned)cls->Data4[1] : 0U, surf, (unsigned)hr,
        (dev ? *dev : NULL));
    if (hr >= 0 && dev && *dev) CensusHookDevice(*dev);
    return hr;
}

static void CensusHookD3D7(void *d3d) {
    void **vt, *old = NULL;

    if (!g_ApiCensus || !d3d) return;
    vt = *(void***)d3d;
    if (!vt || IsBadReadPtr(vt, 8 * sizeof(void*))) return;
    if (CensusVtblDone(vt)) return;
    g_cenOrigCreateDevice = (PF_CenCreateDevice)vt[4];
    PatchVtblSlot(&vt[4], (void*)&CensusCreateDevice, &old);
    {
        unsigned slot;
        int ok = 0;
        for (slot = 3; slot < 8; slot++) {
            void *stub, *o = NULL;
            if (slot == 4) continue;
            stub = CensusMakeStub((CEN_D3D7 << 8) | slot, vt[slot]);
            if (stub && PatchVtblSlot(&vt[slot], stub, &o)) ok++;
        }
        Log("Api: d3d7 vtable %p hooked (CreateDevice + %d slots)", (void*)vt, ok);
    }
}

/* ==========================================================================
   DirectDraw7 / Direct3D7 -> Direct3D9 emulation (SMD_D3D7_EMU)

   Windows 11 (build 26200) ddraw.dll does not hand out IDirect3D7, so the game
   cannot create its Direct3D7 device.  When that happens (or when the
   environment variable SMD_FORCE_D3D7_EMU is set, for testing on Wine), this
   layer replaces the system DirectDraw completely:

     - surfaces live in system memory (DIB sections, so GetDC works)
     - the Direct3D7 device is a Direct3D9 device on the game window
     - textures are converted to A8R8G8B8 Direct3D9 textures on demand, the
       DDCKEY_SRCBLT colour key becomes alpha 0
     - Blt(primary <- back buffer) presents the Direct3D9 back buffer

   Only the methods that the game was observed to call (see the "API census"
   above) are implemented; everything else returns DDERR_UNSUPPORTED or an
   harmless success.  The structure of the render/texture-stage state
   translation follows the approach of DxWrapper's Dd7to9 (zlib license,
   https://github.com/elishacloud/dxwrapper); no source from it is copied.
   ========================================================================== */
#ifndef SMD_D3D7_EMU
#define SMD_D3D7_EMU 1
#endif
static const int g_D7Emu = SMD_D3D7_EMU;
/* Create the Direct3D9 device as a pure device (no runtime state tracking/validation, no Get* calls): the
   layer filters redundant state itself.  Falls back to a normal hardware device if it cannot be created. */
static const int g_D7PureDevice = 1;

static const GUID k_D7_IID_Unknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID k_D7_IID_DD1  = { 0x6C14DB80, 0xA733, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID k_D7_IID_DD2  = { 0xB3A6F3E0, 0x2B43, 0x11CF, { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
static const GUID k_D7_IID_DD3  = { 0x618F8AD4, 0x8B7A, 0x11D0, { 0x8F, 0xCC, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D } };
static const GUID k_D7_IID_DD4  = { 0x9C59509A, 0x39BD, 0x11D1, { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 } };
static const GUID k_D7_IID_DD7  = { 0x15E65EC0, 0x3B9C, 0x11D2, { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } };
static const GUID k_D7_IID_D3D7 = { 0xF5049E77, 0x4861, 0x11D2, { 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8 } };
static const GUID k_D7_IID_Dev7 = { 0xF5049E79, 0x4861, 0x11D2, { 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8 } };
static const GUID k_D7_IID_Surf7 = { 0x06675A80, 0x3B9B, 0x11D2, { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B } };

static int D7_Iid(const GUID *a, const GUID *b) { return memcmp(a, b, sizeof(GUID)) == 0; }

typedef struct { DWORD m[4]; int sh[4], nb[4]; } D7Fmt;   /* R, G, B, A */

typedef struct D7Surf {
    void **vt;
    LONG ref;
    DWORD caps;
    int w, h, bpp, pitch;
    DDPIXELFORMAT pf;
    D7Fmt fmt;
    HBITMAP dib;
    unsigned char *bits;
    int ownBits;
    HDC dc;
    int hasKey;
    DWORD keyLo;
    int dirty;
    int dpart;          /* dirty region is only drc (otherwise the whole surface) */
    RECT drc;
    int lockRO, lockFull;
    RECT lockRc;
    IDirect3DTexture9 *tex;
    struct D7Surf *back, *zbuf;
    int isPrimary;
} D7Surf;

typedef struct { void **vt; LONG ref; } D7Obj;

static void *g_d7SurfVt[49];
static void *g_d7DDVt[30];
static void *g_d7D3DVt[8];
static void *g_d7DevVt[49];
static D7Obj g_d7DD  = { g_d7DDVt, 1 };
static D7Obj g_d7D3D = { g_d7D3DVt, 1 };
static D7Obj g_d7Dev = { g_d7DevVt, 1 };

static struct {
    int active;
    HWND hwnd;
    int dispW, dispH;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS pp;
    D7Surf *primary;
    D7Surf *back3d;
    D7Surf *curTex[8];
    DWORD rs[256];
    unsigned char rsSet[256];
    DWORD tss[8][32];
    unsigned char tssSet[8][32];
    DWORD smp[8][16];
    unsigned char smpSet[8][16];
    int ckEnable;
    unsigned warned;
    unsigned surfLogged;
    int inScene;
    int fs;            /* borderless fullscreen (game asked for DDSCL_FULLSCREEN, i.e. WndMode=off) */
    int fsCw, fsCh;    /* client size the borders were last painted for */
    IDirect3DStateBlock9 *sb;   /* unused (kept for the device reset path) */
    DWORD fvf;                  /* FVF currently set on the device (0 = unknown) */
    void *devTex[8];            /* texture currently set on the device per stage */
} g_d7 = { 0, NULL, 640, 480 };

/* Device state shadows: skip SetFVF / SetTexture calls that would not change anything. */
static void D7_SetFVF(DWORD fvf) {
    if (!g_d7.dev || g_d7.fvf == fvf) return;
    g_d7.fvf = fvf;
    IDirect3DDevice9_SetFVF(g_d7.dev, fvf);
}

static void D7_SetTex(DWORD stage, IDirect3DTexture9 *t) {
    if (!g_d7.dev || stage >= 8) return;
    if (g_d7.devTex[stage] == (void*)t) return;
    g_d7.devTex[stage] = (void*)t;
    IDirect3DDevice9_SetTexture(g_d7.dev, stage, (IDirect3DBaseTexture9*)t);
}

static int D7_EmuActive(void) { return g_d7.active; }

/* Draw batching (see D7_BatchFlush): consecutive sprite quads are collected here. */
#define D7_BATCH_FVF 0x1c4
#define D7_BATCH_MAX 1024
static DWORD g_d7bv[D7_BATCH_MAX * 6 * 8];
static unsigned g_d7bq = 0;
static D7Surf *g_d7bTex = NULL;
static IDirect3DTexture9 *D7_GetTex(D7Surf *s);
static void D7_BatchFlush(void) {
    IDirect3DDevice9 *dev = g_d7.dev;
    IDirect3DTexture9 *tex = NULL;
    unsigned quads = g_d7bq;
    BatchFlushQuads();   /* called before every real state change: the queued sprites use the old state */
    if (!quads) return;
    g_d7bq = 0;
    if (quads > D7_BATCH_MAX) quads = D7_BATCH_MAX;
    if (!dev) {
        g_d7bTex = NULL;
        return;
    }
    if (g_d7bTex) tex = D7_GetTex(g_d7bTex);
    D7_SetTex(0, tex);
    D7_SetFVF(D7_BATCH_FVF);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, quads * 2,
                                     g_d7bv, 8 * sizeof(DWORD));
    g_d7bTex = NULL;
}

#define D7W_BACKLOCK   0x01u
#define D7W_PRIMBLT    0x02u
#define D7W_BACKBLT    0x04u

static int D7_Warn(unsigned bit) {
    if (g_d7.warned & bit) return 0;
    g_d7.warned |= bit;
    return 1;
}

/* ---- pixel format helpers ------------------------------------------- */
static unsigned char g_d7Exp[9][256];   /* n-bit channel value -> 8-bit */
static int g_d7ExpInit = 0;

static void D7_FmtInit(D7Fmt *f, const DDPIXELFORMAT *pf) {
    int i;
    if (!g_d7ExpInit) {
        int nb, v;
        for (nb = 1; nb <= 8; nb++)
            for (v = 0; v < (1 << nb); v++)
                g_d7Exp[nb][v] = (unsigned char)((v * 255) / ((1 << nb) - 1));
        g_d7ExpInit = 1;
    }
    memset(f, 0, sizeof(*f));
    if (!pf) return;
    f->m[0] = pf->dwRBitMask;
    f->m[1] = pf->dwGBitMask;
    f->m[2] = pf->dwBBitMask;
    f->m[3] = (pf->dwFlags & DDPF_ALPHAPIXELS) ? pf->dwRGBAlphaBitMask : 0;
    for (i = 0; i < 4; i++) {
        if (f->m[i]) {
            f->sh[i] = __builtin_ctz(f->m[i]);
            f->nb[i] = __builtin_popcount(f->m[i] >> f->sh[i]);
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

static DWORD D7_FromArgb(const D7Fmt *f, DWORD argb) {
    DWORD c[4], raw = 0;
    int i;
    c[0] = (argb >> 16) & 255; c[1] = (argb >> 8) & 255; c[2] = argb & 255; c[3] = argb >> 24;
    for (i = 0; i < 4; i++) {
        if (f->nb[i]) {
            DWORD v = (c[i] * ((1u << f->nb[i]) - 1u) + 127u) / 255u;
            raw |= (v << f->sh[i]) & f->m[i];
        }
    }
    return raw;
}

static DWORD D7_Rd(const D7Surf *s, const unsigned char *p) {
    if (s->bpp == 32) return *(const DWORD*)p;
    if (s->bpp == 16) return *(const WORD*)p;
    return 0;
}

static void D7_Wr(const D7Surf *s, unsigned char *p, DWORD v) {
    if (s->bpp == 32) *(DWORD*)p = v;
    else if (s->bpp == 16) *(WORD*)p = (WORD)v;
}

static int D7_FmtSame(const D7Surf *a, const D7Surf *b) {
    return a->bpp == b->bpp && a->fmt.m[0] == b->fmt.m[0] && a->fmt.m[1] == b->fmt.m[1] &&
           a->fmt.m[2] == b->fmt.m[2] && a->fmt.m[3] == b->fmt.m[3];
}

static void D7_SetPf(DDPIXELFORMAT *pf, DWORD flags, DWORD bpp, DWORD r, DWORD g, DWORD b, DWORD a) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = flags;
    pf->dwRGBBitCount = bpp;
    pf->dwRBitMask = r;
    pf->dwGBitMask = g;
    pf->dwBBitMask = b;
    pf->dwRGBAlphaBitMask = a;
}

static const struct { DWORD flags, bpp, r, g, b, a; } k_d7Fmts[] = {
    { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000 },
    { DDPF_RGB,                    32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0 },
    { DDPF_RGB,                    16, 0x0000F800, 0x000007E0, 0x0000001F, 0 },
    { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x00007C00, 0x000003E0, 0x0000001F, 0x00008000 },
    { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x00000F00, 0x000000F0, 0x0000000F, 0x0000F000 },
    { DDPF_RGB,                    16, 0x00007C00, 0x000003E0, 0x0000001F, 0 }
};

/* ---- surfaces ------------------------------------------------------- */
static D7Surf *D7_SurfNew(int w, int h, const DDPIXELFORMAT *pf, DWORD caps) {
    D7Surf *s = (D7Surf*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->vt = g_d7SurfVt;
    s->ref = 1;
    s->caps = caps;
    s->w = w;
    s->h = h;
    if (pf) {
        s->pf = *pf;
        s->bpp = (pf->dwFlags & DDPF_RGB) ? (int)pf->dwRGBBitCount : 0;
    }
    D7_FmtInit(&s->fmt, pf);
    if (s->bpp == 16 || s->bpp == 32) {
        struct { BITMAPINFOHEADER hdr; DWORD m[3]; } bi;
        void *bits = NULL;
        memset(&bi, 0, sizeof(bi));
        bi.hdr.biSize = sizeof(BITMAPINFOHEADER);
        bi.hdr.biWidth = w;
        bi.hdr.biHeight = -h;
        bi.hdr.biPlanes = 1;
        bi.hdr.biBitCount = (WORD)s->bpp;
        bi.hdr.biCompression = BI_BITFIELDS;
        bi.m[0] = pf->dwRBitMask; bi.m[1] = pf->dwGBitMask; bi.m[2] = pf->dwBBitMask;
        s->dib = CreateDIBSection(NULL, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        s->bits = (unsigned char*)bits;
        s->pitch = ((w * s->bpp + 31) / 32) * 4;
        if (!s->bits) {
            s->dib = NULL;
            s->bits = (unsigned char*)calloc(1, (size_t)s->pitch * (size_t)h);
            s->ownBits = 1;
        }
        if (!s->bits) { free(s); return NULL; }
    }
    s->dirty = 1;
    return s;
}

static ULONG STDMETHODCALLTYPE ES_AddRef(void *self) {
    return (ULONG)InterlockedIncrement(&((D7Surf*)self)->ref);
}

static ULONG STDMETHODCALLTYPE ES_Release(void *self) {
    D7Surf *s = (D7Surf*)self;
    LONG r = InterlockedDecrement(&s->ref);
    int i;
    if (r > 0) return (ULONG)r;
    if (g_d7bTex == s) D7_BatchFlush();
    if (g_d7.primary == s) g_d7.primary = NULL;
    if (g_d7.back3d == s) g_d7.back3d = NULL;
    for (i = 0; i < 8; i++)
        if (g_d7.curTex[i] == s) g_d7.curTex[i] = NULL;
    if (s->back) ES_Release(s->back);
    if (s->zbuf) ES_Release(s->zbuf);
    if (s->dc) DeleteDC(s->dc);
    if (s->dib) DeleteObject(s->dib);
    else if (s->ownBits) free(s->bits);
    if (s->tex) IDirect3DTexture9_Release(s->tex);
    free(s);
    return 0;
}

/* Marks (part of) a surface as changed so that its texture is refreshed; partial marks are merged. */
static void D7_MarkDirty(D7Surf *s, const RECT *r) {
    RECT c;
    if (!r) {
        s->dirty = 1;
        s->dpart = 0;
        return;
    }
    c = *r;
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > s->w) c.right = s->w;
    if (c.bottom > s->h) c.bottom = s->h;
    if (c.right <= c.left || c.bottom <= c.top) return;
    if (!s->dirty) {
        s->dirty = 1;
        s->dpart = 1;
        s->drc = c;
    } else if (s->dpart) {
        if (c.left < s->drc.left) s->drc.left = c.left;
        if (c.top < s->drc.top) s->drc.top = c.top;
        if (c.right > s->drc.right) s->drc.right = c.right;
        if (c.bottom > s->drc.bottom) s->drc.bottom = c.bottom;
    }
}

/* 16-bit raw value -> A8R8G8B8 lookup table per pixel format (built on first use). */
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
    if (!t) return NULL;
    for (v = 0; v < 65536; v++) t[v] = D7_ToArgb(f, v);
    for (i = 0; i < 4; i++) cache[ncache].m[i] = f->m[i];
    cache[ncache].t = t;
    ncache++;
    return t;
}

/* Direct3D9 texture for a surface (A8R8G8B8, colour key -> alpha 0). */
static IDirect3DTexture9 *D7_GetTex(D7Surf *s) {
    D3DLOCKED_RECT lr;
    int x, y, bytes, n, partial;
    RECT rc;
    const DWORD *lut;

    if (!g_d7.dev || !s->bits || (s->bpp != 16 && s->bpp != 32)) return NULL;
    if (!s->tex) {
        HRESULT hr = IDirect3DDevice9_CreateTexture(g_d7.dev, (UINT)s->w, (UINT)s->h, 1, 0,
                                                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s->tex, NULL);
        if (FAILED(hr)) {
            Log("D3D7 emu: CreateTexture %dx%d failed hr=%08X", s->w, s->h, (unsigned)hr);
            s->tex = NULL;
            return NULL;
        }
        s->dirty = 1;
        s->dpart = 0;
    }
    if (s->dirty) {
        bytes = s->bpp / 8;
        partial = s->dpart;
        rc.left = 0; rc.top = 0; rc.right = s->w; rc.bottom = s->h;
        if (partial) rc = s->drc;
        n = rc.right - rc.left;
        lut = (s->bpp == 16) ? D7_Lut16(&s->fmt) : NULL;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(s->tex, 0, &lr, partial ? &rc : NULL, 0))) {
            for (y = rc.top; y < rc.bottom; y++) {
                const unsigned char *sp = s->bits + (size_t)y * (size_t)s->pitch + (size_t)rc.left * (size_t)bytes;
                DWORD *dp = (DWORD*)((unsigned char*)lr.pBits + (size_t)(y - rc.top) * (size_t)lr.Pitch);
                if (s->bpp == 32 && s->fmt.m[0] == 0x00FF0000u && s->fmt.m[1] == 0x0000FF00u &&
                    s->fmt.m[2] == 0x000000FFu && (s->fmt.m[3] == 0 || s->fmt.m[3] == 0xFF000000u)) {
                    if (!s->hasKey && s->fmt.m[3] == 0xFF000000u) {
                        memcpy(dp, sp, (size_t)n * sizeof(DWORD));
                        continue;
                    }
                    /* fast path: A8R8G8B8 / X8R8G8B8 */
                    DWORD m3 = s->fmt.m[3], addA = m3 ? 0u : 0xFF000000u, keyv = s->keyLo & ~m3;
                    const DWORD *sd = (const DWORD*)sp;
                    for (x = 0; x < n; x++) {
                        DWORD raw = sd[x];
                        DWORD argb = raw | addA;
                        if (s->hasKey && (raw & ~m3) == keyv) argb &= 0x00FFFFFFu;
                        dp[x] = argb;
                    }
                    continue;
                }
                for (x = 0; x < n; x++) {
                    DWORD raw = D7_Rd(s, sp + x * bytes);
                    DWORD argb = lut ? lut[raw & 0xFFFFu] : D7_ToArgb(&s->fmt, raw);
                    if (s->hasKey && (raw & ~s->fmt.m[3]) == (s->keyLo & ~s->fmt.m[3]))
                        argb &= 0x00FFFFFFu;
                    dp[x] = argb;
                }
            }
            IDirect3DTexture9_UnlockRect(s->tex, 0);
        }
        s->dirty = 0;
        s->dpart = 0;
    }
    return s->tex;
}

static void D7_Blit(D7Surf *d, const RECT *drIn, D7Surf *s, const RECT *srIn, int useKey) {
    RECT dr, sr;
    int sw, sh, dw, dh, x, y, sb, db, same;

    if (!d->bits || !s->bits || d->bpp < 16 || s->bpp < 16) return;
    if (drIn) dr = *drIn; else { dr.left = 0; dr.top = 0; dr.right = d->w; dr.bottom = d->h; }
    if (srIn) sr = *srIn; else { sr.left = 0; sr.top = 0; sr.right = s->w; sr.bottom = s->h; }
    sw = sr.right - sr.left; sh = sr.bottom - sr.top;
    dw = dr.right - dr.left; dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    sb = s->bpp / 8; db = d->bpp / 8;
    same = D7_FmtSame(d, s);
    for (y = 0; y < dh; y++) {
        int dy = dr.top + y, sy = sr.top + (int)((long long)y * sh / dh);
        unsigned char *dp;
        const unsigned char *sp;
        if (dy < 0 || dy >= d->h || sy < 0 || sy >= s->h) continue;
        dp = d->bits + (size_t)dy * (size_t)d->pitch;
        sp = s->bits + (size_t)sy * (size_t)s->pitch;
        if (same && !useKey && sw == dw && dr.left >= 0 && dr.left + dw <= d->w &&
            sr.left >= 0 && sr.left + sw <= s->w) {
            memcpy(dp + dr.left * db, sp + sr.left * sb, (size_t)dw * (size_t)db);
            continue;
        }
        for (x = 0; x < dw; x++) {
            int dx = dr.left + x, sx = sr.left + (int)((long long)x * sw / dw);
            DWORD raw;
            if (dx < 0 || dx >= d->w || sx < 0 || sx >= s->w) continue;
            raw = D7_Rd(s, sp + sx * sb);
            if (useKey && s->hasKey && (raw & ~s->fmt.m[3]) == (s->keyLo & ~s->fmt.m[3])) continue;
            if (!same) raw = D7_FromArgb(&d->fmt, D7_ToArgb(&s->fmt, raw));
            D7_Wr(d, dp + dx * db, raw);
        }
    }
    D7_MarkDirty(d, &dr);
}

static void D7_Fill(D7Surf *d, const RECT *rIn, DWORD raw) {
    RECT r;
    int x, y, db;
    if (!d->bits || d->bpp < 16) return;
    if (rIn) r = *rIn; else { r.left = 0; r.top = 0; r.right = d->w; r.bottom = d->h; }
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > d->w) r.right = d->w;
    if (r.bottom > d->h) r.bottom = d->h;
    db = d->bpp / 8;
    for (y = r.top; y < r.bottom; y++) {
        unsigned char *dp = d->bits + (size_t)y * (size_t)d->pitch;
        for (x = r.left; x < r.right; x++) D7_Wr(d, dp + x * db, raw);
    }
    D7_MarkDirty(d, &r);
}

/* ---- Direct3D9 device ------------------------------------------------ */
typedef IDirect3D9 *(WINAPI *PF_D7Create9)(UINT);

static void D7_ApplyRS(DWORD st, DWORD v) {
    if (st < 256 && g_d7.rsSet[st] && g_d7.rs[st] == v) return;
    D7_BatchFlush();
    if (st < 256) { g_d7.rs[st] = v; g_d7.rsSet[st] = 1; }
    IDirect3DDevice9_SetRenderState(g_d7.dev, (D3DRENDERSTATETYPE)st, v);
}

static void D7_ApplyTss(DWORD stage, DWORD type, DWORD v) {
    if (stage < 8 && type < 32 && g_d7.tssSet[stage][type] && g_d7.tss[stage][type] == v) return;
    D7_BatchFlush();
    if (stage < 8 && type < 32) { g_d7.tss[stage][type] = v; g_d7.tssSet[stage][type] = 1; }
    IDirect3DDevice9_SetTextureStageState(g_d7.dev, stage, (D3DTEXTURESTAGESTATETYPE)type, v);
}

static void D7_ApplySamp(DWORD stage, DWORD type, DWORD v) {
    if (stage < 8 && type < 16 && g_d7.smpSet[stage][type] && g_d7.smp[stage][type] == v) return;
    D7_BatchFlush();
    if (stage < 8 && type < 16) { g_d7.smp[stage][type] = v; g_d7.smpSet[stage][type] = 1; }
    IDirect3DDevice9_SetSamplerState(g_d7.dev, stage, (D3DSAMPLERSTATETYPE)type, v);
}

static void D7_Reapply(void) {
    DWORD i, j;
    g_d7.fvf = 0;
    memset(g_d7.devTex, 0, sizeof(g_d7.devTex));
    for (i = 0; i < 256; i++) if (g_d7.rsSet[i]) IDirect3DDevice9_SetRenderState(g_d7.dev, (D3DRENDERSTATETYPE)i, g_d7.rs[i]);
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 32; j++) if (g_d7.tssSet[i][j]) IDirect3DDevice9_SetTextureStageState(g_d7.dev, i, (D3DTEXTURESTAGESTATETYPE)j, g_d7.tss[i][j]);
        for (j = 0; j < 16; j++) if (g_d7.smpSet[i][j]) IDirect3DDevice9_SetSamplerState(g_d7.dev, i, (D3DSAMPLERSTATETYPE)j, g_d7.smp[i][j]);
        if (g_d7.curTex[i]) {
            IDirect3DTexture9 *t = D7_GetTex(g_d7.curTex[i]);
            D7_SetTex(i, t);
        }
    }
}

/* Turn the game window into a borderless window covering its monitor. */
static void D7_SetupBorderless(HWND hwnd) {
    MONITORINFO mi;
    HMONITOR mon;
    DWORD style;

    if (!hwnd) return;
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi)) return;
    style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongA(hwnd, GWL_STYLE, (LONG)style);
    SetWindowLongA(hwnd, GWL_EXSTYLE,
                   (LONG)((DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE) & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME)));
    SetClassLongA(hwnd, GCL_HBRBACKGROUND, (LONG)(LONG_PTR)GetStockObject(BLACK_BRUSH));
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_d7.fsCw = g_d7.fsCh = 0;
    Log("D3D7 emu: borderless fullscreen window %ldx%ld at (%ld,%ld)",
        (long)(mi.rcMonitor.right - mi.rcMonitor.left), (long)(mi.rcMonitor.bottom - mi.rcMonitor.top),
        (long)mi.rcMonitor.left, (long)mi.rcMonitor.top);
}

/* Bring the game window to the foreground (Windows refuses a plain SetForegroundWindow
   from a process that is not in the foreground, so attach to the foreground thread's
   input queue first and fall back to an Alt key tap). */
static void D7_BringToFront(HWND hwnd) {
    HWND fg;
    DWORD fgThread = 0, myThread;
    int attached = 0;

    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    fg = GetForegroundWindow();
    if (fg == hwnd) return;
    myThread = GetCurrentThreadId();
    if (fg) fgThread = GetWindowThreadProcessId(fg, NULL);
    if (fgThread && fgThread != myThread)
        attached = AttachThreadInput(myThread, fgThread, TRUE) ? 1 : 0;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(myThread, fgThread, FALSE);
    if (GetForegroundWindow() != hwnd) {
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(hwnd);
    }
    Log("Foreground: window %p %s", (void*)hwnd, GetForegroundWindow() == hwnd ? "is now in the foreground" : "could not be brought to the foreground");
}

/* Switch the game window between borderless fullscreen and a normal window whose
   client area is the virtual display size. */
static void D7_SetWindowed(int windowed) {
    HWND hwnd = g_d7.hwnd;
    MONITORINFO mi;
    RECT r;
    DWORD style, ex;
    int w, h, x, y;

    if (!hwnd || !g_d7.dev) return;
    if (!windowed) {
        g_d7.fs = 1;
        D7_SetupBorderless(hwnd);
    } else {
        g_d7.fs = 0;
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        ex = ((DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE) & ~(DWORD)WS_EX_TOPMOST) | WS_EX_WINDOWEDGE;
        r.left = 0; r.top = 0; r.right = g_d7.dispW; r.bottom = g_d7.dispH;
        AdjustWindowRectEx(&r, style, FALSE, ex);
        w = r.right - r.left;
        h = r.bottom - r.top;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - w) / 2;
            y = mi.rcMonitor.top + ((mi.rcMonitor.bottom - mi.rcMonitor.top) - h) / 2;
        } else {
            x = 100; y = 100;
        }
        SetWindowLongA(hwnd, GWL_STYLE, (LONG)style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, (LONG)ex);
        SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        Log("D3D7 emu: normal window %dx%d at (%d,%d)", w, h, x, y);
    }
    g_d7.fsCw = g_d7.fsCh = 0;
    InvalidateRect(hwnd, NULL, TRUE);
}

/* Puts the device and the state shadows into the same, known state (all states the layer touches). */
static void D7_SeedState(void) {
    IDirect3DDevice9 *dev = g_d7.dev;
    static const DWORD rsSeed[][2] = {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, TRUE }, { D3DRS_LIGHTING, FALSE },
        { D3DRS_CULLMODE, D3DCULL_CCW }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE },
        { D3DRS_SPECULARENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_SCISSORTESTENABLE, FALSE },
        { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHAFUNC, D3DCMP_ALWAYS }, { D3DRS_ALPHAREF, 0 }
    };
    static const DWORD tssSeed[][3] = {
        { 0, D3DTSS_COLOROP, D3DTOP_MODULATE }, { 0, D3DTSS_COLORARG1, D3DTA_TEXTURE },
        { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE },
        { 1, D3DTSS_COLOROP, D3DTOP_DISABLE }, { 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE }
    };
    static const DWORD smpSeed[][2] = {
        { D3DSAMP_MINFILTER, D3DTEXF_POINT }, { D3DSAMP_MAGFILTER, D3DTEXF_POINT },
        { D3DSAMP_MIPFILTER, D3DTEXF_NONE }, { D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP },
        { D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP }
    };
    unsigned i;

    if (!dev) return;
    for (i = 0; i < sizeof(rsSeed) / sizeof(rsSeed[0]); i++) {
        IDirect3DDevice9_SetRenderState(dev, (D3DRENDERSTATETYPE)rsSeed[i][0], rsSeed[i][1]);
        g_d7.rs[rsSeed[i][0]] = rsSeed[i][1];
        g_d7.rsSet[rsSeed[i][0]] = 1;
    }
    for (i = 0; i < sizeof(tssSeed) / sizeof(tssSeed[0]); i++) {
        IDirect3DDevice9_SetTextureStageState(dev, tssSeed[i][0], (D3DTEXTURESTAGESTATETYPE)tssSeed[i][1], tssSeed[i][2]);
        g_d7.tss[tssSeed[i][0]][tssSeed[i][1]] = tssSeed[i][2];
        g_d7.tssSet[tssSeed[i][0]][tssSeed[i][1]] = 1;
    }
    for (i = 0; i < sizeof(smpSeed) / sizeof(smpSeed[0]); i++) {
        IDirect3DDevice9_SetSamplerState(dev, 0, (D3DSAMPLERSTATETYPE)smpSeed[i][0], smpSeed[i][1]);
        g_d7.smp[0][smpSeed[i][0]] = smpSeed[i][1];
        g_d7.smpSet[0][smpSeed[i][0]] = 1;
    }
    g_d7.fvf = 0;
    memset(g_d7.devTex, 0, sizeof(g_d7.devTex));
}

static int D7_InitD3D9(void) {
    HMODULE m;
    PF_D7Create9 create9;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    HWND hwnd = g_d7.hwnd ? g_d7.hwnd : GetActiveWindow();

    if (g_d7.dev) return 1;
    m = LoadLibraryA("d3d9.dll");
    create9 = m ? (PF_D7Create9)(void*)GetProcAddress(m, "Direct3DCreate9") : NULL;
    if (!create9) { Log("D3D7 emu: d3d9.dll / Direct3DCreate9 not available"); return 0; }
    g_d7.d3d = create9(D3D_SDK_VERSION);
    if (!g_d7.d3d) { Log("D3D7 emu: Direct3DCreate9 failed"); return 0; }
    if (!hwnd) { Log("D3D7 emu: no window"); return 0; }
    if (g_d7.fs) D7_SetupBorderless(hwnd);
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = (UINT)g_d7.dispW;
    pp.BackBufferHeight = (UINT)g_d7.dispH;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = D3DERR_NOTAVAILABLE;
    if (g_D7PureDevice) {
        hr = IDirect3D9_CreateDevice(g_d7.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_FPU_PRESERVE,
                                     &pp, &g_d7.dev);
        Log("D3D7 emu: CreateDevice(pure device) hr=%08X", (unsigned)hr);
        if (FAILED(hr)) g_d7.dev = NULL;
    }
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(g_d7.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_d7.dev);
    }
    if (FAILED(hr)) {
        Log("D3D7 emu: CreateDevice(HW vp) hr=%08X, retrying with software vertex processing", (unsigned)hr);
        hr = IDirect3D9_CreateDevice(g_d7.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_d7.dev);
    }
    if (FAILED(hr)) {
        Log("D3D7 emu: Direct3D9 CreateDevice failed hr=%08X", (unsigned)hr);
        g_d7.dev = NULL;
        return 0;
    }
    g_d7.pp = pp;
    g_d7.rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    g_d7.tss[0][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    g_d7.tss[0][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    g_d7.tss[0][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    IDirect3DDevice9_SetRenderState(g_d7.dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(g_d7.dev, D3DRS_LIGHTING, FALSE);
    g_d7.sb = NULL;
    D7_SeedState();
    Log("D3D7 emu: Direct3D9 device created (%dx%d, hwnd=%p)", g_d7.dispW, g_d7.dispH, (void*)hwnd);
    D7_BringToFront(hwnd);
    return 1;
}

/* Present; in borderless fullscreen the back buffer is stretched to the client area
   keeping its aspect ratio (black bars). */
static HRESULT D7_PresentFS(void) {
    RECT cr, dr;
    int cw, ch, w, h;

    if (!g_d7.fs || !g_d7.hwnd || !GetClientRect(g_d7.hwnd, &cr))
        return IDirect3DDevice9_Present(g_d7.dev, NULL, NULL, NULL, NULL);
    cw = cr.right - cr.left;
    ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0 || g_d7.dispW <= 0 || g_d7.dispH <= 0)
        return IDirect3DDevice9_Present(g_d7.dev, NULL, NULL, NULL, NULL);
    if (cw != g_d7.fsCw || ch != g_d7.fsCh) {
        HDC dc = GetDC(g_d7.hwnd);
        if (dc) {
            FillRect(dc, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
            ReleaseDC(g_d7.hwnd, dc);
        }
        g_d7.fsCw = cw;
        g_d7.fsCh = ch;
    }
    if ((long long)cw * g_d7.dispH <= (long long)ch * g_d7.dispW) {
        w = cw;
        h = (int)((long long)cw * g_d7.dispH / g_d7.dispW);
    } else {
        h = ch;
        w = (int)((long long)ch * g_d7.dispW / g_d7.dispH);
    }
    dr.left = (cw - w) / 2;
    dr.top = (ch - h) / 2;
    dr.right = dr.left + w;
    dr.bottom = dr.top + h;
    return IDirect3DDevice9_Present(g_d7.dev, NULL, &dr, NULL, NULL);
}

static void D7_Present(void) {
    HRESULT hr;
    if (!g_d7.dev) return;
    hr = D7_PresentFS();
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET) {
        hr = IDirect3DDevice9_TestCooperativeLevel(g_d7.dev);
        if (hr == D3DERR_DEVICENOTRESET) {
            if (g_d7.sb) {
                IDirect3DStateBlock9_Release(g_d7.sb);
                g_d7.sb = NULL;
            }
            if (SUCCEEDED(IDirect3DDevice9_Reset(g_d7.dev, &g_d7.pp))) {
                D7_Reapply();
                Log("D3D7 emu: device reset");
            }
        }
    }
}

/* ---- software surface -> Direct3D9 back buffer ------------------------
   Used for Blt/BltFast into the 3D back buffer and for Blt(primary <- 2D
   surface).  The source surface becomes a texture (colour key -> alpha 0,
   see D7_GetTex) and is drawn as a pre-transformed quad.  The complete
   device state is captured in a state block first and restored afterwards,
   so the game's own render/texture-stage state is not disturbed. */
typedef struct { float x, y, z, rhw; DWORD color; float u, v; } D7Quad;
#define D7_QUAD_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static int D7_BltToDev(const RECT *drIn, D7Surf *s, const RECT *srIn, int useKey) {
    RECT dr, sr;
    IDirect3DTexture9 *t;
    IDirect3DStateBlock9 *sb = g_d7.sb;
    IDirect3DDevice9 *dev = g_d7.dev;
    D7Quad q[4];
    float x0, y0, x1, y1, u0, v0, u1, v1;
    int scene = g_d7.inScene;
    int i;
    enum { NRS = 12 };
    static const DWORD rsId[NRS] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_CULLMODE, D3DRS_ALPHABLENDENABLE,
        D3DRS_FOGENABLE, D3DRS_SPECULARENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
        D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF
    };
    DWORD rsVal[NRS], rsOld[NRS], tssOld[6], smpOld[5];
    static const DWORD tssStage[6] = { 0, 0, 0, 0, 1, 1 };
    static const DWORD tssType[6] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
                                      D3DTSS_COLOROP, D3DTSS_ALPHAOP };
    static const DWORD tssVal[6] = { D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTA_TEXTURE,
                                     D3DTOP_DISABLE, D3DTOP_DISABLE };
    static const DWORD smpType[5] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                      D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV };
    static const DWORD smpVal[5] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE,
                                     D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };
    D7Surf *oldCur;
    DWORD oldFvf;

    if (!dev || !s) return 0;
    if (drIn) dr = *drIn; else { dr.left = 0; dr.top = 0; dr.right = g_d7.dispW; dr.bottom = g_d7.dispH; }
    if (srIn) sr = *srIn; else { sr.left = 0; sr.top = 0; sr.right = s->w; sr.bottom = s->h; }
    if (sr.right <= sr.left || sr.bottom <= sr.top || dr.right <= dr.left || dr.bottom <= dr.top) return 0;
    if (sr.left < 0 || sr.top < 0 || sr.right > s->w || sr.bottom > s->h) return 0;
    t = D7_GetTex(s);
    if (!t) return 0;

    /* -0.5: align texel centres with pixel centres for pre-transformed vertices */
    x0 = (float)dr.left - 0.5f;   y0 = (float)dr.top - 0.5f;
    x1 = (float)dr.right - 0.5f;  y1 = (float)dr.bottom - 0.5f;
    u0 = (float)sr.left / (float)s->w;   v0 = (float)sr.top / (float)s->h;
    u1 = (float)sr.right / (float)s->w;  v1 = (float)sr.bottom / (float)s->h;
    for (i = 0; i < 4; i++) { q[i].z = 0.0f; q[i].rhw = 1.0f; q[i].color = 0xFFFFFFFFu; }
    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x0; q[2].y = y1; q[2].u = u0; q[2].v = v1;
    q[3].x = x1; q[3].y = y1; q[3].u = u1; q[3].v = v1;

    /* The game's own render/texture-stage state is saved from the state shadows (all the states touched
       here are seeded by D7_SeedState) and restored afterwards; states that already have the wanted value
       cost nothing, so no device state block is needed. */
    D7_BatchFlush();
    rsVal[0] = FALSE; rsVal[1] = FALSE; rsVal[2] = FALSE; rsVal[3] = D3DCULL_NONE; rsVal[4] = FALSE;
    rsVal[5] = FALSE; rsVal[6] = FALSE; rsVal[7] = FALSE; rsVal[8] = FALSE;
    rsVal[9] = useKey ? TRUE : FALSE; rsVal[10] = D3DCMP_GREATER; rsVal[11] = 0;
    oldCur = g_d7.curTex[0];
    oldFvf = g_d7.fvf;
    if (!scene) IDirect3DDevice9_BeginScene(dev);

    for (i = 0; i < NRS; i++) {
        rsOld[i] = g_d7.rs[rsId[i]];
        D7_ApplyRS(rsId[i], rsVal[i]);
    }
    for (i = 0; i < 6; i++) {
        tssOld[i] = g_d7.tss[tssStage[i]][tssType[i]];
        D7_ApplyTss(tssStage[i], tssType[i], tssVal[i]);
    }
    for (i = 0; i < 5; i++) {
        smpOld[i] = g_d7.smp[0][smpType[i]];
        D7_ApplySamp(0, smpType[i], smpVal[i]);
    }
    D7_SetTex(0, t);
    D7_SetFVF(D7_QUAD_FVF);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(D7Quad));

    for (i = 0; i < NRS; i++) D7_ApplyRS(rsId[i], rsOld[i]);
    for (i = 0; i < 6; i++) D7_ApplyTss(tssStage[i], tssType[i], tssOld[i]);
    for (i = 0; i < 5; i++) D7_ApplySamp(0, smpType[i], smpOld[i]);
    D7_SetTex(0, oldCur ? D7_GetTex(oldCur) : NULL);
    if (oldFvf) D7_SetFVF(oldFvf);
    if (!scene) IDirect3DDevice9_EndScene(dev);
    return 1;
}

/* ---- stub generators ------------------------------------------------- */
#define D7FN(name, ret, ...) \
    static HRESULT STDMETHODCALLTYPE name(void *self, ##__VA_ARGS__) { (void)self; return (ret); }

/* ---- IDirectDrawSurface7 --------------------------------------------- */
static HRESULT STDMETHODCALLTYPE ES_QueryInterface(void *self, const GUID *riid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (D7_Iid(riid, &k_D7_IID_Unknown) || D7_Iid(riid, &k_D7_IID_Surf7)) {
        ES_AddRef(self);
        *out = self;
        return DD_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE ES_AddAttachedSurface(void *self, void *att) {
    D7Surf *s = (D7Surf*)self, *a = (D7Surf*)att;
    if (a && (a->caps & DDSCAPS_ZBUFFER) && !s->zbuf) {
        ES_AddRef(a);
        s->zbuf = a;
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_Blt(void *self, RECT *dr, void *srcp, RECT *sr, DWORD flags, DDBLTFX *fx) {
    D7Surf *d = (D7Surf*)self, *s = (D7Surf*)srcp;

    if (d->isPrimary) {
        if (s && (s == g_d7.back3d || (s->caps & DDSCAPS_3DDEVICE) || s == d->back)) {
            D7_Present();
        } else if (s && g_d7.dev) {
            /* 2D surface -> primary: draw it onto the Direct3D9 back buffer, then present */
            if (!D7_BltToDev(dr, s, sr, (flags & DDBLT_KEYSRC) ? 1 : 0) && D7_Warn(D7W_PRIMBLT))
                Log("D3D7 emu: Blt to the primary surface failed (invalid rectangle or texture)");
            D7_Present();
        } else if (D7_Warn(D7W_PRIMBLT)) {
            Log("D3D7 emu: Blt to the primary surface is not possible (no source or no Direct3D9 device yet)");
        }
        return DD_OK;
    }
    if (d == g_d7.back3d && g_d7.dev) {
        if ((flags & DDBLT_COLORFILL) && fx) {
            D3DRECT rc;
            DWORD argb = D7_ToArgb(&d->fmt, fx->dwFillColor);
            if (dr) { rc.x1 = dr->left; rc.y1 = dr->top; rc.x2 = dr->right; rc.y2 = dr->bottom; }
            IDirect3DDevice9_Clear(g_d7.dev, dr ? 1 : 0, dr ? &rc : NULL, D3DCLEAR_TARGET, argb | 0xFF000000u, 1.0f, 0);
        } else if (s && s != d && !(flags & DDBLT_DEPTHFILL)) {
            if (!D7_BltToDev(dr, s, sr, (flags & DDBLT_KEYSRC) ? 1 : 0) && D7_Warn(D7W_BACKBLT))
                Log("D3D7 emu: Blt into the 3D back buffer failed (flags=%08X)", (unsigned)flags);
        }
        return DD_OK;
    }
    if ((flags & DDBLT_COLORFILL) && fx) D7_Fill(d, dr, fx->dwFillColor);
    else if (s) D7_Blit(d, dr, s, sr, (flags & DDBLT_KEYSRC) ? 1 : 0);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_BltFast(void *self, DWORD x, DWORD y, void *srcp, RECT *sr, DWORD trans) {
    D7Surf *d = (D7Surf*)self, *s = (D7Surf*)srcp;
    RECT dr, src;
    if (!s) return DDERR_INVALIDPARAMS;
    if (sr) src = *sr; else { src.left = 0; src.top = 0; src.right = s->w; src.bottom = s->h; }
    dr.left = (LONG)x; dr.top = (LONG)y;
    dr.right = dr.left + (src.right - src.left);
    dr.bottom = dr.top + (src.bottom - src.top);
    if (g_d7.dev && s != d && (d == g_d7.back3d || d->isPrimary)) {
        if (!D7_BltToDev(&dr, s, &src, (trans & DDBLTFAST_SRCCOLORKEY) ? 1 : 0) && D7_Warn(D7W_BACKBLT))
            Log("D3D7 emu: BltFast into the 3D back buffer / primary failed");
        if (d->isPrimary) D7_Present();
        return DD_OK;
    }
    D7_Blit(d, &dr, s, &src, (trans & DDBLTFAST_SRCCOLORKEY) ? 1 : 0);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_Flip(void *self, void *target, DWORD flags) {
    if (((D7Surf*)self)->isPrimary) D7_Present();
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetAttachedSurface(void *self, DDSCAPS2 *caps, void **out) {
    D7Surf *s = (D7Surf*)self, *r = NULL;
    if (!caps || !out) return DDERR_INVALIDPARAMS;
    if (caps->dwCaps & DDSCAPS_BACKBUFFER) r = s->back;
    else if (caps->dwCaps & DDSCAPS_ZBUFFER) r = s->zbuf;
    if (!r) { *out = NULL; return DDERR_NOTFOUND; }
    ES_AddRef(r);
    *out = r;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetCaps(void *self, DDSCAPS2 *c) {
    if (!c) return DDERR_INVALIDPARAMS;
    memset(c, 0, sizeof(*c));
    c->dwCaps = ((D7Surf*)self)->caps;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetColorKey(void *self, DWORD flags, DDCOLORKEY *k) {
    D7Surf *s = (D7Surf*)self;
    if (!k) return DDERR_INVALIDPARAMS;
    if (!s->hasKey) return DDERR_NOCOLORKEY;
    k->dwColorSpaceLowValue = s->keyLo;
    k->dwColorSpaceHighValue = s->keyLo;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetDC(void *self, HDC *out) {
    D7Surf *s = (D7Surf*)self;
    if (!out) return DDERR_INVALIDPARAMS;
    if (!s->dib) return DDERR_CANTCREATEDC;
    if (!s->dc) {
        s->dc = CreateCompatibleDC(NULL);
        if (!s->dc) return DDERR_CANTCREATEDC;
        SelectObject(s->dc, s->dib);
    }
    *out = s->dc;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_ReleaseDC(void *self, HDC dc) {
    D7Surf *s = (D7Surf*)self;
    GdiFlush();
    D7_MarkDirty(s, NULL);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetPixelFormat(void *self, DDPIXELFORMAT *pf) {
    if (!pf) return DDERR_INVALIDPARAMS;
    *pf = ((D7Surf*)self)->pf;
    return DD_OK;
}

static void D7_FillDesc(const D7Surf *s, DDSURFACEDESC2 *d) {
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    d->dwWidth = (DWORD)s->w;
    d->dwHeight = (DWORD)s->h;
    d->ddpfPixelFormat = s->pf;
    d->ddsCaps.dwCaps = s->caps;
    if (s->bits) {
        d->dwFlags |= DDSD_PITCH;
        d->lPitch = s->pitch;
    }
}

static HRESULT STDMETHODCALLTYPE ES_GetSurfaceDesc(void *self, DDSURFACEDESC2 *d) {
    if (!d) return DDERR_INVALIDPARAMS;
    D7_FillDesc((D7Surf*)self, d);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_Lock(void *self, RECT *r, DDSURFACEDESC2 *d, DWORD flags, HANDLE ev) {
    D7Surf *s = (D7Surf*)self;
    if (!d) return DDERR_INVALIDPARAMS;
    if (!s->bits) return DDERR_SURFACELOST;
    if (s == g_d7.back3d && D7_Warn(D7W_BACKLOCK))
        Log("D3D7 emu: Lock on the 3D back buffer (contents are not synchronised with Direct3D9)");
    D7_FillDesc(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->bits;
    s->lockRO = (flags & DDLOCK_READONLY) ? 1 : 0;
    s->lockFull = 1;
    if (r && r->left >= 0 && r->top >= 0 && r->left < s->w && r->top < s->h &&
        r->right > r->left && r->bottom > r->top) {
        s->lockRc = *r;
        s->lockFull = 0;
    }
    if (r && r->left >= 0 && r->top >= 0 && r->left < s->w && r->top < s->h)
        d->lpSurface = s->bits + (size_t)r->top * (size_t)s->pitch + (size_t)r->left * (size_t)(s->bpp / 8);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_Unlock(void *self, RECT *r) {
    D7Surf *s = (D7Surf*)self;
    /* a read-only lock cannot have changed the pixels; otherwise only the locked rectangle is refreshed */
    if (!s->lockRO) D7_MarkDirty(s, s->lockFull ? NULL : &s->lockRc);
    s->lockRO = 0;
    s->lockFull = 1;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_SetColorKey(void *self, DWORD flags, DDCOLORKEY *k) {
    D7Surf *s = (D7Surf*)self;
    if (flags & DDCKEY_SRCBLT) {
        if (k) {
            if (!s->hasKey || s->keyLo != k->dwColorSpaceLowValue) {
                s->hasKey = 1;
                s->keyLo = k->dwColorSpaceLowValue;
                D7_MarkDirty(s, NULL);
            }
        } else if (s->hasKey) {
            s->hasKey = 0;
            D7_MarkDirty(s, NULL);
        }
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ES_GetDDInterface(void *self, void **out) {
    if (!out) return DDERR_INVALIDPARAMS;
    g_d7DD.ref++;
    *out = &g_d7DD;
    return DD_OK;
}

D7FN(ES_AddOverlayDirtyRect, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_BltBatch, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(ES_DeleteAttachedSurface, DD_OK, DWORD a, DWORD b)
D7FN(ES_EnumAttachedSurfaces, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ES_EnumOverlayZOrders, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(ES_GetBltStatus, DD_OK, DWORD a)
D7FN(ES_GetClipper, DDERR_NOCLIPPERATTACHED, DWORD a)
D7FN(ES_GetFlipStatus, DD_OK, DWORD a)
D7FN(ES_GetOverlayPosition, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ES_GetPalette, DDERR_NOPALETTEATTACHED, DWORD a)
D7FN(ES_Initialize, DDERR_ALREADYINITIALIZED, DWORD a, DWORD b)
D7FN(ES_IsLost, DD_OK)
D7FN(ES_Restore, DD_OK)
D7FN(ES_SetClipper, DD_OK, DWORD a)
D7FN(ES_SetOverlayPosition, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ES_SetPalette, DD_OK, DWORD a)
D7FN(ES_UpdateOverlay, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
D7FN(ES_UpdateOverlayDisplay, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_UpdateOverlayZOrder, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ES_PageLock, DD_OK, DWORD a)
D7FN(ES_PageUnlock, DD_OK, DWORD a)
D7FN(ES_SetSurfaceDesc, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ES_SetPrivateData, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d)
D7FN(ES_GetPrivateData, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(ES_FreePrivateData, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_GetUniquenessValue, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_ChangeUniquenessValue, DDERR_UNSUPPORTED)
D7FN(ES_SetPriority, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_GetPriority, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_SetLOD, DDERR_UNSUPPORTED, DWORD a)
D7FN(ES_GetLOD, DDERR_UNSUPPORTED, DWORD a)

static void *g_d7SurfVt[49] = {
    (void*)ES_QueryInterface, (void*)ES_AddRef, (void*)ES_Release, (void*)ES_AddAttachedSurface,
    (void*)ES_AddOverlayDirtyRect, (void*)ES_Blt, (void*)ES_BltBatch, (void*)ES_BltFast,
    (void*)ES_DeleteAttachedSurface, (void*)ES_EnumAttachedSurfaces, (void*)ES_EnumOverlayZOrders,
    (void*)ES_Flip, (void*)ES_GetAttachedSurface, (void*)ES_GetBltStatus, (void*)ES_GetCaps,
    (void*)ES_GetClipper, (void*)ES_GetColorKey, (void*)ES_GetDC, (void*)ES_GetFlipStatus,
    (void*)ES_GetOverlayPosition, (void*)ES_GetPalette, (void*)ES_GetPixelFormat,
    (void*)ES_GetSurfaceDesc, (void*)ES_Initialize, (void*)ES_IsLost, (void*)ES_Lock,
    (void*)ES_ReleaseDC, (void*)ES_Restore, (void*)ES_SetClipper, (void*)ES_SetColorKey,
    (void*)ES_SetOverlayPosition, (void*)ES_SetPalette, (void*)ES_Unlock, (void*)ES_UpdateOverlay,
    (void*)ES_UpdateOverlayDisplay, (void*)ES_UpdateOverlayZOrder, (void*)ES_GetDDInterface,
    (void*)ES_PageLock, (void*)ES_PageUnlock, (void*)ES_SetSurfaceDesc, (void*)ES_SetPrivateData,
    (void*)ES_GetPrivateData, (void*)ES_FreePrivateData, (void*)ES_GetUniquenessValue,
    (void*)ES_ChangeUniquenessValue, (void*)ES_SetPriority, (void*)ES_GetPriority,
    (void*)ES_SetLOD, (void*)ES_GetLOD
};

/* ---- IDirect3DDevice7 ------------------------------------------------ */
static HRESULT STDMETHODCALLTYPE EV_QueryInterface(void *self, const GUID *riid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (D7_Iid(riid, &k_D7_IID_Unknown) || D7_Iid(riid, &k_D7_IID_Dev7)) {
        g_d7Dev.ref++;
        *out = &g_d7Dev;
        return DD_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EV_AddRef(void *self) { return (ULONG)++g_d7Dev.ref; }
static ULONG STDMETHODCALLTYPE EV_Release(void *self) { return (ULONG)(g_d7Dev.ref > 1 ? --g_d7Dev.ref : 1); }

typedef HRESULT (WINAPI *PF_D7EnumPF)(DDPIXELFORMAT*, void*);

static HRESULT STDMETHODCALLTYPE EV_EnumTextureFormats(void *self, PF_D7EnumPF cb, void *ctx) {
    unsigned i;
    if (!cb) return DDERR_INVALIDPARAMS;
    for (i = 0; i < sizeof(k_d7Fmts) / sizeof(k_d7Fmts[0]); i++) {
        DDPIXELFORMAT pf;
        D7_SetPf(&pf, k_d7Fmts[i].flags, k_d7Fmts[i].bpp, k_d7Fmts[i].r, k_d7Fmts[i].g, k_d7Fmts[i].b, k_d7Fmts[i].a);
        if (cb(&pf, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_BeginScene(void *self) {
    if (g_d7.dev && SUCCEEDED(IDirect3DDevice9_BeginScene(g_d7.dev))) g_d7.inScene = 1;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_EndScene(void *self) {
    if (g_d7.dev) { IDirect3DDevice9_EndScene(g_d7.dev); g_d7.inScene = 0; }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_GetDirect3D(void *self, void **out) {
    if (!out) return DDERR_INVALIDPARAMS;
    g_d7D3D.ref++;
    *out = &g_d7D3D;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_Clear(void *self, DWORD count, void *rects, DWORD flags, DWORD color, float z, DWORD stencil) {
    if (g_d7.dev && (flags & D3DCLEAR_TARGET))
        IDirect3DDevice9_Clear(g_d7.dev, count, (const D3DRECT*)rects, D3DCLEAR_TARGET, color, z, stencil);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_SetViewport(void *self, void *vp) {
    if (g_d7.dev && vp) IDirect3DDevice9_SetViewport(g_d7.dev, (const D3DVIEWPORT9*)vp);
    return DD_OK;
}

static DWORD D7_MapFilter(DWORD v) {
    return (v == 2 || v == 4 || v == 5 || v == 6) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
}

static int D7_RsPass(DWORD st) {
    return st == 7 || st == 8 || st == 9 || (st >= 14 && st <= 16) || (st >= 19 && st <= 20) ||
           (st >= 22 && st <= 29) || (st >= 34 && st <= 38) || (st >= 52 && st <= 60) ||
           (st >= 128 && st <= 143);
}

static HRESULT STDMETHODCALLTYPE EV_SetRenderState(void *self, DWORD st, DWORD v) {
    static unsigned char logged[256];
    if (!g_d7.dev) return DD_OK;
    switch (st) {
    case 17: D7_ApplySamp(0, D3DSAMP_MAGFILTER, D7_MapFilter(v)); break;
    case 18: D7_ApplySamp(0, D3DSAMP_MINFILTER, D7_MapFilter(v)); break;
    case 21: break;
    case 41:
        if (g_d7.ckEnable != (v != 0)) BatchFlushQuads();
        g_d7.ckEnable = (v != 0);
        break;
    default:
        if (D7_RsPass(st)) D7_ApplyRS(st, v);
        else if (st < 256 && !logged[st]) { logged[st] = 1; Log("D3D7 emu: render state %lu ignored (value %lu)", (unsigned long)st, (unsigned long)v); }
        break;
    }
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_GetRenderState(void *self, DWORD st, DWORD *v) {
    if (!v) return DDERR_INVALIDPARAMS;
    *v = (st == 41) ? (DWORD)g_d7.ckEnable : (st < 256 ? g_d7.rs[st] : 0);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_SetTexture(void *self, DWORD stage, void *tex) {
    D7Surf *s = (D7Surf*)tex;
    IDirect3DTexture9 *t = NULL;
    if (!g_d7.dev || stage >= 8) return DD_OK;
    g_d7.curTex[stage] = s;
    if (s) t = D7_GetTex(s);
    D7_SetTex(stage, t);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE EV_SetTextureStageState(void *self, DWORD stage, DWORD type, DWORD v) {
    if (!g_d7.dev || stage >= 8) return DD_OK;
    switch (type) {
    case 12: D7_ApplySamp(stage, D3DSAMP_ADDRESSU, v); D7_ApplySamp(stage, D3DSAMP_ADDRESSV, v); break;
    case 13: D7_ApplySamp(stage, D3DSAMP_ADDRESSU, v); break;
    case 14: D7_ApplySamp(stage, D3DSAMP_ADDRESSV, v); break;
    case 15: D7_ApplySamp(stage, D3DSAMP_BORDERCOLOR, v); break;
    case 16: D7_ApplySamp(stage, D3DSAMP_MAGFILTER, v == 5 ? D3DTEXF_ANISOTROPIC : D7_MapFilter(v)); break;
    case 17: D7_ApplySamp(stage, D3DSAMP_MINFILTER, v == 3 ? D3DTEXF_ANISOTROPIC : D7_MapFilter(v)); break;
    case 18: D7_ApplySamp(stage, D3DSAMP_MIPFILTER, v > 0 ? v - 1 : 0); break;
    case 19: D7_ApplySamp(stage, D3DSAMP_MIPMAPLODBIAS, v); break;
    case 20: D7_ApplySamp(stage, D3DSAMP_MAXMIPLEVEL, v); break;
    case 21: D7_ApplySamp(stage, D3DSAMP_MAXANISOTROPY, v); break;
    default:
        if (type >= 1 && type < 32) D7_ApplyTss(stage, type, v);
        break;
    }
    return DD_OK;
}

static UINT D7_FvfStride(DWORD fvf) {
    UINT st;
    switch (fvf & 0xE) {
    case 0x4: st = 16; break;
    case 0x2: st = 12; break;
    default: return 0;
    }
    if (fvf & 0x10) st += 12;
    if (fvf & 0x20) st += 4;
    if (fvf & 0x40) st += 4;
    if (fvf & 0x80) st += 4;
    st += ((fvf >> 8) & 0xF) * 8;
    return st;
}

/* DDCKEY_SRCBLT emulation: texels equal to the key have alpha 0 (see D7_GetTex);
   while COLORKEYENABLE is on, reject them with an alpha test on the texture alpha. */
static void D7_CkBegin(void) {
    D7_ApplyRS(D3DRS_ALPHATESTENABLE, TRUE);
    D7_ApplyRS(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    D7_ApplyRS(D3DRS_ALPHAREF, 0);
    D7_ApplyTss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    D7_ApplyTss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    D7_ApplyTss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

static void D7_CkEnd(void) {
    D7_ApplyRS(D3DRS_ALPHATESTENABLE, g_d7.rsSet[D3DRS_ALPHATESTENABLE] ? 0 : 0);
}

static HRESULT STDMETHODCALLTYPE EV_DrawPrimitive(void *self, DWORD type, DWORD fvf, void *verts, DWORD count, DWORD flags) {
    UINT stride, prims;
    D7Surf *t0;
    int ck;

    if (!g_d7.dev || !verts || !count) return DD_OK;
    stride = D7_FvfStride(fvf);
    switch (type) {
    case 1: prims = count; break;
    case 2: prims = count / 2; break;
    case 3: prims = count > 1 ? count - 1 : 0; break;
    case 4: prims = count / 3; break;
    case 5:
    case 6: prims = count > 2 ? count - 2 : 0; break;
    default: prims = 0; break;
    }
    if (!stride || !prims) return DD_OK;
    t0 = g_d7.curTex[0];
    if (t0 && t0->dirty) D7_GetTex(t0);
    ck = g_d7.ckEnable && t0 && t0->hasKey;
    if (ck) {
        DWORD at = g_d7.rsSet[D3DRS_ALPHATESTENABLE] ? g_d7.rs[D3DRS_ALPHATESTENABLE] : 0;
        DWORD af = g_d7.rsSet[D3DRS_ALPHAFUNC] ? g_d7.rs[D3DRS_ALPHAFUNC] : D3DCMP_ALWAYS;
        DWORD ar = g_d7.rsSet[D3DRS_ALPHAREF] ? g_d7.rs[D3DRS_ALPHAREF] : 0;
        DWORD op = g_d7.tssSet[0][D3DTSS_ALPHAOP] ? g_d7.tss[0][D3DTSS_ALPHAOP] : D3DTOP_SELECTARG1;
        DWORD a1 = g_d7.tssSet[0][D3DTSS_ALPHAARG1] ? g_d7.tss[0][D3DTSS_ALPHAARG1] : D3DTA_TEXTURE;
        DWORD a2 = g_d7.tssSet[0][D3DTSS_ALPHAARG2] ? g_d7.tss[0][D3DTSS_ALPHAARG2] : D3DTA_CURRENT;
        D7_CkBegin();
        D7_SetFVF(fvf);
        IDirect3DDevice9_DrawPrimitiveUP(g_d7.dev, (D3DPRIMITIVETYPE)type, prims, verts, stride);
        D7_ApplyRS(D3DRS_ALPHATESTENABLE, at);
        D7_ApplyRS(D3DRS_ALPHAFUNC, af);
        D7_ApplyRS(D3DRS_ALPHAREF, ar);
        D7_ApplyTss(0, D3DTSS_ALPHAOP, op);
        D7_ApplyTss(0, D3DTSS_ALPHAARG1, a1);
        D7_ApplyTss(0, D3DTSS_ALPHAARG2, a2);
        return DD_OK;
    }
    D7_SetFVF(fvf);
    IDirect3DDevice9_DrawPrimitiveUP(g_d7.dev, (D3DPRIMITIVETYPE)type, prims, verts, stride);
    return DD_OK;
}

D7FN(EV_GetCaps, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_SetRenderTarget, DD_OK, DWORD a, DWORD b)
D7FN(EV_GetRenderTarget, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_SetTransform, DD_OK, DWORD a, DWORD b)
D7FN(EV_GetTransform, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_MultiplyTransform, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_GetViewport, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_SetMaterial, DD_OK, DWORD a)
D7FN(EV_GetMaterial, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_SetLight, DD_OK, DWORD a, DWORD b)
D7FN(EV_GetLight, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_BeginStateBlock, DDERR_UNSUPPORTED)
D7FN(EV_EndStateBlock, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_PreLoad, DD_OK, DWORD a)
D7FN(EV_DrawIndexedPrimitive, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g)
D7FN(EV_SetClipStatus, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_GetClipStatus, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_DrawPrimitiveStrided, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
D7FN(EV_DrawIndexedPrimitiveStrided, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g)
D7FN(EV_DrawPrimitiveVB, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
D7FN(EV_DrawIndexedPrimitiveVB, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g)
D7FN(EV_ComputeSphereVisibility, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d)
D7FN(EV_GetTexture, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_GetTextureStageState, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(EV_ValidateDevice, DD_OK, DWORD a)
D7FN(EV_ApplyStateBlock, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_CaptureStateBlock, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_DeleteStateBlock, DDERR_UNSUPPORTED, DWORD a)
D7FN(EV_CreateStateBlock, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_Load, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
D7FN(EV_LightEnable, DD_OK, DWORD a, DWORD b)
D7FN(EV_GetLightEnable, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_SetClipPlane, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_GetClipPlane, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(EV_GetInfo, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)

static void *g_d7DevVt[49] = {
    (void*)EV_QueryInterface, (void*)EV_AddRef, (void*)EV_Release, (void*)EV_GetCaps,
    (void*)EV_EnumTextureFormats, (void*)EV_BeginScene, (void*)EV_EndScene, (void*)EV_GetDirect3D,
    (void*)EV_SetRenderTarget, (void*)EV_GetRenderTarget, (void*)EV_Clear, (void*)EV_SetTransform,
    (void*)EV_GetTransform, (void*)EV_SetViewport, (void*)EV_MultiplyTransform, (void*)EV_GetViewport,
    (void*)EV_SetMaterial, (void*)EV_GetMaterial, (void*)EV_SetLight, (void*)EV_GetLight,
    (void*)EV_SetRenderState, (void*)EV_GetRenderState, (void*)EV_BeginStateBlock,
    (void*)EV_EndStateBlock, (void*)EV_PreLoad, (void*)EV_DrawPrimitive,
    (void*)EV_DrawIndexedPrimitive, (void*)EV_SetClipStatus, (void*)EV_GetClipStatus,
    (void*)EV_DrawPrimitiveStrided, (void*)EV_DrawIndexedPrimitiveStrided, (void*)EV_DrawPrimitiveVB,
    (void*)EV_DrawIndexedPrimitiveVB, (void*)EV_ComputeSphereVisibility, (void*)EV_GetTexture,
    (void*)EV_SetTexture, (void*)EV_GetTextureStageState, (void*)EV_SetTextureStageState,
    (void*)EV_ValidateDevice, (void*)EV_ApplyStateBlock, (void*)EV_CaptureStateBlock,
    (void*)EV_DeleteStateBlock, (void*)EV_CreateStateBlock, (void*)EV_Load, (void*)EV_LightEnable,
    (void*)EV_GetLightEnable, (void*)EV_SetClipPlane, (void*)EV_GetClipPlane, (void*)EV_GetInfo
};

/* ---- IDirect3D7 ------------------------------------------------------ */
static HRESULT STDMETHODCALLTYPE E3_QueryInterface(void *self, const GUID *riid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (D7_Iid(riid, &k_D7_IID_Unknown) || D7_Iid(riid, &k_D7_IID_D3D7)) {
        g_d7D3D.ref++;
        *out = &g_d7D3D;
        return DD_OK;
    }
    if (D7_Iid(riid, &k_D7_IID_DD7)) {
        g_d7DD.ref++;
        *out = &g_d7DD;
        return DD_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE E3_AddRef(void *self) { return (ULONG)++g_d7D3D.ref; }
static ULONG STDMETHODCALLTYPE E3_Release(void *self) { return (ULONG)(g_d7D3D.ref > 1 ? --g_d7D3D.ref : 1); }

static HRESULT STDMETHODCALLTYPE E3_CreateDevice(void *self, const GUID *cls, void *surf, void **dev) {
    if (!dev || !surf) return DDERR_INVALIDPARAMS;
    g_d7.back3d = (D7Surf*)surf;
    if (!D7_InitD3D9()) return DDERR_GENERIC;
    g_d7Dev.ref++;
    *dev = &g_d7Dev;
    Log("D3D7 emu: IDirect3D7::CreateDevice ok (back buffer surface %p)", surf);
    return DD_OK;
}

D7FN(E3_EnumDevices, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(E3_CreateVertexBuffer, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(E3_EnumZBufferFormats, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(E3_EvictManagedTextures, DD_OK)

static void *g_d7D3DVt[8] = {
    (void*)E3_QueryInterface, (void*)E3_AddRef, (void*)E3_Release, (void*)E3_EnumDevices,
    (void*)E3_CreateDevice, (void*)E3_CreateVertexBuffer, (void*)E3_EnumZBufferFormats,
    (void*)E3_EvictManagedTextures
};

/* ---- IDirectDraw7 ---------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE ED_QueryInterface(void *self, const GUID *riid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (D7_Iid(riid, &k_D7_IID_Unknown) || D7_Iid(riid, &k_D7_IID_DD7) || D7_Iid(riid, &k_D7_IID_DD1) ||
        D7_Iid(riid, &k_D7_IID_DD2) || D7_Iid(riid, &k_D7_IID_DD3) || D7_Iid(riid, &k_D7_IID_DD4)) {
        g_d7DD.ref++;
        *out = &g_d7DD;
        return DD_OK;
    }
    if (D7_Iid(riid, &k_D7_IID_D3D7)) {
        g_d7D3D.ref++;
        *out = &g_d7D3D;
        return DD_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ED_AddRef(void *self) { return (ULONG)++g_d7DD.ref; }
static ULONG STDMETHODCALLTYPE ED_Release(void *self) { return (ULONG)(g_d7DD.ref > 1 ? --g_d7DD.ref : 1); }

static HRESULT STDMETHODCALLTYPE ED_CreateSurface(void *self, DDSURFACEDESC2 *d, void **out, void *unk) {
    DWORD caps;
    int w, h;
    DDPIXELFORMAT pf;
    D7Surf *s;

    if (!d || !out) return DDERR_INVALIDPARAMS;
    *out = NULL;
    caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    w = (d->dwFlags & DDSD_WIDTH) ? (int)d->dwWidth : 0;
    h = (d->dwFlags & DDSD_HEIGHT) ? (int)d->dwHeight : 0;
    D7_SetPf(&pf, DDPF_RGB, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = g_d7.dispW;
        h = g_d7.dispH;
        s = D7_SurfNew(w, h, &pf, caps);
        if (!s) return DDERR_OUTOFMEMORY;
        s->isPrimary = 1;
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount > 0) {
            s->back = D7_SurfNew(w, h, &pf, DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY);
            if (!s->back) { ES_Release(s); return DDERR_OUTOFMEMORY; }
        }
        g_d7.primary = s;
    } else if ((d->dwFlags & DDSD_PIXELFORMAT) && (d->ddpfPixelFormat.dwFlags & DDPF_ZBUFFER)) {
        if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;
        s = D7_SurfNew(w, h, &d->ddpfPixelFormat, caps | DDSCAPS_ZBUFFER);
        if (!s) return DDERR_OUTOFMEMORY;
    } else {
        if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;
        if ((d->dwFlags & DDSD_PIXELFORMAT) && (d->ddpfPixelFormat.dwFlags & DDPF_RGB))
            pf = d->ddpfPixelFormat;
        if (pf.dwRGBBitCount != 16 && pf.dwRGBBitCount != 32) {
            Log("D3D7 emu: unsupported pixel format (%lu bpp) for a %dx%d surface",
                (unsigned long)pf.dwRGBBitCount, w, h);
            return DDERR_INVALIDPIXELFORMAT;
        }
        s = D7_SurfNew(w, h, &pf, caps);
        if (!s) return DDERR_OUTOFMEMORY;
    }
    if (g_d7.surfLogged < 40) {
        g_d7.surfLogged++;
        Log("D3D7 emu: CreateSurface %dx%d caps=%08X flags=%08X bpp=%d masks=%08X/%08X/%08X/%08X -> %p",
            w, h, (unsigned)caps, (unsigned)d->dwFlags, s->bpp, (unsigned)s->fmt.m[0],
            (unsigned)s->fmt.m[1], (unsigned)s->fmt.m[2], (unsigned)s->fmt.m[3], (void*)s);
    }
    *out = s;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetDisplayMode(void *self, DDSURFACEDESC2 *d) {
    if (!d) return DDERR_INVALIDPARAMS;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH;
    d->dwWidth = (DWORD)g_d7.dispW;
    d->dwHeight = (DWORD)g_d7.dispH;
    d->lPitch = g_d7.dispW * 4;
    D7_SetPf(&d->ddpfPixelFormat, DDPF_RGB, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    return DD_OK;
}

/* ==========================================================================
   Alt+Enter: toggle WndMode in config.txt and, with the Direct3D9 layer, switch
   between borderless fullscreen and a normal window on the fly.  While Alt+Enter
   is held (until Enter is released) the Enter key is hidden from the game: the
   key-state APIs the game imports from user32 are hooked through its IAT.
   ========================================================================== */
static volatile LONG g_enterMask = 0;
typedef SHORT (WINAPI *PF_KeyStateFn)(int);
typedef BOOL (WINAPI *PF_KbStateFn)(PBYTE);
static PF_KeyStateFn g_origGetAsyncKeyState = NULL;
static PF_KeyStateFn g_origGetKeyState = NULL;
static PF_KbStateFn g_origGetKeyboardState = NULL;

static char *WndModeReadFile(const char *path, size_t *outN) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1048576) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *outN = n;
    return buf;
}

/* Find the value of the "WndMode=" line (a line that starts with it; comment lines never do). */
static int WndModeFind(const char *buf, size_t n, size_t *vs, size_t *ve) {
    size_t i = 0;
    while (i < n) {
        size_t ls = i, le = i;
        while (le < n && buf[le] != '\n' && buf[le] != '\r') le++;
        if (le - ls >= 8 && _strnicmp(buf + ls, "WndMode=", 8) == 0) {
            size_t a = ls + 8, b = le;
            while (a < b && (buf[a] == ' ' || buf[a] == '\t')) a++;
            while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t')) b--;
            *vs = a;
            *ve = b;
            return 1;
        }
        i = le;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    return 0;
}

/* 1 = WndMode=on (window), 0 = off (fullscreen), -1 = not found */
static int ReadWndModeConfig(void) {
    char path[MAX_PATH];
    size_t n = 0, vs = 0, ve = 0;
    char *buf;
    int r = -1;
    ExeDirFile("config.txt", path);
    buf = WndModeReadFile(path, &n);
    if (!buf) return -1;
    if (WndModeFind(buf, n, &vs, &ve))
        r = (ve - vs == 2 && _strnicmp(buf + vs, "on", 2) == 0) ? 1 : 0;
    free(buf);
    return r;
}

static int WriteWndModeConfig(int windowed) {
    char path[MAX_PATH], tmp[MAX_PATH];
    size_t n = 0, vs = 0, ve = 0;
    const char *val = windowed ? "on" : "off";
    char *buf;
    FILE *f;
    int ok = 0;

    ExeDirFile("config.txt", path);
    buf = WndModeReadFile(path, &n);
    if (!buf) { Log("WndMode: cannot read %s", path); return 0; }
    if (!WndModeFind(buf, n, &vs, &ve)) {
        Log("WndMode: no WndMode= line in config.txt");
        free(buf);
        return 0;
    }
    strcpy(tmp, path);
    strcat(tmp, ".tmp");
    f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(buf, 1, vs, f) == vs &&
             fwrite(val, 1, strlen(val), f) == strlen(val) &&
             fwrite(buf + ve, 1, n - ve, f) == n - ve;
        if (fclose(f) != 0) ok = 0;
        if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
        if (!ok) DeleteFileA(tmp);
    }
    free(buf);
    Log("WndMode: config.txt WndMode=%s %s", val, ok ? "written" : "FAILED to write");
    return ok;
}

static void ToggleWndMode(void) {
    int live = (g_d7.active && g_d7.dev) ? 1 : 0;
    int cur, wantWindowed;

    if (live) cur = g_d7.fs ? 0 : 1;
    else {
        cur = ReadWndModeConfig();
        if (cur < 0) cur = 0;
    }
    wantWindowed = !cur;
    WriteWndModeConfig(wantWindowed);
    if (live) D7_SetWindowed(wantWindowed);
    Log("WndMode: Alt+Enter -> WndMode=%s%s", wantWindowed ? "on" : "off",
        live ? " (applied now)" : " (takes effect on the next launch)");
}

static int EnterMasked(void) {
    if (!(GetAsyncKeyState(VK_RETURN) & 0x8000)) {
        g_enterMask = 0;
        return 0;
    }
    if (g_enterMask) return 1;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
        g_enterMask = 1;
        return 1;
    }
    return 0;
}

static SHORT WINAPI Hook_GetAsyncKeyState(int vk) {
    SHORT r = g_origGetAsyncKeyState(vk);
    if (vk == VK_RETURN && EnterMasked()) r = 0;
    return r;
}

static SHORT WINAPI Hook_GetKeyState(int vk) {
    SHORT r = g_origGetKeyState(vk);
    if (vk == VK_RETURN && EnterMasked()) r = 0;
    return r;
}

static BOOL WINAPI Hook_GetKeyboardState(PBYTE keys) {
    BOOL r = g_origGetKeyboardState(keys);
    if (r && keys && EnterMasked()) keys[VK_RETURN] = 0;
    return r;
}

/* Replace the import thunk of `func` (any DLL) in the main executable. */
static int HookIat(const char *func, void *hook, void **orig) {
    BYTE *base = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dd;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int n = 0;

    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0) continue;
            if (!VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), PAGE_READWRITE, &old)) continue;
            if (orig && !*orig) *orig = (void*)(uintptr_t)ft->u1.Function;
            ft->u1.Function = (DWORD)(uintptr_t)hook;
            VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), old, &old);
            n++;
        }
    }
    return n;
}

static void PatchEnterMask(void) {
    int a = HookIat("GetAsyncKeyState", (void*)&Hook_GetAsyncKeyState, (void**)&g_origGetAsyncKeyState);
    int b = HookIat("GetKeyState", (void*)&Hook_GetKeyState, (void**)&g_origGetKeyState);
    int c = HookIat("GetKeyboardState", (void*)&Hook_GetKeyboardState, (void**)&g_origGetKeyboardState);
    Log("EnterMask: IAT hooks GetAsyncKeyState=%d GetKeyState=%d GetKeyboardState=%d%s", a, b, c,
        (a + b + c) ? "" : " (the game does not import them; Enter cannot be masked)");
}

static WNDPROC g_origWndProc = NULL;
static HWND g_hotkeyHwnd = NULL;

static LRESULT CALLBACK SmdWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SYSKEYDOWN && w == VK_RETURN) {
        if (!(l & 0x40000000)) {   /* ignore auto-repeat */
            g_enterMask = 1;
            ToggleWndMode();
        }
        return 0;
    }
    if ((m == WM_SYSKEYUP && w == VK_RETURN) || (m == WM_SYSCHAR && w == '\r'))
        return 0;
    return CallWindowProcA(g_origWndProc, h, m, w, l);
}

/* Subclass the game window to catch Alt+Enter (also stops the system beep). */
static void HotkeyAttach(HWND hwnd) {
    LONG_PTR prev;
    if (!hwnd || hwnd == g_hotkeyHwnd || !IsWindow(hwnd)) return;
    prev = (LONG_PTR)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)(LONG_PTR)SmdWndProc);
    if (!prev) {
        Log("WndMode: could not subclass the game window (err=%08X)", (unsigned)GetLastError());
        return;
    }
    g_origWndProc = (WNDPROC)prev;
    g_hotkeyHwnd = hwnd;
    Log("WndMode: Alt+Enter hotkey attached to hwnd=%p", (void*)hwnd);
}

static HRESULT STDMETHODCALLTYPE ED_SetCooperativeLevel(void *self, HWND hwnd, DWORD flags) {
    if (hwnd) g_d7.hwnd = hwnd;
    HotkeyAttach(hwnd);
    g_d7.fs = (flags & DDSCL_FULLSCREEN) ? 1 : 0;
    if (g_d7.fs) D7_SetupBorderless(hwnd);
    Log("D3D7 emu: SetCooperativeLevel hwnd=%p flags=%X (%s)", (void*)hwnd, (unsigned)flags,
        g_d7.fs ? "borderless fullscreen" : "window mode");
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_SetDisplayMode(void *self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
    g_d7.dispW = (int)w;
    g_d7.dispH = (int)h;
    Log("D3D7 emu: SetDisplayMode %lux%lux%lu (virtual, desktop mode unchanged)", (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetDeviceIdentifier(void *self, DDDEVICEIDENTIFIER2 *p, DWORD flags) {
    if (!p) return DDERR_INVALIDPARAMS;
    memset(p, 0, sizeof(*p));
    strcpy(p->szDriver, "d3d9.dll");
    strcpy(p->szDescription, "Direct3D9 (Smd D3D7 emulation)");
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetAvailableVidMem(void *self, DDSCAPS2 *c, DWORD *total, DWORD *freev) {
    if (total) *total = 256u * 1024u * 1024u;
    if (freev) *freev = 256u * 1024u * 1024u;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetMonitorFrequency(void *self, DWORD *f) {
    if (f) *f = 60;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetFourCCCodes(void *self, DWORD *n, DWORD *codes) {
    if (n) *n = 0;
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_GetVerticalBlankStatus(void *self, BOOL *b) {
    if (b) *b = FALSE;
    return DD_OK;
}

D7FN(ED_Compact, DD_OK)
D7FN(ED_CreateClipper, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(ED_CreatePalette, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d)
D7FN(ED_DuplicateSurface, DDERR_UNSUPPORTED, DWORD a, DWORD b)
typedef HRESULT (WINAPI *PF_D7EnumModes)(DDSURFACEDESC2*, void*);

/* Report a fixed set of 16/32 bpp modes (the desktop mode is never changed, see ED_SetDisplayMode). */
static HRESULT STDMETHODCALLTYPE ED_EnumDisplayModes(void *self, DWORD flags, DDSURFACEDESC2 *filter, void *ctx, PF_D7EnumModes cb) {
    static const struct { int w, h, bpp; } modes[] = {
        { 640, 480, 32 }, { 640, 480, 16 }, { 800, 600, 32 }, { 800, 600, 16 },
        { 1024, 768, 32 }, { 1280, 720, 32 }, { 1280, 960, 32 }, { 1920, 1080, 32 }
    };
    unsigned i;
    if (!cb) return DDERR_INVALIDPARAMS;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        DDSURFACEDESC2 d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        d.dwWidth = (DWORD)modes[i].w;
        d.dwHeight = (DWORD)modes[i].h;
        d.lPitch = modes[i].w * (modes[i].bpp / 8);
        d.dwRefreshRate = 60;
        if (modes[i].bpp == 32) D7_SetPf(&d.ddpfPixelFormat, DDPF_RGB, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
        else                    D7_SetPf(&d.ddpfPixelFormat, DDPF_RGB, 16, 0x0000F800, 0x000007E0, 0x0000001F, 0);
        if (filter) {
            if ((filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != d.dwWidth) continue;
            if ((filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != d.dwHeight) continue;
            if ((filter->dwFlags & DDSD_PIXELFORMAT) && filter->ddpfPixelFormat.dwRGBBitCount != d.ddpfPixelFormat.dwRGBBitCount) continue;
        }
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
D7FN(ED_EnumSurfaces, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c, DWORD d)
D7FN(ED_FlipToGDISurface, DD_OK)
static int D7_FillCaps(DDCAPS *c) {
    DWORD sz;
    if (!c) return 1;
    sz = c->dwSize;
    if (sz < 2 * sizeof(DWORD) || sz > sizeof(DDCAPS)) return 0;
    memset(c, 0, sz);
    c->dwSize = sz;
    c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_GDI;
    if (sz >= sizeof(DDCAPS)) {
        c->dwVidMemTotal = 256u * 1024u * 1024u;
        c->dwVidMemFree = 256u * 1024u * 1024u;
    }
    return 1;
}

static HRESULT STDMETHODCALLTYPE ED_GetCaps(void *self, DDCAPS *hal, DDCAPS *hel) {
    if (!hal && !hel) return DDERR_INVALIDPARAMS;
    if (!D7_FillCaps(hal) || !D7_FillCaps(hel)) return DDERR_INVALIDPARAMS;
    return DD_OK;
}
D7FN(ED_GetGDISurface, DDERR_UNSUPPORTED, DWORD a)
D7FN(ED_GetScanLine, DDERR_UNSUPPORTED, DWORD a)
D7FN(ED_Initialize, DDERR_ALREADYINITIALIZED, DWORD a)
D7FN(ED_RestoreDisplayMode, DD_OK)
D7FN(ED_WaitForVerticalBlank, DD_OK, DWORD a, DWORD b)
D7FN(ED_GetSurfaceFromDC, DDERR_UNSUPPORTED, DWORD a, DWORD b)
D7FN(ED_RestoreAllSurfaces, DD_OK)
D7FN(ED_TestCooperativeLevel, DD_OK)
D7FN(ED_StartModeTest, DDERR_UNSUPPORTED, DWORD a, DWORD b, DWORD c)
D7FN(ED_EvaluateMode, DDERR_UNSUPPORTED, DWORD a, DWORD b)

static void *g_d7DDVt[30] = {
    (void*)ED_QueryInterface, (void*)ED_AddRef, (void*)ED_Release, (void*)ED_Compact,
    (void*)ED_CreateClipper, (void*)ED_CreatePalette, (void*)ED_CreateSurface,
    (void*)ED_DuplicateSurface, (void*)ED_EnumDisplayModes, (void*)ED_EnumSurfaces,
    (void*)ED_FlipToGDISurface, (void*)ED_GetCaps, (void*)ED_GetDisplayMode,
    (void*)ED_GetFourCCCodes, (void*)ED_GetGDISurface, (void*)ED_GetMonitorFrequency,
    (void*)ED_GetScanLine, (void*)ED_GetVerticalBlankStatus, (void*)ED_Initialize,
    (void*)ED_RestoreDisplayMode, (void*)ED_SetCooperativeLevel, (void*)ED_SetDisplayMode,
    (void*)ED_WaitForVerticalBlank, (void*)ED_GetAvailableVidMem, (void*)ED_GetSurfaceFromDC,
    (void*)ED_RestoreAllSurfaces, (void*)ED_TestCooperativeLevel, (void*)ED_GetDeviceIdentifier,
    (void*)ED_StartModeTest, (void*)ED_EvaluateMode
};

static int D7E_Forced(void) {
    char b[8];
    return GetEnvironmentVariableA("SMD_FORCE_D3D7_EMU", b, sizeof(b)) != 0;
}

static int g_D7E_ConfigCreated = 0;

static void D7E_WriteConfigAPI(const char *api) {
    char path[MAX_PATH];
    ExeDirFile("raindrop.ini", path);
    WritePrivateProfileStringA("Graphics", "API", api, path);
}

static int D7E_ConfigD3D9(void) {
    static int initialized = 0;
    static int useD3D9 = 0;
    char path[MAX_PATH];
    char value[32];
    if (!initialized) {
        ExeDirFile("raindrop.ini", path);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                static const char defaults[] = "[Graphics]\r\nAPI=DirectDraw\r\n";
                DWORD written;
                WriteFile(file, defaults, (DWORD)(sizeof(defaults) - 1), &written, NULL);
                CloseHandle(file);
                g_D7E_ConfigCreated = 1;
                Log("Created raindrop.ini with default Graphics API");
            }
        }
        GetPrivateProfileStringA("Graphics", "API", "DirectDraw", value, sizeof(value), path);
        useD3D9 = (_stricmp(value, "D3D9") == 0 || _stricmp(value, "Direct3D9") == 0);
        initialized = 1;
        Log("Graphics API: %s (raindrop.ini or default)", useD3D9 ? "D3D9" : "DirectDraw");
    }
    return useD3D9;
}

static void D7E_SaveFallbackAPI(void) {
    if (g_D7E_ConfigCreated) {
        D7E_WriteConfigAPI("D3D9");
        g_D7E_ConfigCreated = 0;
        Log("Saved D3D9 fallback to raindrop.ini");
    }
}

static HRESULT D7E_CreateDD7(void **out) {
    if (!out) return DDERR_INVALIDPARAMS;
    g_d7.active = 1;
    g_d7DD.ref++;
    *out = &g_d7DD;
    Log("D3D7 emu: using the built-in DirectDraw7/Direct3D7 -> Direct3D9 layer");
    return DD_OK;
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
        Log("[Error] LoadSystem32 ddraw.dll failed (err=%08X)", (unsigned)GetLastError());
        return;
    }
    Log("Real ddraw: using %s", sys);
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

typedef struct { GUID ids[16]; unsigned count; } DDAdapterList;
static BOOL WINAPI CollectDDAdapter(GUID *id, LPSTR desc, LPSTR name, LPVOID context, HMONITOR monitor) {
    DDAdapterList *list = (DDAdapterList*)context;
    (void)desc; (void)name; (void)monitor;
    if (id && list->count < sizeof(list->ids) / sizeof(list->ids[0]))
        list->ids[list->count++] = *id;
    return TRUE;
}

typedef struct { IDirectDraw7Vtbl *lpVtbl; IDirectDraw7 *orig; } DD7W;

static const GUID k_IID_IDirectDraw7 = {
    0x15E65EC0, 0x3B9C, 0x11D2,
    { 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B }
};
static const GUID k_IID_IDirect3D7 = {
    0xF5049E77, 0x4861, 0x11D2,
    { 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8 }
};
static const GUID k_IID_IDirectDraw = {
    0x6C14DB80, 0xA733, 0x11CE,
    { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 }
};
static const GUID k_IID_IDirectDraw2 = {
    0xB3A6F3E0, 0x2B43, 0x11CF,
    { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 }
};
static const GUID k_IID_IDirectDraw3 = {
    0x618F8AD4, 0x8B7A, 0x11D0,
    { 0x8F, 0xCC, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0x9D }
};
static const GUID k_IID_IDirectDraw4 = {
    0x9C59509A, 0x39BD, 0x11D1,
    { 0x8C, 0x4A, 0x00, 0xC0, 0x4F, 0xD9, 0x30, 0xC5 }
};

static HRESULT STDMETHODCALLTYPE WDD7_QueryInterface(IDirectDraw7 *self, REFIID riid, void **out) {
    DD7W *w = (DD7W*)self;
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualCLSID(riid, &k_IID_IDirectDraw) ||
        IsEqualCLSID(riid, &k_IID_IDirectDraw2) ||
        IsEqualCLSID(riid, &k_IID_IDirectDraw3) ||
        IsEqualCLSID(riid, &k_IID_IDirectDraw4)) {
        IDirectDraw7_AddRef(w->orig);
        *out = self;
        return DD_OK;
    }
    if (!IsEqualCLSID(riid, &k_IID_IDirectDraw7)) {
        HRESULT hr = IDirectDraw7_QueryInterface(w->orig, riid, out);
        if (FAILED(hr))
            Log("DD7.QI failed iid=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X hr=%08X",
                (unsigned long)riid->Data1, (unsigned)riid->Data2, (unsigned)riid->Data3,
                (unsigned)riid->Data4[0], (unsigned)riid->Data4[1],
                (unsigned)riid->Data4[2], (unsigned)riid->Data4[3],
                (unsigned)riid->Data4[4], (unsigned)riid->Data4[5],
                (unsigned)riid->Data4[6], (unsigned)riid->Data4[7], hr);
        else if (g_ApiCensus && IsEqualCLSID(riid, &k_IID_IDirect3D7))
            CensusHookD3D7(*out);
        return hr;
    }
    IDirectDraw7_AddRef(w->orig);
    *out = self;
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
    if (hr < 0 && g_fullscreenRequested && d &&
        (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        TriggerWindowFallback("CreateSurface(primary) failed");
    if (hr >= 0 && g_LowLatency && d && s && *s &&
        (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        HookPrimary(*s);
    if (hr >= 0 && g_ApiCensus && s && *s)
        CensusHookSurface(*s);
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
    HotkeyAttach(hwnd);
    D7_BringToFront(hwnd);
    if (flags & DDSCL_EXCLUSIVE) g_fullscreenRequested = 1;
    HRESULT hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, flags);
    Log("DD7.SetCooperativeLevel hwnd=%p flags=%X -> hr=%08X",
        (void*)hwnd, (unsigned)flags, hr);
    if (hr >= 0 || !g_RetryCoopLevel) return hr;

    if (flags & DDSCL_EXCLUSIVE) {
        DWORD f = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        Log("DD7.SetCooperativeLevel retry1 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
        TriggerWindowFallback("SetCooperativeLevel(exclusive) failed");
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
    if (hr < 0 && g_RetryDisplayMode)
        hr = TrySetDisplayMode(w7->orig, wd, ht, bpp, refresh, flags);
    if (hr < 0) {
        g_displayModeFailures++;
        if (bpp <= 16 || g_displayModeFailures >= 2)
            TriggerWindowFallback("SetDisplayMode failed");
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
    if (!IsEqualCLSID(riid, &k_IID_IDirectDraw7)) {
        if (!p_DirectDrawCreateEx) return DDERR_GENERIC;
        PF_DDCEx fn = (PF_DDCEx)p_DirectDrawCreateEx;
        HRESULT hr = fn(guid, out, riid, unk);
        Log("DirectDrawCreateEx (other) -> hr=%08X", hr);
        return hr;
    }
    if (g_D7Emu && (D7E_Forced() || D7E_ConfigD3D9())) {
        Log("GraphicsAPI: selecting built-in Direct3D9 emulation");
        return D7E_CreateDD7(out);
    }
    if (!p_DirectDrawCreateEx) {
        if (g_D7Emu) {
            Log("System DirectDrawCreateEx is unavailable; switching to Direct3D9 emulation");
            D7E_SaveFallbackAPI();
            return D7E_CreateDD7(out);
        }
        return DDERR_GENERIC;
    }
    PF_DDCEx fn = (PF_DDCEx)p_DirectDrawCreateEx;
    IDirectDraw7 *orig = NULL;
    HRESULT hr = fn(guid, (LPVOID*)&orig, riid, unk);
    Log("DirectDrawCreateEx(IID_IDirectDraw7) -> hr=%08X orig=%p", hr, (void*)orig);
    if (hr < 0 || !orig) {
        if (g_D7Emu) {
            Log("DirectDraw7 creation failed (hr=%08X); switching to Direct3D9 emulation", (unsigned)hr);
            D7E_SaveFallbackAPI();
            return D7E_CreateDD7(out);
        }
        return hr;
    }
        IUnknown *d3d7 = NULL;
        HRESULT d3dHr = IDirectDraw7_QueryInterface(orig, &k_IID_IDirect3D7, (void**)&d3d7);
        if (d3dHr >= 0 && d3d7) {
            IUnknown_Release(d3d7);
            Log("Direct3D7 probe: available on default adapter");
        } else {
            Log("Direct3D7 probe failed on default adapter (hr=%08X); checking other adapters", (unsigned)d3dHr);
            IDirectDraw7_Release(orig);
            orig = NULL;
            if (p_DirectDrawEnumerateExA) {
                DDAdapterList list;
                unsigned i;
                memset(&list, 0, sizeof(list));
                ((PF_DDEnumEx)p_DirectDrawEnumerateExA)(CollectDDAdapter, &list,
                    DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES);
                for (i = 0; i < list.count; ++i) {
                    IDirectDraw7 *adapter = NULL;
                    HRESULT adapterHr = fn(&list.ids[i], (LPVOID*)&adapter, riid, unk);
                    if (adapterHr < 0 || !adapter) continue;
                    d3d7 = NULL;
                    d3dHr = IDirectDraw7_QueryInterface(adapter, &k_IID_IDirect3D7, (void**)&d3d7);
                    if (d3dHr >= 0 && d3d7) {
                        IUnknown_Release(d3d7);
                        Log("Direct3D7 probe: available on enumerated adapter %u", i);
                        orig = adapter;
                        hr = adapterHr;
                        break;
                    }
                    IDirectDraw7_Release(adapter);
                    Log("Direct3D7 probe failed on enumerated adapter %u (hr=%08X)", i, (unsigned)d3dHr);
                }
                if (!orig) {
                    Log("No enumerated adapter exposes Direct3D7");
                }
            }
            if (orig) goto ddraw_ready;
            if (g_D7Emu) {
                Log("No usable system Direct3D7 interface (hr=%08X); switching to Direct3D9 emulation",
                    (unsigned)d3dHr);
                D7E_SaveFallbackAPI();
                return D7E_CreateDD7(out);
            }
            return d3dHr;
        }
ddraw_ready:
    IDirectDraw7 *wrapped = WrapDD7(orig);
    if (wrapped != orig) {
        Log("DD7 wrapped ok");
        IDirectDraw7_Release(orig);
    }
    *out = wrapped;
    if (g_ApiCensus && wrapped != orig)
        CensusHookVtbl((void**)&g_dd7Vtbl, CEN_DD7, 30, 0);
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

/*
 * Extra practice: no replay save prompt.
 *
 * When a stage ends, the scene update (0x413ab0) goes to the title for a practice run (BYTE 0x4dade0 != 0)
 * and to scene 5 (replay save prompt) otherwise: `push 0; push 5; call 0x4254d0` at 0x413bdb.  The Extra
 * practice is started like a normal Extra run (0x4dade0 = 0), so it ended in the replay save prompt.
 * Every `push 0; push 5; call 0x4254d0` (cdecl, 0x4254d0(scene, force)) in the code is redirected to a hook
 * that asks for the title (scene 1) instead while an Extra practice run (g_exRun) is active.
 */
typedef void (__cdecl *PF_SceneSwitch)(int scene, int force);

static void __cdecl StageEndSceneHook(int scene, int force) {
    if (scene == 5 && g_exRun) {
        Log("ExArea: practice run ended, going to the title instead of the replay save scene");
        scene = 1;
    }
    ((PF_SceneSwitch)(uintptr_t)0x4254d0)(scene, force);
}

static void PatchExNoReplaySave(void) {
    unsigned char *p = (unsigned char*)(uintptr_t)0x401000;
    unsigned char *end = (unsigned char*)(uintptr_t)0x450000;
    int n = 0;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("ExNoReplaySave: unexpected image base, skipped");
        return;
    }
    for (; p + 9 <= end; p++) {
        if (p[0] == 0x6a && p[1] == 0x00 && p[2] == 0x6a && p[3] == 0x05 && p[4] == 0xe8) {
            if (HookCall((DWORD)(uintptr_t)(p + 4), 0x4254d0, (void*)&StageEndSceneHook)) {
                Log("ExNoReplaySave: hooked scene 5 switch at 0x%08X", (unsigned)(uintptr_t)(p + 4));
                n++;
            }
        }
    }
    Log("ExNoReplaySave: patched %d call site(s)", n);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        LogOpen();
        QueryPerformanceFrequency(&g_qpcFreq);
        Log("process attach (pid=%lu)", GetCurrentProcessId());
        LogWindowsVersion();
        CensusInit();
        InitFallbackInstance();
        PatchColorDepth32();
        PatchForceWindowMode();
        PatchTitleCancel();
        PatchOptionDefaults();
        PatchPauseKeys();
        PatchReplayPause();
        PatchReplayLoadCapture();
        PatchReplaySaveCancel();
        PatchResultRetry();
        PatchReplayStage();
        PatchReplayInputTrace();
        PatchPracticePower();
        PatchExAreaSelect();
        PatchExNoReplaySave();
        PatchReplayFpu();
        PatchGamePerf();
        PatchPauseAudio();
        PatchSoundFix();
        PatchFrameWait();
        PatchEnterMask();
        PatchKeepRunningInactive();
        PatchModernController();
        break;
    case DLL_PROCESS_DETACH:
        Log("process detach");
        CensusSummary();
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
