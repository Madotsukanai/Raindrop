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

static int g_fullscreenRequested = 0;
static int g_displayModeFailures = 0;

static void D7ExeDirFile(const char *name, char *out) {
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(out, name);
}

static HRESULT D7TrySetDisplayMode(IDirectDraw7 *orig, DWORD wd, DWORD ht, DWORD bpp, DWORD refresh, DWORD flags) {
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
        SmdLog("DD7.SetDisplayMode retry refresh=%lu -> hr=%08X", (unsigned long)tries[i], (unsigned)hr);
        if (hr >= 0) return hr;
    }
    return hr;
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

void SmdD3D9CensusInit(void) {
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
                SmdLog("Api[%s] %s #%u this=%08X args=%08X %08X %08X %08X %08X", g_cenIfName[iface],
                    CensusName(iface, slot), g_cenCount[iface][slot], argp[0], a1, a2, a3, argp[4], argp[5]);
                break;
            }
            if (s->tag == tag && s->a[0] == a1 && s->a[1] == a2 && s->a[2] == a3) break;
        }
    }
    LeaveCriticalSection(&g_cenCs);
}

void SmdD3D9CensusSummary(void) {
    unsigned iface, slot;
    if (!g_ApiCensus) return;
    EnterCriticalSection(&g_cenCs);
    for (iface = 0; iface < CEN_IFACES; iface++)
        for (slot = 0; slot < CEN_SLOTS; slot++)
            if (g_cenCount[iface][slot])
                SmdLog("ApiSummary[%s] %s calls=%u", g_cenIfName[iface], CensusName(iface, slot),
                    g_cenCount[iface][slot]);
    LeaveCriticalSection(&g_cenCs);
}

