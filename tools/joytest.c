/*
 * joytest.exe -- standalone controller diagnostic for Rain.exe / Smd.
 *
 * Reads the pad through every API layer a Windows program can use and prints what each one reports, so it
 * is visible WHICH layer (if any) receives input from the controller:
 *
 *   winmm  : joyGetDevCapsA / joyGetPosEx, ids 0..15        (what Rain.exe itself calls, via the Smd shim)
 *   XInput : XInputGetState users 0..3
 *   DInput : every attached game controller, read twice
 *              "std" = the predefined c_dfDIJoystick2 data format (DIDFT_ANYINSTANCE)
 *              "smd" = the custom data format Smd_controller.c currently uses (fixed axis instances)
 *   RawHID : the HID devices Windows/Wine exposes (list only)
 *
 * Usage:   wine joytest.exe [seconds]        (default 20)
 * Press buttons / move sticks / the d-pad while it runs.  At the end a summary says which sources saw input.
 *
 * Build:   i686-w64-mingw32-gcc -O2 -Wall -Wno-format -s -o joytest.exe joytest.c -lwinmm -ldinput8 -ldxguid -luuid -luser32
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */
static DWORD g_t0;
#define TS ((unsigned)(GetTickCount() - g_t0))
#define MAXSRC 64

typedef struct { char name[96]; int seen; } Source;
static Source g_src[MAXSRC];
static int g_srcN;

static int SrcAdd(const char *fmt, ...) {
    va_list ap;
    int i;
    if (g_srcN >= MAXSRC) return MAXSRC - 1;
    i = g_srcN++;
    va_start(ap, fmt);
    vsnprintf(g_src[i].name, sizeof(g_src[i].name), fmt, ap);
    va_end(ap);
    g_src[i].seen = 0;
    return i;
}

/* ---------------------------------------------------------------- winmm */
typedef struct { int src; int have; JOYINFOEX last; int lastRet; } WinmmSlot;
static WinmmSlot g_wm[16];
static UINT g_wmCount;

static int WmActive(const JOYINFOEX *j, const JOYCAPSA *c) {
    long cx = ((long)c->wXmin + (long)c->wXmax) / 2, cy = ((long)c->wYmin + (long)c->wYmax) / 2;
    long dx = (long)j->dwXpos - cx, dy = (long)j->dwYpos - cy;
    long span = (long)c->wXmax - (long)c->wXmin;
    long th = span > 0 ? span / 4 : 16000;
    return j->dwButtons != 0 || dx > th || dx < -th || dy > th || dy < -th ||
           (j->dwPOV != 0xFFFF && j->dwPOV != 0xFFFFFFFFu && j->dwPOV != 0);
}

static void WinmmInit(void) {
    UINT id;
    g_wmCount = joyGetNumDevs();
    printf("[winmm] joyGetNumDevs() = %u\n", g_wmCount);
    for (id = 0; id < 16; id++) {
        JOYCAPSA c;
        MMRESULT r;
        memset(&c, 0, sizeof(c));
        r = joyGetDevCapsA(id, &c, sizeof(c));
        if (r != JOYERR_NOERROR) continue;
        printf("[winmm] id=%u \"%s\" mid=%04X pid=%04X buttons=%u axes=%u X=%u..%u Y=%u..%u caps=%X\n", id, c.szPname,
               c.wMid, c.wPid, c.wNumButtons, c.wNumAxes, c.wXmin, c.wXmax, c.wYmin, c.wYmax, c.wCaps);
        g_wm[id].src = SrcAdd("winmm id=%u \"%s\"", id, c.szPname);
        g_wm[id].have = 1;
    }
}

