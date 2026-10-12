/*
 * The Win32 calls this game makes that pcrecomp's win32hle does not answer.
 *
 * win32hle grew up on games of the Windows 95 years, which use the narrow
 * (ANSI) API throughout; this exe is built with Visual C++ 2017, whose C
 * runtime and whose own code call the wide (UTF-16) one. Most of what is here
 * is a wide call forwarded to the narrow one win32hle has: the strings are
 * narrowed into guest memory, the argument slots repointed, and the narrow
 * shim run on the same stack frame (same arguments, same stdcall purge).
 * The game's text is its own (Shift-JIS and ASCII in its archives); paths,
 * window titles and ini keys are all ASCII, so narrowing by code unit loses
 * nothing that is used.
 *
 * The rest are small answers the newer C runtime wants on its way to WinMain,
 * and refusals for what this host does not have yet (those say so).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32hle.h"

#define SETARG(i, v) (MEM32(g_esp + 4u + 4u * (uint32_t)(i)) = (uint32_t)(v))
#define ERROR_FILE_NOT_FOUND_ 2u
#define ERROR_MOD_NOT_FOUND_ 126u
#define ERROR_DEVICE_NOT_CONNECTED_ 1167u
#define FC_IMAGE_BASE 0x00400000u

/* ---- strings ----------------------------------------------------------- */

static uint32_t wlen(uint32_t wva) {
    const uint16_t* w = (const uint16_t*)G2H(wva);
    uint32_t n = 0;
    while (w[n]) n++;
    return n;
}

/* A narrow copy, in guest memory, of a wide guest string. */
static uint32_t narrow_dup(uint32_t wva) {
    uint32_t n = wlen(wva), va = hle_guest_alloc(n + 1);
    const uint16_t* w = (const uint16_t*)G2H(wva);
    char* a = (char*)G2H(va);
    for (uint32_t i = 0; i < n; i++) a[i] = w[i] < 256 ? (char)w[i] : '?';
    a[n] = 0;
    return va;
}

/* A narrow guest string into a wide guest buffer of `cap` code units. */
static uint32_t widen_into(uint32_t wva, const char* a, uint32_t cap) {
    uint16_t* w = (uint16_t*)G2H(wva);
    uint32_t n = 0;
    if (!cap) return 0;
    while (a[n] && n + 1 < cap) { w[n] = (uint8_t)a[n]; n++; }
    w[n] = 0;
    return n;
}

static uint32_t wide_dup(const char* a) {
    uint32_t n = (uint32_t)strlen(a), va = hle_guest_alloc(2 * (n + 1));
    widen_into(va, a, n + 1);
    return va;
}

/* ---- forwarding to the narrow shim -------------------------------------- */

static void tail(const char* name) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) hle_fatal("%s is needed to answer a wide call and win32hle has none", name);
    f();
}

/* Narrow the string arguments named in `mask` (bit i = argument i; values
 * below 64 KB are atoms or NULL and pass through), run the narrow shim. */
static void forward(const char* narrow, unsigned mask) {
    uint32_t tmp[8];
    int n = 0;
    for (int i = 0; i < 16 && n < 8; i++) {
        if (!(mask & (1u << i))) continue;
        uint32_t w = A32(i);
        if (w < 0x10000u) continue;
        tmp[n] = narrow_dup(w);
        SETARG(i, tmp[n]);
        n++;
    }
    tail(narrow);
    while (n) hle_guest_free(tmp[--n]);
}

/* The same for a call that fills a wide buffer: argument `out` is the buffer,
 * `cap` its size in characters, and the narrow call's result is the length. */
static void forward_out(const char* narrow, unsigned mask, int out, uint32_t cap) {
    uint32_t wbuf = A32(out), tmp = hle_guest_alloc(cap + 1);
    SETARG(out, tmp);
    forward(narrow, mask);
    if (wbuf >= 0x10000u) widen_into(wbuf, (const char*)G2H(tmp), cap);
    hle_guest_free(tmp);
}

