#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <mmsystem.h>
#include <stdio.h>

// Global states
static volatile int g_ShowStageMenu = 0;
static volatile int g_StageSelected = 0;
static volatile int g_PracticeMode = 0;   // 1 if started via C key menu (practice mode), 0 if normal game start
static volatile int g_ReturnToStageMenu = 0; // set when practice stage ends to return to stage menu
static volatile int g_CancelToTitle = 0; // set when player cancels back to title
static volatile int g_MenuCursor = 0;   // 0 to 4 (Stage index)
static volatile int g_ActiveColumn = 0; // 0=Stage, 1=Lives, 2=MEFA, 3=CR Stock, 4=CR Gauge
static volatile int g_LifeCursor = 2;   // 0 to 5 (default: 2 in reserve)
static volatile int g_SelectedLives = 2;
static volatile int g_MefaCursor = 2;   // 0 to 6 (default: 2 stocks = 200.0f)
static volatile int g_CrStockCursor = 1; // 0 to 3 (default: 1 stock)
static volatile int g_CrGaugeCursor = 0; // 0 to 4 (default: 0%)
static volatile DWORD g_SelectedMefaDword = 0x43480000; // 200.0f (2 stocks)
static volatile DWORD g_SelectedCrDword = 0x42c80000;   // 100.0f (1 stock, 0%)
static volatile int g_PendingStageInitStats = 0;
static volatile int g_StageInitRenderFrames = 0;

static void ResetStageScore(void);
static DWORD g_LastResetTargetScene = 0; // kept for logging only
static volatile long g_PendingReset = 0;  // set whenever a new stage warp is requested (use InterlockedExchange)

static const DWORD g_ReturnAddr = 0x00440ffe;
static const DWORD g_EndSceneRetAddr = 0x00467273;
static const DWORD g_LoadStartRetAddr  = 0x004410a3; // continue: call 0x43f960
static const DWORD g_LoadStartSkipAddr = 0x004410ba; // skip: past the whole loading block

static HINSTANCE g_hOurDll = NULL;

typedef HRESULT (WINAPI *PFN_D3DXCreateFontA)(
    LPDIRECT3DDEVICE9 pDevice,
    INT Height,
    UINT Width,
    UINT Weight,
    UINT MipLevels,
    BOOL Italic,
    DWORD CharSet,
    DWORD OutputPrecision,
    DWORD Quality,
    DWORD PitchAndFamily,
    LPCSTR pFaceName,
    LPD3DXFONT *ppFont
);

static ID3DXFont *g_pFont = NULL;
static ID3DXFont *g_pStageFont = NULL;
static ID3DXFont *g_pTitleFont = NULL;
static ID3DXFont *g_pFooterFont = NULL;
static DWORD g_CurrentFontWidth = 0;

static void LogMessage(const char *format, ...) {
    char buf[512];
    va_list va;
    va_start(va, format);
    vsnprintf(buf, sizeof(buf), format, va);
    va_end(va);

    OutputDebugStringA(buf);

    FILE *fp = fopen("stage_select.log", "a");
    if (fp) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(fp, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(fp);
    }
}

// 2D Vertex for background box rendering
struct Vertex2D {
    float x, y, z, rhw;
    DWORD color;
};

static void DrawSolidRect(IDirect3DDevice9 *pDevice, float x, float y, float w, float h, DWORD color) {
    struct Vertex2D verts[4] = {
        { x,     y,     0.0f, 1.0f, color },
        { x + w, y,     0.0f, 1.0f, color },
        { x,     y + h, 0.0f, 1.0f, color },
        { x + w, y + h, 0.0f, 1.0f, color }
    };

    pDevice->lpVtbl->SetFVF(pDevice, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    pDevice->lpVtbl->DrawPrimitiveUP(pDevice, D3DPT_TRIANGLESTRIP, 2, verts, sizeof(struct Vertex2D));
}

static void EnsureFont(IDirect3DDevice9 *pDevice, DWORD screenW) {
    if (g_pFont && g_CurrentFontWidth != screenW) {
        if (g_pFont) { g_pFont->lpVtbl->Release(g_pFont); g_pFont = NULL; }
        if (g_pStageFont) { g_pStageFont->lpVtbl->Release(g_pStageFont); g_pStageFont = NULL; }
        if (g_pTitleFont) { g_pTitleFont->lpVtbl->Release(g_pTitleFont); g_pTitleFont = NULL; }
        if (g_pFooterFont) { g_pFooterFont->lpVtbl->Release(g_pFooterFont); g_pFooterFont = NULL; }
    }
    if (!g_pFont) {
        HMODULE hD3DX = GetModuleHandleA("d3dx9_42.dll");
        if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_42.dll");
        if (hD3DX) {
            PFN_D3DXCreateFontA pfnCreateFont = (PFN_D3DXCreateFontA)GetProcAddress(hD3DX, "D3DXCreateFontA");
            if (pfnCreateFont) {
                int itemH = (screenW >= 1000) ? 22 : 14;
                int stageH = (screenW >= 1000) ? 20 : 13;
                int titleH = (screenW >= 1000) ? 26 : 16;
                int footerH = (screenW >= 1000) ? 18 : 12;

                pfnCreateFont(pDevice, itemH, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              "Arial", &g_pFont);
                pfnCreateFont(pDevice, stageH, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              "Arial", &g_pStageFont);
                pfnCreateFont(pDevice, titleH, 0, FW_HEAVY, 1, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              "Arial", &g_pTitleFont);
                pfnCreateFont(pDevice, footerH, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              "Arial", &g_pFooterFont);
                g_CurrentFontWidth = screenW;
                LogMessage("Fonts created for screenW=%lu: itemH=%d, stageH=%d, titleH=%d, footerH=%d",
                           screenW, itemH, stageH, titleH, footerH);
            }
        }
    }
}

static void DrawShadowText(ID3DXFont *pFont, const char *text, int x, int y, DWORD color) {
    if (!pFont) return;
    RECT rcShadow = { x + 2, y + 2, x + 1200, y + 100 };
    RECT rcText   = { x,     y,     x + 1200, y + 100 };
    pFont->lpVtbl->DrawTextA(pFont, NULL, text, -1, &rcShadow, DT_LEFT | DT_NOCLIP, 0xFF000000);
    pFont->lpVtbl->DrawTextA(pFont, NULL, text, -1, &rcText,   DT_LEFT | DT_NOCLIP, color);
}

static int IsKeyTriggered(int vk) {
    static BYTE prev[256] = {0};
    BYTE curr = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    int triggered = curr && !prev[vk];
    prev[vk] = curr;
    return triggered;
}

// Menu Items structure
typedef struct {
    int stage;      // 1 to 5
    const char *label;
} MenuItem;

#define MENU_ITEM_COUNT 5

static const MenuItem g_MenuItems[MENU_ITEM_COUNT] = {
    { 1, "Central 01 - The way to UTOPIA / Natural Landscape -" },
    { 2, "Central 02 - DATA Flow / Artificial Landscape -" },
    { 3, "Central 03 - Virtual-Labo-Factory / Virtual Reality-" },
    { 4, "Central 04 - Divergence layer of higher level data / Haze in dezert-" },
    { 5, "Central 05 - The Central Nervous System / M.R.S. Central Core-" },
};

typedef struct {
    int lives;
    const char *label;
} LifeItem;

#define LIFE_ITEM_COUNT 6

static const LifeItem g_LifeItems[LIFE_ITEM_COUNT] = {
    { 0, "0" },
    { 1, "1" },
    { 2, "2 (DEF)" },
    { 3, "3" },
    { 4, "4" },
    { 5, "5 (MAX)" },
};

static void UpdateSelectedLives(void) {
    g_SelectedLives = g_LifeItems[g_LifeCursor].lives;
}

typedef struct {
    int value;
    const char *label;
} MefaItem;

#define MEFA_ITEM_COUNT 7

static const MefaItem g_MefaItems[MEFA_ITEM_COUNT] = {
    { 0, "0" },
    { 1, "1" },
    { 2, "2 (DEF)" },
    { 3, "3" },
    { 4, "4" },
    { 5, "5" },
    { 6, "6 (MAX)" },
};

typedef struct {
    int stock;
    const char *label;
} CrStockItem;

#define CR_STOCK_ITEM_COUNT 4

static const CrStockItem g_CrStockItems[CR_STOCK_ITEM_COUNT] = {
    { 0, "0" },
    { 1, "1 (DEF)" },
    { 2, "2" },
    { 3, "3 (MAX)" },
};

typedef struct {
    float pct;
    const char *label;
} CrGaugeItem;

#define CR_GAUGE_ITEM_COUNT 5

static const CrGaugeItem g_CrGaugeItems[CR_GAUGE_ITEM_COUNT] = {
    { 0.0f,   "0% (DEF)" },
    { 25.0f,  "25%" },
    { 50.0f,  "50%" },
    { 75.0f,  "75%" },
    { 100.0f, "100%" },
};

static void UpdateSelectedMefa(void) {
    float mefaVal = (float)g_MefaItems[g_MefaCursor].value * 100.0f;
    if (mefaVal > 600.0f) mefaVal = 600.0f;
    union { float f; DWORD dw; } u;
    u.f = mefaVal;
    g_SelectedMefaDword = u.dw;
}

static void UpdateSelectedCr(void) {
    float total = (float)g_CrStockItems[g_CrStockCursor].stock * 100.0f + g_CrGaugeItems[g_CrGaugeCursor].pct;
    if (total > 300.0f) total = 300.0f;
    union { float f; DWORD dw; } u;
    u.f = total;
    g_SelectedCrDword = u.dw;
}

// ---- Sound Effect (SE) Functions for Menu ----
static void PlaySoundDirect(DWORD pSoundObj) {
    if (!pSoundObj || pSoundObj < 0x10000 || pSoundObj > 0x7fff0000) return;

    DWORD bufCount = *(DWORD*)(pSoundObj + 0x108);
    if (bufCount == 0 || bufCount > 16) return;

    DWORD bufIdx = *(DWORD*)(pSoundObj + 0x134);
    if (bufIdx >= bufCount) bufIdx = 0;

    DWORD pDSBuf = *(DWORD*)(pSoundObj + 0x10c + bufIdx * 4);
    if (!pDSBuf || pDSBuf < 0x10000 || pDSBuf > 0x7fff0000) return;

    DWORD *vtbl = *(DWORD**)pDSBuf;
    if (!vtbl || (DWORD)vtbl < 0x10000 || (DWORD)vtbl > 0x7fff0000) return;

    // IDirectSoundBuffer::SetCurrentPosition(0) - rewind to start
    typedef HRESULT (__stdcall *PFN_SetCurrentPosition)(DWORD pBuf, DWORD pos);
    PFN_SetCurrentPosition pfnSetPos = (PFN_SetCurrentPosition)vtbl[0x34 / 4];
    if (pfnSetPos) pfnSetPos(pDSBuf, 0);

    // IDirectSoundBuffer::Play(0, 0, 0) - play once, no looping
    typedef HRESULT (__stdcall *PFN_Play)(DWORD pBuf, DWORD res1, DWORD res2, DWORD flags);
    PFN_Play pfnPlay = (PFN_Play)vtbl[0x30 / 4];
    if (pfnPlay) pfnPlay(pDSBuf, 0, 0, 0);

    // Advance round-robin index
    DWORD totalSlots = *(DWORD*)(pSoundObj + 0x12c);
    if (totalSlots > 0 && totalSlots <= 16) {
        *(DWORD*)(pSoundObj + 0x134) = (bufIdx + 1) % totalSlots;
    }
}

// Call game's GetSoundObject (0x43bf40) and play via PlaySoundDirect
static void PlayGameSE(int seIndex) {
    DWORD pSoundObj = 0;

    __asm__ __volatile__(
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        "movl %%eax, %0\n\t"
        : "=r"(pSoundObj)
        : "r"((DWORD)seIndex), "r"((DWORD)0x43bf40)
        : "eax", "ecx", "edx", "memory"
    );

    if (pSoundObj) {
        PlaySoundDirect(pSoundObj);
        LogMessage("PlayGameSE(%d): played OK", seIndex);
    }
}

// Check if the game is currently loading or playing a replay, demo, or tutorial
static inline int IsReplayOrDemo(void) {
    DWORD replaySlot = *(volatile DWORD*)0x4d237c;
    DWORD gameMode   = *(volatile DWORD*)0x4d2378;

    // Mode 2 = Replay, Mode 3 = Tutorial / Demo
    // replaySlot != 0xFFFFFFFF (-1) indicates a replay/demo file is loaded
    if (replaySlot != 0xFFFFFFFF || gameMode == 2 || gameMode == 3) {
        return 1;
    }
    return 0;
}

// Hook for HSF Native SetLives (0x00462535) - Native 77 (Player lives at 0x5b8e08)
DWORD __cdecl HandleSetLives(DWORD origLives) {
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        LogMessage("SetLives (Native 77): intercepted script call (%lu) -> overriding to %d",
                   origLives, g_SelectedLives);
        return (DWORD)g_SelectedLives;
    }
    return origLives;
}

