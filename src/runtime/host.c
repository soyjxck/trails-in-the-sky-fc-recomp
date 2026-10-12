/*
 * Trails in the Sky FC - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header), shaped like sp00nznet's native32 games
 * (tiberiumwars-recomp's host.c). The lifted code is ed6_win_DX9.exe; its
 * imports, Direct3D 9, DirectSound and DirectInput are the real ones. What is
 * here is only what is specific to this game: where the image goes, the
 * guest's identity, its C++ throws, unattended test runs and the fault report.
 * docs/bringup.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 with a small /STACK (CMakeLists.txt), and started
 * through native32_hold_image, so 0x00400000..0x01955000 is free when the
 * image is mapped: the exe has no relocations and 20 MB of .data.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <d3d9.h>
#include <objbase.h>

#include "native32.h"
#include "recomp_trace.h"

extern const uint32_t fc_entry_va;    /* recomp_dispatch.c */

#define FC_IMAGE_BASE 0x00400000u
#define FC_EXE_STAMP 0x632CA5C5u      /* ed6_win_DX9.exe's PE timestamp: the build lifted (run_lift.py) */
#define FC_EXE "ed6_win_DX9.exe"
#define FC_GAME_ACTIVE 0x00557E5Fu    /* byte: the game's clock runs only while its window is the active one */

static DWORD g_watchdog_s;
static int   g_unattended;            /* window off screen, focus and pointer left alone */
static int   g_force_active;          /* keep the game's clock running in an inactive window */
static HMODULE g_resources;           /* the guest exe as a resource module */

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(nargs) (g_esp += 4 + 4 * (nargs))

/* ---- the guest's identity ------------------------------------------------
 * The guest is ed6_win_DX9.exe, not this host. Its hInstance comes from
 * GetModuleHandle(NULL), the C runtime reads its own headers there, and
 * resources and the version block come from its own file. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[2 * MAX_PATH + 64];
static wchar_t g_guest_exe_w[MAX_PATH], g_guest_cmdline_w[2 * MAX_PATH + 64];

static void shim_GetModuleHandleW(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleW((LPCWSTR)(uintptr_t)ARG(0)) : FC_IMAGE_BASE;
    RET(1);
}

static void shim_GetModuleHandleExW(void) {
    uint32_t flags = ARG(0), name = ARG(1), *out = (uint32_t*)(uintptr_t)ARG(2);
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ? native32_in_guest(name) : !name) {
        *out = FC_IMAGE_BASE;
        g_eax = 1;
    } else {
        g_eax = GetModuleHandleExW(flags, (LPCWSTR)(uintptr_t)name, (HMODULE*)out);
    }
    RET(3);
}

static uint32_t copy_out(void* out, const void* src, uint32_t n, uint32_t size, int wide) {
    if (!size) return n;
    uint32_t k = n < size ? n : size - 1;
    memcpy(out, src, (size_t)k << wide);
    if (wide) ((wchar_t*)out)[k] = 0; else ((char*)out)[k] = 0;
    return k;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0);
    if (h == 0 || h == FC_IMAGE_BASE)
        g_eax = copy_out((void*)(uintptr_t)ARG(1), g_guest_exe, (uint32_t)strlen(g_guest_exe), ARG(2), 0);
    else
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, (char*)(uintptr_t)ARG(1), ARG(2));
    RET(3);
}

static void shim_GetModuleFileNameW(void) {
    uint32_t h = ARG(0);
    if (h == 0 || h == FC_IMAGE_BASE)
        g_eax = copy_out((void*)(uintptr_t)ARG(1), g_guest_exe_w, (uint32_t)wcslen(g_guest_exe_w), ARG(2), 1);
    else
        g_eax = GetModuleFileNameW((HMODULE)(uintptr_t)h, (wchar_t*)(uintptr_t)ARG(1), ARG(2));
    RET(3);
}

static void shim_GetCommandLineA(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline; RET(0); }
static void shim_GetCommandLineW(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline_w; RET(0); }

/* The window class's icon is resource 113 of the exe (RegisterWindowClass,
 * 0x004B3B10). Windows does not know the mapped image as a module, so its
 * resources are read from the file, loaded as a resource-only module. */
