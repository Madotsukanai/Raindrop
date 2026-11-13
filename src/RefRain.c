#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <stdio.h>
#include "d3dx9_font_compat.h"

/* ---------------------------------------------------------------------------
 * Game version / address table.
 * The mod supports the disc build and the Steam build of RefRain 1.03.7.
 * Every address below is stored as the preferred (0x400000-based) VA for that
 * build; at load time DetectVersion() picks the table, then rebases every
 * entry by the actual image base delta (both exes are DYNAMIC_BASE, so the
 * image is not guaranteed to load at 0x400000).
 * ------------------------------------------------------------------------- */
typedef struct {
    /* game variables */
    DWORD Scene, SceneNext, ScenePend, Script;
    DWORD GameMode, ReplaySlot, GameModeNext, Var4d2370;
    DWORD FrameCnt;
    DWORD Unlock, LoadFlag, IsRefRain, Config, InputMgr;
    DWORD Lives, St10, St14, St18, St1c, St20, St24, Diff;
    DWORD In1, In2, In3, Obj;
    DWORD Var5b975d, Cr, Mefa, Char, Player;
    DWORD Tasks, GpuQueue;
    DWORD Frame1, Frame2, ViewIdx;
    DWORD pdRange[6][2];
    /* functions */
    DWORD fnSE, fnTick, fnLoadGlobal, fnSaveGlobal, fnSaveGlobalRet;
    DWORD fnGetGameInfoInt, fnGetGameInfoIntRet;
    DWORD fnSetGameInfoInt, fnSetGameInfoIntRet;
    DWORD fnIsKeyPressed, fnIsKeyTriggered;
    /* vtables / IAT */
    DWORD vtInput, vtJoyA, vtJoyB, vtTask;
    DWORD iatGetKeyboardState, iatXInputGetCaps, iatXInputGetState;
    /* hook sites and continuation addresses */
    DWORD TransSite;
    DWORD LoadStartSite, LoadStartRet, LoadStartSkip;
    DWORD SceneCheckSite, SceneCheckRet;
    DWORD EndSceneSite, EndSceneRet;
    DWORD PreTickSite, PreTickRet;
    DWORD SetMefaSite, SetCrSite, SetLivesSite, SetDiffSite;
    DWORD DispatchEpilogue;      /* steam: shared case epilogue; disc: 0 (inline epilogue) */
    DWORD RefRainCfgSite, RefRainCfgRet;
    DWORD GameInfoRefRainSite;
    DWORD SaveReplayHdrSite, SaveReplayHdrRet;
    DWORD FreadWrapSite, FwriteWrapSite;
    DWORD Getter19Fn, Getter19Entry;
    DWORD ViewIdxWriteSite;    /* mov [viewIdx], eax in the scene/view setter */
} GameAddrs;

typedef struct {
    BYTE Trans[5];
    BYTE LoadStart[7];
    BYTE SceneCheck[6];
    BYTE EndScene[6];
    BYTE SetMefa[13];
    BYTE SetCr[13];
    BYTE SetLives[8];
    BYTE SetDiff[8];
    BYTE RefRainCfg[9];
    BYTE SaveReplayHdr[5];
} VersionSigs;

static const GameAddrs ADDRS_DISC = {
    /* Scene, SceneNext, ScenePend, Script */
    0x005c073c, 0x005c0740, 0x005c0744, 0x005c0760,
    /* GameMode, ReplaySlot, GameModeNext, Var4d2370 */
    0x004d2378, 0x004d237c, 0x004d2380, 0x004d2370,
    /* FrameCnt */
    0x005abb78,
    /* Unlock, LoadFlag, IsRefRain, Config, InputMgr */
    0x005ac4d4, 0x005ac4dc, 0x005ac674, 0x005ac9b0, 0x005ac9b4,
    /* Lives, St10, St14, St18, St1c, St20, St24, Diff */
    0x005b8e08, 0x005b8e10, 0x005b8e14, 0x005b8e18, 0x005b8e1c, 0x005b8e20, 0x005b8e24, 0x005b8e28,
    /* In1, In2, In3, Obj */
    0x005b8e54, 0x005b8e58, 0x005b8e5c, 0x005b8e68,
    /* Var5b975d, Cr, Mefa, Char, Player */
    0x005b975d, 0x005b976c, 0x005b9770, 0x005b9774, 0x5b9778,
    /* Tasks, GpuQueue */
    0x005ba0a8, 0x004df338,
    /* Frame1, Frame2, ViewIdx */
    0x005abb70, 0x005abb74, 0x005abb80,
    /* pdRange (PauseDiag snapshot ranges) */
    { { 0x005abb60, 0x005abbc0 }, { 0x005b8e00, 0x005b8e70 }, { 0x005ac4c0, 0x005ac4f0 },
      { 0x005ac670, 0x005ac680 }, { 0x004d2370, 0x004d2380 }, { 0x005c0730, 0x005c0760 } },
    /* fnSE, fnTick, fnLoadGlobal, fnSaveGlobal, fnSaveGlobalRet */
    0x0043bf40, 0x0042a7c0, 0x0042a8e0, 0x0042c330, 0x0042c335,
    /* fnGetGameInfoInt, ret, fnSetGameInfoInt, ret */
    0x0044fb80, 0x0044fb85, 0x0044fca0, 0x0044fca5,
    /* fnIsKeyPressed, fnIsKeyTriggered */
    0x00431760, 0x004317a0,
    /* vtInput, vtJoyA, vtJoyB, vtTask */
    0x004b80c4, 0x004b83f8, 0x004b807c, 0x004b9a20,
    /* iatGetKeyboardState, iatXInputGetCaps, iatXInputGetState */
    0x0048c248, 0x0048c294, 0x0048c298,
    /* hook sites */
    0x00440deb,
    0x00441097, 0x004410a3, 0x004410ba,
    0x00440ff8, 0x00440ffe,
    0x0046726d, 0x00467273,
    0x0046710d, 0x00467112,
    0x00462430, 0x00462443, 0x00462535, 0x004625dc,
    0x00000000,
    0x00441229, 0x00441232,
    0x0043078e,
    0x00431d24, 0x00431d29,
    0x0046f33b, 0x0047038a,
    0x00462c49, 0x00462dfc,
    0x00441292
};

static const GameAddrs ADDRS_STEAM = {
    0x005c480c, 0x005c4810, 0x005c4814, 0x005c4830,
    0x004d6378, 0x004d637c, 0x004d6380, 0x004d6370,
    0x005afc48,
    0x005b05a4, 0x005b05ac, 0x005b0744, 0x005b0a80, 0x005b0a84,
    0x005bced8, 0x005bcee0, 0x005bcee4, 0x005bcee8, 0x005bceec, 0x005bcef0, 0x005bcef4, 0x005bcef8,
    0x005bcf24, 0x005bcf28, 0x005bcf2c, 0x005bcf38,
    0x005bd82d, 0x005bd83c, 0x005bd840, 0x005bd844, 0x005bd848,
    0x005be178, 0x004e3408,
    0x005afc40, 0x005afc44, 0x005afc50,
    { { 0x005afc30, 0x005afc90 }, { 0x005bced0, 0x005bcf40 }, { 0x005b0590, 0x005b05c0 },
      { 0x005b0740, 0x005b0750 }, { 0x004d6370, 0x004d6380 }, { 0x005c4800, 0x005c4830 } },
    0x0043bf40, 0x0042a7c0, 0x0042a8e0, 0x0042c330, 0x0042c335,
    0x0044fd30, 0x0044fd35, 0x0044fe50, 0x0044fe55,
    0x00431760, 0x004317a0,
    0x004bc524, 0x004bc858, 0x004bc4dc, 0x004be488,
    0x0049024c, 0x00490298, 0x0049029c,
    0x00440e0b,
    0x004410d1, 0x004410dd, 0x004410f4,
    0x00441032, 0x00441038,
    0x00467f1d, 0x00467f23,
    0x00467dbd, 0x00467dc2,
    0x00462fb3, 0x00462fc5, 0x004631c7, 0x00463262,
    0x004632d0,
    0x00441263, 0x0044126c,
    0x0043078e,
    0x00431d24, 0x00431d29,
    0x00471a5b, 0x00472aaa,
    0x004638d9, 0x00463a8c,
    0x004412cc
};

static const VersionSigs SIGS_DISC = {
    { 0xA1, 0x3C, 0x07, 0x5C, 0x00 },                                   /* Trans: mov eax,[scene] */
    { 0x32, 0xC9, 0xC7, 0x05, 0xDC, 0xC4, 0x5A },                       /* LoadStart */
    { 0x8B, 0x15, 0x40, 0x07, 0x5C, 0x00 },                             /* SceneCheck */
    { 0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00 },                             /* EndScene: call [eax+0xa8] */
    { 0xF3, 0x0F, 0x10, 0x45, 0x08, 0xF3, 0x0F, 0x11, 0x05, 0x70, 0x97, 0x5B, 0x00 },  /* SetMefa */
    { 0xF3, 0x0F, 0x10, 0x45, 0x08, 0xF3, 0x0F, 0x11, 0x05, 0x6C, 0x97, 0x5B, 0x00 },  /* SetCR */
    { 0x8B, 0x45, 0x08, 0xA3, 0x08, 0x8E, 0x5B, 0x00 },                 /* SetLives */
    { 0x8B, 0x45, 0x08, 0xA3, 0x28, 0x8E, 0x5B, 0x00 },                 /* SetDifficulty */
    { 0x8B, 0x00, 0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF },           /* GetRefRainConfig */
    { 0x56, 0x6A, 0x01, 0x6A, 0x0C }                                    /* SaveReplayHeader */
};

static const VersionSigs SIGS_STEAM = {
    { 0xA1, 0x0C, 0x48, 0x5C, 0x00 },
    { 0x32, 0xC9, 0xC7, 0x05, 0xAC, 0x05, 0x5B },
    { 0x8B, 0x15, 0x10, 0x48, 0x5C, 0x00 },
    { 0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00 },
    { 0xF3, 0x0F, 0x10, 0x43, 0x08, 0xF3, 0x0F, 0x11, 0x05, 0x40, 0xD8, 0x5B, 0x00 },
    { 0xF3, 0x0F, 0x10, 0x43, 0x08, 0xF3, 0x0F, 0x11, 0x05, 0x3C, 0xD8, 0x5B, 0x00 },
    { 0x8B, 0x43, 0x08, 0xA3, 0xD8, 0xCE, 0x5B, 0x00 },
    { 0x8B, 0x43, 0x08, 0xA3, 0xF8, 0xCE, 0x5B, 0x00 },
    { 0x8B, 0x00, 0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF },
    { 0x56, 0x6A, 0x01, 0x6A, 0x0C }
};

static DWORD  g_BaseDelta = 0;      /* actual image base - 0x400000 */
static volatile int g_IsSteam = 0;
static GameAddrs g_Addrs;           /* runtime copy, rebased */
static const GameAddrs *g_A = &ADDRS_DISC;
static VersionSigs g_Sigs;

/* Accessors for game variables (g_A entries already carry the image delta). */
#define VA(x)  (g_A->x)
#define VD(x)  (*(volatile DWORD*)(g_A->x))
#define VB(x)  (*(volatile BYTE*)(g_A->x))

/* Addresses consumed by naked-hook inline asm (symbol-referenced, so the
 * register setup happens inside the asm block, after pushal-style saves). */
static volatile DWORD g_A_SceneNext;
static volatile DWORD g_A_LoadFlag;
static volatile DWORD g_A_Mefa, g_A_Cr, g_A_Lives, g_A_Diff, g_A_ViewIdx;
static volatile DWORD g_A_GpuQueue;
static volatile DWORD g_DispatchEpilogue;
static volatile DWORD g_SaveGlobalRetAddr;
static volatile DWORD g_GetGameInfoIntRetAddr;
static volatile DWORD g_SetGameInfoIntRetAddr;
static volatile DWORD g_GetGameInfoIntFn;
static volatile DWORD g_RefRainCfgRetAddr;

static void DetectVersion(void);


static volatile int g_ShowStageMenu = 0;
static volatile int g_StageSelected = 0;
static volatile int g_PracticeMode = 0;
static volatile int g_PracticeStage = 0;
static volatile int g_AutoAdvanceToDifficulty = 0;
static volatile int g_AutoNavPhase = 0;
static volatile int g_AutoNavTimer = 0;
/* Where the simulated title navigation should stop:
   0 = reopen the Practice (stage select) menu - used when the menu is cancelled,
   1 = leave the player on the character's desktop - used by the pause-menu Q hotkey. */
static volatile int g_AutoNavTarget = 0;
#define SIMKEY_NONE   0
#define SIMKEY_DECIDE 1
#define SIMKEY_CANCEL 2
#define SIMKEY_DOWN   3
#define SIMKEY_UP     4
static volatile int g_SimulatedKeyAction = SIMKEY_NONE;
static volatile DWORD g_PracticeChar = 0;
static volatile int g_CancelToTitle = 0;
static volatile int g_PauseRetryAll = 0;
static volatile int g_MenuCursor = 0;
static volatile int g_ActiveColumn = 0;
static volatile int g_LifeCursor = 2;
static volatile int g_SelectedLives = 2;
static volatile int g_MefaCursor = 1;
static volatile int g_CrStockCursor = 0;
static volatile int g_CrGaugeCursor = 0;
static volatile DWORD g_SelectedMefaDword = 0x42c80000;
static volatile DWORD g_SelectedCrDword = 0x00000000;
static volatile int g_PendingStageInitStats = 0;
static volatile int g_StageInitRenderFrames = 0;

static void ResetStageScore(void);
static volatile long g_PendingReset = 0;

static volatile DWORD g_ReturnAddr;
static volatile DWORD g_EndSceneRetAddr;
static volatile DWORD g_LoadStartRetAddr;
static volatile DWORD g_LoadStartSkipAddr;

static IDirect3DQuery9 *g_pEventQuery = NULL;
static int g_LowLatencyMode = 1;

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

/* Logging (stage_select.log and OutputDebugString) is only compiled into the debug build
   (make refrain-debug, which defines REFRAIN_DEBUG_BUILD).  The release build keeps the call sites but
   LogMessage does nothing there (the arguments are still evaluated). */
#ifdef REFRAIN_DEBUG_BUILD
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
#else
static inline void LogMessage(const char *format, ...) {
    (void)format;
}
#endif

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

/* Frame of `t` pixels drawn OUTSIDE the box (x, y, w, h).  Four thin rectangles instead of a filled one, so a
   translucent panel drawn inside shows the game screen and not the border color. */