void __attribute__((naked)) Hook_SetLives(void) {
    __asm__ __volatile__(
        "pushl 0x8(%%ebp)\n\t"
        "call _HandleSetLives\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, 0x5b8e08\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        :
        :
    );
}

// Hook for HSF Native SetMefa (0x00462430) - Native 54 (M.E.F.A.2 float at 0x5b9770)
DWORD __cdecl HandleSetMefa(DWORD origDword) {
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        union { DWORD dw; float f; } uOrig, uSel;
        uOrig.dw = origDword;
        uSel.dw = g_SelectedMefaDword;
        LogMessage("SetMefa (Native 54): intercepted script call (%f, 0x%08X) -> overriding to selected MEFA=%f (0x%08X)",
                   uOrig.f, origDword, uSel.f, g_SelectedMefaDword);
        return g_SelectedMefaDword;
    }
    return origDword;
}

void __attribute__((naked)) Hook_SetMefa(void) {
    __asm__ __volatile__(
        "pushl 0x8(%%ebp)\n\t"
        "call _HandleSetMefa\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, 0x5b9770\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        :
        :
    );
}

// Hook for HSF Native SetCR (0x00462443) - Native 55 (Concept Reactor float at 0x5b976c)
DWORD __cdecl HandleSetCR(DWORD origDword) {
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        union { DWORD dw; float f; } uOrig, uSel;
        uOrig.dw = origDword;
        uSel.dw = g_SelectedCrDword;
        LogMessage("SetCR (Native 55): intercepted script call (%f, 0x%08X) -> overriding to selected CR=%f (0x%08X)",
                   uOrig.f, origDword, uSel.f, g_SelectedCrDword);
        return g_SelectedCrDword;
    }
    return origDword;
}

void __attribute__((naked)) Hook_SetCR(void) {
    __asm__ __volatile__(
        "pushl 0x8(%%ebp)\n\t"
        "call _HandleSetCR\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, 0x5b976c\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        :
        :
    );
}