static void WinmmPoll(void) {
    UINT id;
    for (id = 0; id < 16; id++) {
        JOYINFOEX j;
        JOYCAPSA c;
        MMRESULT r;
        if (!g_wm[id].have) continue;
        memset(&j, 0, sizeof(j));
        j.dwSize = sizeof(j);
        j.dwFlags = JOY_RETURNALL;
        r = joyGetPosEx(id, &j);
        memset(&c, 0, sizeof(c));
        joyGetDevCapsA(id, &c, sizeof(c));
        if (r != JOYERR_NOERROR) {
            if (g_wm[id].lastRet != (int)r) printf("%7u winmm id=%u joyGetPosEx error %u\n", TS, id, r);
            g_wm[id].lastRet = (int)r;
            continue;
        }
        g_wm[id].lastRet = 0;
        if (!g_wm[id].last.dwSize || j.dwButtons != g_wm[id].last.dwButtons || j.dwPOV != g_wm[id].last.dwPOV ||
            labs((long)j.dwXpos - (long)g_wm[id].last.dwXpos) > 4000 ||
            labs((long)j.dwYpos - (long)g_wm[id].last.dwYpos) > 4000) {
            printf("%7u winmm id=%u buttons=%08X pov=%u x=%u y=%u z=%u r=%u\n", TS, id, j.dwButtons, j.dwPOV,
                   j.dwXpos, j.dwYpos, j.dwZpos, j.dwRpos);
            g_wm[id].last = j;
        }
        if (WmActive(&j, &c)) g_src[g_wm[id].src].seen = 1;
    }
}

/* ---------------------------------------------------------------- XInput */
typedef struct { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XIPad;
typedef struct { DWORD packet; XIPad pad; } XIState;
typedef DWORD (WINAPI *PF_XIGet)(DWORD, XIState*);
static PF_XIGet g_xiGet;
static int g_xiSrc[4];
static XIState g_xiLast[4];
static int g_xiHave[4];
static int g_xiWasConnected[4];

static void XInputInit(void) {
    static const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int i;
    for (i = 0; i < 3 && !g_xiGet; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (m) {
            g_xiGet = (PF_XIGet)(void*)GetProcAddress(m, "XInputGetState");
            if (g_xiGet) printf("[xinput] using %s\n", names[i]);
        }
    }
    if (!g_xiGet) printf("[xinput] no XInput DLL could be loaded\n");
    for (i = 0; i < 4; i++) g_xiSrc[i] = SrcAdd("XInput user %d", i);
}

static void XInputPoll(void) {
    int u;
    if (!g_xiGet) return;
    for (u = 0; u < 4; u++) {
        XIState s;
        DWORD r;
        memset(&s, 0, sizeof(s));
        r = g_xiGet((DWORD)u, &s);
        if (r != 0) {
            if (g_xiWasConnected[u]) printf("%7u xinput user %d disconnected\n", TS, u);
            g_xiWasConnected[u] = 0;
            g_xiHave[u] = 0;
            continue;
        }
        if (!g_xiWasConnected[u]) printf("%7u xinput user %d CONNECTED\n", TS, u);
        g_xiWasConnected[u] = 1;
        if (!g_xiHave[u] || s.packet != g_xiLast[u].packet) {
            printf("%7u xinput user %d packet=%u buttons=%04X lt=%u rt=%u L=(%d,%d) R=(%d,%d)\n", TS, u, s.packet,
                   s.pad.wButtons, s.pad.lt, s.pad.rt, s.pad.lx, s.pad.ly, s.pad.rx, s.pad.ry);
            g_xiLast[u] = s;
            g_xiHave[u] = 1;
        }
        if (s.pad.wButtons || labs(s.pad.lx) > 8000 || labs(s.pad.ly) > 8000 || s.pad.lt > 40 || s.pad.rt > 40)
            g_src[g_xiSrc[u]].seen = 1;
    }
}

/* ---------------------------------------------------------------- DirectInput */
typedef struct {
    char name[80];
    char inst[80];
    DWORD type;
    GUID guid;
    LPDIRECTINPUTDEVICE8A dev[2];       /* 0 = std format, 1 = smd format */
    int src[2];
    DIJOYSTATE2 last[2];
    int have[2];
    LONG min[2], max[2];
} DiDev;
static DiDev g_di[16];
static int g_diN;
static LPDIRECTINPUT8A g_dinput;
static HWND g_hwnd;

/* the custom format Smd_controller.c uses today (axes with fixed instance numbers) */
static DIOBJECTDATAFORMAT g_smdObj[140];
static DIDATAFORMAT g_smdFmt;
static const DWORD g_smdAxisInst[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };

static void SmdFormatInit(void) {
    static const GUID *const ag[8] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis,
                                       &GUID_Slider, &GUID_Slider };
    static const DWORD ao[8] = { DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0),
                                 DIJOFS_SLIDER(1) };
    unsigned i;
    for (i = 0; i < 8; i++) {
        g_smdObj[i].pguid = (GUID*)ag[i];
        g_smdObj[i].dwOfs = ao[i];
        g_smdObj[i].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(g_smdAxisInst[i]) | DIDFT_OPTIONAL;
    }
    for (i = 0; i < 4; i++) {
        g_smdObj[8 + i].pguid = (GUID*)&GUID_POV;
        g_smdObj[8 + i].dwOfs = DIJOFS_POV(i);
        g_smdObj[8 + i].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
    }
    for (i = 0; i < 128; i++) {
        g_smdObj[12 + i].pguid = (GUID*)&GUID_Button;
        g_smdObj[12 + i].dwOfs = DIJOFS_BUTTON(i);
        g_smdObj[12 + i].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
    }
    memset(&g_smdFmt, 0, sizeof(g_smdFmt));
    g_smdFmt.dwSize = sizeof(g_smdFmt);
    g_smdFmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    g_smdFmt.dwFlags = DIDF_ABSAXIS;
    g_smdFmt.dwDataSize = sizeof(DIJOYSTATE2);
    g_smdFmt.dwNumObjs = 140;
    g_smdFmt.rgodf = g_smdObj;
}