/* Drop the last `drop` of `argc` stdcall arguments and run a shim that takes
 * the rest: the return address stays on top. */
static void forward_fewer(const char* name, int argc, int drop) {
    uint32_t ret = MEM32(g_esp);
    for (int i = argc - drop - 1; i >= 0; i--) MEM32(g_esp + 4u * (uint32_t)(drop + 1 + i)) = A32(i);
    g_esp += 4u * (uint32_t)drop;
    MEM32(g_esp) = ret;
    tail(name);
}

#define W2A(name, narrow, mask) static void w_##name(void) { forward(narrow, mask); }
W2A(CreateFileW, "CreateFileA", 1u << 0)
W2A(DeleteFileW, "DeleteFileA", 1u << 0)
W2A(CreateDirectoryW, "CreateDirectoryA", 1u << 0)
W2A(GetFileAttributesW, "GetFileAttributesA", 1u << 0)
W2A(SetCurrentDirectoryW, "SetCurrentDirectoryA", 1u << 0)
W2A(LoadLibraryW, "LoadLibraryA", 1u << 0)
W2A(GetModuleHandleW, "GetModuleHandleA", 1u << 0)
W2A(CreateMutexW, "CreateMutexA", 1u << 2)
W2A(GetPrivateProfileIntW, "GetPrivateProfileIntA", 1u << 0 | 1u << 1 | 1u << 3)
W2A(WritePrivateProfileStringW, "WritePrivateProfileStringA", 0xFu)
W2A(CreateWindowExW, "CreateWindowExA", 1u << 1 | 1u << 2)
W2A(SetWindowTextW, "SetWindowTextA", 1u << 1)
W2A(DefWindowProcW, "DefWindowProcA", 0)
W2A(PeekMessageW, "PeekMessageA", 0)
W2A(DispatchMessageW, "DispatchMessageA", 0)
W2A(SendMessageW, "SendMessageA", 0)
W2A(SetWindowLongW, "SetWindowLongA", 0)
W2A(GetWindowLongW, "GetWindowLongA", 0)
W2A(MapVirtualKeyW, "MapVirtualKeyA", 0)

static void w_GetModuleFileNameW(void) { forward_out("GetModuleFileNameA", 0, 1, A32(2)); }
static void w_GetCurrentDirectoryW(void) { forward_out("GetCurrentDirectoryA", 0, 1, A32(0)); }
static void w_GetPrivateProfileStringW(void) {
    forward_out("GetPrivateProfileStringA", 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5, 3, A32(4));
}

/* LoadLibraryExW/A(name, file, flags): the two extra arguments choose where
 * Windows looks, which means nothing here. */
static void w_LoadLibraryExA(void) {
    /* steam_api.dll is delay-loaded: the helper in the exe loads it by name
     * and asks for each function. There is no Steam here; its functions are
     * shims of this host (below), and a shim is found through any system
     * module, so the name is answered with one of those. */
    const char* name = A32(0) >= 0x10000u ? ASTR(0) : "";
    const char* base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    if (!strcasecmp(base, "steam_api.dll")) SETARG(0, hle_guest_strdup("kernel32.dll"));
    forward_fewer("LoadLibraryA", 3, 2);
}
static void w_LoadLibraryExW(void) {
    uint32_t tmp = A32(0) >= 0x10000u ? narrow_dup(A32(0)) : 0;
    if (tmp) SETARG(0, tmp);
    forward_fewer("LoadLibraryA", 3, 2);
    if (tmp) hle_guest_free(tmp);
}

/* RegisterClassW: the class structure is the narrow one with two wide
 * strings in it (menu name at +32, class name at +36). */