void __cdecl OnRenderMenu(IDirect3DDevice9 *pDevice) {
    if (!g_ShowStageMenu) return;

    D3DVIEWPORT9 vp;
    if (FAILED(pDevice->lpVtbl->GetViewport(pDevice, &vp))) {
        vp.Width = 640;
        vp.Height = 480;
    }

    EnsureFont(pDevice, vp.Width);
    if (!g_pFont || !g_pTitleFont) return;

    // Save render states
    DWORD fvf, alphaBlend, srcBlend, destBlend, zEnable, lighting, cullMode;
    pDevice->lpVtbl->GetFVF(pDevice, &fvf);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_ALPHABLENDENABLE, &alphaBlend);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_SRCBLEND, &srcBlend);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_DESTBLEND, &destBlend);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_ZENABLE, &zEnable);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_LIGHTING, &lighting);
    pDevice->lpVtbl->GetRenderState(pDevice, D3DRS_CULLMODE, &cullMode);

    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ZENABLE, FALSE);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_LIGHTING, FALSE);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    pDevice->lpVtbl->SetTexture(pDevice, 0, NULL);

    // Layout configuration: 2-Row Layout
    // Row 1 (Top): STAGE SELECT (Full width)
    // Row 2 (Bottom): 4 parameter columns (LIVES, M.E.F.A.2, CR STOCK, CR GAUGE)
    // Footer: Navigation guide
    int isHD = (vp.Width >= 1000);

    float totalW, startX;
    float stageBoxH, paramBoxH, footerH, rowGap;
    float headerH, stageLineSpacing, paramLineSpacing;
    float colW, colGap;
    float stageHighlightH, paramHighlightH;

    if (isHD) {
        totalW = 1200.0f;
        stageBoxH = 200.0f;
        paramBoxH = 280.0f;
        footerH = 64.0f;
        rowGap = 10.0f;
        headerH = 34.0f;
        stageLineSpacing = 28.0f;
        paramLineSpacing = 32.0f;
        colGap = 16.0f;
        colW = (totalW - (3.0f * colGap)) / 4.0f; // (1200 - 48) / 4 = 288.0f
        stageHighlightH = 24.0f;
        paramHighlightH = 24.0f;
    } else {
        totalW = 616.0f;
        stageBoxH = 136.0f;
        paramBoxH = 176.0f;
        footerH = 46.0f;
        rowGap = 6.0f;
        headerH = 24.0f;
        stageLineSpacing = 20.0f;
        paramLineSpacing = 20.0f;
        colGap = 8.0f;
        colW = (totalW - (3.0f * colGap)) / 4.0f; // (616 - 24) / 4 = 148.0f
        stageHighlightH = 18.0f;
        paramHighlightH = 18.0f;
    }

    float totalH = stageBoxH + rowGap + paramBoxH + rowGap + footerH;
    startX = ((float)vp.Width - totalW) * 0.5f;
    if (startX < 8.0f) startX = 8.0f;

    float startY = ((float)vp.Height - totalH) * 0.5f;
    if (startY < 4.0f) startY = 4.0f;

    float stageBoxY = startY;
    float paramBoxY = stageBoxY + stageBoxH + rowGap;
    float footerY   = paramBoxY + paramBoxH + rowGap;

    float xLives   = startX;
    float xMefa    = xLives + colW + colGap;
    float xCrStock = xMefa + colW + colGap;
    float xCrGauge = xCrStock + colW + colGap;

    // Active/Inactive visual styles
    DWORD borderCol0 = (g_ActiveColumn == 0) ? 0xFF00BFFF : 0xFF335577;
    DWORD borderCol1 = (g_ActiveColumn == 1) ? 0xFF00BFFF : 0xFF335577;
    DWORD borderCol2 = (g_ActiveColumn == 2) ? 0xFF00BFFF : 0xFF335577;
    DWORD borderCol3 = (g_ActiveColumn == 3) ? 0xFF00BFFF : 0xFF335577;
    DWORD borderCol4 = (g_ActiveColumn == 4) ? 0xFF00BFFF : 0xFF335577;

    DWORD headerBg0  = (g_ActiveColumn == 0) ? 0x88004488 : 0x55112233;
    DWORD headerBg1  = (g_ActiveColumn == 1) ? 0x88004488 : 0x55112233;
    DWORD headerBg2  = (g_ActiveColumn == 2) ? 0x88004488 : 0x55112233;
    DWORD headerBg3  = (g_ActiveColumn == 3) ? 0x88004488 : 0x55112233;
    DWORD headerBg4  = (g_ActiveColumn == 4) ? 0x88004488 : 0x55112233;

    DWORD titleCol0  = (g_ActiveColumn == 0) ? 0xFFFFCC00 : 0xFF88AABB;
    DWORD titleCol1  = (g_ActiveColumn == 1) ? 0xFFFFCC00 : 0xFF88AABB;
    DWORD titleCol2  = (g_ActiveColumn == 2) ? 0xFFFFCC00 : 0xFF88AABB;
    DWORD titleCol3  = (g_ActiveColumn == 3) ? 0xFFFFCC00 : 0xFF88AABB;
    DWORD titleCol4  = (g_ActiveColumn == 4) ? 0xFFFFCC00 : 0xFF88AABB;

    // Top Panel: STAGE SELECT (Full width)
    DrawSolidRect(pDevice, startX - 2.0f, stageBoxY - 2.0f, totalW + 4.0f, stageBoxH + 4.0f, borderCol0);
    DrawSolidRect(pDevice, startX, stageBoxY, totalW, stageBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, startX + 4.0f, stageBoxY + 4.0f, totalW - 8.0f, headerH, headerBg0);
    DrawShadowText(g_pTitleFont, "STAGE SELECT", (int)startX + (isHD ? 14 : 10), (int)stageBoxY + (isHD ? 8 : 5), titleCol0);

    // Items for Column 0 (Stage)
    int stageStartY = (int)stageBoxY + (isHD ? 44 : 30);
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int itemY = stageStartY + (int)(i * stageLineSpacing);
        char buf[128];
        if (i == g_MenuCursor) {
            if (g_ActiveColumn == 0) {
                DrawSolidRect(pDevice, startX + 4.0f, (float)itemY - 1.0f, totalW - 8.0f, stageHighlightH, 0x660077CC);
                snprintf(buf, sizeof(buf), ">> %s", g_MenuItems[i].label);
                DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, 0xFFFFFF00);
            } else {
                DrawSolidRect(pDevice, startX + 4.0f, (float)itemY - 1.0f, totalW - 8.0f, stageHighlightH, 0x44005588);
                snprintf(buf, sizeof(buf), ">> %s", g_MenuItems[i].label);
                DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, 0xFF00FFCC);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_MenuItems[i].label);
            DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, (g_ActiveColumn == 0) ? 0xFFAAAAAA : 0xFF666666);
        }
    }

    // Bottom Row: 4 parameter columns
    int paramStartY = (int)paramBoxY + (isHD ? 44 : 30);

    // Column 1: LIVES
    DrawSolidRect(pDevice, xLives - 2.0f, paramBoxY - 2.0f, colW + 4.0f, paramBoxH + 4.0f, borderCol1);
    DrawSolidRect(pDevice, xLives, paramBoxY, colW, paramBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, xLives + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg1);
    DrawShadowText(g_pTitleFont, "LIVES", (int)xLives + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol1);

    for (int i = 0; i < LIFE_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_LifeCursor) {
            if (g_ActiveColumn == 1) {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x660077CC);
                snprintf(buf, sizeof(buf), ">> %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, 0xFFFFFF00);
            } else if (g_ActiveColumn > 1) {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x44005588);
                snprintf(buf, sizeof(buf), ">> %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, 0xFF00FFCC);
            } else {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_LifeItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, (g_ActiveColumn == 1) ? 0xFFAAAAAA : 0xFF555555);
        }
    }

    // Column 2: M.E.F.A.2
    DrawSolidRect(pDevice, xMefa - 2.0f, paramBoxY - 2.0f, colW + 4.0f, paramBoxH + 4.0f, borderCol2);
    DrawSolidRect(pDevice, xMefa, paramBoxY, colW, paramBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, xMefa + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg2);
    DrawShadowText(g_pTitleFont, "M.E.F.A.2", (int)xMefa + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol2);

    for (int i = 0; i < MEFA_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_MefaCursor) {
            if (g_ActiveColumn == 2) {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x660077CC);
                snprintf(buf, sizeof(buf), ">> %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, 0xFFFFFF00);
            } else if (g_ActiveColumn > 2) {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x44005588);
                snprintf(buf, sizeof(buf), ">> %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, 0xFF00FFCC);
            } else {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_MefaItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, (g_ActiveColumn == 2) ? 0xFFAAAAAA : 0xFF555555);
        }
    }

    // Column 3: CR STOCK
    DrawSolidRect(pDevice, xCrStock - 2.0f, paramBoxY - 2.0f, colW + 4.0f, paramBoxH + 4.0f, borderCol3);
    DrawSolidRect(pDevice, xCrStock, paramBoxY, colW, paramBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, xCrStock + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg3);
    DrawShadowText(g_pTitleFont, "CR STOCK", (int)xCrStock + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol3);

    for (int i = 0; i < CR_STOCK_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_CrStockCursor) {
            if (g_ActiveColumn == 3) {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x660077CC);
                snprintf(buf, sizeof(buf), ">> %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, 0xFFFFFF00);
            } else if (g_ActiveColumn > 3) {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x44005588);
                snprintf(buf, sizeof(buf), ">> %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, 0xFF00FFCC);
            } else {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_CrStockItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, (g_ActiveColumn == 3) ? 0xFFAAAAAA : 0xFF555555);
        }
    }

    // Column 4: CR GAUGE
    DrawSolidRect(pDevice, xCrGauge - 2.0f, paramBoxY - 2.0f, colW + 4.0f, paramBoxH + 4.0f, borderCol4);
    DrawSolidRect(pDevice, xCrGauge, paramBoxY, colW, paramBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, xCrGauge + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg4);
    DrawShadowText(g_pTitleFont, "CR GAUGE", (int)xCrGauge + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol4);

    for (int i = 0; i < CR_GAUGE_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_CrGaugeCursor) {
            if (g_ActiveColumn == 4) {
                DrawSolidRect(pDevice, xCrGauge + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x660077CC);
                snprintf(buf, sizeof(buf), ">> %s", g_CrGaugeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, 0xFFFFFF00);
            } else {
                DrawSolidRect(pDevice, xCrGauge + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_CrGaugeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_CrGaugeItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, (g_ActiveColumn == 4) ? 0xFFAAAAAA : 0xFF555555);
        }
    }

    // Footer panel
    DrawSolidRect(pDevice, startX - 2.0f, footerY - 2.0f, totalW + 4.0f, footerH + 4.0f, 0xFF335577);
    DrawSolidRect(pDevice, startX, footerY, totalW, footerH, 0xEE0D111A);

    const char *help1 = "";
    const char *help2 = "";
    if (g_ActiveColumn == 0) {
        help1 = "[UP/DOWN] Select Stage    [Z/ENTER/RIGHT] Next    [1-5] Quick Select";
        help2 = "[X/ESC] Return to Title";
    } else if (g_ActiveColumn == 1) {
        help1 = "[UP/DOWN] Select Lives    [Z/ENTER/RIGHT] Next    [0-5] Quick Select";
        help2 = "[X/ESC/LEFT] Back to Stage Select";
    } else if (g_ActiveColumn == 2) {
        help1 = "[UP/DOWN] Select MEFA     [Z/ENTER/RIGHT] Next    [0-6] Quick Select";
        help2 = "[X/ESC/LEFT] Back to Lives Select";
    } else if (g_ActiveColumn == 3) {
        help1 = "[UP/DOWN] Select Stock    [Z/ENTER/RIGHT] Next    [0-3] Quick Select";
        help2 = "[X/ESC/LEFT] Back to M.E.F.A.2 Select";
    } else if (g_ActiveColumn == 4) {
        help1 = "[UP/DOWN] Select Gauge    [Z/ENTER] START GAME    [0-4] Quick Select";
        help2 = "[X/ESC/LEFT] Back to CR Stock Select";
    }

    ID3DXFont *pFootFont = g_pFooterFont ? g_pFooterFont : g_pFont;
    DrawShadowText(pFootFont, help1, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 8 : 5), 0xFF88DDFF);
    DrawShadowText(pFootFont, help2, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 32 : 23), 0xFF6699BB);

    // Joypad input polling
    int trig_up = 0, trig_down = 0, trig_left = 0, trig_right = 0;
    int trig_btn1 = 0, trig_btn2 = 0;

    JOYINFOEX joy;
    joy.dwSize = sizeof(joy);
    joy.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx(0, &joy) == JOYERR_NOERROR) {
        static int prev_x = 0;
        static int prev_y = 0;
        static DWORD prev_buttons = 0;

        int curr_x = 0;
        int curr_y = 0;
        if (joy.dwXpos < 0x3000) curr_x = -1;
        else if (joy.dwXpos > 0xD000) curr_x = 1;

        if (joy.dwYpos < 0x3000) curr_y = -1;
        else if (joy.dwYpos > 0xD000) curr_y = 1;

        if (joy.dwPOV != JOY_POVCENTERED) {
            if (joy.dwPOV == JOY_POVFORWARD || joy.dwPOV == 4500 || joy.dwPOV == 31500) curr_y = -1;
            if (joy.dwPOV == JOY_POVBACKWARD || joy.dwPOV == 13500 || joy.dwPOV == 22500) curr_y = 1;
            if (joy.dwPOV == JOY_POVLEFT || joy.dwPOV == 22500 || joy.dwPOV == 31500) curr_x = -1;
            if (joy.dwPOV == JOY_POVRIGHT || joy.dwPOV == 4500 || joy.dwPOV == 13500) curr_x = 1;
        }

        if (curr_y == -1 && prev_y != -1) trig_up = 1;
        if (curr_y == 1 && prev_y != 1) trig_down = 1;
        if (curr_x == -1 && prev_x != -1) trig_left = 1;
        if (curr_x == 1 && prev_x != 1) trig_right = 1;
        prev_x = curr_x;
        prev_y = curr_y;

        if ((joy.dwButtons & 1) && !(prev_buttons & 1)) trig_btn1 = 1;
        if ((joy.dwButtons & 2) && !(prev_buttons & 2)) trig_btn2 = 1;
        prev_buttons = joy.dwButtons;
    }

    int do_up = IsKeyTriggered(VK_UP) || IsKeyTriggered('W') || trig_up;
    int do_down = IsKeyTriggered(VK_DOWN) || IsKeyTriggered('S') || trig_down;
    int do_left = IsKeyTriggered(VK_LEFT) || IsKeyTriggered('A') || trig_left;
    int do_right = IsKeyTriggered(VK_RIGHT) || IsKeyTriggered('D') || trig_right;
    int do_decide = IsKeyTriggered('Z') || IsKeyTriggered(VK_RETURN) || IsKeyTriggered(VK_SPACE) || trig_btn1;
    int do_cancel = IsKeyTriggered('X') || IsKeyTriggered(VK_ESCAPE) || trig_btn2;

    // Vertical navigation in active column
    if (do_up) {
        if (g_ActiveColumn == 0) {
            g_MenuCursor = (g_MenuCursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        } else if (g_ActiveColumn == 1) {
            g_LifeCursor = (g_LifeCursor + LIFE_ITEM_COUNT - 1) % LIFE_ITEM_COUNT;
            UpdateSelectedLives();
        } else if (g_ActiveColumn == 2) {
            g_MefaCursor = (g_MefaCursor + MEFA_ITEM_COUNT - 1) % MEFA_ITEM_COUNT;
            UpdateSelectedMefa();
        } else if (g_ActiveColumn == 3) {
            g_CrStockCursor = (g_CrStockCursor + CR_STOCK_ITEM_COUNT - 1) % CR_STOCK_ITEM_COUNT;
            UpdateSelectedCr();
        } else if (g_ActiveColumn == 4) {
            g_CrGaugeCursor = (g_CrGaugeCursor + CR_GAUGE_ITEM_COUNT - 1) % CR_GAUGE_ITEM_COUNT;
            UpdateSelectedCr();
        }
    }
    if (do_down) {
        if (g_ActiveColumn == 0) {
            g_MenuCursor = (g_MenuCursor + 1) % MENU_ITEM_COUNT;
        } else if (g_ActiveColumn == 1) {
            g_LifeCursor = (g_LifeCursor + 1) % LIFE_ITEM_COUNT;
            UpdateSelectedLives();
        } else if (g_ActiveColumn == 2) {
            g_MefaCursor = (g_MefaCursor + 1) % MEFA_ITEM_COUNT;
            UpdateSelectedMefa();
        } else if (g_ActiveColumn == 3) {
            g_CrStockCursor = (g_CrStockCursor + 1) % CR_STOCK_ITEM_COUNT;
            UpdateSelectedCr();
        } else if (g_ActiveColumn == 4) {
            g_CrGaugeCursor = (g_CrGaugeCursor + 1) % CR_GAUGE_ITEM_COUNT;
            UpdateSelectedCr();
        }
    }

    // Number keys quick jump
    if (g_ActiveColumn == 0) {
        for (int i = 0; i < MENU_ITEM_COUNT; i++) {
            if (IsKeyTriggered('1' + i)) g_MenuCursor = i;
        }
    } else if (g_ActiveColumn == 1) {
        for (int i = 0; i < LIFE_ITEM_COUNT; i++) {
            if (IsKeyTriggered('0' + i)) {
                g_LifeCursor = i;
                UpdateSelectedLives();
            }
        }
    } else if (g_ActiveColumn == 2) {
        for (int i = 0; i < MEFA_ITEM_COUNT; i++) {
            if (IsKeyTriggered('0' + i)) {
                g_MefaCursor = i;
                UpdateSelectedMefa();
            }
        }
    } else if (g_ActiveColumn == 3) {
        for (int i = 0; i < CR_STOCK_ITEM_COUNT; i++) {
            if (IsKeyTriggered('0' + i)) {
                g_CrStockCursor = i;
                UpdateSelectedCr();
            }
        }
    } else if (g_ActiveColumn == 4) {
        for (int i = 0; i < CR_GAUGE_ITEM_COUNT; i++) {
            if (IsKeyTriggered('0' + i)) {
                g_CrGaugeCursor = i;
                UpdateSelectedCr();
            }
        }
    }

    // Decide or Right navigation
    if (do_decide) {
        if (g_ActiveColumn == 0) {
            g_ActiveColumn = 1;
            PlayGameSE(1040); // Decide SE
            LogMessage("Stage confirmed: %d -> moving to Lives", g_MenuItems[g_MenuCursor].stage);
        } else if (g_ActiveColumn == 1) {
            UpdateSelectedLives();
            g_ActiveColumn = 2;
            PlayGameSE(1040); // Decide SE
            LogMessage("Lives confirmed: %d -> moving to MEFA", g_SelectedLives);
        } else if (g_ActiveColumn == 2) {
            UpdateSelectedMefa();
            g_ActiveColumn = 3;
            PlayGameSE(1040); // Decide SE
            LogMessage("MEFA confirmed: %d stocks (0x%08X) -> moving to CR Stock",
                       g_MefaItems[g_MefaCursor].value, g_SelectedMefaDword);
        } else if (g_ActiveColumn == 3) {
            UpdateSelectedCr();
            g_ActiveColumn = 4;
            PlayGameSE(1040); // Decide SE
            LogMessage("CR Stock confirmed: %d -> moving to CR Gauge", g_CrStockItems[g_CrStockCursor].stock);
        } else if (g_ActiveColumn == 4) {
            UpdateSelectedLives();
            UpdateSelectedMefa();
            UpdateSelectedCr();
            int stage = g_MenuItems[g_MenuCursor].stage;

            g_ShowStageMenu = 0;
            g_StageSelected = 1;
            g_PracticeMode = 1;
            g_ReturnToStageMenu = 0;
            g_ActiveColumn = 0;
            g_PendingStageInitStats = 1;
            g_StageInitRenderFrames = 0;
            InterlockedExchange(&g_PendingReset, 1);
            ResetStageScore();
            *(volatile DWORD*)0x5c0740 = 2 + stage;
            PlayGameSE(1040); // Decide SE
            LogMessage("All confirmed -> Starting Stage %d (Lives=%d, MEFA stocks=%d, dw=0x%08X, CR Stock=%d, CR Gauge=%s, CR Total=0x%08X)",
                       stage, g_SelectedLives, g_MefaItems[g_MefaCursor].value, g_SelectedMefaDword,
                       g_CrStockItems[g_CrStockCursor].stock,
                       g_CrGaugeItems[g_CrGaugeCursor].label, g_SelectedCrDword);
        }
    } else if (do_right) {
        if (g_ActiveColumn == 0) {
            g_ActiveColumn = 1;
            PlayGameSE(1040);
        } else if (g_ActiveColumn == 1) {
            UpdateSelectedLives();
            g_ActiveColumn = 2;
            PlayGameSE(1040);
        } else if (g_ActiveColumn == 2) {
            UpdateSelectedMefa();
            g_ActiveColumn = 3;
            PlayGameSE(1040);
        } else if (g_ActiveColumn == 3) {
            UpdateSelectedCr();
            g_ActiveColumn = 4;
            PlayGameSE(1040);
        }
    }

    // Cancel or Left navigation
    if (do_cancel) {
        if (g_ActiveColumn == 4) {
            g_ActiveColumn = 3;
            PlayGameSE(1048); // Cancel SE
        } else if (g_ActiveColumn == 3) {
            g_ActiveColumn = 2;
            PlayGameSE(1048); // Cancel SE
        } else if (g_ActiveColumn == 2) {
            g_ActiveColumn = 1;
            PlayGameSE(1048); // Cancel SE
        } else if (g_ActiveColumn == 1) {
            g_ActiveColumn = 0;
            PlayGameSE(1048); // Cancel SE
        } else if (g_ActiveColumn == 0) {
            g_ShowStageMenu = 0;
            g_StageSelected = 0;
            g_PracticeMode = 0;
            g_ReturnToStageMenu = 0;
            g_CancelToTitle = 1;
            g_ActiveColumn = 0;
            *(volatile DWORD*)0x5c0740 = 1; // Return to Title
            PlayGameSE(1048); // Cancel SE
            LogMessage("Stage Menu Cancel -> Return to Title");
        }
    } else if (do_left) {
        if (g_ActiveColumn == 4) {
            g_ActiveColumn = 3;
            PlayGameSE(1048);
        } else if (g_ActiveColumn == 3) {
            g_ActiveColumn = 2;
            PlayGameSE(1048);
        } else if (g_ActiveColumn == 2) {
            g_ActiveColumn = 1;
            PlayGameSE(1048);
        } else if (g_ActiveColumn == 1) {
            g_ActiveColumn = 0;
            PlayGameSE(1048);
        }
    }

    // Restore render states
    pDevice->lpVtbl->SetFVF(pDevice, fvf);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ALPHABLENDENABLE, alphaBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_SRCBLEND, srcBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_DESTBLEND, destBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ZENABLE, zEnable);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_LIGHTING, lighting);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_CULLMODE, cullMode);
}

