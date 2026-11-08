#define CINTERFACE
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <dinput.h>
#include <mmreg.h>
#include <dsound.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "Smd_internal.h"

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
typedef MMRESULT (WINAPI *PF_JoyGetPos)(UINT id, LPJOYINFO info);
typedef UINT (WINAPI *PF_JoyGetNumDevs)(void);
typedef HRESULT (WINAPI *PF_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

#define JOY_CAPS_BRIDGED (JOYCAPS_HASZ | JOYCAPS_HASR | JOYCAPS_HASU | JOYCAPS_HASV | \
                          JOYCAPS_HASPOV | JOYCAPS_POV4DIR | JOYCAPS_POVCTS)

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
static PF_XInputGetState g_xiGetStateCandidates[3];
static unsigned g_xiCandidateCount;
static unsigned g_xiPreferredCandidate;
static unsigned char g_xiPhantom[4];        /* XInput slot proven inert (Wine bug: connected + permanently zeroed) */
static unsigned char g_xiPhantomChecked[4];
static PF_JoyGetCapsA g_winmmJoyGetCapsA = NULL;
static PF_JoyGetPosEx g_winmmJoyGetPosEx = NULL;
static PF_JoyGetPos g_winmmJoyGetPos = NULL;
static PF_JoyGetNumDevs g_winmmJoyGetNumDevs = NULL;
static int g_joyImportsPatched = 0;
static unsigned char g_joyPlugged[4];
static HMODULE g_dinputModule;
static LPDIRECTINPUT8A g_di8;
static LPDIRECTINPUTDEVICE8A g_diDevices[16];
static LONG g_diAxisMin[16][8];
static LONG g_diAxisMax[16][8];
static const DWORD g_diAxisInstance[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
static unsigned g_diCount;
static DWORD g_diLastScan;
static DIDATAFORMAT g_diFormat;
static DIOBJECTDATAFORMAT g_diObjects[140];
static int g_diFormatReady;
static unsigned g_diScans;      /* DirectInput enumerations so far (details are logged for the first ones) */
static unsigned g_diEnumSeen;   /* devices seen by the current enumeration */
static JOYCAPSA g_joyCapsId;     /* shared XInput caps instance (memcpy'd into game buffers) */

/* Raw hidraw fallback (Wine/Proton on Linux): Wine's winmm and dinput backends can
   misparse some pads (confirmed broken for Switch Pro Controller 057E:2009 on
   hid-nintendo — a counting field lands in buttons/axes and the hat sticks at UP).
   Reading the Linux hidraw report directly via Z:\dev\hidraw* bypasses all of that.
   On real Windows, Z:\dev\hidraw* does not exist, so this never activates and the
   normal XInput/DirectInput/winmm paths are used unchanged. */
#define HIDRAW_MAX 16
#define HIDRAW_REP 64
static HANDLE g_hidHandle = INVALID_HANDLE_VALUE;
static OVERLAPPED g_hidOv;
static HANDLE g_hidEv;
static unsigned char g_hidBuf[HIDRAW_REP];
static JOYINFOEX g_hidState;
static int g_hidReady;
static int g_hidPending;
static unsigned short g_hidVid, g_hidPid;
static DWORD g_hidLastScan;
static int g_hidScanned;

static void JoyHidrawClose(void) {
    if (g_hidEv) { CloseHandle(g_hidEv); g_hidEv = NULL; }
    if (g_hidHandle != INVALID_HANDLE_VALUE) { CloseHandle(g_hidHandle); g_hidHandle = INVALID_HANDLE_VALUE; }
    g_hidPending = g_hidReady = 0;
}

static int JoyHidrawMatch(unsigned short vid, unsigned short pid) {
    /* only pads whose Wine winebus parsing is known-broken need the raw path;
       everything else keeps using the normal (and on Windows, only) API paths */
    return vid == 0x057Eu && pid == 0x2009u;   /* Nintendo Switch Pro Controller (USB) */
}

static void JoyHidParseSwitchPro(const unsigned char *r, DWORD n);

static int JoyHidrawOpen(int idx) {
    char dev[64], uev[80], buf[512];
    HANDLE h;
    DWORD rd = 0, got = 0;
    unsigned vid = 0, pid = 0, bvid = 0, bpid = 0;
    unsigned char *p, *end;

    sprintf(dev, "Z:\\dev\\hidraw%d", idx);
    sprintf(uev, "Z:\\sys\\class\\hidraw\\hidraw%d\\device\\uevent", idx);
    h = CreateFileA(uev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        memset(buf, 0, sizeof(buf));
        ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
        CloseHandle(h);
    }
    if (rd) {
        /* HID_ID=BBBB:VVVVVVVV:PPPPPPPP */
        end = (unsigned char*)buf + rd;
        for (p = (unsigned char*)buf; p < end; p += strlen((char*)p) + 1) {
            if (strncmp((char*)p, "HID_ID=", 7) == 0)
                sscanf((char*)p + 7, "%x:%x:%x", &bvid, &vid, &pid);
            if (p + strlen((char*)p) + 1 >= end) break;
        }
    }
    /* sysfs unreadable? fall back to matching the known-broken pad by trying to read:
       open the hidraw node and peek a report — if it looks like Switch Pro 0x30, accept */
    if (!vid && !pid) {
        HANDLE hp = CreateFileA(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, 0, NULL);
        if (hp != INVALID_HANDLE_VALUE) {
            unsigned char probe[HIDRAW_REP];
            DWORD prd = 0;
            memset(probe, 0, sizeof(probe));
            if (ReadFile(hp, probe, sizeof(probe), &prd, NULL) && prd >= 12 && probe[0] == 0x30) {
                vid = 0x057E; pid = 0x2009; /* Switch Pro USB full report signature */
                SmdLog("JoyBridge: hidraw%d identified as 057E:2009 by report signature", idx);
            }
            CloseHandle(hp);
        }
    }
    if (!JoyHidrawMatch((unsigned short)vid, (unsigned short)pid)) return 0;

    h = CreateFileA(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SmdLog("JoyBridge: hidraw%d %04X:%04X matched but CreateFile(overlapped) failed err=%u", idx, vid, pid, (unsigned)GetLastError());
        return 0;
    }
    g_hidEv = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hidEv) { CloseHandle(h); return 0; }
    g_hidHandle = h;
    g_hidVid = (unsigned short)vid;
    g_hidPid = (unsigned short)pid;
    memset(&g_hidOv, 0, sizeof(g_hidOv));
    g_hidOv.hEvent = g_hidEv;
    memset(&g_hidState, 0, sizeof(g_hidState));
    g_hidState.dwXpos = g_hidState.dwYpos = g_hidState.dwZpos = g_hidState.dwRpos = 32767;
    g_hidState.dwPOV = 0xFFFF;
    g_hidReady = 1; /* advertise as ready immediately so broken backends don't inject phantoms */
    g_hidPending = 0;
    if (ReadFile(g_hidHandle, g_hidBuf, sizeof(g_hidBuf), &got, &g_hidOv)) {
        g_hidPending = 0;
        if (got) JoyHidParseSwitchPro(g_hidBuf, got);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        g_hidPending = 1;
    }
    SmdLog("JoyBridge: raw hidraw%d opened for %04X:%04X (Linux hidraw fallback)", idx, vid, pid);
    return 1;
}

