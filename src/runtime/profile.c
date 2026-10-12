/*
 * A sampling profiler for the lifted game: where does a frame's time go?
 *
 * --profile FILE samples the game's main thread about a thousand times a
 * second (suspend, read eip, resume) and, when the run ends, writes where it
 * was: which module, and inside this exe which function, by the build's own
 * symbols. A lifted function is sub_XXXXXXXX, so the report names game code by
 * its address in the original; the rest of this exe is the runtime (the
 * native bridge, the machine lock, dispatch), and every other module is
 * Windows, Direct3D or the driver, which the report also splits by the
 * import the game was inside at the time.
 *
 * Wall time, not CPU time: a thread asleep in the frame limiter is sampled
 * too, and shows up as Sleep. That is on purpose, since what is left after
 * the sleeps is the frame's real cost.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "recomp_trace.h"
#include "profile.h"

#define MAX_SAMPLES (4u << 20)        /* a little over an hour at 1 kHz */

typedef struct { uint32_t eip, cur_func; const char* import; } sample_t;

static sample_t* g_samples;
static volatile LONG g_nsamples;
static HANDLE g_target, g_thread;
static volatile LONG g_stop;
static char g_out[MAX_PATH];
static DWORD g_started;

static DWORD WINAPI sampler(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (!g_stop) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_target) != (DWORD)-1) {
            if (GetThreadContext(g_target, &ctx) && (uint32_t)g_nsamples < MAX_SAMPLES) {
                sample_t* s = &g_samples[g_nsamples];
                s->eip = ctx.Eip;
                s->cur_func = g_cur_func;
                s->import = g_cur_import;
                InterlockedIncrement(&g_nsamples);
            }
            ResumeThread(g_target);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
}

