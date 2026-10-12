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
#include <d3d9on12.h>
#include <objbase.h>
#include <psapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <shlobj.h>

#include "native32.h"
#include "recomp_trace.h"
#include "profile.h"
#include "mods.h"
#include "settings_menu.h"

extern const uint32_t fc_entry_va;    /* recomp_dispatch.c */

#define FC_IMAGE_BASE 0x00400000u
#define FC_EXE_STAMP 0x632CA5C5u      /* ed6_win_DX9.exe's PE timestamp: the build lifted (run_lift.py) */
#define FC_EXE "ed6_win_DX9.exe"
#define FC_GAME_ACTIVE 0x00557E5Fu    /* byte: the game's clock runs only while its window is the active one */

static DWORD g_watchdog_s;
static int   g_unattended;            /* window off screen, focus and pointer left alone */
static int   g_force_active;          /* keep the game's clock running in an inactive window */
static int   g_silent;                /* this process's audio session is muted */
static HMODULE g_resources;           /* the guest exe as a resource module */
static DWORD g_t0;                    /* GetTickCount at start, for the log's timestamps */
static volatile LONG g_presents;      /* frames the game has presented */
static char g_shots[MAX_PATH];        /* --shots: where frames are saved */
static HWND g_window;                 /* the game's top-level window */
static HANDLE g_guest_thread;         /* the thread the game's main function runs on */
static int   g_input_rest;            /* just out of the settings menu: no button counts until all are let go */
static void pace_report(void);
static void frame_times_mark(const char* text);
static uint64_t thread_cycles(HANDLE thread);
static uint64_t g_pace_cycles0, g_pace_tsc0;

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

static int g_count_top = 60;      /* --count-natives N: how many of them to list */

/* MoviePlay, wrapped for the portable host's sake: here it is the game's own,
 * on DirectShow. */
void fc_orig_004886D0(void);
void fc_wrap_004886D0(void) { fc_orig_004886D0(); }

/* Voices (PATCHES in run_lift.py) are src/mods/voice.c's, on both hosts. What
 * it needs of this one is in voice_out.c, which asks for the game's window. */
HWND fc_host_window(void) { return g_window; }

static void shim_ExitProcess(void) {
    fprintf(stderr, "\n[exit] ExitProcess(%u) from sub_%08X\n", ARG(0), g_cur_func);
    recomp_trace_flush();
    profile_report();
    pace_report();
    if (native32_count_natives) native32_dump_counts(g_count_top);
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
    if (h && top) g_window = h, fc_menu_attach(h);
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

/* ---- play scripts -----------------------------------------------------------
 * --script FILE plays a list of button presses, for hands-off test runs: the
 * title menu, a new game, loading a save. Input_Poll (0x004802C0) returns the
 * buttons held in edx:eax; the lift wraps it (WRAPS in run_lift.py), and
 * while a script plays it is the only source of input.
 *
 *   wait <ms>
 *   hold <button> <ms>
 *   tap <button> [count] [interval ms]     press for 100 ms, `count` times
 *   shot <name>                            save the next frame as <name>.png (--shots)
 *   log <text>
 *   quit                                   close the game's window
 *
 * Lines starting with # are comments; a button is a name below or a hex mask.
 * Waits and long holds are timed at the game's pace, not the wall's: a poll
 * moves that time on by the time since the poll before, but by no more than
 * 34 ms, which is as far as the game's own clock goes in a frame. So a script
 * keeps its place while the game loads something or the machine is busy. A
 * press of a quarter of a second or less is timed by the wall clock all the
 * same: stretched over a load it would be held long enough to repeat. */
#define FC_MOUSE_IN_USE 0x00557E5Eu   /* byte: set while the mouse drives the input */
#define FC_MOUSE_MODE   0x006B0018u   /* byte: mouse mode */

static const struct { const char* name; uint32_t bit; } k_buttons[] = {
    { "UP", 0x1 }, { "DOWN", 0x2 }, { "RIGHT", 0x4 }, { "LEFT", 0x8 }, { "ESC", 0x10 }, { "SPACE", 0x20 },
    { "ENTER", 0x40 }, { "CTRL", 0x80 }, { "NUM0", 0x100 }, { "SHIFT", 0x200 }, { "Z", 0x400 }, { "X", 0x800 },
    { "PGUP", 0x1000 }, { "PGDN", 0x2000 }, { "B", 0x8000 }, { "C", 0x10000 }, { "M", 0x20000 },
    { "A", 0x400000 }, { "V", 0x800000 },
};
enum { STEP_WAIT, STEP_HOLD, STEP_SHOT, STEP_LOG, STEP_QUIT };
typedef struct { int kind; uint32_t ms, buttons; char text[128]; } step_t;
#define LONGEST_TAP_MS 250u

static step_t* g_steps;
static int g_nsteps, g_step, g_step_entered;
static DWORD g_step_started, g_step_started_wall;
static char g_shot_pending[64];       /* a script's `shot`, taken at the next Present */

static uint32_t button_mask(const char* word) {
    for (int i = 0; i < (int)(sizeof k_buttons / sizeof k_buttons[0]); i++)
        if (!_stricmp(word, k_buttons[i].name)) return k_buttons[i].bit;
    return strtoul(word, NULL, 16);
}

static void add_step(int kind, uint32_t ms, uint32_t buttons, const char* text) {
    g_steps = (step_t*)realloc(g_steps, (g_nsteps + 1) * sizeof *g_steps);
    step_t* s = &g_steps[g_nsteps++];
    s->kind = kind, s->ms = ms, s->buttons = buttons;
    strncpy(s->text, text, sizeof s->text - 1);
    s->text[sizeof s->text - 1] = 0;
}

static int load_script(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    if (!f) { fprintf(stderr, "[script] cannot open %s\n", path); return 1; }
    while (fgets(line, sizeof line, f)) {
        char verb[16] = "", a[64] = "", rest[128] = "";
        unsigned n1 = 0, n2 = 0;
        if (sscanf(line, "%15s", verb) != 1 || verb[0] == '#') continue;
        if (!_stricmp(verb, "wait") && sscanf(line, "%*s %u", &n1) == 1) {
            add_step(STEP_WAIT, n1, 0, "");
        } else if (!_stricmp(verb, "hold") && sscanf(line, "%*s %63s %u", a, &n1) == 2) {
            add_step(STEP_HOLD, n1, button_mask(a), "");
        } else if (!_stricmp(verb, "tap") && sscanf(line, "%*s %63s %u %u", a, &n1, &n2) >= 1) {
            unsigned count = n1 ? n1 : 1, interval = n2 > 100 ? n2 : 500;
            for (unsigned i = 0; i < count; i++) {
                add_step(STEP_HOLD, 100, button_mask(a), "");
                add_step(STEP_WAIT, interval - 100, 0, "");
            }
        } else if (!_stricmp(verb, "shot") && sscanf(line, "%*s %47s", a) == 1) {
            add_step(STEP_SHOT, 0, 0, a);
        } else if (!_stricmp(verb, "log") && sscanf(line, "%*s %127[^\r\n]", rest) == 1) {
            add_step(STEP_LOG, 0, 0, rest);
        } else if (!_stricmp(verb, "quit")) {
            add_step(STEP_QUIT, 0, 0, "");
        } else {
            fprintf(stderr, "[script] cannot read line: %s", line);
        }
    }
    fclose(f);
    fprintf(stderr, "[script] %d steps from %s\n", g_nsteps, path);
    return 0;
}

/* The game is asked to leave the way a player would, by closing its window:
 * its own throw, catch and exit run. If it has not left soon after, say so. */
static DWORD WINAPI quit_timeout(LPVOID unused) {
    (void)unused;
    Sleep(20000);
    fprintf(stderr, "\n[script] the game is still running 20 s after its window was closed: in sub_%08X, last native call %s\n",
            g_cur_func, g_cur_import);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 5);
    return 0;
}