static void JoyHidrawScan(void) {
    DWORD now = GetTickCount();
    int i, foundDev = 0;
    if (g_hidHandle != INVALID_HANDLE_VALUE) return;
    if (g_hidScanned && now - g_hidLastScan < 3000) return;
    g_hidLastScan = now;
    g_hidScanned = 1;
    for (i = 0; i < HIDRAW_MAX; i++) {
        char dev[64];
        sprintf(dev, "Z:\\dev\\hidraw%d", i);
        if (GetFileAttributesA(dev) != INVALID_FILE_ATTRIBUTES) foundDev = 1;
        if (JoyHidrawOpen(i)) return;
    }
#ifdef SMD_DEBUG_BUILD
    {
        static int logged;
        if (!logged) {
            logged = 1;
            SmdLog("JoyBridge: hidraw scan: no matching raw device (hidraw nodes %s, pad 057E:2009)",
                foundDev ? "seen" : "not seen under Z:\\dev");
        }
    }
#else
    (void)foundDev;
#endif
}

static unsigned short JoyHidStick12(unsigned lo, unsigned hiNibble) {
    unsigned v = (unsigned)lo | ((unsigned)(hiNibble & 0x0Fu) << 8); /* 0..4095 */
    return (unsigned short)((v * 65535u) / 4095u);
}
static unsigned short JoyHidStick12hi(unsigned mid, unsigned hi) {
    unsigned v = ((unsigned)(mid & 0xF0u) >> 4) | ((unsigned)hi << 4);
    return (unsigned short)((v * 65535u) / 4095u);
}

static void JoyHidParseSwitchPro(const unsigned char *r, DWORD n) {
    unsigned b3, b4, b5, dw = 0;
    if (n < 12 || r[0] != 0x30) return;
    b3 = r[3]; b4 = r[4]; b5 = r[5];
    /* byte 3: Y,X,B,A, RSR,RSL,R,ZR */
    if (b3 & 0x08) dw |= JOY_BUTTON1;  /* A */
    if (b3 & 0x04) dw |= JOY_BUTTON2;  /* B */
    if (b3 & 0x02) dw |= JOY_BUTTON3;  /* X */
    if (b3 & 0x01) dw |= JOY_BUTTON4;  /* Y */
    if (b3 & 0x40) dw |= JOY_BUTTON8;  /* R */
    if (b3 & 0x80) dw |= JOY_BUTTON16; /* ZR */
    /* byte 4: Minus,Plus,RStick,LStick,Home,Capture */
    if (b4 & 0x02) dw |= JOY_BUTTON5;  /* Plus / Start */
    if (b4 & 0x01) dw |= JOY_BUTTON6;  /* Minus / Back */
    if (b4 & 0x08) dw |= JOY_BUTTON9;  /* LStick */
    if (b4 & 0x04) dw |= JOY_BUTTON10; /* RStick */
    /* byte 5: Down,Up,Right,Left, SR,SL,L,ZL */
    if (b5 & 0x40) dw |= JOY_BUTTON7;  /* L */
    if (b5 & 0x80) dw |= JOY_BUTTON15; /* ZL */
    /* d-pad: also emit POV (projected to axes later) */
    if (b5 & 0x02)      g_hidState.dwPOV = 0;      /* Up */
    else if (b5 & 0x04) g_hidState.dwPOV = 9000;   /* Right */
    else if (b5 & 0x01) g_hidState.dwPOV = 18000;  /* Down */
    else if (b5 & 0x08) g_hidState.dwPOV = 27000;  /* Left */
    else                g_hidState.dwPOV = 0xFFFF;
    /* diagonals */
    if ((b5 & 0x0A) == 0x0A) g_hidState.dwPOV = 4500;   /* Up+Right */
    if ((b5 & 0x06) == 0x06) g_hidState.dwPOV = 13500;  /* Right+Down */
    if ((b5 & 0x05) == 0x05) g_hidState.dwPOV = 22500;  /* Down+Left */
    if ((b5 & 0x09) == 0x09) g_hidState.dwPOV = 31500;  /* Left+Up */

    g_hidState.dwButtons = dw;
    /* left stick: bytes 6,7,8 */
    g_hidState.dwXpos = JoyHidStick12(r[6], r[7]);
    g_hidState.dwYpos = 65535u - JoyHidStick12hi(r[7], r[8]); /* up = large in raw? invert for Windows (down = large) */
    /* right stick: bytes 9,10,11 */
    if (n >= 12) {
        g_hidState.dwZpos = JoyHidStick12(r[9], r[10]);
        g_hidState.dwRpos = 65535u - JoyHidStick12hi(r[10], r[11]);
    }
    g_hidReady = 1;
}

static void JoyHidrawPoll(void) {
    DWORD got = 0;
    int drain = 0;
    JoyHidrawScan();
    if (g_hidHandle == INVALID_HANDLE_VALUE) return;
    /* drain all available reports and keep only the latest (low latency) */
    for (;;) {
        if (g_hidPending) {
            if (WaitForSingleObject(g_hidEv, 0) != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(g_hidHandle, &g_hidOv, &got, FALSE)) { JoyHidrawClose(); return; }
            g_hidPending = 0;
            if (g_hidVid == 0x057Eu && g_hidPid == 0x2009u) JoyHidParseSwitchPro(g_hidBuf, got);
        }
        ResetEvent(g_hidEv);
        memset(&g_hidOv, 0, sizeof(g_hidOv));
        g_hidOv.hEvent = g_hidEv;
        if (ReadFile(g_hidHandle, g_hidBuf, sizeof(g_hidBuf), &got, &g_hidOv)) {
            if (g_hidVid == 0x057Eu && g_hidPid == 0x2009u) JoyHidParseSwitchPro(g_hidBuf, got);
            if (++drain >= 16) break; /* keep queue bounded */
            continue;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            g_hidPending = 1;
            break;
        } else {
            JoyHidrawClose();
            return;
        }
    }
}

static int JoyHidrawState(JOYINFOEX *out) {
    JOYINFOEX ji0;
    if (!g_hidReady) return 0;
    ji0 = g_hidState;
    ji0.dwSize = sizeof(ji0);
    ji0.dwFlags = JOY_RETURNALL;
    /* let JoyNativeToJi handle POV->axes projection and button-count; copy back raw here */
    *out = ji0;
    return 1;
}

static void JoyLogCapsConnected(unsigned id, unsigned connected)
{
    SmdLog("JoyBridge: joyGetDevCapsA id=%u xinput-connected=%u", id, connected);
}

/* DirectInput's SetCooperativeLevel needs a window of this process: a foreground window of another
   program is refused, which would make every pad unusable while the game is not focused. */
static BOOL CALLBACK JoyFindOwnWindowProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

static HWND JoyOwnWindow(void) {
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (h && pid == GetCurrentProcessId()) return h;
    h = NULL;
    EnumWindows(JoyFindOwnWindowProc, (LPARAM)&h);
    return h;
}