static void DrawFrame(IDirect3DDevice9 *pDevice, float x, float y, float w, float h, float t, DWORD color) {
    DrawSolidRect(pDevice, x - t, y - t, w + 2.0f * t, t, color);          /* top */
    DrawSolidRect(pDevice, x - t, y + h, w + 2.0f * t, t, color);          /* bottom */
    DrawSolidRect(pDevice, x - t, y,     t,            h, color);          /* left */
    DrawSolidRect(pDevice, x + w, y,     t,            h, color);          /* right */
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
                int itemH = (screenW >= 1000) ? 20 : 13;
                int stageH = (screenW >= 1000) ? 20 : 13;
                int titleH = (screenW >= 1000) ? 24 : 16;
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

typedef struct {
    DWORD panel, borderOn, borderOff, headerOn, headerOff, titleOn, titleOff;
    DWORD hiOn, hiSel, hiTextOn, hiTextSel, itemOn, itemOff, dimText, lockText;
    DWORD footerBorder, foot1, foot2;
} PracticeMenuTheme;

/* background panel opacity shared by every Practice menu theme (0x00 = invisible .. 0xFF = opaque) */
#define MENU_PANEL_ALPHA 0xC4
#define PANEL_COLOR(rgb) ((((DWORD)MENU_PANEL_ALPHA) << 24) | (DWORD)(rgb))

static const PracticeMenuTheme *GetPracticeMenuTheme(DWORD ch) {
    static const PracticeMenuTheme themes[3] = {
    	/* Miria */
        { PANEL_COLOR(0x0D111A), 0xFF00BFFF, 0xFF335577, 0x88004488, 0x55112233, 0xFFFFCC00, 0xFF88AABB,
          0x660077CC, 0x44005588, 0xFFFFFF00, 0xFF00FFCC, 0xFFAAAAAA, 0xFF666666, 0xFF555555, 0xFF333333,
          0xFF335577, 0xFF88DDFF, 0xFF6699BB },
        /* Tee */
        { PANEL_COLOR(0x2A1022), 0xFFFF88CC, 0xFFC0609C, 0x88C83C90, 0x55702C58, 0xFFFFE08A, 0xFFFFCCE4,
          0x66E048A8, 0x44B83C88, 0xFFFFFF88, 0xFFFFAADE, 0xFFFFD6EA, 0xFFD296B4, 0xFFA87890, 0xFF8C6078,
          0xFFC0609C, 0xFFFFC4E2, 0xFFE0A0C0 },
        /* Mews */
        { PANEL_COLOR(0x0D1A14), 0xFF28B898, 0xFF33574D, 0x880C5848, 0x55143328, 0xFFFFCC66, 0xFF99CCBB,
          0x66208870, 0x44185850, 0xFFFFFF66, 0xFF66FFD8, 0xFFB0DDCC, 0xFF668877, 0xFF446655, 0xFF335544,
          0xFF33574D, 0xFF88DDCC, 0xFF669988 }
    };
    if (ch >= 1 && ch <= 3) ch--;
    if (ch > 2) ch = (DWORD)g_PracticeChar;
    if (ch > 2) ch = 0;
    return &themes[ch];
}

static int IsKeyTriggered(int vk) {
    static BYTE prev[256] = {0};
    BYTE curr = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    int triggered = curr && !prev[vk];
    prev[vk] = curr;
    return triggered;
}

static void BurnMenuOpenEdges(void) {
    IsKeyTriggered('Z');
    IsKeyTriggered(VK_RETURN);
    IsKeyTriggered(VK_SPACE);
}

typedef struct {
    int stage;
    const char *label;
} MenuItem;

#define MENU_ITEM_COUNT 5

/* visitCentral[%d] (1-based, %d = stage 1..5) lives in the global data struct. Its per-build
   base VAs (index 1) are 0x5ac4e0 (disc) / 0x5b05b0 (steam); rebased by g_BaseDelta at runtime.
   A non-zero value means the player has reached that Central in normal play. */
static inline int IsStageReached(int stage) {
    if (stage <= 1) return 1;
    if (stage > MENU_ITEM_COUNT) return 0;
    static const DWORD visitBase[2] = { 0x005ac4e0, 0x005b05b0 };  /* [0]=disc, [1]=steam */
    DWORD *p = (DWORD*)((uintptr_t)(visitBase[g_IsSteam ? 1 : 0] + g_BaseDelta) + (DWORD)(stage - 1) * 4);
    if (IsBadReadPtr(p, 4)) return 0;
    return *p != 0;
}

/* Number of practice-menu stages selectable now: limited by difficulty (Easy = 1..4) and by
   normal-play progress (a stage is hidden until reached, so stage N is selectable only when
   visitCentral[N] is set). */
static inline int GetSelectableStageCount(void) {
    DWORD diff = VD(Diff);
    int cap = (diff == 1) ? 4 : MENU_ITEM_COUNT;
    int n = 1;  /* Central 01 is always available */
    while (n < cap && IsStageReached(n + 1)) n++;
    return n;
}

static const MenuItem g_MenuItems[MENU_ITEM_COUNT] = {
    { 1, "Central 01 - The way to UTOPIA / Natural Landscape -" },
    { 2, "Central 02 - DATA Flow / Artificial Landscape -" },
    { 3, "Central 03 - Virtual-Labo-Factory / Virtual Reality-" },
    { 4, "Central 04 - Divergence layer of higher level data / Haze in desert-" },
    { 5, "Central 05 - The Central Nervous System / M.R.S. Central Core-" },
};

typedef struct {
    int lives;
    const char *label;
} LifeItem;

#define LIFE_ITEM_COUNT 11

static const LifeItem g_LifeItems[LIFE_ITEM_COUNT] = {
    { 0, "0" },
    { 1, "1" },
    { 2, "2 (Default)" },
    { 3, "3" },
    { 4, "4" },
    { 5, "5" },
    { 6, "6" },
    { 7, "7" },
    { 8, "8" },
    { 9, "9" },
    { 10, "10 (Maximum)" },
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
    { 1, "1 (Default)" },
    { 2, "2" },
    { 3, "3" },
    { 4, "4" },
    { 5, "5" },
    { 6, "6 (Maximum)" },
};

typedef struct {
    int stock;
    const char *label;
} CrStockItem;

#define CR_STOCK_ITEM_COUNT 4

static const CrStockItem g_CrStockItems[CR_STOCK_ITEM_COUNT] = {
    { 0, "0 (Default)" },
    { 1, "1" },
    { 2, "2" },
    { 3, "3 (Maximum)" },
};

typedef struct {
    float pct;
    const char *label;
} CrGaugeItem;

#define CR_GAUGE_ITEM_COUNT 5

static const CrGaugeItem g_CrGaugeItems[CR_GAUGE_ITEM_COUNT] = {
    { 0.0f,   "0% (Default)" },
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

    typedef HRESULT (__stdcall *PFN_SetCurrentPosition)(DWORD pBuf, DWORD pos);
    PFN_SetCurrentPosition pfnSetPos = (PFN_SetCurrentPosition)vtbl[0x34 / 4];
    if (pfnSetPos) pfnSetPos(pDSBuf, 0);

    typedef HRESULT (__stdcall *PFN_Play)(DWORD pBuf, DWORD res1, DWORD res2, DWORD flags);
    PFN_Play pfnPlay = (PFN_Play)vtbl[0x30 / 4];
    if (pfnPlay) pfnPlay(pDSBuf, 0, 0, 0);

    DWORD totalSlots = *(DWORD*)(pSoundObj + 0x12c);
    if (totalSlots > 0 && totalSlots <= 16) {
        *(DWORD*)(pSoundObj + 0x134) = (bufIdx + 1) % totalSlots;
    }
}

static void PlayGameSE(int seIndex) {
    DWORD pSoundObj = 0;

    __asm__ __volatile__(
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        "movl %%eax, %0\n\t"
        : "=r"(pSoundObj)
        : "r"((DWORD)seIndex), "r"(g_A->fnSE)
        : "eax", "ecx", "edx", "memory"
    );

    if (pSoundObj) {
        PlaySoundDirect(pSoundObj);
        LogMessage("PlayGameSE(%d): played OK", seIndex);
    }
}

static inline int IsReplayOrDemo(void) {
    DWORD gameMode = VD(GameMode);
    if (gameMode == 2 || gameMode == 3) {
        return 1;
    }
    return 0;
}

typedef unsigned char (__attribute__((thiscall)) *PFN_IsKey)(void *this, int key);
static PFN_IsKey orig_IsKeyPressed = NULL;
static PFN_IsKey orig_IsKeyTriggered = NULL;
static volatile int g_PauseMenuOpen = 0;
/* Tick of the last game-native "pause opened" signal (ESC trigger seen by IsKeyTriggered, or an
   ESC edge). Q/R act while the flag is open or within this grace window, so a heuristic glitch
   can never silence the hotkeys right after the menu opens. */
static volatile DWORD g_LastPauseOpenTick = 0;
static void PauseDiagKey(int key);

static inline int MatchSimulatedKey(int action, int key) {
    switch (action) {
        case SIMKEY_DECIDE:
            if (key == 'Z' || key == 'z' || key == 0x5A || key == 0x7A ||
                key == VK_RETURN || key == 0x0D ||
                key == VK_SPACE || key == 0x20 ||
                key == 0x2C || key == 0x1C) {
                return 1;
            }
            break;
        case SIMKEY_CANCEL:
            if (key == 'X' || key == 'x' || key == 0x58 || key == 0x78 ||
                key == VK_ESCAPE || key == 0x1B ||
                key == 0x2D || key == 0x01) {
                return 1;
            }
            break;
        case SIMKEY_DOWN:
            if (key == VK_DOWN || key == 0x28 || key == 0xD0) {
                return 1;
            }
            break;
        case SIMKEY_UP:
            if (key == VK_UP || key == 0x26 || key == 0xC8) {
                return 1;
            }
            break;
    }
    return 0;
}

static const DWORD ORIG_MAGIC = 0x52464552;
static const DWORD ORIG_VERS  = 0x3F83D70A;

static const BYTE PracKey[32] = {
    0x5B, 0xC3, 0x17, 0xE1, 0x9F, 0x48, 0x70, 0x2A,
    0x8D, 0x3B, 0xC9, 0x56, 0xE7, 0x1F, 0x64, 0xAC,
    0x2F, 0x91, 0x4D, 0x78, 0x0E, 0xD5, 0x63, 0xB8,
    0x9C, 0x3E, 0x71, 0xA9, 0x54, 0x08, 0xDB, 0x6F
};

static volatile DWORD g_ScrambleStream = 0;
static volatile DWORD g_ScrambleCursor = 0;
static volatile DWORD g_ScrambleSeed = 0;
static volatile int g_ScrambleWrites = 0;
static volatile int g_ScrambleActive = 0;

static volatile DWORD g_DescrambleStream = 0;
static volatile DWORD g_DescrambleCursor = 0;
static volatile DWORD g_DescrambleSeed = 0;
static volatile int g_DescrambleActive = 0;
static volatile int g_DescrambleForce = 0;
static volatile DWORD g_DescrambleArmTick = 0;

static BYTE PracKeyByte(DWORD abs, DWORD seed) {
    DWORD s = seed ^ 0x7F4A2531u ^ (abs * 2654435761u);
    s ^= s >> 13;
    s *= 0x9E3779B1u;
    s ^= s >> 16;
    return (BYTE)((PracKey[(abs + (s >> 24)) & 31] + (BYTE)(s >> 17)) ^ (s >> 25));
}

static void PracXor(BYTE *buf, DWORD n, DWORD base, DWORD seed) {
    for (DWORD i = 0; i < n; i++) {
        buf[i] ^= PracKeyByte(base + i, seed);
    }
}

#define PRAC_MAX_SEEDS 8192
static DWORD g_PracSeeds[PRAC_MAX_SEEDS];
static volatile int g_PracSeedCount = 0;

static int IsPracticeSeed(DWORD seed) {
    int cnt = g_PracSeedCount;
    for (int i = 0; i < cnt; i++) {
        if (g_PracSeeds[i] == seed) return 1;
    }
    return 0;
}

static void AddPracticeSeed(DWORD seed) {
    if (IsPracticeSeed(seed) || g_PracSeedCount >= PRAC_MAX_SEEDS) return;
    g_PracSeeds[g_PracSeedCount++] = seed;
}

static void DisarmDescramble(void) {
    g_DescrambleActive = 0;
    g_DescrambleForce = 0;
    g_DescrambleStream = 0;
}

#define PRAC_TAG_SIZE 16
static volatile int g_PracScanned = 0;

static void MakePracticeTag(DWORD seed, BYTE *tag) {
    static const BYTE magic[8] = { 'R', 'R', 'P', 'R', 'T', 'A', 'G', '1' };
    memcpy(tag, magic, 8);
    for (int i = 0; i < 8; i++) {
        tag[8 + i] = PracKeyByte(0x7E000000u + (DWORD)i, seed ^ 0xA5C3E17Bu);
    }
}

static int HasPracticeTag(FILE *fp, DWORD seed) {
    BYTE tag[PRAC_TAG_SIZE], want[PRAC_TAG_SIZE];
    if (fseek(fp, -PRAC_TAG_SIZE, SEEK_END) != 0) return 0;
    if (fread(tag, 1, PRAC_TAG_SIZE, fp) != PRAC_TAG_SIZE) return 0;
    MakePracticeTag(seed, want);
    return memcmp(tag, want, PRAC_TAG_SIZE) == 0;
}

static void ScanReplayDirForPractice(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("replay\\*.rep", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogMessage("Replay scan: replay folder not found");
        return;
    }
    int total = 0, tagged = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        int plen = snprintf(path, sizeof(path), "replay\\%s", fd.cFileName);
        if (plen < 0 || (size_t)plen >= sizeof(path)) continue;
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        total++;
        DWORD hdr[3];
        if (fread(hdr, 1, 12, fp) == 12 && hdr[0] == ORIG_MAGIC && hdr[1] == ORIG_VERS) {
            if (HasPracticeTag(fp, hdr[2])) {
                AddPracticeSeed(hdr[2]);
                tagged++;
            }
        }
        fclose(fp);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    LogMessage("Replay scan: %d replays, %d tagged practice replays", total, tagged);
}

__attribute__((used)) static void HandleSaveReplayHeader(DWORD hdr, DWORD stream) {
    g_ScrambleStream = 0;
    if (!g_PracticeMode || IsReplayOrDemo()) return;
    if (!hdr || IsBadReadPtr((void*)hdr, 12)) return;
    DWORD *p = (DWORD*)hdr;
    if (p[0] != ORIG_MAGIC || p[1] != ORIG_VERS) return;
    g_ScrambleSeed = p[2];
    AddPracticeSeed(p[2]);
    g_ScrambleCursor = 0;
    g_ScrambleWrites = 4;
    g_ScrambleActive = 1;
    g_ScrambleStream = stream;
    LogMessage("ReplaySave: practice replay saved with REFR header, body scrambled via rolling XOR");
}

typedef __attribute__((cdecl)) size_t PFN_FILE4(void*, size_t, size_t, void*);
static DWORD g_FwriteTramp = 0;
static DWORD g_FreadTramp = 0;

__attribute__((used)) static size_t Detour_FwriteWrapper(void *ptr, size_t sz, size_t cnt, void *stream) {
    DWORD n = (DWORD)sz * (DWORD)cnt;
    int appendTag = 0;
    DWORD tagSeed = 0;
    if (g_ScrambleActive && (DWORD)stream == g_ScrambleStream && ptr && !IsBadWritePtr(ptr, n)) {
        LogMessage("ReplayTrace: fwrite #%d size=%lu at offset %lu", 5 - g_ScrambleWrites, n, g_ScrambleCursor);
        if (g_ScrambleCursor == 0) {
            g_ScrambleCursor += n;
        } else if (n > 0) {
            PracXor((BYTE*)ptr, n, g_ScrambleCursor, g_ScrambleSeed);
            g_ScrambleCursor += n;
        }
        if (--g_ScrambleWrites <= 0) {
            g_ScrambleActive = 0;
            g_ScrambleStream = 0;
            appendTag = 1;
            tagSeed = g_ScrambleSeed;
        }
    }
    size_t wr = ((PFN_FILE4*)g_FwriteTramp)(ptr, sz, cnt, stream);
    if (appendTag) {
        BYTE tag[PRAC_TAG_SIZE];
        MakePracticeTag(tagSeed, tag);
        ((PFN_FILE4*)g_FwriteTramp)(tag, 1, PRAC_TAG_SIZE, stream);
        LogMessage("ReplaySave: practice tag appended (seed 0x%08X)", tagSeed);
    }
    return wr;
}

__attribute__((used)) static size_t Detour_FreadWrapper(void *ptr, size_t sz, size_t cnt, void *stream) {
    size_t r = ((PFN_FILE4*)g_FreadTramp)(ptr, sz, cnt, stream);
    DWORD nbytes = (DWORD)r * (DWORD)sz;
    int justArmed = 0;
    if ((DWORD)sz * (DWORD)cnt == 12 && r == cnt && ptr && !IsBadReadPtr(ptr, 12)) {
        DWORD *hp = (DWORD*)ptr;
        if (hp[0] == ORIG_MAGIC && hp[1] == ORIG_VERS) {
            if (!g_PracScanned) {
                g_PracScanned = 1;
                ScanReplayDirForPractice();
            }
            if (g_DescrambleForce && (DWORD)stream == g_DescrambleStream) {
                DisarmDescramble();
            }
            if (IsPracticeSeed(hp[2])) {
                g_DescrambleSeed = hp[2];
                g_DescrambleCursor = 12;
                g_DescrambleForce = 1;
                g_DescrambleArmTick = GetTickCount();
                g_DescrambleStream = (DWORD)stream;
                g_DescrambleActive = 1;
                justArmed = 1;
            }
        }
    }
    if (!justArmed && g_DescrambleActive && g_DescrambleForce && (DWORD)stream == g_DescrambleStream &&
        ptr && nbytes > 0 && !IsBadWritePtr(ptr, nbytes)) {
        if (GetTickCount() - g_DescrambleArmTick > 1000) {
            DisarmDescramble();
        } else {
            PracXor((BYTE*)ptr, nbytes, g_DescrambleCursor, g_DescrambleSeed);
            g_DescrambleCursor += nbytes;
        }
    }
    return r;
}

void __attribute__((naked)) Hook_FwriteWrapper(void) {
    __asm__ __volatile__("jmp _Detour_FwriteWrapper");
}

void __attribute__((naked)) Hook_FreadWrapper(void) {
    __asm__ __volatile__("jmp _Detour_FreadWrapper");
}

static void InstallWrapperDetour(DWORD target, DWORD detourFn, DWORD trampolineOut) {
    unsigned char orig[5];
    DWORD oldProtect;
    memcpy(orig, (void*)target, 5);
    if (VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        BYTE *tramp = (BYTE*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp) {
            VirtualProtect((void*)target, 5, oldProtect, &oldProtect);
            LogMessage("[Error] trampoline alloc failed for wrapper at 0x%08X", target);
            return;
        }
        memcpy(tramp, orig, 5);
        DWORD rel = (target + 5) - ((DWORD)tramp + 10);
        tramp[5] = 0xE9;
        memcpy(&tramp[6], &rel, 4);
        *(DWORD*)trampolineOut = (DWORD)tramp;
        DWORD jrel = (DWORD)detourFn - (target + 5);
        unsigned char patch[5];
        patch[0] = 0xE9;
        memcpy(&patch[1], &jrel, 4);
        memcpy((void*)target, patch, 5);
        VirtualProtect((void*)target, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);
        FlushInstructionCache(GetCurrentProcess(), tramp, 16);
        LogMessage("FILE wrapper detour installed at 0x%08X (trampoline 0x%08X)", target, tramp);
    }
}

static volatile LONGLONG g_LatQpcFreq = 0;
static volatile LONGLONG g_LatLastRenderQpc = 0;
static volatile LONGLONG g_LatLastKeyPollQpc = 0;
static volatile LONGLONG g_LatLastPadPollQpc = 0;
static volatile LONGLONG g_LatPollAgeAccum = 0;
static volatile LONGLONG g_LatPollAgeMax = 0;
static volatile DWORD g_LatPollAgeCount = 0;
static volatile DWORD g_LatKeyPollCount = 0;
static volatile DWORD g_LatPadPollCount = 0;
static volatile DWORD g_LatFrameCount = 0;

typedef BOOL (WINAPI *PFN_GetKeyboardState_t)(PBYTE);
static PFN_GetKeyboardState_t orig_GetKeyboardState = NULL;

/* "Is the game actually paused?" The frame counter does NOT freeze while the pause menu is up
   (both builds keep advancing FrameCnt/Frame2 during pause - verified by PauseDiag freeze/resume
   diffs and the hotkey logs), so this verdict is almost always 0. It stays as a harmless backup
   signal; the reliable pause detection is the game's own ESC trigger, see Hook_IsKeyTriggered. */
static int GameLooksPaused(void) {
    static DWORD lastFrame = 0;
    static DWORD lastScene = 0xFFFFFFFF;
    static DWORD lastChange = 0;
    static int primed = 0;
    static DWORD lastThrottle = 0xFFFFFFFF;
    static int framePaused = 0;
    DWORD fr = VD(FrameCnt);
    DWORD th = VD(Frame2);
    DWORD sc = VD(Scene);
    DWORD now = GetTickCount();
    if (!primed || sc != lastScene) {
        /* New scene: the counter is meaningless until the game has actually ticked once
           (it is 0 during the stage intro and after a practice retry). */
        primed = 1;
        lastScene = sc;
        lastFrame = fr;
        lastThrottle = th;
        lastChange = now;
        framePaused = 0;
        return 0;
    }
    if (fr != lastFrame) {
        lastFrame = fr;
        lastChange = now;
        /* An opening/closing animation ticks the frame counter for a moment even though the
           throttle keeps running; hold the old verdict briefly instead of flip-flopping. */
        if (!framePaused) return 0;
        if (now - lastChange < 300) return framePaused;
        framePaused = 0;
        return 0;
    }
    if (th != lastThrottle) {
        /* The throttle only advances while the game runs, never while a pause is open
           (PauseDiag freeze/resume diffs show it parked during pause). */
        lastThrottle = th;
        lastChange = now;
        framePaused = 0;
        return 0;
    }
    if (!framePaused && (now - lastChange) >= 120) {
        framePaused = 1;
        return 1;
    }
    return framePaused;
}

static BOOL WINAPI Mine_GetKeyboardState(PBYTE p) {
    BOOL r = orig_GetKeyboardState(p);
    if (r && p) {
        DWORD scene = VD(Scene);
        int inStage = ((scene >= 2 && scene <= 7) || scene == 10);
        static int qWasDown = 0;
        static int rWasDown = 0;
        static int escWasDown = 0;
        int qDown = (GetAsyncKeyState('Q') & 0x8000) != 0;
        int rDown = (GetAsyncKeyState('R') & 0x8000) != 0;
        int escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

        int paused = GameLooksPaused();
        DWORD now = GetTickCount();
        /* Q/R act while the pause flag is set or within the grace window after the last
           game-native "pause opened" signal. */
        int hotkeyArmed = g_PauseMenuOpen ||
                          (g_LastPauseOpenTick != 0 && now - g_LastPauseOpenTick < 10000);
        if (inStage) {
            if (escDown && !escWasDown) {
                /* The game does not close the pause menu on ESC (the stage script polls ESC via
                   IsKeyTriggered only while unpaused - pressing ESC with the menu open is a
                   no-op for the game), so an ESC edge always means "the menu just opened". */
                g_PauseMenuOpen = 1;
                g_LastPauseOpenTick = now;
                LogMessage("Pause menu state: open (ESC edge, scene %lu)", scene);
            }
            if (paused) {
                g_PauseMenuOpen = 1;
                g_LastPauseOpenTick = now;
            }
            if (hotkeyArmed) {
                if (qDown && !qWasDown) {
                    g_PauseMenuOpen = 0;
                    g_LastPauseOpenTick = 0;
                    VD(SceneNext) = 1;
                    LogMessage("Pause hotkey Q: Return to Title (next scene=1)");
                } else if (rDown && !rWasDown) {
                    g_PauseRetryAll = 1;
                    g_PauseMenuOpen = 0;
                    g_LastPauseOpenTick = 0;
                    DWORD retryScene = (g_PracticeMode && g_PracticeStage >= 1 &&
                                        g_PracticeStage <= 5)
                                       ? 2 + (DWORD)g_PracticeStage
                                       : 3;
                    VD(SceneNext) = retryScene;
                    LogMessage("Pause hotkey R: direct Retry All (next scene=%lu)", retryScene);
                }
            }
#ifdef REFRAIN_DEBUG_BUILD
            if ((qDown && !qWasDown) || (rDown && !rWasDown)) {
                LogMessage("Pause hotkey seen: q=%d r=%d armed=%d flag=%d sinceOpen=%lums paused=%d frame=%lu scene=%lu",
                           qDown && !qWasDown, rDown && !rWasDown, hotkeyArmed, g_PauseMenuOpen,
                           g_LastPauseOpenTick ? now - g_LastPauseOpenTick : 0, paused,
                           VD(FrameCnt), scene);
            }
#endif
            qWasDown = qDown;
            rWasDown = rDown;
            escWasDown = escDown;
        } else {
            if (g_PauseMenuOpen)
                LogMessage("Pause menu state: reset (scene %lu left the stage range)", scene);
            g_PauseMenuOpen = 0;
            g_LastPauseOpenTick = 0;
            qWasDown = rWasDown = escWasDown = 0;
        }

        if (g_PauseMenuOpen &&
            ((p['Z'] | p[VK_RETURN] | p[VK_SPACE] | p['X'] | p[VK_ESCAPE]) & 0x80)) {
            g_PauseMenuOpen = 0;
            LogMessage("Pause menu state: closed (decide/cancel)");
        }
    }
    if (g_LatQpcFreq) {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        g_LatLastKeyPollQpc = li.QuadPart;
        InterlockedIncrement((volatile LONG*)&g_LatKeyPollCount);
    }
    return r;
}

typedef DWORD (WINAPI *PFN_XInputGetState_t)(DWORD, void*);
static PFN_XInputGetState_t orig_XInputGetState = NULL;

static DWORD WINAPI Mine_XInputGetState(DWORD n, void *s) {
    DWORD r = orig_XInputGetState(n, s);
    if (g_LatQpcFreq) {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        g_LatLastPadPollQpc = li.QuadPart;
        InterlockedIncrement((volatile LONG*)&g_LatPadPollCount);
    }
    return r;
}

typedef DWORD (WINAPI *PFN_XInputGetCaps_t)(DWORD, DWORD, void*);
static PFN_XInputGetCaps_t orig_XInputGetCaps = NULL;

static DWORD WINAPI Mine_XInputGetCaps(DWORD user, DWORD flags, void *caps) {
    return orig_XInputGetCaps(user, flags, caps);
}

static void PatchIATPtr(DWORD iatVa, void *newFn, void **origOut) {
    void **slot = (void**)iatVa;
    DWORD old;
    if (IsBadReadPtr(slot, 4)) return;
    if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        void *cur = *slot;
        if (cur && cur != newFn) {
            if (origOut && !*origOut) *origOut = cur;
            *slot = newFn;
            FlushInstructionCache(GetCurrentProcess(), slot, 4);
        }
        VirtualProtect(slot, 4, old, &old);
    }
}