/* The buttons the script holds right now. */
static uint32_t scripted_buttons(void) {
    static DWORD last_poll, script_ms;
    DWORD wall = GetTickCount();
    if (last_poll) script_ms += wall - last_poll < 34 ? wall - last_poll : 34;
    last_poll = wall;
    while (g_step < g_nsteps) {
        const step_t* s = &g_steps[g_step];
        if (!g_step_entered) {
            g_step_entered = 1;
            g_step_started = script_ms;
            g_step_started_wall = wall;
            if (s->kind == STEP_SHOT && g_shots[0]) strcpy_s(g_shot_pending, sizeof g_shot_pending, s->text);
            if (s->kind == STEP_LOG) fprintf(stderr, "[script] %s\n", s->text), frame_times_mark(s->text);
            if (s->kind == STEP_QUIT) {
                fprintf(stderr, "[script] quit at %lu ms, %ld frames presented\n", GetTickCount() - g_t0, g_presents);
                if (g_window) PostMessageW(g_window, WM_CLOSE, 0, 0);
                CloseHandle(CreateThread(NULL, 0, quit_timeout, NULL, 0, NULL));
            }
        }
        int tap = s->kind == STEP_HOLD && s->ms <= LONGEST_TAP_MS;
        DWORD elapsed = tap ? wall - g_step_started_wall : script_ms - g_step_started;
        /* A picture is of a finished frame: hold the script until it is taken. */
        if (s->kind == STEP_SHOT && g_shot_pending[0]) return 0;
        if (elapsed < s->ms) return s->kind == STEP_HOLD ? s->buttons : 0;
        g_step++;
        g_step_entered = 0;
    }
    return 0;
}

/* Input_Poll, wrapped. The poll itself still runs, because the pointer
 * position it records must be kept up: the menus move the pointer to the
 * selected item themselves and take the item under it as the selection. */
void fc_orig_004802C0(void);
void fc_wrap_004802C0(void) {
    fc_orig_004802C0();
    if (g_input_rest) {                 /* the key that closed the settings menu is not a key for the game */
        if (g_eax | g_edx) g_eax = g_edx = 0;
        else g_input_rest = 0;
    }
    if (g_step < g_nsteps) {
        MEM8(FC_MOUSE_IN_USE) = 0;
        MEM8(FC_MOUSE_MODE) = 0;
        g_eax = scripted_buttons();
        g_edx = 0;
    }
}

/* ---- the frame limiter ---------------------------------------------------------
 * Clock_Update (0x004C11A0; thiscall, two byte arguments: wait, notify) is
 * where the game holds a frame back: with `wait` set it loops
 *
 *     read the counter; if more than 7 ms have passed, go on;
 *     PumpMessages(); Sleep(0);
 *
 * so no frame is shorter than 8 ms (125 a second), and every frame spins on
 * Sleep(0) for whatever is left of its 8 ms: thousands of turns, each three
 * calls out of the lifted code, and one CPU core kept at full load to wait.
 *
 * The host waits first, asleep, until the moment that loop would let the
 * The host does the waiting instead, asleep: a high-resolution timer for all
 * but the last millisecond and a half, then a short spin, and the game's loop
 * is told not to wait (its `wait` argument cleared). --fps N sets the rate
 * (30, 60, 120, 144...; 0 = no limit; default 125, the game's own ceiling;
 * above that the game runs on shorter time steps than it was made for).
 * The message pump the loop ran still runs while waiting.
 * --original-limiter leaves the game's loop as it was. */
#define FC_CLOCK_LAST      0x007C8BB8u   /* i64: counter value at the last update */
#define FC_CLOCK_LEFTOVER  0x007C8BC0u   /* i64: ticks left over after whole milliseconds */
#define FC_CLOCK_PER_MS    0x007C8BD0u   /* i64: counter ticks per millisecond */
#define FC_CLOCK_COUNTER   0x007C8B4Du   /* byte: a performance counter is in use */
#define FC_ORIGINAL_FPS 125

static int g_fps_limit = FC_ORIGINAL_FPS;
static HANDLE g_frame_timer;
static LARGE_INTEGER g_qpc_freq;

void sub_004B39E0(void);              /* PumpMessages, lifted */
void fc_orig_004C11A0(void);