static BOOL CALLBACK DiObjCb(const DIDEVICEOBJECTINSTANCEA *o, void *ctx) {
    DWORD t = o->dwType;
    const char *kind = (t & DIDFT_AXIS) ? "axis" : (t & DIDFT_BUTTON) ? "button" : (t & DIDFT_POV) ? "pov" : "other";
    (void)ctx;
    printf("      %-6s instance=%-3u offs=%-3u \"%s\"\n", kind, (unsigned)DIDFT_GETINSTANCE(t), (unsigned)o->dwOfs,
           o->tszName);
    return DIENUM_CONTINUE;
}

static BOOL CALLBACK DiEnumCb(const DIDEVICEINSTANCEA *inst, void *ctx) {
    DiDev *d;
    int k;
    DIPROPDWORD vp;
    (void)ctx;
    if (g_diN >= 16) return DIENUM_STOP;
    d = &g_di[g_diN];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", inst->tszProductName);
    snprintf(d->inst, sizeof(d->inst), "%s", inst->tszInstanceName);
    d->type = inst->dwDevType;
    d->guid = inst->guidInstance;
    printf("[dinput] #%d \"%s\" instance=\"%s\" devtype=%08X\n", g_diN, d->name, d->inst, (unsigned)d->type);
    for (k = 0; k < 2; k++) {
        HRESULT hr = IDirectInput8_CreateDevice(g_dinput, &inst->guidInstance, &d->dev[k], NULL);
        if (FAILED(hr) || !d->dev[k]) {
            printf("      CreateDevice(%s) failed hr=%08X\n", k ? "smd" : "std", (unsigned)hr);
            d->dev[k] = NULL;
            continue;
        }
        if (k == 0) {
            memset(&vp, 0, sizeof(vp));
            vp.diph.dwSize = sizeof(vp);
            vp.diph.dwHeaderSize = sizeof(vp.diph);
            if (SUCCEEDED(IDirectInputDevice8_GetProperty(d->dev[0], DIPROP_VIDPID, &vp.diph)))
                printf("      VID=%04X PID=%04X\n", (unsigned)LOWORD(vp.dwData), (unsigned)HIWORD(vp.dwData));
            IDirectInputDevice8_EnumObjects(d->dev[0], DiObjCb, NULL, DIDFT_ALL);
        }
        hr = IDirectInputDevice8_SetDataFormat(d->dev[k], k ? &g_smdFmt : &c_dfDIJoystick2);
        if (FAILED(hr)) printf("      SetDataFormat(%s) failed hr=%08X\n", k ? "smd" : "std", (unsigned)hr);
        hr = IDirectInputDevice8_SetCooperativeLevel(d->dev[k], g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(hr)) printf("      SetCooperativeLevel(%s) failed hr=%08X\n", k ? "smd" : "std", (unsigned)hr);
        {
            DIPROPRANGE r;
            memset(&r, 0, sizeof(r));
            r.diph.dwSize = sizeof(r);
            r.diph.dwHeaderSize = sizeof(r.diph);
            r.diph.dwHow = DIPH_DEVICE;
            r.lMin = -1000;
            r.lMax = 1000;
            hr = IDirectInputDevice8_SetProperty(d->dev[k], DIPROP_RANGE, &r.diph);
            d->min[k] = -1000;
            d->max[k] = 1000;
            if (FAILED(hr)) {
                /* fall back to whatever range the device reports for X */
                DIPROPRANGE g;
                memset(&g, 0, sizeof(g));
                g.diph.dwSize = sizeof(g);
                g.diph.dwHeaderSize = sizeof(g.diph);
                g.diph.dwHow = DIPH_BYOFFSET;
                g.diph.dwObj = DIJOFS_X;
                if (SUCCEEDED(IDirectInputDevice8_GetProperty(d->dev[k], DIPROP_RANGE, &g.diph))) {
                    d->min[k] = g.lMin;
                    d->max[k] = g.lMax;
                }
            }
        }
        IDirectInputDevice8_Acquire(d->dev[k]);
        d->src[k] = SrcAdd("DirectInput #%d \"%s\" [%s format]", g_diN, d->name, k ? "smd" : "std");
    }
    g_diN++;
    return DIENUM_CONTINUE;
}