void __cdecl OnRenderHook(IDirect3DDevice9 *pDevice) {
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        union { float f; DWORD dw; } uMefa, uCr;
        uMefa.dw = g_SelectedMefaDword;
        uCr.dw = g_SelectedCrDword;

        // Enforce the values in globals while stage is initializing
        *(volatile DWORD*)0x5b8e08 = (DWORD)g_SelectedLives; // Lives (0..5)
        *(volatile DWORD*)0x5b9770 = g_SelectedMefaDword;     // M.E.F.A.2 (float, 0..600)
        *(volatile DWORD*)0x5b976c = g_SelectedCrDword;       // Concept Reactor (float, 0..300)

        // Also enforce in GameState if allocated
        char *pGameState = *(char**)0x5ac9b0;
        if (pGameState && (DWORD)pGameState > 0x10000 && !IsBadReadPtr(pGameState, 0x200)) {
            for (int s = 1; s <= 6; s++) {
                *(DWORD*)(pGameState + s * 4 + 0x115) = (DWORD)g_SelectedLives; // stageLives
                *(DWORD*)(pGameState + s * 4 + 0x12d) = g_SelectedMefaDword;    // stageMefa
                *(DWORD*)(pGameState + s * 4 + 0x145) = g_SelectedCrDword;      // stageCr
            }
        }

        DWORD pPlayer = *(volatile DWORD*)0x5b9778;
        DWORD frameCount = *(volatile DWORD*)0x5abb78;
        DWORD currentScene = *(volatile DWORD*)0x5c073c;

        g_StageInitRenderFrames++;

        // Once in game scene (scene 2..7, 10), player object exists, and at least 30 frames elapsed
        // Or if 120 render frames have elapsed in the game scene (safety fallback)
        if ((currentScene >= 2 && currentScene <= 7) || currentScene == 10) {
            if ((pPlayer != 0 && frameCount >= 30) || g_StageInitRenderFrames >= 120) {
                g_PendingStageInitStats = 0;
                g_StageInitRenderFrames = 0;
                LogMessage("Stage startup stats finalized: Lives=%d, MEFA=%f (dw=0x%08X), CR=%f (dw=0x%08X) (frameCount=%lu)",
                           g_SelectedLives, uMefa.f, uMefa.dw, uCr.f, uCr.dw, frameCount);
            }
        }
    }

    DWORD currentScene = *(volatile DWORD*)0x5c073c;
    if (g_ReturnToStageMenu && currentScene <= 2) {
        g_ReturnToStageMenu = 0;
        g_ShowStageMenu = 1;
        g_ActiveColumn = 0;
        g_StageSelected = 0;
        g_PracticeMode = 1;
        int targetStage = g_MenuItems[g_MenuCursor].stage;
        *(volatile DWORD*)0x5c0740 = 2 + targetStage;
        LogMessage("Practice stage finished -> Title/Menu reached (scene=%lu), showing Stage Menu for stage %d",
                   currentScene, targetStage);
    }

    if (g_ShowStageMenu) {
        OnRenderMenu(pDevice);
    }
}