static void frame_wait(int64_t target) {
    /* How long before the moment to stop sleeping and spin instead: as long as
     * the timer has lately woken late, and a little more. A good timer earns
     * a short spin (a quarter of a millisecond), a poor one up to two. */
    static double margin = 1.0;
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t left = target - now.QuadPart;
        if (left <= 0) return;
        double ms = (double)left * 1000.0 / (double)g_qpc_freq.QuadPart;
        if (ms <= margin + 0.1) {     /* the last stretch: no timer is this exact */
            YieldProcessor();
            continue;
        }
        double slice = ms - margin > 4.0 ? 4.0 : ms - margin;
        LARGE_INTEGER due, woke;
        const char* was = g_cur_import;
        due.QuadPart = -(LONGLONG)(slice * 10000.0);
        g_cur_import = "(frame limiter) WaitForSingleObject";
        mach_leave();                 /* asleep: the music's thread can have the machine */
        if (g_frame_timer && SetWaitableTimer(g_frame_timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(g_frame_timer, 50);
        else
            Sleep(1);
        QueryPerformanceCounter(&woke);
        mach_enter();
        g_cur_import = was;
        {
            double late = (double)(woke.QuadPart - now.QuadPart) * 1000.0 / (double)g_qpc_freq.QuadPart - slice;
            double want = late < 0 ? 0.25 : late * 2.0 + 0.25;
            margin = want > margin ? want : margin * 0.98 + want * 0.02;   /* up at once, down slowly */
            if (margin > 2.0) margin = 2.0;
        }
        if (ms - slice > 3.0) {       /* the game's loop pumped messages while it waited; not in the last stretch */
            uint32_t ecx = g_ecx, eax = g_eax, edx = g_edx;
            PUSH32(g_esp, RECOMP_RETADDR);
            sub_004B39E0();
            g_ecx = ecx, g_eax = eax, g_edx = edx;
        }
    }
}

/* Frames are let go on a schedule, each one period after the moment the last
 * was DUE rather than after the moment it actually went: waking up a little
 * late then costs that frame a little, not every frame after it, and the rate
 * comes out as asked. A frame that overruns by more than a period (a load)
 * starts the schedule again from now. */
void fc_wrap_004C11A0(void) {
    static int64_t due;
    if (MEM8(g_esp + 4) && MEM8(FC_CLOCK_COUNTER) && g_fps_limit >= 0) {
        if (g_fps_limit > 0) {
            int64_t period = g_qpc_freq.QuadPart / g_fps_limit;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (!due) due = *(const int64_t*)(uintptr_t)FC_CLOCK_LAST;
            due += period;
            if (due < now.QuadPart - period) due = now.QuadPart;
            frame_wait(due);
        }
        MEM8(g_esp + 4) = 0;          /* the wait is done: the game's own loop has nothing to add */
    }
    fc_orig_004C11A0();
}

int fc_host_fps(void) { return g_fps_limit; }
void fc_host_set_fps(int fps) { g_fps_limit = fps; }

/* ---- D3DX vector maths, called directly --------------------------------------
 * Half of a frame's calls out of the lifted code are these few functions
 * (D3DXMatrixMultiply alone about 4,600 times). They are the real D3DX
 * functions still, so every result is bit for bit what it was; only the way
 * there is shorter than the general bridge, which copies 24 argument slots
 * and watches for exceptions and x87 results it cannot have here. */
typedef void* (WINAPI* d3dx_pp_t)(void*, const void*);
typedef void* (WINAPI* d3dx_ppp_t)(void*, const void*, const void*);
typedef void* (WINAPI* d3dx_pfff_t)(void*, float, float, float);
static d3dx_ppp_t g_d3dx_multiply, g_d3dx_transform_coord;
static d3dx_pp_t g_d3dx_transpose, g_d3dx_normalize;
static d3dx_pfff_t g_d3dx_translation, g_d3dx_scaling, g_d3dx_yaw_pitch_roll;

#define PARG(n) ((void*)(uintptr_t)ARG(n))
#define FARG(n) (*(const float*)(uintptr_t)(g_esp + 4 + 4 * (n)))
static void shim_D3DXMatrixMultiply(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_multiply(PARG(0), PARG(1), PARG(2)); RET(3); }
static void shim_D3DXVec3TransformCoord(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_transform_coord(PARG(0), PARG(1), PARG(2)); RET(3); }
static void shim_D3DXMatrixTranspose(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_transpose(PARG(0), PARG(1)); RET(2); }
static void shim_D3DXVec3Normalize(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_normalize(PARG(0), PARG(1)); RET(2); }
static void shim_D3DXMatrixTranslation(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_translation(PARG(0), FARG(1), FARG(2), FARG(3)); RET(4); }
static void shim_D3DXMatrixScaling(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_scaling(PARG(0), FARG(1), FARG(2), FARG(3)); RET(4); }
static void shim_D3DXMatrixRotationYawPitchRoll(void) { g_eax = (uint32_t)(uintptr_t)g_d3dx_yaw_pitch_roll(PARG(0), FARG(1), FARG(2), FARG(3)); RET(4); }

static int d3dx_direct(void) {
    HMODULE m = LoadLibraryA("d3dx9_43.dll");
    if (!m) return 0;
    g_d3dx_multiply = (d3dx_ppp_t)(void*)GetProcAddress(m, "D3DXMatrixMultiply");
    g_d3dx_transform_coord = (d3dx_ppp_t)(void*)GetProcAddress(m, "D3DXVec3TransformCoord");
    g_d3dx_transpose = (d3dx_pp_t)(void*)GetProcAddress(m, "D3DXMatrixTranspose");
    g_d3dx_normalize = (d3dx_pp_t)(void*)GetProcAddress(m, "D3DXVec3Normalize");
    g_d3dx_translation = (d3dx_pfff_t)(void*)GetProcAddress(m, "D3DXMatrixTranslation");
    g_d3dx_scaling = (d3dx_pfff_t)(void*)GetProcAddress(m, "D3DXMatrixScaling");
    g_d3dx_yaw_pitch_roll = (d3dx_pfff_t)(void*)GetProcAddress(m, "D3DXMatrixRotationYawPitchRoll");
    return g_d3dx_multiply && g_d3dx_transform_coord && g_d3dx_transpose && g_d3dx_normalize &&
           g_d3dx_translation && g_d3dx_scaling && g_d3dx_yaw_pitch_roll;
}

/* ---- what the game asks of D3DX (--d3dx-dump DIR) ------------------------------
 * A host without Windows has to answer these itself. With a folder given,
 * each shader's source, what D3DX assembled from it and the listing D3DX
 * made of that are written there, and the log says what every picture, copy
 * between surfaces and mesh the game asks for looks like (each kind once). */
static const char* g_d3dx_dump;
static void* d3dx_fn(const char* name) {
    static HMODULE m;
    if (!m) m = LoadLibraryA("d3dx9_43.dll");
    return m ? (void*)GetProcAddress(m, name) : NULL;
}
static void d3dx_dump_file(const char* kind, int n, const char* ext, const void* data, size_t len) {
    char path[MAX_PATH];
    if (!g_d3dx_dump || !data) return;
    _snprintf(path, sizeof path - 1, "%s\\%s_%02d.%s", g_d3dx_dump, kind, n, ext);
    path[sizeof path - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}
/* ID3DXBuffer: GetBufferPointer is its fourth method, GetBufferSize its fifth */
static void* d3dx_buf_ptr(void* b) { return ((void* (WINAPI*)(void*))(*(void***)b)[3])(b); }
static DWORD d3dx_buf_len(void* b) { return ((DWORD (WINAPI*)(void*))(*(void***)b)[4])(b); }
/* 1 the first time a line is seen (up to 256 different lines) */
static int d3dx_new_kind(const char* line) {
    static char* seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], line)) return 0;
    if (n < 256) seen[n++] = _strdup(line);
    return 1;
}

static void shim_D3DXAssembleShader(void) {      /* (source, length, defines, include, flags, &shader, &errors) */
    typedef HRESULT (WINAPI* fn_t)(const char*, UINT, const void*, void*, DWORD, void**, void**);
    static int n;
    HRESULT hr = ((fn_t)d3dx_fn("D3DXAssembleShader"))((const char*)PARG(0), ARG(1), PARG(2), PARG(3), ARG(4),
                                                        (void**)PARG(5), (void**)PARG(6));
    if (g_d3dx_dump) {
        void** out = (void**)PARG(5);
        d3dx_dump_file("shader", n, "txt", PARG(0), ARG(1));
        if (SUCCEEDED(hr) && out && *out) d3dx_dump_file("shader", n, "bin", d3dx_buf_ptr(*out), d3dx_buf_len(*out));
        fprintf(stderr, "[d3dx] AssembleShader #%d: %u bytes, flags 0x%X, defines %p, include %p -> 0x%08lX\n",
                n, ARG(1), ARG(4), PARG(2), PARG(3), (unsigned long)hr);
        n++;
    }
    g_eax = (uint32_t)hr;
    RET(7);
}

