/*
 * Trails in the Sky FC - the portable host (Linux, and from there the Switch).
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: every import the game makes
 * is answered by the host's own code, so there is no Windows underneath. On a
 * 64-bit host the guest's 4 GB is one reservation and every guest address an
 * offset into it (win32hle's guest_mem.c), which is what an arm64 machine
 * needs.
 *
 * This is the bring-up host. It maps the exe, binds what win32hle already
 * answers, and gives every other import a stub that names itself when
 * called: --imports lists them, and a run goes until the game calls one.
 *
 *   trails-portable [options] <path to ed6_win_DX9.exe>
 *     --imports          list the imports with no answer and stop
 *     --saves DIR        the game's Saved Games folder (default: save)
 *     --headless         draw with no window (a test run)
 *     --frames N         leave after N frames have been presented
 *     --shots DIR        save the picture there as PNG: the first frame, and
 *     --shot-every N     every Nth after it
 *     --watch S          say every S seconds where the game is
 *     --trace PREFIX     list every call out whose name starts so (IDirect3D...)
 *     --d3d-log          list render targets, viewports and what is read back
 *
 * On the Switch there is no command line: the same options are read from
 * /switch/trails/args.txt if it is there.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"

/* On the Switch the game's folder is on the SD card and there is no console
 * to write to: what a run says goes to a file beside it, ending with a line
 * that starts "done" (how a test run under an emulator knows it has ended). */
#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>
#define FC_DEFAULT_EXE "/switch/trails/game/ed6_win_DX9.exe"
#define FC_SAVES_DIR   "/switch/trails/save"
#define FC_LOG_FILE    "/switch/trails/trails.log"
#define FC_ARGS_FILE   "/switch/trails/args.txt"
static void log_done(int code) {
    static int said;
    if (said++) return;
    fprintf(stderr, "done: exit code %d\n", code);
    fflush(stderr);
    fclose(stderr);                     /* the card's file is only whole once closed */
}
static void log_done_at_exit(void) { log_done(0); }
#endif

extern const uint32_t fc_entry_va;    /* recomp_dispatch.c */
extern const win32hle_shim fc_win32_wide[];   /* win32_wide.c */
void fc_set_saves_folder(const char* host_dir);

/* ---- what the lift calls into (HOOKS and WRAPS in run_lift.py) -------------
 * Passed straight through for now: the Windows host's versions (the frame
 * limiter, play scripts, the mod loader) move here as this host grows. */
void fc_orig_004802C0(void);
void fc_orig_004C11A0(void);
void fc_orig_00477580(void);
void fc_orig_00477180(void);

/* ---- a controller -----------------------------------------------------------------
 * Input_Poll (0x004802C0) gathers keyboard, mouse and pad into one mask of
 * buttons and returns it. It is wrapped, and what SDL's first controller
 * holds down is added to the mask as the game's own pad bindings would give
 * it: the direction pad and left stick as the four directions, and
 *
 *     A  confirm          B  cancel            (as the console labels them:
 *     X, Y                the game's third and tenth pad buttons   on a PC pad the
 *     L, R                its fourth and fifth                     lower button
 *     ZL, ZR              its ninth and sixth                      confirms)
 *     +, -                its seventh and eighth
 *
 * DirectInput, which is how the game itself finds a pad, is not answered yet;
 * this needs none of it. */
#include <SDL.h>
#define FC_MOUSE_IN_USE 0x00557E5Eu   /* byte: set while the mouse drives the menus */
#define FC_MOUSE_MODE   0x006B0018u   /* byte: mouse mode */
enum { B_UP = 0x1, B_DOWN = 0x2, B_RIGHT = 0x4, B_LEFT = 0x8, B_CONFIRM = 0x60, B_CANCEL = 0x90, B_PAD3 = 0x400000,
       B_PAD4 = 0x400, B_PAD5 = 0x800, B_PAD6 = 0x8000, B_PAD7 = 0x10000, B_PAD8 = 0x20000, B_PAD9 = 0x200, B_PAD10 = 0x800000 };
