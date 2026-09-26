#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <mmsystem.h>
#include <stdio.h>

static volatile int g_ShowStageMenu = 0;
static volatile int g_StageSelected = 0;
static volatile int g_PracticeMode = 0;
static volatile int g_PracticeStage = 0;
static volatile int g_ReturnToStageMenu = 0;
static volatile int g_AutoAdvanceToDifficulty = 0;
static volatile int g_AutoNavPhase = 0;
static volatile int g_AutoNavTimer = 0;
#define SIMKEY_NONE   0
#define SIMKEY_DECIDE 1
#define SIMKEY_CANCEL 2
#define SIMKEY_DOWN   3
#define SIMKEY_UP     4
static volatile int g_SimulatedKeyAction = SIMKEY_NONE;
static volatile DWORD g_PracticeChar = 0;
static volatile int g_CancelToTitle = 0;
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
static DWORD g_LastResetTargetScene = 0;
static volatile long g_PendingReset = 0;

static const DWORD g_ReturnAddr = 0x00440ffe;
static const DWORD g_EndSceneRetAddr = 0x00467273;
static const DWORD g_LoadStartRetAddr  = 0x004410a3;
static const DWORD g_LoadStartSkipAddr = 0x004410ba;

// Ultra Low Latency synchronization
static IDirect3DQuery9 *g_pEventQuery = NULL;
static int g_LowLatencyMode = 1; // 1 = Enabled (VSync OFF + GPU queue flush + Direct polling)

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

static int IsKeyTriggered(int vk) {
    static BYTE prev[256] = {0};
    BYTE curr = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    int triggered = curr && !prev[vk];
    prev[vk] = curr;
    return triggered;
}

typedef struct {
    int stage;
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
        : "r"((DWORD)seIndex), "r"((DWORD)0x43bf40)
        : "eax", "ecx", "edx", "memory"
    );

    if (pSoundObj) {
        PlaySoundDirect(pSoundObj);
        LogMessage("PlayGameSE(%d): played OK", seIndex);
    }
}

static inline int IsReplayOrDemo(void) {
    DWORD gameMode = *(volatile DWORD*)0x4d2378;
    if (gameMode == 2 || gameMode == 3) {
        return 1;
    }
    return 0;
}

typedef unsigned char (__attribute__((thiscall)) *PFN_IsKey)(void *this, int key);
static PFN_IsKey orig_IsKeyPressed = (PFN_IsKey)0x00431760;
static PFN_IsKey orig_IsKeyTriggered = (PFN_IsKey)0x004317a0;

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
    DWORD pConfig = *(volatile DWORD*)0x5ac9b0;
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
    void *pInputMgr = *(void**)0x5ac9b4;
    if (pInputMgr && !IsBadReadPtr(pInputMgr, 16)) {
        void *pJ = *(void**)((DWORD)pInputMgr + 0xc);
        if (pJ && !IsBadReadPtr(pJ, 0x400)) {
            DWORD vtbl = *(DWORD*)pJ;
            if (vtbl == 0x4b83f8 && crKey >= 0 && crKey < 256) {
                int btn = *(int*)((DWORD)pJ + 0x34 + crKey * 4);
                if (btn >= 0 && btn < 32) return btn;
            } else if (vtbl == 0x4b807c && crKey >= 0 && crKey < 256) {
                int btn = *(int*)((DWORD)pJ + 0x1f4 + crKey * 4);
                if (btn >= 0 && btn < 32) return btn;
            }
        }
    }
    return 2;
}

static int IsConceptReactorTriggered(void) {
    int crKey = GetConceptReactorKey();

    void *pInputMgr = *(void**)0x5ac9b4;
    if (pInputMgr && !IsBadReadPtr(pInputMgr, 16)) {
        if (orig_IsKeyTriggered(pInputMgr, crKey)) {
            return 1;
        }
    }

    static int s_prevKey = 0;
    int keyState = (GetAsyncKeyState(crKey) & 0x8000) != 0;
    int keyTrig = keyState && !s_prevKey;
    s_prevKey = keyState;
    if (keyTrig) {
        return 1;
    }

    JOYINFOEX joy;
    joy.dwSize = sizeof(joy);
    joy.dwFlags = JOY_RETURNBUTTONS;
    if (joyGetPosEx(0, &joy) == JOYERR_NOERROR) {
        static DWORD s_prevJoyButtons = 0;
        DWORD trigButtons = joy.dwButtons & ~s_prevJoyButtons;
        s_prevJoyButtons = joy.dwButtons;

        int btnIdx = GetConceptReactorJoyButton(crKey);
        if (btnIdx >= 0 && btnIdx < 32) {
            if (trigButtons & (1 << btnIdx)) {
                return 1;
            }
        }
    }

    return 0;
}