typedef int (__attribute__((thiscall)) *PFN_InputMethod1)(void*);

static int ThiscallCallInput(DWORD obj, DWORD slotOff) {
    if (!obj || obj < 0x10000 || IsBadReadPtr((void*)(obj + slotOff + 0x1E), 4)) return 1;
    DWORD vtbl = *(volatile DWORD*)obj;
    if (!vtbl || vtbl < 0x10000 || IsBadReadPtr((void*)(vtbl + slotOff), 4)) return 1;
    PFN_InputMethod1 fn = (PFN_InputMethod1)(*(volatile DWORD*)(vtbl + slotOff));
    if (!fn || IsBadCodePtr((FARPROC)fn)) return 1;
    return fn((void*)obj);
}

__attribute__((used)) static void PreTickInputPoll(void) {
    if (!g_LatQpcFreq) return;

    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    LONGLONG now = li.QuadPart;
    LONGLONG lastKey = g_LatLastKeyPollQpc;
    if (lastKey) {
        LONGLONG age = now - lastKey;
        if (age > g_LatPollAgeMax) g_LatPollAgeMax = age;
        g_LatPollAgeAccum += age;
        g_LatPollAgeCount++;
    }

    DWORD o = VD(Obj);
    if (o) {
        if (!ThiscallCallInput(o, 0x0C)) return;
    }
    o = VD(In3);
    if (o && *(volatile BYTE*)(o + 0x1E)) {
        if (!ThiscallCallInput(o, 0x10)) return;
    }
    o = VD(In2);
    if (o && *(volatile BYTE*)(o + 0x1E)) {
        if (!ThiscallCallInput(o, 0x10)) return;
    }
    o = VD(In1);
    if (o) {
        ThiscallCallInput(o, 0x10);
    }
}

static volatile DWORD g_PreTickRetAddr;
static volatile DWORD g_PreTickTarget;

void __attribute__((naked)) Hook_PreTickPollCall(void) {
    __asm__ __volatile__(
        "pushfl\n\t"
        "pushal\n\t"
        "call _PreTickInputPoll\n\t"
        "popal\n\t"
        "popfl\n\t"
        "pushl %0\n\t"
        "jmp *%1\n\t"
        :
        : "m"(g_PreTickRetAddr), "m"(g_PreTickTarget)
    );
}

static volatile DWORD g_SaveReplayHdrRetAddr;

void __attribute__((naked)) Hook_SaveReplayHeader(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl 0x4(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "movl 0x4(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "call _HandleSaveReplayHeader\n\t"
        "addl $8, %%esp\n\t"
        "popal\n\t"
        "pushl %%esi\n\t"
        "pushl $1\n\t"
        "pushl $0x0C\n\t"
        "jmp *%0\n\t"
        :
        : "m"(g_SaveReplayHdrRetAddr)
    );
}

unsigned char __attribute__((thiscall)) Hook_IsKeyPressed(void *this, int key) {
    if (g_ShowStageMenu) return 0;

    if (g_AutoAdvanceToDifficulty) {
        if (g_SimulatedKeyAction != SIMKEY_NONE && MatchSimulatedKey(g_SimulatedKeyAction, key)) {
            LogMessage("Hook_IsKeyPressed: SIMULATED MATCH action=%d key=0x%02X", g_SimulatedKeyAction, key);
            return 1;
        }
        return 0;
    }

    return orig_IsKeyPressed(this, key);
}

static inline int GetConceptReactorKey(void) {
    DWORD pConfig = VD(Config);
    if (pConfig && !IsBadReadPtr((void*)pConfig, 0x800)) {
        DWORD crIdx = *(DWORD*)(pConfig + 0x434);
        if (crIdx < 32) {
            DWORD keyCode = *(DWORD*)(pConfig + 0x728 + crIdx * 4);
            if (keyCode > 0 && keyCode < 256) {
                return (int)keyCode;
            }
        }
    }
    return 'C';
}

static inline int GetConceptReactorJoyButton(int crKey) {
    void *pInputMgr = *(void**)g_A->InputMgr;
    if (pInputMgr && !IsBadReadPtr(pInputMgr, 16)) {
        void *pJ = *(void**)((DWORD)pInputMgr + 0xc);
        if (pJ && !IsBadReadPtr(pJ, 0x400)) {
            DWORD vtbl = *(DWORD*)pJ;
            if (vtbl == g_A->vtJoyA && crKey >= 0 && crKey < 256) {
                int btn = *(int*)((DWORD)pJ + 0x34 + crKey * 4);
                if (btn >= 0 && btn < 32) return btn;
            } else if (vtbl == g_A->vtJoyB && crKey >= 0 && crKey < 256) {
                int btn = *(int*)((DWORD)pJ + 0x1f4 + crKey * 4);
                if (btn >= 0 && btn < 32) return btn;
            }
        }
    }
    return 2;
}

static int IsConceptReactorHeld(void) {
    int crKey = GetConceptReactorKey();

    void *pInputMgr = *(void**)g_A->InputMgr;
    if (pInputMgr && !IsBadReadPtr(pInputMgr, 16)) {
        if (orig_IsKeyPressed(pInputMgr, crKey)) {
            return 1;
        }
    }

    if ((GetAsyncKeyState(crKey) & 0x8000) != 0) {
        return 1;
    }

    JOYINFOEX joy;
    joy.dwSize = sizeof(joy);
    joy.dwFlags = JOY_RETURNBUTTONS;
    if (joyGetPosEx(0, &joy) == JOYERR_NOERROR) {
        int btnIdx = GetConceptReactorJoyButton(crKey);
        if (btnIdx >= 0 && btnIdx < 32) {
            if (joy.dwButtons & (1 << btnIdx)) {
                return 1;
            }
        }
    }

    return 0;
}

static volatile int g_RefrainWindowState = 0;
static volatile int g_RePromptFrames = 0;
static volatile int g_RefrainSubMenuCursor = 0;
static volatile int g_SelectedReMode = 0;
static volatile int g_CancelDebounceActive = 0;
static volatile int g_CancelDebounceTimer = 0;
static volatile int g_DifficultyConfirmed = 0;

static int IsCancelHeld(void) {
    if ((GetAsyncKeyState('X') & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) return 1;
    void *pInputMgr = *(void**)g_A->InputMgr;
    if (pInputMgr && !IsBadReadPtr(pInputMgr, 16)) {
        if (orig_IsKeyPressed(pInputMgr, 'X') || orig_IsKeyPressed(pInputMgr, VK_ESCAPE) ||
            orig_IsKeyPressed(pInputMgr, 0x1B) || orig_IsKeyPressed(pInputMgr, 0x2D)) {
            return 1;
        }
    }
    for (UINT jid = 0; jid < 4; jid++) {
        JOYINFOEX joy;
        joy.dwSize = sizeof(joy);
        joy.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(jid, &joy) == JOYERR_NOERROR) {
            if (joy.dwButtons & 2) return 1;
        }
    }
    return 0;
}