static uint32_t pad_buttons(void) {
    static SDL_Joystick* joy;
    static SDL_GameController* pad;
    static int tried;
    uint32_t b = 0;
    int x = 0, y = 0;
    if (!tried) {
        tried = 1;
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0 || SDL_NumJoysticks() < 1) return 0;
#ifndef __SWITCH__
        if (SDL_IsGameController(0)) pad = SDL_GameControllerOpen(0);
#endif
        if (!pad) joy = SDL_JoystickOpen(0);
        fprintf(stderr, "[pad] %s\n", pad ? SDL_GameControllerName(pad) : joy ? SDL_JoystickName(joy) : "none could be opened");
    }
    if (pad) {
        static const struct { SDL_GameControllerButton from; uint32_t to; } map[] = {
            { SDL_CONTROLLER_BUTTON_A, B_CONFIRM }, { SDL_CONTROLLER_BUTTON_B, B_CANCEL }, { SDL_CONTROLLER_BUTTON_X, B_PAD3 },
            { SDL_CONTROLLER_BUTTON_Y, B_PAD10 }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_PAD4 },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_PAD5 }, { SDL_CONTROLLER_BUTTON_START, B_PAD7 },
            { SDL_CONTROLLER_BUTTON_BACK, B_PAD8 }, { SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT },
        };
        SDL_GameControllerUpdate();
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(pad, map[i].from)) b |= map[i].to;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= B_PAD9;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= B_PAD6;
        x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    } else if (joy) {
        /* The console's own pad, in the order its buttons are numbered: A, B, X,
         * Y, the two sticks, L, R, ZL, ZR, plus, minus, then left, up, right, down. */
        static const uint32_t map[16] = { B_CONFIRM, B_CANCEL, B_PAD3, B_PAD10, 0, 0, B_PAD4, B_PAD5, B_PAD9, B_PAD6,
                                          B_PAD7, B_PAD8, B_LEFT, B_UP, B_RIGHT, B_DOWN };
        SDL_JoystickUpdate();
        for (int i = 0; i < 16 && i < SDL_JoystickNumButtons(joy); i++) if (SDL_JoystickGetButton(joy, i)) b |= map[i];
        if (SDL_JoystickNumAxes(joy) >= 2) x = SDL_JoystickGetAxis(joy, 0), y = SDL_JoystickGetAxis(joy, 1);
    }
    if (x < -16000) b |= B_LEFT;
    if (x > 16000) b |= B_RIGHT;
    if (y < -16000) b |= B_UP;
    if (y > 16000) b |= B_DOWN;
    return b;
}
void fc_wrap_004802C0(void) {
    fc_orig_004802C0();
    uint32_t held = pad_buttons();
    if (held) {
        MEM8(FC_MOUSE_IN_USE) = 0;        /* the menus follow the buttons, not a pointer that is not moving */
        MEM8(FC_MOUSE_MODE) = 0;
        g_eax |= held;
    }
}
void fc_wrap_004C11A0(void) { fc_orig_004C11A0(); }
void fc_wrap_00477580(void) { fc_orig_00477580(); }
void fc_wrap_00477180(void) { fc_orig_00477180(); }

/* _CxxThrowException (the C runtime is linked into the exe, so a throw is
 * this function): dispatched over the guest's own frames by eh32. */
void native32_shim_CxxThrowException(void);
void fc_hook_004FD597(void) { native32_shim_CxxThrowException(); }

/* ---- test runs: frames counted, pictures saved ------------------------------ */
static const char* g_shots;
static unsigned g_shot_every, g_frames, g_frame_now;
static void on_frame(unsigned frame) {
    g_frame_now = frame;
    if (g_shots && (frame == 1 || (g_shot_every && frame % g_shot_every == 0))) {
        char path[700];
        snprintf(path, sizeof path, "%s/frame_%06u.png", g_shots, frame);
        fprintf(stderr, "[shots] frame %u -> %s%s\n", frame, path, hle_d3d9_save_png(path) == 0 ? "" : " (not written)");
    }
    if (g_frames && frame >= g_frames) {
        fprintf(stderr, "[frames] %u presented with %lu draws: leaving\n", frame, hle_d3d9_draws());
        hle_exit(0);
    }
}

/* ---- a fault: say where, in the guest's terms and the host's ------------------ */
#if defined(__linux__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
static void on_fault(int sig, siginfo_t* info, void* context) {
    void* frames[24];
    (void)context;
    uintptr_t base = (uintptr_t)hle_g2h(0x10000) - 0x10000, at = (uintptr_t)info->si_addr;
    fprintf(stderr, "\n[fault] signal %d at address %p", sig, info->si_addr);
    if (at >= base && at - base <= 0xFFFFFFFFu) fprintf(stderr, " (the guest's 0x%08X)", (uint32_t)(at - base));
    fprintf(stderr, ", in or under sub_%08X\n", g_cur_func);
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
    _exit(3);
}
static void catch_faults(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL), sigaction(SIGBUS, &sa, NULL), sigaction(SIGFPE, &sa, NULL), sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
#else
static void catch_faults(void) {}
#endif