static void DirectInputInit(void) {
    HRESULT hr;
    SmdFormatInit();
    hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void**)&g_dinput, NULL);
    if (FAILED(hr)) {
        printf("[dinput] DirectInput8Create failed hr=%08X\n", (unsigned)hr);
        return;
    }
    hr = IDirectInput8_EnumDevices(g_dinput, DI8DEVCLASS_GAMECTRL, DiEnumCb, NULL, DIEDFL_ATTACHEDONLY);
    printf("[dinput] EnumDevices hr=%08X, %d game controller(s)\n", (unsigned)hr, g_diN);
}

static int DiActive(const DiDev *d, int k, const DIJOYSTATE2 *s) {
    LONG span = d->max[k] - d->min[k], c = d->min[k] + span / 2, th = span / 4;
    int i;
    for (i = 0; i < 32; i++)
        if (s->rgbButtons[i] & 0x80) return 1;
    if (s->rgdwPOV[0] != 0xFFFFFFFFu && LOWORD(s->rgdwPOV[0]) != 0xFFFF) return 1;
    if (labs(s->lX - c) > th || labs(s->lY - c) > th || labs(s->lZ - c) > th || labs(s->lRz - c) > th) return 1;
    return 0;
}

static void DirectInputPoll(void) {
    int i, k;
    for (i = 0; i < g_diN; i++) {
        for (k = 0; k < 2; k++) {
            DiDev *d = &g_di[i];
            DIJOYSTATE2 s;
            HRESULT hr;
            unsigned b = 0;
            int j, changed;
            if (!d->dev[k]) continue;
            hr = IDirectInputDevice8_Poll(d->dev[k]);
            if (FAILED(hr)) {
                IDirectInputDevice8_Acquire(d->dev[k]);
                hr = IDirectInputDevice8_Poll(d->dev[k]);
            }
            memset(&s, 0, sizeof(s));
            hr = IDirectInputDevice8_GetDeviceState(d->dev[k], sizeof(s), &s);
            if (FAILED(hr)) continue;
            for (j = 0; j < 32; j++)
                if (s.rgbButtons[j] & 0x80) b |= 1u << j;
            changed = !d->have[k] || memcmp(s.rgbButtons, d->last[k].rgbButtons, 32) != 0 ||
                      s.rgdwPOV[0] != d->last[k].rgdwPOV[0] || labs(s.lX - d->last[k].lX) > 200 ||
                      labs(s.lY - d->last[k].lY) > 200 || labs(s.lZ - d->last[k].lZ) > 200 ||
                      labs(s.lRz - d->last[k].lRz) > 200;
            if (changed) {
                printf("%7u dinput #%d [%s] buttons=%08X pov=%08X x=%ld y=%ld z=%ld rx=%ld ry=%ld rz=%ld\n", TS, i,
                       k ? "smd" : "std", b, (unsigned)s.rgdwPOV[0], (long)s.lX, (long)s.lY, (long)s.lZ, (long)s.lRx,
                       (long)s.lRy, (long)s.lRz);
                d->last[k] = s;
                d->have[k] = 1;
            }
            if (DiActive(d, k, &s)) g_src[d->src[k]].seen = 1;
        }
    }
}