static int IsRefrainWindowOpen(void) {
    DWORD *pTasks = (DWORD*)g_A->Tasks;
    for (DWORD i = 0; i < 6000; i++) {
        DWORD pTask = pTasks[i];
        if (pTask >= 0x10000 && pTask < 0x7FFF0000 && ((uintptr_t)pTask & 3) == 0) {
            if (*(DWORD*)pTask == g_A->vtTask) {
                DWORD fn = *(DWORD*)(pTask + 0x1C);
                DWORD active = *(DWORD*)(pTask + 0x20);
                if (fn >= 82 && fn <= 112 && active > 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void SetGameInfoInt(const char *key, int value) {
    if (!key) return;
    DWORD oldMode = VD(GameMode);
    VD(GameMode) = 1;
    __asm__ __volatile__(
        "pushl %0\n\t"
        "movl $0xffffffff, %%edx\n\t"
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        "addl $4, %%esp\n\t"
        :
        : "r"((DWORD)value), "r"(key), "r"(g_A->fnSetGameInfoInt)
        : "eax", "ecx", "edx", "memory"
    );
    VD(GameMode) = oldMode;
    LogMessage("SetGameInfoInt (0x0044fca0): key='%s' -> set to %d", key, value);
}

static unsigned char g_GlobalDatBackup[4096];
static size_t g_GlobalDatBackupLen = 0;

static void BackupGlobalData(void) {
    FILE *fp = fopen("global.dat", "rb");
    if (fp) {
        g_GlobalDatBackupLen = fread(g_GlobalDatBackup, 1, sizeof(g_GlobalDatBackup), fp);
        fclose(fp);
        LogMessage("BackupGlobalData: backed up %lu bytes from global.dat", (unsigned long)g_GlobalDatBackupLen);
    }
}

static void RestoreGlobalData(void) {
    if (g_GlobalDatBackupLen > 0) {
        FILE *fp = fopen("global.dat", "wb");
        if (fp) {
            fwrite(g_GlobalDatBackup, 1, g_GlobalDatBackupLen, fp);
            fclose(fp);
            LogMessage("RestoreGlobalData: restored %lu bytes to global.dat", (unsigned long)g_GlobalDatBackupLen);
        }
        g_GlobalDatBackupLen = 0;
    }
    typedef void (__cdecl *PFN_LoadGlobalData)(void);
    PFN_LoadGlobalData pfnLoadGlobalData = (PFN_LoadGlobalData)g_A->fnLoadGlobal;
    pfnLoadGlobalData();
    LogMessage("RestoreGlobalData: reloaded clean global data via LoadGlobalData (0x0042a8e0)");
}

int __cdecl HandleSaveGlobalData(void) {
    if (g_PracticeMode) {
        LogMessage("SaveGlobalData (0x0042c330): BLOCKED during Practice Mode to protect global.dat");
        return 1;
    }
    return 0;
}

void __attribute__((naked)) Hook_SaveGlobalData(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "call _HandleSaveGlobalData\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "popal\n\t"
        "pushl %%ebp\n\t"
        "movl %%esp, %%ebp\n\t"
        "pushl $-1\n\t"
        "movl _g_SaveGlobalRetAddr, %%eax\n\t"
        "jmp *%%eax\n\t"
        "1:\n\t"
        "popal\n\t"
        "movl $1, %%eax\n\t"
        "ret\n\t"
        :
        :
    );
}

int __cdecl HandleSetGameInfoIntHook(const char *key, int format, int value) {
    if (!key || IsBadReadPtr(key, 1)) return 0;
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("SetGameInfoInt raw: key='%s' value=%d format=%d practice=%d", key, value, format, g_PracticeMode);
#endif

    if (g_PracticeMode) {
        if (strcmp(key, "GameInfo_RefRain") == 0 || strcmp(key, "lastRefRain") == 0 ||
            strcmp(key, "GameInfo_ReplaySave") == 0 || strcmp(key, "ReplaySave") == 0) {
            return 0;
        }
        LogMessage("SetGameInfoInt (0x0044fca0): BLOCKED key='%s' (val=%d, fmt=%d) during Practice Mode",
                   key, value, format);
        return 1;
    }
    return 0;
}

void __attribute__((naked)) Hook_SetGameInfoInt(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl 36(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "call _HandleSetGameInfoIntHook\n\t"
        "addl $12, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "popal\n\t"
        "pushl %%ebp\n\t"
        "movl %%esp, %%ebp\n\t"
        "pushl $-1\n\t"
        "movl _g_SetGameInfoIntRetAddr, %%eax\n\t"
        "jmp *%%eax\n\t"
        "1:\n\t"
        "popal\n\t"
        "ret\n\t"
        :
        :
    );
}

static int OrigGetGameInfoInt(const char *key, int format) {
    if (!key) return -1;
    int res = -1;
    __asm__ __volatile__(
        "pushl %2\n\t"
        "pushl %1\n\t"
        "call 1f\n\t"
        "addl $8, %%esp\n\t"
        "movl %%eax, %0\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl 4(%%esp), %%ecx\n\t"
        "movl 8(%%esp), %%edx\n\t"
        "pushl %%ebp\n\t"
        "movl %%esp, %%ebp\n\t"
        "pushl $-1\n\t"
        "movl %3, %%eax\n\t"
        "jmp *%%eax\n\t"
        "2:\n\t"
        : "=r"(res)
        : "r"(key), "r"(format), "r"(g_A->fnGetGameInfoIntRet)
        : "eax", "ecx", "edx", "memory"
    );
    return res;
}

unsigned char __attribute__((thiscall)) Hook_IsKeyTriggered(void *this, int key) {
    DWORD currentScene = VD(Scene);
    if (g_ShowStageMenu) return 0;

    if (g_CancelDebounceActive && MatchSimulatedKey(SIMKEY_CANCEL, key)) {
        return 0;
    }

    if (g_AutoAdvanceToDifficulty && currentScene == 1) {
        if (g_SimulatedKeyAction != SIMKEY_NONE && MatchSimulatedKey(g_SimulatedKeyAction, key)) {
            LogMessage("Hook_IsKeyTriggered: SIMULATED MATCH action=%d key=0x%02X", g_SimulatedKeyAction, key);
            return 1;
        }
        return 0;
    }

    unsigned char res = orig_IsKeyTriggered(this, key);
    if (res) PauseDiagKey(key);
    /* The stage script polls ESC (0x1B) via IsKeyTriggered every frame it runs and opens the
       pause menu on the trigger; the menu itself then consumes all further ESC presses (no more
       0x1B triggers until the menu is gone), and the menu polls Z/X/up/down through this same
       hook. So a 0x1B trigger in a stage scene is the game's own "pause menu opened" event -
       log evidence: exactly one key=0x1B per session, coinciding with the menu opening. */
    if (res && key == 0x1B &&
        ((currentScene >= 2 && currentScene <= 7) || currentScene == 10)) {
        if (!g_PauseMenuOpen)
            LogMessage("Pause menu state: open (ESC trigger, scene %lu)", currentScene);
        g_PauseMenuOpen = 1;
        g_LastPauseOpenTick = GetTickCount();
    }
    if (g_PauseMenuOpen && res &&
        (key == 'Z' || key == VK_RETURN || key == VK_SPACE || key == 'X' || key == 0x58)) {
        g_PauseMenuOpen = 0;
        LogMessage("Pause menu state: closed (decide)");
    }

    if (currentScene == 1 && g_RefrainWindowState == 1 && IsRefrainWindowOpen() && !g_ShowStageMenu && !g_AutoAdvanceToDifficulty && !IsReplayOrDemo() && res) {
        if (key == VK_UP || key == 'W' || key == 'w' || key == 0x26 || key == 0xC8) {
            DWORD diff = VD(Diff);
            DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
            if (g_RefrainSubMenuCursor > 0) g_RefrainSubMenuCursor--;
            if (g_RefrainSubMenuCursor == 1 && !(diff >= 2 && isUnlocked)) g_RefrainSubMenuCursor = 0;
            g_SelectedReMode = (g_RefrainSubMenuCursor == 1 && diff >= 2 && isUnlocked) ? 1 : 0;
            SetGameInfoInt("lastRefRain", g_SelectedReMode);
            LogMessage("Hook_IsKeyTriggered: UP triggered -> subCursor=%d, SelectedReMode=%d", g_RefrainSubMenuCursor, g_SelectedReMode);
        } else if (key == VK_DOWN || key == 'S' || key == 's' || key == 0x28 || key == 0xD0) {
            DWORD diff = VD(Diff);
            DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
            if (g_RefrainSubMenuCursor == 0 && diff >= 2 && isUnlocked) {
                g_RefrainSubMenuCursor = 1;
                g_SelectedReMode = 1;
                SetGameInfoInt("lastRefRain", 1);
                LogMessage("Hook_IsKeyTriggered: DOWN triggered -> subCursor=1, SelectedReMode=1");
            } else if (g_RefrainSubMenuCursor <= 1) {
                g_RefrainSubMenuCursor = 2;
                g_SelectedReMode = 0;
                SetGameInfoInt("lastRefRain", 0);
                LogMessage("Hook_IsKeyTriggered: DOWN triggered -> subCursor=2 (Tutorial), SelectedReMode=0");
            } else {
                LogMessage("Hook_IsKeyTriggered: DOWN triggered (already subCursor=2 Tutorial) -> no change");
            }
        } else if (key == 'X' || key == 'x' || key == VK_ESCAPE || key == 0x1B || key == 0x2D || key == 0x01) {
            g_RefrainWindowState = 0;
            LogMessage("Hook_IsKeyTriggered: CANCEL triggered -> windowState=0 (preserved subCursor=%d, SelectedReMode=%d)",
                       g_RefrainSubMenuCursor, g_SelectedReMode);
        }
    }

    if (MatchSimulatedKey(SIMKEY_DECIDE, key)) {
        static int s_pollDecideDown = 0;
        int downNow = (GetAsyncKeyState(key) & 0x8000) != 0;
        int freshEdge = downNow && !s_pollDecideDown;
        s_pollDecideDown = downNow;
        if (IsRefrainWindowOpen() && g_RefrainWindowState == 1 && g_RePromptFrames >= 2 &&
            g_RefrainSubMenuCursor != 2 &&
            freshEdge && IsConceptReactorHeld()) {
            DWORD diff = VD(Diff);
            DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
            g_RefrainSubMenuCursor = (g_RefrainSubMenuCursor == 1 && diff >= 2 && isUnlocked) ? 1 : 0;
            g_SelectedReMode = g_RefrainSubMenuCursor;
            g_ShowStageMenu = 1;
            BurnMenuOpenEdges();
            g_ActiveColumn = 0;
            g_StageSelected = 0;
            g_PracticeMode = 1;
            DWORD curChar = VD(Char);
            if (curChar <= 2) g_PracticeChar = curChar;
            PlayGameSE(1040);
            LogMessage("DECIDE key=0x%02X with Concept Reactor hold on Re-mode prompt -> swallowing confirm, Stage Menu opens NOW (char=%lu, diff=%lu, subCursor=%d, ReMode=%d)",
                       key, g_PracticeChar, diff, g_RefrainSubMenuCursor, g_SelectedReMode);
            return 0;
        }
    }

    return res;
}

DWORD __cdecl HandleSetLives(DWORD origLives) {
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("SetLives (Native 77) raw: arg=%lu (0x%08X) steam=%d pending=%d replay=%d",
               (unsigned long)origLives, (unsigned long)origLives, g_IsSteam, g_PendingStageInitStats, IsReplayOrDemo());
#endif
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        LogMessage("SetLives (Native 77): intercepted script call (%lu) -> overriding to %d",
                   origLives, g_SelectedLives);
        return (DWORD)g_SelectedLives;
    }
    return origLives;
}

void __attribute__((naked)) Hook_SetLives(void) {
    __asm__ __volatile__(
        "cmpl $0, _g_IsSteam\n\t"
        "jne 1f\n\t"
        "pushl 0x8(%%ebp)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "pushl 0x8(%%ebx)\n\t"
        "2:\n\t"
        "call _HandleSetLives\n\t"
        "addl $4, %%esp\n\t"
        "pushl %%edx\n\t"
        "movl _g_A_Lives, %%edx\n\t"
        "movl %%eax, (%%edx)\n\t"
        "popl %%edx\n\t"
        "cmpl $0, _g_IsSteam\n\t"
        "jne 3f\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        "3:\n\t"
        "movl _g_DispatchEpilogue, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

DWORD __cdecl HandleSetMefa(DWORD origDword) {
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("SetMefa (Native 54) raw: arg=%lu (0x%08X) steam=%d pending=%d replay=%d",
               (unsigned long)origDword, (unsigned long)origDword, g_IsSteam, g_PendingStageInitStats, IsReplayOrDemo());
#endif
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
        "cmpl $0, _g_IsSteam\n\t"
        "jne 1f\n\t"
        "pushl 0x8(%%ebp)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "pushl 0x8(%%ebx)\n\t"
        "2:\n\t"
        "call _HandleSetMefa\n\t"
        "addl $4, %%esp\n\t"
        "pushl %%edx\n\t"
        "movl _g_A_Mefa, %%edx\n\t"
        "movl %%eax, (%%edx)\n\t"
        "popl %%edx\n\t"
        "cmpl $0, _g_IsSteam\n\t"
        "jne 3f\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        "3:\n\t"
        "movl _g_DispatchEpilogue, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

DWORD __cdecl HandleSetCR(DWORD origDword) {
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("SetCR (Native 55) raw: arg=%lu (0x%08X) steam=%d pending=%d replay=%d",
               (unsigned long)origDword, (unsigned long)origDword, g_IsSteam, g_PendingStageInitStats, IsReplayOrDemo());
#endif
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
        "cmpl $0, _g_IsSteam\n\t"
        "jne 1f\n\t"
        "pushl 0x8(%%ebp)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "pushl 0x8(%%ebx)\n\t"
        "2:\n\t"
        "call _HandleSetCR\n\t"
        "addl $4, %%esp\n\t"
        "pushl %%edx\n\t"
        "movl _g_A_Cr, %%edx\n\t"
        "movl %%eax, (%%edx)\n\t"
        "popl %%edx\n\t"
        "cmpl $0, _g_IsSteam\n\t"
        "jne 3f\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        "3:\n\t"
        "movl _g_DispatchEpilogue, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

DWORD __cdecl HandleSetDifficulty(DWORD diff) {
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("SetDifficulty (Native 79) raw: arg=%lu (0x%08X) steam=%d pending=%d replay=%d",
               (unsigned long)diff, (unsigned long)diff, g_IsSteam, g_PendingStageInitStats, IsReplayOrDemo());
#endif
    g_RefrainWindowState = 1;
    g_DifficultyConfirmed = 1;
    DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
    if (!g_AutoAdvanceToDifficulty) {
        int nativeVal = OrigGetGameInfoInt("lastRefRain", -1);
        int effectiveRe = (nativeVal == 1 && diff >= 2 && isUnlocked) ? 1 : 0;
        g_RefrainSubMenuCursor = effectiveRe;
        g_SelectedReMode = effectiveRe;
    } else {
        int effectiveRe = (g_SelectedReMode && diff >= 2 && isUnlocked) ? 1 : 0;
        g_RefrainSubMenuCursor = effectiveRe;
        g_SelectedReMode = effectiveRe;
    }
    LogMessage("SetDifficulty (Native 79): intercepted diff=%lu -> subCursor=%d, ReMode=%d (autoNav=%d)",
               diff, g_RefrainSubMenuCursor, g_SelectedReMode, g_AutoAdvanceToDifficulty);
    return diff;
}

void __attribute__((naked)) Hook_SetDifficulty(void) {
    __asm__ __volatile__(
        "cmpl $0, _g_IsSteam\n\t"
        "jne 1f\n\t"
        "pushl 0x8(%%ebp)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "pushl 0x8(%%ebx)\n\t"
        "2:\n\t"
        "call _HandleSetDifficulty\n\t"
        "addl $4, %%esp\n\t"
        "pushl %%edx\n\t"
        "movl _g_A_Diff, %%edx\n\t"
        "movl %%eax, (%%edx)\n\t"
        "popl %%edx\n\t"
        "cmpl $0, _g_IsSteam\n\t"
        "jne 3f\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        "3:\n\t"
        "movl _g_DispatchEpilogue, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

int __cdecl HandleGetGameInfoInt(const char *key, int format) {
    if (!key || IsBadReadPtr(key, 1)) return -1;

    DWORD diff = VD(Diff);
    DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
    int isRe = (g_SelectedReMode && diff >= 2 && isUnlocked) ? 1 : 0;

    if (strcmp(key, "GameInfo_RefRain") == 0) {
        if (g_PracticeMode) {
            LogMessage("GetGameInfoInt (0x0044fb80): key='GameInfo_RefRain' (Practice, diff=%lu, isRe=%d) -> returning %d",
                       diff, isRe, isRe);
            return isRe;
        } else {
            return -1;
        }
    }

    if (strcmp(key, "lastRefRain") == 0) {
        if (g_PracticeMode) {
            LogMessage("GetGameInfoInt (0x0044fb80): key='lastRefRain' (Practice, diff=%lu, isRe=%d) -> returning %d",
                       diff, isRe, isRe);
            return isRe;
        } else {
            int nativeVal = OrigGetGameInfoInt(key, format);
            int effectiveRe = (nativeVal == 1 && diff >= 2 && isUnlocked) ? 1 : 0;
            g_RefrainSubMenuCursor = effectiveRe;
            g_SelectedReMode = effectiveRe;
            LogMessage("GetGameInfoInt (0x0044fb80): key='lastRefRain' (Title, diff=%lu) -> nativeVal=%d, effectiveRe=%d -> synced subCursor=%d, SelectedReMode=%d",
                       diff, nativeVal, effectiveRe, g_RefrainSubMenuCursor, g_SelectedReMode);
            return nativeVal;
        }
    }

    if (strcmp(key, "GameInfo_ReplaySave") == 0) {
        int val = OrigGetGameInfoInt(key, format);
        LogMessage("GetGameInfoInt (0x0044fb80): key='GameInfo_ReplaySave' -> %d (Replay saving %s)",
                   val, val ? "ENABLED" : "DISABLED");
        return val;
    }

    if (g_PracticeMode && VD(Scene) == 2) {
        if (strcmp(key, "GameInfo_NoMiss") == 0 ||
            strncmp(key, "stageClear2", 11) == 0 ||
            strncmp(key, "stageClear", 10) == 0) {
            if (VD(SceneNext) != 1) {
                LogMessage("GetGameInfoInt (0x0044fb80): Stage 5 clear detected (key='%s', format=%d, stage=%d) in Practice Mode -> redirecting to Title immediately to skip flavor text",
                           key, format, g_PracticeStage);
                VD(SceneNext) = 1;
            }
        }
    }

    if (g_PracticeMode && key[0] != '\0') {
        LogMessage("GetGameInfoInt (0x0044fb80): queried key='%s', format=%d", key, format);
    }

    return -1;
}

void __attribute__((naked)) Hook_GetGameInfoInt(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "call _HandleGetGameInfoInt\n\t"
        "addl $8, %%esp\n\t"
        "cmpl $-1, %%eax\n\t"
        "jne 1f\n\t"
        "popal\n\t"
        "pushl %%ebp\n\t"
        "movl %%esp, %%ebp\n\t"
        "pushl $-1\n\t"
        "movl _g_GetGameInfoIntRetAddr, %%eax\n\t"
        "jmp *%%eax\n\t"
        "1:\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"
        "ret\n\t"
        :
        :
    );
}

DWORD __cdecl HandleGetGameInfoRefRainPractice(void) {
    DWORD diff = VD(Diff);
    DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
    if (g_SelectedReMode && diff >= 2 && isUnlocked) {
        LogMessage("HandleGetGameInfoRefRain (0x0043078E): Practice Re:MODE -> returning 1 (diff=%lu)", diff);
        return 1;
    } else {
        LogMessage("HandleGetGameInfoRefRain (0x0043078E): Practice Normal MODE -> returning 0 (diff=%lu)", diff);
        return 0;
    }
}

void __attribute__((naked)) Hook_GetGameInfoRefRain(void) {
    __asm__ __volatile__(
        "cmpl $0, _g_PracticeMode\n\t"
        "jz 1f\n\t"
        "pushal\n\t"
        "call _HandleGetGameInfoRefRainPractice\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"
        "ret\n\t"
        "1:\n\t"
        "movl _g_GetGameInfoIntFn, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

DWORD __cdecl HandleGetRefRainConfig(DWORD *pVal) {
    DWORD diff = VD(Diff);
    DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("GetRefRainConfig: pVal=%p raw=%08X practice=%d reMode=%d diff=%lu unlocked=%lu",
               pVal, (pVal && !IsBadReadPtr(pVal, 4)) ? *(DWORD*)pVal : 0xFFFFFFFFu,
               g_PracticeMode, g_SelectedReMode, diff, isUnlocked);
#endif
    if (g_PracticeMode) {
        if (g_SelectedReMode && diff >= 2 && isUnlocked) {
            LogMessage("HandleGetRefRainConfig (0x00441229): Practice Re:MODE -> overriding to 1 (diff=%lu)", diff);
            return 1;
        } else {
            LogMessage("HandleGetRefRainConfig (0x00441229): Practice Normal MODE -> overriding to 0 (diff=%lu)", diff);
            return 0;
        }
    }
    if (pVal && !IsBadReadPtr(pVal, 4)) {
        return *pVal;
    }
    return 0;
}

void __attribute__((naked)) Hook_GetRefRainConfig(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "pushl %%eax\n\t"
        "call _HandleGetRefRainConfig\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"
        "movl $0xffffffff, -0x4(%%ebp)\n\t"
        "pushl _g_RefRainCfgRetAddr\n\t"
        "ret\n\t"
        :
        :
    );
}

DWORD __cdecl HandleGetterReMode(void) {
    DWORD currentScene = VD(Scene);
    if (currentScene == 1) {
        if (!IsBadReadPtr((void*)g_A->Unlock, 4)) {
            return VD(Unlock) ? 1 : 0;
        }
        return 0;
    }

    DWORD diff = VD(Diff);
    if (g_PracticeMode) {
        int res = (g_SelectedReMode && diff >= 2) ? 1 : 0;
        LogMessage("HandleGetterReMode (Stage Practice): diff=%lu, g_SelectedReMode=%d -> returning %d",
                   diff, g_SelectedReMode, res);
        return (DWORD)res;
    }

    char *pInputMgr = *(char**)g_A->InputMgr;
    if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 16)) {
        char *pGS = *(char**)(pInputMgr + 8);
        if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x60)) {
            return *(BYTE*)(pGS + 0x52) ? 1 : 0;
        }
    }
    return (VD(IsRefRain) == 1) ? 1 : 0;
}

void __attribute__((naked)) Hook_GetterReMode(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "call _HandleGetterReMode\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"
        "movl %%eax, (%%esi)\n\t"
        "movl %%esi, %%eax\n\t"
        "popl %%esi\n\t"
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

    int isHD = (vp.Width >= 1000);

    float totalW, startX;
    float stageBoxH, paramBoxH, footerH, rowGap;
    float headerH, stageLineSpacing, paramLineSpacing;
    float colW, colGap;
    float stageHighlightH, paramHighlightH;

    if (isHD) {
        totalW = 1200.0f;
        paramBoxH = 325.0f;
        footerH = 56.0f;
        rowGap = 8.0f;
        headerH = 30.0f;
        stageLineSpacing = 27.0f;
        paramLineSpacing = 25.0f;
        colGap = 16.0f;
        colW = (totalW - (3.0f * colGap)) / 4.0f;
        stageHighlightH = 23.0f;
        paramHighlightH = 22.0f;
    } else {
        totalW = 616.0f;
        paramBoxH = 195.0f;
        footerH = 38.0f;
        rowGap = 5.0f;
        headerH = 20.0f;
        stageLineSpacing = 17.0f;
        paramLineSpacing = 15.0f;
        colGap = 8.0f;
        colW = (totalW - (3.0f * colGap)) / 4.0f;
        stageHighlightH = 15.0f;
        paramHighlightH = 14.0f;
    }

    int visibleStages = GetSelectableStageCount();
    if (g_MenuCursor >= visibleStages) g_MenuCursor = visibleStages - 1;
    if (g_MenuCursor < 0) g_MenuCursor = 0;
    stageBoxH = (isHD ? 40.0f : 30.0f) + (float)visibleStages * stageLineSpacing;

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

    DWORD curChar = g_PracticeChar;
    if (!IsBadReadPtr((void*)g_A->Char, 4)) curChar = VD(Char);
    const PracticeMenuTheme *theme = GetPracticeMenuTheme(curChar);

    DWORD borderCol0 = (g_ActiveColumn == 0) ? theme->borderOn : theme->borderOff;
    DWORD borderCol1 = (g_ActiveColumn == 1) ? theme->borderOn : theme->borderOff;
    DWORD borderCol2 = (g_ActiveColumn == 2) ? theme->borderOn : theme->borderOff;
    DWORD borderCol3 = (g_ActiveColumn == 3) ? theme->borderOn : theme->borderOff;
    DWORD borderCol4 = (g_ActiveColumn == 4) ? theme->borderOn : theme->borderOff;

    DWORD headerBg0  = (g_ActiveColumn == 0) ? theme->headerOn : theme->headerOff;
    DWORD headerBg1  = (g_ActiveColumn == 1) ? theme->headerOn : theme->headerOff;
    DWORD headerBg2  = (g_ActiveColumn == 2) ? theme->headerOn : theme->headerOff;
    DWORD headerBg3  = (g_ActiveColumn == 3) ? theme->headerOn : theme->headerOff;
    DWORD headerBg4  = (g_ActiveColumn == 4) ? theme->headerOn : theme->headerOff;

    DWORD titleCol0  = (g_ActiveColumn == 0) ? theme->titleOn : theme->titleOff;
    DWORD titleCol1  = (g_ActiveColumn == 1) ? theme->titleOn : theme->titleOff;
    DWORD titleCol2  = (g_ActiveColumn == 2) ? theme->titleOn : theme->titleOff;
    DWORD titleCol3  = (g_ActiveColumn == 3) ? theme->titleOn : theme->titleOff;
    DWORD titleCol4  = (g_ActiveColumn == 4) ? theme->titleOn : theme->titleOff;

    DWORD diff = VD(Diff);
    const char *diffLabel = "NORMAL";
    if (diff == 1) diffLabel = "EASY";
    else if (diff == 2) diffLabel = (g_SelectedReMode ? "Re:NORMAL" : "NORMAL");
    else if (diff == 3) diffLabel = (g_SelectedReMode ? "Re:ADVANCED" : "ADVANCED");
    else if (diff == 4) diffLabel = (g_SelectedReMode ? "Re:EXTREME" : "EXTREME");

    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "STAGE SELECT [%s]", diffLabel);
    DWORD titleCol = (g_SelectedReMode && diff >= 2) ? 0xFFFF4444 : titleCol0;
    DWORD hBg0 = (g_SelectedReMode && diff >= 2) ? 0x88661122 : headerBg0;

    DrawFrame(pDevice, startX, stageBoxY, totalW, stageBoxH, 2.0f, borderCol0);
    DrawSolidRect(pDevice, startX, stageBoxY, totalW, stageBoxH, theme->panel);
    DrawSolidRect(pDevice, startX + 4.0f, stageBoxY + 4.0f, totalW - 8.0f, headerH, hBg0);
    DrawShadowText(g_pTitleFont, titleBuf, (int)startX + (isHD ? 14 : 10), (int)stageBoxY + (isHD ? 8 : 5), titleCol);

    int stageStartY = (int)stageBoxY + (isHD ? 36 : 24);
    for (int i = 0; i < visibleStages; i++) {
        int itemY = stageStartY + (int)(i * stageLineSpacing);
        char buf[128];
        if (i == g_MenuCursor) {
            if (g_ActiveColumn == 0) {
                DrawSolidRect(pDevice, startX + 4.0f, (float)itemY - 1.0f, totalW - 8.0f, stageHighlightH, theme->hiOn);
                snprintf(buf, sizeof(buf), ">> %s", g_MenuItems[i].label);
                DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, theme->hiTextOn);
            } else {
                DrawSolidRect(pDevice, startX + 4.0f, (float)itemY - 1.0f, totalW - 8.0f, stageHighlightH, theme->hiSel);
                snprintf(buf, sizeof(buf), ">> %s", g_MenuItems[i].label);
                DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, theme->hiTextSel);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_MenuItems[i].label);
            DrawShadowText(g_pStageFont ? g_pStageFont : g_pFont, buf, (int)startX + (isHD ? 12 : 8), itemY, (g_ActiveColumn == 0) ? theme->itemOn : theme->itemOff);
        }
    }

    int paramStartY = (int)paramBoxY + (isHD ? 36 : 24);

    DrawFrame(pDevice, xLives, paramBoxY, colW, paramBoxH, 2.0f, borderCol1);
    DrawSolidRect(pDevice, xLives, paramBoxY, colW, paramBoxH, theme->panel);
    DrawSolidRect(pDevice, xLives + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg1);
    DrawShadowText(g_pTitleFont, "LIVES", (int)xLives + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol1);

    for (int i = 0; i < LIFE_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_LifeCursor) {
            if (g_ActiveColumn == 1) {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiOn);
                snprintf(buf, sizeof(buf), ">> %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, theme->hiTextOn);
            } else if (g_ActiveColumn > 1) {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiSel);
                snprintf(buf, sizeof(buf), ">> %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, theme->hiTextSel);
            } else {
                DrawSolidRect(pDevice, xLives + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_LifeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_LifeItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xLives + 6, itemY, (g_ActiveColumn == 1) ? theme->itemOn : theme->dimText);
        }
    }

    DrawFrame(pDevice, xMefa, paramBoxY, colW, paramBoxH, 2.0f, borderCol2);
    DrawSolidRect(pDevice, xMefa, paramBoxY, colW, paramBoxH, theme->panel);
    DrawSolidRect(pDevice, xMefa + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg2);
    DrawShadowText(g_pTitleFont, "M.E.F.A.2", (int)xMefa + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol2);

    for (int i = 0; i < MEFA_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_MefaCursor) {
            if (g_ActiveColumn == 2) {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiOn);
                snprintf(buf, sizeof(buf), ">> %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, theme->hiTextOn);
            } else if (g_ActiveColumn > 2) {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiSel);
                snprintf(buf, sizeof(buf), ">> %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, theme->hiTextSel);
            } else {
                DrawSolidRect(pDevice, xMefa + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_MefaItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_MefaItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xMefa + 6, itemY, (g_ActiveColumn == 2) ? theme->itemOn : theme->dimText);
        }
    }

    DrawFrame(pDevice, xCrStock, paramBoxY, colW, paramBoxH, 2.0f, borderCol3);
    DrawSolidRect(pDevice, xCrStock, paramBoxY, colW, paramBoxH, theme->panel);
    DrawSolidRect(pDevice, xCrStock + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg3);
    DrawShadowText(g_pTitleFont, "CR STOCK", (int)xCrStock + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol3);

    for (int i = 0; i < CR_STOCK_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_CrStockCursor) {
            if (g_ActiveColumn == 3) {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiOn);
                snprintf(buf, sizeof(buf), ">> %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, theme->hiTextOn);
            } else if (g_ActiveColumn > 3) {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiSel);
                snprintf(buf, sizeof(buf), ">> %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, theme->hiTextSel);
            } else {
                DrawSolidRect(pDevice, xCrStock + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_CrStockItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_CrStockItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xCrStock + 6, itemY, (g_ActiveColumn == 3) ? theme->itemOn : theme->dimText);
        }
    }

    DrawFrame(pDevice, xCrGauge, paramBoxY, colW, paramBoxH, 2.0f, borderCol4);
    DrawSolidRect(pDevice, xCrGauge, paramBoxY, colW, paramBoxH, theme->panel);
    DrawSolidRect(pDevice, xCrGauge + 4.0f, paramBoxY + 4.0f, colW - 8.0f, headerH, headerBg4);
    DrawShadowText(g_pTitleFont, "CR GAUGE", (int)xCrGauge + (isHD ? 12 : 8), (int)paramBoxY + (isHD ? 8 : 5), titleCol4);

    for (int i = 0; i < CR_GAUGE_ITEM_COUNT; i++) {
        int itemY = paramStartY + (int)(i * paramLineSpacing);
        char buf[64];
        if (i == g_CrGaugeCursor) {
            if (g_ActiveColumn == 4) {
                DrawSolidRect(pDevice, xCrGauge + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, theme->hiOn);
                snprintf(buf, sizeof(buf), ">> %s", g_CrGaugeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, theme->hiTextOn);
            } else {
                DrawSolidRect(pDevice, xCrGauge + 4.0f, (float)itemY - 1.0f, colW - 8.0f, paramHighlightH, 0x22112233);
                snprintf(buf, sizeof(buf), ">  %s", g_CrGaugeItems[i].label);
                DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, 0xFF888888);
            }
        } else {
            snprintf(buf, sizeof(buf), "   %s", g_CrGaugeItems[i].label);
            DrawShadowText(g_pFont, buf, (int)xCrGauge + 6, itemY, (g_ActiveColumn == 4) ? theme->itemOn : theme->dimText);
        }
    }

    DrawFrame(pDevice, startX, footerY, totalW, footerH, 2.0f, theme->footerBorder);
    DrawSolidRect(pDevice, startX, footerY, totalW, footerH, theme->panel);

    char help1Buf[128];
    const char *help1 = "";
    const char *help2 = "";
    if (g_ActiveColumn == 0) {
        snprintf(help1Buf, sizeof(help1Buf),
                 "[UP/DOWN] Select Stage    [Z/ENTER/RIGHT] Next    [1-%d] Quick Select",
                 visibleStages);
        help1 = help1Buf;
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
    char help2Buf[256];
    DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
    if (diff >= 2 && isUnlocked) {
        const char *modeLabel;
        if (diff == 3)      modeLabel = (g_SelectedReMode ? "Re:ADVANCED" : "ADVANCED");
        else if (diff == 4) modeLabel = (g_SelectedReMode ? "Re:EXTREME"  : "EXTREME");
        else                modeLabel = (g_SelectedReMode ? "Re:NORMAL"   : "NORMAL");
        snprintf(help2Buf, sizeof(help2Buf), "%s    [R] Mode: %s",
                 help2, modeLabel);
        help2 = help2Buf;
    }
    DrawShadowText(pFootFont, help1, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 8 : 5), theme->foot1);
    DrawShadowText(pFootFont, help2, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 32 : 23), theme->foot2);

    int trig_up = 0, trig_down = 0, trig_left = 0, trig_right = 0;
    int trig_btn1 = 0, trig_btn2 = 0, trig_btn3 = 0;

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
        if (((joy.dwButtons & 4) && !(prev_buttons & 4)) || ((joy.dwButtons & 8) && !(prev_buttons & 8))) trig_btn3 = 1;
        prev_buttons = joy.dwButtons;
    }

    int do_up = IsKeyTriggered(VK_UP) || IsKeyTriggered('W') || trig_up;
    int do_down = IsKeyTriggered(VK_DOWN) || IsKeyTriggered('S') || trig_down;
    int do_left = IsKeyTriggered(VK_LEFT) || IsKeyTriggered('A') || trig_left;
    int do_right = IsKeyTriggered(VK_RIGHT) || IsKeyTriggered('D') || trig_right;
    int do_decide = IsKeyTriggered('Z') || IsKeyTriggered(VK_RETURN) || IsKeyTriggered(VK_SPACE) || trig_btn1;
    int do_cancel = IsKeyTriggered('X') || IsKeyTriggered(VK_ESCAPE) || trig_btn2;

    if (diff >= 2 && isUnlocked && (IsKeyTriggered('R') || IsKeyTriggered('r') || trig_btn3)) {
        g_SelectedReMode = !g_SelectedReMode;
        g_RefrainSubMenuCursor = g_SelectedReMode;
        PlayGameSE(1040);
        LogMessage("Stage Menu: Toggled Re:Mode -> %d", g_SelectedReMode);
    }

    if (do_up) {
        if (g_ActiveColumn == 0) {
            int maxStages = GetSelectableStageCount();
            g_MenuCursor = (g_MenuCursor + maxStages - 1) % maxStages;
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
            int maxStages = GetSelectableStageCount();
            g_MenuCursor = (g_MenuCursor + 1) % maxStages;
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

    if (g_ActiveColumn == 0) {
        int maxStages = GetSelectableStageCount();
        for (int i = 0; i < maxStages; i++) {
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

    if (do_decide) {
        if (g_ActiveColumn == 0) {
            g_ActiveColumn = 1;
            PlayGameSE(1040);
            LogMessage("Stage confirmed: %d -> moving to Lives", g_MenuItems[g_MenuCursor].stage);
        } else if (g_ActiveColumn == 1) {
            UpdateSelectedLives();
            g_ActiveColumn = 2;
            PlayGameSE(1040);
            LogMessage("Lives confirmed: %d -> moving to MEFA", g_SelectedLives);
        } else if (g_ActiveColumn == 2) {
            UpdateSelectedMefa();
            g_ActiveColumn = 3;
            PlayGameSE(1040);
            LogMessage("MEFA confirmed: %d stocks (0x%08X) -> moving to CR Stock",
                       g_MefaItems[g_MefaCursor].value, g_SelectedMefaDword);
        } else if (g_ActiveColumn == 3) {
            UpdateSelectedCr();
            g_ActiveColumn = 4;
            PlayGameSE(1040);
            LogMessage("CR Stock confirmed: %d -> moving to CR Gauge", g_CrStockItems[g_CrStockCursor].stock);
        } else if (g_ActiveColumn == 4) {
            UpdateSelectedLives();
            UpdateSelectedMefa();
            UpdateSelectedCr();
            int stage = g_MenuItems[g_MenuCursor].stage;

            DWORD curChar = VD(Char);
            if (curChar <= 2) g_PracticeChar = curChar;

            DWORD diff = VD(Diff);
            if (diff < 1 || diff > 4) diff = 2;

            if (g_SelectedReMode && diff >= 2) {
                LogMessage("All confirmed -> Starting Stage %d in Re:MODE (char=%lu, diff=%lu, Lives=%d, MEFA=0x%08X, CR Stock=%d, CR Gauge=%s)",
                           stage, g_PracticeChar, diff, g_SelectedLives, g_SelectedMefaDword,
                           g_CrStockItems[g_CrStockCursor].stock, g_CrGaugeItems[g_CrGaugeCursor].label);
            } else {
                LogMessage("All confirmed -> Starting Stage %d in Normal MODE (char=%lu, diff=%lu, Lives=%d, MEFA=0x%08X, CR Stock=%d, CR Gauge=%s)",
                           stage, g_PracticeChar, diff, g_SelectedLives, g_SelectedMefaDword,
                           g_CrStockItems[g_CrStockCursor].stock, g_CrGaugeItems[g_CrGaugeCursor].label);
            }

            DWORD isRefRain = (g_SelectedReMode && diff >= 2) ? 1 : 0;
            VD(GameMode) = 1;
            VD(IsRefRain) = isRefRain;
            SetGameInfoInt("GameInfo_RefRain", isRefRain);
            SetGameInfoInt("lastRefRain", isRefRain);
            g_ShowStageMenu = 0;
            g_StageSelected = 1;
            g_PracticeMode = 1;
            g_PracticeStage = stage;
            g_AutoAdvanceToDifficulty = 0;
            g_ActiveColumn = 0;
            g_PendingStageInitStats = 1;
            g_StageInitRenderFrames = 0;
            InterlockedExchange(&g_PendingReset, 1);
            ResetStageScore();
            BackupGlobalData();
            VD(SceneNext) = 2 + stage;
            PlayGameSE(1040);
            LogMessage("All confirmed -> Starting Stage %d (char=%lu, Lives=%d, MEFA stocks=%d, dw=0x%08X, CR Stock=%d, CR Gauge=%s, CR Total=0x%08X)",
                       stage, g_PracticeChar, g_SelectedLives, g_MefaItems[g_MefaCursor].value, g_SelectedMefaDword,
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

    if (do_cancel) {
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
        } else if (g_ActiveColumn == 0) {
            g_ShowStageMenu = 0;
            g_CancelDebounceActive = 1;
            g_CancelDebounceTimer = 8;
            g_StageSelected = 0;
            g_PracticeMode = 0;
            g_PracticeStage = 0;
            RestoreGlobalData();
            g_AutoAdvanceToDifficulty = 0;
            g_AutoNavPhase = 0;
            g_AutoNavTimer = 0;
            g_SimulatedKeyAction = SIMKEY_NONE;
            g_ActiveColumn = 0;
            PlayGameSE(1048);
            DWORD prevScene = VD(Scene);
            if (prevScene != 1) {
                g_CancelToTitle = 1;
                VD(SceneNext) = 1;
                LogMessage("Stage Menu Cancel -> Redirecting to Title (prevScene=%lu)", prevScene);
            } else {
                g_CancelToTitle = 0;
                LogMessage("Stage Menu Cancel -> Closed menu on Title screen (DIVE 2 M.R.S. preserved)");
            }
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

    pDevice->lpVtbl->SetFVF(pDevice, fvf);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ALPHABLENDENABLE, alphaBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_SRCBLEND, srcBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_DESTBLEND, destBlend);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_ZENABLE, zEnable);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_LIGHTING, lighting);
    pDevice->lpVtbl->SetRenderState(pDevice, D3DRS_CULLMODE, cullMode);
}

#ifdef REFRAIN_DEBUG_BUILD
static void PeriodicVarDump(void) {
    static DWORD ctr = 0;
    if ((++ctr % 240) != 0) return;
    LogMessage("VARDUMP: Scene=%lu next=%lu Diff=%lu Char=%lu Lives=%lu Mefa=%08X Cr=%08X ViewIdx=%lu FrameCnt=%lu GameMode=%lu IsRefRain=%lu Unlock=%lu practice=%d reMode=%d pending=%d",
               VD(Scene), VD(SceneNext), VD(Diff), VD(Char), VD(Lives), VD(Mefa), VD(Cr), VD(ViewIdx),
               VD(FrameCnt), VD(GameMode), VD(IsRefRain), VD(Unlock), g_PracticeMode, g_SelectedReMode,
               g_PendingStageInitStats);
    LogMessage("VARDUMP: cfg(GameInfo_RefRain)=%d cfg(lastRefRain)=%d",
               OrigGetGameInfoInt("GameInfo_RefRain", -1), OrigGetGameInfoInt("lastRefRain", -1));
}
#endif

void __cdecl OnRenderHook(IDirect3DDevice9 *pDevice) {
#ifdef REFRAIN_DEBUG_BUILD
    PeriodicVarDump();
#endif
    if (g_LatQpcFreq) {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        LONGLONG now = li.QuadPart, prev = g_LatLastRenderQpc;
        g_LatLastRenderQpc = now;
        DWORD fc = ++g_LatFrameCount;
        if ((fc & 0x1FF) == 0 && g_LatPollAgeCount > 0) {
            double freq = (double)g_LatQpcFreq;
            double fps = (prev && now > prev) ? 1000.0 / ((now - prev) * 1000.0 / freq) : 0.0;
            LogMessage("LATENCY: frames=%lu fps(present-to-present)=%.1f | inputAge@tick avg=%.2f ms max=%.2f ms (n=%lu) | kbPolls=%lu padPolls=%lu (per ~512f)",
                       fc, fps, (g_LatPollAgeAccum * 1000.0 / (freq * g_LatPollAgeCount)),
                       (g_LatPollAgeMax * 1000.0 / freq), g_LatPollAgeCount,
                       g_LatKeyPollCount, g_LatPadPollCount);
            g_LatPollAgeAccum = 0;
            g_LatPollAgeMax = 0;
            g_LatPollAgeCount = 0;
            g_LatKeyPollCount = 0;
            g_LatPadPollCount = 0;
        }
    }

    {
        DWORD curScene = VD(Scene);
        int promptUp = (curScene == 1 && !g_ShowStageMenu && !g_AutoAdvanceToDifficulty &&
                        !IsReplayOrDemo() && IsRefrainWindowOpen() && g_RefrainWindowState == 1);
        if (promptUp) {
            g_RePromptFrames++;
        } else {
            g_RePromptFrames = 0;
        }
    }

    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        union { float f; DWORD dw; } uMefa, uCr;
        uMefa.dw = g_SelectedMefaDword;
        uCr.dw = g_SelectedCrDword;

        VD(Lives) = (DWORD)g_SelectedLives;
        VD(Mefa) = g_SelectedMefaDword;
        VD(Cr) = g_SelectedCrDword;

        char *pGameState = *(char**)g_A->Config;
        if (pGameState && (DWORD)pGameState > 0x10000 && !IsBadReadPtr(pGameState, 0x200)) {
            for (int s = 1; s <= 6; s++) {
                *(DWORD*)(pGameState + s * 4 + 0x115) = (DWORD)g_SelectedLives;
                *(DWORD*)(pGameState + s * 4 + 0x12d) = g_SelectedMefaDword;
                *(DWORD*)(pGameState + s * 4 + 0x145) = g_SelectedCrDword;
            }
            DWORD diff = VD(Diff);
            if (diff < 1 || diff > 4) diff = 2;
            BYTE isRe = (g_SelectedReMode && diff >= 2) ? 1 : 0;
            *(BYTE*)(pGameState + 0x52) = isRe;
        }

        char *pInputMgr = *(char**)g_A->InputMgr;
        if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 16)) {
            char *pGS = *(char**)(pInputMgr + 8);
            if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x60)) {
                DWORD diff = VD(Diff);
                if (diff < 1 || diff > 4) diff = 2;
                BYTE isRe = (g_SelectedReMode && diff >= 2) ? 1 : 0;
                *(BYTE*)(pGS + 0x52) = isRe;
            }
        }

        DWORD diff = VD(Diff);
        if (diff < 1 || diff > 4) diff = 2;
        DWORD isRefRain = (g_SelectedReMode && diff >= 2) ? 1 : 0;
        DWORD curChar = VD(Char);
        if (curChar < 1 || curChar > 3) curChar = 1;
        VD(ViewIdx) = (curChar - 1) + 3 * ((diff - 1) + 4 * isRefRain);
#ifdef REFRAIN_DEBUG_BUILD
        LogMessage("StageInit wrote ViewIdx=%lu (Char=%lu Diff=%lu isRefRain=%lu frame=%lu)",
                   VD(ViewIdx), curChar, diff, isRefRain, VD(FrameCnt));
#endif
        VD(GameMode) = 1;
        VD(IsRefRain) = isRefRain;

        DWORD pPlayer = VD(Player);
        DWORD frameCount = VD(FrameCnt);
        DWORD currentScene = VD(Scene);

        g_StageInitRenderFrames++;

        if ((currentScene >= 2 && currentScene <= 7) || currentScene == 10) {
            if ((pPlayer != 0 && frameCount >= 30) || g_StageInitRenderFrames >= 120) {
                g_PendingStageInitStats = 0;
                g_StageInitRenderFrames = 0;
                LogMessage("Stage startup stats finalized: Lives=%d, MEFA=%f (dw=0x%08X), CR=%f (dw=0x%08X) (frameCount=%lu)",
                           g_SelectedLives, uMefa.f, uMefa.dw, uCr.f, uCr.dw, frameCount);
            }
        }
    }

    DWORD currentScene = VD(Scene);

    static int scriptDumped = 0;
    if (!scriptDumped && currentScene == 1) {
        char *pScript = *(char**)g_A->Script;
        if (pScript && (DWORD)pScript > 0x10000 && !IsBadReadPtr(pScript, 0x100)) {
            DWORD size = *(DWORD*)(pScript + 8);
            if (size > 0 && size < 0x200000 && !IsBadReadPtr(pScript, size)) {
                FILE *fout = fopen("full_title_script.bin", "wb");
                if (fout) {
                    fwrite(pScript, 1, size, fout);
                    fclose(fout);
                    LogMessage("Dumped full Title.rsr script: size=%lu bytes", size);
                    scriptDumped = 1;
                }
            }
        }
    }
    if (g_CancelDebounceActive) {
        if (!IsCancelHeld() || --g_CancelDebounceTimer <= 0) {
            g_CancelDebounceActive = 0;
            g_CancelDebounceTimer = 0;
            LogMessage("Cancel debounce cleared (released=%d)", !IsCancelHeld());
        }
    }

    if (currentScene == 1) {
        if (!IsRefrainWindowOpen()) {
            g_RefrainWindowState = 0;
            if (!g_ShowStageMenu && !g_AutoAdvanceToDifficulty) {
                g_RefrainSubMenuCursor = 0;
                g_SelectedReMode = 0;
            }
        }
    } else {
        g_RefrainWindowState = 0;
        g_RefrainSubMenuCursor = 0;
    }

    if (currentScene == 1 && g_AutoAdvanceToDifficulty) {
        g_AutoNavTimer++;

        if (g_AutoNavPhase == 0) {
            if (g_AutoNavTimer >= 2 && (g_AutoNavTimer % 2 == 0) && !IsRefrainWindowOpen()) {
                g_SimulatedKeyAction = SIMKEY_DECIDE;
            } else {
                g_SimulatedKeyAction = SIMKEY_NONE;
            }
            if (g_AutoNavTimer >= 3 && IsRefrainWindowOpen()) {
                LogMessage("AutoNav: Phase 0 complete! RefRain window opened at timer=%d", g_AutoNavTimer);
                if (g_AutoNavTarget == 1) {
                    g_AutoAdvanceToDifficulty = 0;
                    g_AutoNavPhase = 0;
                    g_AutoNavTimer = 0;
                    g_SimulatedKeyAction = SIMKEY_NONE;
                    g_RefrainWindowState = 1;
                    LogMessage("AutoNav: stopping on the character desktop (Q hotkey)");
                } else {
                    g_AutoNavPhase = 1;
                    g_AutoNavTimer = 0;
                    g_DifficultyConfirmed = 0;
                    g_SimulatedKeyAction = SIMKEY_NONE;
                }
            } else if (g_AutoNavTimer >= 80) {
                if (g_AutoNavTarget == 1) {
                    g_AutoAdvanceToDifficulty = 0;
                    g_AutoNavPhase = 0;
                    g_AutoNavTimer = 0;
                    g_SimulatedKeyAction = SIMKEY_NONE;
                    LogMessage("AutoNav: Phase 0 timeout with desktop target -> releasing input, staying where we are");
                } else {
                    LogMessage("AutoNav: Phase 0 fallback timeout (%d frames) -> advancing to Phase 1", g_AutoNavTimer);
                    g_AutoNavPhase = 1;
                    g_AutoNavTimer = 0;
                    g_DifficultyConfirmed = 0;
                    g_SimulatedKeyAction = SIMKEY_NONE;
                }
            }
        } else if (g_AutoNavPhase == 1) {
            if (g_AutoNavTimer >= 2 && (g_AutoNavTimer % 2 == 0) && !g_DifficultyConfirmed) {
                g_SimulatedKeyAction = SIMKEY_DECIDE;
            } else {
                g_SimulatedKeyAction = SIMKEY_NONE;
            }
            if (g_DifficultyConfirmed) {
                LogMessage("AutoNav: Phase 1 complete! Difficulty confirmed at timer=%d", g_AutoNavTimer);
                g_AutoNavPhase = 2;
                g_AutoNavTimer = 0;
                g_SimulatedKeyAction = SIMKEY_NONE;
            } else if (g_AutoNavTimer >= 60) {
                LogMessage("AutoNav: Phase 1 fallback timeout (%d frames) -> advancing to Phase 2", g_AutoNavTimer);
                g_AutoNavPhase = 2;
                g_AutoNavTimer = 0;
                g_SimulatedKeyAction = SIMKEY_NONE;
            }
        } else if (g_AutoNavPhase == 2) {
            g_SimulatedKeyAction = SIMKEY_NONE;
            if (g_AutoNavTimer >= 5) {
                g_AutoAdvanceToDifficulty = 0;
                g_AutoNavPhase = 0;
                g_AutoNavTimer = 0;
                g_RefrainWindowState = 1;
                g_ShowStageMenu = 1;
                BurnMenuOpenEdges();
                g_ActiveColumn = 0;
                g_StageSelected = 0;
                g_PracticeMode = 1;
                LogMessage("AutoNav: Ready! Both DIVE 2 M.R.S. and Stage Menu open (stage=%d, ReMode=%d)",
                           g_MenuItems[g_MenuCursor].stage, g_SelectedReMode);
            }
        }
    }

    if (g_ShowStageMenu) {
        OnRenderMenu(pDevice);
    }
}