/* ---- where the game is, said from a thread of the host's own -------------------- */
#include <pthread.h>
#include <time.h>
static unsigned g_watch;
static void* watch_thread(void* arg) {
    struct timespec nap = { (time_t)g_watch, 0 };
    unsigned long last[HLE_THREADS_SEEN] = { 0 };
    (void)arg;
    for (unsigned t = g_watch;; t += g_watch) {
        nanosleep(&nap, NULL);
        fprintf(stderr, "[watch] %u s: frame %u, %lu draws, the renderer is %s\n", t, g_frame_now, hle_d3d9_draws(), hle_d3d9_doing);
        for (int i = 0; i < HLE_THREADS_SEEN; i++) {
            unsigned long calls = hle_threads[i].calls;
            if (!hle_threads[i].shim) continue;
            fprintf(stderr, "[watch]   thread %d: %lu calls out since the last line; the last was %s from sub_%08X, %s\n", i,
                    calls - last[i], hle_threads[i].shim, hle_threads[i].from, hle_threads[i].inside ? "not yet back" : "returned");
            last[i] = calls;
        }
    }
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X (called from 0x%08X)\n", va, g_cur_func);
    hle_exit(2);
}

static int g_unanswered;
static uint32_t resolve_and_count(const char* name) {
    uint32_t va = hle_resolve(name);
    if (va) return va;
    g_unanswered++;
    fprintf(stderr, "  unanswered: %s\n", name);
    return hle_resolve_or_stub(name);
}

int main(int argc, char** argv) {
    const char* exe = NULL;
    const char* saves = "save";       /* --saves DIR: where the game's Saved Games folder is */
    int list_only = 0;
#ifdef __SWITCH__
    static char text[2048];
    static char* words[64];
    /* The SD card is the default device from here on, so a path that starts
     * with a slash is on it; the game's folder is the working directory, as
     * the game expects of its own. */
    if (chdir("sdmc:/switch/trails/game") != 0) return 1;
    if (!freopen(FC_LOG_FILE, "w", stderr)) return 1;
    /* The console gives a core's time by turns only to threads of priority
     * 59, which is where the C library starts every thread but this one. The
     * main thread starts above them, and wherever the game polls for another
     * thread's work it would never let that thread run. */
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    hle_on_exit = log_done;
    atexit(log_done_at_exit);
    saves = FC_SAVES_DIR;
    hle_d3d9_vsync = 1;
    {
        FILE* f = fopen(FC_ARGS_FILE, "r");
        size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        text[n] = 0;
        argc = 1, argv = words;
        for (char* w = strtok(text, " \t\r\n"); w && argc < 63; w = strtok(NULL, " \t\r\n")) words[argc++] = w;
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--imports")) list_only = 1;
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
        else if (!strcmp(argv[i], "--headless")) hle_d3d9_headless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) g_shots = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) g_watch = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--d3d-log")) hle_d3d9_log = 1;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) setenv("HLE_SHIMTRACE", argv[++i], 1);
        else exe = argv[i];
    }
#ifdef __SWITCH__
    if (!exe) exe = FC_DEFAULT_EXE;
#endif
    if (g_shots) mkdir(g_shots, 0755);
    hle_d3d9_title = "Trails in the Sky FC";
    hle_d3d9_frame_hook = on_frame;
    if (g_watch) {
        pthread_t t;
        pthread_create(&t, NULL, watch_thread, NULL);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    catch_faults();
    if (!exe) {
        fprintf(stderr, "usage: trails-portable [--imports] [--saves DIR] [--headless] [--frames N]\n"
                        "                       [--shots DIR] [--shot-every N] [--watch S] <path to ed6_win_DX9.exe>\n");
        return 1;
    }
    mkdir(saves, 0755);
    fc_set_saves_folder(saves);
    win32hle_register(fc_win32_wide);       /* first: this game's own answers win */
    recomp_host_init();
    pe_image img;
    if (recomp_pe_map(exe, &img) != 0) { fprintf(stderr, "cannot map %s\n", exe); return 1; }
    {
        char guest[1024];
        hle_guest_path(exe, guest, sizeof guest);
        hle_module_add(guest, img.base, img.resource_rva, img.resource_size, 1);
        hle_set_command_line(guest);
    }
    fprintf(stderr, "Trails in the Sky FC, portable host: image at 0x%08X, entry 0x%08X, %u lifted functions\n",
            img.base, img.entry, recomp_dispatch_count);
    recomp_pe_bind(&img, resolve_and_count);
    fprintf(stderr, "%d imports have no answer yet\n", g_unanswered);
    if (list_only) return 0;
    fprintf(stderr, "entering 0x%08X\n\n", fc_entry_va);
    uint32_t rc = hle_call_guest(fc_entry_va, 0, NULL);
    fprintf(stderr, "\nentry returned eax=%08X\n", rc);
#ifdef __SWITCH__
    log_done((int)rc);
#endif
    return (int)rc;
}
