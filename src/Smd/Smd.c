#define CINTERFACE
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <mmsystem.h>
#include <dinput.h>
#include <mmreg.h>
#include <dsound.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Smd_internal.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

static FILE *g_log = NULL;
const int g_LogAll = 0;
const int g_StripAllowModeX = 1;
const int g_RetryCoopLevel = 1;
const int g_RetryDisplayMode = 1;
#ifndef SMD_LOWLATENCY
#define SMD_LOWLATENCY 1
#endif
#ifndef SMD_DWMFLUSH
#define SMD_DWMFLUSH 1
#endif
const int g_LowLatency = SMD_LOWLATENCY;
const int g_UseDwmFlush = SMD_DWMFLUSH;
static const int g_StatsIntervalMs = 5000;
static const int g_KeyLatencyStats = 0;
static const int g_WaitTimeoutMs = 50;
static const int g_ForceColorDepth32 = 1;
static const int g_AutoWindowFallback = 1;
static const int g_ForceWindowMode = 0;
static int g_fallbackInstance = 0;

static void ExeDirFile(const char *name, char *out) {
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(out, name);
}

#ifdef SMD_DEBUG_BUILD
static void LogOpen(void) {
    if (g_log) return;
    char path[MAX_PATH];
    ExeDirFile("Smd_ddraw.log", path);
    g_log = fopen(path, "a");
}