void __attribute__((naked)) Hook_EndScene(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl 32(%%esp), %%eax\n\t" // pDevice was pushed at 0x0046726c
        "pushl %%eax\n\t"
        "call _OnRenderHook\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        // Original EndScene call
        "movl (%%esp), %%ecx\n\t"
        "movl (%%ecx), %%eax\n\t"
        "call *0xa8(%%eax)\n\t"
        "jmp *%0\n\t"
        :
        : "m"(g_EndSceneRetAddr)
    );
}

// Called from Hook_SceneCheck (0x440ff8): reads [0x5c0740] every frame.
void __cdecl HandleSceneCheck(DWORD *pEdx) {
    DWORD nextScene = *pEdx;

    if (nextScene <= 2) {
        g_StageSelected = 0;
        if (!g_ReturnToStageMenu) {
            g_PracticeMode = 0; // reset practice mode when returning to Title or Menu without practice return
        }
        g_LastResetTargetScene = 0;
        g_PendingStageInitStats = 0;
        g_StageInitRenderFrames = 0;
        InterlockedExchange(&g_PendingReset, 0);
    }
}

// Called from Hook_Trans: fires at the top of the scene-transition function (0x440dc0)
// before any loading or scene change happens.
// Returns the value to put in eax (= [0x5c073c]).

