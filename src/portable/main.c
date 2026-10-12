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
static void log_done(int code) {
    fprintf(stderr, "done: exit code %d\n", code);
    fflush(stderr);
}
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
void fc_wrap_004802C0(void) { fc_orig_004802C0(); }
void fc_wrap_004C11A0(void) { fc_orig_004C11A0(); }
void fc_wrap_00477580(void) { fc_orig_00477580(); }
void fc_wrap_00477180(void) { fc_orig_00477180(); }

/* _CxxThrowException (the C runtime is linked into the exe, so a throw is
 * this function): dispatched over the guest's own frames by eh32. */
void native32_shim_CxxThrowException(void);
void fc_hook_004FD597(void) { native32_shim_CxxThrowException(); }

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
    (void)argc; (void)argv;
    /* The SD card is the default device from here on, so a path that starts
     * with a slash is on it; the game's folder is the working directory, as
     * the game expects of its own. */
    if (chdir("sdmc:/switch/trails/game") != 0) return 1;
    if (!freopen(FC_LOG_FILE, "w", stderr)) return 1;
    hle_on_exit = log_done;
    exe = FC_DEFAULT_EXE;
    saves = FC_SAVES_DIR;
#else
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--imports")) list_only = 1;
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
        else exe = argv[i];
    }
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!exe) {
        fprintf(stderr, "usage: trails-portable [--imports] <path to ed6_win_DX9.exe>\n");
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