static void w_RegisterClassW(void) {
    uint32_t wc = A32(0), copy = hle_guest_alloc(40), menu = 0, cls = 0;
    memcpy(G2H(copy), G2H(wc), 40);
    if (MEM32(wc + 32) >= 0x10000u) MEM32(copy + 32) = menu = narrow_dup(MEM32(wc + 32));
    if (MEM32(wc + 36) >= 0x10000u) MEM32(copy + 36) = cls = narrow_dup(MEM32(wc + 36));
    SETARG(0, copy);
    tail("RegisterClassA");
    /* the class name stays allocated: the window manager keeps the pointer */
    if (menu) hle_guest_free(menu);
    hle_guest_free(copy);
    (void)cls;
}

/* ---- what the Visual C++ 2017 runtime asks on its way to WinMain --------- */

static uint32_t g_cmdline_w;
static void w_GetCommandLineW(void) {
    if (!g_cmdline_w) {
        /* the narrow shim's answer, widened once */
        uint32_t esp = g_esp;
        g_esp -= 4;
        MEM32(g_esp) = 0;
        tail("GetCommandLineA");
        g_esp = esp;
        g_cmdline_w = wide_dup((const char*)G2H(g_eax));
    }
    RET(g_cmdline_w, 0);
}

static void w_GetStartupInfoW(void) {
    uint32_t si = A32(0);
    if (si) { memset(G2H(si), 0, 68); MEM32(si) = 68; }
    RETV(1);
}

/* An empty environment block: two wide NULs. */
static void w_GetEnvironmentStringsW(void) { RET(hle_guest_alloc(4), 0); }
static void w_FreeEnvironmentStringsW(void) { hle_guest_free(A32(0)); RET(1, 1); }
static void w_SetEnvironmentVariableW(void) { RET(1, 2); }

static void w_InitializeCriticalSectionAndSpinCount(void) {
    forward_fewer("InitializeCriticalSection", 2, 1);
    g_eax = 1;
}
static void w_EncodePointer(void) { RET(A32(0), 1); }       /* DecodePointer too: no obfuscation needed */
static void w_InitializeSListHead(void) { memset(APTR(0), 0, 8); RETV(1); }
static void w_IsDebuggerPresent(void) { RET(0, 0); }
static void w_SwitchToThread(void) { hle_yield(); RET(1, 0); }
static void w_GetConsoleCP(void) { RET(0, 0); }
static void w_WriteConsoleW(void) { RET(0, 5); }
static void w_ReadConsoleW(void) { RET(0, 5); }
static void w_PeekNamedPipe(void) { RET(0, 6); }
static void w_EnumSystemLocalesW(void) { RET(0, 2); }

/* GetModuleHandleExW(flags, name or address, out): the only module the game
 * asks about this way is itself. */
static void w_GetModuleHandleExW(void) {
    uint32_t out = A32(2);
    if (out) MEM32(out) = FC_IMAGE_BASE;
    RET(1, 3);
}

/* Windows 10, as the game's "is this Windows 8 or later" checks want. */
static void w_GetVersionExW(void) {
    uint32_t v = A32(0);
    if (!v || MEM32(v) < 276) RET(0, 1);
    uint32_t size = MEM32(v);
    memset(G2H(v), 0, size);
    MEM32(v) = size;
    MEM32(v + 4) = 10, MEM32(v + 8) = 0, MEM32(v + 12) = 19045, MEM32(v + 16) = 2;   /* VER_PLATFORM_WIN32_NT */
    RET(1, 1);
}
static void w_VerSetConditionMask(void) { g_edx = A32(1); RET(A32(0) | 1u, 4); }   /* (u64 mask, type, condition) */
/* VerifyVersionInfoW(info, type mask, u64 conditions): the game's one use is
 * "is this Windows new enough for GOG Galaxy" (0x004B7128). No: there is no
 * Galaxy here, and that is the answer that leaves it alone. */
static void w_VerifyVersionInfoW(void) { RET(0, 4); }

/* SystemTimeToTzSpecificLocalTime(tz, utc, local): local time is UTC here. */
static void w_SystemTimeToTzSpecificLocalTime(void) {
    if (A32(1) && A32(2)) memcpy(APTR(2), APTR(1), 16);
    RET(1, 3);
}

