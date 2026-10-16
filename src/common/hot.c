/*
 * hot.c -- functions of the game's own that a console's profile puts at the
 * top, done by the host on the path most of their calls take
 * (docs/bringup.md, 26).
 *
 * Each is in run_lift.py's WRAPS: the lift is kept as fc_orig_<address> and
 * runs for every case not answered here. What is answered here is only the
 * common, simple path, with the same arithmetic in the same order, so that
 * the answer is the original's to the bit. FC_HOT_CHECK=1 runs the original
 * beside each answer and says where they differ (the first few of them, then
 * a count every so often).
 *
 *   0x004459B0  thiscall bool (plane *this, const float a[3], const float b[3]),
 *               ret 8: whether the segment from a to b meets the plane. Each
 *               end's distance is (n.y*y + x*n.x) + n.z*z - d in single
 *               precision (the normal at +0x24, d at +0x58), as the original's
 *               SSE has it; the segment meets the plane when both distances
 *               are finite and one is at or below zero while the other is at or
 *               above it. Only then does the original go on to work out where
 *               (0x00444920, 0x00444CF0), and only then is it run. It is a
 *               collision test, called for each face of each shape near anyone
 *               moving: 5.7% of the console's time in a busy street. The
 *               original's early answers leave eax 0 and touch nothing else a
 *               caller looks at; the SSE registers it used on the way are
 *               scratch to its callers.
 */
#if defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")   /* a multiply and an add stay two roundings, as SSE does them */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

void fc_orig_004459B0(void);

static float guest_float(uint32_t at) {
    float f;
    memcpy(&f, (const void*)ADDR(at), 4);
    return f;
}
static int finite_float(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}

static int g_check = -1, g_off = -1;
static unsigned long g_checked, g_differed;
static int checking(void) {
    if (g_check < 0) g_check = getenv("FC_HOT_CHECK") && atoi(getenv("FC_HOT_CHECK")) > 0;
    return g_check;
}
/* FC_HOT_OFF=1: every call to the original, to measure what this saves. */
static int off(void) {
    if (g_off < 0) g_off = getenv("FC_HOT_OFF") && atoi(getenv("FC_HOT_OFF")) > 0;
    return g_off;
}

/* The distance of the point at p from the plane at pl, as 0x004459B0 has it. */
static float plane_distance(uint32_t pl, uint32_t p) {
    float t = guest_float(pl + 0x28) * guest_float(p + 4);
    t = t + guest_float(p) * guest_float(pl + 0x24);
    t = t + guest_float(pl + 0x2C) * guest_float(p + 8);
    return t - guest_float(pl + 0x58);
}

void fc_wrap_004459B0(void) {
    if (off()) { fc_orig_004459B0(); return; }
    uint32_t pl = g_ecx, a = MEM32(g_esp + 4), b = MEM32(g_esp + 8);
    float da = plane_distance(pl, a), db = plane_distance(pl, b);
    int meets = finite_float(da) && finite_float(db) && ((da >= 0.0f && db <= 0.0f) || (da <= 0.0f && db >= 0.0f));
    if (meets) {
        static unsigned long met;
        if (checking() && ++met % 10000ul == 0) fprintf(stderr, "[hot] 0x004459B0: %lu segments met their plane\n", met);
        fc_orig_004459B0();                         /* where: the original's */
        return;
    }
    if (checking()) {
        uint32_t esp = g_esp;
        fc_orig_004459B0();
        g_checked++;
        if ((g_eax & 0xFFu) != 0 || g_esp != esp + 12) {
            if (g_differed++ < 8)
                fprintf(stderr, "[hot] 0x004459B0 differs: here no, the original %u (distances %.9g, %.9g)\n",
                        (unsigned)(g_eax & 0xFFu), (double)da, (double)db);
        }
        if (g_checked % 100000ul == 0)
            fprintf(stderr, "[hot] 0x004459B0: %lu early answers checked, %lu differed\n", g_checked, g_differed);
        g_esp = esp;
    }
    g_eax = 0;
    g_esp += 12;                                    /* the return address and the two arguments */
}