void profile_start(HANDLE thread, const char* out_path) {
    g_samples = (sample_t*)VirtualAlloc(NULL, MAX_SAMPLES * sizeof *g_samples, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_samples) { fprintf(stderr, "[profile] no memory for the samples\n"); return; }
    DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    GetFullPathNameA(out_path, MAX_PATH, g_out, NULL);
    g_started = GetTickCount();
    g_thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct { char name[96]; uint32_t n; } bucket_t;

static bucket_t* bump(bucket_t* b, int* nb, int cap, const char* name) {
    for (int i = 0; i < *nb; i++)
        if (!strcmp(b[i].name, name)) { b[i].n++; return &b[i]; }
    if (*nb == cap) return NULL;
    strncpy(b[*nb].name, name, sizeof b[0].name - 1);
    b[*nb].name[sizeof b[0].name - 1] = 0;
    b[*nb].n = 1;
    return &b[(*nb)++];
}

static int by_count(const void* a, const void* b) {
    uint32_t x = ((const bucket_t*)a)->n, y = ((const bucket_t*)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void print_top(FILE* f, const char* title, bucket_t* b, int nb, uint32_t total, int top) {
    qsort(b, nb, sizeof *b, by_count);
    fprintf(f, "\n%s\n", title);
    for (int i = 0; i < nb && i < top; i++)
        fprintf(f, "  %6.2f%%  %7u  %s\n", 100.0 * b[i].n / total, b[i].n, b[i].name);
}

void profile_report(void) {
    static volatile LONG once;
    if (!g_thread || InterlockedExchange(&once, 1)) return;
    g_stop = 1;
    WaitForSingleObject(g_thread, 2000);
    uint32_t n = (uint32_t)g_nsamples;
    FILE* f = fopen(g_out, "w");
    if (!f || !n) { fprintf(stderr, "[profile] nothing written (%u samples)\n", n); return; }

    HANDLE proc = GetCurrentProcess();
    HMODULE self = GetModuleHandleW(NULL);
    MODULEINFO mi = { 0 };
    GetModuleInformation(proc, self, &mi, sizeof mi);
    uint32_t lo = (uint32_t)(uintptr_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);

    enum { CAP = 8192 };
    bucket_t* mods = (bucket_t*)calloc(256, sizeof *mods);
    bucket_t* funcs = (bucket_t*)calloc(CAP, sizeof *funcs);
    bucket_t* imports = (bucket_t*)calloc(1024, sizeof *imports);
    bucket_t* callers = (bucket_t*)calloc(CAP, sizeof *callers);
    bucket_t* spots = (bucket_t*)calloc(CAP * 4, sizeof *spots);
    bucket_t* natives = (bucket_t*)calloc(CAP, sizeof *natives);
    int nmods = 0, nfuncs = 0, nimports = 0, ncallers = 0, nspots = 0, nnatives = 0;
    uint32_t lifted = 0, runtime = 0, outside = 0, asleep = 0;
    char sym[sizeof(SYMBOL_INFO) + 256];

    for (uint32_t i = 0; i < n; i++) {
        const sample_t* s = &g_samples[i];
        char name[128];
        if (s->eip >= lo && s->eip < hi) {
            SYMBOL_INFO* si = (SYMBOL_INFO*)sym;
            DWORD64 disp = 0;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            if (SymFromAddr(proc, s->eip, &disp, si)) _snprintf(name, sizeof name - 1, "%s", si->Name);
            else _snprintf(name, sizeof name - 1, "trails+0x%X", s->eip - lo);
            name[sizeof name - 1] = 0;
            if (!strncmp(name, "sub_", 4) || !strncmp(name, "fc_orig_", 8)) lifted++; else runtime++;
            bump(funcs, &nfuncs, CAP, name);
            {   /* the instruction itself, for a function that is hot for no clear reason */
                char spot[128];
                _snprintf(spot, sizeof spot - 1, "%s+0x%X  (0x%08X)", name, (unsigned)disp, s->eip);
                spot[sizeof spot - 1] = 0;
                bump(spots, &nspots, CAP * 4, spot);
            }
            bump(mods, &nmods, 256, !strncmp(name, "sub_", 4) || !strncmp(name, "fc_orig_", 8)
                                        ? "(lifted game code)" : "(runtime: bridge, lock, dispatch, host)");
        } else {
            HMODULE m = NULL;
            char path[MAX_PATH] = "";
            const char* imp = s->import ? s->import : "(native)";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)s->eip, &m) && GetModuleFileNameA(m, path, sizeof path)) {
                const char* base = strrchr(path, '\\');
                bump(mods, &nmods, 256, base ? base + 1 : path);
                {   /* the nearest export: enough to tell a lock from a wait */
                    SYMBOL_INFO* si = (SYMBOL_INFO*)sym;
                    DWORD64 disp = 0;
                    char where[160];
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen = 255;
                    if (SymFromAddr(proc, s->eip, &disp, si))
                        _snprintf(where, sizeof where - 1, "%s!%s", base ? base + 1 : path, si->Name);
                    else
                        _snprintf(where, sizeof where - 1, "%s!?", base ? base + 1 : path);
                    where[sizeof where - 1] = 0;
                    bump(natives, &nnatives, CAP, where);
                }
            } else {
                bump(mods, &nmods, 256, "(no module)");
            }
            if (strstr(imp, "!Sleep") || strstr(imp, "WaitForSingleObject") || strstr(imp, "MsgWait")) asleep++;
            else outside++;
            bump(imports, &nimports, 1024, imp);
            _snprintf(name, sizeof name - 1, "sub_%08X -> %s", s->cur_func, imp);
            name[sizeof name - 1] = 0;
            bump(callers, &ncallers, CAP, name);
        }
    }

    DWORD ms = GetTickCount() - g_started;
    uint32_t busy = n - asleep;
    fprintf(f, "%u samples over %.1f s (%.0f a second)\n", n, ms / 1000.0, n * 1000.0 / (ms ? ms : 1));
    fprintf(f, "\nWhere the main thread was:\n");
    fprintf(f, "  %6.2f%%  asleep (Sleep, waits)\n", 100.0 * asleep / n);
    fprintf(f, "  %6.2f%%  lifted game code\n", 100.0 * lifted / n);
    fprintf(f, "  %6.2f%%  the runtime in this exe (bridge, lock, dispatch, host)\n", 100.0 * runtime / n);
    fprintf(f, "  %6.2f%%  native code the game called (Windows, Direct3D, the driver)\n", 100.0 * outside / n);
    if (busy)
        fprintf(f, "\nOf the time not asleep: lifted %.1f%%, runtime %.1f%%, native %.1f%%\n",
                100.0 * lifted / busy, 100.0 * runtime / busy, 100.0 * outside / busy);
    print_top(f, "By module:", mods, nmods, n, 24);
    print_top(f, "Functions in this exe (sub_XXXXXXXX is lifted game code at that address):", funcs, nfuncs, n, 60);
    print_top(f, "Hottest instructions in this exe (function+offset, address):", spots, nspots, n, 40);
    print_top(f, "Native code by nearest export:", natives, nnatives, n, 40);
    print_top(f, "Native time by the import the game was inside:", imports, nimports, n, 30);
    print_top(f, "Native time by the lifted function that made the call:", callers, ncallers, n, 30);
    fclose(f);
    fprintf(stderr, "[profile] %u samples -> %s\n", n, g_out);
}
