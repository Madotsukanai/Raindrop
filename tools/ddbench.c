/* DirectDraw API-level bench: Lock+fill+Unlock and Blt on a 640x480 16bpp
 * offscreen surface. Run the exe next to our ddraw.dll to bench the shim,
 * or from a directory without it to bench the system (wine builtin) ddraw. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

static HWND mkwin(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ddbench";
    RegisterClassA(&wc);
    return CreateWindowExA(0, "ddbench", "dd", WS_POPUP, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
}

static double qpf;
static LARGE_INTEGER t0;
static void tick(void) { QueryPerformanceCounter(&t0); }
static double tock(void) { LARGE_INTEGER e; QueryPerformanceCounter(&e); return (e.QuadPart - t0.QuadPart) / qpf; }

#define W 640
#define H 480
#define N 1000
#define REPS 3
static double best = 1e9;
static double rep(void){ double t = tock(); if (t < best) best = t; return t; }

static void fill(DDSURFACEDESC *d) {
    unsigned i, n = (unsigned)(W * H);
    WORD *p = (WORD*)d->lpSurface;
    for (i = 0; i < n; i++) p[i] = (WORD)(i * 7 + (i >> 5));
}

int main(void) {
    LARGE_INTEGER f;
    LPDIRECTDRAW dd = NULL;
    LPDIRECTDRAWSURFACE s1 = NULL, s2 = NULL;
    DDSURFACEDESC d;
    int i;
    double tLock, tBlt, tPerFrame;
    HRESULT hr;
    typedef HRESULT (WINAPI *DDC)(LPVOID, LPDIRECTDRAW*, LPUNKNOWN);
    DDC create;
    HMODULE h = LoadLibraryA("ddraw.dll");
    char path[MAX_PATH];
    QueryPerformanceFrequency(&f); qpf = (double)f.QuadPart;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    printf("exe: %s\n", path);
    if (!h) { printf("load ddraw failed\n"); return 1; }
    create = (DDC)GetProcAddress(h, "DirectDrawCreate");
    hr = create(NULL, &dd, NULL);
    printf("DirectDrawCreate -> %08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !dd) return 1;
    hr = IDirectDraw_SetCooperativeLevel(dd, mkwin(), DDSCL_NORMAL);
    printf("SetCooperativeLevel -> %08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = W; d.dwHeight = H;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 16;
    d.ddpfPixelFormat.dwRBitMask = 0xF800;
    d.ddpfPixelFormat.dwGBitMask = 0x07E0;
    d.ddpfPixelFormat.dwBBitMask = 0x001F;
    hr = IDirectDraw_CreateSurface(dd, &d, &s1, NULL);
    printf("CreateSurface s1 -> %08lx\n", (unsigned long)hr);
    hr = IDirectDraw_CreateSurface(dd, &d, &s2, NULL);
    printf("CreateSurface s2 -> %08lx\n", (unsigned long)hr);
    if (!s1 || !s2) return 1;

    /* Lock + CPU fill + Unlock */
    for (i = 0; i < 5; i++) {
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        IDirectDrawSurface_Lock(s1, NULL, &d, DDLOCK_WAIT, NULL);
        fill(&d);
        IDirectDrawSurface_Unlock(s1, NULL);
    }
    tLock = 1e9;
    for (i = 0; i < REPS; i++) {
        tick();
        for (i = 0; i < N; i++) {
            memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
            if (FAILED(IDirectDrawSurface_Lock(s1, NULL, &d, DDLOCK_WAIT, NULL))) break;
            fill(&d);
            IDirectDrawSurface_Unlock(s1, NULL);
        }
        double t = tock(); if (t < tLock) tLock = t;
    }

    /* Blt same-size offscreen -> offscreen */
    memset(&d, 0, sizeof(d));
    for (i = 0; i < 5; i++) IDirectDrawSurface_Blt(s2, NULL, s1, NULL, DDBLT_WAIT, NULL);
    tBlt = 1e9;
    for (i = 0; i < REPS; i++) {
        tick();
        for (i = 0; i < N; i++) IDirectDrawSurface_Blt(s2, NULL, s1, NULL, DDBLT_WAIT, NULL);
        double t = tock(); if (t < tBlt) tBlt = t;
    }

    printf("Lock+fill+Unlock : %7.1f us/frame (%.1f MB/s CPU write)\n", tLock / N * 1e6, (W * H * 2.0) / 1024 / 1024 / (tLock / N));
    printf("Blt 640x480 16bpp: %7.1f us/frame (%.1f MB/s)\n", tBlt / N * 1e6, (W * H * 2.0 * 2.0) / 1024 / 1024 / (tBlt / N));
    (void)tPerFrame;
    return 0;
}