// Retrieve GameState pointer for the currently running replay
static char *GetReplayGameState(void) {
    char *pInputMgr = *(char**)0x5ac9b4;
    if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 0x20)) {
        if (*(DWORD*)(pInputMgr + 4) == 4) { // CReplayInput type
            char *pReplayGS = *(char**)(pInputMgr + 8);
            if (pReplayGS && (DWORD)pReplayGS > 0x10000 && !IsBadReadPtr(pReplayGS, 0x200)) {
                return pReplayGS;
            }
        }
    }
    char *pGS = *(char**)0x5ac9b0;
    if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x200)) {
        return pGS;
    }
    return NULL;
}

// Check if the loaded replay contains recording data for the given stage (1 to 5)
static int ReplayHasStage(int stage) {
    if (stage < 1 || stage > 5) return 0;

    char *pReplayGS = GetReplayGameState();
    if (!pReplayGS) {
        LogMessage("ReplayHasStage(%d): GameState not found", stage);
        return 0;
    }

    // [pGameState + stage + 0x52] is the stage visited / recorded flag (1 = recorded)
    BYTE visited = *(BYTE*)(pReplayGS + stage + 0x52);
    DWORD startFrame = *(DWORD*)(pReplayGS + stage * 4 + 0x15d);

    LogMessage("ReplayHasStage(%d): visited=%u, startFrame=%lu", stage, visited, startFrame);

    if (stage == 1) {
        // Stage 1 is always present in any valid replay
        return 1;
    }
    return (visited != 0 && startFrame > 0);
}

// If we want to suppress the transition, return [0x5c0740] so the equality check passes
// and the function skips all transition logic.
DWORD __cdecl HandleTrans(void) {
    DWORD prevScene = *(volatile DWORD*)0x5c073c;
    DWORD nextScene = *(volatile DWORD*)0x5c0740;

    // Allow cancel-to-title transition through unconditionally.
    // Return a value != nextScene so the cmp eax,[0x5c0740] check fails
    // and the transition logic actually runs.
    if (g_CancelToTitle) {
        g_CancelToTitle = 0;
        g_PracticeMode = 0;
        g_ReturnToStageMenu = 0;
        LogMessage("Trans: cancel to title allowed (prev=%lu next=%lu)", prevScene, nextScene);
        return nextScene - 1; // guaranteed != nextScene, triggers the transition
    }

    // Do NOT show stage select menu when playing back a replay or watching a demo/tutorial!
    if (IsReplayOrDemo()) {
        LogMessage("Trans: Replay/Demo playback detected (replaySlot=%ld, mode=%lu) -> passing through without menu",
                   (long)*(volatile DWORD*)0x4d237c, *(volatile DWORD*)0x4d2378);
        return prevScene;
    }

    // Practice Mode: when a stage finishes (clear, game over, quit to title, etc.), redirect transition to Title
    // and flag g_ReturnToStageMenu so the Stage Menu re-opens upon arrival.
    // Note: In RefRain, in-game gameplay scene is 10. Transitions out of scene 10 signify stage end.
    if (((prevScene >= 3 && prevScene <= 7) || prevScene == 10) &&
        prevScene != nextScene && nextScene != 10 &&
        g_PracticeMode && g_PendingReset == 0) {
        LogMessage("Practice stage end (prev=%lu next=%lu) -> redirecting to Title", prevScene, nextScene);
        g_ReturnToStageMenu = 1;
        g_StageSelected = 0;
        *(volatile DWORD*)0x5c0740 = 1; // redirect scene transition to Title
        return prevScene;
    }

    // Practice Mode: once arrived at Title/Menu after stage end, open the Stage Menu
    if (prevScene <= 2 && g_ReturnToStageMenu) {
        g_ReturnToStageMenu = 0;
        g_ShowStageMenu = 1;
        g_ActiveColumn = 0;
        g_StageSelected = 0;
        g_PracticeMode = 1;
        int targetStage = g_MenuItems[g_MenuCursor].stage;
        *(volatile DWORD*)0x5c0740 = 2 + targetStage;
        LogMessage("Arrived at Title after practice stage -> opening Stage Menu for stage %d", targetStage);
        return 2 + targetStage;
    }

    // When the game is about to enter a stage scene (Stages 1-5 = scenes 3-7) and no stage has been selected yet
    // Note: Scene 8 is Stage 6 (Tutorial), which is excluded from the stage select menu
    if (nextScene >= 3 && nextScene <= 7 && !g_StageSelected) {
        if (g_ShowStageMenu) {
            // Stage select menu is already open; keep suppressing transition
            return nextScene;
        }

        // Only show stage select menu if 'C' was held when starting the game
        int isC = (GetAsyncKeyState('C') & 0x8000) != 0;
        if (isC) {
            g_ShowStageMenu = 1;
            g_ActiveColumn = 0;
            g_PracticeMode = 1;
            LogMessage("Trans intercepted with C key: prev=%lu next=%lu -> showing stage select menu (practice mode)", prevScene, nextScene);
            // Return nextScene so eax == [0x5c0740] -> equality check passes -> skip transition
            return nextScene;
        } else {
            // Normal game start without C key: pass through directly to original game start
            g_StageSelected = 1;
            g_PracticeMode = 0; // Not practice mode: hotkeys disabled!
            LogMessage("Game started normally (without C key): prev=%lu next=%lu -> starting stage directly (hotkeys disabled)", prevScene, nextScene);
            return prevScene;
        }
    }

    // Run this on the game's transition thread, immediately before the
    // original transition code.  The previous implementation did this from
    // HotkeyThread, racing the score/FPS update code and losing the reset.
    if (nextScene >= 3 && nextScene <= 7 && g_StageSelected && InterlockedExchange(&g_PendingReset, 0)) {
        ResetStageScore();
        g_LastResetTargetScene = nextScene; // kept for logging only
    }

    // Allow the transition
    return prevScene;
}

// Hook at 0x00440deb (5 bytes: A1 3C 07 5C 00  =  mov eax,[0x5c073c])
// If we want to suppress, we return [0x5c0740] as eax so cmp eax,[0x5c0740] is equal
// and the je at 0x440df6 skips all transition logic.
void __attribute__((naked)) Hook_Trans(void) {
    __asm__ __volatile__(
        "pushal\n\t"               // pushes 8 regs (32 bytes); ret addr is at esp+32
        "call _HandleTrans\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"                // restores all regs including eax = HandleTrans result
        "ret\n\t"                  // return to caller (0x440df0), eax is set
        :
        :
    );
}