static void shim_D3DXDisassembleShader(void) {   /* (tokens, colour, comments, &text) */
    typedef HRESULT (WINAPI* fn_t)(const DWORD*, BOOL, const char*, void**);
    static int n;
    HRESULT hr = ((fn_t)d3dx_fn("D3DXDisassembleShader"))((const DWORD*)PARG(0), (BOOL)ARG(1), (const char*)PARG(2),
                                                           (void**)PARG(3));
    if (g_d3dx_dump) {
        void** out = (void**)PARG(3);
        if (SUCCEEDED(hr) && out && *out) d3dx_dump_file("listing", n, "txt", d3dx_buf_ptr(*out), d3dx_buf_len(*out));
        fprintf(stderr, "[d3dx] DisassembleShader #%d: colour %u, comments %p -> 0x%08lX\n", n, ARG(1), PARG(2),
                (unsigned long)hr);
        n++;
    }
    g_eax = (uint32_t)hr;
    RET(4);
}

/* (device, data, size, width, height, mip levels, usage, format, pool, filter, mip filter, colour key, &info,
 * palette, &texture) */
static void shim_D3DXCreateTextureFromFileInMemoryEx(void) {
    typedef HRESULT (WINAPI* fn_t)(void*, const void*, UINT, UINT, UINT, UINT, DWORD, DWORD, DWORD, DWORD, DWORD,
                                   DWORD, void*, void*, void**);
    HRESULT hr = ((fn_t)d3dx_fn("D3DXCreateTextureFromFileInMemoryEx"))(
        PARG(0), PARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10), ARG(11),
        PARG(12), PARG(13), (void**)PARG(14));
    if (g_d3dx_dump) {
        const unsigned char* d = (const unsigned char*)PARG(1);
        IDirect3DTexture9** out = (IDirect3DTexture9**)PARG(14);
        D3DSURFACE_DESC got = { 0 };
        DWORD levels = 0;
        char line[400];
        if (SUCCEEDED(hr) && out && *out) {
            IDirect3DTexture9_GetLevelDesc(*out, 0, &got);
            levels = IDirect3DTexture9_GetLevelCount(*out);
        }
        _snprintf(line, sizeof line - 1,
                  "[d3dx] CreateTextureFromFileInMemoryEx: file '%.4s' (header %02X%02X%02X%02X ... %02X%02X%02X%02X at 84), "
                  "asked %dx%d, %d levels, usage 0x%X, format %u, pool %u, filter 0x%X, mip filter 0x%X, key 0x%08X, "
                  "info %s, palette %s -> 0x%08lX: %ux%u, %lu levels, format %u",
                  (const char*)d, d[0], d[1], d[2], d[3], ARG(2) > 88 ? d[84] : 0, ARG(2) > 88 ? d[85] : 0,
                  ARG(2) > 88 ? d[86] : 0, ARG(2) > 88 ? d[87] : 0, (int)ARG(3), (int)ARG(4), (int)ARG(5), ARG(6),
                  ARG(7), ARG(8), ARG(9), ARG(10), ARG(11), PARG(12) ? "yes" : "no", PARG(13) ? "yes" : "no",
                  (unsigned long)hr, got.Width, got.Height, (unsigned long)levels, (unsigned)got.Format);
        line[sizeof line - 1] = 0;
        if (d3dx_new_kind(line)) fprintf(stderr, "%s\n", line);
    }
    g_eax = (uint32_t)hr;
    RET(15);
}

static int d3dx_rect(char* out, size_t n, const RECT* r) {
    if (!r) return _snprintf(out, n, "all");
    return _snprintf(out, n, "%ldx%ld", r->right - r->left, r->bottom - r->top);
}

/* (to, its palette, its rectangle, from, its palette, its rectangle, filter, colour key) */
static void shim_D3DXLoadSurfaceFromSurface(void) {
    typedef HRESULT (WINAPI* fn_t)(void*, const void*, const RECT*, void*, const void*, const RECT*, DWORD, DWORD);
    HRESULT hr = ((fn_t)d3dx_fn("D3DXLoadSurfaceFromSurface"))(PARG(0), PARG(1), (const RECT*)PARG(2), PARG(3),
                                                                PARG(4), (const RECT*)PARG(5), ARG(6), ARG(7));
    if (g_d3dx_dump) {
        D3DSURFACE_DESC to = { 0 }, from = { 0 };
        char line[400], r1[40], r2[40];
        if (PARG(0)) IDirect3DSurface9_GetDesc((IDirect3DSurface9*)PARG(0), &to);
        if (PARG(3)) IDirect3DSurface9_GetDesc((IDirect3DSurface9*)PARG(3), &from);
        d3dx_rect(r1, sizeof r1, (const RECT*)PARG(2));
        d3dx_rect(r2, sizeof r2, (const RECT*)PARG(5));
        _snprintf(line, sizeof line - 1,
                  "[d3dx] LoadSurfaceFromSurface: to %ux%u format %u usage 0x%lX pool %u (rect %s) from %ux%u format %u "
                  "usage 0x%lX pool %u (rect %s), filter 0x%X, key 0x%08X, palettes %s -> 0x%08lX",
                  to.Width, to.Height, (unsigned)to.Format, (unsigned long)to.Usage, (unsigned)to.Pool, r1, from.Width,
                  from.Height, (unsigned)from.Format, (unsigned long)from.Usage, (unsigned)from.Pool, r2, ARG(6),
                  ARG(7), PARG(1) || PARG(4) ? "yes" : "no", (unsigned long)hr);
        line[sizeof line - 1] = 0;
        if (d3dx_new_kind(line)) fprintf(stderr, "%s\n", line);
    }
    g_eax = (uint32_t)hr;
    RET(8);
}