void __cdecl FlushGPUQueue(IDirect3DDevice9 *pDevice) {
    if (!g_LowLatencyMode || !pDevice) return;
    if (!g_pEventQuery) {
        pDevice->lpVtbl->CreateQuery(pDevice, D3DQUERYTYPE_EVENT, &g_pEventQuery);
    }
    if (g_pEventQuery) {
        g_pEventQuery->lpVtbl->Issue(g_pEventQuery, D3DISSUE_END);
        while (g_pEventQuery->lpVtbl->GetData(g_pEventQuery, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        }
    }
}

void __attribute__((naked)) Hook_EndScene(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl 32(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "call _OnRenderHook\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "movl (%%esp), %%ecx\n\t"
        "movl (%%ecx), %%eax\n\t"
        "call *0xa8(%%eax)\n\t"
        "pushal\n\t"
        "movl _g_A_GpuQueue, %%eax\n\t"
        "movl (%%eax), %%eax\n\t"
        "pushl %%eax\n\t"
        "call _FlushGPUQueue\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "jmp *%0\n\t"
        :
        : "m"(g_EndSceneRetAddr)
    );
}

void __cdecl HandleSceneCheck(DWORD *pEdx) {
    DWORD nextScene = *pEdx;

    if (nextScene == 1) {
        g_StageSelected = 0;
        if (!g_AutoAdvanceToDifficulty) {
            g_PracticeMode = 0;
            g_PracticeStage = 0;
            RestoreGlobalData();
        }
        g_PendingStageInitStats = 0;
        g_StageInitRenderFrames = 0;
        InterlockedExchange(&g_PendingReset, 0);
    }
}

static char *GetReplayGameState(void) {
    char *pInputMgr = *(char**)g_A->InputMgr;
    if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 0x20)) {
        if (*(DWORD*)(pInputMgr + 4) == 4) {
            char *pReplayGS = *(char**)(pInputMgr + 8);
            if (pReplayGS && (DWORD)pReplayGS > 0x10000 && !IsBadReadPtr(pReplayGS, 0x200)) {
                return pReplayGS;
            }
        }
    }
    char *pGS = *(char**)g_A->Config;
    if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x200)) {
        return pGS;
    }
    return NULL;
}