void SmdLog(const char *fmt, ...) {
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
#else
static void LogOpen(void) {}
void SmdLog(const char *fmt, ...) { (void)fmt; }
#endif

void SmdLogOpen(void) { LogOpen(); }
#define Log SmdLog

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

void SmdTriggerWindowFallback(const char *why) {
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

static int SmdIsWine(void) {
    static int v = -1;
    if (v < 0) {
        HMODULE ntd = GetModuleHandleA("ntdll.dll");
        v = (ntd && GetProcAddress(ntd, "wine_get_version")) ? 1 : 0;
    }
    return v;
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
    if (g_LowLatency && !flip && g_UseDwmFlush && g_pDwmFlush && !SmdIsWine()) g_pDwmFlush();
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

static void SmdWineForceBorderless(HWND hwnd) {
    MONITORINFO mi;
    HMONITOR mon;
    DWORD style;
    if (!hwnd || !IsWindow(hwnd)) return;
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi)) return;
    style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongA(hwnd, GWL_STYLE, (LONG)style);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Wine: when the game blits to the primary in "fullscreen", the 640x480 image can
   land in the top-left of a larger (or badly sized) window.  Stretch to the window. */
static void SmdWineStretchPrimary(IDirectDrawSurface7 *self, LPDIRECTDRAWSURFACE7 src, LPRECT sr) {
    static int logged, fsLogged, dbg;
    HDC wdc = NULL;
    HWND hw = NULL;
    RECT cr;
    DDSURFACEDESC2 pd, sd;
    int cw, ch, sw = 640, sh = 480;
    if (!SmdIsWine() || !src) return;
    memset(&pd, 0, sizeof(pd)); pd.dwSize = sizeof(pd);
    if (SUCCEEDED(IDirectDrawSurface7_GetSurfaceDesc(self, &pd))) hw = (HWND)pd.lpSurface;
    if (!hw || !IsWindow(hw)) hw = GetActiveWindow();
    if (!hw || !IsWindow(hw)) hw = GetForegroundWindow();
    if (!hw || !GetClientRect(hw, &cr)) {
        if (dbg < 3) { dbg++; Log("WineStretch: no hwnd/client"); }
        return;
    }
    cw = cr.right - cr.left; ch = cr.bottom - cr.top;
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    if (SUCCEEDED(IDirectDrawSurface7_GetSurfaceDesc(src, &sd)) && sd.dwWidth && sd.dwHeight) { sw = (int)sd.dwWidth; sh = (int)sd.dwHeight; }
    if (sr) { sw = sr->right - sr->left; sh = sr->bottom - sr->top; }
    /* Wine reports a 1920x1080 desktop-sized surface in "fullscreen" while rendering
       640x480 content into its top-left; stretch the game rect to the window. */
    if (sw > 800 || sh > 600) { sw = 640; sh = 480; sr = NULL; }
    if (cw <= sw + 8 && ch <= sh + 8) {
        SmdWineForceBorderless(hw);
        if (!fsLogged) { fsLogged = 1; Log("WineStretch: forced borderless on hwnd=%p (client was %dx%d)", hw, cw, ch); }
        if (GetClientRect(hw, &cr)) { cw = cr.right - cr.left; ch = cr.bottom - cr.top; }
    }
    if (cw <= sw + 8 && ch <= sh + 8) {
        if (dbg < 5) { dbg++; Log("WineStretch: skip client=%dx%d src=%dx%d hw=%p", cw, ch, sw, sh, hw); }
        return;
    }
    /* Blit to the primary DC (overwrites the 640x480 top-left region), then
       stretch that region to fill the window. */
    {
        HDC pdc = NULL;
        {
            HDC memdc = CreateCompatibleDC(NULL);
            HBITMAP bm = CreateCompatibleBitmap(GetDC(NULL), sw, sh);
            HGDIOBJ oldbm = NULL;
            if (memdc && bm) oldbm = SelectObject(memdc, bm);
            if (SUCCEEDED(IDirectDrawSurface7_GetDC(self, &pdc)) && memdc && bm) {
                BitBlt(memdc, 0, 0, sw, sh, pdc, 0, 0, SRCCOPY);
                IDirectDrawSurface7_ReleaseDC(self, pdc); pdc = NULL;
                wdc = GetDC(hw);
                if (wdc) {
                    RECT zr = {0,0,sw,sh};
                    FillRect(wdc, &zr, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    SetStretchBltMode(wdc, COLORONCOLOR);
                    StretchBlt(wdc, 0, 0, cw, ch, memdc, 0, 0, sw, sh, SRCCOPY);
                    ReleaseDC(hw, wdc);
                    if (!logged) { logged = 1; Log("WineStretch: primary stretched %dx%d -> client %dx%d (hwnd=%p)", sw, sh, cw, ch, hw); }
                }
            }
            if (pdc) IDirectDrawSurface7_ReleaseDC(self, pdc);
            if (memdc) { if (oldbm) SelectObject(memdc, oldbm); DeleteDC(memdc); }
            if (bm) DeleteObject(bm);
        }
    }
}

/* DirectDraw present pacing (real ddraw path): aligns frame presents to 16.667 ms boundaries to smooth
   60 fps jitter.  Skips when present/vsync is already the bottleneck. */
static LARGE_INTEGER g_ddLastPresent;
static double g_ddCostEwmaMs;
static int g_ddPaceTimerInit;
static HANDLE g_ddPaceTimer;
static int g_ddPaceHiRes;
static const double SMD_PACE_INTERVAL_MS = 16.666667;

static void DDPaceWait(void) {
    LARGE_INTEGER now;
    double rem, margin, wait;
    if (!g_qpcFreq.QuadPart || !g_ddLastPresent.QuadPart) return;
    if (g_ddCostEwmaMs > 4.0) return;
    QueryPerformanceCounter(&now);
    rem = SMD_PACE_INTERVAL_MS -
        (double)(now.QuadPart - g_ddLastPresent.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
    if (rem <= 0.3 || rem > 4.0) return;
    if (!g_ddPaceTimerInit) {
        typedef HANDLE (WINAPI *PF_CWTExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        HMODULE k = GetModuleHandleA("kernel32.dll");
        PF_CWTExW cw = k ? (PF_CWTExW)(void*)GetProcAddress(k, "CreateWaitableTimerExW") : NULL;
        g_ddPaceTimerInit = 1;
        if (cw && !SmdIsWine()) {
            g_ddPaceTimer = cw(NULL, NULL, 2, TIMER_ALL_ACCESS);
            if (g_ddPaceTimer) g_ddPaceHiRes = 1;
        }
    }
    margin = g_ddPaceHiRes ? 0.4 : 2.0;
    wait = rem - margin;
    if (g_ddPaceHiRes && g_ddPaceTimer && wait > 0.1) {
        LARGE_INTEGER lt;
        lt.QuadPart = -(LONGLONG)(wait * 10000.0);
        if (SetWaitableTimer(g_ddPaceTimer, &lt, 0, NULL, NULL, FALSE))
            MsgWaitForMultipleObjects(1, &g_ddPaceTimer, FALSE, INFINITE, QS_ALLINPUT);
    } else if (wait > 1.0) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)wait, QS_ALLINPUT);
    }
    for (;;) {
        LARGE_INTEGER cur;
        QueryPerformanceCounter(&cur);
        if ((double)(cur.QuadPart - g_ddLastPresent.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart
            >= SMD_PACE_INTERVAL_MS - 0.05) break;
    }
}

static void DDPaceUpdate(LARGE_INTEGER t1, double cost) {
    g_ddLastPresent = t1;
    g_ddCostEwmaMs = g_ddCostEwmaMs ? g_ddCostEwmaMs * 0.85 + cost * 0.15 : cost;
}

static HRESULT STDMETHODCALLTYPE Hook_Blt(IDirectDrawSurface7 *self, LPRECT dr, LPDIRECTDRAWSURFACE7 src,
                                          LPRECT sr, DWORD flags, LPDDBLTFX fx) {
    LARGE_INTEGER a, b;
    RECT wdr, gsr;
    LPRECT useDr = dr, useSr = sr;
    BatchFlush();
    if (SmdIsWine() && self == g_primary && src) {
        HWND hw = NULL; DDSURFACEDESC2 pd, sd; RECT cr;
        int sw = 640, sh = 480, cw = 0, ch = 0;
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        if (SUCCEEDED(IDirectDrawSurface7_GetSurfaceDesc(src, &sd)) && sd.dwWidth > 800 && sd.dwHeight > 600)
            { gsr.left = 0; gsr.top = 0; gsr.right = sw; gsr.bottom = sh; useSr = &gsr; }
        memset(&pd, 0, sizeof(pd)); pd.dwSize = sizeof(pd);
        if (SUCCEEDED(IDirectDrawSurface7_GetSurfaceDesc(self, &pd))) hw = (HWND)pd.lpSurface;
        if (!hw || !IsWindow(hw)) hw = GetActiveWindow();
        if (!hw || !IsWindow(hw)) hw = GetForegroundWindow();
        if (hw && GetClientRect(hw, &cr)) { cw = cr.right - cr.left; ch = cr.bottom - cr.top; }
        if (cw <= 648 && ch <= 488 && hw) { SmdWineForceBorderless(hw); if (GetClientRect(hw,&cr)){cw=cr.right-cr.left;ch=cr.bottom-cr.top;} }
        /* Wine ddraw Blt-scaling on a desktop-sized primary is unstable; keep native blit.
           Run in a window (WndMode) + gamescope for stable fullscreen scaling. */
        useDr = dr; useSr = sr;
    }
    /* Hook_Blt: pace before primary-present blits */
    int isPresent = (self == g_primary && src);
    if (isPresent) DDPaceWait();
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origBlt(self, useDr, src, useSr, flags, fx);
    QueryPerformanceCounter(&b);
    if (hr >= 0 && isPresent && g_measureReady) {
        DDPaceUpdate(b, TicksToMs(b.QuadPart - a.QuadPart));
        FinishPresent(self, 0, a, b);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Flip(IDirectDrawSurface7 *self, LPDIRECTDRAWSURFACE7 target, DWORD flags) {
    LARGE_INTEGER a, b;
    BatchFlush();
    if (self == g_primary) DDPaceWait();
    QueryPerformanceCounter(&a);
    HRESULT hr = g_origFlip(self, target, flags);
    QueryPerformanceCounter(&b);
    if (hr >= 0 && self == g_primary && g_measureReady) {
        DDPaceUpdate(b, TicksToMs(b.QuadPart - a.QuadPart));
        FinishPresent(self, 1, a, b);
    }
    return hr;
}

void SmdHookPrimary(IDirectDrawSurface7 *s) {
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
static void *g_OptionDefaultScene;  /* first texture of the option screen the row was built for */
static int g_OptionDefaultRowInit;
static char g_OptionDefaultLabelRow[0xe8] __attribute__((aligned(16)));
static void *g_OptionDefaultLabelScene;
static int g_OptionDefaultLabelReady;
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
 * so the row and its surface are rebuilt whenever +0x00 changes.  The game's native text renderer is
 * used so this label shares the option menu's built-in font style.
 */
static int g_OptionDefaultLabelReady;

static void OptionDefaultDraw(void *opt) {
    typedef void (__attribute__((thiscall)) *PF_OptionLabelDraw)(void *row);
    char *source = (char*)opt + 0x1d48 + 8 * 0xe8;
    double targetY = 338.0;
    if (g_OptionDefaultLabelScene != *(void**)opt) {
        g_OptionDefaultLabelScene = *(void**)opt;
        memcpy(g_OptionDefaultLabelRow, source, sizeof(g_OptionDefaultLabelRow));
        memcpy(g_OptionDefaultLabelRow + 0x10, &targetY, sizeof(targetY));
        g_OptionDefaultLabelReady = 1;
    }
    if (g_OptionDefaultLabelReady) {
        OPT_F(g_OptionDefaultLabelRow, 0x20) = OPT_F(opt, 0x3310) == 7 ? 0xff : 0x66;
        ((PF_OptionLabelDraw)(uintptr_t)0x4200a0)(g_OptionDefaultLabelRow);
    }
}

static int g_optPrevEnter;

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
static int g_pauseAfterLoad = 0;
static int g_pauseLoadArmed = 0;
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
static int g_exSimulatingStageFrame = 0;
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
    /* Practice frame simulation must not consume live keyboard or controller input. */
    if (g_exSimulatingStageFrame) return 0;
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
static int g_practiceBossStage = 0;
static int g_practiceBossActive = 0;
static int g_practiceBossTarget = 0;
static int g_practiceScorePending = 0;
static int g_practiceScoreActive = 0;
static int g_practiceScoreStage = 0;
static volatile int g_postJumpLog = 0;  /* frames remaining to trace after a practice jump */
static int g_practiceBgmSeekPending = 0;
static int g_practiceBgmSeekDeadline = 0;
static int g_practiceBgmSeekStartFrame = 0;   /* the BGM is loaded by another thread: do not touch it right after the jump */
static int g_practiceBgmSeekSeconds = 0;
static int g_practiceBgmFadeInFrames = 0;
static int g_practiceBgmFadeInVolume = 0;
static int g_practiceBgmStartedDuringSeek = 0;
static int g_practiceBgmLoaded = 0;   /* a BGM id was actually passed to the real loader this run */
static int g_practiceBgmLastId = -1;
static int g_practiceBgmSimLastId = -1;
static int g_practiceBossSeekingSecondForm = 0;
static int g_practiceBossFormBgmChanged = 0;
static int g_practiceBossLiveTransition = 0;
static int g_practiceBossLiveKillAtFrame = 0;
static int g_practiceBossLiveDefeated = 0;
static int g_practiceBossLiveMissing = 0;
static unsigned *g_practiceBossLiveFirstObject = NULL;
static int g_practiceStage3EventLog = 0;
static int g_exBoss2Kill = 0;         /* Boss 2 practice: defeat Boss 1 at once (see ExBoss2Step) */
static int g_exBoss2Transition = 0;   /* suppress dialogue until Boss 2's battle BGM starts */
static int g_exBoss2KillFrames = 0;
static int g_exBoss1Defeated = 0;
static int g_exBoss2BgmSeen = 0;      /* Boss 2's battle BGM has started (VM bgm op seen outside the fast-forward) */
static int g_exBoss2ObjectSeen = 0;
static int g_exBoss2BgmFrame = 0;
static int g_exBoss2TransitionFastForwarded = 0;
static int g_exBoss1BgmPending = 0;
static int g_exBoss1DonePending = 0;
static int g_exBoss2ScoreReset = 0;
static int g_exBossReturnAtFrame = 0;
static int g_exBossClearSim = 0;      /* the stage clear opcode was called while simulating */
static int g_exBoss2Logged = 0;
static unsigned *g_exBoss1Obj = NULL;   /* the Boss 1 object being defeated (so that Boss 2 is never touched) */
static unsigned *g_exLastBoss = NULL;

typedef void (__stdcall *PF_GameStart)(int stage);
typedef int (__cdecl *PF_VmBgm)(int id);

static void ApplyPracticePower(int stage, const char *what) {
    int p;
    if (stage == 1 && g_practiceBossActive && g_practiceBossStage == stage &&
        g_practiceBossTarget == 1) {
        p = 5;
    } else if (stage == 1 && g_practiceBossActive && g_practiceBossStage == stage &&
               g_practiceBossTarget == 2) {
        p = 5;
    } else if (stage == 2 || stage == 3) {
        if (*(volatile unsigned char*)(uintptr_t)0x4dade0 == 0) return;
        if ((stage == 2 || stage == 3) && g_practiceBossActive &&
            g_practiceBossStage == stage && g_practiceBossTarget >= 1)
            p = 7;
        else
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
static const int g_exBossDefaultFrame[2] = { 18452, 24017 };
static int g_exBgmTime[EX_BGM_IDS];
static int g_exBossBgmTime[2];
static int g_exAreaN = EX_AREA_N;
static int g_exStartReq = 0;
static int g_practiceBossChoice[3];
static int g_practiceBossStart = 0;
static int g_practiceBossPending = 0;
static int g_exPending = 0;
static int g_exSkipWait = 0;
static int g_exShorten = 0;

static int g_exBossRunLogged = 0;
static volatile int g_exMuteSe = 0;   /* 1 while a practice skip fast-forwards: sound effects are not played */
static int g_exBgmMuted = 0;
static int g_exBgmSavedVol = 0;

/* The BGM stays muted through practice fast-forward and until a requested stream seek completes. */
static void ExBgmMuteSync(void) {
    int shouldMute = g_exMuteSe || g_practiceBgmSeekPending || g_exBoss1BgmPending;
    if (shouldMute && !g_exBgmMuted) {
        g_exBgmSavedVol = *(volatile unsigned char*)(uintptr_t)0x45ab14;
        ((PF_SetVolume)(uintptr_t)0x4117c0)(0);
        g_exBgmMuted = 1;
        Log("ExArea: BGM muted for the fast-forward (volume %d)", g_exBgmSavedVol);
    } else if (!shouldMute && g_exBgmMuted) {
        g_exBgmMuted = 0;
        if (g_practiceBgmFadeInFrames > 0) {
            Log("ExArea: BGM fade-in started from mute (target volume %d)", g_practiceBgmFadeInVolume);
        } else {
            ((PF_SetVolume)(uintptr_t)0x4117c0)(g_exBgmSavedVol);
            Log("ExArea: BGM volume restored to %d", g_exBgmSavedVol);
        }
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
static int PracticeSeekBgm(int seconds);

static void PracticeResetScore(const char *reason, int stage) {
    *(volatile int*)(uintptr_t)0x4dadf0 = 0;
    *(volatile int*)(uintptr_t)0x4dade8 = 0;
    Log("PracticeScore: score and displayed score set to 0 (%s, stage %d)", reason, stage);
}

static void ExOnGameStart(int stage) {
    g_exRun = (stage == 4) && g_exStartReq;
    g_exBossRun = g_exRun ? g_exBossSelected : 0;
    g_exBoss1BgmPending = g_exRun && g_exBossRun == 1;
    g_exBoss2Kill = 0;
    g_exBoss2Transition = 0;
    g_exBoss2KillFrames = 0;
    g_exBoss1Defeated = 0;
    g_exBoss2BgmSeen = 0;
    g_exBoss2ObjectSeen = 0;
    g_exBoss2BgmFrame = 0;
    g_exBoss2TransitionFastForwarded = 0;
    g_exBoss2Logged = 0;
    g_exBoss1Obj = NULL;
    g_exLastBoss = NULL;
    g_exBossRunLogged = 0;
    g_exBoss1DonePending = 0;
    g_exBoss2ScoreReset = 0;
    g_exBossReturnAtFrame = 0;
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
        g_exBoss1BgmPending = g_exRun && g_exBossRun == 1;
        g_exBoss2Kill = 0;
        g_exBoss2Transition = 0;
        g_exBoss2KillFrames = 0;
        g_exBoss1Defeated = 0;
        g_exBoss2BgmSeen = 0;
        g_exBoss2ObjectSeen = 0;
        g_exBoss2BgmFrame = 0;
        g_exBoss2TransitionFastForwarded = 0;
        g_exBoss1DonePending = 0;
        g_exBoss2ScoreReset = 0;
        g_exBossReturnAtFrame = 0;
        g_exBossClearSim = 0;
        g_exBoss2Logged = 0;
        g_exBoss1Obj = NULL;
        g_exLastBoss = NULL;
        g_exBossRunLogged = 0;
        g_exPending = 1;
        ExArm();
    }
}

static int g_practiceReturn = 0;   /* the last game start was a practice run: the title opens the Practice menu */

static void __stdcall GameStartHook(int stage) {
    int practiceStart = g_practiceScorePending && stage == g_practiceScoreStage;
    g_practiceReturn = (*(volatile unsigned char*)(uintptr_t)0x4dade0 != 0);
    g_practiceBgmSeekPending = 0;
    g_practiceBgmFadeInFrames = 0;
    g_practiceBgmStartedDuringSeek = 0;
    g_practiceBgmLoaded = 0;
    g_pauseAfterLoad = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    g_pauseLoadArmed = 1;
    Log("Pause: stage %d load started (Escape held=%d)", stage, g_pauseAfterLoad);
    g_practiceBossPending = g_practiceBossStart == stage;
    if (g_practiceBossPending) {
        g_practiceBgmLastId = -1;
        g_practiceBgmSimLastId = -1;
        g_practiceBgmSimLastId = -1;
        g_practiceBgmStartedDuringSeek = 0;
        g_practiceBgmLoaded = 0;
        g_practiceBossSeekingSecondForm = 0;
        g_practiceBossFormBgmChanged = 0;
        g_practiceBossLiveTransition = 0;
        g_practiceBossLiveKillAtFrame = 0;
        g_practiceBossLiveDefeated = 0;
        g_practiceBossLiveMissing = 0;
        g_practiceBossLiveFirstObject = NULL;
        g_practiceStage3EventLog = 0;
    }
    ((PF_GameStart)(uintptr_t)0x423880)(stage);
    ExOnGameStart(stage);
    if (practiceStart) {
        g_practiceScorePending = 0;
        g_practiceScoreActive = 1;
        PracticeResetScore("start", stage);
    } else {
        g_practiceScorePending = 0;
        g_practiceScoreActive = 0;
        g_practiceScoreStage = 0;
    }
    if (g_practiceBossPending) {
        ExDlgInstall();
        Log("PracticeBoss: stage %d boss start requested", stage);
    }
    g_practiceBossStart = 0;
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
    g_practiceBgmSeekPending = 0;
    g_practiceBgmFadeInFrames = 0;
    g_practiceBgmStartedDuringSeek = 0;
    g_practiceBgmLoaded = 0;
    ((PF_GameStart)(uintptr_t)0x419d10)(stage);
    if (g_practiceScoreActive && stage == g_practiceScoreStage) {
        PracticeResetScore("retry", stage);
    }
    if (g_practiceBossActive && stage == g_practiceBossStage) {
        g_practiceBgmLastId = -1;
        g_practiceBgmSimLastId = -1;
        g_practiceBossSeekingSecondForm = 0;
        g_practiceBossFormBgmChanged = 0;
        g_practiceBossLiveTransition = 0;
        g_practiceBossLiveKillAtFrame = 0;
        g_practiceBossLiveDefeated = 0;
        g_practiceBossLiveMissing = 0;
        g_practiceBossLiveFirstObject = NULL;
        g_practiceBossPending = 1;
        Log("PracticeBoss: retry requested; Stage %d will seek to the selected boss again", stage);
    }
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
    if (g_exSimulatingStageFrame)
        return ((PF_FrameNoArg)(uintptr_t)target)();
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
    if (g_exSimulatingStageFrame) {
        memset(g_gamePerf, 0, sizeof(g_gamePerf));
        return;
    }
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
    if (!g_exSimulatingStageFrame) DebugDisablePlayerHitbox();
#endif
    return PerfCall(2, 0x42db70);
}
static unsigned char __cdecl PerfObjectChecks(void) {
#ifdef SMD_DEBUG_BUILD
    if (!g_exSimulatingStageFrame) DebugDisablePlayerHitbox();
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
    Log("PracticePower: patched %d/6 call sites", n);
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
static char g_practiceModeRow[3][0xe8] __attribute__((aligned(16)));
static char g_exLabelRow[0xe8] __attribute__((aligned(16)));
static int g_exRowInit[EX_MAX_AREAS];
static int g_practiceModeInit[3];
static void *g_exRowTex[EX_MAX_AREAS];
static int g_exLabelInit;
static void *g_exLabelTex;
static int g_exLastFade = 0;

static void ExRowsReset(void) {
    memset(g_exRowInit, 0, sizeof(g_exRowInit));
    memset(g_exRowTex, 0, sizeof(g_exRowTex));
    g_exLabelInit = 0;
    g_exLabelTex = NULL;
    memset(g_practiceModeInit, 0, sizeof(g_practiceModeInit));
}

static void PracticeRowsExpand(void *obj) {
    int i;
    double *firstY = (double*)((char*)obj + 0x100 + 0x10);
    double *secondY = (double*)((char*)obj + 0x100 + 0xe8 + 0x10);
    if (*secondY - *firstY >= 0x30) return;
    /* The menu has three stage rows followed by Extra and Exit. Move each later
       row down to make a full line available for the Stage/Boss selector. */
    for (i = 1; i < 5; i++) {
        double *y = (double*)((char*)obj + 0x100 + 0xe8 * i + 0x10);
        *y += (i < 4) ? i * 0x20 : 0x60;
    }
}

static void PracticeModeRowEnsure(void *obj, int stage) {
    char *row = g_practiceModeRow[stage];
    void *tex = *(void**)((char*)obj + 0x10);
    static const int y[3] = { 0x98, 0xd8, 0x118 };
    static const char *label[2][3] = {
        { "< Stage >", "< MidBoss >", "< Boss >" },
        { "< Stage >", "< Boss >", NULL }
    };
    if (stage == 2 && g_practiceBossChoice[stage] > 1)
        g_practiceBossChoice[stage] = 1;
    if (g_practiceModeInit[stage]) return;
    g_practiceModeInit[stage] = 1;
    memset(row, 0, 0xe8);
    ((PF_RowCtor)(uintptr_t)0x41fdf0)(row);
    ((PF_RowSetup)(uintptr_t)0x41ff10)(row, tex, 0xe0, y[stage], 0, 0, 0x100, 0x100, 0);
    ((PF_RowText)(uintptr_t)0x41ff80)(row, 0x10, 0xc, 8, 9, 0xb,
        stage == 2 ? label[1][g_practiceBossChoice[stage]] :
                     label[0][g_practiceBossChoice[stage]]);
    *(int*)(row + 0xac) = 0x3f4ccccd;
    *(int*)(row + 0xb0) = 0x3f4ccccd;
    *(int*)(row + 0xd8) = 0x505050;
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
    ((PF_RowSetup)(uintptr_t)0x41ff10)(row, *(void**)((char*)obj + 0x10), 0xe0, 0x158, 0, 0, 0x100, 0x100, 0);
    if (area < EX_AREA_N)
        ((PF_RowText)(uintptr_t)0x41ff80)(row, 0x10, 0xc, 8, 9, 0xb, "< Area %d/%d >", area + 1, g_exAreaN);
    else
        ((PF_RowText)(uintptr_t)0x41ff80)(row, 0x10, 0xc, 8, 9, 0xb, "< Boss %d/2 >", area - EX_AREA_N + 1);
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
    ((PF_RowSetup)(uintptr_t)0x41ff10)(g_exLabelRow, tex, 0x6c, 0x138, 0, 0, 0x100, 0x100, 0);
    ((PF_RowText)(uintptr_t)0x41ff80)(g_exLabelRow, 0x10, 0xc, 8, 9, 0xb,
                                     "Extra      %3d     %3d", 0,
                                     *(volatile int*)(uintptr_t)0x46c7c4);
    *(int*)(g_exLabelRow + 0xac) = 0x3f4ccccd;
    *(int*)(g_exLabelRow + 0xb0) = 0x3f4ccccd;
    *(int*)(g_exLabelRow + 0xd8) = 0x505050;
}

static unsigned char __attribute__((fastcall)) PracticeUpdateCore(void *obj) {
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
    PracticeRowsExpand(obj);
    if (*cur < 0 || *cur >= rows) *cur = 0;

    if (edge(input, 4) || rawkey(input, 0xd)) {
        *(int*)(scene + 0xc) = 0;
        g_practiceScorePending = 1;
        if (*cur < 3) {
            g_practiceScoreStage = *cur + 1;
            *(int*)(scene + 0x10) = *cur + 1;
            *(volatile unsigned char*)(uintptr_t)0x4dade0 = 1;
            *(volatile int*)(uintptr_t)0x4dadd8 = *(int*)(uintptr_t)(0x46bc14 + 4 * *cur) - 1;
            g_exStartReq = 0;
            g_exBossSelected = 0;
            if (g_practiceBossChoice[*cur]) {
                g_practiceBossStart = *cur + 1;
                g_practiceBossStage = *cur + 1;
                g_practiceBossPending = 1;
                g_practiceBossActive = 1;
                g_practiceBossTarget = g_practiceBossChoice[*cur];
                Log("PracticeBoss: Stage %d %s practice start", *cur + 1,
                    *cur < 2 ? (g_practiceBossTarget == 1 ? "MidBoss" : "Boss") :
                               "Boss");
            } else {
                g_practiceBossStart = 0;
                g_practiceBossStage = 0;
                g_practiceBossPending = 0;
                g_practiceBossActive = 0;
                g_practiceBossTarget = 0;
            }
        } else {
            g_practiceScoreStage = 4;
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
    {
        if (hold(input, 2, 0xf)) {
            if (*cur < 3) {
                int maxChoice = (*cur == 2) ? 1 : 2;
                g_practiceBossChoice[*cur] =
                    (g_practiceBossChoice[*cur] + maxChoice) % (maxChoice + 1);
            }
            else {
                g_exAreaChoice = (g_exAreaChoice + g_exAreaN + 1) % (g_exAreaN + 2);
                if (g_exAreaChoice < g_exAreaN) g_exArea = g_exAreaChoice;
            }
            playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
        }
        if (hold(input, 3, 0xf)) {
            if (*cur < 3) {
                int maxChoice = (*cur == 2) ? 1 : 2;
                g_practiceBossChoice[*cur] =
                    (g_practiceBossChoice[*cur] + 1) % (maxChoice + 1);
            }
            else {
                g_exAreaChoice = (g_exAreaChoice + 1) % (g_exAreaN + 2);
                if (g_exAreaChoice < g_exAreaN) g_exArea = g_exAreaChoice;
            }
            playSe((void*)(uintptr_t)0x4ea3b8, 0, 0xff);
        }
    }
    for (i = 0; i < 3; i++) {
        static const char *label[2][3] = {
            { "< Stage >", "< MidBoss >", "< Boss >" },
            { "< Stage >", "< Boss >", NULL }
        };
        if (i == 2 && g_practiceBossChoice[i] > 1)
            g_practiceBossChoice[i] = 1;
        PracticeModeRowEnsure(obj, i);
        ((PF_RowText)(uintptr_t)0x41ff80)(g_practiceModeRow[i], 0x10, 0xc, 8, 9, 0xb,
            i == 2 ? label[1][g_practiceBossChoice[i]] :
                     label[0][g_practiceBossChoice[i]]);
        *(int*)(g_practiceModeRow[i] + 0xd8) = (*cur == i) ? 0xffffff : 0x505050;
    }
    for (i = 0; i < 3; i++) *(int*)(o + 0x1d8 + 0xe8 * i) = 0x505050;
    if (*cur < 3) *(int*)(o + 0x1d8 + 0xe8 * *cur) = 0xffffff;
    v = *fade + 4;
    *fade = v > 0x80 ? 0x80 : v;
    return 1;
}

static int g_exInputSuppress = 0;

typedef unsigned char (__attribute__((thiscall)) *PF_ExInputKey)(void *self, int id);
typedef unsigned char (__attribute__((thiscall)) *PF_ExInputHold)(void *self, int id, int delay);

static int ExInputIsHeld(void *input) {
    int i;
    if (!input || IsBadReadPtr(input, 0x108)) return 0;
    if (*(DWORD*)((char*)input + 4) & 0x1ff) return 1;
    for (i = 0; i < 0x100; i++)
        if (*((unsigned char*)input + 8 + i) & 0x80) return 1;
    return 0;
}

static int ExInputSuppressQuery(void *input) {
    if (!g_exInputSuppress) return 0;
    if (!ExInputIsHeld(input)) {
        g_exInputSuppress = 0;
        return 0;
    }
    return 1;
}

static unsigned char __attribute__((fastcall)) ExInputRawKeyHook(void *self, void *edx, int id) {
    if (ExInputSuppressQuery(self)) return 0;
    return ((PF_ExInputKey)(uintptr_t)0x431500)(self, id);
}

static unsigned char __attribute__((fastcall)) ExInputEdgeHook(void *self, void *edx, int id) {
    if (ExInputSuppressQuery(self)) return 0;
    return ((PF_ExInputKey)(uintptr_t)0x431540)(self, id);
}

static unsigned char __attribute__((fastcall)) ExInputHoldHook(void *self, void *edx, int id, int delay) {
    if (ExInputSuppressQuery(self)) return 0;
    return ((PF_ExInputHold)(uintptr_t)0x431480)(self, id, delay);
}

static void PatchExInputSuppression(void) {
    DWORD a;
    int raw = 0, edge = 0, hold = 0;
    for (a = 0x423180; a < 0x423447; a++) {
        unsigned char *p = (unsigned char*)(uintptr_t)a;
        DWORD target;
        if (*p != 0xe8) continue;
        target = a + 5 + *(DWORD*)(uintptr_t)(a + 1);
        if (target == 0x431500) raw += HookCall(a, target, (void*)&ExInputRawKeyHook);
        else if (target == 0x431540) edge += HookCall(a, target, (void*)&ExInputEdgeHook);
        else if (target == 0x431480) hold += HookCall(a, target, (void*)&ExInputHoldHook);
    }
    Log("ExArea: title input calls filtered raw=%d edge=%d hold=%d", raw, edge, hold);
}

static unsigned char __attribute__((fastcall)) PracticeUpdateHook(void *obj) {
    unsigned char result;
    void *input;
    DWORD buttons, previousButtons;
    unsigned char keys[0x100], keyLatch[0x100];
    int masked = 0, neutral = 1, i;
    if (g_exInputSuppress) {
        input = *(void**)(uintptr_t)0x46bbec;
        if (input && !IsBadReadPtr(input, 0x230)) {
            buttons = *(DWORD*)((char*)input + 4);
            previousButtons = *(DWORD*)((char*)input + 0x12c);
            memcpy(keys, (char*)input + 8, sizeof(keys));
            memcpy(keyLatch, (char*)input + 0x130, sizeof(keyLatch));
            neutral = (buttons & 0x1ff) == 0;
            for (i = 0; i < 0x100; i++) if (keys[i] & 0x80) neutral = 0;
            *(DWORD*)((char*)input + 4) = 0;
            *(DWORD*)((char*)input + 0x12c) = 0;
            memset((char*)input + 8, 0, sizeof(keys));
            memset((char*)input + 0x130, 0, sizeof(keyLatch));
            masked = 1;
        }
        result = PracticeUpdateCore(obj);
        if (masked) {
            *(DWORD*)((char*)input + 4) = buttons;
            *(DWORD*)((char*)input + 0x12c) = previousButtons;
            memcpy((char*)input + 8, keys, sizeof(keys));
            memcpy((char*)input + 0x130, keyLatch, sizeof(keyLatch));
            if (neutral) g_exInputSuppress = 0;
        }
        return result;
    }
    return PracticeUpdateCore(obj);
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
    *count = 8;
    r = ((PF_PracticeFn)(uintptr_t)0x420de0)(obj);
    *count = saved;
    if (*(int*)((char*)obj + 0xc) * 6 > 0xff) {
        char *row;
        int mi;
        for (mi = 0; mi < 3; mi++) ((PF_RowDraw)(uintptr_t)0x420270)(g_practiceModeRow[mi], 0);
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

static int g_exBgmMoveDelta = 0;   /* frames between the moved BGM event's original time and the jump */
static int g_exBgmMoveTime = 0;    /* original time of the moved BGM event */

static void ExSkipTo(int target, int area) {
    unsigned **headp = (unsigned**)(uintptr_t)0x4ea1d8;
    unsigned *prev = NULL, *n = *headp;
    const int dropOldBg = 0;
    int dropped = 0, bgDropped = 0, spawnLogged = 0;
    unsigned layerKey[8];
    int layerLast[8], nLayers = 0, li;

    g_exBgmMoveDelta = 0;
    g_exBgmMoveTime = 0;

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
            g_exBgmMoveTime = bestT;
            g_exBgmMoveDelta = target - bestT;
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

static int PracticeBossDefeat(unsigned *boss) {
    static unsigned char fakeShot[0x830] __attribute__((aligned(16)));
    typedef void (__cdecl *PF_HitHandler)(void *self, void *other);
    PF_HitHandler hit;
    if (!boss || IsBadReadPtr(boss, 0x824)) return 0;
    *(float*)((char*)boss + 0x48) = 0.0f;
    if (*((unsigned char*)boss + 0x109) == 0) return 1;
    hit = (PF_HitHandler)(uintptr_t)boss[0x818 / 4];
    if (!hit || IsBadCodePtr((FARPROC)hit)) return 0;
    memset(fakeShot, 0, sizeof(fakeShot));
    *(int*)(fakeShot + 0x04) = 2;
    *(float*)(fakeShot + 0x48) = 1.0f;
    hit(boss, fakeShot);
    Log("PracticeBoss: forced MidBoss defeat through hit handler %p", (void*)hit);
    return 1;
}

static int PracticeBossSeek(int from, int limit, int targetBoss, int *bossFrame) {
    int f, firstSeen = 0;
    int stage3KillFrame = 0;
    unsigned *boss, *firstBoss = NULL;
    for (f = from; f < limit; f++) {
        if (!ExRunStageFrame(f)) {
            Log("PracticeBoss: stage update stopped while seeking at frame %d", f);
            break;
        }
        if (g_practiceBossStage == 3 && targetBoss == 2 && g_practiceBossFormBgmChanged) {
            *bossFrame = f + 1;
            g_practiceBossSeekingSecondForm = 0;
            g_practiceBossFormBgmChanged = 0;
            Log("PracticeBoss: Stage 3 Boss 2 BGM change reached at frame %d", f + 1);
            return 1;
        }
        if (g_practiceBossStage == 3 && targetBoss == 2 && stage3KillFrame) {
            if (f + 1 - stage3KillFrame > 1800) {
                Log("PracticeBoss: Stage 3 second form BGM not reached within %d frames after defeating Boss 1; "
                    "continuing with the natural transition at frame %d", f + 1 - stage3KillFrame, f + 1);
                g_practiceBossSeekingSecondForm = 1;
                g_practiceBossLiveTransition = 1;
                g_practiceBossLiveDefeated = 0;
                g_practiceBossLiveMissing = 0;
                *bossFrame = f + 1;
                return 1;
            }
            boss = *(unsigned**)(uintptr_t)0x4dae04;
            if (boss && boss == firstBoss && !IsBadReadPtr(boss, 0x824))
                PracticeBossDefeat(boss);
        }
        boss = *(unsigned**)(uintptr_t)0x4dae04;
        if (boss && !IsBadReadPtr(boss, 0x824)) {
            if (!firstSeen) {
                firstSeen = 1;
                firstBoss = boss;
                if (targetBoss == 1) {
                    *bossFrame = f + 1;
                    Log("PracticeBoss: MidBoss object %p appeared at frame %d", (void*)boss, f + 1);
                    return 1;
                }
                if (g_practiceBossStage == 3 && targetBoss == 2) {
                    g_practiceBossFormBgmChanged = 0;
                    g_practiceBossSeekingSecondForm = 1;
                    g_practiceBossLiveTransition = 0;
                    g_practiceBossLiveDefeated = 0;
                    g_practiceBossLiveMissing = 0;
                    g_practiceBossLiveFirstObject = boss;
                    stage3KillFrame = f + 1;
                    PracticeBossDefeat(boss);
                    Log("PracticeBoss: Stage 3 Boss 1 found at frame %d; defeating it to reach the second form BGM (id 0)",
                        f + 1);
                    continue;
                }
                if (!PracticeBossDefeat(boss)) {
                    Log("PracticeBoss: could not defeat the MidBoss during seek");
                    break;
                }
            } else if (boss != firstBoss &&
                       !(g_practiceBossStage == 3 && targetBoss == 2)) {
                *bossFrame = f + 1;
                Log("PracticeBoss: main Boss object %p appeared at frame %d", (void*)boss, f + 1);
                return 1;
            }
        }
        if (f && f % 5000 == 0)
            Log("PracticeBoss: still seeking boss at frame %d", f);
    }
    *bossFrame = f;
    return 0;
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
/* Skip all dialogue opcodes during EX boss practice so no message is shown and the VM can advance. */
static const int g_ExBossDlgSkip = 1;
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
    int skipIntro = i == 21 && g_ExBossDlgSkip &&
                    ((g_exRun && g_exBossRun >= 1) || g_practiceBossActive);
    int hideBoss2Transition = i == 21 && g_ExBossDlgSkip && g_exBossRun == 2 &&
                              g_exBoss2Transition;
    if (i == 21 && ((g_exRun && g_exBossRun >= 1) || g_practiceBossActive) &&
        !g_exSimulatingStageFrame && g_dlgShownLog < 80) {
        const char *action = skipIntro ? "skipped" : (hideBoss2Transition ? "hidden" : "SHOWN");
        g_dlgShownLog++;
        Log("ExDlg: Boss practice stage=%d boss=%d op21 thr=%p ip=%d line=%d frame=%d -> %s",
            g_practiceBossActive ? *(volatile int*)(uintptr_t)0x4dade4 : 4, g_exBossRun, thr, ip,
            IsBadReadPtr(args, 8) ? -1 : (int)((unsigned*)args)[1],
            *(volatile int*)(uintptr_t)0x4dadf8, action);
    }
    if (skipIntro) {
        /* Skipping op21 leaves the dialogue wait flag at 0, so its confirmation loop exits while
           the surrounding entrance and battle script continue normally. */
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
static int g_exReturnPracticeMenuPending = 0;

typedef unsigned char (__attribute__((fastcall)) *PF_TitleUpdate)(void *obj, void *edx);

static unsigned char __attribute__((fastcall)) ExReturnPracticeTitleUpdate(void *obj, void *edx) {
    unsigned char result;
    result = ((PF_TitleUpdate)(uintptr_t)0x421040)(obj, edx);
    if (g_exReturnPracticeMenuPending && obj) {
        /* Open the Practice menu the way the main menu does for its Practice item (cursor 2):
           `lea ecx,[title+0x22e90]; call 0x420b40; mov [title+8],0xc` (state 0xc = Practice menu;
           state 0xb would start a stage).  Cancel in the Practice menu goes back to the main menu. */
        char *title = *(char**)(uintptr_t)0x4e93c8;
        if (title && !IsBadReadPtr(title, 0x23300)) {
            g_exReturnPracticeMenuPending = 0;
            if (*(volatile int*)(uintptr_t)0x46bc10 != 0) {
                ((void (__attribute__((thiscall)) *)(void*))(uintptr_t)0x420b40)(title + 0x22e90);
                *(int*)(title + 0x2324c) = 2;
                *(int*)(title + 0xc) = 0;
                *(int*)(title + 8) = 0xc;
                Log("ExArea: opened the Practice menu after the title update");
            } else {
                Log("ExArea: Practice is not unlocked, staying in the main menu");
            }
        }
    }
    return result;
}

static void PatchExReturnPracticeTitleUpdate(void) {
    static const unsigned char expected[5] = { 0xe9, 0x3c, 0xd8, 0xff, 0xff };
    unsigned char *site = (unsigned char*)(uintptr_t)0x4237ff;
    DWORD oldProtect, rel;
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        Log("ExArea: title update hook signature mismatch");
        return;
    }
    if (!VirtualProtect(site, sizeof(expected), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ExArea: title update hook VirtualProtect failed (%08X)", GetLastError());
        return;
    }
    site[0] = 0xe9;
    rel = (DWORD)(uintptr_t)&ExReturnPracticeTitleUpdate - (0x4237ff + 5);
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, sizeof(expected), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(expected));
    Log("ExArea: title update hook installed");
}

/* Any practice run (Stage 1-3, Extra area/boss) that ends in the title scene (stage end, game over, "title" in
   the pause menu) opens the Practice menu: the title scene init (slot 0 of the title vtable, 0x453f38 ->
   0x4227b0, thiscall) is hooked and, when the last game start was a practice run (BYTE 0x4dade0 != 0, noted
   by GameStartHook) or ExReturnToPracticeMenu asked for it, opens the menu right after the original init
   (the title update hook above only runs while Extra is unlocked, so it is just a fallback). */
static const int g_ReturnToPractice = 1;
typedef unsigned char (__attribute__((fastcall)) *PF_TitleInit)(void *title);
static PF_TitleInit g_origTitleInit = NULL;

static int g_practiceMenuOpenWait = 0;    /* frames until the Practice menu is opened in the new title scene */
static int g_practiceMenuOpenTries = 0;
static int g_practiceMenuWatch = 0;       /* frames since the Practice menu was opened (blank menu watchdog) */

typedef unsigned char (__attribute__((fastcall)) *PF_TitleUpd)(void *title);
static PF_TitleUpd g_origTitleUpd = NULL;

/* Opens the Practice menu the way the main menu's Practice item does (init 0x420b40, cursor 2, state 0xc).
   Returns 0 when the menu's textures could not be created, so the caller can try again. */
static int PracticeMenuOpen(char *t, const char *why) {
    char *menu = t + 0x22e90;
    if (*(volatile int*)(uintptr_t)0x46bc10 == 0) {
        Log("ReturnToPractice: Practice menu not available, staying in the main menu");
        return 1;
    }
    ((void (__attribute__((thiscall)) *)(void*))(uintptr_t)0x420b40)(menu);
    if (!*(void**)(menu + 0x10) || !*(void**)(menu + 0x14)) {
        Log("ReturnToPractice: Practice menu textures are missing after init (%p/%p) [%s]",
            *(void**)(menu + 0x10), *(void**)(menu + 0x14), why);
        return 0;
    }
    *(int*)(t + 0x2324c) = 2;
    *(int*)(t + 0xc) = 0;
    *(int*)(t + 8) = 0xc;
    g_practiceMenuWatch = 1;
    Log("ReturnToPractice: opened the Practice menu [%s]", why);
    return 1;
}

/* The title scene is created in the middle of the scene switch, so the menu is not opened inside its init:
   it is opened from the title update a few frames later, when the scene runs like after a normal menu choice. */
static unsigned char __attribute__((fastcall)) TitleInitHook(void *title) {
    unsigned char r = g_origTitleInit(title);
    g_practiceMenuWatch = 0;
    g_practiceMenuOpenWait = 0;
    if (g_practiceReturn || g_exReturnPracticeMenuPending) {
        char *t = (char*)title;
        g_practiceReturn = 0;
        g_exReturnPracticeMenuPending = 0;
        if (g_origTitleUpd) {
            g_practiceMenuOpenWait = 3;
            g_practiceMenuOpenTries = 0;
            Log("ReturnToPractice: title created after a practice run, the Practice menu opens in 3 frames");
        } else if (!IsBadReadPtr(t, 0x23300)) {
            PracticeMenuOpen(t, "title init");
        }
    }
    return r;
}

static unsigned char __attribute__((fastcall)) TitleUpdateHook2(void *title) {
    unsigned char r = g_origTitleUpd(title);
    char *t = (char*)title;
    if (g_practiceMenuOpenWait > 0) {
        if (!IsBadReadPtr(t, 0x23300) && --g_practiceMenuOpenWait == 0) {
            if (!PracticeMenuOpen(t, "title update")) {
                if (++g_practiceMenuOpenTries < 60) {
                    g_practiceMenuOpenWait = 2;
                } else {
                    Log("ReturnToPractice: giving up, the Practice menu could not be created");
                }
            }
        }
    } else if (g_practiceMenuWatch > 0 && !IsBadReadPtr(t, 0x23300)) {
        if (*(int*)(t + 8) != 0xc) {
            g_practiceMenuWatch = 0;
        } else if (++g_practiceMenuWatch >= 90) {
            /* The menu's fade counter rises by 4 per update up to 0x80: after 90 frames in the menu a low value
               (or a missing texture) means it is not running, i.e. it would stay blank. */
            int fade = *(int*)(t + 0x22e90 + 0xc);
            char *menu = t + 0x22e90;
            g_practiceMenuWatch = 0;
            if (fade < 0x20 || !*(void**)(menu + 0x10) || !*(void**)(menu + 0x14)) {
                Log("ReturnToPractice: Practice menu looks blank (fade=%d), opening it again", fade);
                if (!PracticeMenuOpen(t, "watchdog")) g_practiceMenuOpenWait = 2;
            }
        }
    }
    return r;
}

static void PatchReturnToPractice(void) {
    void **slot = (void**)(uintptr_t)0x453f38;
    DWORD old;

    if (!g_ReturnToPractice) return;
    if (*slot != (void*)(uintptr_t)0x4227b0) {
        Log("ReturnToPractice: title vtable slot is %p, skipped", *slot);
        return;
    }
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Log("ReturnToPractice: VirtualProtect failed (err=%08X)", GetLastError());
        return;
    }
    g_origTitleInit = (PF_TitleInit)*slot;
    *slot = (void*)TitleInitHook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("ReturnToPractice: title init hooked");
    {
        void **uslot = (void**)(uintptr_t)0x453f40;
        DWORD uold;
        if (*uslot != (void*)(uintptr_t)0x422fa0) {
            Log("ReturnToPractice: title update slot is %p, not hooked", *uslot);
        } else if (VirtualProtect(uslot, sizeof(void*), PAGE_READWRITE, &uold)) {
            g_origTitleUpd = (PF_TitleUpd)*uslot;
            *uslot = (void*)TitleUpdateHook2;
            VirtualProtect(uslot, sizeof(void*), uold, &uold);
            Log("ReturnToPractice: title update hooked");
        }
    }
}

static void ExReturnToPracticeMenu(const char *reason) {
    Log("ExArea: %s finished; returning to the Practice menu", reason);
    g_exBoss1BgmPending = 0;
    g_exRun = 0;
    g_exBossRun = 0;
    g_practiceBossActive = 0;
    g_practiceBossStage = 0;
    g_practiceBossTarget = 0;
    g_practiceBossSeekingSecondForm = 0;
    g_practiceBossFormBgmChanged = 0;
    g_practiceBossLiveTransition = 0;
    g_practiceBossLiveKillAtFrame = 0;
    g_practiceBossLiveDefeated = 0;
    g_practiceBossLiveMissing = 0;
    g_practiceBossLiveFirstObject = NULL;
    g_practiceBgmLastId = -1;
    g_exStartReq = 0;
    g_exPending = 0;
    g_exSkipWait = 0;
    g_exBoss2Kill = 0;
    g_exBoss2Transition = 0;
    g_exBoss2ObjectSeen = 0;
    g_exBossReturnAtFrame = 0;
    ExBgmMuteSync();
    g_exInputSuppress = 1;
    g_exReturnPracticeMenuPending = 1;
    ((void (__cdecl *)(int, int))(uintptr_t)0x4254d0)(1, 0);
}

static unsigned char __cdecl ExStageClearOp(void *thr, void *args) {
    int transitionClear = g_exBossRun == 2 && !g_exBoss2ObjectSeen;
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
        if (!g_exBossReturnAtFrame) {
            g_exBossReturnAtFrame = *(volatile int*)(uintptr_t)0x4dadf8 + 180;
            Log("ExArea: Boss 1 defeated; returning to Practice in 180 frames (at %d)",
                g_exBossReturnAtFrame);
        }
        return 1;
    }
    if (g_exRun && g_exBossRun == 2 && g_exBoss2BgmSeen) {
        int at = *(volatile int*)(uintptr_t)0x4dadf8 + 180;
        if (!g_exBossReturnAtFrame || at < g_exBossReturnAtFrame) {
            g_exBossReturnAtFrame = at;
            Log("ExArea: Boss 2 defeated; returning to Practice in 180 frames (at %d)",
                g_exBossReturnAtFrame);
        }
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
            g_exBoss2ObjectSeen = 1;
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
        g_exBoss2ObjectSeen = 1;
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

static void PracticeBossTrackStage3BgmEvent(int frame) {
    unsigned *n = *(unsigned**)(uintptr_t)0x4ea1d8;
    if (!g_practiceBossActive || g_practiceBossStage != 3) return;
    for (; n; n = (unsigned*)n[0]) {
        unsigned *args;
        int id;
        if ((int)n[1] != frame || (int)n[2] != 6 || !n[3]) continue;
        args = (unsigned*)n[3];
        id = (int)args[3];
        if (g_practiceBossSeekingSecondForm && g_practiceBgmLastId >= 0 &&
            id != g_practiceBgmLastId) {
            g_practiceBossFormBgmChanged = 1;
            Log("PracticeBoss: Stage 3 scheduled BGM changed %d -> %d at frame %d",
                g_practiceBgmLastId, id, frame);
        } else if (!g_practiceBossSeekingSecondForm && id != g_practiceBgmLastId) {
            Log("PracticeBoss: Stage 3 scheduled BGM id=%d at frame %d", id, frame);
        }
        g_practiceBgmLastId = id;
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
    if (g_pauseLoadArmed && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
        g_pauseAfterLoad = 1;
    if (g_exSimulatingStageFrame) {
        PracticeBossTrackStage3BgmEvent(frame);
        return ((PF_EventDispatch)(uintptr_t)0x430020)(frame);
    }
    if (g_practiceBgmSeekPending) {
        if (g_practiceBgmLoaded && frame >= g_practiceBgmSeekStartFrame &&
            PracticeSeekBgm(g_practiceBgmSeekSeconds)) {
            g_practiceBgmSeekPending = 0;
            g_practiceBgmFadeInVolume = g_exBgmSavedVol;
            g_practiceBgmFadeInFrames = 8;
            ExBgmMuteSync();
        } else if (frame >= g_practiceBgmSeekDeadline) {
            g_practiceBgmSeekPending = 0;
            Log("Practice/BGM seek to %d second(s) timed out (frame %d, music loaded=%d)",
                g_practiceBgmSeekSeconds, frame, g_practiceBgmLoaded);
            ExBgmMuteSync();
        }
    }
    if (g_practiceBgmFadeInFrames > 0 && !g_practiceBgmSeekPending && !g_exMuteSe) {
        int step = 9 - g_practiceBgmFadeInFrames;
        int volume = (g_practiceBgmFadeInVolume * step) / 8;
        ((PF_SetVolume)(uintptr_t)0x4117c0)(volume);
        if (--g_practiceBgmFadeInFrames == 0)
            Log("ExArea: BGM fade-in complete at volume %d", g_practiceBgmFadeInVolume);
    }
    if (g_pauseAfterLoad && *(volatile int*)(uintptr_t)0x4dade4 >= 1 &&
        *(volatile int*)(uintptr_t)0x4dade4 <= 4) {
        void *input = *(void**)(uintptr_t)0x46bbec;
        void *pause = *(void**)(uintptr_t)0x4dae08;
        if (input && pause) {
            g_pauseAfterLoad = 0;
            g_pauseLoadArmed = 0;
            ((void (__attribute__((thiscall)) *)(void*))(uintptr_t)0x4191f0)(pause);
            *(volatile unsigned char*)(uintptr_t)0x4dae16 = 1;
            ((void (__attribute__((thiscall)) *)(void*))(uintptr_t)0x431850)(input);
            Log("Pause: opening pause menu after stage load because Escape was held");
        }
    } else if (g_pauseLoadArmed && !g_exSimulatingStageFrame) {
        g_pauseLoadArmed = 0;
    }
    if (g_practiceBossLiveTransition && g_practiceBossActive &&
        g_practiceBossStage == 3 && g_practiceBossTarget == 2) {
        unsigned *boss;
        PracticeBossTrackStage3BgmEvent(frame);
        boss = *(unsigned**)(uintptr_t)0x4dae04;
        if (g_practiceBossFormBgmChanged) {
            g_practiceBossLiveTransition = 0;
            g_practiceBossSeekingSecondForm = 0;
            g_practiceBossFormBgmChanged = 0;
            Log("PracticeBoss: Stage 3 Boss 2 BGM reached during live transition at frame %d", frame);
        } else if (boss && !IsBadReadPtr(boss, 0x824) &&
                   boss != g_practiceBossLiveFirstObject) {
            g_practiceBossLiveTransition = 0;
            g_practiceBossSeekingSecondForm = 0;
            Log("PracticeBoss: Stage 3 Boss 2 object appeared during live transition at frame %d (boss=%p)",
                frame, (void*)boss);
        } else if (!boss && g_practiceBossLiveDefeated) {
            g_practiceBossLiveMissing = 1;
        } else if (boss && !IsBadReadPtr(boss, 0x824) && g_practiceBossLiveMissing &&
                   *((unsigned char*)boss + 0x109) != 0 &&
                   *(float*)((char*)boss + 0x48) > 0.0f) {
            g_practiceBossLiveTransition = 0;
            g_practiceBossSeekingSecondForm = 0;
            Log("PracticeBoss: Stage 3 Boss 2 reused its object during live transition at frame %d (boss=%p hp=%.3f)",
                frame, (void*)boss, (double)*(float*)((char*)boss + 0x48));
        }
    }
    int f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9prev && *(volatile int*)(uintptr_t)0x4dade4 == 4)
        Log("ExArea: marker frame=%d", frame);
    f9prev = f9;
#ifdef SMD_DEBUG_BUILD
    {   /* JoyDbg: dump the game's joystick buffer and parsed direction flags */
        static int joyDbgN = 0;
        static int joyDbgFrame = -1;
        if ((frame % 60) == 0 && frame != joyDbgFrame && joyDbgN < 20) {
            void *input = *(void**)(uintptr_t)0x46bbec;
            joyDbgFrame = frame;
            if (input && !IsBadReadPtr(input, 0x1400)) {
                unsigned char *ib = (unsigned char *)input;
                joyDbgN++;
                Log("JoyDbg: f=%d cnt=%d ids=[%d %d %d] ok=%c caps=[min?] pos[X=%u Y=%u Z=%u R=%u U=%u V=%u btn=%08X n=%d pov=%d] "
                    "ctr[X=(%d+%d) Y=(%d+%d)] fl[108=%02X%02X%02X%02X 10c=%02X%02X%02X%02X 110=%02X]",
                    frame, *(int *)(ib + 0x1078),
                    *(int *)(ib + 0x107c), *(int *)(ib + 0x1080), *(int *)(ib + 0x1084),
                    SmdControllerIsConnected(0) ? 'Y' : 'n',
                    *(unsigned *)(ib + 0x23f), *(unsigned *)(ib + 0x243),
                    *(unsigned *)(ib + 0x247), *(unsigned *)(ib + 0x24b),
                    *(unsigned *)(ib + 0x24f), *(unsigned *)(ib + 0x253),
                    *(unsigned *)(ib + 0x257), *(unsigned *)(ib + 0x25b),
                    *(unsigned *)(ib + 0x25f),
                    *(int *)(ib + 0x3fb), *(int *)(ib + 0x3ff),
                    *(int *)(ib + 0x403), *(int *)(ib + 0x407),
                    ib[0x108], ib[0x109], ib[0x10a], ib[0x10b],
                    ib[0x10c], ib[0x10d], ib[0x10e], ib[0x10f], ib[0x110]);
                    {
                        int didx = *(int *)(ib + 0x107c);
                        int capsBase = (int)(ib + 0x3d7) + didx * 0x194;
                        Log("JoyDbgCaps: id=%d buf caps base=%p name=%.32s wXmin=%u wXmax=%u wYmin=%u wYmax=%u wCaps=%04X wNumAxes=%u wNumButtons=%u",
                            didx,
                            (void*)(ib + 0x3d7 + didx * 0x194),
                            (char*)(ib + 0x3d7 + didx * 0x194 + 4),
                            *(unsigned *)(ib + 0x3ec + didx * 0x194),
                            *(unsigned *)(ib + 0x3f0 + didx * 0x194),
                            *(unsigned *)(ib + 0x3f4 + didx * 0x194),
                            *(unsigned *)(ib + 0x3f8 + didx * 0x194),
                            *(unsigned *)(ib + 0x400 + didx * 0x194),
                            *(unsigned *)(ib + 0x3f6 + didx * 0x194),
                            *(unsigned *)(ib + 0x3f8 + didx * 0x194),
                            *(unsigned *)(ib + 0x404 + didx * 0x194),
                            *(unsigned *)(ib + 0x408 + didx * 0x194),
                            *(unsigned *)(ib + 0x40c + didx * 0x194),
                            *(unsigned *)(ib + 0x410 + didx * 0x194),
                            (unsigned)(ib[0x108]), (unsigned)(ib[0x109]), (unsigned)(ib[0x10a]), (unsigned)(ib[0x10b]));
                    }
            }
        }
    }
#endif
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
    if (g_exBossReturnAtFrame > 0 && frame >= g_exBossReturnAtFrame && !g_exSimulatingStageFrame) {
        ExReturnToPracticeMenu(g_exBossRun == 1 ? "Boss 1" : "Boss 2");
        return 1;
    }
    if (g_exRun && g_exBossRun == 1 && g_exBoss1DonePending) {
        g_exBoss1DonePending = 0;
        if (!g_exBossReturnAtFrame) {
            g_exBossReturnAtFrame = frame + 180;
            Log("ExArea: Boss 1 defeat transition reached; returning to Practice in 180 frames (at %d)",
                g_exBossReturnAtFrame);
        }
    }
    if (g_exRun && g_exBossRun == 0 && *(volatile int*)(uintptr_t)0x4dade4 == 4 &&
        g_exArea >= 0 && g_exArea < EX_AREA_N - 1 &&
        frame >= g_exAreaEndFrame[g_exArea]) {
        ExReturnToPracticeMenu("Area");
        return 1;
    }
    if (g_exBossRun >= 1 && !g_exBoss2Kill && !g_exSimulatingStageFrame && g_exBossRunLogged < 400) {
        /* A missing boss after a live boss starts the one-second return delay. In Boss 2 practice,
           ignore Boss 1's disappearance until Boss 2's battle BGM has started. */
        unsigned *b = *(unsigned**)(uintptr_t)0x4dae04;
        if (b != g_exLastBoss) {
            g_exBossRunLogged++;
            if (b && !IsBadReadPtr(b, 0x824)) {
                if (g_exBossRun == 2 && g_exBoss2BgmSeen) g_exBoss2ObjectSeen = 1;
                Log("ExDlg: Boss practice: current boss object -> %p type=%u hp=%.3f (frame %d)", (void*)b, b[1],
                    *(float*)((char*)b + 0x48), frame);
            } else if (g_exLastBoss) {
                Log("ExDlg: Boss practice: current boss object -> none (frame %d, was %p) [run=%d bgm2=%d seen=%d ret=%d]",
                    frame, (void*)g_exLastBoss, g_exBossRun, g_exBoss2BgmSeen, g_exBoss2ObjectSeen, g_exBossReturnAtFrame);
                if ((g_exBossRun == 1 || g_exBoss2BgmSeen) && !g_exBossReturnAtFrame) {
                    /* Fallback when the stage clear opcode never fires: return 3 seconds after the
                       boss object disappears (both Boss 1 and Boss 2).  ExStageClearOp can override
                       this with an earlier natural return. */
                    g_exBossReturnAtFrame = frame + 180;
                    Log("ExArea: Boss %d defeated; returning to Practice in 180 frames (at %d)",
                        g_exBossRun, g_exBossReturnAtFrame);
                }
            }
            g_exLastBoss = b;
        }
    }
    if (g_exPending && frame == 0 && *(volatile int*)(uintptr_t)0x4dade4 == 4) {
        g_exPending = 0;
        ExLogQueue();
    }
    if (g_practiceBossPending && frame == 0) {
        int stage = *(volatile int*)(uintptr_t)0x4dade4;
        int target = frame;
        g_practiceBossPending = 0;
        if (stage >= 1 && stage <= 3) {
            ExPracticeSave sv;
            ExPracticeSaveState(&sv);
            g_exMuteSe = 1;
            if (PracticeBossSeek(frame, 60000, g_practiceBossTarget, &target)) {
                *(volatile int*)(uintptr_t)0x4dadf8 = target;
            } else {
                Log("PracticeBoss: boss did not appear during the seek; returning to Practice menu");
                target = 0;
                *(volatile int*)(uintptr_t)0x4dadf8 = 0;
            }
            g_exMuteSe = 0;
            ExPracticeRestoreState(&sv);
            if (!target) {
                ExReturnToPracticeMenu("Boss seek failed");
                return 1;
            }
            if (target > 0) {
                ApplyPracticePower(stage, "boss seek");
                void *pl = *(void**)(uintptr_t)0x4e9ad4;
                if (pl) {
                    *(int*)((char*)pl + 0x8) = 0;
                    if (*(int*)((char*)pl + 0x2c) <= 0) *(int*)((char*)pl + 0x2c) = 5;
                    *(int*)((char*)pl + 0x14) = 0;
                }
                frame = target;
                if ((stage == 1 || stage == 2) && g_practiceBossTarget == 1) {
                    g_practiceBgmSeekSeconds = stage == 1 ? 38 : 48;
                    g_practiceBgmFadeInFrames = 0;
                    g_practiceBgmSeekPending = 1;
                    g_practiceBgmSeekDeadline = target + 120;
                    g_practiceBgmSeekStartFrame = target + 8;
                    Log("PracticeBoss: Stage %d MidBoss will seek its BGM to %d seconds",
                        stage, g_practiceBgmSeekSeconds);
                } else {
                    g_practiceBgmSeekPending = 0;
                    if (g_practiceBgmSimLastId >= 0) {
                        int bgmId = g_practiceBgmSimLastId;
                        int result;
                        g_practiceBgmLoaded = 1;
                        result = ((PF_VmBgm)(uintptr_t)0x433740)(bgmId);
                        Log("PracticeBoss: restarted BGM id=%d after fast-forward (result=%d)",
                            bgmId, result);
                    }
                }
                Log("PracticeBoss: stage %d fast-forwarded to boss start frame %d", stage, target);
                g_postJumpLog = 4;
            }
        }
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
        if (g_exBossRun == 1) {
            target = g_exBossDefaultFrame[0];
        } else if (g_exBossRun == 2) {
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
            /* EX area practice: use settle mode so background tiles match normal play */
            int areaBgMode = (!isBoss && g_exRun) ? 2 : g_ExBgMode;
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
            if (!isBoss && g_exBgmMoveDelta > 0) {
                int seekSec = g_exBgmMoveDelta / 60;
                if (seekSec > 0) {
                    g_practiceBgmSeekSeconds = seekSec;
                    g_practiceBgmSeekPending = 1;
                    g_practiceBgmSeekDeadline = target + 600;
                    g_practiceBgmSeekStartFrame = target + 8;
                    Log("ExArea: will seek the area BGM to %d second(s) after the jump "
                        "(event t=%d, target %d, bgmId=%d)",
                        seekSec, g_exBgmMoveTime, target, bgmId);
                }
            }
            frame = target;
            g_postJumpLog = 4;
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
    int tracePostJump = g_postJumpLog > 0;
    if (tracePostJump) {
        Log("EventDispatch: post-jump frame %d entry (sim=%d, left=%d)",
            frame, g_exSimulatingStageFrame ? 1 : 0, g_postJumpLog);
        g_postJumpLog--;
    }
    QueryPerformanceCounter(&a);
    result = EventDispatchImpl(frame);
    QueryPerformanceCounter(&b);
    LONGLONG dt = b.QuadPart - a.QuadPart;
    g_gamePerf[0].ticks += dt;
    if (dt > g_gamePerf[0].maxTicks) g_gamePerf[0].maxTicks = dt;
    g_gamePerf[0].calls++;
    if (tracePostJump) Log("EventDispatch: frame %d returned %02x (dt=%lld ticks)", frame, result, dt);
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
    if (node && *(volatile int*)(uintptr_t)0x4dade4 == 3 && g_practiceBossActive &&
        (int)node[2] != 0 && g_practiceStage3EventLog < 600) {
        unsigned *a = (unsigned*)node[3];
        g_practiceStage3EventLog++;
        Log("PracticeBoss: Stage 3 script event #%d now=%d time=%d type=%u args=%08X %08X %08X %08X %08X %08X",
            g_practiceStage3EventLog, *(volatile int*)(uintptr_t)0x4dadf8,
            time, node[2], a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0,
            a ? a[3] : 0, a ? a[4] : 0, a ? a[5] : 0);
    }
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

static int g_exVmBgmLog = 1;
static int __cdecl ExVmBgmHook(int id);
static int __cdecl ExBgmEventHook(int id);
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
    PatchExReturnPracticeTitleUpdate();
    PatchReturnToPractice();
    PatchExInputSuppression();
    n += HookCall(0x413c45, 0x430020, (void*)&EventDispatchHook);
    Log("ExArea: patched %d/3 call sites", n);
    Log("ExArea: event insert log: %d call site(s) hooked", PatchEventInsertLog());
    if (HookCall(0x4300b4, 0x427d40, (void*)&BgPushHook))
        Log("ExArea: background push log hooked");
    if (HookCall(0x42f0c3, 0x433740, (void*)&ExVmBgmHook))
        Log("ExArea: VM bgm op hooked");
    if (HookCall(0x430161, 0x433740, (void*)&ExBgmEventHook))
        Log("ExArea: queued BGM event op hooked");
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

static void BgmNotify(int frame, int id, const char *path) {
    if (id < 0 || id >= EX_BGM_IDS) return;
    if (g_practiceBossActive && g_practiceBossStage == 3) {
        if (g_practiceBossSeekingSecondForm && g_practiceBgmLastId >= 0 &&
            id != g_practiceBgmLastId) {
            g_practiceBossFormBgmChanged = 1;
            Log("PracticeBoss: Stage 3 form BGM changed %d -> %d at frame %d (sim=%d, %s)",
                g_practiceBgmLastId, id, frame, g_exSimulatingStageFrame ? 1 : 0, path);
        }
        g_practiceBgmLastId = id;
    }
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
        Log("ExArea: %s BGM id=%d frame=%d (sim=%d)", path, id, frame,
            g_exSimulatingStageFrame ? 1 : 0);
}

/* Both the script VM opcode (op6 -> 0x433740) and the queued type-6 event dispatcher
   (event at 0x430152 -> 0x433740) start the BGM.  They both need the same suppression and
   accounting, so share it through one function. */
static int BgmDispatch(int id, const char *path) {
    int frame = *(volatile int*)(uintptr_t)0x4dadf8;
    int result;
    if (g_exSimulatingStageFrame) {
        BgmNotify(frame, id, path);
        if (g_practiceBossActive && g_practiceBossStage >= 1 &&
            g_practiceBossStage <= 3 && id >= 0 && id < EX_BGM_IDS)
            g_practiceBgmSimLastId = id;
        if (g_practiceBossActive &&
            (g_practiceBossStage == 1 || g_practiceBossStage == 2) &&
            g_practiceBossTarget == 1 && !g_practiceBgmStartedDuringSeek) {
            g_practiceBgmStartedDuringSeek = 1;
            g_practiceBgmLoaded = 1;
            Log("PracticeBoss: starting Stage %d BGM during MidBoss fast-forward (id=%d, frame=%d, via %s)",
                g_practiceBossStage, id, frame, path);
            return ((PF_VmBgm)(uintptr_t)0x433740)(id);
        }
        if (g_exBossRun == 2 && id == g_exBossBgm[1]) {
            g_practiceBgmLoaded = 1;
            return ((PF_VmBgm)(uintptr_t)0x433740)(id);
        }
        return 0;
    }
    BgmNotify(frame, id, path);
    if (g_practiceBgmLoaded != 1) g_practiceBgmLoaded = 1;
    result = ((PF_VmBgm)(uintptr_t)0x433740)(id);
    if (g_exBoss1BgmPending && g_exRun && g_exBossRun == 1 &&
        id == g_exBossBgm[0]) {
        g_exBoss1BgmPending = 0;
        g_practiceBgmFadeInVolume = g_exBgmSavedVol;
        g_practiceBgmFadeInFrames = 8;
        Log("ExArea: Boss 1 BGM started; releasing the intro BGM mute");
        ExBgmMuteSync();
    }
    return result;
}

static int __cdecl ExVmBgmHook(int id) {
    return BgmDispatch(id, "VM op");
}

static int __cdecl ExBgmEventHook(int id) {
    return BgmDispatch(id, "event");
}

static const int g_PauseAudio = 1;

#define AUDIO_MAX 1024
typedef struct { IDirectSoundBuffer *buf; int paused, looping, posValid; DWORD pos, settled; } AudioBuf;
static AudioBuf g_audio[AUDIO_MAX];
static int g_audioN = 0;
static int g_audioInitialized = 0;
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

int SmdPatchVtblSlot(void **slot, void *hook, void **original) {
    return PatchVtblSlot(slot, hook, original);
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

static int PracticeSeekBgmInner(int seconds) {
    typedef void *(__cdecl *PF_GetCurrentMusic)(void);
    typedef unsigned char (__attribute__((thiscall)) *PF_RebufferMusic)(void *);
    typedef unsigned char (__attribute__((thiscall)) *PF_SourceSeek)(void *, DWORD);
    typedef long long (__attribute__((thiscall)) *PF_SourceTell)(void *);
    typedef DWORD (__attribute__((thiscall)) *PF_SourceLength)(void *);
    void *music;
    void *source;
    void *format;
    void *buffer;
    void **vtable;
    DWORD avgBytesPerSec;
    DWORD samplesPerSec;
    DWORD offset;
    DWORD targetSeconds;
    WORD blockAlign;
    DWORD status = 0;
    HRESULT playHr = E_FAIL;
    HRESULT positionHr = E_FAIL;
    IDirectSoundBuffer *ds;
    PF_RebufferMusic rebuffer = (PF_RebufferMusic)(uintptr_t)0x4133b0;
    PF_SourceSeek seekSource;
    PF_SourceTell tellSource;
    PF_SourceLength getSourceLength;
    DWORD sourceLength;
    int refilled;

    if (seconds <= 0) return 0;
    Log("Practice/BGM seek step 1: get current music (%d s)", seconds);
    music = ((PF_GetCurrentMusic)(uintptr_t)0x411880)();
    if (!music || IsBadReadPtr(music, 0x70)) return 0;
    if (*(DWORD*)((char*)music + 0x14) != 0x453548) {
        Log("Practice/BGM seek step 2: music type is 0x%08X, not 0x453548",
            *(DWORD*)((char*)music + 0x14));
        return 0;
    }
    Log("Practice/BGM seek step 3: music=%p", music);
    source = *(void**)((char*)music + 0x38);
    if (!source || IsBadReadPtr(source, sizeof(void*))) return 0;
    vtable = *(void***)source;
    if (!vtable || IsBadReadPtr(vtable + 6, sizeof(void*)) || !vtable[6] ||
        IsBadCodePtr((FARPROC)vtable[6])) {
        Log("Practice/BGM seek step 3: bad source vtable");
        return 0;
    }
    Log("Practice/BGM seek step 4: source=%p vtable=%p, calling getFormat", source, vtable);
    format = ((void *(__attribute__((thiscall)) *)(void*))vtable[6])(source);
    if (!format || IsBadReadPtr(format, 16)) {
        Log("Practice/BGM seek step 5: bad format");
        return 0;
    }
    avgBytesPerSec = *(DWORD*)((char*)format + 8);
    samplesPerSec = *(DWORD*)((char*)format + 4);
    blockAlign = *(WORD*)((char*)format + 12);
    if (avgBytesPerSec < 8000 || avgBytesPerSec > 2000000 ||
        samplesPerSec < 8000 || samplesPerSec > 192000) {
        Log("Practice/BGM seek step 6: format implausible (sr=%lu byterate=%lu)",
            (unsigned long)samplesPerSec, (unsigned long)avgBytesPerSec);
        return 0;
    }
    Log("Practice/BGM seek step 7: format ok (sr=%lu byterate=%lu align=%u)",
        (unsigned long)samplesPerSec, (unsigned long)avgBytesPerSec, (unsigned)blockAlign);
    targetSeconds = (DWORD)seconds;
    offset = (DWORD)((unsigned long long)samplesPerSec * targetSeconds);
    if (!vtable[2] || !vtable[3] || !vtable[4] ||
        IsBadCodePtr((FARPROC)vtable[2]) || IsBadCodePtr((FARPROC)vtable[3]) ||
        IsBadCodePtr((FARPROC)vtable[4])) {
        Log("Practice/BGM seek step 8: bad source vtable members");
        return 0;
    }
    getSourceLength = (PF_SourceLength)vtable[2];
    Log("Practice/BGM seek step 9: calling getSourceLength at %p", (void*)vtable[2]);
    sourceLength = getSourceLength(source);
    Log("Practice/BGM seek step 10: sourceLength=%lu bytes, target=%lu sample frames",
        (unsigned long)sourceLength, (unsigned long)offset);
    if ((unsigned long long)offset * blockAlign >= sourceLength) {
        Log("PracticeBoss: BGM seek target %lu frames exceeds source length %lu bytes",
            (unsigned long)offset, (unsigned long)sourceLength);
        return 0;
    }
    seekSource = (PF_SourceSeek)vtable[3];
    tellSource = (PF_SourceTell)vtable[4];
    Log("Practice/BGM seek step 11: calling sample-frame seek at %p with %lu frames",
        (void*)vtable[3], (unsigned long)offset);
    if (!seekSource(source, offset)) {
        Log("PracticeBoss: decoder rejected seek to sample frame %lu", (unsigned long)offset);
        return 0;
    }
    {
        long long actualFrame = tellSource(source);
        Log("Practice/BGM seek: decoder position after seek=%lld frames (requested=%lu)",
            actualFrame, (unsigned long)offset);
    }
    Log("Practice/BGM seek step 12: source seeked, ds=%p",
        (void*)(*(void**)((char*)music + 0x18)));
    buffer = *(void**)((char*)music + 0x18);
    if (!buffer || IsBadReadPtr(buffer, sizeof(void*))) return 0;
    ds = (IDirectSoundBuffer*)buffer;
    if (FAILED(IDirectSoundBuffer_GetStatus(ds, &status))) return 0;
    Log("Practice/BGM seek step 13: ds ok, hr status=%08X looping=%d",
        (unsigned)status, status & 0x4 ? 1 : 0);

    {
        int di;
        char line[400];
        int len = 0;
        for (di = 0x1c; di <= 0x6c && len < 360; di += 4)
            len += snprintf(line + len, sizeof(line) - (size_t)len, " +%02X=%08X", di,
                            (unsigned)*(DWORD*)((char*)music + di));
        Log("Practice/BGM seek step 14: music fields%s", line);
    }
    Log("Practice/BGM seek step 15: Stop");
    IDirectSoundBuffer_Stop(ds);
    Log("Practice/BGM seek step 16: stopped");
    if ((status & DSBSTATUS_BUFFERLOST) && FAILED(IDirectSoundBuffer_Restore(ds))) {
        Log("PracticeBoss: failed to restore lost DirectSound buffer before refill");
        return 0;
    }
    positionHr = IDirectSoundBuffer_SetCurrentPosition(ds, 0);
    if (FAILED(positionHr)) {
        Log("PracticeBoss: failed to reset DirectSound cursor before refill (hr=%08X)",
            (unsigned)positionHr);
        return 0;
    }
    /* Refilling the ring via the game's rebuffer(0x4133b0) crashes on some paths (Stage 1 MidBoss
       practice seek) with a null deref inside the rebuffer, and is racy with the loader/streaming
       thread.  The decoder source is already seeked; clear the stale ring to silence and restart
       playback so the refill replaces it with seeked audio.  Do not touch music+0x4c/+0x50/+0x54/+0x58
       (stream cursor state) — altering them misaligns refill (silence/delayed/wrong offset) and
       touching +0x58 crashed rebuffer.  Without rebuffer the ring/cursor alignment is imperfect, so
       playback starts near (not exactly at) the seek target; this is preferable to a crash. */
    {
        DSBCAPS caps;
        void *p1 = NULL, *p2 = NULL;
        DWORD b1 = 0, b2 = 0;
        memset(&caps, 0, sizeof(caps));
        caps.dwSize = sizeof(caps);
        if (SUCCEEDED(IDirectSoundBuffer_GetCaps(ds, &caps)) &&
            SUCCEEDED(IDirectSoundBuffer_Lock(ds, 0, caps.dwBufferBytes, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
            if (p1 && b1) memset(p1, 0, b1);
            if (p2 && b2) memset(p2, 0, b2);
            IDirectSoundBuffer_Unlock(ds, p1, b1, p2, b2);
        }
    }
    Log("Practice/BGM seek step 17: Play (rebuffer skipped, ring cleared)");
    playHr = IDirectSoundBuffer_Play(ds, 0, 0,
        (status & DSBSTATUS_LOOPING) ? DSBPLAY_LOOPING : 0);
    Log("Practice/BGM seek step 18: Play hr=%08X", (unsigned)playHr);
    Log("PracticeBoss: native BGM source seeked to %lu seconds (id=%d, frame_offset=%lu, sample_rate=%lu, byte_rate=%lu, align=%u)",
        (unsigned long)targetSeconds, g_practiceBgmLastId, (unsigned long)offset,
        (unsigned long)samplesPerSec, (unsigned long)avgBytesPerSec, (unsigned)blockAlign);
    return 1;
}

/* Logs the faulting instruction and registers of a crash, so a crash during the BGM seek can be located. */
static LONG CALLBACK SeekCrashVeh(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_IN_PAGE_ERROR) {
        CONTEXT *c = ep->ContextRecord;
        DWORD *sp = (DWORD*)(uintptr_t)c->Esp;
        Log("Crash: exception 0x%08X at eip=%08X rw=%u addr=%08X", (unsigned)code, (unsigned)c->Eip,
            ep->ExceptionRecord->NumberParameters >= 2 ? (unsigned)ep->ExceptionRecord->ExceptionInformation[0] : 0u,
            ep->ExceptionRecord->NumberParameters >= 2 ? (unsigned)ep->ExceptionRecord->ExceptionInformation[1] : 0u);
        Log("Crash: eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
            (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Esi,
            (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
        if (!IsBadReadPtr(sp, 32))
            Log("Crash: stack %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)sp[0], (unsigned)sp[1],
                (unsigned)sp[2], (unsigned)sp[3], (unsigned)sp[4], (unsigned)sp[5], (unsigned)sp[6], (unsigned)sp[7]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The streaming thread decodes and refills the music from the same source object, so the whole seek
   (format query, source seek, ring refill) runs under the music lock (0x40efe0 / 0x40eff0, music+0x1c). */
static int PracticeSeekBgm(int seconds) {
    typedef void *(__cdecl *PF_GetCurrentMusic)(void);
    typedef void (__attribute__((thiscall)) *PF_MusicLock)(void *);
    void *music = ((PF_GetCurrentMusic)(uintptr_t)0x411880)();
    static int vehInstalled = 0;
    int r;

    if (!vehInstalled) {
        vehInstalled = 1;
        AddVectoredExceptionHandler(1, SeekCrashVeh);
    }
    if (seconds <= 0 || !music || IsBadReadPtr(music, 0x70)) return 0;
    ((PF_MusicLock)(uintptr_t)0x40efe0)((char*)music + 0x1c);
    r = PracticeSeekBgmInner(seconds);
    ((PF_MusicLock)(uintptr_t)0x40eff0)((char*)music + 0x1c);
    return r;
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
    g_audioInitialized = 1;
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
void SmdBatchFlushQuads(void) {
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
    SmdBatchFlushQuads();
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
    {
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

/* In-game FPS counter (bottom-right HUD, 0x414A80 draws 0x416010 at 0x414ABF) shows integer fps.
   Show 1 decimal place using the game's digit glyphs + a dot glyph from the same atlas. */
typedef void (__cdecl *PF_DrawNumber)(int value, int font, int x, int y, int unk1, int a, int g, int b, int color);
static LARGE_INTEGER g_fpsSampleTime;
static unsigned int g_fpsSampleFrames;
static double g_fpsTenths; /* fixed at 1 decimal, e.g. 598 = 59.8 */
static int g_fpsDotLogged;

/* 0x4089A0 glyph blit: __thiscall(ecx=tex, 10 stack args), ret 0x28. Mirrors 416145:
   (dstX,dstY,srcH,srcW,srcX,srcY,srcH,srcW,0,color). */
static void __attribute__((noinline)) GameBlitGlyph(void *tex, int dstX, int dstY, int srcH, int srcW, int srcX, int srcY, int color) {
    struct { uintptr_t fn; void *tex; int a[10]; } s;
    s.fn = 0x4089a0; s.tex = tex;
    s.a[0]=dstX; s.a[1]=dstY; s.a[2]=srcH; s.a[3]=srcW; s.a[4]=srcX; s.a[5]=srcY; s.a[6]=srcH; s.a[7]=srcW; s.a[8]=0; s.a[9]=color;
    __asm__ volatile(
        "mov %[s], %%eax\n\t"
        "pushl 44(%%eax)\n\tpushl 40(%%eax)\n\tpushl 36(%%eax)\n\tpushl 32(%%eax)\n\tpushl 28(%%eax)\n\t"
        "pushl 24(%%eax)\n\tpushl 20(%%eax)\n\tpushl 16(%%eax)\n\tpushl 12(%%eax)\n\tpushl 8(%%eax)\n\t"
        "mov 4(%%eax), %%ecx\n\tcall *0(%%eax)"
        : : [s]"r"(&s) : "eax","ecx","edx","memory");
}

/* Same blit with a separate destination size: (dstX,dstY,dstW,dstH,srcX,srcY,srcW,srcH,0,alpha). */
static void __attribute__((noinline)) GameBlitGlyphScaled(void *tex, int dstX, int dstY, int dstW, int dstH,
                                                          int srcX, int srcY, int srcW, int srcH, int alpha) {
    struct { uintptr_t fn; void *tex; int a[10]; } s;
    s.fn = 0x4089a0; s.tex = tex;
    s.a[0]=dstX; s.a[1]=dstY; s.a[2]=dstW; s.a[3]=dstH; s.a[4]=srcX; s.a[5]=srcY; s.a[6]=srcW; s.a[7]=srcH; s.a[8]=0; s.a[9]=alpha;
    __asm__ volatile(
        "mov %[s], %%eax\n\t"
        "pushl 44(%%eax)\n\tpushl 40(%%eax)\n\tpushl 36(%%eax)\n\tpushl 32(%%eax)\n\tpushl 28(%%eax)\n\t"
        "pushl 24(%%eax)\n\tpushl 20(%%eax)\n\tpushl 16(%%eax)\n\tpushl 12(%%eax)\n\tpushl 8(%%eax)\n\t"
        "mov 4(%%eax), %%ecx\n\tcall *0(%%eax)"
        : : [s]"r"(&s) : "eax","ecx","edx","memory");
}

/* 0x416010 args: (value, digit count, x, y, font index, texSel, alpha, zero-pad flag, color).  x is the
   right-most digit's left edge; digit i (from the right) is at x + cw[font]*i; the 0x4089A0 last arg is alpha.
   The parameter names below keep the old order: `font` = digit count, `unk1` = font index. */
static void __cdecl FpsDisplayHook(int value, int font, int x, int y, int unk1, int a, int g, int b, int color) {
    LARGE_INTEGER now;
    int *gw = (int*)0x4db0c4, *gh = (int*)0x4db0e0, *sy = (int*)0x4db0fc, *cw = (int*)0x4db0a8, *sx = (int*)0x4db118;
    void *tex = a ? *(void**)0x4db13c : *(void**)0x4db144; /* same selection as 416010 */
    PF_DrawNumber draw = (PF_DrawNumber)(uintptr_t)0x416010;
    int fi = unk1; /* real font index */
    int count = font;

    int v, ip, tenths, digitW, cellW, cellH, baseSY, baseSX;
    int oneSX, inkX, inkY, dotSize, dotX, dotY, slot, tenthX, dotGW, dotGH, dotDW, dotDH;

    (void)value;
    if (fi < 0 || fi > 3) fi = 1;
    font = fi;
    QueryPerformanceCounter(&now);
    g_fpsSampleFrames++;
    if (g_fpsSampleTime.QuadPart && g_qpcFreq.QuadPart) {
        double dt = (double)(now.QuadPart - g_fpsSampleTime.QuadPart) / (double)g_qpcFreq.QuadPart;
        if (dt >= 0.25) {
            g_fpsTenths = ((double)g_fpsSampleFrames / dt) * 10.0;
            g_fpsSampleFrames = 0;
            g_fpsSampleTime = now;
        }
    } else {
        g_fpsSampleTime = now;
        g_fpsSampleFrames = 0;
        g_fpsTenths = 600.0;
    }
    v = (int)(g_fpsTenths + 0.5);
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    ip = v / 10;
    tenths = v % 10;

    /* Keep right edge near original: shift left by (dot slot + tenths digit). */
    digitW = cw[font] ? cw[font] : 8;
    cellW = gw[font] ? gw[font] : digitW;
    cellH = gh[font] ? gh[font] : digitW;
    baseSY = sy[font];
    baseSX = sx[font];

    /* Decimal point generated from the in-game digit atlas (pbgtk/PBG5 -> Grp.ac5 Text2/shield.tga;
       font3: sy=104, gh=16, digits at sx+(gh+4)*n, no '.' glyph exists). Sample a solid block from
       the '1' glyph stroke so color/alpha/texture match the digits, placed on the digit baseline. */
    /* Text.tga (tex 0x4db144) has a real '.' glyph: font 4's row (sy=132) reads "0123456789." with the dot in
       the 11th cell, i.e. srcX = (gh+4)*10 + sx.  Blit that whole cell, centred in the dot slot. */
    dotGW = gw[4] ? gw[4] : cellW;
    dotGH = gh[4] ? gh[4] : cellH;
    inkX = (dotGH + 4) * 10 + sx[4];
    inkY = sy[4];
    dotSize = dotGW;
    oneSX = 0;
    slot = digitW * 2 / 3 > 4 ? digitW * 2 / 3 : 4;  /* width reserved for the dot, between int part and tenths */
    /* x is the LEFT edge of the whole number (digit i of count at x + cw*i, least significant digit last).
       Keep the right edge: int part is shifted left by slot + cw, then dot slot, then the tenths digit in
       the original last-digit cell. */
    tenthX = x + digitW * (count - 1);
    /* enlarge the dot 1.5x, keep its bottom on the digit row, nudge it right */
    /* the game passes (gh, gw) as the (w, h) pair for both dst and src (see 416145), keep that order */
    dotDW = dotGH * 3 / 2;
    dotDH = dotGW * 3 / 2;
    dotX = tenthX - slot + (slot - dotDW) / 2 + 3;
    dotY = y + dotGW - dotDH;

    if (!g_fpsDotLogged) {
        g_fpsDotLogged = 1;
        Log("FpsDisplay: font=%d digitW=%d gw=%d gh=%d sy=%d sx=%d ink=%d,%d sz=%d dot=%d,%d tex=%p tex2=%p",
            font, digitW, cellW, cellH, baseSY, baseSX, inkX, inkY, dotSize, dotX, dotY, tex, *(void**)0x4db144);
    }
    draw(ip, count, x - digitW - slot, y, unk1, a, g, b, color);
    if (tex) GameBlitGlyphScaled(tex, dotX, dotY, dotDW, dotDH, inkX, inkY, dotGH, dotGW, g);
    draw(tenths, 1, tenthX, y, unk1, a, g, b, color);
}

static void PatchFpsDisplay(void) {
    int n;
    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        Log("FpsDisplay: unexpected image base, skipped");
        return;
    }
    n = HookCall(0x414abf, 0x416010, (void*)&FpsDisplayHook);
    Log("FpsDisplay: %s (1 decimal)", n ? "patched" : "skipped");
}

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
    if (!SmdIsWine() && cw) {
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
    SmdControllerPoll();
    BatchTryInstall();
    r = ((PF_FrameTimer)(uintptr_t)0x424f20)(ctx);
    /* on Wine the original paces adequately; adding sleep on top double-waits → <60fps */
    if (g_FrameWaitSleep && !SmdIsWine() && (r & 0xff) == 0) {
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
        SmdLog("ExArea: practice run ended, going to the title instead of the replay save scene");
        scene = 1;
    }
    ((PF_SceneSwitch)(uintptr_t)0x4254d0)(scene, force);
}

static void PatchExNoReplaySave(void) {
    unsigned char *p = (unsigned char*)(uintptr_t)0x401000;
    unsigned char *end = (unsigned char*)(uintptr_t)0x450000;
    int n = 0;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        SmdLog("ExNoReplaySave: unexpected image base, skipped");
        return;
    }
    for (; p + 9 <= end; p++) {
        if (p[0] == 0x6a && p[1] == 0x00 && p[2] == 0x6a && p[3] == 0x05 && p[4] == 0xe8) {
            if (HookCall((DWORD)(uintptr_t)(p + 4), 0x4254d0, (void*)&StageEndSceneHook)) {
                SmdLog("ExNoReplaySave: hooked scene 5 switch at 0x%08X", (unsigned)(uintptr_t)(p + 4));
                n++;
            }
        }
    }
    SmdLog("ExNoReplaySave: patched %d call site(s)", n);
}


BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        LogOpen();
        QueryPerformanceFrequency(&g_qpcFreq);
        Log("process attach (pid=%lu)", GetCurrentProcessId());
        LogWindowsVersion();
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
        PatchFpsDisplay();
        PatchKeepRunningInactive();
        SmdControllerInit();
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