/* (file, options, device, &adjacency, &materials, &effects, &how many materials, &mesh) */
static void shim_D3DXLoadMeshFromXA(void) {
    typedef HRESULT (WINAPI* fn_t)(const char*, DWORD, void*, void**, void**, void**, DWORD*, void**);
    HRESULT hr = ((fn_t)d3dx_fn("D3DXLoadMeshFromXA"))((const char*)PARG(0), ARG(1), PARG(2), (void**)PARG(3),
                                                        (void**)PARG(4), (void**)PARG(5), (DWORD*)PARG(6),
                                                        (void**)PARG(7));
    if (g_d3dx_dump)
        fprintf(stderr, "[d3dx] LoadMeshFromXA '%s', options 0x%X -> 0x%08lX\n", (const char*)PARG(0), ARG(1),
                (unsigned long)hr);
    g_eax = (uint32_t)hr;
    RET(8);
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
static int g_shot_every = 300;

static void save_shot(IDirect3DDevice9* dev, LONG frame, const char* name) {
    IDirect3DSurface9* back = NULL;
    char path[MAX_PATH + 80];
    if (!g_d3dx_save) {
        HMODULE m = LoadLibraryA("d3dx9_43.dll");
        g_d3dx_save = m ? (d3dx_save_t)(void*)GetProcAddress(m, "D3DXSaveSurfaceToFileA") : NULL;
        if (!g_d3dx_save) { fprintf(stderr, "[shots] no D3DXSaveSurfaceToFileA\n"); g_shots[0] = 0; return; }
    }
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    if (name) _snprintf(path, sizeof path - 1, "%s\\%s.png", g_shots, name);
    else _snprintf(path, sizeof path - 1, "%s\\frame_%06ld.png", g_shots, frame);
    path[sizeof path - 1] = 0;
    HRESULT hr = g_d3dx_save(path, 3 /* D3DXIFF_PNG */, back, NULL, NULL);
    IDirect3DSurface9_Release(back);
    fprintf(stderr, "[shots] frame %ld -> %s%s\n", frame, path, FAILED(hr) ? " FAILED" : "");
}

void fc_host_shot(void* dev, const char* name) {
    if (g_shots[0]) save_shot((IDirect3DDevice9*)dev, 0, name);
}

static HRESULT g_last_present_hr;

/* How even the frames are, for the log: the time from each Present to the
 * next, as an average and the share of frames that took much longer. */
static LARGE_INTEGER g_pace_last, g_pace_first;
static double g_pace_sum_ms, g_pace_worst_ms;
static LONG g_pace_n, g_pace_late;

/* --frame-times FILE: every frame, for a closer look than that summary. A
 * line is when the frame was presented (ms after the first), how long after
 * the one before (ms), and how much of that the main thread was running (ms,
 * by its cycle count). A play script's `log` lines are in it too, each
 * before the first frame after it, as `# mark: <text>`: what part of a run a
 * stretch of frames belongs to. Kept in memory and written as the game leaves. */
static char g_frame_times[MAX_PATH];
static struct { LONG frame; char text[64]; } g_ft_mark[64];
static int g_ft_marks;
typedef struct { double at_ms; float ms; uint32_t cycles; } frame_time;
static frame_time* g_ft;
static LONG g_ft_n, g_ft_room;
static uint64_t g_ft_cycles, g_ft_tsc0, g_ft_tsc;
static double g_ft_at;

static void frame_times_note(double ms) {
    if (!g_frame_times[0]) return;
    if (g_ft_n == g_ft_room) {
        frame_time* more = realloc(g_ft, (size_t)(g_ft_room = g_ft_room ? g_ft_room * 2 : 1 << 16) * sizeof *g_ft);
        if (!more) { g_frame_times[0] = 0; return; }
        g_ft = more;
    }
    uint64_t cycles = thread_cycles(g_guest_thread), ran = g_ft_n ? cycles - g_ft_cycles : 0;
    g_ft_tsc = __rdtsc();
    if (!g_ft_n) g_ft_tsc0 = g_ft_tsc;
    g_ft_at += ms;
    g_ft[g_ft_n].at_ms = g_ft_at, g_ft[g_ft_n].ms = (float)ms;
    g_ft[g_ft_n].cycles = ran > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)ran;
    g_ft_cycles = cycles;
    g_ft_n++;
}
static void frame_times_mark(const char* text) {
    if (!g_frame_times[0] || g_ft_marks >= 64) return;
    g_ft_mark[g_ft_marks].frame = g_ft_n;
    strncpy(g_ft_mark[g_ft_marks].text, text, sizeof g_ft_mark[0].text - 1);
    g_ft_marks++;
}
static void frame_times_write(void) {
    if (!g_frame_times[0] || g_ft_n < 3) return;
    FILE* f = fopen(g_frame_times, "w");
    if (!f) { fprintf(stderr, "[frames] cannot write %s\n", g_frame_times); return; }
    double per_ms = (double)(g_ft_tsc - g_ft_tsc0) / (g_ft_at - g_ft[0].at_ms);     /* the cycle counter's rate */
    fprintf(f, "# presented at (ms), frame (ms), main thread ran (ms)\n");
    LONG m = 0;
    for (LONG i = 0; i < g_ft_n; i++) {
        while (m < g_ft_marks && g_ft_mark[m].frame <= i) fprintf(f, "# mark: %s\n", g_ft_mark[m++].text);
        fprintf(f, "%.3f %.4f %.4f\n", g_ft[i].at_ms, g_ft[i].ms, g_ft[i].cycles / per_ms);
    }
    while (m < g_ft_marks) fprintf(f, "# mark: %s\n", g_ft_mark[m++].text);     /* a mark after the last frame */
    fclose(f);
    fprintf(stderr, "[frames] %ld frames' times in %s\n", g_ft_n, g_frame_times);
    g_frame_times[0] = 0;
}

static void pace_note(void) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    if (g_pace_last.QuadPart) {
        double ms = (double)(now.QuadPart - g_pace_last.QuadPart) * 1000.0 / (double)f.QuadPart;
        frame_times_note(ms);
        static LONG warm_up;          /* the first frames load things: not what pacing is about */
        if (warm_up < 120) {
            if (++warm_up == 120) g_pace_cycles0 = thread_cycles(g_guest_thread), g_pace_tsc0 = __rdtsc();
        } else {
            double avg = g_pace_n ? g_pace_sum_ms / g_pace_n : ms;
            g_pace_sum_ms += ms;
            g_pace_n++;
            if (ms > g_pace_worst_ms) g_pace_worst_ms = ms;
            if (ms > avg * 1.5 + 1.0) g_pace_late++;
        }
    } else {
        g_pace_first = now;
    }
    g_pace_last = now;
}

/* What a frame costs, not only how often one came: the main thread's share of
 * a core, by the cycles it actually ran. (The tick-based thread times charge a
 * whole timer tick to whoever is running when it fires, and a thread woken by
 * that very timer always is.) */
static uint64_t thread_cycles(HANDLE thread) {
    ULONG64 c = 0;
    return thread && QueryThreadCycleTime(thread, &c) ? c : 0;
}

static uint64_t g_pace_cycles0, g_pace_tsc0;

static void pace_report(void) {
    frame_times_write();
    if (g_pace_n < 2) return;
    double share = (double)(thread_cycles(g_guest_thread) - g_pace_cycles0) / (double)(__rdtsc() - g_pace_tsc0);
    fprintf(stderr, "[frames] %ld frames, %.1f a second (%.2f ms each); %ld took half as long again or more, the longest %.1f ms\n",
            g_pace_n, 1000.0 * g_pace_n / g_pace_sum_ms, g_pace_sum_ms / g_pace_n, g_pace_late, g_pace_worst_ms);
    fprintf(stderr, "[frames] the main thread ran for %.2f ms of each frame, %.0f%% of one core\n",
            share * g_pace_sum_ms / g_pace_n, 100.0 * share);
}

/* The settings menu (src/ui/settings_menu.cpp). The game is held here, in
 * the Present of the frame it has just drawn, for as long as the menu is
 * open: the menu's frames are presented instead, and the window's messages
 * are pumped as the game's own loop would pump them (this thread is not in
 * the machine during Present, so the game's window procedure can run). When
 * the menu closes the game's clock is put forward by the time it was held,
 * so that the frame after is an ordinary one and not one that long. */