static void JoyDirectInputFormatInit(void) {
    static const GUID *const axisGuid[8] = {
        &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
        &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider
    };
    static const DWORD axisOffset[8] = {
        DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX,
        DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)
    };
    unsigned i;
    if (g_diFormatReady) return;
    memset(&g_diFormat, 0, sizeof(g_diFormat));
    for (i = 0; i < 8; i++) {
        g_diObjects[i].pguid = (GUID*)axisGuid[i];
        g_diObjects[i].dwOfs = axisOffset[i];
        g_diObjects[i].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(g_diAxisInstance[i]) | DIDFT_OPTIONAL;
    }
    for (i = 0; i < 4; i++) {
        g_diObjects[8 + i].pguid = (GUID*)&GUID_POV;
        g_diObjects[8 + i].dwOfs = DIJOFS_POV(i);
        g_diObjects[8 + i].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
    }
    for (i = 0; i < 128; i++) {
        g_diObjects[12 + i].pguid = (GUID*)&GUID_Button;
        g_diObjects[12 + i].dwOfs = DIJOFS_BUTTON(i);
        g_diObjects[12 + i].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
    }
    g_diFormat.dwSize = sizeof(g_diFormat);
    g_diFormat.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    g_diFormat.dwFlags = DIDF_ABSAXIS;
    g_diFormat.dwDataSize = sizeof(DIJOYSTATE2);
    g_diFormat.dwNumObjs = sizeof(g_diObjects) / sizeof(g_diObjects[0]);
    g_diFormat.rgodf = g_diObjects;
    g_diFormatReady = 1;
}

/* Same d-pad-to-axes projection the XInput and DirectInput paths use. */
static void JoyPovToAxes(DWORD pov, DWORD *xpos, DWORD *ypos) {
    /* POV is 0..35900 (hundredths of a degree, 0 = up), with 0xFFFF = centered */
    if (pov == 0xFFFFu || pov == 0xFFFFFFFFu) return;
    if (pov <= 4500 || pov >= 31500) *ypos = 0;          /* up (includes 0) */
    else if (pov >= 13500 && pov <= 22500) *ypos = 65535; /* down */
    if (pov >= 4500 && pov <= 13500) *xpos = 65535;       /* right */
    else if (pov >= 22500 && pov <= 31500) *xpos = 0;     /* left */
}

static BOOL CALLBACK JoyEnumDirectInput(const DIDEVICEINSTANCEA *instance, void *context) {
    HWND hwnd = (HWND)context;
    LPDIRECTINPUTDEVICE8A device = NULL;
    HRESULT hr;
    DIPROPRANGE axisRange;
    unsigned i;
    if (g_diCount >= sizeof(g_diDevices) / sizeof(g_diDevices[0])) return DIENUM_STOP;
    JoyDirectInputFormatInit();
    g_diEnumSeen++;
    hr = IDirectInput8_CreateDevice(g_di8, &instance->guidInstance, &device, NULL);
    if (FAILED(hr) || !device) {
        if (g_diScans <= 3)
            SmdLog("JoyBridge: DI \"%s\": CreateDevice failed hr=%08X", instance->tszProductName, (unsigned)hr);
        return DIENUM_CONTINUE;
    }
    hr = IDirectInputDevice8_SetDataFormat(device, &g_diFormat);
    if (FAILED(hr) && g_diScans <= 3)
        SmdLog("JoyBridge: DI \"%s\": SetDataFormat failed hr=%08X", instance->tszProductName, (unsigned)hr);
    if (SUCCEEDED(hr) && hwnd) {
        hr = IDirectInputDevice8_SetCooperativeLevel(device, hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(hr) && g_diScans <= 3)
            SmdLog("JoyBridge: DI \"%s\": SetCooperativeLevel failed hr=%08X hwnd=%p", instance->tszProductName,
                (unsigned)hr, (void*)hwnd);
    }
    if (FAILED(hr)) {
        IDirectInputDevice8_Release(device);
        return DIENUM_CONTINUE;
    }
    if (g_diScans <= 3)
        SmdLog("JoyBridge: DI \"%s\" (devtype=%08X) usable as DirectInput joystick %u", instance->tszProductName,
            (unsigned)instance->dwDevType, g_diCount);
    memset(&axisRange, 0, sizeof(axisRange));
    axisRange.diph.dwSize = sizeof(axisRange);
    axisRange.diph.dwHeaderSize = sizeof(axisRange.diph);
    axisRange.diph.dwHow = DIPH_BYID;
    axisRange.lMin = -1000;
    axisRange.lMax = 1000;
    for (i = 0; i < 8; i++) {
        HRESULT hrSet, hrGet;
        axisRange.diph.dwObj = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(g_diAxisInstance[i]);
        hrSet = IDirectInputDevice8_SetProperty(device, DIPROP_RANGE, &axisRange.diph);
        /* always read the range back: Wine or the driver may clamp the range we asked for,
           so trust the actual device range when it reports one */
        hrGet = IDirectInputDevice8_GetProperty(device, DIPROP_RANGE, &axisRange.diph);
        if (SUCCEEDED(hrGet)) {
            g_diAxisMin[g_diCount][i] = axisRange.lMin;
            g_diAxisMax[g_diCount][i] = axisRange.lMax;
        } else if (FAILED(hrSet)) {
            g_diAxisMin[g_diCount][i] = -1000;
            g_diAxisMax[g_diCount][i] = 1000;
        } else {
            g_diAxisMin[g_diCount][i] = -1000;
            g_diAxisMax[g_diCount][i] = 1000;
        }
        (void)hrSet;
    }
    IDirectInputDevice8_Acquire(device);
    g_diDevices[g_diCount++] = device;
    return DIENUM_CONTINUE;
}

static void JoyDirectInputScan(void) {
    PF_DirectInput8Create create;
    HWND hwnd;
    DWORD now = GetTickCount();
    if (g_di8 && now - g_diLastScan < 1000) return;
    g_diLastScan = now;
    if (!g_dinputModule) {
        g_dinputModule = LoadLibraryA("dinput8.dll");
        if (!g_dinputModule) {
            static int warned;
            if (!warned) { warned = 1; SmdLog("JoyBridge: dinput8.dll cannot be loaded (err=%08X)", (unsigned)GetLastError()); }
            return;
        }
    }
    create = (PF_DirectInput8Create)(void*)GetProcAddress(g_dinputModule, "DirectInput8Create");
    if (!create) return;
    if (!g_di8) {
        HRESULT chr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void**)&g_di8, NULL);
        if (FAILED(chr)) {
            static int warned;
            g_di8 = NULL;
            if (!warned) { warned = 1; SmdLog("JoyBridge: DirectInput8Create failed hr=%08X", (unsigned)chr); }
            return;
        }
    }
    if (!g_di8) return;
    hwnd = JoyOwnWindow();
    if (!hwnd) hwnd = GetActiveWindow();
    {
        unsigned oldCount = g_diCount;
        HRESULT ehr;
        while (g_diCount) {
            g_diCount--;
            IDirectInputDevice8_Release(g_diDevices[g_diCount]);
            g_diDevices[g_diCount] = NULL;
        }
        g_diScans++;
        g_diEnumSeen = 0;
        ehr = IDirectInput8_EnumDevices(g_di8, DI8DEVCLASS_GAMECTRL, JoyEnumDirectInput, hwnd, DIEDFL_ATTACHEDONLY);
        if (oldCount != g_diCount || g_diScans <= 3)
            SmdLog("JoyBridge: DirectInput scan #%u: EnumDevices hr=%08X, %u device(s) enumerated, %u usable (was %u), window=%p",
                g_diScans, (unsigned)ehr, g_diEnumSeen, g_diCount, oldCount, (void*)hwnd);
    }
}