/* SetFilePointerEx(h, i64 distance, out i64*, method) on SetFilePointer(h, lo, &hi, method). */
static void w_SetFilePointerEx(void) {
    uint32_t h = A32(0), lo = A32(1), hi = A32(2), out = A32(3), method = A32(4);
    uint32_t ret = MEM32(g_esp), hi_slot = hle_guest_alloc(4);
    MEM32(hi_slot) = hi;
    g_esp += 4;                                   /* five slots become four */
    MEM32(g_esp) = ret;
    SETARG(0, h), SETARG(1, lo), SETARG(2, hi_slot), SETARG(3, method);
    tail("SetFilePointer");
    int failed = g_eax == 0xFFFFFFFFu && hle_get_last_error() != 0;
    if (!failed && out) { MEM32(out) = g_eax; MEM32(out + 4) = MEM32(hi_slot); }
    hle_guest_free(hi_slot);
    g_eax = !failed;
}

/* FindFirstFileExA(name, level, data, op, filter, flags) on FindFirstFileA(name, data). */
static void w_FindFirstFileExA(void) {
    uint32_t name = A32(0), data = A32(2), ret = MEM32(g_esp);
    g_esp += 16;                                  /* six slots become two */
    MEM32(g_esp) = ret;
    SETARG(0, name), SETARG(1, data);
    tail("FindFirstFileA");
}

static void w_GetDriveTypeW(void) { RET(3, 1); }            /* DRIVE_FIXED */
static void w_OpenMutexW(void) { hle_set_last_error(ERROR_FILE_NOT_FOUND_); RET(0, 3); }   /* no other instance */
static void w_lstrcpyW(void) {
    uint32_t n = wlen(A32(1));
    memmove(APTR(0), APTR(1), 2 * (n + 1));
    RET(A32(0), 2);
}
static void w_lstrcatW(void) {
    uint32_t d = wlen(A32(0)), n = wlen(A32(1));
    memmove((uint16_t*)APTR(0) + d, APTR(1), 2 * (n + 1));
    RET(A32(0), 2);
}

/* ---- the window, the user, the shell ------------------------------------ */

static void w_MessageBoxW(void) {
    char text[512] = "", title[128] = "";
    const uint16_t* t = A32(1) ? (const uint16_t*)APTR(1) : NULL;
    const uint16_t* c = A32(2) ? (const uint16_t*)APTR(2) : NULL;
    for (int i = 0; t && t[i] && i < 510; i++) text[i] = t[i] < 128 ? (char)t[i] : '?';
    for (int i = 0; c && c[i] && i < 126; i++) title[i] = c[i] < 128 ? (char)c[i] : '?';
    fprintf(stderr, "[messagebox] %s: %s\n", title, text);
    RET(1, 4);                                    /* IDOK */
}
static void w_LoadIconW(void) { RET(0x00010003u, 2); }      /* a handle; nothing draws it */
static void w_FindWindowW(void) { RET(0, 2); }
static void w_keybd_event(void) { RETV(4); }
static void w_WINNLSGetEnableStatus(void) { RET(0, 1); }
static void w_WINNLSEnableIME(void) { RET(0, 2); }
/* SystemParametersInfoW(SPI_GETWORKAREA = 0x30, 0, RECT*, 0): the screen. */
static void w_SystemParametersInfoW(void) {
    if (A32(0) == 0x30 && A32(2)) {
        uint32_t r = A32(2);
        MEM32(r) = 0, MEM32(r + 4) = 0, MEM32(r + 8) = 1280, MEM32(r + 12) = 720;
        RET(1, 4);
    }
    RET(0, 4);
}
static void w_GetUserNameW(void) {
    uint32_t buf = A32(0), size = A32(1);
    if (!buf || !size || MEM32(size) < 7) { if (size) MEM32(size) = 7; RET(0, 2); }
    MEM32(size) = widen_into(buf, "Player", 7) + 1;
    RET(1, 2);
}
/* SHGetKnownFolderPath(folder id, flags, token, out wide string): the game
 * finds this one with GetProcAddress and asks it for one folder, Saved Games,
 * where its saves and config.ini go (FALCOM\ED6 under it). The answer is the
 * folder the host was given, as the guest spells it. */