static void menu_hold(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    LARGE_INTEGER t0, t1;
    MSG msg;
    QueryPerformanceCounter(&t0);
    fc_menu_open(dev);
    for (;;) {
        int quit = 0;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = 1; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) { PostQuitMessage((int)msg.wParam); break; }     /* the game's own loop is the one that leaves */
        if (!fc_menu_frame(dev)) break;
        if (FAILED(g_real_present(dev, src, dst, wnd, dirty))) break;   /* a lost device is the game's to mend */
        Sleep(6);
    }
    fc_menu_close();
    QueryPerformanceCounter(&t1);
    if (MEM8(FC_CLOCK_COUNTER)) *(int64_t*)(uintptr_t)FC_CLOCK_LAST += t1.QuadPart - t0.QuadPart;
    g_pace_last = t1;
    g_input_rest = 1;
}

static HRESULT WINAPI host_Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    LONG n = InterlockedIncrement(&g_presents);
    if (n == 1) fprintf(stderr, "[d3d9] first Present\n");
    if (g_force_active) *(volatile uint8_t*)(uintptr_t)FC_GAME_ACTIVE = 1;
    if (g_shots[0] && (n == 1 || n % g_shot_every == 0)) save_shot((IDirect3DDevice9*)dev, n, NULL);
    if (g_shots[0] && g_shot_pending[0]) {
        save_shot((IDirect3DDevice9*)dev, n, g_shot_pending);
        g_shot_pending[0] = 0;
    }
    if (fc_menu_wanted()) menu_hold(dev, src, dst, wnd, dirty);
    HRESULT hr = g_real_present(dev, src, dst, wnd, dirty);
    pace_note();
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
        /* Also here, not only at each Present: once the game believes it is
         * inactive it stops drawing, and would never be told otherwise. */
        if (g_force_active) *(volatile uint8_t*)(uintptr_t)FC_GAME_ACTIVE = 1;
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
    fc_menu_close();                    /* nothing of the menu's may be on a device that is reset */
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

/* ---- native calls that keep the machine ---------------------------------------
 * A frame of this game is about 20,000 calls out of lifted code, nearly all
 * of them short: Direct3D state and draw calls, D3DX vector maths, the heap,
 * the clock. For those the bridge does not hand the machine over
 * (native32_fast_*): none of them waits for another thread of the game. What
 * stays on the slow path is anything that can sleep or wait (Sleep, the
 * message pump, file and wait calls) and Present, which is the host's own
 * function and lies outside Direct3D's range. --count-natives lists what a
 * run called, and which of it was fast. */
static void fast_module(HMODULE m) {
    MODULEINFO mi = { 0 };
    if (m && GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi))
        native32_fast_range((uint32_t)(uintptr_t)mi.lpBaseOfDll, (uint32_t)(uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage);
}

static void fast_imports(void) {
    static const char* const names[] = {
        "D3DXMatrix*", "D3DXVec3*", "D3DXQuaternion*", "D3DXGetFVFVertexSize",
        "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount", "timeGetTime",
        "GetLastError", "SetLastError", "TlsGetValue", "TlsSetValue", "GetCurrentThreadId",
        "GetCurrentProcessId", "GetCurrentProcess", "GetProcessHeap", "HeapAlloc", "HeapFree",
        "HeapReAlloc", "HeapSize", "MultiByteToWideChar", "WideCharToMultiByte", "lstrcmpiA",
        "lstrcpyW", "lstrcatW", "GetACP", "GetOEMCP", "GetCPInfo", "GetStringTypeW", "LCMapStringW",
        "CompareStringW", "EncodePointer", "DecodePointer", "IsProcessorFeaturePresent",
        "GetSystemTimeAsFileTime", "GetAsyncKeyState", "XInputGetState", "SetRect",
        "GetSystemMetrics", "MapVirtualKeyW", "IsDebuggerPresent", "InitializeSListHead",
    };
    int n = 0;
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) n += native32_fast_import(names[i]);
    fprintf(stderr, "[bridge] %d imports and Direct3D keep the machine\n", n);
}

/* ---- the renderer ---------------------------------------------------------------
 * The game draws through Direct3D 9 and nothing else, and what stands behind
 * `Direct3DCreate9` is the host's choice (--renderer):
 *
 *   d3d9    Windows' own Direct3D 9.
 *   vulkan  DXVK, which implements Direct3D 9 on Vulkan: its 32-bit d3d9.dll,
 *           loaded from the dxvk folder beside this exe. Nothing is put beside
 *           the game, and no other program sees it.
 *   d3d12   Direct3D 9 on Direct3D 12, which Windows 10 and later carry
 *           (Direct3DCreate9On12).
 *
 * The game's import of Direct3DCreate9 is a shim that asks the chosen one.
 * D3DX is the system's in every case: it only ever talks to the device it is
 * given. A renderer that is not there falls back to Direct3D 9, with a line
 * in the log saying so. */
enum { RENDERER_D3D9, RENDERER_VULKAN, RENDERER_D3D12 };
static const char* const k_renderer_names[] = { "Direct3D 9", "Vulkan (DXVK)", "Direct3D 12 (D3D9On12)" };
static int g_renderer = RENDERER_D3D9;
static d3d_create9_t g_create9, g_system_create9;
static HMODULE g_system_d3d9;

static void* WINAPI create9_on12(UINT sdk) {
    typedef void* (WINAPI* on12_t)(UINT, D3D9ON12_ARGS*, UINT);
    on12_t f = g_system_d3d9 ? (on12_t)(void*)GetProcAddress(g_system_d3d9, "Direct3DCreate9On12") : NULL;
    D3D9ON12_ARGS args = { 0 };
    args.Enable9On12 = TRUE;
    return f ? f(sdk, &args, 1) : NULL;
}

static void shim_Direct3DCreate9(void) {
    g_eax = (uint32_t)(uintptr_t)g_create9(ARG(0));
    RET(1);
}

/* DXVK's d3d9.dll: in the dxvk folder beside the exe, or beside the build
 * folder in a checkout. Its log and shader cache go beside this exe rather
 * than into the game's folder, which is the working directory. */
static HMODULE load_dxvk(void) {
    char exe[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* cut = strrchr(exe, '\\');
    if (cut) *cut = 0;
    SetEnvironmentVariableA("DXVK_LOG_PATH", exe);
    SetEnvironmentVariableA("DXVK_STATE_CACHE_PATH", exe);
    for (int up = 0; up < 2; up++) {
        _snprintf(path, sizeof path - 1, "%s\\%sdxvk\\d3d9.dll", exe, up ? "..\\" : "");
        path[sizeof path - 1] = 0;
        HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) return m;
    }
    return NULL;
}