static void shim_LoadIconW(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    if ((uint32_t)(uintptr_t)inst == FC_IMAGE_BASE && g_resources) inst = g_resources;
    g_eax = (uint32_t)(uintptr_t)LoadIconW(inst, (LPCWSTR)(uintptr_t)ARG(1));
    RET(2);
}

/* DirectInput8Create wants a module Windows knows. */
typedef HRESULT (WINAPI* dinput_create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static void shim_DirectInput8Create(void) {
    static dinput_create_t real;
    if (!real) {
        HMODULE m = LoadLibraryA("DINPUT8.dll");
        real = m ? (dinput_create_t)(void*)GetProcAddress(m, "DirectInput8Create") : NULL;
    }
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    if ((uint32_t)(uintptr_t)inst == FC_IMAGE_BASE) inst = GetModuleHandleW(NULL);
    g_eax = real ? (uint32_t)real(inst, ARG(1), (REFIID)(uintptr_t)ARG(2), (LPVOID*)(uintptr_t)ARG(3),
                                  (LPUNKNOWN)(uintptr_t)ARG(4))
                 : (uint32_t)E_FAIL;
    RET(5);
}

/* ---- saves and settings --------------------------------------------------
 * Every path the game builds for saves and config.ini starts from the Saved
 * Games folder, which it finds with GetProcAddress(shell32,
 * "SHGetKnownFolderPath"). --savedir DIR answers that lookup with DIR, so a
 * test run never reads or writes the player's own saves. */
static wchar_t g_savedir[MAX_PATH];
static const GUID k_saved_games = { 0x4C5C32FF, 0xBB9D, 0x43B0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };
typedef HRESULT (WINAPI* known_folder_t)(const GUID*, DWORD, HANDLE, PWSTR*);
static known_folder_t g_real_known_folder;

static HRESULT WINAPI host_SHGetKnownFolderPath(const GUID* id, DWORD flags, HANDLE token, PWSTR* out) {
    if (g_savedir[0] && !memcmp(id, &k_saved_games, sizeof *id)) {
        size_t bytes = (wcslen(g_savedir) + 1) * sizeof(wchar_t);
        *out = (PWSTR)CoTaskMemAlloc(bytes);
        if (!*out) return E_OUTOFMEMORY;
        memcpy(*out, g_savedir, bytes);
        return S_OK;
    }
    return g_real_known_folder(id, flags, token, out);
}

static void shim_GetProcAddress(void) {
    uint32_t h = ARG(0), name = ARG(1);
    void* p;
    if (h == FC_IMAGE_BASE) {
        p = (void*)(uintptr_t)native32_export(h, (const char*)(uintptr_t)name);
    } else {
        p = (void*)GetProcAddress((HMODULE)(uintptr_t)h, (LPCSTR)(uintptr_t)name);
        if (p && name >= 0x10000u && !strcmp((const char*)(uintptr_t)name, "SHGetKnownFolderPath")) {
            g_real_known_folder = (known_folder_t)p;
            p = (void*)host_SHGetKnownFolderPath;
        }
    }
    g_eax = (uint32_t)(uintptr_t)p;
    RET(2);
}

/* ---- exits and exceptions -------------------------------------------------- */

/* The game's own exits (a /GS failure, a fatal error) end in TerminateProcess:
 * say so, and flush --calltrace, whose newest entries are the ones that matter. */
static void shim_TerminateProcess(void) {
    fprintf(stderr, "\n[exit] TerminateProcess(0x%X) from sub_%08X\n", ARG(1), g_cur_func);
    native32_dump_icalls(8);
    recomp_trace_flush();
    TerminateProcess((HANDLE)(uintptr_t)ARG(0), ARG(1));
    g_eax = 1;
    RET(2);
}

static void shim_ExitProcess(void) {
    fprintf(stderr, "\n[exit] ExitProcess(%u) from sub_%08X\n", ARG(0), g_cur_func);
    recomp_trace_flush();
    ExitProcess(ARG(0));
}

/* The C runtime installs its own top-level filter (and the game a minidump
 * writer through _set_se_translator). A fault under the host is the host's
 * to report: the guest's filter would be called as native code. */
static void shim_SetUnhandledExceptionFilter(void) {
    g_eax = 0;
    RET(1);
}

/* _CxxThrowException (0x004FD597, a HOOK in run_lift.py). The C runtime is
 * linked into the exe, so a throw is this function and not an import: lifted
 * as it is, it raises on the host's SEH chain, which has never heard of the
 * guest's frames. native32's eh32 dispatches it over the guest's own. */
void native32_shim_CxxThrowException(void);
void fc_hook_004FD597(void) { native32_shim_CxxThrowException(); }

/* ---- one instance ---------------------------------------------------------
 * The game opens a named mutex and, when it exists, brings the other window
 * to the front and leaves (0x004B6CC0). An unattended run is a test beside
 * whatever else is running: it neither sees another instance nor is seen. */
static void shim_OpenMutexW(void) {
    g_eax = g_unattended ? 0 : (uint32_t)(uintptr_t)OpenMutexW(ARG(0), (BOOL)ARG(1), (LPCWSTR)(uintptr_t)ARG(2));
    if (g_unattended) SetLastError(ERROR_FILE_NOT_FOUND);
    RET(3);
}

static void shim_CreateMutexW(void) {
    g_eax = (uint32_t)(uintptr_t)CreateMutexW((LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(0), (BOOL)ARG(1),
                                              g_unattended ? NULL : (LPCWSTR)(uintptr_t)ARG(2));
    RET(3);
}

/* ---- unattended ------------------------------------------------------------
 * A test run must not disturb whoever is using the machine. Left alone the
 * game puts a window on the desktop, takes the focus and moves the real mouse
 * pointer: its menus glide the pointer to the selected item and take whatever
 * is under the pointer as the selection. With --unattended the window is
 * created off screen and never activated, the pointer the game moves and
 * reads is a private one, and Direct3D never leaves windowed mode. The game
 * still draws every frame; --shots saves them from the back buffer. */
#define OFF_SCREEN (-20000)
static POINT g_pointer = { OFF_SCREEN + 640, OFF_SCREEN + 360 };

static void shim_CreateWindowExW(void) {
    uint32_t ex = ARG(0), style = ARG(3);
    HWND parent = (HWND)(uintptr_t)ARG(8);
    int top = !parent && !(style & WS_CHILD);
    int x = (int)ARG(4), y = (int)ARG(5);
    if (g_unattended && top) {
        ex |= WS_EX_NOACTIVATE;
        x = y = OFF_SCREEN;
    }
    HWND h = CreateWindowExW(ex, (LPCWSTR)(uintptr_t)ARG(1), (LPCWSTR)(uintptr_t)ARG(2),
                             g_unattended && top ? style & ~WS_VISIBLE : style, x, y, (int)ARG(6), (int)ARG(7),
                             parent, (HMENU)(uintptr_t)ARG(9), (HINSTANCE)(uintptr_t)ARG(10),
                             (LPVOID)(uintptr_t)ARG(11));
    /* Created hidden and then shown without activation, so the focus stays where it was. */
    if (h && g_unattended && top && (style & WS_VISIBLE))
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    fprintf(stderr, "[window] CreateWindowExW(%ls) style 0x%08X %dx%d -> %p\n",
            ARG(2) ? (const wchar_t*)(uintptr_t)ARG(2) : L"", style, (int)ARG(6), (int)ARG(7), (void*)h);
    g_eax = (uint32_t)(uintptr_t)h;
    RET(12);
}

static void shim_SetWindowPos(void) {
    HWND w = (HWND)(uintptr_t)ARG(0), after = (HWND)(uintptr_t)ARG(1);
    int x = (int)ARG(2), y = (int)ARG(3);
    UINT flags = ARG(6);
    if (g_unattended && GetAncestor(w, GA_ROOT) == w) {
        after = HWND_BOTTOM;
        x = y = OFF_SCREEN;
        flags |= SWP_NOACTIVATE | SWP_NOZORDER;
    }
    g_eax = SetWindowPos(w, after, x, y, (int)ARG(4), (int)ARG(5), flags);
    fprintf(stderr, "[window] SetWindowPos(%d, %d, %dx%d, flags 0x%X) from sub_%08X\n", (int)ARG(2), (int)ARG(3),
            (int)ARG(4), (int)ARG(5), ARG(6), g_cur_func);
    RET(7);
}

static void shim_SetCursorPos(void) {
    if (g_unattended) { g_pointer.x = (int)ARG(0); g_pointer.y = (int)ARG(1); g_eax = 1; }
    else g_eax = SetCursorPos((int)ARG(0), (int)ARG(1));
    RET(2);
}

static void shim_GetCursorPos(void) {
    if (g_unattended) { *(POINT*)(uintptr_t)ARG(0) = g_pointer; g_eax = 1; }
    else g_eax = GetCursorPos((POINT*)(uintptr_t)ARG(0));
    RET(1);
}

static void shim_SetForegroundWindow(void) {
    g_eax = g_unattended ? 1 : SetForegroundWindow((HWND)(uintptr_t)ARG(0));
    RET(1);
}

static void shim_BringWindowToTop(void) {
    g_eax = g_unattended ? 1 : BringWindowToTop((HWND)(uintptr_t)ARG(0));
    RET(1);
}

static void shim_SetActiveWindow(void) {
    g_eax = g_unattended ? 0 : (uint32_t)(uintptr_t)SetActiveWindow((HWND)(uintptr_t)ARG(0));
    RET(1);
}

static void shim_SetFocus(void) {
    g_eax = g_unattended ? 0 : (uint32_t)(uintptr_t)SetFocus((HWND)(uintptr_t)ARG(0));
    RET(1);
}

/* Num Lock makes the game release the arrow keys with keybd_event, which is
 * a keystroke for the whole desktop. */
static void shim_keybd_event(void) {
    if (!g_unattended) keybd_event((BYTE)ARG(0), (BYTE)ARG(1), ARG(2), ARG(3));
    RET(4);
}

static void shim_MessageBoxW(void) {
    const wchar_t* text = ARG(1) ? (const wchar_t*)(uintptr_t)ARG(1) : L"(null)";
    const wchar_t* title = ARG(2) ? (const wchar_t*)(uintptr_t)ARG(2) : L"(null)";
    fprintf(stderr, "[messagebox] type 0x%X: %ls: %ls\n", ARG(3), title, text);
    if (g_unattended) {
        uint32_t buttons = ARG(3) & MB_TYPEMASK;
        g_eax = (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) ? IDNO : IDOK;
    } else {
        g_eax = MessageBoxW((HWND)(uintptr_t)ARG(0), text, title, ARG(3));
    }
    RET(4);
}

/* ---- Direct3D 9 -------------------------------------------------------------
 * IDirect3D9::CreateDevice (slot 16) and the device's Reset (16) and Present
 * (17) are wrapped by patching the vtables d3d9.dll shares between every
 * object it hands out, through one object made before the guest runs.
 *   - unattended: every device is windowed whatever the game asks for, so a
 *     test run can never switch the real monitors' mode;
 *   - Present counts frames (the watchdog's progress), keeps the game's clock
 *     running when asked, and saves the back buffer every --shot-every frames. */
typedef void* (WINAPI* d3d_create9_t)(UINT);
typedef HRESULT (WINAPI* d3d_createdev_t)(void*, UINT, DWORD, HWND, DWORD, D3DPRESENT_PARAMETERS*, void**);
typedef HRESULT (WINAPI* d3d_reset_t)(void*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI* d3d_present_t)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT (WINAPI* d3dx_save_t)(const char*, DWORD, void*, const void*, const RECT*);
static d3d_createdev_t g_real_createdev;
static d3d_reset_t g_real_reset;
static d3d_present_t g_real_present;
static d3dx_save_t g_d3dx_save;
static volatile LONG g_presents;      /* frames the game has presented */
static char g_shots[MAX_PATH];
static int g_shot_every = 300;

static void save_shot(IDirect3DDevice9* dev, LONG frame) {
    IDirect3DSurface9* back = NULL;
    char path[MAX_PATH + 32];
    if (!g_d3dx_save) {
        HMODULE m = LoadLibraryA("d3dx9_43.dll");
        g_d3dx_save = m ? (d3dx_save_t)(void*)GetProcAddress(m, "D3DXSaveSurfaceToFileA") : NULL;
        if (!g_d3dx_save) { fprintf(stderr, "[shots] no D3DXSaveSurfaceToFileA\n"); g_shots[0] = 0; return; }
    }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    _snprintf(path, sizeof path - 1, "%s\\frame_%06ld.png", g_shots, frame);
    HRESULT hr = g_d3dx_save(path, 3 /* D3DXIFF_PNG */, back, NULL, NULL);
    IDirect3DSurface9_Release(back);
    fprintf(stderr, "[shots] frame %ld -> %s%s\n", frame, path, FAILED(hr) ? " FAILED" : "");
}

static DWORD g_t0;                    /* GetTickCount at start, for the log's timestamps */
static HRESULT g_last_present_hr;

static HRESULT WINAPI host_Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    LONG n = InterlockedIncrement(&g_presents);
    if (n == 1) fprintf(stderr, "[d3d9] first Present\n");
    if (g_force_active) *(volatile uint8_t*)(uintptr_t)FC_GAME_ACTIVE = 1;
    if (g_shots[0] && (n == 1 || n % g_shot_every == 0)) save_shot((IDirect3DDevice9*)dev, n);
    HRESULT hr = g_real_present(dev, src, dst, wnd, dirty);
    if (hr != g_last_present_hr) {       /* a lost device shows here first */
        fprintf(stderr, "[d3d9] Present %ld at %lu ms -> 0x%08lX\n", n, GetTickCount() - g_t0, (unsigned long)hr);
        g_last_present_hr = hr;
    }
    return hr;
}

/* Whatever changes the desktop under a test run (a display mode switch, a
 * monitor added or removed) loses every Direct3D 9 device on it, and the game
 * answers a lost device it cannot reset at once by leaving. Say when it
 * happens, so that exit is not read as a fault of the lift. */
static DWORD WINAPI desktop_watch(LPVOID unused) {
    int w0 = 0, h0 = 0, n0 = 0, vw0 = 0, vh0 = 0;
    (void)unused;
    for (int first = 1;; first = 0) {
        DEVMODEA dm = { 0 };
        dm.dmSize = sizeof dm;
        EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
        int w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight, n = GetSystemMetrics(SM_CMONITORS);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (!first && (w != w0 || h != h0 || n != n0 || vw != vw0 || vh != vh0))
            fprintf(stderr, "[desktop] at %lu ms: primary %dx%d -> %dx%d, monitors %d -> %d, desktop %dx%d -> %dx%d\n",
                    GetTickCount() - g_t0, w0, h0, w, h, n0, n, vw0, vh0, vw, vh);
        w0 = w, h0 = h, n0 = n, vw0 = vw, vh0 = vh;
        Sleep(50);
    }
}

static void force_windowed(D3DPRESENT_PARAMETERS* pp, const char* what) {
    if (g_unattended && pp && !pp->Windowed) {
        fprintf(stderr, "[d3d9] %s asked for %ux%u fullscreen: windowed instead (unattended)\n", what,
                pp->BackBufferWidth, pp->BackBufferHeight);
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
}

static void patch_slot(void* obj, int slot, void* fn, void** real) {
    void** vt = *(void***)obj;
    DWORD old;
    if (vt[slot] == fn) return;
    if (!*real) *real = vt[slot];
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

/* The game resets a lost device once and, when that fails, leaves as if its
 * window had been closed (Device_CheckLost, 0x00460740). But a lost device
 * cannot be reset until Windows says so (D3DERR_DEVICENOTRESET), which after
 * a locked screen, a UAC prompt or a display mode switch is some time later:
 * the game was gone by then. The wait the original does not have is here. */
#define LOST_DEVICE_WAIT_MS (10 * 60 * 1000)

static HRESULT WINAPI host_Reset(void* dev, D3DPRESENT_PARAMETERS* pp) {
    force_windowed(pp, "Reset");
    HRESULT level = IDirect3DDevice9_TestCooperativeLevel((IDirect3DDevice9*)dev);
    if (level == D3DERR_DEVICELOST) {
        DWORD since = GetTickCount();
        fprintf(stderr, "[d3d9] the device is lost and cannot be reset yet: waiting\n");
        while (level == D3DERR_DEVICELOST && GetTickCount() - since < LOST_DEVICE_WAIT_MS) {
            Sleep(50);
            level = IDirect3DDevice9_TestCooperativeLevel((IDirect3DDevice9*)dev);
        }
    }
    HRESULT hr = g_real_reset(dev, pp);
    fprintf(stderr, "[d3d9] Reset %ux%u windowed=%d at %lu ms (cooperative level 0x%08lX) -> 0x%08lX\n",
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
            GetTickCount() - g_t0, (unsigned long)level, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI host_CreateDevice(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                        D3DPRESENT_PARAMETERS* pp, void** out) {
    force_windowed(pp, "CreateDevice");
    HRESULT hr = g_real_createdev(d3d, adapter, type, focus, flags, pp, out);
    fprintf(stderr, "[d3d9] CreateDevice %ux%u windowed=%d flags 0x%lX -> 0x%08lX\n",
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
            (unsigned long)flags, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        patch_slot(*out, 16, (void*)host_Reset, (void**)&g_real_reset);
        patch_slot(*out, 17, (void*)host_Present, (void**)&g_real_present);
    }
    return hr;
}

static int wrap_d3d9(void) {
    HMODULE m = LoadLibraryA("d3d9.dll");
    d3d_create9_t create = m ? (d3d_create9_t)(void*)GetProcAddress(m, "Direct3DCreate9") : NULL;
    void* d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { fprintf(stderr, "cannot create Direct3D 9\n"); return 1; }
    patch_slot(d3d, 16, (void*)host_CreateDevice, (void**)&g_real_createdev);
    ((ULONG (WINAPI*)(void*))(*(void***)d3d)[2])(d3d);            /* Release; the vtable stays */
    return 0;
}

static native32_shim_t g_shims[] = {
    { "GetModuleHandleW", shim_GetModuleHandleW },
    { "GetModuleHandleExW", shim_GetModuleHandleExW },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetModuleFileNameW", shim_GetModuleFileNameW },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetCommandLineW", shim_GetCommandLineW },
    { "GetProcAddress", shim_GetProcAddress },
    { "LoadIconW", shim_LoadIconW },
    { "DirectInput8Create", shim_DirectInput8Create },
    { "TerminateProcess", shim_TerminateProcess },
    { "ExitProcess", shim_ExitProcess },
    { "SetUnhandledExceptionFilter", shim_SetUnhandledExceptionFilter },
    { "OpenMutexW", shim_OpenMutexW },
    { "CreateMutexW", shim_CreateMutexW },
    { "CreateWindowExW", shim_CreateWindowExW },
    { "SetWindowPos", shim_SetWindowPos },
    { "SetCursorPos", shim_SetCursorPos },
    { "GetCursorPos", shim_GetCursorPos },
    { "SetForegroundWindow", shim_SetForegroundWindow },
    { "BringWindowToTop", shim_BringWindowToTop },
    { "SetActiveWindow", shim_SetActiveWindow },
    { "SetFocus", shim_SetFocus },
    { "keybd_event", shim_keybd_event },
    { "MessageBoxW", shim_MessageBoxW },
};

/* ---- diagnostics ---------------------------------------------------------
 * --probe VA (repeatable): report indirect calls to VA -- a virtual method or
 * callback -- with `this` and the first arguments, the first five calls.
 * RECOMP_ICALL asks this hook before the dispatch table. */
#define MAX_PROBES 8
static uint32_t g_probe[MAX_PROBES];
static int g_nprobe;
static volatile LONG g_probe_hits[MAX_PROBES];

recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_nprobe; i++)
        if (g_probe[i] == va && InterlockedIncrement(&g_probe_hits[i]) <= 5)
            fprintf(stderr, "[probe] sub_%08X from sub_%08X  ecx=%08X edx=%08X  args %08X %08X %08X %08X\n",
                    va, g_cur_func, g_ecx, g_edx, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8),
                    MEM32(g_esp + 12));
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* After native32's own handler, so callbacks are resolved first and only real
 * faults get here. Written with WriteFile from a static buffer, not stdio: a
 * fault while another thread holds stderr's lock otherwise reports nothing. */
static char g_crash_buf[16384];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static void crash_module(const char* what, const void* addr) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && GetModuleFileNameA(m, path, sizeof path))
        crash_emit("  %s %s at +0x%lX\n", what, path, (unsigned long)((uintptr_t)addr - (uintptr_t)m));
    else
        crash_emit("  %s no module (heap or generated code)\n", what);
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    crash_module("in", r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s, %ld frames presented\n", g_cur_func, g_cur_import,
               g_presents);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 40 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    crash_emit("guest stack at %08X:", g_esp);
    for (int k = 0; k < 64; k++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)(g_esp + 4 * k), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        crash_emit("%s%08X", k % 8 ? " " : "\n  ", MEM32(g_esp + 4 * k));
    }
    crash_emit("\nhost frames:\n");
    {
        void* frames[16];
        USHORT n = CaptureStackBackTrace(0, 16, frames, NULL);
        for (USHORT i = 0; i < n; i++) crash_module("  ", frames[i]);
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames presented\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, g_presents);
    native32_dump_icalls(8);
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* The lifted code runs on a thread of its own with a large stack: a lifted
 * function's frame is several times the original's, and the process's first
 * stack has to be small for the image's range to be free (native32_hold_image). */