static int IsConceptReactorHeld(void) {
    int crKey = GetConceptReactorKey();

    void *pInputMgr = *(void**)0x5ac9b4;
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

// Track state inside REFRAIN window:
// 0 = Difficulty Selection screen
// 1 = Post-Difficulty screen ("DIVE 2 M.R.S.")
static volatile int g_RefrainWindowState = 0;
static volatile int g_RefrainSubMenuCursor = 0;
static volatile int g_SelectedReMode = 0;
static volatile int g_CancelDebounceActive = 0;
static volatile int g_CancelDebounceTimer = 0;
static volatile int g_DifficultyConfirmed = 0;

static int IsCancelHeld(void) {
    if ((GetAsyncKeyState('X') & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) return 1;
    void *pInputMgr = *(void**)0x5ac9b4;
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
    DWORD *pTasks = (DWORD*)0x5ba0a8;
    for (DWORD i = 0; i < 6000; i++) {
        DWORD pTask = pTasks[i];
        if (pTask >= 0x00400000 && pTask < 0x7FFF0000 && ((uintptr_t)pTask & 3) == 0) {
            if (*(DWORD*)pTask == 0x004B9A20) {
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
    DWORD oldMode = *(volatile DWORD*)0x4d2378;
    *(volatile DWORD*)0x4d2378 = 1;
    __asm__ __volatile__(
        "pushl %0\n\t"
        "movl $0xffffffff, %%edx\n\t"
        "movl %1, %%ecx\n\t"
        "movl $0x0044fca0, %%eax\n\t"
        "call *%%eax\n\t"
        "addl $4, %%esp\n\t"
        :
        : "r"((DWORD)value), "r"(key)
        : "eax", "ecx", "edx", "memory"
    );
    *(volatile DWORD*)0x4d2378 = oldMode;
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
    PFN_LoadGlobalData pfnLoadGlobalData = (PFN_LoadGlobalData)0x0042a8e0;
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
        "movl $0x0042c335, %%eax\n\t"
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

    if (g_PracticeMode) {
        if (strcmp(key, "GameInfo_RefRain") == 0 || strcmp(key, "lastRefRain") == 0) {
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
        "movl $0x0044fca5, %%eax\n\t"
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
        "movl $0x0044fb85, %%eax\n\t"
        "jmp *%%eax\n\t"
        "2:\n\t"
        : "=r"(res)
        : "r"(key), "r"(format)
        : "eax", "ecx", "edx", "memory"
    );
    return res;
}

unsigned char __attribute__((thiscall)) Hook_IsKeyTriggered(void *this, int key) {
    DWORD currentScene = *(volatile DWORD*)0x5c073c;
    DWORD nextScene = *(volatile DWORD*)0x5c0740;

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

    // Track sub-menu cursor on post-difficulty screen (currentScene == 1)
    if (currentScene == 1 && g_RefrainWindowState == 1 && IsRefrainWindowOpen() && !g_ShowStageMenu && !g_AutoAdvanceToDifficulty && !IsReplayOrDemo() && res) {
        if (key == VK_UP || key == 'W' || key == 'w' || key == 0x26 || key == 0xC8) {
            g_RefrainSubMenuCursor = 0;
            g_SelectedReMode = 0;
            SetGameInfoInt("lastRefRain", 0);
            LogMessage("Hook_IsKeyTriggered: UP triggered -> subCursor=0, SelectedReMode=0");
        } else if (key == VK_DOWN || key == 'S' || key == 's' || key == 0x28 || key == 0xD0) {
            DWORD diff = *(volatile DWORD*)0x5b8e28;
            DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
            if (diff >= 2 && isUnlocked) {
                g_RefrainSubMenuCursor = 1;
                g_SelectedReMode = 1;
                SetGameInfoInt("lastRefRain", 1);
                LogMessage("Hook_IsKeyTriggered: DOWN triggered -> subCursor=1, SelectedReMode=1");
            } else {
                g_RefrainSubMenuCursor = 0;
                g_SelectedReMode = 0;
                SetGameInfoInt("lastRefRain", 0);
                LogMessage("Hook_IsKeyTriggered: DOWN triggered (not unlocked or easy) -> subCursor=0, SelectedReMode=0");
            }
        } else if (key == 'X' || key == 'x' || key == VK_ESCAPE || key == 0x1B || key == 0x2D || key == 0x01) {
            g_RefrainWindowState = 0;
            LogMessage("Hook_IsKeyTriggered: CANCEL triggered -> windowState=0 (preserved subCursor=%d, SelectedReMode=%d)",
                       g_RefrainSubMenuCursor, g_SelectedReMode);
        }
    }

    if (currentScene == 1 && nextScene == 1 && !g_ShowStageMenu && !g_AutoAdvanceToDifficulty && !IsReplayOrDemo()) {
        if (IsConceptReactorTriggered()) {
            if (!IsRefrainWindowOpen() || g_RefrainWindowState != 1) {
                LogMessage("Concept Reactor blocked on Title (windowOpen=%d, state=%d) -> only allowed on post-difficulty screen (DIVE 2 M.R.S.)",
                           IsRefrainWindowOpen(), g_RefrainWindowState);
                return 0;
            }
            DWORD diff = *(volatile DWORD*)0x5b8e28;
            DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
            if (g_RefrainSubMenuCursor == 1 && diff >= 2 && isUnlocked) {
                g_SelectedReMode = 1;
            } else {
                g_SelectedReMode = 0;
            }
            g_ShowStageMenu = 1;
            g_ActiveColumn = 0;
            g_StageSelected = 0;
            g_PracticeMode = 1;
            DWORD curChar = *(volatile DWORD*)0x5b9774;
            if (curChar <= 2) g_PracticeChar = curChar;
            PlayGameSE(1040);
            LogMessage("Concept Reactor opened Stage Menu on post-difficulty screen (char=%lu, diff=%lu, subCursor=%d, ReMode=%d)",
                       g_PracticeChar, diff, g_RefrainSubMenuCursor, g_SelectedReMode);
            return 0;
        }
    }

    return res;
}


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

DWORD __cdecl HandleSetDifficulty(DWORD diff) {
    g_RefrainWindowState = 1;
    g_DifficultyConfirmed = 1;
    DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
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
        "pushl 0x8(%%ebp)\n\t"
        "call _HandleSetDifficulty\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, 0x5b8e28\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "movl %%ebp, %%esp\n\t"
        "popl %%ebp\n\t"
        "ret\n\t"
        :
        :
    );
}

int __cdecl HandleGetGameInfoInt(const char *key, int format) {
    if (!key || IsBadReadPtr(key, 1)) return -1;

    DWORD diff = *(volatile DWORD*)0x5b8e28;
    DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
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

    if (g_PracticeMode && *(volatile DWORD*)0x5c073c == 2) {
        if (strcmp(key, "GameInfo_NoMiss") == 0 ||
            strncmp(key, "stageClear2", 11) == 0 ||
            strncmp(key, "stageClear", 10) == 0) {
            if (*(volatile DWORD*)0x5c0740 != 1) {
                LogMessage("GetGameInfoInt (0x0044fb80): Stage 5 clear detected (key='%s', format=%d, stage=%d) in Practice Mode -> redirecting to Title immediately to skip flavor text",
                           key, format, g_PracticeStage);
                *(volatile DWORD*)0x5c0740 = 1;
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
        "movl $0x0044fb85, %%eax\n\t"
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
    DWORD diff = *(volatile DWORD*)0x5b8e28;
    DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
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
        "movl $0x0044fb80, %%eax\n\t"
        "jmp *%%eax\n\t"
        :
        :
    );
}

DWORD __cdecl HandleGetRefRainConfig(DWORD *pVal) {
    DWORD diff = *(volatile DWORD*)0x5b8e28;
    DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
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
        "pushl $0x00441232\n\t"
        "ret\n\t"
        :
        :
    );
}

DWORD __cdecl HandleGetterReMode(void) {
    DWORD currentScene = *(volatile DWORD*)0x5c073c;
    if (currentScene == 1) {
        // Title screen achievement check: return actual unlock status from save data (do not force)
        if (!IsBadReadPtr((void*)0x5ac4d4, 4)) {
            return *(volatile DWORD*)0x5ac4d4 ? 1 : 0;
        }
        return 0;
    }

    DWORD diff = *(volatile DWORD*)0x5b8e28;
    if (g_PracticeMode) {
        int res = (g_SelectedReMode && diff >= 2) ? 1 : 0;
        LogMessage("HandleGetterReMode (Stage Practice): diff=%lu, g_SelectedReMode=%d -> returning %d",
                   diff, g_SelectedReMode, res);
        return (DWORD)res;
    }

    // Normal game in stage: check pGameState+0x52 or lastRefRain
    char *pInputMgr = *(char**)0x5ac9b4;
    if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 16)) {
        char *pGS = *(char**)(pInputMgr + 8);
        if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x60)) {
            return *(BYTE*)(pGS + 0x52) ? 1 : 0;
        }
    }
    return (*(volatile DWORD*)0x5ac674 == 1) ? 1 : 0;
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
        stageBoxH = 175.0f;
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
        stageBoxH = 115.0f;
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

    DWORD diff = *(volatile DWORD*)0x5b8e28;
    const char *diffLabel = "NORMAL";
    if (diff == 1) diffLabel = "EASY";
    else if (diff == 2) diffLabel = (g_SelectedReMode ? "Re:NORMAL" : "NORMAL");
    else if (diff == 3) diffLabel = (g_SelectedReMode ? "Re:ADVANCED" : "ADVANCED");
    else if (diff == 4) diffLabel = (g_SelectedReMode ? "Re:EXTREME" : "EXTREME");

    char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "STAGE SELECT [%s]", diffLabel);
    DWORD titleCol = (g_SelectedReMode && diff >= 2) ? 0xFFFF4444 : titleCol0;
    DWORD hBg0 = (g_SelectedReMode && diff >= 2) ? 0x88661122 : headerBg0;

    DrawSolidRect(pDevice, startX - 2.0f, stageBoxY - 2.0f, totalW + 4.0f, stageBoxH + 4.0f, borderCol0);
    DrawSolidRect(pDevice, startX, stageBoxY, totalW, stageBoxH, 0xEE0D111A);
    DrawSolidRect(pDevice, startX + 4.0f, stageBoxY + 4.0f, totalW - 8.0f, headerH, hBg0);
    DrawShadowText(g_pTitleFont, titleBuf, (int)startX + (isHD ? 14 : 10), (int)stageBoxY + (isHD ? 8 : 5), titleCol);

    int stageStartY = (int)stageBoxY + (isHD ? 36 : 24);
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

    int paramStartY = (int)paramBoxY + (isHD ? 36 : 24);

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
    char help2Buf[256];
    DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
    if (diff >= 2 && isUnlocked) {
        snprintf(help2Buf, sizeof(help2Buf), "%s    [R] Mode: %s",
                 help2, g_SelectedReMode ? "Re:MODE" : "NORMAL");
        help2 = help2Buf;
    }
    DrawShadowText(pFootFont, help1, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 8 : 5), 0xFF88DDFF);
    DrawShadowText(pFootFont, help2, (int)startX + (isHD ? 16 : 10), (int)footerY + (isHD ? 32 : 23), 0xFF6699BB);

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

            DWORD curChar = *(volatile DWORD*)0x5b9774;
            if (curChar <= 2) g_PracticeChar = curChar;

            DWORD diff = *(volatile DWORD*)0x5b8e28;
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
            *(volatile DWORD*)0x4d2378 = 1;
            *(volatile DWORD*)0x5ac674 = isRefRain;
            SetGameInfoInt("GameInfo_RefRain", isRefRain);
            SetGameInfoInt("lastRefRain", isRefRain);
            g_ShowStageMenu = 0;
            g_StageSelected = 1;
            g_PracticeMode = 1;
            g_PracticeStage = stage;
            g_ReturnToStageMenu = 0;
            g_AutoAdvanceToDifficulty = 0;
            g_ActiveColumn = 0;
            g_PendingStageInitStats = 1;
            g_StageInitRenderFrames = 0;
            InterlockedExchange(&g_PendingReset, 1);
            ResetStageScore();
            BackupGlobalData();
            *(volatile DWORD*)0x5c0740 = 2 + stage;
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
            g_ReturnToStageMenu = 0;
            g_AutoAdvanceToDifficulty = 0;
            g_AutoNavPhase = 0;
            g_AutoNavTimer = 0;
            g_SimulatedKeyAction = SIMKEY_NONE;
            g_ActiveColumn = 0;
            PlayGameSE(1048);
            DWORD prevScene = *(volatile DWORD*)0x5c073c;
            if (prevScene != 1) {
                g_CancelToTitle = 1;
                *(volatile DWORD*)0x5c0740 = 1;
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

void __cdecl OnRenderHook(IDirect3DDevice9 *pDevice) {
    if (g_PendingStageInitStats && !IsReplayOrDemo()) {
        union { float f; DWORD dw; } uMefa, uCr;
        uMefa.dw = g_SelectedMefaDword;
        uCr.dw = g_SelectedCrDword;

        *(volatile DWORD*)0x5b8e08 = (DWORD)g_SelectedLives;
        *(volatile DWORD*)0x5b9770 = g_SelectedMefaDword;
        *(volatile DWORD*)0x5b976c = g_SelectedCrDword;

        char *pGameState = *(char**)0x5ac9b0;
        if (pGameState && (DWORD)pGameState > 0x10000 && !IsBadReadPtr(pGameState, 0x200)) {
            for (int s = 1; s <= 6; s++) {
                *(DWORD*)(pGameState + s * 4 + 0x115) = (DWORD)g_SelectedLives;
                *(DWORD*)(pGameState + s * 4 + 0x12d) = g_SelectedMefaDword;
                *(DWORD*)(pGameState + s * 4 + 0x145) = g_SelectedCrDword;
            }
            DWORD diff = *(volatile DWORD*)0x5b8e28;
            if (diff < 1 || diff > 4) diff = 2;
            BYTE isRe = (g_SelectedReMode && diff >= 2) ? 1 : 0;
            *(BYTE*)(pGameState + 0x52) = isRe;
        }

        char *pInputMgr = *(char**)0x5ac9b4;
        if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 16)) {
            char *pGS = *(char**)(pInputMgr + 8);
            if (pGS && (DWORD)pGS > 0x10000 && !IsBadReadPtr(pGS, 0x60)) {
                DWORD diff = *(volatile DWORD*)0x5b8e28;
                if (diff < 1 || diff > 4) diff = 2;
                BYTE isRe = (g_SelectedReMode && diff >= 2) ? 1 : 0;
                *(BYTE*)(pGS + 0x52) = isRe;
            }
        }

        DWORD diff = *(volatile DWORD*)0x5b8e28;
        if (diff < 1 || diff > 4) diff = 2;
        DWORD isRefRain = (g_SelectedReMode && diff >= 2) ? 1 : 0;
        DWORD curChar = *(volatile DWORD*)0x5b9774;
        if (curChar < 1 || curChar > 3) curChar = 1;
        *(volatile DWORD*)0x5abb80 = (curChar - 1) + 3 * ((diff - 1) + 4 * isRefRain);
        *(volatile DWORD*)0x4d2378 = 1;
        *(volatile DWORD*)0x5ac674 = isRefRain;

        DWORD pPlayer = *(volatile DWORD*)0x5b9778;
        DWORD frameCount = *(volatile DWORD*)0x5abb78;
        DWORD currentScene = *(volatile DWORD*)0x5c073c;

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

    DWORD currentScene = *(volatile DWORD*)0x5c073c;

    static int scriptDumped = 0;
    if (!scriptDumped && currentScene == 1) {
        char *pScript = *(char**)0x5c0760;
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
            if (!g_ShowStageMenu && !g_ReturnToStageMenu && !g_AutoAdvanceToDifficulty) {
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
            // Pulse DECIDE starting at frame 2 until RefRain window opens
            if (g_AutoNavTimer >= 2 && (g_AutoNavTimer % 2 == 0) && !IsRefrainWindowOpen()) {
                g_SimulatedKeyAction = SIMKEY_DECIDE;
            } else {
                g_SimulatedKeyAction = SIMKEY_NONE;
            }
            if (g_AutoNavTimer >= 3 && IsRefrainWindowOpen()) {
                LogMessage("AutoNav: Phase 0 complete! RefRain window opened at timer=%d", g_AutoNavTimer);
                g_AutoNavPhase = 1;
                g_AutoNavTimer = 0;
                g_DifficultyConfirmed = 0;
                g_SimulatedKeyAction = SIMKEY_NONE;
            } else if (g_AutoNavTimer >= 80) {
                LogMessage("AutoNav: Phase 0 fallback timeout (%d frames) -> advancing to Phase 1", g_AutoNavTimer);
                g_AutoNavPhase = 1;
                g_AutoNavTimer = 0;
                g_DifficultyConfirmed = 0;
                g_SimulatedKeyAction = SIMKEY_NONE;
            }
        } else if (g_AutoNavPhase == 1) {
            // Window is open (difficulty selection). Pulse DECIDE every 2 frames until difficulty is confirmed.
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
        // Spin until GPU finishes the frame commands, forcing flip-queue to 0 frames
        while (g_pEventQuery->lpVtbl->GetData(g_pEventQuery, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            // tight spin for minimum latency
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
        // Original EndScene call: call *0xa8(%eax)
        // pDevice is on the stack (pushed by caller at 0x46726c)
        "movl (%%esp), %%ecx\n\t"
        "movl (%%ecx), %%eax\n\t"
        "call *0xa8(%%eax)\n\t"
        // EndScene is stdcall (COM) and has already popped pDevice from stack.
        // Low Latency: flush GPU. Load pDevice from game global 0x4df338.
        "pushal\n\t"
        "movl 0x4df338, %%eax\n\t"
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
        if (!g_ReturnToStageMenu && !g_AutoAdvanceToDifficulty) {
            g_PracticeMode = 0;
            g_PracticeStage = 0;
            RestoreGlobalData();
        }
        g_LastResetTargetScene = 0;
        g_PendingStageInitStats = 0;
        g_StageInitRenderFrames = 0;
        InterlockedExchange(&g_PendingReset, 0);
    }
}

static char *GetReplayGameState(void) {
    char *pInputMgr = *(char**)0x5ac9b4;
    if (pInputMgr && (DWORD)pInputMgr > 0x10000 && !IsBadReadPtr(pInputMgr, 0x20)) {
        if (*(DWORD*)(pInputMgr + 4) == 4) {
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
    DWORD prevScene = *(volatile DWORD*)0x5c073c;
    DWORD nextScene = *(volatile DWORD*)0x5c0740;

    if (g_CancelToTitle) {
        g_CancelToTitle = 0;
        g_PracticeMode = 0;
        g_ReturnToStageMenu = 0;
        LogMessage("Trans: cancel to title allowed (prev=%lu next=%lu)", prevScene, nextScene);
        return nextScene - 1;
    }

    if (IsReplayOrDemo()) {
        LogMessage("Trans: Replay/Demo playback detected (replaySlot=%ld, mode=%lu) -> passing through without menu",
                   (long)*(volatile DWORD*)0x4d237c, *(volatile DWORD*)0x4d2378);
        return prevScene;
    }

    if ((prevScene == 2 || (prevScene >= 3 && prevScene <= 10)) &&
        prevScene != nextScene &&
        g_PracticeMode && g_PendingReset == 0) {

        RestoreGlobalData();

        DWORD curChar = *(volatile DWORD*)0x5b9774;
        if (curChar <= 2) g_PracticeChar = curChar;
        DWORD diff = *(volatile DWORD*)0x5b8e28;
        DWORD isUnlocked = (!IsBadReadPtr((void*)0x5ac4d4, 4) && *(volatile DWORD*)0x5ac4d4) ? 1 : 0;
        int effectiveRe = (g_SelectedReMode && diff >= 2 && isUnlocked) ? 1 : 0;
        SetGameInfoInt("lastRefRain", effectiveRe);

        LogMessage("Practice stage end (prev=%lu next=%lu) -> redirecting to Title with fast AutoNav (char=%lu, ReMode=%d)",
                   prevScene, nextScene, g_PracticeChar, effectiveRe);
        g_ReturnToStageMenu = 0;
        g_AutoAdvanceToDifficulty = 1;
        g_AutoNavPhase = 0;
        g_AutoNavTimer = 0;
        g_DifficultyConfirmed = 0;
        g_SimulatedKeyAction = SIMKEY_NONE;
        g_StageSelected = 0;
        *(volatile DWORD*)0x5c0740 = 1;
        return prevScene;
    }

    if (nextScene >= 3 && nextScene <= 7 && !g_StageSelected) {
        if (g_ShowStageMenu) {
            return nextScene;
        }

        int isC = IsConceptReactorHeld();
        if (isC) {
            g_ShowStageMenu = 1;
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
        g_LastResetTargetScene = nextScene;
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

int __cdecl HandleLoadStart(void) {
    if (IsReplayOrDemo()) {
        return 0;
    }

    if (g_ReturnToStageMenu || g_AutoAdvanceToDifficulty) {
        return 0;
    }

    if (!g_StageSelected) {
        if (g_ShowStageMenu) {
            return 1;
        }
        int isC = IsConceptReactorHeld();
        if (isC) {
            g_ShowStageMenu = 1;
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

    *(volatile DWORD*)0x5abb78 = 0;
    *(volatile DWORD*)0x5abb74 = 0;
    *(volatile DWORD*)0x5abb70 = 0x3c;

    *(volatile DWORD*)0x5b8e20 = 0;
    *(volatile DWORD*)0x5b8e24 = 0;
    *(volatile DWORD*)0x5b8e08 = (DWORD)g_SelectedLives;
    *(volatile DWORD*)0x5b8e1c = 0;
    *(volatile DWORD*)0x5b8e14 = 0;
    *(volatile DWORD*)0x5b8e10 = 0;
    *(volatile DWORD*)0x5b8e18 = 0;
    *(volatile DWORD*)0x5b9770 = g_SelectedMefaDword;
    *(volatile DWORD*)0x5b976c = g_SelectedCrDword;
    *(volatile BYTE*)0x5b975d = 0;

    DWORD diff = *(volatile DWORD*)0x5b8e28;
    if (diff < 1 || diff > 4) diff = 2;
    DWORD isRefRain = (g_SelectedReMode && diff >= 2) ? 1 : 0;
    DWORD curChar = *(volatile DWORD*)0x5b9774;
    if (curChar < 1 || curChar > 3) curChar = 1;
    *(volatile DWORD*)0x5abb80 = (curChar - 1) + 3 * ((diff - 1) + 4 * isRefRain);
    *(volatile DWORD*)0x4d2378 = 1;
    *(volatile DWORD*)0x5ac674 = isRefRain;
    SetGameInfoInt("GameInfo_RefRain", isRefRain);
    SetGameInfoInt("lastRefRain", isRefRain);

    char *pGameState = *(char**)0x5ac9b0;
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

static DWORD WINAPI HotkeyThread(LPVOID param) {
    (void)param;
    int key_state[5] = {0};

    while (1) {
        Sleep(50);

        if (!g_PracticeMode && !IsReplayOrDemo()) {
            memset(key_state, 0, sizeof(key_state));
            continue;
        }

        DWORD currentScene = *(volatile DWORD*)0x5c073c;

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
                            *(volatile DWORD*)0x5c0740 = 2 + stage;
                            PlayGameSE(1040);
                        }
                    } else if (g_PracticeMode) {
                        LogMessage("Hotkey F%d -> Warping to Stage %d (currentScene=%lu)", stage, stage, currentScene);
                        g_PracticeStage = stage;
                        g_MenuCursor = stage - 1;
                        g_StageSelected = 1;
                        g_PendingStageInitStats = 1;
                        g_StageInitRenderFrames = 0;
                        InterlockedExchange(&g_PendingReset, 1);
                        ResetStageScore();
                        BackupGlobalData();
                        *(volatile DWORD*)0x5c0740 = 2 + stage;
                        PlayGameSE(1040);
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

    void *transAddr = (void*)0x00440deb;
    unsigned char expectedTrans[5] = { 0xA1, 0x3C, 0x07, 0x5C, 0x00 };
    if (memcmp(transAddr, expectedTrans, 5) == 0) {
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

    void *loadStartAddr = (void*)0x00441097;
    unsigned char expectedLoadStart[7] = { 0x32, 0xC9, 0xC7, 0x05, 0xDC, 0xC4, 0x5A };
    if (memcmp(loadStartAddr, expectedLoadStart, 7) == 0) {
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

    void *sceneAddr = (void*)0x00440ff8;
    unsigned char expectedScene[6] = { 0x8B, 0x15, 0x40, 0x07, 0x5C, 0x00 };
    if (memcmp(sceneAddr, expectedScene, 6) == 0) {
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

    void *endSceneAddr = (void*)0x0046726d;
    unsigned char expectedEndScene[6] = { 0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00 };
    if (memcmp(endSceneAddr, expectedEndScene, 6) == 0) {
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

    void *setMefaAddr = (void*)0x00462430;
    unsigned char expectedSetMefa[13] = {
        0xF3, 0x0F, 0x10, 0x45, 0x08,
        0xF3, 0x0F, 0x11, 0x05, 0x70, 0x97, 0x5B, 0x00
    };
    if (memcmp(setMefaAddr, expectedSetMefa, 13) == 0) {
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

    void *setCrAddr = (void*)0x00462443;
    unsigned char expectedSetCr[13] = {
        0xF3, 0x0F, 0x10, 0x45, 0x08,
        0xF3, 0x0F, 0x11, 0x05, 0x6C, 0x97, 0x5B, 0x00
    };
    if (memcmp(setCrAddr, expectedSetCr, 13) == 0) {
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

    void *setLivesAddr = (void*)0x00462535;
    unsigned char expectedSetLives[8] = {
        0x8B, 0x45, 0x08,
        0xA3, 0x08, 0x8E, 0x5B, 0x00
    };
    if (memcmp(setLivesAddr, expectedSetLives, 8) == 0) {
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

    void *setDiffAddr = (void*)0x004625dc;
    unsigned char expectedSetDiff[8] = {
        0x8B, 0x45, 0x08,
        0xA3, 0x28, 0x8E, 0x5B, 0x00
    };
    if (memcmp(setDiffAddr, expectedSetDiff, 8) == 0) {
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

    void *refRainCfgAddr = (void*)0x00441229;
    unsigned char expectedRefRainCfg[9] = { 0x8B, 0x00, 0xC7, 0x45, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF };
    if (memcmp(refRainCfgAddr, expectedRefRainCfg, 9) == 0) {
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

    void *refRainGameInfoAddr = (void*)0x0043078e;
    unsigned char expectedRefRainGameInfo[5] = { 0xE8, 0xED, 0xF3, 0x01, 0x00 };
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

    void *gameInfoIntAddr = (void*)0x0044fb80;
    unsigned char expectedGameInfoInt[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    if (memcmp(gameInfoIntAddr, expectedGameInfoInt, 5) == 0) {
        if (VirtualProtect(gameInfoIntAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9; // JMP rel32
            DWORD relOffset = (DWORD)Hook_GetGameInfoInt - ((DWORD)gameInfoIntAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(gameInfoIntAddr, patch, 5);
            VirtualProtect(gameInfoIntAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), gameInfoIntAddr, 5);
            LogMessage("GetGameInfoInt (0x0044fb80) global hook installed successfully");
        }
    } else {
        LogMessage("[Error] GetGameInfoInt signature at 0x%08X did not match!", gameInfoIntAddr);
    }

    void *getterTableEntry19 = (void*)0x00462dfc;
    DWORD origGetter19 = 0x00462c49;
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

    void *inputVtableAddr = (void*)0x004b80c4;
    DWORD origIsKey[2] = { 0x00431760, 0x004317a0 };
    if (memcmp(inputVtableAddr, origIsKey, 8) == 0) {
        if (VirtualProtect(inputVtableAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *(DWORD*)0x004b80c4 = (DWORD)Hook_IsKeyPressed;
            *(DWORD*)0x004b80c8 = (DWORD)Hook_IsKeyTriggered;
            VirtualProtect(inputVtableAddr, 8, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), inputVtableAddr, 8);
            LogMessage("InputMgr vtable hooked successfully at 0x%08X", inputVtableAddr);
        }
    } else {
        LogMessage("[Error] InputMgr vtable signature at 0x%08X did not match! (found 0x%08X, 0x%08X)",
                   inputVtableAddr, *(DWORD*)0x004b80c4, *(DWORD*)0x004b80c8);
    }

    void *saveGlobalAddr = (void*)0x0042c330;
    unsigned char expectedSaveGlobal[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    if (memcmp(saveGlobalAddr, expectedSaveGlobal, 5) == 0) {
        if (VirtualProtect(saveGlobalAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SaveGlobalData - ((DWORD)saveGlobalAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(saveGlobalAddr, patch, 5);
            VirtualProtect(saveGlobalAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), saveGlobalAddr, 5);
            LogMessage("SaveGlobalData (0x0042c330) hook installed successfully");
        }
    } else {
        LogMessage("[Error] SaveGlobalData signature at 0x0042c330 did not match!");
    }

    void *setGameInfoAddr = (void*)0x0044fca0;
    unsigned char expectedSetGameInfo[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };
    if (memcmp(setGameInfoAddr, expectedSetGameInfo, 5) == 0) {
        if (VirtualProtect(setGameInfoAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            unsigned char patch[5];
            patch[0] = 0xE9;
            DWORD relOffset = (DWORD)Hook_SetGameInfoInt - ((DWORD)setGameInfoAddr + 5);
            memcpy(&patch[1], &relOffset, 4);
            memcpy(setGameInfoAddr, patch, 5);
            VirtualProtect(setGameInfoAddr, 5, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), setGameInfoAddr, 5);
            LogMessage("SetGameInfoInt (0x0044fca0) hook installed successfully");
        }
    } else {
        LogMessage("[Error] SetGameInfoInt signature at 0x0044fca0 did not match!");
    }

    // Ultra Low Latency Patch: Force D3DPRESENT_INTERVAL_IMMEDIATE (VSync OFF)
    // At 0x00424fb9: mov dword ptr [0x4df324], 1 (EnableAutoDepthStencil)
    // Replace with: mov dword ptr [0x4df334], 0x80000000 (PresentationInterval = IMMEDIATE)
    void *d3dPresentIntAddr = (void*)0x00424fb9;
    unsigned char expectedD3dPresentInt[10] = {
        0xC7, 0x05, 0x24, 0xF3, 0x4D, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    if (memcmp(d3dPresentIntAddr, expectedD3dPresentInt, 10) == 0) {
        if (VirtualProtect(d3dPresentIntAddr, 10, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // mov dword ptr [0x4df334], 0x80000000
            // Also explicitly ensure EnableAutoDepthStencil (0x4df324) = 1 in memory
            *(DWORD*)0x4df324 = 1;
            unsigned char patch[10] = {
                0xC7, 0x05, 0x34, 0xF3, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x80
            };
            memcpy(d3dPresentIntAddr, patch, 10);
            VirtualProtect(d3dPresentIntAddr, 10, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), d3dPresentIntAddr, 10);
            LogMessage("Ultra Low Latency: PresentationInterval patched to D3DPRESENT_INTERVAL_IMMEDIATE at 0x%08X", d3dPresentIntAddr);
        }
    } else {
        LogMessage("[Warning] D3DPresentInterval signature at 0x%08X did not match!", d3dPresentIntAddr);
    }
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
