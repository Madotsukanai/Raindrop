/* Shared GitHub Releases self-updater for Raindrop (header-only).
   Requires windows.h and a Log(fmt, ...) macro plus a VERSION macro from the includer. */
#ifndef RAIN_UPDATE_H
#define RAIN_UPDATE_H

#include <wininet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_ruApiUrl = "https://api.github.com/repos/Madotsukanai/Raindrop/releases/latest";
static HMODULE g_ruModule = NULL;
static int g_ruUpdateCheck = 1;
static char g_ruUpdateSkip[64] = "";

static int RuBool(const char *value, int def) {
    if (!_stricmp(value, "1") || !_stricmp(value, "on") ||
        !_stricmp(value, "true") || !_stricmp(value, "yes")) return 1;
    if (!_stricmp(value, "0") || !_stricmp(value, "off") ||
        !_stricmp(value, "false") || !_stricmp(value, "no")) return 0;
    return def;
}

static void RainUpdateIniPath(char *out) {
    char *slash;
    GetModuleFileNameA(NULL, out, MAX_PATH);
    slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, "raindrop.ini");
    else strcpy(out, "raindrop.ini");
}

static int RuHasSection(const char *buf, const char *name) {
    const char *p = buf;
    size_t len = strlen(name);
    while ((p = strchr(p, '[')) != NULL) {
        if (!_strnicmp(p + 1, name, len) && p[1 + len] == ']') return 1;
        p++;
    }
    return 0;
}

/* Ensures raindrop.ini exists and contains the auto-update settings only. */
static void RainUpdateEnsureIni(void) {
    static const char block[] = "[General]\r\nUpdateCheck=1\r\nUpdateSkip=\r\n\r\n";
    char path[MAX_PATH];
    FILE *f;
    RainUpdateIniPath(path);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, block, (DWORD)(sizeof(block) - 1), &written, NULL);
            CloseHandle(file);
            Log("Update: created raindrop.ini with [General] update settings");
        }
    } else {
        long size;
        char *buf;
        f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            buf = (char*)malloc((size_t)size + 1);
            if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
                buf[size] = 0;
                if (!RuHasSection(buf, "general")) {
                    fclose(f);
                    f = fopen(path, "wb");
                    if (f) {
                        fwrite(block, 1, sizeof(block) - 1, f);
                        fwrite(buf, 1, (size_t)size, f);
                        Log("Update: added the [General] section to raindrop.ini");
                    } else {
                        Log("Update: could not rewrite raindrop.ini");
                    }
                }
                free(buf);
            } else if (buf) {
                free(buf);
            }
            if (f) fclose(f);
        }
    }
    {
        char value[32];
        GetPrivateProfileStringA("General", "UpdateCheck", "\x1notfound", value, sizeof(value), path);
        if (!strcmp(value, "\x1notfound")) {
            WritePrivateProfileStringA("General", "UpdateCheck", "1", path);
            Log("Update: added the missing UpdateCheck entry");
        }
    }
    {
        char value[64];
        GetPrivateProfileStringA("General", "UpdateSkip", "\x1notfound", value, sizeof(value), path);
        if (!strcmp(value, "\x1notfound")) {
            WritePrivateProfileStringA("General", "UpdateSkip", "", path);
            Log("Update: added the missing UpdateSkip entry");
        }
    }
}

static void RainUpdateInit(HMODULE module) {
    char value[32];
    g_ruModule = module;
    RainUpdateEnsureIni();
    {
        char path[MAX_PATH];
        RainUpdateIniPath(path);
        GetPrivateProfileStringA("General", "UpdateCheck", "1", value, sizeof(value), path);
        g_ruUpdateCheck = RuBool(value, 1);
        GetPrivateProfileStringA("General", "UpdateSkip", "", g_ruUpdateSkip, sizeof(g_ruUpdateSkip), path);
        Log("Update: check %s, skip=%s", g_ruUpdateCheck ? "enabled" : "disabled",
            g_ruUpdateSkip[0] ? g_ruUpdateSkip : "none");
    }
}