const char* fc_host_renderer(void) {
    static const char* const words[] = { "d3d9", "vulkan", "d3d12" };
    return words[g_renderer];
}
int fc_host_has_dxvk(void) {
    char exe[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* cut = strrchr(exe, '\\');
    if (cut) *cut = 0;
    for (int up = 0; up < 2; up++) {
        _snprintf(path, sizeof path - 1, "%s\\%sdxvk\\d3d9.dll", exe, up ? "..\\" : "");
        path[sizeof path - 1] = 0;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

/* The port's own settings are in the game's settings file, in a section of
 * their own, so that there is one file to edit:
 *
 *     [Recomp]
 *     Renderer=d3d9        d3d9, vulkan or d3d12
 *     Fps=125              frames a second; 0 = no limit
 *
 * The game writes its own keys one by one and leaves the section alone. The
 * settings menu writes it; --renderer and --fps win over it for one run. */
static int g_renderer_given, g_fps_given;

static void recomp_settings(void) {
    wchar_t path[MAX_PATH + 40] = L"", text[32] = L"";
    if (g_savedir[0]) wcsncpy(path, g_savedir, MAX_PATH - 1);
    else {
        PWSTR found = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&k_saved_games, 0, NULL, &found)) && found) wcsncpy(path, found, MAX_PATH - 1);
        CoTaskMemFree(found);
    }
    if (!path[0]) return;
    wcscat(path, L"\\FALCOM\\ED6\\config.ini");
    fprintf(stderr, "[settings] %ls\n", path);
    if (!g_renderer_given && GetPrivateProfileStringW(L"Recomp", L"Renderer", L"", text, 32, path))
        g_renderer = !_wcsicmp(text, L"vulkan") || !_wcsicmp(text, L"dxvk") ? RENDERER_VULKAN
                   : !_wcsicmp(text, L"d3d12") || !_wcsicmp(text, L"dx12") ? RENDERER_D3D12 : RENDERER_D3D9;
    if (!g_fps_given) {
        int fps = (int)GetPrivateProfileIntW(L"Recomp", L"Fps", (INT)-2, path);
        if (fps >= 0) g_fps_limit = fps;
    }
}

static int wrap_d3d9(void) {
    void* d3d = NULL;
    g_system_d3d9 = LoadLibraryA("d3d9.dll");
    g_system_create9 = g_system_d3d9 ? (d3d_create9_t)(void*)GetProcAddress(g_system_d3d9, "Direct3DCreate9") : NULL;
    g_create9 = g_system_create9;
    if (g_renderer == RENDERER_VULKAN) {
        HMODULE m = load_dxvk();
        d3d_create9_t f = m ? (d3d_create9_t)(void*)GetProcAddress(m, "Direct3DCreate9") : NULL;
        if (f && (d3d = f(D3D_SDK_VERSION)) != NULL) { g_create9 = f; fast_module(m); }
        else fprintf(stderr, "[renderer] Vulkan was asked for, but %s: using Direct3D 9\n",
                     m ? "DXVK could not start (no Vulkan driver?)" : "there is no dxvk\\d3d9.dll beside the exe");
    } else if (g_renderer == RENDERER_D3D12) {
        if ((d3d = create9_on12(D3D_SDK_VERSION)) != NULL) g_create9 = create9_on12;
        else fprintf(stderr, "[renderer] Direct3D 12 was asked for, but this Windows has no Direct3D 9 on 12: using Direct3D 9\n");
    }
    if (!d3d) {
        g_renderer = RENDERER_D3D9;
        d3d = g_create9 ? g_create9(D3D_SDK_VERSION) : NULL;
    }
    if (!d3d) { fprintf(stderr, "cannot create Direct3D 9\n"); return 1; }
    fast_module(g_system_d3d9);
    fprintf(stderr, "[renderer] %s\n", k_renderer_names[g_renderer]);
    patch_slot(d3d, 16, (void*)host_CreateDevice, (void**)&g_real_createdev);
    ((ULONG (WINAPI*)(void*))(*(void***)d3d)[2])(d3d);            /* Release; the vtable stays */
    return 0;
}

/* ---- silence ---------------------------------------------------------------
 * A test run must not be heard either, but it should still run the game's
 * sound code: the music is streamed from a multimedia timer's thread, the one
 * place the game runs lifted code on a second thread. --silent mutes this
 * process's audio session, which is every stream it opens (DirectSound's, and
 * DirectShow's for the movies), whatever volume the game sets.
 *
 * Windows remembers a session's mute by the exe's path, so a run that is not
 * silent unmutes at start: a silent run that was stopped half way must not
 * leave the game mute for the player. */
static const GUID k_clsid_device_enumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_iid_device_enumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_iid_session_manager = { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

static int session_mute(BOOL mute) {
    IMMDeviceEnumerator* devices = NULL;
    IMMDevice* device = NULL;
    IAudioSessionManager* sessions = NULL;
    ISimpleAudioVolume* volume = NULL;
    BOOL now = !mute;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(CoCreateInstance(&k_clsid_device_enumerator, NULL, CLSCTX_ALL, &k_iid_device_enumerator, (void**)&devices)) &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender, eConsole, &device)) &&
        SUCCEEDED(IMMDevice_Activate(device, &k_iid_session_manager, CLSCTX_ALL, NULL, (void**)&sessions)) &&
        SUCCEEDED(IAudioSessionManager_GetSimpleAudioVolume(sessions, NULL, FALSE, &volume)) &&
        SUCCEEDED(ISimpleAudioVolume_SetMute(volume, mute, NULL)))
        ISimpleAudioVolume_GetMute(volume, &now);
    if (volume) ISimpleAudioVolume_Release(volume);
    if (sessions) IAudioSessionManager_Release(sessions);
    if (device) IMMDevice_Release(device);
    if (devices) IMMDeviceEnumerator_Release(devices);
    return now == mute;
}

static native32_shim_t g_shims[] = {
    /* the seven D3DX entries stay first: main() drops them if D3DX is not there to call */
    { "D3DXMatrixMultiply", shim_D3DXMatrixMultiply },
    { "D3DXVec3TransformCoord", shim_D3DXVec3TransformCoord },
    { "D3DXMatrixTranspose", shim_D3DXMatrixTranspose },
    { "D3DXVec3Normalize", shim_D3DXVec3Normalize },
    { "D3DXMatrixTranslation", shim_D3DXMatrixTranslation },
    { "D3DXMatrixScaling", shim_D3DXMatrixScaling },
    { "D3DXMatrixRotationYawPitchRoll", shim_D3DXMatrixRotationYawPitchRoll },
    { "Direct3DCreate9", shim_Direct3DCreate9 },
    { "D3DXAssembleShader", shim_D3DXAssembleShader },
    { "D3DXDisassembleShader", shim_D3DXDisassembleShader },
    { "D3DXCreateTextureFromFileInMemoryEx", shim_D3DXCreateTextureFromFileInMemoryEx },
    { "D3DXLoadSurfaceFromSurface", shim_D3DXLoadSurfaceFromSurface },
    { "D3DXLoadMeshFromXA", shim_D3DXLoadMeshFromXA },
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
    profile_report();
    pace_report();
    if (native32_count_natives) native32_dump_counts(g_count_top);
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

/* A mod of the original game often comes as a DLL with a system DLL's name,
 * put in the game's folder to be loaded in its place: SoraVoice's dinput8.dll,
 * DXVK as d3d9.dll. They are made for the original exe. The port has the
 * voices itself (src/mods/voice.c) and picks its own renderer (--renderer), so
 * the DLLs the game imports from are Windows' own: where the game's folder
 * has a file of the same name, the system's is loaded here by its full path,
 * before anything asks for it by name alone. */
static void system_dlls_first(const char* game_full) {
    static const char* const names[] = { "d3d9.dll", "dinput8.dll", "dsound.dll", "winmm.dll", "version.dll",
                                         "dbghelp.dll", "xinput9_1_0.dll", "d3dx9_43.dll", "d3dxof.dll" };
    char sys[MAX_PATH], path[MAX_PATH + 32];
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        _snprintf(path, sizeof path - 1, "%s\\%s", game_full, names[i]);
        path[sizeof path - 1] = 0;
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        _snprintf(path, sizeof path - 1, "%s\\%s", sys, names[i]);
        if (LoadLibraryA(path)) fprintf(stderr, "[mods] %s in the game's folder is left alone: Windows' own is used\n", names[i]);
    }
}