/* copy only the data fields; dwSize/dwFlags stay exactly what the caller passed */
static void JoyCopyState(JOYINFOEX *dst, const JOYINFOEX *src) {
    dst->dwXpos = src->dwXpos;
    dst->dwYpos = src->dwYpos;
    dst->dwZpos = src->dwZpos;
    dst->dwRpos = src->dwRpos;
    dst->dwUpos = src->dwUpos;
    dst->dwVpos = src->dwVpos;
    dst->dwButtons = src->dwButtons;
    dst->dwButtonNumber = src->dwButtonNumber;
    dst->dwPOV = src->dwPOV;
}

static int JoyDirectInputState(unsigned id, JOYINFOEX *out) {
    DIJOYSTATE2 state;
    LPDIRECTINPUTDEVICE8A device;
    HRESULT hr;
    DWORD pov;
    unsigned i, buttons = 0;
    JOYINFOEX ji0;
    if (id >= g_diCount || !out) return 0;
    device = g_diDevices[id];
    hr = IDirectInputDevice8_Poll(device);
    if (FAILED(hr)) {
        IDirectInputDevice8_Acquire(device);
        hr = IDirectInputDevice8_Poll(device);
    }
    if (FAILED(hr)) {
#ifdef SMD_DEBUG_BUILD
        static unsigned pollFails[8];
        if (id < 8 && pollFails[id]++ < 5)
            SmdLog("JoyBridge: DI poll failed id=%u hr=%08X", id, (unsigned)hr);
#endif
        return 0;
    }
    memset(&state, 0, sizeof(state));
    hr = IDirectInputDevice8_GetDeviceState(device, sizeof(state), &state);
#ifdef SMD_DEBUG_BUILD
    {
        static unsigned stateLogs[8];
        if (id < 8 && stateLogs[id]++ < 8)
            SmdLog("JoyBridge: DI state id=%u hr=%08X x=%ld y=%ld pov=%u buttons=%02X%02X%02X%02X",
                id, (unsigned)hr, (long)state.lX, (long)state.lY, (unsigned)state.rgdwPOV[0],
                state.rgbButtons[0], state.rgbButtons[1], state.rgbButtons[2], state.rgbButtons[3]);
    }
#endif
    if (FAILED(hr)) return 0;
    memset(&ji0, 0, sizeof(ji0));
#define DI_AXIS(axis, value) ((DWORD)(((value) <= g_diAxisMin[id][axis]) ? 0 : \
    ((value) >= g_diAxisMax[id][axis]) ? 65535 : \
    ((long long)((value) - g_diAxisMin[id][axis]) * 65535LL) / \
    ((long long)g_diAxisMax[id][axis] - g_diAxisMin[id][axis])))
    ji0.dwXpos = DI_AXIS(0, state.lX);
    ji0.dwYpos = DI_AXIS(1, state.lY);
    ji0.dwZpos = DI_AXIS(2, state.lZ);
    ji0.dwRpos = DI_AXIS(3, state.lRx);
    ji0.dwUpos = DI_AXIS(4, state.lRy);
    ji0.dwVpos = DI_AXIS(5, state.lRz);
#undef DI_AXIS
    for (i = 0; i < 16; i++) if (state.rgbButtons[i] & 0x80) buttons |= (1u << i);
    ji0.dwButtons = buttons;
    ji0.dwButtonNumber = 0;
    for (i = 0; i < 16; i++) if (buttons & (1u << i)) ji0.dwButtonNumber++;
    pov = state.rgdwPOV[0];
    ji0.dwPOV = pov == 0xffffffffu ? 0xffffu : pov;
    if (pov != 0xffffffffu) JoyPovToAxes(pov, &ji0.dwXpos, &ji0.dwYpos);
    *out = ji0;
    return 1;
}

/* rate-limited per-call tracer for the game's polls (debug): counts every
   joyGetPosEx/joyGetPos invocation and periodically prints the served state, so
   per-frame activity is visible even when the values never change */
static void JoyLogPoll(unsigned id, const char *src, const JOYINFOEX *ji) {
    static unsigned calls;
    calls++;
    if (calls % 600 == 0)
        SmdLog("JoyBridge: poll #%u id=%u src=%s%s", calls, id, src,
            ji ? "" : " (failed)");
    if (ji && calls % 600 >= 590)
        SmdLog("JoyBridge: poll #%u id=%u X=%u Y=%u B=%08X POV=%u", calls, id,
            (unsigned)ji->dwXpos, (unsigned)ji->dwYpos, (unsigned)ji->dwButtons, (unsigned)ji->dwPOV);
}

#ifdef SMD_DEBUG_BUILD
static void JoyLogStateChange(const char *source, unsigned id, const JOYINFOEX *ji) {    static DWORD prev[16][4];
    static unsigned char initialized[16];
    DWORD values[4];
    if (id >= 16 || !ji) return;
    values[0] = ji->dwXpos;
    values[1] = ji->dwYpos;
    values[2] = ji->dwButtons;
    values[3] = ji->dwPOV;
    if (!initialized[id] || memcmp(prev[id], values, sizeof(values)) != 0) {
        SmdLog("JoyBridge: %s id=%u X=%u Y=%u buttons=%08X POV=%u", source, id,
            (unsigned)values[0], (unsigned)values[1], (unsigned)values[2], (unsigned)values[3]);
        memcpy(prev[id], values, sizeof(values));
        initialized[id] = 1;
    }
}
#endif

/* what the legacy winmm joystick API (Wine's own implementation on Linux) reports for ids 0..3 */
static void JoyNativeToJi(const JOYINFOEX *nat, JOYINFOEX *ji, DWORD requestedSize, DWORD requestedFlags);
static unsigned JoyNativeRead(unsigned id, JOYINFOEX *exe);
static void JoyLogNativeCaps(void) {
    unsigned id;
    if (!g_winmmJoyGetCapsA) return;
    for (id = 0; id < 4; id++) {
        JOYCAPSA jc;
        unsigned r;
        memset(&jc, 0, sizeof(jc));
        r = g_winmmJoyGetCapsA(id, &jc, sizeof(jc));
        if (r == JOYERR_NOERROR)
            SmdLog("JoyBridge: native winmm joystick id=%u: \"%s\" buttons=%u axes=%u", id, jc.szPname,
                (unsigned)jc.wNumButtons, (unsigned)jc.wNumAxes);
        else
            SmdLog("JoyBridge: native winmm joystick id=%u: not available (%u)", id, r);
    }
}

/* One-time pass over the native winmm slots at attach (before the game polls).  The real
   pad can live on any id (Wine enumerates phantom slots before the physical one), so this
   only logs what exists; which id serves the game is decided per poll with hysteresis. */
static void JoyClassifyNatives(void) {
    unsigned id;
    if (!g_winmmJoyGetCapsA || !g_winmmJoyGetPosEx) return;
    for (id = 0; id < 4; id++) {
        JOYCAPSA jc;
        JOYINFOEX st;
        if (g_winmmJoyGetCapsA(id, &jc, sizeof(jc)) != JOYERR_NOERROR) continue;
        if (JoyNativeRead(id, &st) != JOYERR_NOERROR) continue;
        SmdLog("JoyBridge: native winmm id=%u idle state: X=%u Y=%u Z=%u B=%08X POV=%u", id,
            (unsigned)st.dwXpos, (unsigned)st.dwYpos, (unsigned)st.dwZpos,
            (unsigned)st.dwButtons, (unsigned)st.dwPOV);
    }
}