static int ReplayHasStage(int stage) {
    if (stage < 1 || stage > 5) return 0;

    char *pReplayGS = GetReplayGameState();
    if (!pReplayGS) {
        LogMessage("ReplayHasStage(%d): GameState not found", stage);
        return 0;
    }

    BYTE visited = *(BYTE*)(pReplayGS + stage + 0x52);
    DWORD startFrame = *(DWORD*)(pReplayGS + stage * 4 + 0x15d);

    LogMessage("ReplayHasStage(%d): visited=%u, startFrame=%lu", stage, visited, startFrame);

    if (stage == 1) {
        return 1;
    }
    return (visited != 0 && startFrame > 0);
}

DWORD __cdecl HandleTrans(void) {
    DWORD prevScene = VD(Scene);
    DWORD nextScene = VD(SceneNext);

    if (g_PauseRetryAll && prevScene != nextScene &&
        (prevScene == 2 || (prevScene >= 3 && prevScene <= 10))) {
        g_PauseRetryAll = 0;
        if (g_PracticeMode && g_PracticeStage >= 1 && g_PracticeStage <= 5) {
            DWORD retryScene = 2 + (DWORD)g_PracticeStage;
            g_StageSelected = 1;
            g_ShowStageMenu = 0;
            g_AutoAdvanceToDifficulty = 0;
            g_AutoNavPhase = 0;
            g_AutoNavTimer = 0;
            g_DifficultyConfirmed = 0;
            g_SimulatedKeyAction = SIMKEY_NONE;
            g_PendingStageInitStats = 1;
            g_StageInitRenderFrames = 0;
            InterlockedExchange(&g_PendingReset, 1);
            ResetStageScore();
            VD(SceneNext) = retryScene;
            LogMessage("Pause hotkey R: restarting current Practice stage %d (scene=%lu), skipping Practice menu",
                       g_PracticeStage, retryScene);
            return prevScene;
        }

        g_PracticeMode = 0;
        g_PracticeStage = 0;
        g_StageSelected = 1;
        g_ShowStageMenu = 0;
        g_AutoAdvanceToDifficulty = 0;
        g_AutoNavPhase = 0;
        g_AutoNavTimer = 0;
        g_DifficultyConfirmed = 0;
        g_SimulatedKeyAction = SIMKEY_NONE;
        RestoreGlobalData();
        LogMessage("Pause hotkey R: allowing native Retry All transition without Practice menu (prev=%lu next=%lu)",
                   prevScene, nextScene);
        return prevScene;
    }

    if (g_CancelToTitle) {
        g_CancelToTitle = 0;
        g_PracticeMode = 0;
        LogMessage("Trans: cancel to title allowed (prev=%lu next=%lu)", prevScene, nextScene);
        return nextScene - 1;
    }

    if (IsReplayOrDemo()) {
        LogMessage("Trans: Replay/Demo playback detected (replaySlot=%ld, mode=%lu) -> passing through without menu",
                   (long)VD(ReplaySlot), VD(GameMode));
        return prevScene;
    }

    if ((prevScene == 2 || (prevScene >= 3 && prevScene <= 10)) &&
        prevScene != nextScene &&
        g_PracticeMode && g_PendingReset == 0) {

        RestoreGlobalData();

        DWORD curChar = VD(Char);
        if (curChar <= 2) g_PracticeChar = curChar;
        DWORD diff = VD(Diff);
        DWORD isUnlocked = (!IsBadReadPtr((void*)g_A->Unlock, 4) && VD(Unlock)) ? 1 : 0;
        int effectiveRe = (g_SelectedReMode && diff >= 2 && isUnlocked) ? 1 : 0;
        SetGameInfoInt("lastRefRain", effectiveRe);

        LogMessage("Practice stage end (prev=%lu next=%lu) -> redirecting to Title with fast AutoNav (char=%lu, ReMode=%d)",
                   prevScene, nextScene, g_PracticeChar, effectiveRe);
        g_AutoAdvanceToDifficulty = 1;
        g_AutoNavTarget = 0;
        g_AutoNavPhase = 0;
        g_AutoNavTimer = 0;
        g_DifficultyConfirmed = 0;
        g_SimulatedKeyAction = SIMKEY_NONE;
        g_StageSelected = 0;
        VD(SceneNext) = 1;
        return prevScene;
    }

    /* A stray DECIDE during the pause-Q navigation must never start a DIVE. */
    if (g_AutoAdvanceToDifficulty && g_AutoNavTarget == 1 && nextScene >= 3 && nextScene <= 8) {
        LogMessage("Trans: blocking stage load while auto-navigating to the desktop (prev=%lu next=%lu)",
                   prevScene, nextScene);
        VD(SceneNext) = 1;
        return prevScene;
    }

    if (nextScene >= 3 && nextScene <= 7 && !g_StageSelected) {
        if (g_ShowStageMenu) {
            if (prevScene == 1 && !g_AutoAdvanceToDifficulty) {
                LogMessage("Trans: suppressing game-initiated stage load while Stage Menu open (prev=%lu next=%lu)", prevScene, nextScene);
                VD(SceneNext) = 1;
            }
            return prevScene;
        }

        int isC = (IsConceptReactorHeld() && g_RefrainSubMenuCursor != 2);
        if (isC) {
            g_ShowStageMenu = 1;
            BurnMenuOpenEdges();
            g_ActiveColumn = 0;
            g_PracticeMode = 1;
            LogMessage("Trans intercepted with Concept Reactor: prev=%lu next=%lu -> showing stage select menu (practice mode, ReMode=%d)",
                       prevScene, nextScene, g_SelectedReMode);
            return nextScene;
        } else {
            g_StageSelected = 1;
            g_PracticeMode = 0;
            LogMessage("Game started normally (without Concept Reactor): prev=%lu next=%lu -> starting stage directly (hotkeys disabled)", prevScene, nextScene);
            return prevScene;
        }
    }

    if (nextScene >= 3 && nextScene <= 7 && g_StageSelected && InterlockedExchange(&g_PendingReset, 0)) {
        ResetStageScore();
    }

    return prevScene;
}