void SmdD3D9CensusSummaryPeriodic(void) {
    static int n = 0;
    if (g_ApiCensus && ++n % 6 == 0) SmdD3D9CensusSummary();
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
        if (stub && SmdPatchVtblSlot(&vt[slot], stub, &old)) ok++;
    }
    SmdLog("Api: %s vtable %p hooked (%d slots)", g_cenIfName[iface], (void*)vt, ok);
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
    SmdLog("Api[d3d7] CreateDevice clsid=%08lX-%04X-%04X-%02X%02X surf=%p -> hr=%08X dev=%p",
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
    SmdPatchVtblSlot(&vt[4], (void*)&CensusCreateDevice, &old);
    {
        unsigned slot;
        int ok = 0;
        for (slot = 3; slot < 8; slot++) {
            void *stub, *o = NULL;
            if (slot == 4) continue;
            stub = CensusMakeStub((CEN_D3D7 << 8) | slot, vt[slot]);
            if (stub && SmdPatchVtblSlot(&vt[slot], stub, &o)) ok++;
        }
        SmdLog("Api: d3d7 vtable %p hooked (CreateDevice + %d slots)", (void*)vt, ok);
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

int SmdD3D9EmuActive(void) { return g_d7.active; }

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
    SmdBatchFlushQuads();   /* called before every real state change: the queued sprites use the old state */
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
            SmdLog("D3D7 emu: CreateTexture %dx%d failed hr=%08X", s->w, s->h, (unsigned)hr);
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
    SmdLog("D3D7 emu: borderless fullscreen window %ldx%ld at (%ld,%ld)",
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
    SmdLog("Foreground: window %p %s", (void*)hwnd, GetForegroundWindow() == hwnd ? "is now in the foreground" : "could not be brought to the foreground");
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
        SmdLog("D3D7 emu: normal window %dx%d at (%d,%d)", w, h, x, y);
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
    if (!create9) { SmdLog("D3D7 emu: d3d9.dll / Direct3DCreate9 not available"); return 0; }
    g_d7.d3d = create9(D3D_SDK_VERSION);
    if (!g_d7.d3d) { SmdLog("D3D7 emu: Direct3DCreate9 failed"); return 0; }
    if (!hwnd) { SmdLog("D3D7 emu: no window"); return 0; }
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
        SmdLog("D3D7 emu: CreateDevice(pure device) hr=%08X", (unsigned)hr);
        if (FAILED(hr)) g_d7.dev = NULL;
    }
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(g_d7.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_d7.dev);
    }
    if (FAILED(hr)) {
        SmdLog("D3D7 emu: CreateDevice(HW vp) hr=%08X, retrying with software vertex processing", (unsigned)hr);
        hr = IDirect3D9_CreateDevice(g_d7.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_d7.dev);
    }
    if (FAILED(hr)) {
        SmdLog("D3D7 emu: Direct3D9 CreateDevice failed hr=%08X", (unsigned)hr);
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
    SmdLog("D3D7 emu: Direct3D9 device created (%dx%d, hwnd=%p)", g_d7.dispW, g_d7.dispH, (void*)hwnd);
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
                SmdLog("D3D7 emu: device reset");
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
                SmdLog("D3D7 emu: Blt to the primary surface failed (invalid rectangle or texture)");
            D7_Present();
        } else if (D7_Warn(D7W_PRIMBLT)) {
            SmdLog("D3D7 emu: Blt to the primary surface is not possible (no source or no Direct3D9 device yet)");
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
                SmdLog("D3D7 emu: Blt into the 3D back buffer failed (flags=%08X)", (unsigned)flags);
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
            SmdLog("D3D7 emu: BltFast into the 3D back buffer / primary failed");
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

static void D7_SyncD3D9PixelsToDib(D7Surf *s) {
    IDirect3DSurface9 *back = NULL, *readback = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lock;
    HRESULT hr;
    int x, y;
    if (!s || !s->bits || (s->bpp != 16 && s->bpp != 32) || !g_d7.dev ||
        (s != g_d7.back3d && !s->isPrimary)) return;
    D7_BatchFlush();
    hr = IDirect3DDevice9_GetBackBuffer(g_d7.dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (FAILED(hr) || !back) return;
    hr = IDirect3DSurface9_GetDesc(back, &desc);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_d7.dev, desc.Width, desc.Height, desc.Format,
                                                          D3DPOOL_SYSTEMMEM, &readback, NULL);
    if (SUCCEEDED(hr) && readback)
        hr = IDirect3DDevice9_GetRenderTargetData(g_d7.dev, back, readback);
    if (SUCCEEDED(hr) && readback)
        hr = IDirect3DSurface9_LockRect(readback, &lock, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr) && readback) {
        for (y = 0; y < s->h; y++) {
            int sy = (int)((long long)y * desc.Height / s->h);
            const DWORD *src = (const DWORD*)((const unsigned char*)lock.pBits + (size_t)sy * lock.Pitch);
            unsigned char *dst = s->bits + (size_t)y * s->pitch;
            for (x = 0; x < s->w; x++) {
                int sx = (int)((long long)x * desc.Width / s->w);
                D7_Wr(s, dst + (size_t)x * (s->bpp / 8), D7_FromArgb(&s->fmt, src[sx] | 0xff000000u));
            }
        }
        IDirect3DSurface9_UnlockRect(readback);
        D7_MarkDirty(s, NULL);
    }
    if (readback) IDirect3DSurface9_Release(readback);
    IDirect3DSurface9_Release(back);
}

static HRESULT STDMETHODCALLTYPE ES_GetDC(void *self, HDC *out) {
    D7Surf *s = (D7Surf*)self;
    if (!out) return DDERR_INVALIDPARAMS;
    if (!s->dib) return DDERR_CANTCREATEDC;
    D7_SyncD3D9PixelsToDib(s);
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
        SmdLog("D3D7 emu: Lock on the 3D back buffer (contents are not synchronised with Direct3D9)");
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
        if (g_d7.ckEnable != (v != 0)) SmdBatchFlushQuads();
        g_d7.ckEnable = (v != 0);
        break;
    default:
        if (D7_RsPass(st)) D7_ApplyRS(st, v);
        else if (st < 256 && !logged[st]) { logged[st] = 1; SmdLog("D3D7 emu: render state %lu ignored (value %lu)", (unsigned long)st, (unsigned long)v); }
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
    SmdLog("D3D7 emu: IDirect3D7::CreateDevice ok (back buffer surface %p)", surf);
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
            SmdLog("D3D7 emu: unsupported pixel format (%lu bpp) for a %dx%d surface",
                (unsigned long)pf.dwRGBBitCount, w, h);
            return DDERR_INVALIDPIXELFORMAT;
        }
        s = D7_SurfNew(w, h, &pf, caps);
        if (!s) return DDERR_OUTOFMEMORY;
    }
    if (g_d7.surfLogged < 40) {
        g_d7.surfLogged++;
        SmdLog("D3D7 emu: CreateSurface %dx%d caps=%08X flags=%08X bpp=%d masks=%08X/%08X/%08X/%08X -> %p",
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
    D7ExeDirFile("config.txt", path);
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

    D7ExeDirFile("config.txt", path);
    buf = WndModeReadFile(path, &n);
    if (!buf) { SmdLog("WndMode: cannot read %s", path); return 0; }
    if (!WndModeFind(buf, n, &vs, &ve)) {
        SmdLog("WndMode: no WndMode= line in config.txt");
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
    SmdLog("WndMode: config.txt WndMode=%s %s", val, ok ? "written" : "FAILED to write");
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
    SmdLog("WndMode: Alt+Enter -> WndMode=%s%s", wantWindowed ? "on" : "off",
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

void SmdD3D9PatchEnterMask(void) {
    int a = HookIat("GetAsyncKeyState", (void*)&Hook_GetAsyncKeyState, (void**)&g_origGetAsyncKeyState);
    int b = HookIat("GetKeyState", (void*)&Hook_GetKeyState, (void**)&g_origGetKeyState);
    int c = HookIat("GetKeyboardState", (void*)&Hook_GetKeyboardState, (void**)&g_origGetKeyboardState);
    SmdLog("EnterMask: IAT hooks GetAsyncKeyState=%d GetKeyState=%d GetKeyboardState=%d%s", a, b, c,
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
        SmdLog("WndMode: could not subclass the game window (err=%08X)", (unsigned)GetLastError());
        return;
    }
    g_origWndProc = (WNDPROC)prev;
    g_hotkeyHwnd = hwnd;
    SmdLog("WndMode: Alt+Enter hotkey attached to hwnd=%p", (void*)hwnd);
}

static HRESULT STDMETHODCALLTYPE ED_SetCooperativeLevel(void *self, HWND hwnd, DWORD flags) {
    if (hwnd) g_d7.hwnd = hwnd;
    HotkeyAttach(hwnd);
    g_d7.fs = (flags & DDSCL_FULLSCREEN) ? 1 : 0;
    if (g_d7.fs) D7_SetupBorderless(hwnd);
    SmdLog("D3D7 emu: SetCooperativeLevel hwnd=%p flags=%X (%s)", (void*)hwnd, (unsigned)flags,
        g_d7.fs ? "borderless fullscreen" : "window mode");
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE ED_SetDisplayMode(void *self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
    g_d7.dispW = (int)w;
    g_d7.dispH = (int)h;
    SmdLog("D3D7 emu: SetDisplayMode %lux%lux%lu (virtual, desktop mode unchanged)", (unsigned long)w, (unsigned long)h, (unsigned long)bpp);
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
    D7ExeDirFile("raindrop.ini", path);
    WritePrivateProfileStringA("Graphics", "API", api, path);
}

static int D7E_ConfigD3D9(void) {
    static int initialized = 0;
    static int useD3D9 = 0;
    char path[MAX_PATH];
    char value[32];
    if (!initialized) {
        D7ExeDirFile("raindrop.ini", path);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                static const char defaults[] = "[Graphics]\r\nAPI=DirectDraw\r\n";
                DWORD written;
                WriteFile(file, defaults, (DWORD)(sizeof(defaults) - 1), &written, NULL);
                CloseHandle(file);
                g_D7E_ConfigCreated = 1;
                SmdLog("Created raindrop.ini with default Graphics API");
            }
        }
        GetPrivateProfileStringA("Graphics", "API", "DirectDraw", value, sizeof(value), path);
        useD3D9 = (_stricmp(value, "D3D9") == 0 || _stricmp(value, "Direct3D9") == 0);
        initialized = 1;
        SmdLog("Graphics API: %s (raindrop.ini or default)", useD3D9 ? "D3D9" : "DirectDraw");
    }
    return useD3D9;
}

static void D7E_SaveFallbackAPI(void) {
    if (g_D7E_ConfigCreated) {
        D7E_WriteConfigAPI("D3D9");
        g_D7E_ConfigCreated = 0;
        SmdLog("Saved D3D9 fallback to raindrop.ini");
    }
}

static HRESULT D7E_CreateDD7(void **out) {
    if (!out) return DDERR_INVALIDPARAMS;
    g_d7.active = 1;
    g_d7DD.ref++;
    *out = &g_d7DD;
    SmdLog("D3D7 emu: using the built-in DirectDraw7/Direct3D7 -> Direct3D9 layer");
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
    SmdLogOpen();
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strcat(sys, "\\ddraw.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) {
        SmdLog("[Error] LoadSystem32 ddraw.dll failed (err=%08X)", (unsigned)GetLastError());
        return;
    }
    SmdLog("Real ddraw: using %s", sys);
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
    SmdLog("Smd ddraw proxy loaded (real=%p lowlatency=%d dwmflush=%d)",
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
            SmdLog("DD7.QI failed iid=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X hr=%08X",
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
    SmdLog("DD7.Release -> orig refs=%lu", r);
    if (r == 0) {
        free(w);
        SmdLog("DD7 wrapper destroyed");
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE WDD7_Compact(IDirectDraw7 *self) {
    return IDirectDraw7_Compact(((DD7W*)self)->orig);
}
static HRESULT STDMETHODCALLTYPE WDD7_CreateClipper(IDirectDraw7 *self, DWORD f, LPDIRECTDRAWCLIPPER *c, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreateClipper(w->orig, f, c, u);
    SmdLog("DD7.CreateClipper f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_CreatePalette(IDirectDraw7 *self, DWORD f, LPPALETTEENTRY t, LPDIRECTDRAWPALETTE *p, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreatePalette(w->orig, f, t, p, u);
    SmdLog("DD7.CreatePalette f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_CreateSurface(IDirectDraw7 *self, LPDDSURFACEDESC2 d, LPDIRECTDRAWSURFACE7 *s, IUnknown *u) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_CreateSurface(w->orig, d, s, u);
    if (d && g_LogAll)
        SmdLog("DD7.CreateSurface %lux%lux bpp=%d caps=%lX/%lX back=%d dwFlags=%lX -> hr=%08X",
            (unsigned long)d->dwWidth, (unsigned long)d->dwHeight,
            (d->dwFlags & DDSD_PIXELFORMAT) ? (int)d->ddpfPixelFormat.dwRGBBitCount : -1,
            (unsigned long)((d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0),
            (unsigned long)d->dwFlags, (int)d->dwBackBufferCount, hr);
    else if (hr < 0)
        SmdLog("DD7.CreateSurface -> hr=%08X", hr);
    if (hr < 0 && g_fullscreenRequested && d &&
        (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        SmdTriggerWindowFallback("CreateSurface(primary) failed");
    if (hr >= 0 && g_LowLatency && d && s && *s &&
        (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        SmdHookPrimary(*s);
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
    SmdLog("DD7.EnumDisplayModes f=%X -> hr=%08X", f, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_EnumSurfaces(IDirectDraw7 *self, DWORD f, LPDDSURFACEDESC2 d, LPVOID c, LPDDENUMSURFACESCALLBACK7 cb) {
    return IDirectDraw7_EnumSurfaces(((DD7W*)self)->orig, f, d, c, cb);
}
static HRESULT STDMETHODCALLTYPE WDD7_FlipToGDISurface(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_FlipToGDISurface(((DD7W*)self)->orig);
    SmdLog("DD7.FlipToGDISurface -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetCaps(IDirectDraw7 *self, LPDDCAPS h, LPDDCAPS hel) {
    HRESULT hr = IDirectDraw7_GetCaps(((DD7W*)self)->orig, h, hel);
    if (g_LogAll) SmdLog("DD7.GetCaps -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetDisplayMode(IDirectDraw7 *self, LPDDSURFACEDESC2 d) {
    HRESULT hr = IDirectDraw7_GetDisplayMode(((DD7W*)self)->orig, d);
    if (d && g_LogAll)
        SmdLog("DD7.GetDisplayMode -> hr=%08X %lux%lux modex=%s bpp=%d", hr,
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
    if (g_LogAll) SmdLog("DD7.GetMonitorFrequency -> hr=%08X freq=%lu", hr, f ? *f : 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetScanLine(IDirectDraw7 *self, LPDWORD s) {
    return IDirectDraw7_GetScanLine(((DD7W*)self)->orig, s);
}
static HRESULT STDMETHODCALLTYPE WDD7_GetVerticalBlankStatus(IDirectDraw7 *self, BOOL *b) {
    return IDirectDraw7_GetVerticalBlankStatus(((DD7W*)self)->orig, b);
}
static HRESULT STDMETHODCALLTYPE WDD7_Initialize(IDirectDraw7 *self, GUID *g) {
    return IDirectDraw7_Initialize(((DD7W*)self)->orig, g);
}
static HRESULT STDMETHODCALLTYPE WDD7_RestoreDisplayMode(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_RestoreDisplayMode(((DD7W*)self)->orig);
    SmdLog("DD7.RestoreDisplayMode -> hr=%08X", hr);
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
    SmdLog("DD7.SetCooperativeLevel hwnd=%p flags=%X -> hr=%08X",
        (void*)hwnd, (unsigned)flags, hr);
    if (hr >= 0 || !g_RetryCoopLevel) return hr;

    if (flags & DDSCL_EXCLUSIVE) {
        DWORD f = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        SmdLog("DD7.SetCooperativeLevel retry1 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
        SmdTriggerWindowFallback("SetCooperativeLevel(exclusive) failed");
    }
    {
        DWORD f = DDSCL_NORMAL | DDSCL_NOWINDOWCHANGES;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        SmdLog("DD7.SetCooperativeLevel retry2 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
    }
    if (flags & DDSCL_FULLSCREEN) {
        DWORD f = DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES;
        hr = IDirectDraw7_SetCooperativeLevel(w7->orig, hwnd, f);
        SmdLog("DD7.SetCooperativeLevel retry3 flags=%X -> hr=%08X", f, hr);
        if (hr >= 0) return hr;
    }
    return DDERR_GENERIC;
}
static HRESULT STDMETHODCALLTYPE WDD7_SetDisplayMode(IDirectDraw7 *self, DWORD wd, DWORD ht, DWORD bpp, DWORD refresh, DWORD flags) {
    DD7W *w7 = (DD7W*)self;
    HRESULT hr = IDirectDraw7_SetDisplayMode(w7->orig, wd, ht, bpp, refresh, flags);
    SmdLog("DD7.SetDisplayMode %lux%lux bpp=%lu refresh=%lu f=%X -> hr=%08X",
        (unsigned long)wd, (unsigned long)ht, (unsigned long)bpp, (unsigned long)refresh, (unsigned)flags, hr);
    if (hr < 0 && g_RetryDisplayMode)
        hr = D7TrySetDisplayMode(w7->orig, wd, ht, bpp, refresh, flags);
    if (hr < 0) {
        g_displayModeFailures++;
        if (bpp <= 16 || g_displayModeFailures >= 2)
            SmdTriggerWindowFallback("SetDisplayMode failed");
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_WaitForVerticalBlank(IDirectDraw7 *self, DWORD f, HANDLE ev) {
    HRESULT hr = IDirectDraw7_WaitForVerticalBlank(((DD7W*)self)->orig, f, ev);
    if (g_LogAll) SmdLog("DD7.WaitForVerticalBlank f=%X -> hr=%08X", f, hr);
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
    SmdLog("DD7.RestoreAllSurfaces -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_TestCooperativeLevel(IDirectDraw7 *self) {
    HRESULT hr = IDirectDraw7_TestCooperativeLevel(((DD7W*)self)->orig);
    if (hr < 0 || g_LogAll) SmdLog("DD7.TestCooperativeLevel -> hr=%08X", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WDD7_GetDeviceIdentifier(IDirectDraw7 *self, LPDDDEVICEIDENTIFIER2 p, DWORD f) {
    DD7W *w = (DD7W*)self;
    HRESULT hr = IDirectDraw7_GetDeviceIdentifier(w->orig, p, f);
    SmdLog("DD7.GetDeviceIdentifier -> hr=%08X dev=%lld desc=%.64s", hr,
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
        SmdLog("DirectDrawCreateEx (other) -> hr=%08X", hr);
        return hr;
    }
    if (g_D7Emu && (D7E_Forced() || D7E_ConfigD3D9())) {
        SmdLog("GraphicsAPI: selecting built-in Direct3D9 emulation");
        return D7E_CreateDD7(out);
    }
    if (!p_DirectDrawCreateEx) {
        if (g_D7Emu) {
            SmdLog("System DirectDrawCreateEx is unavailable; switching to Direct3D9 emulation");
            D7E_SaveFallbackAPI();
            return D7E_CreateDD7(out);
        }
        return DDERR_GENERIC;
    }
    PF_DDCEx fn = (PF_DDCEx)p_DirectDrawCreateEx;
    IDirectDraw7 *orig = NULL;
    HRESULT hr = fn(guid, (LPVOID*)&orig, riid, unk);
    SmdLog("DirectDrawCreateEx(IID_IDirectDraw7) -> hr=%08X orig=%p", hr, (void*)orig);
    if (hr < 0 || !orig) {
        if (g_D7Emu) {
            SmdLog("DirectDraw7 creation failed (hr=%08X); switching to Direct3D9 emulation", (unsigned)hr);
            D7E_SaveFallbackAPI();
            return D7E_CreateDD7(out);
        }
        return hr;
    }
        IUnknown *d3d7 = NULL;
        HRESULT d3dHr = IDirectDraw7_QueryInterface(orig, &k_IID_IDirect3D7, (void**)&d3d7);
        if (d3dHr >= 0 && d3d7) {
            IUnknown_Release(d3d7);
            SmdLog("Direct3D7 probe: available on default adapter");
        } else {
            SmdLog("Direct3D7 probe failed on default adapter (hr=%08X); checking other adapters", (unsigned)d3dHr);
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
                        SmdLog("Direct3D7 probe: available on enumerated adapter %u", i);
                        orig = adapter;
                        hr = adapterHr;
                        break;
                    }
                    IDirectDraw7_Release(adapter);
                    SmdLog("Direct3D7 probe failed on enumerated adapter %u (hr=%08X)", i, (unsigned)d3dHr);
                }
                if (!orig) {
                    SmdLog("No enumerated adapter exposes Direct3D7");
                }
            }
            if (orig) goto ddraw_ready;
            if (g_D7Emu) {
                SmdLog("No usable system Direct3D7 interface (hr=%08X); switching to Direct3D9 emulation",
                    (unsigned)d3dHr);
                D7E_SaveFallbackAPI();
                return D7E_CreateDD7(out);
            }
            return d3dHr;
        }
ddraw_ready:
    IDirectDraw7 *wrapped = WrapDD7(orig);
    if (wrapped != orig) {
        SmdLog("DD7 wrapped ok");
        IDirectDraw7_Release(orig);
    }
    *out = wrapped;
    if (g_ApiCensus && wrapped != orig)
        CensusHookVtbl((void**)&g_dd7Vtbl, CEN_DD7, 30, 0);
    return hr;
}

#if defined(_MSC_VER)
#define SMD_DLL_EXPORT
#else
#define SMD_DLL_EXPORT __declspec(dllexport)
#endif

SMD_DLL_EXPORT HRESULT WINAPI DirectDrawCreate(GUID *g, LPDIRECTDRAW *out, IUnknown *u) {
    LoadReal();
    if (!p_DirectDrawCreate) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDC)p_DirectDrawCreate)(g, out, u);
    SmdLog("DirectDrawCreate -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER *c, IUnknown *u) {
    LoadReal();
    if (!p_DirectDrawCreateClipper) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDClip)p_DirectDrawCreateClipper)(flags, c, u);
    SmdLog("DirectDrawCreateClipper -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawCreateEx(GUID *g, LPVOID *o, REFIID i, IUnknown *u) {
    return Hook_DirectDrawCreateEx(g, o, i, u);
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, void *ctx) {
    LoadReal();
    if (!p_DirectDrawEnumerateA) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnum)p_DirectDrawEnumerateA)(cb, ctx);
    SmdLog("DirectDrawEnumerateA -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, void *ctx) {
    typedef HRESULT (WINAPI *PF_DDEnumW)(LPDDENUMCALLBACKW, LPVOID);
    LoadReal();
    if (!p_DirectDrawEnumerateW) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumW)p_DirectDrawEnumerateW)(cb, ctx);
    SmdLog("DirectDrawEnumerateW -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, void *ctx, DWORD flags) {
    LoadReal();
    if (!p_DirectDrawEnumerateExA) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumEx)p_DirectDrawEnumerateExA)(cb, ctx, flags);
    SmdLog("DirectDrawEnumerateExA -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, void *ctx, DWORD flags) {
    typedef HRESULT (WINAPI *PF_DDEnumExW)(LPDDENUMCALLBACKEXW, LPVOID, DWORD);
    LoadReal();
    if (!p_DirectDrawEnumerateExW) return DDERR_GENERIC;
    HRESULT hr = ((PF_DDEnumExW)p_DirectDrawEnumerateExW)(cb, ctx, flags);
    SmdLog("DirectDrawEnumerateExW -> hr=%08X", hr);
    return hr;
}
SMD_DLL_EXPORT HRESULT WINAPI DllCanUnloadNow(void) {
    LoadReal();
    if (!p_DllCanUnloadNow) return S_FALSE;
    return ((PF_FPOCH)p_DllCanUnloadNow)();
}
SMD_DLL_EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID cls, REFIID iid, void **out) {
    typedef HRESULT (WINAPI *PF_DGCO)(REFCLSID, REFIID, void**);
    LoadReal();
    if (!p_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return ((PF_DGCO)p_DllGetClassObject)(cls, iid, out);
}
SMD_DLL_EXPORT HRESULT WINAPI DllRegisterServer(void) {
    LoadReal();
    if (!p_DllRegisterServer) return E_FAIL;
    return ((PF_FPOCH)p_DllRegisterServer)();
}
SMD_DLL_EXPORT HRESULT WINAPI DllUnregisterServer(void) {
    LoadReal();
    if (!p_DllUnregisterServer) return E_FAIL;
    return ((PF_FPOCH)p_DllUnregisterServer)();
}