static void JoyIoInit(void) {
    const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    int i;

    g_joyIoInited = 1;
    for (i = 0; i < 3; i++) {
        HMODULE xi = LoadLibraryA(names[i]);
        PF_XInputGetState getState = xi
            ? (PF_XInputGetState)(void*)GetProcAddress(xi, (LPCSTR)"XInputGetState") : NULL;
        if (!getState) continue;
        g_xiGetStateCandidates[g_xiCandidateCount++] = getState;
        SmdLog("JoyBridge: XInput candidate %s loaded", names[i]);
    }
    g_joyIoOk = 1;
    SmdLog("JoyBridge: probing %u XInput interface(s) for connected controllers", g_xiCandidateCount);
    JoyLogNativeCaps();
    JoyClassifyNatives();
}

/* Fetches an XInput slot's state through all loaded interfaces.  An interface that reports
   "connected" but hands back a permanently zeroed, fixed-packet state is dead (Wine's built-in
   XInput does this for a pad that its HID layer cannot see); it is marked as a phantom and
   skipped so the real input sources stay in charge. */
static int JoyXinputLooksDead(unsigned id, PF_XInputGetState fn, const unsigned char *st) {
    unsigned char st2[64];
    unsigned p1 = *(unsigned*)st, i;
    for (i = 0; i < 16; i++) if (st[i]) return 0;
    if (p1 != 1) return 0;              /* a live pad's packet counter rarely idles at exactly 1 */
    Sleep(120);
    memset(st2, 0, sizeof(st2));
    if (fn(id, st2) != 0) return 0;
    for (i = 0; i < 16; i++) if (st2[i]) return 0;
    return *(unsigned*)st2 == p1;
}

static int JoyXInputGrab(unsigned id, void *state) {
    unsigned n;
    if (!g_xiCandidateCount || id >= 4) return 0;
    for (n = 0; n < g_xiCandidateCount; n++) {
        unsigned index = (g_xiPreferredCandidate + n) % g_xiCandidateCount;
        PF_XInputGetState fn = g_xiGetStateCandidates[index];
        unsigned char st[64];
        memset(st, 0, sizeof(st));
        if (fn(id, st) != 0) continue;
        if (!g_xiPhantomChecked[id]) {
            g_xiPhantomChecked[id] = 1;
            if (JoyXinputLooksDead(id, fn, st)) {
                if (!g_xiPhantom[id])
                    SmdLog("JoyBridge: XInput interface %u id=%u is a phantom (success + stuck-zero state), ignored",
                        index, id);
                g_xiPhantom[id] = 1;
                continue;
            }
            SmdLog("JoyBridge: XInput interface %u reports live pads for id=%u", index, id);
        }
        if (g_xiPhantom[id]) continue;
        g_xiPreferredCandidate = index;
        memcpy(state, st, 16);
        return 1;
    }
    return 0;
}

/* read a native winmm id through the original winmm entry point; JOYERR_NOERROR on success */
static unsigned JoyNativeRead(unsigned id, JOYINFOEX *exe) {
    unsigned r;
    memset(exe, 0, sizeof(*exe));
    exe->dwSize = sizeof(*exe);
    exe->dwFlags = JOY_RETURNALL;
    if (!g_winmmJoyGetPosEx) return JOYERR_UNPLUGGED;
    r = g_winmmJoyGetPosEx(id, exe);
    if (r != JOYERR_NOERROR) return r;
    JoyNativeToJi(exe, exe, sizeof(*exe), JOY_RETURNALL);
    return JOYERR_NOERROR;
}

static void PatchJoyImports(void);

/* Keep all four XInput user slots in the game's enumeration even when winmm
   reports fewer legacy joysticks; otherwise higher-numbered XInput pads vanish. */
static unsigned __stdcall JoyGetNumDevs(void) {
    unsigned native, reported;
    if (!g_joyIoInited) JoyIoInit();
    native = g_winmmJoyGetNumDevs ? g_winmmJoyGetNumDevs() : 0;
    reported = native;
    if (g_joyIoOk && native < 4) reported = 4;
#ifdef SMD_DEBUG_BUILD
    {
        static unsigned calls;
        if (calls++ < 4) SmdLog("JoyBridge: joyGetNumDevs native=%u reported=%u", native, reported);
    }
#endif
    return reported;
}

void SmdControllerPoll(void) {
    static DWORD lastPoll;
    DWORD now = GetTickCount();
    if (now - lastPoll < 250) return;
    lastPoll = now;
    if (!g_joyIoInited) JoyIoInit();
    if (!g_joyImportsPatched) PatchJoyImports();
    JoyDirectInputScan();
}

static unsigned JoyNativeBest(void);
static int JoyDirectInputState(unsigned id, JOYINFOEX *out);
static unsigned __stdcall JoyGetCapsA(unsigned id, JOYCAPSA *jc, unsigned cb);

/* fill the game's JOYCAPSA buffer with the values it expects.  The game computes the stick
   centres from caps itself: X centre = (wXmin+wXmax)/2 and Y centre = (wZmin+wRmin)/2,
   each with a 0x4e20 dead zone.  Those must both land on 32767 or every centre position
   reads as "sticks down" through Wine's 0/65535 caps. */
static void JoyVirtualCaps(JOYCAPSA *jc, const char *name, WORD mid, WORD pid) {
    memset(jc, 0, sizeof(*jc));
    jc->wMid = mid;
    jc->wPid = pid;
    strcpy(jc->szPname, name);
    jc->wXmin = 0;      jc->wXmax = 0xFFFE;
    jc->wYmin = 0;      jc->wYmax = 0xFFFE;
    jc->wZmin = 0x7FFF; jc->wZmax = 0xFFFF;
    jc->wRmin = 0x7FFF; jc->wRmax = 0xFFFF;
    jc->wUmin = 0;      jc->wUmax = 0xFFFE;
    jc->wVmin = 0;      jc->wVmax = 0xFFFE;
    jc->wPeriodMin = jc->wPeriodMax = 10000;
    jc->wMaxButtons = jc->wNumButtons = 16;
    jc->wMaxAxes = jc->wNumAxes = 6;
    jc->wCaps = JOY_CAPS_BRIDGED;
}

/* Enumerative part: what the game sees at construction decides how many
   joysticks are polled in its per-frame process.  The ith slot proposed HERE
   MUST therefore return real data later. */