/* ---------------------------------------------------------------- raw HID list */
static void RawList(void) {
    UINT n = 0, i;
    RAWINPUTDEVICELIST *list;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) {
        printf("[rawinput] no devices (or GetRawInputDeviceList unsupported)\n");
        return;
    }
    list = (RAWINPUTDEVICELIST*)calloc(n, sizeof(*list));
    if (!list || GetRawInputDeviceList(list, &n, sizeof(*list)) == (UINT)-1) {
        printf("[rawinput] GetRawInputDeviceList failed\n");
        free(list);
        return;
    }
    printf("[rawinput] %u device(s):\n", n);
    for (i = 0; i < n; i++) {
        char name[260];
        UINT sz = sizeof(name);
        RID_DEVICE_INFO info;
        UINT isz = sizeof(info);
        name[0] = 0;
        GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &sz);
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &isz);
        if (list[i].dwType == RIM_TYPEHID)
            printf("   HID  vid=%04X pid=%04X usagePage=%04X usage=%04X  %.100s\n", (unsigned)info.hid.dwVendorId,
                   (unsigned)info.hid.dwProductId, (unsigned)info.hid.usUsagePage, (unsigned)info.hid.usUsage, name);
        else
            printf("   %s  %.100s\n", list[i].dwType == RIM_TYPEKEYBOARD ? "KBD " : "MOUSE", name);
    }
    free(list);
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int i;
    if (seconds < 3) seconds = 3;
    g_t0 = GetTickCount();
    printf("joytest: reading the controller through winmm / XInput / DirectInput for %d s\n", seconds);
    printf("         press buttons, move both sticks and the d-pad now\n\n");
    g_hwnd = CreateWindowExA(0, "STATIC", "joytest", WS_POPUP, 0, 0, 1, 1, NULL, NULL, GetModuleHandleA(NULL), NULL);
    WinmmInit();
    XInputInit();
    DirectInputInit();
    RawList();
    printf("\n--- live (only changes are printed) ---\n");
    g_t0 = GetTickCount();
    while ((int)(GetTickCount() - g_t0) < seconds * 1000) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        WinmmPoll();
        XInputPoll();
        DirectInputPoll();
        Sleep(10);
    }
    printf("\n--- summary: which sources saw input ---\n");
    for (i = 0; i < g_srcN; i++) {
        /* XInput users that never existed are not interesting */
        if (strncmp(g_src[i].name, "XInput user", 11) == 0 && !g_src[i].seen && !g_xiWasConnected[g_src[i].name[12] - '0'])
            continue;
        printf("  %-60s %s\n", g_src[i].name, g_src[i].seen ? "INPUT SEEN" : "no input (neutral all the time)");
    }
    return 0;
}