// Hook at 0x00440ff8 (6 bytes: 8B 15 40 07 5C 00 -> mov edx, [0x5c0740])
void __attribute__((naked)) Hook_SceneCheck(void) {
    __asm__ __volatile__(
        "movl 0x5c0740, %%edx\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%eax\n\t"
        "leal 8(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "call _HandleSceneCheck\n\t"
        "addl $4, %%esp\n\t"
        "popl %%eax\n\t"
        "popl %%ecx\n\t"
        "popl %%edx\n\t"
        "jmp *%0\n\t"
        :
        : "m"(g_ReturnAddr)
    );
}

// Called from Hook_LoadStart: fires when the game is about to start loading a stage.
// If we haven't selected a stage yet, show the menu and suppress the load.
// Returns 1 if suppressed, 0 if allowed through.
int __cdecl HandleLoadStart(void) {
    if (IsReplayOrDemo()) {
        return 0; // allow stage load for replay/demo without menu
    }

    if (g_ReturnToStageMenu) {
        return 0; // allow transition to Title
    }

    if (!g_StageSelected) {
        if (g_ShowStageMenu) {
            return 1; // suppressed while menu is open
        }
        int isC = (GetAsyncKeyState('C') & 0x8000) != 0;
        if (isC) {
            g_ShowStageMenu = 1;
            g_ActiveColumn = 0;
            g_PracticeMode = 1;
            LogMessage("LoadStart intercepted with C key -> showing stage select menu (practice mode)");
            return 1; // suppressed
        } else {
            g_StageSelected = 1;
            g_PracticeMode = 0;
            LogMessage("LoadStart passed through normally (without C key, hotkeys disabled)");
            return 0; // allow
        }
    }
    // Fallback: if transition hook was bypassed, reset score here too
    if (InterlockedExchange(&g_PendingReset, 0)) {
        ResetStageScore();
    }
    return 0; // allow
}

// Hook at 0x00441097 (7 bytes: 32 C9 C7 05 DC C4 5A 00)
//   xor cl, cl
//   movl $1, [0x5ac4dc]   <- start of loading sequence
// If HandleLoadStart returns non-zero, skip the entire loading block.
// Otherwise run the original bytes and fall through.
void __attribute__((naked)) Hook_LoadStart(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "call _HandleLoadStart\n\t"
        "testl %%eax, %%eax\n\t"
        "popal\n\t"
        "jnz 1f\n\t"
        // not suppressed: run original bytes then continue
        "xorl %%ecx, %%ecx\n\t"
        "movl $1, 0x5ac4dc\n\t"
        "jmp *%0\n\t"
        "1:\n\t"
        "jmp *%1\n\t"
        :
        : "m"(g_LoadStartRetAddr), "m"(g_LoadStartSkipAddr)
    );
}

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
static PFN_Direct3DCreate9 orig_Direct3DCreate9 = NULL;

__declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    if (!orig_Direct3DCreate9) {
        char sysPath[MAX_PATH];
        UINT len = GetSystemDirectoryA(sysPath, MAX_PATH);
        if (len > 0) {
            strcat(sysPath, "\\d3d9.dll");
        } else {
            strcpy(sysPath, "C:\\Windows\\System32\\d3d9.dll");
        }

        HMODULE hOrig = LoadLibraryA(sysPath);
        if (hOrig == g_hOurDll) {
            LogMessage("[Error] LoadLibrary returned our own DLL instead of system d3d9.dll!");
            return NULL;
        }

        if (hOrig) {
            orig_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(hOrig, "Direct3DCreate9");
            LogMessage("Direct3DCreate9 proxy initialized from %s (proc: 0x%p)", sysPath, orig_Direct3DCreate9);
        } else {
            LogMessage("[Error] Failed to load system d3d9.dll from %s (Error: %lu)", sysPath, GetLastError());
        }
    }

    if (orig_Direct3DCreate9) {
        return orig_Direct3DCreate9(SDKVersion);
    }
    return NULL;
}

static void ResetStageScore(void) {
    UpdateSelectedLives();
    UpdateSelectedMefa();
    UpdateSelectedCr();

    union { float f; DWORD dw; } uMefa, uCr;
    uMefa.dw = g_SelectedMefaDword;
    uCr.dw = g_SelectedCrDword;

    // 1. Reset frame / FPS counters (mirrors 0x00441332)
    *(volatile DWORD*)0x5abb78 = 0;
    *(volatile DWORD*)0x5abb74 = 0;
    *(volatile DWORD*)0x5abb70 = 0x3c;

    // 2. Reset score & player status globals (mirrors Title scene start at 0x00441718)
    *(volatile DWORD*)0x5b8e20 = 0;                 // Score Low (32-bit)
    *(volatile DWORD*)0x5b8e24 = 0;                 // Score High (32-bit)
    *(volatile DWORD*)0x5b8e08 = (DWORD)g_SelectedLives; // Lives (0..5 in reserve)
    *(volatile DWORD*)0x5b8e1c = 0;                 // Score multiplier / rate
    *(volatile DWORD*)0x5b8e14 = 0;                 // Prisms
    *(volatile DWORD*)0x5b8e10 = 0;                 // Miss / continue count
    *(volatile DWORD*)0x5b8e18 = 0;
    *(volatile DWORD*)0x5b9770 = g_SelectedMefaDword;// M.E.F.A.2 (float, 0..600)
    *(volatile DWORD*)0x5b976c = g_SelectedCrDword;  // Concept Reactor (float, 0..300)
    *(volatile BYTE*)0x5b975d = 0;                  // Force PointTask to re-initialize

    // 3. Clear GameState stage records (0x5ac9b0) so 0x431de0 won't restore old scores
    char *pGameState = *(char**)0x5ac9b0;
    if (pGameState && (DWORD)pGameState > 0x10000) {
        for (int s = 1; s <= 6; s++) {
            *(BYTE*)(pGameState + s + 0x52) = 0;                      // Stage visited flag = 0
            *(DWORD*)(pGameState + s * 8 + 0x69) = 0;                 // Saved Score Low = 0
            *(DWORD*)(pGameState + s * 8 + 0x6d) = 0;                 // Saved Score High = 0
            *(DWORD*)(pGameState + s * 4 + 0x9d) = 0;                 // Saved multiplier = 0
            *(DWORD*)(pGameState + s * 4 + 0xcd) = 0;                 // Saved prisms = 0
            *(DWORD*)(pGameState + s * 4 + 0x115) = (DWORD)g_SelectedLives; // Saved lives
            *(DWORD*)(pGameState + s * 4 + 0x12d) = g_SelectedMefaDword; // Saved stageMefa
            *(DWORD*)(pGameState + s * 4 + 0x145) = g_SelectedCrDword;   // Saved stageCr
            *(DWORD*)(pGameState + s * 4 + 0x15d) = 0;
            *(DWORD*)(pGameState + s * 4 + 0x17c) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xb5) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xe5) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xfd) = 0;
        }
        *(DWORD*)(pGameState + 0x7a8) = 0; // Total play time = 0
    }

    LogMessage("ResetStageScore: Score and player stats reset (Lives=%d, MEFA=%f (dw=0x%08X), CR=%f (dw=0x%08X), GameState=0x%p)",
               g_SelectedLives, uMefa.f, uMefa.dw, uCr.f, uCr.dw, pGameState);
}