static int dir_exists(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int main(int argc, char** argv) {
    const char* game = NULL;
    const char* savedir = NULL;
    const char* script = NULL;
    const char* profile = NULL;
    const char* mods = NULL;
    const char* extra = "";             /* --gameargs "...": appended to the guest's command line */
    char exe_full[MAX_PATH], game_full[MAX_PATH];
    int run = 0, voice_log = 0;
    /* Unbuffered: the log is the evidence, and ExitProcess flushes nothing
     * with the C runtime linked in (/MT). */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--unattended")) g_unattended = 1;
        else if (!strcmp(argv[i], "--active")) g_force_active = 1;
        else if (!strcmp(argv[i], "--silent")) g_silent = 1;
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            const char* r = argv[++i];
            g_renderer_given = 1;
            g_renderer = !_stricmp(r, "vulkan") || !_stricmp(r, "dxvk") ? RENDERER_VULKAN
                       : !_stricmp(r, "d3d12") || !_stricmp(r, "dx12") ? RENDERER_D3D12 : RENDERER_D3D9;
        }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps_limit = atoi(argv[++i]), g_fps_given = 1;
        else if (!strcmp(argv[i], "--original-limiter")) g_fps_limit = -1, g_fps_given = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--savedir") && i + 1 < argc) savedir = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_shots, NULL);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--mods") && i + 1 < argc) mods = argv[++i];
        else if (!strcmp(argv[i], "--voice-log")) voice_log = 1;
        else if (!strcmp(argv[i], "--frame-times") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_frame_times, NULL);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            static char full[MAX_PATH];      /* before the working directory changes */
            GetFullPathNameA(argv[++i], MAX_PATH, full, NULL);
            profile = full;
        }
        else if (!strcmp(argv[i], "--gameargs") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--count-natives")) {
            native32_count_natives = 1;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) g_count_top = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--d3dx-dump") && i + 1 < argc) g_d3dx_dump = argv[++i];
        else if (!strcmp(argv[i], "--slow-calls") && i + 1 < argc) native32_slow_ms = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: trails [--run] [--game DIR] [--savedir DIR] [--unattended] [--active] [--silent]\n"
                   "              [--renderer d3d9|vulkan|d3d12] [--mods DIR] [--voice-log]\n"
                   "              [--fps N (0 = no limit; default 125, the game's own)] [--original-limiter]\n"
                   "              [--shots DIR] [--shot-every N] [--script FILE] [--watchdog S]\n"
                   "              [--profile FILE] [--frame-times FILE] [--gameargs \"...\"]\n"
                   "              [--probe VA] [--native-trace] [--callbacks] [--count-natives [N]] [--d3dx-dump DIR]\n"
                   "              [--slow-calls MS]\n");
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
    if (script && load_script(script)) return 1;
    mods_scan_game(game_full);          /* SoraVoice's scripts, where that mod puts them */
    {   /* The mods folder: --mods, or "mods" beside the exe, or beside the build folder in a checkout. */
        char dir[MAX_PATH], exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* cut = strrchr(exe, '\\');
        if (cut) *cut = 0;
        if (mods) {
            GetFullPathNameA(mods, MAX_PATH, dir, NULL);
        } else {
            _snprintf(dir, sizeof dir - 1, "%s\\mods", exe);
            dir[sizeof dir - 1] = 0;
            if (!dir_exists(dir)) _snprintf(dir, sizeof dir - 1, "%s\\..\\mods", exe);
        }
        if (dir_exists(dir)) mods_scan(dir);
    }
    /* steam_api.dll, Galaxy.dll and xinput1_3.dll sit beside the game exe. */
    system_dlls_first(game_full);
    SetDllDirectoryA(game_full);

    /* Muted for a silent run, and unmuted for any other (see session_mute). */
    if (!session_mute(g_silent) && g_silent) {
        fprintf(stderr, "--silent: this process's audio session could not be muted; not running\n");
        return 1;
    }
    if (g_silent) fprintf(stderr, "[audio] this process's audio session is muted\n");
    native32_init();
    AddVectoredExceptionHandler(0, crash);
    /* The C runtime in the exe picks its string and math routines by CPUID.
     * Leaf 1's ecx is everything after SSE2 (SSE3 to AVX, and OSXSAVE): with
     * it clear the runtime stays on its SSE2 paths, which are what lift32
     * covers. edx (SSE, SSE2, CMOV, MMX) stays as the CPU reports it. */
    g_cpuid_ecx1 = 0;
    printf("Trails in the Sky FC recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    recomp_settings();
    {   /* A test's look at the settings menu: TRAILS_MENU_GRAB=<frame> opens it by itself at that frame, saves a
         * picture of each page (--shots) and closes it; <frame>,set also changes two settings while it is open. */
        const char* grab = getenv("TRAILS_MENU_GRAB");
        if (grab && atoi(grab) > 0) fc_menu_grab_after(atoi(grab), strstr(grab, ",set") != NULL);
    }
    if (wrap_d3d9()) return 1;
    uint32_t span = native32_map(exe_full, FC_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, FC_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", FC_EXE, FC_IMAGE_BASE, FC_IMAGE_BASE + span);
    g_resources = LoadLibraryExA(exe_full, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    {
        int skip = d3dx_direct() ? 0 : 7;       /* without D3DX to call, its imports bind as usual (and fail there) */
        if (native32_bind(FC_IMAGE_BASE, g_shims + skip, (int)(sizeof g_shims / sizeof g_shims[0]) - skip)) return 1;
    }
    fast_imports();
    QueryPerformanceFrequency(&g_qpc_freq);
    /* A timer that keeps its word to well under a millisecond, without
     * changing the whole system's tick (Windows 10 1803 on); else the tick. */
    g_frame_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
    if (!g_frame_timer) timeBeginPeriod(1);
    printf("  guest command line %s\n", g_guest_cmdline);
    if (savedir) printf("  Saved Games folder %ls\n", g_savedir);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", fc_entry_va);
        return 0;
    }
    /* The archives (ED6_DT*.dat) are opened relative to the game folder. */
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    fc_voice_init(voice_log);
    g_t0 = GetTickCount();
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, desktop_watch, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", fc_entry_va);
    fflush(stdout);
    HANDLE t = CreateThread(NULL, GUEST_MAIN_STACK, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { fprintf(stderr, "cannot start the guest thread (error %lu)\n", GetLastError()); return 1; }
    g_guest_thread = t;
    if (profile) profile_start(t, profile);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    printf("\nentry returned eax=%08lX, %ld frames presented\n", (unsigned long)code, g_presents);
    return (int)code;
}