static char g_saves[600];
void fc_set_saves_folder(const char* host_dir) { hle_guest_path(host_dir, g_saves, sizeof g_saves); }
static void w_SHGetKnownFolderPath(void) {
    if (!g_saves[0] || !A32(3)) RET(0x80004005u, 4);
    MEM32(A32(3)) = wide_dup(g_saves);          /* freed by the game with CoTaskMemFree */
    RET(0, 4);
}

/* These three are the game's fallback for that folder, and it can go without. */
static void w_SHGetSpecialFolderLocation(void) { RET(0x80004005u, 3); }   /* E_FAIL */
static void w_SHGetPathFromIDListW(void) { RET(0, 2); }
static void w_SHGetMalloc(void) { RET(0x80004005u, 1); }
static void w_CoTaskMemFree(void) { if (A32(0)) hle_guest_free(A32(0)); RETV(1); }
static void w_CoSetProxyBlanket(void) { RET(0, 8); }
static void w_GetFileVersionInfoSizeW(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(0, 2); }
static void w_GetFileVersionInfoW(void) { RET(0, 4); }
static void w_VerQueryValueW(void) { RET(0, 4); }
static void w_MiniDumpWriteDump(void) { RET(0, 7); }

/* ---- Steam: not running, as far as the game can tell ---------------------
 * cdecl, so each returns leaving its arguments to the caller. */
static void s_zero(void) { RET(0, 0); }

/* ---- not here yet: each says so, once, and refuses ------------------------ */

static void once(const char* what) {
    static const char* said[16];
    for (int i = 0; i < 16; i++) {
        if (said[i] == what) return;
        if (!said[i]) { said[i] = what; break; }
    }
    fprintf(stderr, "[portable] %s is not implemented in this host yet\n", what);
}
static void w_XInputGetState(void) { RET(ERROR_DEVICE_NOT_CONNECTED_, 2); }
static void w_DirectInput8Create(void) { once("DirectInput"); RET(0x80004005u, 5); }
static void w_Direct3DCreate9(void) { once("Direct3D 9"); RET(0, 1); }
static void w_DirectXFileCreate(void) { once("the X file reader (d3dxof)"); RET(0x80004005u, 1); }