// In-game F1-F5 hotkeys thread (for practice stage warp anytime)
static DWORD WINAPI HotkeyThread(LPVOID param) {
    (void)param;
    int key_state[5] = {0};

    while (1) {
        Sleep(50);

        // Hotkeys are disabled during normal game start (only active in Practice Mode or Replay playback)
        if (!g_PracticeMode && !IsReplayOrDemo()) {
            memset(key_state, 0, sizeof(key_state));
            continue;
        }

        DWORD currentScene = *(volatile DWORD*)0x5c073c; // 0x5c073c = current scene

        // F1-F5 for Stages 1-5 (Stage 6 is Tutorial, excluded)
        for (int i = 0; i < 5; i++) {
            int is_down = (GetAsyncKeyState(VK_F1 + i) & 0x8000) != 0;
            if (is_down && !key_state[i]) {
                int stage = i + 1;
                // Allow warp during menu (scene 2) and gameplay (scene 10 or 3-7).
                // Block while our custom stage menu is showing.
                if (!g_ShowStageMenu && ((currentScene >= 2 && currentScene <= 7) || currentScene == 10)) {
                    if (IsReplayOrDemo()) {
                        // Replay mode: check if the replay contains this stage
                        if (!ReplayHasStage(stage)) {
                            LogMessage("Hotkey F%d -> Replay does NOT contain Stage %d; skipping warp", stage, stage);
                        } else {
                            LogMessage("Hotkey F%d -> Replay seeking to Stage %d", stage, stage);
                            g_StageSelected = 1;
                            // Do NOT call ResetStageScore() or set g_PendingReset in replay mode!
                            // CReplayInput::SeekToStage will restore the replay's recorded score and inputs.
                            *(volatile DWORD*)0x5c0740 = 2 + stage;
                            PlayGameSE(1040); // Decide SE
                        }
                    } else if (g_PracticeMode) {
                        // Practice mode: warp and reset score/stats
                        LogMessage("Hotkey F%d -> Warping to Stage %d (currentScene=%lu)", stage, stage, currentScene);
                        g_MenuCursor = stage - 1;
                        g_StageSelected = 1;
                        g_PendingStageInitStats = 1;
                        g_StageInitRenderFrames = 0;
                        InterlockedExchange(&g_PendingReset, 1);
                        ResetStageScore();
                        *(volatile DWORD*)0x5c0740 = 2 + stage;
                        PlayGameSE(1040); // Decide SE
                    }
                }
            }
            key_state[i] = is_down;
        }
    }
    return 0;
}

static void InstallHooks(void) {
    DWORD oldProtect;

    // 1. Trans Hook at 0x00440deb (5 bytes: A1 3C 07 5C 00)
    //    Fires at the top of the scene-transition function, before any loading starts.
    void *transAddr = (void*)0x00440deb;
    unsigned char expectedTrans[5] = { 0xA1, 0x3C, 0x07, 0x5C, 0x00 };
    if (memcmp(transAddr, expectedTrans, 5) == 0) {
        if (VirtualProtect(transAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE8; // CALL rel32
            DWORD relOffset = (DWORD)Hook_Trans - ((DWORD)transAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(transAddr, patch, 5);
            VirtualProtect(transAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), transAddr, 5);
            LogMessage("Trans hook installed successfully at 0x%08X", transAddr);
        }
    } else {
        LogMessage("[Error] Trans signature at 0x%08X did not match!", transAddr);
    }

    // 2. LoadStart Hook at 0x00441097 (7 bytes: 32 C9 C7 05 DC C4 5A 00)
    void *loadStartAddr = (void*)0x00441097;
    unsigned char expectedLoadStart[7] = { 0x32, 0xC9, 0xC7, 0x05, 0xDC, 0xC4, 0x5A };
    if (memcmp(loadStartAddr, expectedLoadStart, 7) == 0) {
        if (VirtualProtect(loadStartAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[7];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_LoadStart - ((DWORD)loadStartAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90; // NOP
            patch[6] = 0x90; // NOP
            memcpy(loadStartAddr, patch, 7);
            VirtualProtect(loadStartAddr, 7, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), loadStartAddr, 7);
            LogMessage("LoadStart hook installed successfully at 0x%08X", loadStartAddr);
        }
    } else {
        LogMessage("[Error] LoadStart signature at 0x%08X did not match!", loadStartAddr);
    }

    // 3. Scene Check Hook at 0x00440ff8 (6 bytes: 8B 15 40 07 5C 00)
    void *sceneAddr = (void*)0x00440ff8;
    unsigned char expectedScene[6] = { 0x8B, 0x15, 0x40, 0x07, 0x5C, 0x00 };
    if (memcmp(sceneAddr, expectedScene, 6) == 0) {
        if (VirtualProtect(sceneAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[6];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_SceneCheck - ((DWORD)sceneAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90; // NOP
            memcpy(sceneAddr, patch, 6);
            VirtualProtect(sceneAddr, 6, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), sceneAddr, 6);
            LogMessage("SceneCheck hook installed successfully at 0x%08X", sceneAddr);
        }
    } else {
        LogMessage("[Error] Memory signature at 0x%08X did not match!", sceneAddr);
    }

    // 4. EndScene Hook at 0x0046726d (6 bytes: FF 90 A8 00 00 00)
    void *endSceneAddr = (void*)0x0046726d;
    unsigned char expectedEndScene[6] = { 0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00 };
    if (memcmp(endSceneAddr, expectedEndScene, 6) == 0) {
        if (VirtualProtect(endSceneAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[6];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_EndScene - ((DWORD)endSceneAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90; // NOP
            memcpy(endSceneAddr, patch, 6);
            VirtualProtect(endSceneAddr, 6, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), endSceneAddr, 6);
            LogMessage("EndScene hook installed successfully at 0x%08X", endSceneAddr);
        }
    } else {
        LogMessage("[Error] EndScene signature at 0x%08X did not match!", endSceneAddr);
    }

    // 5. SetMefa Hook at 0x00462430 (13 bytes: F3 0F 10 45 08 F3 0F 11 05 70 97 5B 00) - Native 54
    void *setMefaAddr = (void*)0x00462430;
    unsigned char expectedSetMefa[13] = {
        0xF3, 0x0F, 0x10, 0x45, 0x08,
        0xF3, 0x0F, 0x11, 0x05, 0x70, 0x97, 0x5B, 0x00
    };
    if (memcmp(setMefaAddr, expectedSetMefa, 13) == 0) {
        if (VirtualProtect(setMefaAddr, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[13];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_SetMefa - ((DWORD)setMefaAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memset(&patch[5], 0x90, 8); // 8 NOPs
            memcpy(setMefaAddr, patch, 13);
            VirtualProtect(setMefaAddr, 13, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setMefaAddr, 13);
            LogMessage("SetMefa hook installed successfully at 0x%08X", setMefaAddr);
        }
    } else {
        LogMessage("[Error] SetMefa signature at 0x%08X did not match!", setMefaAddr);
    }

    // 6. SetCR Hook at 0x00462443 (13 bytes: F3 0F 10 45 08 F3 0F 11 05 6C 97 5B 00) - Native 55
    void *setCrAddr = (void*)0x00462443;
    unsigned char expectedSetCr[13] = {
        0xF3, 0x0F, 0x10, 0x45, 0x08,
        0xF3, 0x0F, 0x11, 0x05, 0x6C, 0x97, 0x5B, 0x00
    };
    if (memcmp(setCrAddr, expectedSetCr, 13) == 0) {
        if (VirtualProtect(setCrAddr, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[13];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_SetCR - ((DWORD)setCrAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memset(&patch[5], 0x90, 8); // 8 NOPs
            memcpy(setCrAddr, patch, 13);
            VirtualProtect(setCrAddr, 13, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setCrAddr, 13);
            LogMessage("SetCR hook installed successfully at 0x%08X", setCrAddr);
        }
    } else {
        LogMessage("[Error] SetCR signature at 0x%08X did not match!", setCrAddr);
    }

    // 7. SetLives Hook at 0x00462535 (8 bytes: 8B 45 08 A3 08 8E 5B 00) - Native 77
    void *setLivesAddr = (void*)0x00462535;
    unsigned char expectedSetLives[8] = {
        0x8B, 0x45, 0x08,
        0xA3, 0x08, 0x8E, 0x5B, 0x00
    };
    if (memcmp(setLivesAddr, expectedSetLives, 8) == 0) {
        if (VirtualProtect(setLivesAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[8];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_SetLives - ((DWORD)setLivesAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90; // NOP
            patch[6] = 0x90; // NOP
            patch[7] = 0x90; // NOP
            memcpy(setLivesAddr, patch, 8);
            VirtualProtect(setLivesAddr, 8, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setLivesAddr, 8);
            LogMessage("SetLives hook installed successfully at 0x%08X", setLivesAddr);
        }
    } else {
        LogMessage("[Error] SetLives signature at 0x%08X did not match!", setLivesAddr);
    }

    // Leave the script entry point alone: the stage-select logic only needs the scene
    // transition and load-start hooks. Hooking the script dispatcher is more invasive and
    // can destabilize stage loads when a menu selection triggers a new stage script.
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hOurDll = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        LogMessage("RefRain stage select mod loaded.");
        InstallHooks();
        CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
    }
    return TRUE;
}