/* ---- GitHub Releases self-updater ----------------------------------------- */
typedef struct {
    char *data;
    size_t size;
    size_t cap;
} UpdateBuffer;

typedef HINTERNET (WINAPI *PF_InternetOpenA)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
typedef HINTERNET (WINAPI *PF_InternetOpenUrlA)(HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *PF_InternetReadFile)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *PF_InternetCloseHandle)(HINTERNET);
typedef BOOL (WINAPI *PF_InternetSetOptionA)(HINTERNET, DWORD, LPVOID, DWORD);

static int UpdateBufferAppend(UpdateBuffer *b, const void *data, size_t len) {
    char *p;
    size_t cap;
    if (b->size + len + 1 > b->cap) {
        cap = b->cap ? b->cap : 4096;
        while (cap < b->size + len + 1) cap *= 2;
        p = (char*)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->size, data, len);
    b->size += len;
    b->data[b->size] = 0;
    return 1;
}

static int UpdateFetch(const char *url, UpdateBuffer *out, size_t limit) {
    HMODULE wi = LoadLibraryA("wininet.dll");
    PF_InternetOpenA open;
    PF_InternetOpenUrlA openUrl;
    PF_InternetReadFile readFn;
    PF_InternetCloseHandle closeFn;
    PF_InternetSetOptionA setOpt;
    HINTERNET root = NULL, req = NULL;
    int accessTypes[2] = { INTERNET_OPEN_TYPE_PRECONFIG, INTERNET_OPEN_TYPE_DIRECT };
    int attempt, ok = 0;
    DWORD timeout;

    if (!wi) { Log("Update: wininet.dll unavailable"); return 0; }
    open = (PF_InternetOpenA)GetProcAddress(wi, "InternetOpenA");
    openUrl = (PF_InternetOpenUrlA)GetProcAddress(wi, "InternetOpenUrlA");
    readFn = (PF_InternetReadFile)GetProcAddress(wi, "InternetReadFile");
    closeFn = (PF_InternetCloseHandle)GetProcAddress(wi, "InternetCloseHandle");
    setOpt = (PF_InternetSetOptionA)GetProcAddress(wi, "InternetSetOptionA");
    if (!open || !openUrl || !readFn || !closeFn || !setOpt) {
        Log("Update: wininet.dll exports missing");
        FreeLibrary(wi);
        return 0;
    }
    for (attempt = 0; attempt < 2 && !root; attempt++)
        root = open("Raindrop Updater", (DWORD)accessTypes[attempt], NULL, NULL, 0);
    if (!root) {
        Log("Update: InternetOpenA failed (err=%lu)", (unsigned long)GetLastError());
        FreeLibrary(wi);
        return 0;
    }
    timeout = 5000;
    setOpt(root, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    setOpt(root, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    timeout = 15000;
    setOpt(root, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    req = openUrl(root, url, NULL, 0,
                  INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                  INTERNET_FLAG_NO_UI | INTERNET_FLAG_SECURE, 0);
    if (!req) {
        Log("Update: cannot open %s (err=%lu)", url, (unsigned long)GetLastError());
        closeFn(root);
        FreeLibrary(wi);
        return 0;
    }
    setOpt(req, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    timeout = 5000;
    setOpt(req, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    setOpt(req, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    ok = 1;
    for (;;) {
        char chunk[8192];
        DWORD got = 0;
        if (!readFn(req, chunk, sizeof(chunk), &got) || got == 0) break;
        if (out->size + got > limit) {
            Log("Update: response exceeds %u bytes", (unsigned)limit);
            ok = 0;
            break;
        }
        if (!UpdateBufferAppend(out, chunk, got)) {
            ok = 0;
            break;
        }
    }
    closeFn(req);
    closeFn(root);
    FreeLibrary(wi);
    if (ok && out->size) return 1;
    free(out->data);
    out->data = NULL;
    out->size = 0;
    out->cap = 0;
    return 0;
}

static const char *UpdateFindInRange(const char *from, const char *end, const char *needle) {
    size_t n = strlen(needle);
    const char *p;
    if (!from || !end) return NULL;
    for (p = from; p <= end && p + n <= end; p++)
        if (memcmp(p, needle, n) == 0) return p;
    return NULL;
}

static int UpdateParseString(const char *from, const char *end, char *out, size_t cap) {
    const char *p = from;
    size_t n = 0;
    if (!p || !end) return 0;
    /* from points at the opening quote of the JSON key; skip the key itself */
    if (*p == '"') {
        p++;
        while (p < end && *p != '"') p++;
        if (p < end) p++;
    }
    while (p < end && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != '"') return 0;
    p++;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            if (p >= end) break;
            c = *p;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
        }
        if (n + 1 < cap) out[n++] = c;
        p++;
    }
    out[n] = 0;
    return n > 0;
}

static int UpdateReleaseTag(const char *json, const char *end, char *out, size_t cap) {
    const char *p = UpdateFindInRange(json, end, "\"tag_name\"");
    return p ? UpdateParseString(p, end, out, cap) : 0;
}

static int UpdateAssetField(const char *json, const char *end, const char *name,
                            const char *field, char *out, size_t cap) {
    const char *assets = UpdateFindInRange(json, end, "\"assets\"");
    const char *p, *objStart, *objEnd;
    char curName[128];
    if (!assets) return 0;
    p = assets;
    while (p < end && *p != '[') p++;
    if (p >= end) return 0;
    p++;
    while (p < end) {
        const char *np, *fp;
        int depth = 0;
        objStart = p;
        while (objStart < end && *objStart != '{') objStart++;
        if (objStart >= end) break;
        objEnd = objStart;
        while (objEnd < end) {
            if (*objEnd == '{') depth++;
            else if (*objEnd == '}' && --depth == 0) break;
            objEnd++;
        }
        if (objEnd >= end) break;
        np = UpdateFindInRange(objStart, objEnd, "\"name\"");
        if (np && UpdateParseString(np, objEnd, curName, sizeof(curName)) &&
            _stricmp(curName, name) == 0) {
            fp = UpdateFindInRange(objStart, objEnd, field);
            if (fp && UpdateParseString(fp, objEnd, out, cap)) return 1;
        }
        p = objEnd + 1;
    }
    return 0;
}

static int UpdateHexByte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* GitHub publishes asset digests as "sha256:<64 hex characters>". */
static int UpdateDigestBytes(const char *digest, unsigned char out[32]) {
    int hi, lo, i;
    if (_strnicmp(digest, "sha256:", 7) != 0) return 0;
    digest += 7;
    for (i = 0; i < 32; i++) {
        hi = UpdateHexByte(digest[i * 2]);
        lo = UpdateHexByte(digest[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

/* Compact SHA-256 (FIPS 180-4), used to verify downloaded release assets. */
typedef struct {
    uint32_t h[8];
    unsigned char buf[64];
    size_t len;
    uint64_t total;
} Sha256Ctx;

static uint32_t ShaRotate(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void ShaTransform(Sha256Ctx *s, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2, s0, s1, ch, maj;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (; i < 64; i++) {
        s0 = ShaRotate(w[i - 15], 7) ^ ShaRotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ShaRotate(w[i - 2], 17) ^ ShaRotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        s1 = ShaRotate(e, 6) ^ ShaRotate(e, 11) ^ ShaRotate(e, 25);
        ch = (e & f) ^ ((~e) & g);
        t1 = h + s1 + ch + k[i] + w[i];
        s0 = ShaRotate(a, 2) ^ ShaRotate(a, 13) ^ ShaRotate(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void ShaInit(Sha256Ctx *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->total = 0;
}

static void ShaUpdate(Sha256Ctx *s, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char*)data;
    size_t need;
    s->total += n;
    if (s->len) {
        need = 64 - s->len;
        if (n < need) need = n;
        memcpy(s->buf + s->len, p, need);
        s->len += need; p += need; n -= need;
        if (s->len == 64) { ShaTransform(s, s->buf); s->len = 0; }
    }
    while (n >= 64) { ShaTransform(s, p); p += 64; n -= 64; }
    if (n) { memcpy(s->buf, p, n); s->len = n; }
}

static void ShaFinal(Sha256Ctx *s, unsigned char out[32]) {
    uint64_t bits = s->total * 8;
    int i;
    s->buf[s->len++] = 0x80;
    if (s->len > 56) {
        memset(s->buf + s->len, 0, 64 - s->len);
        ShaTransform(s, s->buf);
        s->len = 0;
    }
    memset(s->buf + s->len, 0, 56 - s->len);
    s->buf[56] = (unsigned char)(bits >> 56); s->buf[57] = (unsigned char)(bits >> 48);
    s->buf[58] = (unsigned char)(bits >> 40); s->buf[59] = (unsigned char)(bits >> 32);
    s->buf[60] = (unsigned char)(bits >> 24); s->buf[61] = (unsigned char)(bits >> 16);
    s->buf[62] = (unsigned char)(bits >> 8);  s->buf[63] = (unsigned char)bits;
    ShaTransform(s, s->buf);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

static void UpdateSha256(const void *data, size_t size, unsigned char out[32]) {
    Sha256Ctx ctx;
    ShaInit(&ctx);
    ShaUpdate(&ctx, data, size);
    ShaFinal(&ctx, out);
}

static int UpdateVersionCompare(const char *a, const char *b) {
    int ai[4] = { 0, 0, 0, 0 }, bi[4] = { 0, 0, 0, 0 };
    int ac = 0, bc = 0, i;
    const char *p;
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    p = a;
    while (*p && ac < 4) {
        if (*p >= '0' && *p <= '9') { ai[ac] = ai[ac] * 10 + (*p - '0'); p++; continue; }
        if (*p == '.') { ac++; p++; continue; }
        break;
    }
    p = b;
    while (*p && bc < 4) {
        if (*p >= '0' && *p <= '9') { bi[bc] = bi[bc] * 10 + (*p - '0'); p++; continue; }
        if (*p == '.') { bc++; p++; continue; }
        break;
    }
    for (i = 0; i < 4; i++) {
        if (ai[i] != bi[i]) return ai[i] > bi[i] ? 1 : -1;
    }
    return 0;
}

static void UpdateTrim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

static void UpdateModulePath(char *out) {
    HMODULE mod = g_ruModule ? g_ruModule : GetModuleHandleA("ddraw.dll");
    GetModuleFileNameA(mod, out, MAX_PATH);
}

static void UpdateSiblingPath(const char *base, const char *name, char *out) {
    char *slash;
    strcpy(out, base);
    slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, name);
    else strcpy(out, name);
}

static void UpdateCurrentVersion(char *out, size_t cap) {
    char path[MAX_PATH], dll[MAX_PATH];
    FILE *f;
    UpdateModulePath(dll);
    UpdateSiblingPath(dll, "raindrop.version", path);
    f = fopen(path, "r");
    if (f) {
        if (fgets(out, (int)cap, f)) {
            UpdateTrim(out);
            if (out[0]) { fclose(f); return; }
        }
        fclose(f);
    }
    strncpy(out, VERSION, cap - 1);
    out[cap - 1] = 0;
}

static int UpdateWriteFile(const char *path, const void *data, size_t size) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, data, (DWORD)size, &written, NULL);
    CloseHandle(f);
    return ok && written == size;
}

static int UpdateMessageBox(const char *title, const char *text, DWORD type) {
    wchar_t titleW[256], textW[3072];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, titleW, 256);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, textW, 3072);
    return MessageBoxW(NULL, textW, titleW, type);
}

/* A detached cmd.exe waits for this process to exit, then relaunches the game so the
   freshly installed DLL is loaded.  Cannot be done from the loader lock in DllMain. */
static void UpdateScheduleRestart(const char *dllPath) {
    char batPath[MAX_PATH], exePath[MAX_PATH], cmd[MAX_PATH * 2];
    FILE *f;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD pid = GetCurrentProcessId();

    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    UpdateSiblingPath(dllPath, "raindrop_restart.bat", batPath);
    f = fopen(batPath, "w");
    if (!f) {
        Log("Update: cannot create restart script %s", batPath);
        return;
    }
    fprintf(f, "@echo off\n");
    fprintf(f, ":wait\n");
    fprintf(f, "tasklist /FI \"PID eq %lu\" 2>NUL | find \"%lu\" >NUL\n",
            (unsigned long)pid, (unsigned long)pid);
    fprintf(f, "if not errorlevel 1 (\n  ping -n 2 127.0.0.1 >NUL\n  goto wait\n)\n");
    fprintf(f, "ping -n 3 127.0.0.1 >NUL\n");
    fprintf(f, "start \"\" \"%s\"\n", exePath);
    fprintf(f, "del \"%%~f0\"\n");
    fclose(f);

    snprintf(cmd, sizeof(cmd), "cmd.exe /c \"%s\"", batPath);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("Update: restart scheduled for pid %lu -> %s", (unsigned long)pid, exePath);
    } else {
        Log("Update: failed to start restart script (err=%lu)", (unsigned long)GetLastError());
    }
}

static void RainUpdateTick(void) {
    static int checked = 0;
    UpdateBuffer json, dll;
    char tag[64], cur[64], skip[64], notes[600], text[4096];
    char ini[MAX_PATH], dllPath[MAX_PATH], newPath[MAX_PATH], oldPath[MAX_PATH], verPath[MAX_PATH];
    char assetUrl[1024], assetName[MAX_PATH], digest[160], *slash;
    const char *body, *jsonEnd;
    unsigned char expected[32], actual[32];
    int ret;
    DWORD err;

    if (checked) return;
    checked = 1;
    if (!g_ruUpdateCheck) {
        Log("Update: disabled by raindrop.ini");
        return;
    }

    UpdateModulePath(dllPath);
    UpdateSiblingPath(dllPath, "ddraw.dll.old", oldPath);
    DeleteFileA(oldPath);

    memset(&json, 0, sizeof(json));
    if (!UpdateFetch(g_ruApiUrl, &json, 2 * 1024 * 1024)) {
        Log("Update: release check failed (offline?)");
        return;
    }
    jsonEnd = json.data + json.size;
    if (!UpdateReleaseTag(json.data, jsonEnd, tag, sizeof(tag)) || !tag[0]) {
        Log("Update: response has no tag_name");
        free(json.data);
        return;
    }
    UpdateCurrentVersion(cur, sizeof(cur));
    RainUpdateIniPath(ini);
    GetPrivateProfileStringA("General", "UpdateSkip", "", skip, sizeof(skip), ini);
    Log("Update: current=%s latest=%s skip=%s", cur, tag, skip[0] ? skip : "none");
    if (UpdateVersionCompare(tag, cur) <= 0 || _stricmp(tag, skip) == 0) {
        free(json.data);
        return;
    }

    notes[0] = 0;
    body = UpdateFindInRange(json.data, jsonEnd, "\"body\"");
    if (body) UpdateParseString(body, jsonEnd, notes, sizeof(notes));
    snprintf(text, sizeof(text),
             "Raindrop に新しいバージョンがあります。\n\n"
             "現在のバージョン: %s\n最新のバージョン: %s\n\n"
             "%s\n\n"
             "ダウンロードして更新しますか？\n",
             cur, tag, notes);
    ret = UpdateMessageBox("Raindrop アップデート", text,
                           MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (ret != IDYES) {
        WritePrivateProfileStringA("General", "UpdateSkip", tag, ini);
        Log("Update: user declined %s", tag);
        free(json.data);
        return;
    }

    slash = strrchr(dllPath, '\\');
    strcpy(assetName, slash ? slash + 1 : dllPath);
    if (!UpdateAssetField(json.data, jsonEnd, assetName, "\"browser_download_url\"",
                          assetUrl, sizeof(assetUrl))) {
        Log("Update: no release asset for %s", assetName);
        UpdateMessageBox("Raindrop アップデート", "このリリースには更新対象の DLL がありません。",
                         MB_OK | MB_ICONWARNING);
        free(json.data);
        return;
    }
    digest[0] = 0;
    if (!UpdateAssetField(json.data, jsonEnd, assetName, "\"digest\"", digest, sizeof(digest))) {
        Log("Update: no SHA-256 digest for %s", assetName);
        UpdateMessageBox("Raindrop アップデート",
                         "GitHub から SHA-256 ハッシュを取得できませんでした。更新を中止します。",
                         MB_OK | MB_ICONERROR);
        free(json.data);
        return;
    }
    if (!UpdateDigestBytes(digest, expected)) {
        Log("Update: invalid SHA-256 digest '%s'", digest);
        UpdateMessageBox("Raindrop アップデート", "SHA-256 ハッシュの形式が不正です。更新を中止します。",
                         MB_OK | MB_ICONERROR);
        free(json.data);
        return;
    }
    Log("Update: SHA-256 digest %s", digest);
    free(json.data);

    memset(&dll, 0, sizeof(dll));
    if (!UpdateFetch(assetUrl, &dll, 64 * 1024 * 1024)) {
        Log("Update: failed to download %s", assetUrl);
        UpdateMessageBox("Raindrop アップデート", "更新ファイルのダウンロードに失敗しました。",
                         MB_OK | MB_ICONERROR);
        return;
    }
    Log("Update: downloaded %s (%lu bytes)", assetUrl, (unsigned long)dll.size);

    UpdateSha256(dll.data, dll.size, actual);
    if (memcmp(actual, expected, 32) != 0) {
        char gotHex[65];
        int i;
        for (i = 0; i < 32; i++)
            snprintf(gotHex + i * 2, 3, "%02x", actual[i]);
        Log("Update: SHA-256 mismatch (got %s, expected %s)", gotHex, digest);
        UpdateMessageBox("Raindrop アップデート",
                         "SHA-256 ハッシュが一致しませんでした。更新を中止しました。",
                         MB_OK | MB_ICONERROR);
        free(dll.data);
        return;
    }
    Log("Update: SHA-256 verified");

    strcpy(newPath, dllPath);
    strcat(newPath, ".new");
    UpdateSiblingPath(dllPath, "raindrop.version", verPath);
    if (!UpdateWriteFile(newPath, dll.data, dll.size)) {
        Log("Update: cannot write %s (err=%lu)", newPath, (unsigned long)GetLastError());
        UpdateMessageBox("Raindrop アップデート", "更新ファイルの保存に失敗しました。",
                         MB_OK | MB_ICONERROR);
        free(dll.data);
        return;
    }
    if (MoveFileA(dllPath, oldPath)) {
        if (MoveFileA(newPath, dllPath)) {
            UpdateWriteFile(verPath, tag, strlen(tag));
            DeleteFileA(oldPath);
            Log("Update: installed %s", tag);
            UpdateMessageBox("Raindrop アップデート",
                             "アップデートを適用しました。ゲームを再起動します。",
                             MB_OK | MB_ICONINFORMATION);
            UpdateScheduleRestart(dllPath);
            PostQuitMessage(0);
        } else {
            err = GetLastError();
            MoveFileA(oldPath, dllPath);
            Log("Update: failed to install new DLL (err=%lu)", (unsigned long)err);
            UpdateMessageBox("Raindrop アップデート", "更新ファイルの適用に失敗しました。",
                             MB_OK | MB_ICONERROR);
        }
    } else {
        Log("Update: running DLL cannot be renamed; left %s", newPath);
        snprintf(text, sizeof(text),
                 "更新ファイルを %s に保存しました。\nゲーム終了後に手動で入れ替えてください。",
                 newPath);
        UpdateMessageBox("Raindrop アップデート", text, MB_OK | MB_ICONINFORMATION);
    }
    free(dll.data);
}

#endif