#define GUEST_MAIN_STACK (64u << 20)
static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    native32_call_guest(fc_entry_va, 0, NULL);
    return g_eax;
}

static int dir_exists(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int main(int argc, char** argv) {
    const char* game = NULL;
    const char* savedir = NULL;
    const char* extra = "";             /* --gameargs "...": appended to the guest's command line */
    char exe_full[MAX_PATH], game_full[MAX_PATH];
    int run = 0;
    /* Unbuffered: the log is the evidence, and ExitProcess flushes nothing
     * with the C runtime linked in (/MT). */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--unattended")) g_unattended = 1;
        else if (!strcmp(argv[i], "--active")) g_force_active = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--savedir") && i + 1 < argc) savedir = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_shots, NULL);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gameargs") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: trails [--run] [--game DIR] [--savedir DIR] [--unattended] [--active]\n"
                   "              [--shots DIR] [--shot-every N] [--watchdog S] [--gameargs \"...\"]\n"
                   "              [--probe VA] [--native-trace] [--callbacks]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    if (g_shot_every < 1) g_shot_every = 1;
    /* The game folder: --game, or "Trails in the Sky FC" here or beside the build folder. */
    if (!game) {
        static char beside[MAX_PATH];
        GetModuleFileNameA(NULL, beside, MAX_PATH);
        char* cut = strrchr(beside, '\\');
        if (cut) *cut = 0;
        cut = strrchr(beside, '\\');
        if (cut) strcpy_s(cut + 1, MAX_PATH - (cut + 1 - beside), "Trails in the Sky FC");
        game = dir_exists("Trails in the Sky FC") ? "Trails in the Sky FC" : beside;
    }
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    _snprintf(exe_full, sizeof exe_full - 1, "%s\\" FC_EXE, game_full);
    exe_full[sizeof exe_full - 1] = 0;

    {   /* The lifted C is one build's: another crashes in game. */
        uint8_t head[4096] = { 0 };
        FILE* f = fopen(exe_full, "rb");
        if (!f) { fprintf(stderr, "no %s: pass --game <the game's folder>\n", exe_full); return 1; }
        fread(head, 1, sizeof head, f);
        fclose(f);
        uint32_t nt = *(const uint32_t*)(head + 0x3C);
        uint32_t stamp = nt < sizeof head - 12 ? *(const uint32_t*)(head + nt + 8) : 0;
        if (stamp != FC_EXE_STAMP) {
            fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, "
                    "wanted 0x%08X): the Steam release of 2022-09-22.\n", exe_full, stamp, FC_EXE_STAMP);
            return 1;
        }
    }
    /* From here on this is the second process, with the image's range held
     * for it; the first one waits and exits with its exit code. */
    if (!native32_hold_image(exe_full, FC_IMAGE_BASE))
        fprintf(stderr, "warning: the image's range was not held; mapping may fail\n");

    strcpy_s(g_guest_exe, sizeof g_guest_exe, exe_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s%s", g_guest_exe, *extra ? " " : "", extra);
    MultiByteToWideChar(CP_ACP, 0, g_guest_exe, -1, g_guest_exe_w, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, g_guest_cmdline, -1, g_guest_cmdline_w,
                        sizeof g_guest_cmdline_w / sizeof g_guest_cmdline_w[0]);
    if (savedir) {
        char full[MAX_PATH];
        GetFullPathNameA(savedir, MAX_PATH, full, NULL);
        CreateDirectoryA(full, NULL);
        MultiByteToWideChar(CP_ACP, 0, full, -1, g_savedir, MAX_PATH);
    }
    if (g_shots[0]) CreateDirectoryA(g_shots, NULL);
    /* steam_api.dll, Galaxy.dll and xinput1_3.dll sit beside the game exe. */
    SetDllDirectoryA(game_full);

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    /* The C runtime in the exe picks its string and math routines by CPUID.
     * Leaf 1's ecx is everything after SSE2 (SSE3 to AVX, and OSXSAVE): with
     * it clear the runtime stays on its SSE2 paths, which are what lift32
     * covers. edx (SSE, SSE2, CMOV, MMX) stays as the CPU reports it. */
    g_cpuid_ecx1 = 0;
    printf("Trails in the Sky FC recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    if (wrap_d3d9()) return 1;
    uint32_t span = native32_map(exe_full, FC_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, FC_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", FC_EXE, FC_IMAGE_BASE, FC_IMAGE_BASE + span);
    g_resources = LoadLibraryExA(exe_full, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (native32_bind(FC_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);
    if (savedir) printf("  Saved Games folder %ls\n", g_savedir);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", fc_entry_va);
        return 0;
    }
    /* The archives (ED6_DT*.dat) are opened relative to the game folder. */
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    g_t0 = GetTickCount();
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, desktop_watch, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", fc_entry_va);
    fflush(stdout);
    HANDLE t = CreateThread(NULL, GUEST_MAIN_STACK, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { fprintf(stderr, "cannot start the guest thread (error %lu)\n", GetLastError()); return 1; }
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    printf("\nentry returned eax=%08lX, %ld frames presented\n", (unsigned long)code, g_presents);
    return (int)code;
}