static unsigned __stdcall JoyGetCapsA(unsigned id, JOYCAPSA *jc, unsigned cb) {
    int xi;
#ifdef SMD_DEBUG_BUILD
    static unsigned calls;
    if (calls++ < 8) SmdLog("JoyBridge: game queried joyGetDevCapsA id=%u cb=%u", id, cb);
#endif
    if (!g_joyIoInited) JoyIoInit();
    if (id >= 4) return g_winmmJoyGetCapsA ? g_winmmJoyGetCapsA(id, jc, cb) : JOYERR_UNPLUGGED;

    xi = JoyXInputGrab(id, &(unsigned char[16]){0});
    JoyLogCapsConnected(id, (unsigned)xi);
    if (id == 0) {
        /* game slot #1: always virtual.  The real data may come from XInput, native winmm
           (any id - see JoyPickNative) or DirectInput, but JoyGetPosEx owns that choice;
           keeping caps cheap and stable matters because the game's frame poll calls caps
           for the slot every single frame before reading the state. */
        JoyVirtualCaps(jc, "Smd Bridged Pad", 0x57E, 0x2009);
        return JOYERR_NOERROR;
    }
    if (xi) {
        JoyVirtualCaps(jc, "Modern Controller (XInput)", 0x28E, 0x2FF);
        return JOYERR_NOERROR;
    }

    if (g_winmmJoyGetCapsA)
        return g_winmmJoyGetCapsA(id, jc, cb);
    return JOYERR_UNPLUGGED;
}

/* centred sticks (the game's own threshold is 0x4e20 = 20000 around the centre), no button, no POV */
static int JoyStateNeutral(const JOYINFOEX *j) {
    long dx = (long)j->dwXpos - 32768L, dy = (long)j->dwYpos - 32768L;
    return j->dwButtons == 0 && (j->dwPOV == 0xFFFFu || j->dwPOV == 0xFFFFFFFFu) &&
           dx > -16000 && dx < 16000 && dy > -16000 && dy < 16000;
}

/* The physical pad under Wine/Linux is exposed through the legacy winmm joystick API (the
   "Wine joystick driver"); on regular Windows the pad comes through XInput instead. */
static void JoyNativeToJi(const JOYINFOEX *nat, JOYINFOEX *ji, DWORD requestedSize, DWORD requestedFlags) {
    int count = 0;
    unsigned b;
    *ji = *nat;
    ji->dwSize = requestedSize;
    ji->dwFlags = requestedFlags;
    /* POV 0 = d-pad UP (valid).  Do not neutralize it — an old heuristic for Wine's
       broken backends ate real UP presses; the hidraw/DI fixes make that unnecessary. */
    /* Wine passthrough pads can keep phantom button bits >=16 alive (watched bit 17 on a
       Switch Pro Controller); the game turns every dwButtons bit into an action code, and
       only bits 1..16 are mapped, so strip the rest. */
    ji->dwButtons &= 0xFFFFu;
    b = ji->dwButtons;
    while (b) { b &= (b - 1); count++; }
    ji->dwButtonNumber = (DWORD)count;
    /* the game polls with dwFlags=0x83 and never reads dwPOV: give the hat as stick motion,
       same as the XInput and DirectInput paths do */
    if (ji->dwPOV != 0xFFFFu && ji->dwPOV != 0xFFFFFFFFu)
        JoyPovToAxes(ji->dwPOV, &ji->dwXpos, &ji->dwYpos);
}

/* Pick the native winmm id that will serve the game's id 0.  Only slots free of phantom
   input qualify (classified once at init, see JoyClassifyNatives); this just re-checks
   the cheap caps info so vanished pads are noticed without touching the polled device. */
static unsigned g_nativeBestId = 0xFFFFFFFFu;
static unsigned g_nativeSaneMask;   /* bit per id: caps-OK and not flagged phantom */
static DWORD g_nativeBestAt;

static unsigned JoyNativeBest(void) {
    unsigned id, best = 0xFFFFFFFFu;
    DWORD now = GetTickCount();
    if (!g_winmmJoyGetCapsA) return 0xFFFFFFFFu;
    if (now - g_nativeBestAt < 500) return g_nativeBestId;
    g_nativeBestAt = now;
    g_nativeSaneMask = 0;
    for (id = 0; id < 4; id++) {
        JOYCAPSA jc;
        if (g_winmmJoyGetCapsA(id, &jc, sizeof(jc)) != JOYERR_NOERROR) continue;
        g_nativeSaneMask |= 1u << id;
        if (best == 0xFFFFFFFFu) best = id;
    }
    if (best != g_nativeBestId) {
        if (best == 0xFFFFFFFFu)
            SmdLog("JoyBridge: native winmm has no sane joystick for the game's polling");
        else
            SmdLog("JoyBridge: serving the game's winmm polling from native id=%u", best);
    }
    g_nativeBestId = best;
    return best;
}

/* Which native slot currently carries the pad?  Input does not always stay on js0:
   Steam Input (a Proton game holding the pad) re-routes physical input to its own
   virtual js devices, leaving the physical js0 silent.  So the game's joystick #1 is
   fed by whichever sane native id actually reports input, with hysteresis: the current
   slot keeps serving while it is active; a silent incumbent only yields once an id
   with input is found or it stops existing. */
static unsigned g_nativeServingId = 0xFFFFFFFFu;

static unsigned JoyPickNative(void) {
    JOYINFOEX st;
    unsigned it, firstSane = 0xFFFFFFFFu, activeId = 0xFFFFFFFFu;

    JoyNativeBest();    /* refreshes the sane mask every 500ms, otherwise free */
    if (!g_winmmJoyGetPosEx) return 0xFFFFFFFFu;
#ifdef SMD_DEBUG_BUILD
    {
        static DWORD lastScan;
        DWORD now = GetTickCount();
        if (now - lastScan >= 1000) {
            lastScan = now;
            for (it = 0; it < 4; it++) {
                if ((g_nativeSaneMask & (1u << it)) == 0) continue;
                if (JoyNativeRead(it, &st) != JOYERR_NOERROR) {
                    SmdLog("JoyBridge: scan id=%u read failed", it);
                    continue;
                }
                SmdLog("JoyBridge: scan id=%u X=%u Y=%u B=%08X POV=%u", it,
                    (unsigned)st.dwXpos, (unsigned)st.dwYpos, (unsigned)st.dwButtons, (unsigned)st.dwPOV);
            }
        }
    }
#endif
    for (it = 0; it < 4; it++) {
        if ((g_nativeSaneMask & (1u << it)) == 0) continue;
        if (firstSane == 0xFFFFFFFFu) firstSane = it;
        if (JoyNativeRead(it, &st) != JOYERR_NOERROR) continue;
        if (JoyStateNeutral(&st)) continue;
        if (g_nativeServingId == it) return it;             /* incumbent works */
        if (activeId == 0xFFFFFFFFu) activeId = it;         /* first active */
    }
    if (activeId != 0xFFFFFFFFu) {
        if (activeId != g_nativeServingId)
            SmdLog("JoyBridge: game input now served from native winmm id=%u (input detected there)",
                activeId);
        g_nativeServingId = activeId;
        return activeId;
    }
    if (g_nativeServingId < 4 && (g_nativeSaneMask & (1u << g_nativeServingId)))
        return g_nativeServingId;   /* no activity anywhere: keep the incumbent while it exists */
    if (firstSane != g_nativeServingId)
        SmdLog("JoyBridge: game input served from native winmm id=%u (no active slot; default)", firstSane);
    g_nativeServingId = firstSane;
    return firstSane;
}

/* Same hysteresis pick for DirectInput joysticks.  The Wine legacy winmm passthrough
   feeds corrupted state for many pads (watched on a Switch Pro Controller: a running
   bit-counter in dwButtons, the hat stuck at UP and axis low bits jittering), while
   DirectInput delivers usage-mapped values, so DirectInput is tried first for the
   game's slot #1 and native winmm only as the fallback. */