const win32hle_shim fc_win32_wide[] = {
    { "CreateFileW", w_CreateFileW }, { "DeleteFileW", w_DeleteFileW },
    { "CreateDirectoryW", w_CreateDirectoryW }, { "GetFileAttributesW", w_GetFileAttributesW },
    { "SetCurrentDirectoryW", w_SetCurrentDirectoryW }, { "GetCurrentDirectoryW", w_GetCurrentDirectoryW },
    { "LoadLibraryW", w_LoadLibraryW }, { "LoadLibraryExW", w_LoadLibraryExW },
    { "LoadLibraryExA", w_LoadLibraryExA }, { "GetModuleHandleW", w_GetModuleHandleW },
    { "GetModuleHandleExW", w_GetModuleHandleExW }, { "GetModuleFileNameW", w_GetModuleFileNameW },
    { "CreateMutexW", w_CreateMutexW }, { "OpenMutexW", w_OpenMutexW },
    { "GetPrivateProfileIntW", w_GetPrivateProfileIntW },
    { "GetPrivateProfileStringW", w_GetPrivateProfileStringW },
    { "WritePrivateProfileStringW", w_WritePrivateProfileStringW },
    { "GetCommandLineW", w_GetCommandLineW }, { "GetStartupInfoW", w_GetStartupInfoW },
    { "GetEnvironmentStringsW", w_GetEnvironmentStringsW },
    { "FreeEnvironmentStringsW", w_FreeEnvironmentStringsW },
    { "SetEnvironmentVariableW", w_SetEnvironmentVariableW },
    { "InitializeCriticalSectionAndSpinCount", w_InitializeCriticalSectionAndSpinCount },
    { "EncodePointer", w_EncodePointer }, { "DecodePointer", w_EncodePointer },
    { "InitializeSListHead", w_InitializeSListHead }, { "IsDebuggerPresent", w_IsDebuggerPresent },
    { "SwitchToThread", w_SwitchToThread }, { "GetConsoleCP", w_GetConsoleCP },
    { "WriteConsoleW", w_WriteConsoleW }, { "ReadConsoleW", w_ReadConsoleW },
    { "PeekNamedPipe", w_PeekNamedPipe }, { "EnumSystemLocalesW", w_EnumSystemLocalesW },
    { "GetVersionExW", w_GetVersionExW }, { "VerSetConditionMask", w_VerSetConditionMask },
    { "VerifyVersionInfoW", w_VerifyVersionInfoW },
    { "SystemTimeToTzSpecificLocalTime", w_SystemTimeToTzSpecificLocalTime },
    { "SetFilePointerEx", w_SetFilePointerEx }, { "FindFirstFileExA", w_FindFirstFileExA },
    { "GetDriveTypeW", w_GetDriveTypeW }, { "lstrcpyW", w_lstrcpyW }, { "lstrcatW", w_lstrcatW },
    { "RegisterClassW", w_RegisterClassW }, { "CreateWindowExW", w_CreateWindowExW },
    { "SetWindowTextW", w_SetWindowTextW }, { "DefWindowProcW", w_DefWindowProcW },
    { "PeekMessageW", w_PeekMessageW }, { "DispatchMessageW", w_DispatchMessageW },
    { "SendMessageW", w_SendMessageW }, { "SetWindowLongW", w_SetWindowLongW },
    { "GetWindowLongW", w_GetWindowLongW }, { "MapVirtualKeyW", w_MapVirtualKeyW },
    { "MessageBoxW", w_MessageBoxW }, { "LoadIconW", w_LoadIconW }, { "FindWindowW", w_FindWindowW },
    { "keybd_event", w_keybd_event }, { "WINNLSGetEnableStatus", w_WINNLSGetEnableStatus },
    { "WINNLSEnableIME", w_WINNLSEnableIME }, { "SystemParametersInfoW", w_SystemParametersInfoW },
    { "SHGetKnownFolderPath", w_SHGetKnownFolderPath },
    { "GetUserNameW", w_GetUserNameW }, { "SHGetSpecialFolderLocation", w_SHGetSpecialFolderLocation },
    { "SHGetPathFromIDListW", w_SHGetPathFromIDListW }, { "SHGetMalloc", w_SHGetMalloc },
    { "CoTaskMemFree", w_CoTaskMemFree }, { "CoSetProxyBlanket", w_CoSetProxyBlanket },
    { "GetFileVersionInfoSizeW", w_GetFileVersionInfoSizeW }, { "GetFileVersionInfoW", w_GetFileVersionInfoW },
    { "VerQueryValueW", w_VerQueryValueW }, { "MiniDumpWriteDump", w_MiniDumpWriteDump },
    { "SteamAPI_Init", s_zero }, { "SteamAPI_Shutdown", s_zero }, { "SteamAPI_RunCallbacks", s_zero },
    { "SteamAPI_GetHSteamUser", s_zero }, { "SteamAPI_SetMiniDumpComment", s_zero },
    { "SteamAPI_WriteMiniDump", s_zero }, { "SteamInternal_FindOrCreateUserInterface", s_zero },
    { "SteamInternal_ContextInit", s_zero },
    { "XInputGetState", w_XInputGetState }, { "DirectInput8Create", w_DirectInput8Create },
    { "Direct3DCreate9", w_Direct3DCreate9 }, { "DirectXFileCreate", w_DirectXFileCreate },
    { NULL, NULL }
};