void __attribute__((naked)) Hook_Trans(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "call _HandleTrans\n\t"
        "movl %%eax, 28(%%esp)\n\t"
        "popal\n\t"
        "ret\n\t"
        :
        :
    );
}

void __attribute__((naked)) Hook_SceneCheck(void) {
    __asm__ __volatile__(
        "movl _g_A_SceneNext, %%edx\n\t"
        "movl (%%edx), %%edx\n\t"
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

int __cdecl HandleLoadStart(void) {
    if (IsReplayOrDemo()) {
        return 0;
    }

    if (g_AutoAdvanceToDifficulty) {
        return 0;
    }

    if (!g_StageSelected) {
        if (g_ShowStageMenu) {
            return 1;
        }
        int isC = (IsConceptReactorHeld() && g_RefrainSubMenuCursor != 2);
        if (isC) {
            g_ShowStageMenu = 1;
            BurnMenuOpenEdges();
            g_ActiveColumn = 0;
            g_PracticeMode = 1;
            LogMessage("LoadStart intercepted with Concept Reactor -> showing stage select menu (practice mode)");
            return 1;
        } else {
            g_StageSelected = 1;
            g_PracticeMode = 0;
            LogMessage("LoadStart passed through normally (without Concept Reactor, hotkeys disabled)");
            return 0;
        }
    }
    if (InterlockedExchange(&g_PendingReset, 0)) {
        ResetStageScore();
    }
    return 0;
}

void __attribute__((naked)) Hook_LoadStart(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "call _HandleLoadStart\n\t"
        "testl %%eax, %%eax\n\t"
        "popal\n\t"
        "jnz 1f\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "pushl %%eax\n\t"
        "movl _g_A_LoadFlag, %%eax\n\t"
        "movl $1, (%%eax)\n\t"
        "popl %%eax\n\t"
        "jmp *%0\n\t"
        "1:\n\t"
        "jmp *%1\n\t"
        :
        : "m"(g_LoadStartRetAddr), "m"(g_LoadStartSkipAddr)
    );
}

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
static PFN_Direct3DCreate9 orig_Direct3DCreate9 = NULL;

#if defined(_MSC_VER)
IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
#else
__declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
#endif
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

    VD(FrameCnt) = 0;
    VD(Frame2) = 0;
    VD(Frame1) = 0x3c;

    VD(St20) = 0;
    VD(St24) = 0;
    VD(Lives) = (DWORD)g_SelectedLives;
    VD(St1c) = 0;
    VD(St14) = 0;
    VD(St10) = 0;
    VD(St18) = 0;
    VD(Mefa) = g_SelectedMefaDword;
    VD(Cr) = g_SelectedCrDword;
    VB(Var5b975d) = 0;

    DWORD diff = VD(Diff);
    if (diff < 1 || diff > 4) diff = 2;
    DWORD isRefRain = (g_SelectedReMode && diff >= 2) ? 1 : 0;
    DWORD curChar = VD(Char);
    if (curChar < 1 || curChar > 3) curChar = 1;
    VD(ViewIdx) = (curChar - 1) + 3 * ((diff - 1) + 4 * isRefRain);
#ifdef REFRAIN_DEBUG_BUILD
    LogMessage("ResetStageScore wrote ViewIdx=%lu (Char=%lu Diff=%lu isRefRain=%lu)",
               VD(ViewIdx), curChar, diff, isRefRain);
#endif
    VD(GameMode) = 1;
    VD(IsRefRain) = isRefRain;
    SetGameInfoInt("GameInfo_RefRain", isRefRain);
    SetGameInfoInt("lastRefRain", isRefRain);

    char *pGameState = *(char**)g_A->Config;
    if (pGameState && (DWORD)pGameState > 0x10000) {
        *(BYTE*)(pGameState + 0x52) = (BYTE)isRefRain;
        for (int s = 1; s <= 6; s++) {
            *(BYTE*)(pGameState + s + 0x52) = 0;
            *(DWORD*)(pGameState + s * 8 + 0x69) = 0;
            *(DWORD*)(pGameState + s * 8 + 0x6d) = 0;
            *(DWORD*)(pGameState + s * 4 + 0x9d) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xcd) = 0;
            *(DWORD*)(pGameState + s * 4 + 0x115) = (DWORD)g_SelectedLives;
            *(DWORD*)(pGameState + s * 4 + 0x12d) = g_SelectedMefaDword;
            *(DWORD*)(pGameState + s * 4 + 0x145) = g_SelectedCrDword;
            *(DWORD*)(pGameState + s * 4 + 0x15d) = 0;
            *(DWORD*)(pGameState + s * 4 + 0x17c) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xb5) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xe5) = 0;
            *(DWORD*)(pGameState + s * 4 + 0xfd) = 0;
        }
        *(DWORD*)(pGameState + 0x7a8) = 0;
    }

    LogMessage("ResetStageScore: Score and player stats reset (Lives=%d, MEFA=%f (dw=0x%08X), CR=%f (dw=0x%08X), GameState=0x%p)",
               g_SelectedLives, uMefa.f, uMefa.dw, uCr.f, uCr.dw, pGameState);
}

/* ---- PauseDiag: ポーズメニューの調査用ログ (ゲーム動作は変更しない) ----
 * stage_select.log の "PauseDiag:" 行を共有する。
 *  - シーン変数 0x5c073c(現在) / 0x5c0740(次) / 0x5c0744 / 0x4d237c(リプレイ枠) / 0x5b8e68 の変化
 *  - フレームカウンタ 0x5abb78 が止まった(=ポーズ)/再開した瞬間の、ゲーム変数領域の差分
 *  - IsKeyTriggered が true を返したキー
 */
static const int g_PauseDiag = 1;
#define PD_NRANGE 6
#define PD_MAXDW 128
static volatile int g_pdFrozen = 0;

static void PauseDiagKey(int key) {
    if (!g_PauseDiag) return;
    LogMessage("PauseDiag: IsKeyTriggered key=0x%02X scene=%lu next=%lu frame=%lu frozen=%d", key,
               VD(Scene), VD(SceneNext),
               VD(FrameCnt), g_pdFrozen);
}

static int PauseDiagSnap(DWORD *dst, DWORD *addrs) {
    int n = 0;
    for (int r = 0; r < PD_NRANGE; r++) {
        for (DWORD a = g_A->pdRange[r][0]; a < g_A->pdRange[r][1] && n < PD_MAXDW; a += 4) {
            addrs[n] = a;
            dst[n] = IsBadReadPtr((void*)a, 4) ? 0 : *(volatile DWORD*)a;
            n++;
        }
    }
    return n;
}

static void PauseDiagDiff(const char *tag, const DWORD *a, const DWORD *b, const DWORD *addrs, int n) {
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] && shown < 40) {
            LogMessage("PauseDiag: %s [0x%08X] %08X -> %08X", tag, addrs[i], a[i], b[i]);
            shown++;
        }
    }
    if (!shown) LogMessage("PauseDiag: %s no differences", tag);
}

static DWORD WINAPI PauseDiagThread(LPVOID param) {
    DWORD prevScene = 0xffffffff, prevNext = 0xffffffff, prevPend = 0xffffffff;
    DWORD prevSlot = 0xffffffff, prevObj = 0xffffffff, lastFrame = 0xffffffff;
    DWORD lastMove = GetTickCount();
    DWORD before[PD_MAXDW], during[PD_MAXDW], after[PD_MAXDW], addrs[PD_MAXDW];
    int n = 0, haveBefore = 0;
    (void)param;
    LogMessage("PauseDiag: active (scene vars / frame freeze diff / triggered keys)");
    for (;;) {
        DWORD sc, nx, pd, slot, obj, fr;
        Sleep(1);
        sc = VD(Scene);
        nx = VD(SceneNext);
        pd = VD(ScenePend);
        slot = VD(ReplaySlot);
        obj = VD(Obj);
        if (sc != prevScene || nx != prevNext || pd != prevPend || slot != prevSlot || obj != prevObj) {
            LogMessage("PauseDiag: scene cur=%lu next=%lu pend=%lu replaySlot=%ld sceneObj=%08lX frame=%lu mode=%lu",
                       sc, nx, pd, (long)slot, obj, VD(FrameCnt), VD(GameMode));
            prevScene = sc; prevNext = nx; prevPend = pd; prevSlot = slot; prevObj = obj;
        }
        if (!((sc >= 2 && sc <= 7) || sc == 10)) {
            if (g_pdFrozen) g_pdFrozen = 0;
            haveBefore = 0;
            lastFrame = 0xffffffff;
            continue;
        }
        fr = VD(FrameCnt);
        if (fr != lastFrame) {
            lastFrame = fr;
            lastMove = GetTickCount();
            if (g_pdFrozen) {
                g_pdFrozen = 0;
                n = PauseDiagSnap(after, addrs);
                LogMessage("PauseDiag: frame counter running again (frame=%lu) -> pause closed", fr);
                PauseDiagDiff("resume", during, after, addrs, n);
            } else {
                n = PauseDiagSnap(before, addrs);
                haveBefore = 1;
            }
        } else if (!g_pdFrozen && haveBefore && GetTickCount() - lastMove > 200) {
            g_pdFrozen = 1;
            n = PauseDiagSnap(during, addrs);
            LogMessage("PauseDiag: frame counter frozen at %lu -> pause open? (scene=%lu)", fr, sc);
            PauseDiagDiff("freeze", before, during, addrs, n);
        }
    }
    return 0;
}

static DWORD WINAPI HotkeyThread(LPVOID param) {
    (void)param;
    int key_state[5] = {0};

    while (1) {
        Sleep(50);

        if (!g_PracticeMode && !IsReplayOrDemo()) {
            memset(key_state, 0, sizeof(key_state));
            continue;
        }

        DWORD currentScene = VD(Scene);

        for (int i = 0; i < 5; i++) {
            int is_down = (GetAsyncKeyState(VK_F1 + i) & 0x8000) != 0;
            if (is_down && !key_state[i]) {
                int stage = i + 1;
                if (!g_ShowStageMenu && ((currentScene >= 2 && currentScene <= 7) || currentScene == 10)) {
                    if (IsReplayOrDemo()) {
                        if (!ReplayHasStage(stage)) {
                            LogMessage("Hotkey F%d -> Replay does NOT contain Stage %d; skipping warp", stage, stage);
                        } else {
                            LogMessage("Hotkey F%d -> Replay seeking to Stage %d", stage, stage);
                            g_StageSelected = 1;
                            VD(SceneNext) = 2 + stage;
                            PlayGameSE(1040);
                        }
                    } else if (g_PracticeMode) {
                        DWORD diff = VD(Diff);
                        if (stage == 5 && diff == 1) {
                            LogMessage("Hotkey F%d -> Stage %d blocked on Easy difficulty", stage, stage);
                        } else if (!IsStageReached(stage)) {
                            LogMessage("Hotkey F%d -> Stage %d not yet reached in normal play, skipped", stage, stage);
                        } else {
                        LogMessage("Hotkey F%d -> Warping to Stage %d (currentScene=%lu)", stage, stage, currentScene);
                        g_PracticeStage = stage;
                        g_MenuCursor = stage - 1;
                        g_StageSelected = 1;
                        g_PendingStageInitStats = 1;
                        g_StageInitRenderFrames = 0;
                        InterlockedExchange(&g_PendingReset, 1);
                        ResetStageScore();
                        BackupGlobalData();
                        VD(SceneNext) = 2 + stage;
                        PlayGameSE(1040);
                        }
                    }
                }
            }
            key_state[i] = is_down;
        }
    }
    return 0;
}

#ifdef REFRAIN_DEBUG_BUILD
/* ---- Practice-start diagnostics (debug build only) --------------------------
 * The stage-init code uses the "view index" global (disc 0x5abb80 / steam
 * 0x5afc50) to index an 8-byte-per-entry table.  It is written by exactly one
 * game instruction (mov [viewIdx], eax) inside the view setter, from
 *   (Char - 1) + 3 * ((Diff - 1) + 4 * <Re:MODE config value>)
 * where the config value is the eax our Hook_GetRefRainConfig leaves behind.
 * The hook below logs those three inputs every time the game writes the index,
 * which pinpoints which input went out of range.
 */
static volatile DWORD g_WvIdx, g_WvEbx, g_WvEsi, g_WvRet;
static volatile DWORD g_ViewIdxWriteRet = 0;
static volatile DWORD g_LastLoggedIdx = 0xFFFFFFFF;
static volatile DWORD g_CfgHookValue = 0xFFFFFFFF;
static volatile DWORD g_CfgHookCount = 0;

__attribute__((used)) static void __cdecl HandleViewIdxWatch(void) {
    DWORD idx = g_WvIdx;
    if (idx != g_LastLoggedIdx) {
        g_LastLoggedIdx = idx;
        LogMessage("ViewIdxWatch: write %lu (idx=%lu) <- Char=%lu Diff=%lu cfgRet=%lu | caller=%08X esi=%08X ebx=%08X | Scene=%lu next=%lu practice=%d reMode=%d pending=%d",
                   idx, idx, g_WvEsi, g_WvEbx, g_CfgHookValue, g_WvRet, g_WvEsi, g_WvEbx,
                   VD(Scene), VD(SceneNext), g_PracticeMode, g_SelectedReMode, g_PendingStageInitStats);
        LogMessage("ViewIdxWatch: game vars Char=%lu Diff=%lu Lives=%lu Mefa=%08X Cr=%08X IsRefRain=%lu Unlock=%lu GameMode=%lu",
                   VD(Char), VD(Diff), VD(Lives), VD(Mefa), VD(Cr), VD(IsRefRain), VD(Unlock), VD(GameMode));
    }
}

void __attribute__((naked)) Hook_ViewIdxWatch(void) {
    __asm__ __volatile__(
        "pushal\n\t"
        "movl 28(%%esp), %%eax\n\t"
        "movl %%eax, _g_WvIdx\n\t"
        "movl 16(%%esp), %%eax\n\t"
        "movl %%eax, _g_WvEbx\n\t"
        "movl 4(%%esp), %%eax\n\t"
        "movl %%eax, _g_WvEsi\n\t"
        "movl 32(%%esp), %%eax\n\t"
        "movl %%eax, _g_WvRet\n\t"
        "call _HandleViewIdxWatch\n\t"
        "popal\n\t"
        "pushl %%edx\n\t"
        "movl _g_A_ViewIdx, %%edx\n\t"
        "movl %%eax, (%%edx)\n\t"
        "popl %%edx\n\t"
        "pushl _g_ViewIdxWriteRet\n\t"
        "ret\n\t"
        :
        :
    );
}

static void InstallViewIdxWatch(void) {
    DWORD oldProtect;
    void *site = (void*)g_A->ViewIdxWriteSite;
    unsigned char expected[5];
    expected[0] = 0xA3;
    DWORD vi = g_A->ViewIdx;
    memcpy(&expected[1], &vi, 4);
    if (memcmp(site, expected, 5) != 0) {
        LogMessage("[Diag] ViewIdx write site at 0x%08X did not match (found %02X %02X %02X %02X %02X)",
                   site, *(unsigned char*)site, *(unsigned char*)((DWORD)site + 1),
                   *(unsigned char*)((DWORD)site + 2), *(unsigned char*)((DWORD)site + 3),
                   *(unsigned char*)((DWORD)site + 4));
        return;
    }
    g_ViewIdxWriteRet = g_A->ViewIdxWriteSite + 5;
    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        unsigned char patch[5];
        patch[0] = 0xE9;
        DWORD rel = (DWORD)Hook_ViewIdxWatch - ((DWORD)site + 5);
        memcpy(&patch[1], &rel, 4);
        memcpy(site, patch, 5);
        VirtualProtect(site, 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        LogMessage("[Diag] ViewIdx write watch installed at 0x%08X", site);
    }
}
#else
static void InstallViewIdxWatch(void) {
}
#endif