static int g_diServingId = -1;

static int JoyPickDI(void) {
    JOYINFOEX st;
    int it, first = -1, active = -1;

    if (!g_diCount) return -1;
    for (it = 0; it < (int)g_diCount; it++) {
        if (first < 0) first = it;
        if (!JoyDirectInputState(it, &st)) continue;
        if (JoyStateNeutral(&st)) continue;
        if (g_diServingId == it) return it;             /* incumbent works */
        if (active < 0) active = it;                    /* first active */
    }
    if (active >= 0) {
        if (active != g_diServingId)
            SmdLog("JoyBridge: game input now served from DirectInput joystick %d (input detected there)", active);
        g_diServingId = active;
        return active;
    }
    if (g_diServingId >= 0 && g_diServingId < (int)g_diCount)
        return g_diServingId;                           /* nothing active: keep the incumbent */
    g_diServingId = first;
    return first;
}

/* The data path for the game's per-frame polls.  Order of preference:
   1. DirectInput   - usage-mapped and stable (Wine's winmm passthrough corrupts some pads)
   2. native winmm  - fallback; feeds the pad on systems without DirectInput support
   3. XInput        - first-class on real Windows (phantom interfaces never make it here) */
static unsigned __stdcall JoyGetPosEx(unsigned id, JOYINFOEX *ji) {
    DWORD requestedSize = ji ? ji->dwSize : 0, requestedFlags = ji ? ji->dwFlags : 0;
    JOYINFOEX st;

    if (!g_joyIoInited) JoyIoInit();
    if (!g_joyImportsPatched) PatchJoyImports();

    if (id == 0) {
        int haveNative = 0;
        /* 0. raw hidraw (Linux/Wine only, known-broken pads): bypasses Wine's misparsed
           winmm/dinput backends entirely; no-ops on real Windows */
        JoyHidrawPoll();
        if (g_hidReady) {
            /* matched pad present: hidraw is the ground truth (Wine's winmm/dinput
               misparse this device).  Serve it even when idle so the broken backends
               cannot inject phantom button presses. */
            JOYINFOEX hs = g_hidState;
            hs.dwSize = sizeof(hs);
            hs.dwFlags = JOY_RETURNALL;
            JoyNativeToJi(&hs, ji, requestedSize, requestedFlags);
#ifdef SMD_DEBUG_BUILD
            JoyLogStateChange("served hidraw", id, ji);
            JoyLogPoll(id, "hidraw", ji);
#endif
            g_joyPlugged[id] = 1;
            return JOYERR_NOERROR;
        }
        /* 1. DirectInput (best on Wine): the legacy winmm passthrough is broken for several
           pads (e.g. Switch Pro reports a counting button field and stuck-up POV/axis bits);
           DirectInput delivers usage-mapped state that is stable */
        JoyDirectInputScan();
        {
            JOYINFOEX diSt;
            int diId = JoyPickDI();
            if (diId >= 0 && JoyDirectInputState(diId, &diSt) == 1 && !JoyStateNeutral(&diSt)) {
                JoyNativeToJi(&diSt, ji, requestedSize, requestedFlags);
#ifdef SMD_DEBUG_BUILD
                JoyLogStateChange("served DirectInput", id, ji);
                JoyLogPoll(id, "dinput", ji);
#endif
                g_joyPlugged[id] = 1;
                return JOYERR_NOERROR;
            }
        }
        /* 2. native winmm (fallback for pads without a sane DirectInput device, or real
           systems that do not route XInput through winmm) */
        {
            JOYINFOEX natSt;
            unsigned nativeId = JoyPickNative();
            if (nativeId != 0xFFFFFFFFu && JoyNativeRead(nativeId, &natSt) == JOYERR_NOERROR) {
                int neutral = JoyStateNeutral(&natSt);
                JoyNativeToJi(&natSt, ji, requestedSize, requestedFlags);
                haveNative = 1;
                if (!neutral) {
#ifdef SMD_DEBUG_BUILD
                    JoyLogStateChange("served native winmm", id, ji);
                    JoyLogPoll(id, "native", ji);
#endif
                    g_joyPlugged[id] = 1;
                    return JOYERR_NOERROR;
                }
            }
        }
        /* 3. XInput (first-class on real Windows; phantom on Wine) */
        if (JoyXInputGrab(id, &st)) {
            unsigned char *x = (unsigned char*)&st;
            unsigned short btn = *(unsigned short*)(x + 4);
            unsigned char lt = x[6], rt = x[7];
            short sLX = *(short*)(x + 8), sLY = *(short*)(x + 0xA);
            short sRX = *(short*)(x + 0xC), sRY = *(short*)(x + 0xE);
            unsigned dwButtons = 0;

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

            /* -32768..32767 -> 0..0xFFFF; X: right = large; Y: down = large (XInput's Y is up-positive) */
            memset(&st, 0, sizeof(st));
            st.dwXpos = (DWORD)((int)sLX + 32768);
            st.dwYpos = (DWORD)(32768 - (int)sLY);
            st.dwZpos = (DWORD)((int)sRX + 32768);
            st.dwRpos = (DWORD)(32768 - (int)sRY);
            if (st.dwYpos > 65535) st.dwYpos = 65535;
            st.dwUpos = (DWORD)(lt ? (unsigned)lt * 0x101 : 0);
            st.dwVpos = (DWORD)(rt ? (unsigned)rt * 0x101 : 0);
            st.dwButtons = dwButtons;
            {
                int n = 0;
                unsigned b = dwButtons;
                while (b) { b &= (b - 1); n++; }
                st.dwButtonNumber = (DWORD)n;
            }
            if (btn & XI_ID_DPAD_UP)            st.dwPOV = 0;          /* north */
            else if (btn & XI_ID_DPAD_RIGHT)    st.dwPOV = 9000;
            else if (btn & XI_ID_DPAD_DOWN)     st.dwPOV = 18000;
            else if (btn & XI_ID_DPAD_LEFT)     st.dwPOV = 27000;
            else                                st.dwPOV = 0xFFFF;     /* centered */
            /* the game also reads axis movement from the d-pad, so project it there too */
            JoyPovToAxes(st.dwPOV, &st.dwXpos, &st.dwYpos);
            if (!JoyStateNeutral(&st)) {
                JoyNativeToJi(&st, ji, requestedSize, requestedFlags);
#ifdef SMD_DEBUG_BUILD
                JoyLogStateChange("served XInput", id, ji);
                JoyLogPoll(id, "xinput", ji);
#endif
                g_joyPlugged[id] = 1;
                return JOYERR_NOERROR;
            }
        }
        if (haveNative) {
            /* nothing is pressed on any source: the idle native data is already in ji */
#ifdef SMD_DEBUG_BUILD
            {
                JOYINFOEX tr = *ji;
                JoyLogPoll(id, "native-idle", &tr);
            }
#endif
            g_joyPlugged[id] = 1;
            return JOYERR_NOERROR;
        }
    }

    if (id >= 4) return g_winmmJoyGetPosEx ? g_winmmJoyGetPosEx(id, ji) : JOYERR_UNPLUGGED;
    if (g_winmmJoyGetPosEx && JoyNativeRead(id, &st) == JOYERR_NOERROR) {
        JoyNativeToJi(&st, ji, requestedSize, requestedFlags);
#ifdef SMD_DEBUG_BUILD
        JoyLogStateChange("native winmm", id, ji);
        JoyLogPoll(id, "native-passthrough", ji);
#endif
        g_joyPlugged[id] = 1;
        return JOYERR_NOERROR;
    }
    JoyDirectInputScan();
    if (JoyDirectInputState(id, &st)) {
        JoyNativeToJi(&st, ji, requestedSize, requestedFlags);
#ifdef SMD_DEBUG_BUILD
        JoyLogStateChange("DirectInput", id, ji);
#endif
        g_joyPlugged[id] = 1;
        return JOYERR_NOERROR;
    }
    if (id == 0) {
        /* no source reports slot #1 at all: the game never checks joyGetPosEx's return
           value but reads the buffer directly, so answer success with a neutral state
           instead of UNPLUGGED with an untouched buffer full of stale stack values */
        memset(ji, 0, requestedSize);
        ji->dwSize = requestedSize;
        ji->dwFlags = requestedFlags;
        ji->dwXpos = ji->dwYpos = ji->dwZpos = ji->dwRpos = 32767;
        ji->dwUpos = ji->dwVpos = 0;
        ji->dwPOV = 0xFFFF;
#ifdef SMD_DEBUG_BUILD
        {
            static DWORD lastIdle;
            DWORD now = GetTickCount();
            if (now - lastIdle >= 2000) {
                lastIdle = now;
                SmdLog("JoyBridge: no live source for slot #1, serving neutral idle");
            }
        }
#endif
        return JOYERR_NOERROR;
    }
    if (g_joyPlugged[id]) SmdLog("JoyBridge: joystick id=%u unplugged (no source reports it)", id);
    g_joyPlugged[id] = 0;
    return JOYERR_UNPLUGGED;
}

