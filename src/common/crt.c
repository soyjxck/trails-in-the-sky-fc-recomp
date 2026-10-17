/*
 * crt.c -- pieces of the game's C runtime, done by the host.
 *
 * The C runtime is linked into the exe, so its functions are lifted like the
 * game's own. A few of them are called so often, and are so much longer as
 * lifted code than as what they mean, that they show at the top of a profile
 * (docs/bringup.md, 23): they are written for SSE2, and an SSE register is
 * memory to the lifted code. These are replaced: run_lift.py emits a call to
 * fc_hook_<address> in place of the lift (its HOOKS), on every host.
 *
 * Each does what the original does to the registers and memory a caller may
 * look at afterwards: the result, and the return address taken off the
 * stack. What the original leaves in ecx, edx, the flags and the SSE
 * registers on the way is scratch to its callers.
 *
 *   0x00504ABF  _isnan(double), cdecl
 *   0x00504A01  _finite(double), cdecl
 *   0x00522510  the compiler's helper for (__int64)float: the float in xmm0,
 *               the result in edx:eax, truncated; out of range or not a
 *               number, the smallest 64-bit integer
 *   0x004FE110  memset(to, value, count), cdecl
 *   0x004FD597  _CxxThrowException: a throw is this function, not an import.
 *               Lifted as it is, it raises on the host's own exception
 *               chain, which has never heard of the guest's frames;
 *               pcrecomp's eh32 dispatches it over the guest's own.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "recomp_types.h"

static double stack_double(uint32_t at) {
    double d;
    memcpy(&d, (const void*)ADDR(at), 8);
    return d;
}

void fc_hook_00504ABF(void) {
    g_eax = isnan(stack_double(g_esp + 4)) ? 1u : 0u;
    g_esp += 4;
}

void fc_hook_00504A01(void) {
    g_eax = isfinite(stack_double(g_esp + 4)) ? 1u : 0u;
    g_esp += 4;
}

void fc_hook_00522510(void) {
    float f = g_xmm[0].f32[0];
    int64_t v = INT64_MIN;
    if (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) v = (int64_t)f;   /* neither holds for a NaN */
    g_eax = (uint32_t)(uint64_t)v;
    g_edx = (uint32_t)((uint64_t)v >> 32);
    g_esp += 4;
}

void fc_hook_004FE110(void) {
    uint32_t to = MEM32(g_esp + 4), count = MEM32(g_esp + 12);
    if (count) memset((void*)ADDR(to), (int)MEM8(g_esp + 8), count);
    g_eax = to;
    g_esp += 4;
}

void native32_shim_CxxThrowException(void);
void fc_hook_004FD597(void) { native32_shim_CxxThrowException(); }