static void InstallHooks(void) {
    DWORD oldProtect;

    void *transAddr = (void*)g_A->TransSite;
    if (memcmp(transAddr, g_Sigs.Trans, 5) == 0) {
        if (VirtualProtect(transAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE8;
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

    void *loadStartAddr = (void*)g_A->LoadStartSite;
    if (memcmp(loadStartAddr, g_Sigs.LoadStart, 7) == 0) {
        if (VirtualProtect(loadStartAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[7];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_LoadStart - ((DWORD)loadStartAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90;
            patch[6] = 0x90;
            memcpy(loadStartAddr, patch, 7);
            VirtualProtect(loadStartAddr, 7, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), loadStartAddr, 7);
            LogMessage("LoadStart hook installed successfully at 0x%08X", loadStartAddr);
        }
    } else {
        LogMessage("[Error] LoadStart signature at 0x%08X did not match!", loadStartAddr);
    }

    void *sceneAddr = (void*)g_A->SceneCheckSite;
    if (memcmp(sceneAddr, g_Sigs.SceneCheck, 6) == 0) {
        if (VirtualProtect(sceneAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[6];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SceneCheck - ((DWORD)sceneAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90;
            memcpy(sceneAddr, patch, 6);
            VirtualProtect(sceneAddr, 6, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), sceneAddr, 6);
            LogMessage("SceneCheck hook installed successfully at 0x%08X", sceneAddr);
        }
    } else {
        LogMessage("[Error] Memory signature at 0x%08X did not match!", sceneAddr);
    }

    void *endSceneAddr = (void*)g_A->EndSceneSite;
    if (memcmp(endSceneAddr, g_Sigs.EndScene, 6) == 0) {
        if (VirtualProtect(endSceneAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[6];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_EndScene - ((DWORD)endSceneAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90;
            memcpy(endSceneAddr, patch, 6);
            VirtualProtect(endSceneAddr, 6, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), endSceneAddr, 6);
            LogMessage("EndScene hook installed successfully at 0x%08X", endSceneAddr);
        }
    } else {
        LogMessage("[Error] EndScene signature at 0x%08X did not match!", endSceneAddr);
    }

    void *preTickAddr = (void*)g_A->PreTickSite;
    {
        unsigned char expectedPreTick[5] = { 0xE8, 0, 0, 0, 0 };
        DWORD relTick = g_A->fnTick - (g_A->PreTickSite + 5);
        memcpy(&expectedPreTick[1], &relTick, 4);
        if (memcmp(preTickAddr, expectedPreTick, 5) == 0) {
            if (VirtualProtect(preTickAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                unsigned char patchP[5];
                patchP[0] = 0xE9;
                DWORD relOffsetP = (DWORD)Hook_PreTickPollCall - ((DWORD)preTickAddr + 5);
                memcpy(&patchP[1], &relOffsetP, 4);
                memcpy(preTickAddr, patchP, 5);
                VirtualProtect(preTickAddr, 5, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), preTickAddr, 5);
                LogMessage("PreTick input poll hook installed successfully at 0x%08X", preTickAddr);
            }
        } else {
            LogMessage("[Error] PreTick signature at 0x%08X did not match! (found %02X %02X %02X %02X %02X)",
                       preTickAddr,
                       *(unsigned char*)preTickAddr, *(unsigned char*)((DWORD)preTickAddr + 1),
                       *(unsigned char*)((DWORD)preTickAddr + 2), *(unsigned char*)((DWORD)preTickAddr + 3),
                       *(unsigned char*)((DWORD)preTickAddr + 4));
        }
    }

    LARGE_INTEGER liFreq;
    if (QueryPerformanceFrequency(&liFreq)) {
        g_LatQpcFreq = liFreq.QuadPart;
    }
    PatchIATPtr(g_A->iatGetKeyboardState, (void*)Mine_GetKeyboardState, (void**)&orig_GetKeyboardState);
    PatchIATPtr(g_A->iatXInputGetCaps, (void*)Mine_XInputGetCaps, (void**)&orig_XInputGetCaps);
    PatchIATPtr(g_A->iatXInputGetState, (void*)Mine_XInputGetState, (void**)&orig_XInputGetState);
    if (!orig_XInputGetState || !orig_XInputGetCaps || !orig_GetKeyboardState) {
        LogMessage("[Error] IAT hook partial: kb=%08X getState=%08X getCaps=%08X",
                   (DWORD)(DWORD_PTR)orig_GetKeyboardState, (DWORD)(DWORD_PTR)orig_XInputGetState,
                   (DWORD)(DWORD_PTR)orig_XInputGetCaps);
    }
    LogMessage("Latency instrumentation installed (kbHook=%08X padHook=%08X freq=%08X%08X)",
               (DWORD)(DWORD_PTR)orig_GetKeyboardState, (DWORD)(DWORD_PTR)orig_XInputGetState,
               (DWORD)(g_LatQpcFreq >> 32), (DWORD)g_LatQpcFreq);

    void *setMefaAddr = (void*)g_A->SetMefaSite;
    if (memcmp(setMefaAddr, g_Sigs.SetMefa, 13) == 0) {
        if (VirtualProtect(setMefaAddr, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[13];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetMefa - ((DWORD)setMefaAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memset(&patch[5], 0x90, 8);
            memcpy(setMefaAddr, patch, 13);
            VirtualProtect(setMefaAddr, 13, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setMefaAddr, 13);
            LogMessage("SetMefa hook installed successfully at 0x%08X", setMefaAddr);
        }
    } else {
        LogMessage("[Error] SetMefa signature at 0x%08X did not match!", setMefaAddr);
    }

    void *setCrAddr = (void*)g_A->SetCrSite;
    if (memcmp(setCrAddr, g_Sigs.SetCr, 13) == 0) {
        if (VirtualProtect(setCrAddr, 13, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[13];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetCR - ((DWORD)setCrAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memset(&patch[5], 0x90, 8);
            memcpy(setCrAddr, patch, 13);
            VirtualProtect(setCrAddr, 13, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setCrAddr, 13);
            LogMessage("SetCR hook installed successfully at 0x%08X", setCrAddr);
        }
    } else {
        LogMessage("[Error] SetCR signature at 0x%08X did not match!", setCrAddr);
    }

    void *setLivesAddr = (void*)g_A->SetLivesSite;
    if (memcmp(setLivesAddr, g_Sigs.SetLives, 8) == 0) {
        if (VirtualProtect(setLivesAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[8];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetLives - ((DWORD)setLivesAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90;
            patch[6] = 0x90;
            patch[7] = 0x90;
            memcpy(setLivesAddr, patch, 8);
            VirtualProtect(setLivesAddr, 8, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setLivesAddr, 8);
            LogMessage("SetLives hook installed successfully at 0x%08X", setLivesAddr);
        }
    } else {
        LogMessage("[Error] SetLives signature at 0x%08X did not match!", setLivesAddr);
    }

    void *setDiffAddr = (void*)g_A->SetDiffSite;
    if (memcmp(setDiffAddr, g_Sigs.SetDiff, 8) == 0) {
        if (VirtualProtect(setDiffAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[8];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetDifficulty - ((DWORD)setDiffAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            patch[5] = 0x90;
            patch[6] = 0x90;
            patch[7] = 0x90;
            memcpy(setDiffAddr, patch, 8);
            VirtualProtect(setDiffAddr, 8, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setDiffAddr, 8);
            LogMessage("SetDifficulty (Native 79) hook installed successfully at 0x%08X", setDiffAddr);
        }
    } else {
        LogMessage("[Error] SetDifficulty signature at 0x%08X did not match!", setDiffAddr);
    }

    void *refRainCfgAddr = (void*)g_A->RefRainCfgSite;
    if (memcmp(refRainCfgAddr, g_Sigs.RefRainCfg, 9) == 0) {
        if (VirtualProtect(refRainCfgAddr, 9, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[9];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_GetRefRainConfig - ((DWORD)refRainCfgAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memset(&patch[5], 0x90, 4);
            memcpy(refRainCfgAddr, patch, 9);
            VirtualProtect(refRainCfgAddr, 9, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), refRainCfgAddr, 9);
            LogMessage("GetRefRainConfig hook installed successfully at 0x%08X", refRainCfgAddr);
        }
    } else {
        LogMessage("[Error] GetRefRainConfig signature at 0x%08X did not match!", refRainCfgAddr);
    }

    void *refRainGameInfoAddr = (void*)g_A->GameInfoRefRainSite;
    {
        unsigned char expectedRefRainGameInfo[5] = { 0xE8, 0, 0, 0, 0 };
        DWORD relGII = g_A->fnGetGameInfoInt - (g_A->GameInfoRefRainSite + 5);
        memcpy(&expectedRefRainGameInfo[1], &relGII, 4);
        if (memcmp(refRainGameInfoAddr, expectedRefRainGameInfo, 5) == 0) {
            if (VirtualProtect(refRainGameInfoAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                unsigned char patch[5];
                patch[0] = 0xE8;
                DWORD relOffset = (DWORD)Hook_GetGameInfoRefRain - ((DWORD)refRainGameInfoAddr + 5);
                memcpy(&patch[1], &relOffset, 4);
                memcpy(refRainGameInfoAddr, patch, 5);
                VirtualProtect(refRainGameInfoAddr, 5, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), refRainGameInfoAddr, 5);
                LogMessage("GetGameInfoRefRain hook installed successfully at 0x%08X", refRainGameInfoAddr);
            }
        } else {
            LogMessage("[Error] GetGameInfoRefRain signature at 0x%08X did not match!", refRainGameInfoAddr);
        }
    }

    void *gameInfoIntAddr = (void*)g_A->fnGetGameInfoInt;
    unsigned char expectedPrologue[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    if (memcmp(gameInfoIntAddr, expectedPrologue, 5) == 0) {
        if (VirtualProtect(gameInfoIntAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_GetGameInfoInt - ((DWORD)gameInfoIntAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(gameInfoIntAddr, patch, 5);
            VirtualProtect(gameInfoIntAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), gameInfoIntAddr, 5);
            LogMessage("GetGameInfoInt (0x%08X) global hook installed successfully", gameInfoIntAddr);
        }
    } else {
        LogMessage("[Error] GetGameInfoInt signature at 0x%08X did not match!", gameInfoIntAddr);
    }

    void *getterTableEntry19 = (void*)g_A->Getter19Entry;
    DWORD origGetter19 = g_A->Getter19Fn;
    if (memcmp(getterTableEntry19, &origGetter19, 4) == 0) {
        if (VirtualProtect(getterTableEntry19, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(DWORD*)getterTableEntry19 = (DWORD)Hook_GetterReMode;
            VirtualProtect(getterTableEntry19, 4, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), getterTableEntry19, 4);
            LogMessage("Getter 19 (Re:MODE) table entry hooked successfully at 0x%08X", getterTableEntry19);
        }
    } else {
        LogMessage("[Error] Getter table entry 19 at 0x%08X did not match! (found 0x%08X)",
                   getterTableEntry19, *(DWORD*)getterTableEntry19);
    }

    void *inputVtableAddr = (void*)g_A->vtInput;
    DWORD origIsKey[2] = { g_A->fnIsKeyPressed, g_A->fnIsKeyTriggered };
    if (memcmp(inputVtableAddr, origIsKey, 8) == 0) {
        if (VirtualProtect(inputVtableAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(DWORD*)inputVtableAddr = (DWORD)Hook_IsKeyPressed;
            *((DWORD*)inputVtableAddr + 1) = (DWORD)Hook_IsKeyTriggered;
            VirtualProtect(inputVtableAddr, 8, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), inputVtableAddr, 8);
            LogMessage("InputMgr vtable hooked successfully at 0x%08X", inputVtableAddr);
        }
    } else {
        LogMessage("[Error] InputMgr vtable signature at 0x%08X did not match! (found 0x%08X, 0x%08X)",
                   inputVtableAddr, *(DWORD*)inputVtableAddr, *((DWORD*)inputVtableAddr + 1));
    }

    void *saveGlobalAddr = (void*)g_A->fnSaveGlobal;
    if (memcmp(saveGlobalAddr, expectedPrologue, 5) == 0) {
        if (VirtualProtect(saveGlobalAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SaveGlobalData - ((DWORD)saveGlobalAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(saveGlobalAddr, patch, 5);
            VirtualProtect(saveGlobalAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), saveGlobalAddr, 5);
            LogMessage("SaveGlobalData (0x%08X) hook installed successfully", saveGlobalAddr);
        }
    } else {
        LogMessage("[Error] SaveGlobalData signature at 0x%08X did not match!", saveGlobalAddr);
    }

    void *setGameInfoAddr = (void*)g_A->fnSetGameInfoInt;
    if (memcmp(setGameInfoAddr, expectedPrologue, 5) == 0) {
        if (VirtualProtect(setGameInfoAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetGameInfoInt - ((DWORD)setGameInfoAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(setGameInfoAddr, patch, 5);
            VirtualProtect(setGameInfoAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setGameInfoAddr, 5);
            LogMessage("SetGameInfoInt (0x%08X) hook installed successfully", setGameInfoAddr);
        }
    } else {
        LogMessage("[Error] SetGameInfoInt signature at 0x%08X did not match!", setGameInfoAddr);
    }

    InstallWrapperDetour(g_A->FwriteWrapSite, (DWORD)Hook_FwriteWrapper, (DWORD)&g_FwriteTramp);
    InstallWrapperDetour(g_A->FreadWrapSite, (DWORD)Hook_FreadWrapper, (DWORD)&g_FreadTramp);

    void *saveReplayHdrAddr = (void*)g_A->SaveReplayHdrSite;
    if (memcmp(saveReplayHdrAddr, g_Sigs.SaveReplayHdr, 5) == 0) {
        if (VirtualProtect(saveReplayHdrAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SaveReplayHeader - ((DWORD)saveReplayHdrAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(saveReplayHdrAddr, patch, 5);
            VirtualProtect(saveReplayHdrAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), saveReplayHdrAddr, 5);
            LogMessage("SaveReplayHeader hook installed successfully at 0x%08X", saveReplayHdrAddr);
        }
    } else {
        LogMessage("[Error] SaveReplayHeader signature at 0x%08X did not match!", saveReplayHdrAddr);
    }

    InstallViewIdxWatch();
}

static void DetectVersion(void) {
    BYTE *base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) base = (BYTE*)0x400000;
    g_BaseDelta = (DWORD)(DWORD_PTR)base - 0x400000;

    int isSteam = memcmp(base + (ADDRS_STEAM.TransSite - 0x400000), SIGS_STEAM.Trans, 5) == 0 &&
                  memcmp(base + (ADDRS_STEAM.LoadStartSite - 0x400000), SIGS_STEAM.LoadStart, 7) == 0;
    int isDisc = memcmp(base + (ADDRS_DISC.TransSite - 0x400000), SIGS_DISC.Trans, 5) == 0 &&
                 memcmp(base + (ADDRS_DISC.LoadStartSite - 0x400000), SIGS_DISC.LoadStart, 7) == 0;
    if (isSteam) {
        g_IsSteam = 1;
    } else if (isDisc) {
        g_IsSteam = 0;
    } else {
        g_IsSteam = 0;
        LogMessage("[Error] Unknown RefRain.exe build (neither disc nor Steam signature matched); assuming disc version");
    }

    const GameAddrs *tmpl = g_IsSteam ? &ADDRS_STEAM : &ADDRS_DISC;
    memcpy(&g_Addrs, tmpl, sizeof(g_Addrs));
    memcpy(&g_Sigs, g_IsSteam ? &SIGS_STEAM : &SIGS_DISC, sizeof(g_Sigs));

    /* rebase every DWORD address field (0 means "not used by this version") */
    DWORD *p = (DWORD*)&g_Addrs;
    for (size_t i = 0; i < sizeof(g_Addrs) / sizeof(DWORD); i++) {
        if (p[i] != 0) p[i] += g_BaseDelta;
    }

    g_A = &g_Addrs;

    /* addresses used by naked-hook inline asm (referenced by symbol, so the
       register setup runs inside the asm block, after pushal-style saves) */
    g_A_SceneNext = g_A->SceneNext;
    g_A_LoadFlag = g_A->LoadFlag;
    g_A_Mefa = g_A->Mefa;
    g_A_Cr = g_A->Cr;
    g_A_Lives = g_A->Lives;
    g_A_Diff = g_A->Diff;
    g_A_ViewIdx = g_A->ViewIdx;
    g_A_GpuQueue = g_A->GpuQueue;
    g_DispatchEpilogue = g_A->DispatchEpilogue;
    g_SaveGlobalRetAddr = g_A->fnSaveGlobalRet;
    g_GetGameInfoIntRetAddr = g_A->fnGetGameInfoIntRet;
    g_SetGameInfoIntRetAddr = g_A->fnSetGameInfoIntRet;
    g_GetGameInfoIntFn = g_A->fnGetGameInfoInt;
    g_RefRainCfgRetAddr = g_A->RefRainCfgRet;

    /* hook continuation addresses */
    g_ReturnAddr = g_A->SceneCheckRet;
    g_EndSceneRetAddr = g_A->EndSceneRet;
    g_LoadStartRetAddr = g_A->LoadStartRet;
    g_LoadStartSkipAddr = g_A->LoadStartSkip;
    g_PreTickRetAddr = g_A->PreTickRet;
    g_PreTickTarget = g_A->fnTick;
    g_SaveReplayHdrRetAddr = g_A->SaveReplayHdrRet;

    /* original functions */
    orig_IsKeyPressed = (PFN_IsKey)g_A->fnIsKeyPressed;
    orig_IsKeyTriggered = (PFN_IsKey)g_A->fnIsKeyTriggered;

    LogMessage("Version detect: %s build, image base 0x%08X (delta=%+ld)",
               g_IsSteam ? "Steam" : "Disk", (DWORD)(DWORD_PTR)base, (long)g_BaseDelta);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hOurDll = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        DetectVersion();
        LogMessage("RefRain stage select mod loaded (%s build, image base 0x%08X).",
                   g_IsSteam ? "Steam" : "Disk", (DWORD)(DWORD_PTR)GetModuleHandleA(NULL));
        InstallHooks();
        CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
        if (g_PauseDiag) CreateThread(NULL, 0, PauseDiagThread, NULL, 0, NULL);
    }
    return TRUE;
}