/* joyGetPos is the older, JOYINFO-sized variant.  Some game paths (the config UI
   in particular) use it instead of joyGetPosEx; serve it from the same state. */
static unsigned __stdcall JoyGetPos(unsigned id, JOYINFO *ji) {
    JOYINFOEX ex;
    unsigned r;
    if (!ji) return g_winmmJoyGetPos ? g_winmmJoyGetPos(id, ji) : JOYERR_PARMS;
    memset(&ex, 0, sizeof(ex));
    ex.dwSize = sizeof(ex);
    ex.dwFlags = JOY_RETURNALL;
    r = JoyGetPosEx(id, &ex);
    if (r != JOYERR_NOERROR) return r;
#ifdef SMD_DEBUG_BUILD
    {
        JOYINFOEX tr = ex;
        JoyLogPoll(id, "joyGetPos", &tr);
    }
#endif
    ji->wXpos = (UINT)ex.dwXpos;
    ji->wYpos = (UINT)ex.dwYpos;
    ji->wZpos = (UINT)ex.dwZpos;
    ji->wButtons = (WORD)ex.dwButtons;
    return JOYERR_NOERROR;
}

/* Patch every winmm joystick import by name.  The two hardcoded slots below are
   enough for the game's own poll, but joyGetNumDevs decides whether it ever
   enumerates at all, and XInput-only systems can report 0 of those. */
static void PatchJoyImports(void) {
    HMODULE mod = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *desc;
    DWORD dirRva;
    int patched = 0, foundWinmm = 0;

    if (g_joyImportsPatched || !mod) return;
    dos = (IMAGE_DOS_HEADER*)(void*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS32*)((char*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    dirRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!dirRva) return;

    for (desc = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod + dirRva); desc->Name; desc++) {
        const char *dll = (const char*)mod + desc->Name;
        IMAGE_THUNK_DATA32 *oft, *iat;
        if (_stricmp(dll, "winmm.dll") != 0) continue;
        foundWinmm = 1;
        if (!desc->OriginalFirstThunk) continue;
        oft = (IMAGE_THUNK_DATA32*)((char*)mod + desc->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA32*)((char*)mod + desc->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, iat++) {
            const char *name;
            void *shim = NULL;
            void **orig = NULL;
            DWORD old;

            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            name = (const char*)((IMAGE_IMPORT_BY_NAME*)((char*)mod + oft->u1.AddressOfData))->Name;
            if (strcmp(name, "joyGetDevCapsA") == 0) {
                shim = (void*)&JoyGetCapsA;
                orig = (void**)&g_winmmJoyGetCapsA;
            } else if (strcmp(name, "joyGetPosEx") == 0) {
                shim = (void*)&JoyGetPosEx;
                orig = (void**)&g_winmmJoyGetPosEx;
            } else if (strcmp(name, "joyGetPos") == 0) {
                shim = (void*)&JoyGetPos;
                orig = (void**)&g_winmmJoyGetPos;
            } else if (strcmp(name, "joyGetNumDevs") == 0) {
                shim = (void*)&JoyGetNumDevs;
                orig = (void**)&g_winmmJoyGetNumDevs;
            }
            if (!shim || (void*)(uintptr_t)iat->u1.Function == shim) continue;
            if (!*orig) *orig = (void*)(uintptr_t)iat->u1.Function;
            if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old)) {
                iat->u1.Function = (DWORD_PTR)(uintptr_t)shim;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
                patched++;
                SmdLog("JoyBridge: winmm import %s patched", name);
            }
        }
    }
    /* If winmm was not in the import table yet, retry from SmdControllerPoll. */
    if (foundWinmm) g_joyImportsPatched = 1;
    if (patched)
        SmdLog("JoyBridge: winmm import table patched (%d entr%s)", patched, patched == 1 ? "y" : "ies");
}

void SmdControllerInit(void) {
    void **pPos = (void**)(uintptr_t)0x4532c4;
    void **pCaps = (void**)(uintptr_t)0x4532c8;
    DWORD old = 0;

    if ((DWORD)(uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        SmdLog("JoyBridge: unexpected image base, skipped");
        return;
    }
    PatchJoyImports();
    if (!pPos || !pCaps || !*pPos || !*pCaps) {
        SmdLog("JoyBridge: winmm joystick entry points are unavailable");
        return;
    }
    if (*pPos == (void *)&JoyGetPosEx && *pCaps == (void *)&JoyGetCapsA) return;
    if (VirtualProtect(pPos, 8, PAGE_READWRITE, &old)) {
        if (*pPos != (void *)&JoyGetPosEx) {
            if (!g_winmmJoyGetPosEx) g_winmmJoyGetPosEx = (PF_JoyGetPosEx)*pPos;
            *pPos = (void *)&JoyGetPosEx;
        }
        if (*pCaps != (void *)&JoyGetCapsA) {
            if (!g_winmmJoyGetCapsA) g_winmmJoyGetCapsA = (PF_JoyGetCapsA)*pCaps;
            *pCaps = (void *)&JoyGetCapsA;
        }
        VirtualProtect(pPos, 8, old, &old);
        SmdLog("JoyBridge: dynamic XInput hotplug hooks installed; disconnected devices use winmm");
    } else {
        SmdLog("JoyBridge: VirtualProtect failed (err=%08X)", (unsigned)GetLastError());
    }
}


int SmdControllerIsConnected(unsigned id) {
    return id < 4 && g_joyPlugged[id] != 0;
}
