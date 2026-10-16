/*
 * hot.c -- functions of the game's own that a console's profile puts at the
 * top, done by the host on the path most of their calls take
 * (docs/bringup.md, 26).
 *
 * Each is in run_lift.py's WRAPS: the lift is kept as fc_orig_<address> and
 * runs for every case not answered here. What is answered here is only the
 * common, simple path, with the same arithmetic in the same order, so that
 * the answer is the original's to the bit, eax included. FC_HOT_CHECK=1
 * (--hot-check) runs the original beside each answer and says where they
 * differ (the first few of them, and a count every so often); FC_HOT_OFF=1
 * (--no-hot) leaves every call to the original.
 *
 * The collision tests below are called for each face of each shape near
 * anyone moving (some 12,700 calls a frame of the plane test alone, walking
 * about the busy Grancel fair), and nearly all of them answer no. What an
 * original leaves in ecx, edx, the flags and the SSE registers on the way is
 * scratch to its callers; the general registers it keeps are kept here too.
 *
 *   0x004459B0  thiscall bool (plane *this, const float a[3], const float b[3]),
 *               ret 8: whether the segment from a to b meets the plane. Each
 *               end's distance is (n.y*y + x*n.x) + n.z*z - d in single
 *               precision (the normal at +0x24, d at +0x58), as the original's
 *               SSE has it; the segment meets the plane when both distances
 *               are finite and one is at or below zero while the other is at or
 *               above it. Only then does the original go on to work out where
 *               (0x00444920, 0x00444CF0), and only then is it run. Its early
 *               answers leave eax 0.
 *   0x00442820  thiscall bool (shape *this, a, b), ret 8: whether the segment
 *               meets a shape of twelve faces (planes at +0x18, 0x68 apart):
 *               yes at once if either end is inside the six faces 0, 2 .. 10
 *               (no distance above zero), else whether it meets any of the
 *               twelve, by 0x004459B0. Done here whole.
 *   0x00446D60  thiscall bool (triangle *this, a, b, float hit[3]), ret 0xC: a
 *               raycast down onto a triangle, keeping the highest hit. The
 *               segment and the triangle's plane first, as 0x004459B0 has them
 *               (but without the test for finite distances, which the compares
 *               do for it); where they do not meet, no, with eax what the
 *               original leaves there (b, its low byte cleared). Where they
 *               do, the original.
 *   0x00443A20  MeshProbe and 0x00443AF0: the octree walks that call those two,
 *               done here whole with them (below, at the functions); MeshTest
 *               0x00443940 and its shape test 0x00442730, the same for a sphere
 *               about the segment, with the game's own sphere-and-triangle test.
 *   0x00465400  ShaderConstants_Set: the constants asked for are those set
 *               already (below).
 *   0x00467740  thiscall bool (object *this, const float m[16], const vertex
 *               in[3] (0x28 each), const material *mat, int order, short
 *               flags), ret 0x14: one of the triangles the game sorts by depth
 *               and draws itself (banners, flags, leaves: some thousands a
 *               frame in the fair). A record in the draw list; each corner
 *               swayed by the wind if the material is of kind 6, put through
 *               m times the view (0x0057D3A8) by D3DXMatrixTranslation and
 *               D3DXMatrixMultiply, divided by w and tried against the screen;
 *               then, unless all three are off it, each corner's colour: fog
 *               from its depth, the material's colour scales, the light near
 *               the player if there is one, the screen's brightness; and a key
 *               of its depth for the sort. Done here whole, with the host's own
 *               D3DX arithmetic (hle_d3dx_matrix_multiply) and the game's own
 *               sinf (0x00524D15, lifted) so that every bit comes out as the
 *               calls would make it; where D3DX is a real DLL (the Windows
 *               host), the original runs. With FC_HOT_CHECK the memory it
 *               writes is compared, not only eax.
 */
#if defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")   /* a multiply and an add stay two roundings, as SSE does them */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

void fc_orig_004459B0(void);
void fc_orig_00442820(void);
void fc_orig_00446D60(void);
void fc_orig_00443A20(void);
void fc_orig_00443AF0(void);
void fc_orig_00442730(void);
void fc_orig_00443940(void);
void sub_00446A60(void);

static float guest_float(uint32_t at) {
    float f;
    memcpy(&f, (const void*)ADDR(at), 4);
    return f;
}
static uint32_t get32(uint32_t at) { uint32_t v; memcpy(&v, (const void*)ADDR(at), 4); return v; }
static void put32(uint32_t at, uint32_t v) { memcpy((void*)ADDR(at), &v, 4); }
static int finite_float(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}

static int g_check = -1, g_off = -1;
static int checking(void) {
    if (g_check < 0) g_check = getenv("FC_HOT_CHECK") && atoi(getenv("FC_HOT_CHECK")) > 0;
    return g_check;
}
static int off(void) {
    if (g_off < 0) g_off = getenv("FC_HOT_OFF") && atoi(getenv("FC_HOT_OFF")) > 0;
    return g_off;
}

/* With FC_HOT_CHECK: the original run from the state the call came with
 * (ecx and the stack), its eax and what it took off the stack compared with
 * what is answered here, and the state put back for the answer. */
typedef struct { const char* name; unsigned long checked, differed; } hot_check;
static void check_with(hot_check* c, void (*orig)(void), uint32_t ecx, uint32_t eax, uint32_t pops) {
    uint32_t esp = g_esp, cur = g_cur_func;
    g_ecx = ecx;
    orig();
    g_cur_func = cur;
    c->checked++;
    if (g_eax != eax || g_esp != esp + pops) {
        if (c->differed++ < 8)
            fprintf(stderr, "[hot] %s differs: eax %08X here, %08X the original's; %u taken off the stack, %u by the original\n",
                    c->name, (unsigned)eax, (unsigned)g_eax, (unsigned)pops, (unsigned)(g_esp - esp));
    }
    if (c->checked % 100000ul == 0)
        fprintf(stderr, "[hot] %s: %lu answers checked, %lu differed\n", c->name, c->checked, c->differed);
    g_esp = esp;
}

/* The distance of the point at p from the plane at pl, as 0x004459B0 has it
 * (and 0x00442820, and 0x00446D60, whose orders of the same sums and
 * products give the same bits). */
static float plane_distance(uint32_t pl, uint32_t p) {
    float t = guest_float(pl + 0x28) * guest_float(p + 4);
    t = t + guest_float(p) * guest_float(pl + 0x24);
    t = t + guest_float(pl + 0x2C) * guest_float(p + 8);
    return t - guest_float(pl + 0x58);
}
static int straddles(float da, float db) { return (da >= 0.0f && db <= 0.0f) || (da <= 0.0f && db >= 0.0f); }

/* An original called from here as the game would call it: with its stack
 * where it would be (at, the place of the return address, which is what the
 * game's caller's frame makes it: an original's answer can carry an address
 * of that stack in eax's upper bits), its arguments above, ecx; the stack the
 * host's own caller has given back afterwards. */
static uint32_t call_original(void (*fn)(void), uint32_t ecx, uint32_t at, int n, const uint32_t* args) {
    uint32_t esp = g_esp, cur = g_cur_func;
    MEM32(at) = RECOMP_RETADDR;
    for (int i = 0; i < n; i++) MEM32(at + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = at;
    g_ecx = ecx;
    fn();
    g_cur_func = cur;
    g_esp = esp;
    return g_eax;
}

/* 0x004459B0 for a caller here: whether the segment meets the plane, and the
 * eax the original would leave. Where it does, the original, its stack at at. */
static int segment_meets_plane(uint32_t plane, uint32_t a, uint32_t b, uint32_t* eax, uint32_t at) {
    float da = plane_distance(plane, a), db = plane_distance(plane, b);
    if (!(finite_float(da) && finite_float(db) && straddles(da, db))) { *eax = 0; return 0; }
    uint32_t args[2] = { a, b };
    *eax = call_original(fc_orig_004459B0, plane, at, 2, args);
    return (*eax & 0xFFu) != 0;
}

void fc_wrap_004459B0(void) {
    static hot_check c = { "0x004459B0", 0, 0 };
    if (off()) { fc_orig_004459B0(); return; }
    uint32_t pl = g_ecx, a = MEM32(g_esp + 4), b = MEM32(g_esp + 8);
    float da = plane_distance(pl, a), db = plane_distance(pl, b);
    if (finite_float(da) && finite_float(db) && straddles(da, db)) {
        fc_orig_004459B0();                         /* where: the original's */
        return;
    }
    if (checking()) check_with(&c, fc_orig_004459B0, pl, 0, 12);
    g_eax = 0;
    g_esp += 12;                                    /* the return address and the two arguments */
}

/* 0x00442820, and with r the radius 0x00442730 (which has its faces'
 * distances against r, handed over in xmm3, where 0x00442820 has 0): the eax
 * it leaves, its low byte the answer. */
static uint32_t shape_test_within(uint32_t shape, uint32_t a, uint32_t b, float r, uint32_t esp) {
    uint32_t eax;
    int i;
    for (i = 0; i < 12; i += 2) if (plane_distance(shape + 0x18 + 0x68u * (uint32_t)i, a) > r) break;
    if (i == 12) return ((shape + 0x44 + 6 * 0xD0) & 0xFFFFFF00u) | 1u;  /* the walk's pointer, al 1 */
    for (i = 0; i < 12; i += 2) if (plane_distance(shape + 0x18 + 0x68u * (uint32_t)i, b) > r) break;
    if (i == 12) return 1u;                                               /* the walk's count 12, al 1 */
    eax = 0;
    for (i = 0; i < 12; i++)
        if (segment_meets_plane(shape + 0x18 + 0x68u * (uint32_t)i, a, b, &eax, esp - 32)) return (eax & 0xFFFFFF00u) | 1u;
    return eax;
}
/* esp: where its return address is, as with every function done here; it
 * saves five registers, then pushes the plane test's two arguments. */
static uint32_t shape_test(uint32_t shape, uint32_t a, uint32_t b, uint32_t esp) { return shape_test_within(shape, a, b, 0.0f, esp); }
void fc_wrap_00442730(void) {
    static hot_check c = { "0x00442730", 0, 0 };
    if (off()) { fc_orig_00442730(); return; }
    xmm_t radius = g_xmm[3];                        /* the plane test may be the original's, which uses xmm3 too */
    uint32_t shape = g_ecx, eax = shape_test_within(shape, MEM32(g_esp + 4), MEM32(g_esp + 8), radius.f32[0], g_esp);
    if (checking()) { g_xmm[3] = radius; check_with(&c, fc_orig_00442730, shape, eax, 12); }
    g_eax = eax;
    g_esp += 12;
}
void fc_wrap_00442820(void) {
    static hot_check c = { "0x00442820", 0, 0 };
    if (off()) { fc_orig_00442820(); return; }
    uint32_t shape = g_ecx, eax = shape_test(shape, MEM32(g_esp + 4), MEM32(g_esp + 8), g_esp);
    if (checking()) check_with(&c, fc_orig_00442820, shape, eax, 12);
    g_eax = eax;
    g_esp += 12;
}

/* 0x00446D60: the eax it leaves. Where the segment reaches the triangle's
 * plane, the original, its stack at at. */
static uint32_t raycast(uint32_t tri, uint32_t a, uint32_t b, uint32_t hit, uint32_t at) {
    if (!straddles(plane_distance(tri, a), plane_distance(tri, b))) return b & 0xFFFFFF00u;
    uint32_t args[3] = { a, b, hit };
    return call_original(fc_orig_00446D60, tri, at, 3, args);
}
void fc_wrap_00446D60(void) {
    static hot_check c = { "0x00446D60", 0, 0 };
    if (off()) { fc_orig_00446D60(); return; }
    uint32_t tri = g_ecx, a = MEM32(g_esp + 4), b = MEM32(g_esp + 8);
    if (straddles(plane_distance(tri, a), plane_distance(tri, b))) {
        fc_orig_00446D60();                         /* it may hit: the original's */
        return;
    }
    uint32_t eax = b & 0xFFFFFF00u;
    if (checking()) check_with(&c, fc_orig_00446D60, tri, eax, 16);
    g_eax = eax;
    g_esp += 16;                                    /* the return address and the three arguments */
}

/* With FC_HOT_CHECK, for the walks: the hit kept before, the answer and the
 * hit found here kept, the hit put back for the original, and both compared
 * after it has run (the nodes each visits are marked the same by both). */
static void check_walk(hot_check* c, void (*orig)(void), uint32_t node, uint32_t eax, uint32_t pops, uint32_t hit, const uint8_t was[12]) {
    uint8_t mine[12];
    memcpy(mine, (const void*)ADDR(hit), 12);
    memcpy((void*)ADDR(hit), was, 12);
    check_with(c, orig, node, eax, pops);
    if (memcmp(mine, (const void*)ADDR(hit), 12) && c->differed++ < 8)
        fprintf(stderr, "[hot] %s differs in the hit it found\n", c->name);
}

/* MeshProbe 0x00443A20, thiscall bool (node *this, a, b, float hit[3]), ret 0xC:
 * the octree under a node walked for the highest hit of the segment a-b on
 * its triangles: a node's box (+0x18) by 0x00442820, then its eight children
 * (+8, 0x510 each) or its triangles (+0x10, +0x14 of them, 0x68 each) by
 * 0x00446D60; +4 is the last triangle hit. eax as the original leaves it. */
static uint32_t mesh_probe(uint32_t node, uint32_t a, uint32_t b, uint32_t hit, uint32_t eax_in, uint32_t esp) {
    uint32_t bl = 0, eax, frame = (esp - 4) & ~7u;  /* ebp pushed, the stack aligned to 8, four registers pushed */
    put32(node + 4, 0);
    if (get32(node + 0x14) == 0 && get32(node + 8) == 0) return eax_in & 0xFFFFFF00u;
    eax = shape_test(node + 0x18, a, b, frame - 28);
    if (!(eax & 0xFFu)) return eax & 0xFFFFFF00u;
    if (get32(node + 8) == 0) {
        uint32_t n = get32(node + 0x14), i;
        if (n == 0) return eax & 0xFFFFFF00u;
        for (i = 0; i < n; i++)
            if (raycast(get32(node + 0x10) + i * 0x68, a, b, hit, frame - 32) & 0xFFu) bl = 1, put32(node + 4, i);
        return ((n * 0x68) & 0xFFFFFF00u) | bl;
    }
    for (uint32_t k = 0; k < 0x2880; k += 0x510)
        if (mesh_probe(get32(node + 8) + k, a, b, hit, 1, frame - 32) & 0xFFu) bl = 1;
    return bl;                                      /* eax 1 from the last round, its low byte bl */
}
void fc_wrap_00443A20(void) {
    static hot_check c = { "0x00443A20", 0, 0 };
    if (off()) { fc_orig_00443A20(); return; }
    uint32_t node = g_ecx, a = get32(g_esp + 4), b = get32(g_esp + 8), hit = get32(g_esp + 12);
    uint8_t was[12];
    if (checking()) memcpy(was, (const void*)ADDR(hit), 12);
    uint32_t eax = mesh_probe(node, a, b, hit, g_eax, g_esp);
    if (checking()) check_walk(&c, fc_orig_00443A20, node, eax, 16, hit, was);
    g_eax = eax;
    g_esp += 16;
}

/* 0x00446A60, the game's own: whether a sphere of radius r (in xmm3, as a
 * movss leaves it) about the segment a-b touches a triangle. */
static uint32_t sphere_triangle(uint32_t tri, uint32_t a, uint32_t b, float r, uint32_t at) {
    uint32_t args[2] = { a, b };
    memset(&g_xmm[3], 0, sizeof g_xmm[3]);
    g_xmm[3].f32[0] = r;
    return call_original(sub_00446A60, tri, at, 2, args);
}
/* MeshTest 0x00443940, thiscall bool (node *this, a, b), the radius r in xmm3,
 * ret 8: whether the sphere about the segment touches any triangle under the
 * node: its box by 0x00442730, its eight children, or its triangles by
 * 0x00446A60, the first that does marked at +4. */
static uint32_t mesh_test(uint32_t node, uint32_t a, uint32_t b, float r, uint32_t eax_in, uint32_t esp) {
    uint32_t frame = (esp - 4) & ~7u;               /* ebp pushed, the stack aligned to 8, 12 bytes and three registers */
    put32(node + 4, 0);
    if (get32(node + 0x14) == 0 && get32(node + 8) == 0) return eax_in & 0xFFFFFF00u;
    uint32_t eax = shape_test_within(node + 0x18, a, b, r, frame - 36);
    if (!(eax & 0xFFu)) return eax & 0xFFFFFF00u;
    if (get32(node + 8) == 0) {
        uint32_t n = get32(node + 0x14), t = get32(node + 0x10), i;
        for (i = 0; i < n; i++)
            if ((eax = sphere_triangle(t + i * 0x68, a, b, r, frame - 36)) & 0xFFu) { put32(node + 4, i); return (eax & 0xFFFFFF00u) | 1u; }
        return eax & 0xFFFFFF00u;
    }
    for (uint32_t k = 0; k < 0x2880; k += 0x510)
        if ((eax = mesh_test(get32(node + 8) + k, a, b, r, eax, frame - 36)) & 0xFFu) return (eax & 0xFFFFFF00u) | 1u;
    return eax & 0xFFFFFF00u;
}
void fc_wrap_00443940(void) {
    static hot_check c = { "0x00443940", 0, 0 };
    if (off()) { fc_orig_00443940(); return; }
    uint32_t node = g_ecx, a = get32(g_esp + 4), b = get32(g_esp + 8);
    xmm_t radius = g_xmm[3];
    uint32_t eax = mesh_test(node, a, b, radius.f32[0], g_eax, g_esp);
    if (checking()) { g_xmm[3] = radius; check_with(&c, fc_orig_00443940, node, eax, 12); }
    g_eax = eax;
    g_esp += 12;
}

/* 0x00443AF0, thiscall node* (node *this, x, a, b, float hit[3], node *skip[8]),
 * ret 0x14: the first node under this one whose triangles the segment hits,
 * leaving out the eight in skip; each child it goes into is told its parent
 * (+0xC). x is only handed down. */
static uint32_t mesh_find(uint32_t node, uint32_t x, uint32_t a, uint32_t b, uint32_t hit, uint32_t skip, uint32_t esp) {
    /* four registers pushed */
    put32(node + 4, 0);
    if (get32(node + 0x14) == 0 && get32(node + 8) == 0) return 0;
    for (uint32_t k = 0; k < 8; k++) if (get32(skip + 4 * k) == node) return 0;
    if (!(shape_test(node + 0x18, a, b, esp - 28) & 0xFFu)) return 0;
    if (get32(node + 8) == 0) {
        uint32_t n = get32(node + 0x14), i;
        for (i = 0; i < n; i++)
            if (raycast(get32(node + 0x10) + i * 0x68, a, b, hit, esp - 32) & 0xFFu) { put32(node + 4, i); return node; }
        return 0;
    }
    for (uint32_t k = 0; k < 0x2880; k += 0x510) {
        uint32_t child = get32(node + 8) + k, found;
        put32(child + 0xC, node);
        if ((found = mesh_find(child, x, a, b, hit, skip, esp - 40)) != 0) return found;
    }
    return 0;
}
void fc_wrap_00443AF0(void) {
    static hot_check c = { "0x00443AF0", 0, 0 };
    if (off()) { fc_orig_00443AF0(); return; }
    uint32_t node = g_ecx, x = get32(g_esp + 4), a = get32(g_esp + 8), b = get32(g_esp + 12), hit = get32(g_esp + 16);
    uint32_t skip = get32(g_esp + 20);
    uint8_t was[12];
    if (checking()) memcpy(was, (const void*)ADDR(hit), 12);
    uint32_t eax = mesh_find(node, x, a, b, hit, skip, g_esp);
    if (checking()) check_walk(&c, fc_orig_00443AF0, node, eax, 24, hit, was);
    g_eax = eax;
    g_esp += 24;
}

/* ---- 0x00465400 ShaderConstants_Set ------------------------------------------------
 * (first register in ecx, the values in edx, the count on the stack, taken off
 * by the caller): the game keeps the vertex shader constants it last set
 * (0x01383D90, 16 bytes a register; 0x0079A750 a byte a register for "kept"),
 * and only where one of those it is asked to set is not kept, or differs (as
 * ucomiss says: a number not a number is never the same), copies them all
 * and calls SetVertexShaderConstantF, whose answer it returns. Most draws ask
 * for what is already there; that answer, 0, is given here. */
void fc_orig_00465400(void);
static int constants_kept(uint32_t reg, uint32_t data, uint32_t n) {
    for (uint32_t i = n; i-- > 0;) {
        if (MEM8(0x0079A750 + reg + i) == 0) return 0;
        uint32_t kept = 0x01383D90 + (reg + i) * 16, now = data + i * 16;
        for (uint32_t k = 0; k < 16; k += 4)
            if (!(guest_float(kept + k) == guest_float(now + k))) return 0;
    }
    return 1;
}
void fc_wrap_00465400(void) {
    static hot_check c = { "0x00465400", 0, 0 };
    if (off()) { fc_orig_00465400(); return; }
    uint32_t reg = g_ecx, data = g_edx, n = get32(g_esp + 4);
    if (n != 0 && !constants_kept(reg, data, n)) { fc_orig_00465400(); return; }
    if (checking()) { g_edx = data; check_with(&c, fc_orig_00465400, reg, 0, 4); }
    g_eax = 0;
    g_esp += 4;                                     /* the return address only */
}

/* ---- 0x00467740: a depth-sorted triangle ------------------------------------------ */
void fc_orig_00467740(void);
#if defined(HLE_WITH_D3D9)
void hle_d3dx_matrix_multiply(float* r, const float* a, const float* b);
void sub_00524D15(void);                            /* the game's sinf, SSE2: xmm0 in, xmm0 out */

static void put_float(uint32_t at, float f) { memcpy((void*)ADDR(at), &f, 4); }
/* cvttss2si: truncated, and 0x80000000 for what does not fit or is not a number */
static int32_t cvtt(float f) { return f >= -2147483648.0f && f < 2147483648.0f ? (int32_t)f : INT32_MIN; }
/* the compiler's helper for (__int64)float, 0x00522510, as the lift has it inlined: its low half */
static uint32_t to_int64_low(float f) {
    int64_t v = INT64_MIN;
    if (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) v = (int64_t)f;
    return (uint32_t)(uint64_t)v;
}
/* an unsigned 32-bit value as the game makes it a float: to double exactly (cvtdq2pd and 2^32 when the
   top bit is set, the table at 0x00548470), then rounded once */
static float from_unsigned(uint32_t u) { return (float)(double)u; }
static float game_sinf(float x) {
    uint32_t cur = g_cur_func;
    memset(&g_xmm[0], 0, sizeof g_xmm[0]);
    g_xmm[0].f32[0] = x;                            /* as a movss leaves it: the other lanes 0 */
    g_esp -= 4;
    MEM32(g_esp) = RECOMP_RETADDR;
    sub_00524D15();                                 /* its ret takes the return address off */
    g_cur_func = cur;
    return g_xmm[0].f32[0];
}

static uint32_t sorted_triangle(uint32_t self, uint32_t m, uint32_t in, uint32_t mat, uint32_t order, uint32_t flags) {
    if (order == 0xFFFFFFFFu) put32(0x005B7A80, order = get32(0x005B7A80) + 1);
    uint32_t idx = get32(0x005B7A7C), rec = 0x005BFA84 + idx * 24;
    MEM16(rec + 8) = 0;
    MEM16(rec + 0xC) = (uint16_t)flags;
    put32(rec + 4, 0x1C4);
    MEM16(rec + 0xE) = 0x104;
    MEM8(rec + 0x10) = 3;
    put32(rec, self);
    MEM16(rec + 0xA) = (uint16_t)order;
    put32(rec + 0x14, get32(0x005B7A7C));

    float mv[16];                                   /* m times the view */
    hle_d3dx_matrix_multiply(mv, (const float*)ADDR(m), (const float*)ADDR(0x0057D3A8));
    uint32_t cam = get32(0x0191400C), kind = get32(mat + 0x14C);
    float zfar = guest_float(cam + 8), znear = guest_float(cam + 4), zrange = zfar - znear;
    float high = (float)(int32_t)MEM8(mat + 0x14A) * guest_float(0x00546934);
    float wave = from_unsigned(get32(mat + 0x160)) * guest_float(0x00546910);
    uint32_t vert = get32(0x005EFA8C) + idx * 0x60 + 0xC;
    int off_screen = 0;
    for (int c = 0; c < 3; c++) {
        uint32_t v = in + 8 + 0x28u * (uint32_t)c, o = vert + 0x20u * (uint32_t)c;
        float x = guest_float(v - 8), y = guest_float(v - 4), z = guest_float(v);
        if (kind == 6 && !(guest_float(mat + 0x150) > guest_float(v + 0x18) && guest_float(mat + 0x158) > guest_float(v + 0x1C))) {
            float k = guest_float(mat + 0x154);
            float kx = k * guest_float(v - 8);
            float fx = kx - (float)cvtt(kx);
            float kz = guest_float(v) * k;
            float phase = ((guest_float(0x007C0630) * guest_float(0x00546C28)) * guest_float(0x0054694C)) + fx * guest_float(0x00546F04);
            float s = game_sinf(phase);
            float h = (high - guest_float(v + 0x1C)) * wave;
            float fz = kz - (float)cvtt(kz);
            float sway = s * h;
            x = sway * fx + guest_float(v - 8);
            z = fz * sway + guest_float(v);
        }
        float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };   /* D3DXMatrixTranslation */
        hle_d3dx_matrix_multiply(t, t, mv);
        float rhw = guest_float(0x00546ABC) / t[15];
        float sx = rhw * t[12], sy = rhw * t[13], sz = rhw * t[14];
        put_float(o, rhw);
        put_float(o - 0xC, sx);
        put_float(o - 8, sy);
        put_float(o - 4, sz);
        if (0.0f >= sz || sz >= guest_float(0x005B7A68) || guest_float(0x005B7A6C) >= sx || sx >= guest_float(0x005B7A70) ||
            guest_float(0x005B7A74) >= sy || sy >= guest_float(0x005B7A78))
            off_screen++;
    }
    if (off_screen == 3) return 1;                  /* eax 3, its low byte made 1 */

    float scale_r = guest_float(0x006B9580), scale_g = guest_float(0x006B9584), scale_b = guest_float(0x006B9588);
    float scale_a = guest_float(0x006B958C), one = guest_float(0x00546ABC), depth = 0.0f;
    vert = get32(0x005EFA8C) + get32(0x005B7A7C) * 0x60 + 0xC;
    for (int c = 0; c < 3; c++) {
        uint32_t src = in + 0x1C + 0x28u * (uint32_t)c, o = vert + 0x20u * (uint32_t)c;
        float z = guest_float(o - 4) / guest_float(o), fog;
        if (znear >= z) fog = one;
        else if (z >= zfar) fog = 0.0f;
        else fog = (zfar - z) / zrange;
        uint32_t col = get32(src - 4);
        fog = fog * guest_float(0x005470B4);
        uint32_t a = (uint32_t)cvtt(from_unsigned(col >> 24) * scale_a) & 0xFFu;
        uint32_t r = (uint32_t)cvtt(from_unsigned((col >> 16) & 0xFFu) * scale_r) & 0xFFu;
        uint32_t g = (uint32_t)cvtt(from_unsigned((col >> 8) & 0xFFu) * scale_g) & 0xFFu;
        uint32_t b = (uint32_t)cvtt(from_unsigned(col & 0xFFu) * scale_b) & 0xFFu;
        uint32_t spec = (to_int64_low(fog) << 24) + (get32(src) & 0xFFFFFFu);
        if (MEM8(0x01674BE4) & 4) {                 /* a light near the player */
            float lx, ly, lz, strength = guest_float(0x0169B280);
            if (MEM8(0x0169B27F) == 0) {
                uint32_t e = (uint32_t)MEM16(0x0169B27C) * 0xCC;
                lx = guest_float(e + 0x01674E4C), ly = guest_float(e + 0x01674E50), lz = guest_float(e + 0x01674E54);
            } else {
                uint32_t p = get32(0x0057CD90);
                lx = guest_float(p + 0x10), ly = guest_float(p + 0x14) + one, lz = guest_float(p + 0x18);
            }
            float dx = guest_float(src - 0x1C) - lx, dz = guest_float(src - 0x14) - lz, dy = guest_float(src - 0x18) - ly;
            float d2 = (dy * dy + dx * dx) + dz * dz;
            float inv = one / sqrtf(d2), inv2 = inv * inv;
            uint32_t add = to_int64_low(((inv2 * inv2) * strength) * guest_float(0x00547128));
            r += add, g += add, b += add;
            if (r > 0xFFu) r = 0xFFu;
            if (g > 0xFFu) g = 0xFFu;
            if (b > 0xFFu) b = 0xFFu;
        }
        float bright = guest_float(0x0079AA28);
        uint32_t out = (to_int64_low(from_unsigned(r) * bright) | a << 8) << 8;
        out = (out | to_int64_low(from_unsigned(g) * bright)) << 8;
        out |= to_int64_low(from_unsigned(b) * bright);
        depth = depth + guest_float(o);
        put32(o + 4, out);
        put32(o + 8, spec);
        put32(o + 0xC, get32(src + 4));
        put32(o + 0x10, get32(src + 8));
    }
    int32_t key = cvtt(depth * guest_float(0x0054717C));
    if (key > 0xEFFF) key = 0xEFFF;
    else if (key < -0x1000) key = -0x1000;
    uint32_t n = get32(0x005B7A7C), entry = ((uint32_t)key + 0x2000u) << 16;
    entry += n;
    put32(0x005B7A84 + n * 4, entry);
    put32(0x005B7A7C, n + 1);
    put32(0x007BD14C, get32(0x007BD14C) + 1);
    return (entry & 0xFFFFFF00u) | 1u;
}

/* What 0x00467740 may write, for FC_HOT_CHECK: kept, and compared after both have run. */
typedef struct { uint32_t at, n; uint8_t was[0x60], native[0x60]; } written;
static int triangle_written(written* w) {
    uint32_t idx = get32(0x005B7A7C);
    w[0].at = 0x005BFA84 + idx * 24, w[0].n = 24;
    w[1].at = 0x005B7A80, w[1].n = 4;
    w[2].at = get32(0x005EFA8C) + idx * 0x60, w[2].n = 0x60;
    w[3].at = 0x005B7A84 + idx * 4, w[3].n = 4;
    w[4].at = 0x005B7A7C, w[4].n = 4;
    w[5].at = 0x007BD14C, w[5].n = 4;
    return 6;
}
#endif

void fc_wrap_00467740(void) {
#if defined(HLE_WITH_D3D9)
    static hot_check c = { "0x00467740", 0, 0 };
    if (off()) { fc_orig_00467740(); return; }
    uint32_t self = g_ecx, esp = g_esp;
    uint32_t m = get32(esp + 4), in = get32(esp + 8), mat = get32(esp + 12), order = get32(esp + 16), flags = MEM16(esp + 20);
    if (!checking()) {
        g_eax = sorted_triangle(self, m, in, mat, order, flags);
        g_esp = esp + 24;                           /* the return address and five arguments */
        return;
    }
    written w[6];
    int n = triangle_written(w);
    for (int i = 0; i < n; i++) memcpy(w[i].was, (const void*)ADDR(w[i].at), w[i].n);
    uint32_t eax = sorted_triangle(self, m, in, mat, order, flags);
    for (int i = 0; i < n; i++) {
        memcpy(w[i].native, (const void*)ADDR(w[i].at), w[i].n);
        memcpy((void*)ADDR(w[i].at), w[i].was, w[i].n);
    }
    g_esp = esp;
    check_with(&c, fc_orig_00467740, self, eax, 24);
    for (int i = 0; i < n; i++)
        if (memcmp(w[i].native, (const void*)ADDR(w[i].at), w[i].n)) {
            c.differed++;
            if (c.differed <= 8) {
                const uint8_t* o = (const uint8_t*)ADDR(w[i].at);
                int k = 0;
                while (k < (int)w[i].n && o[k] == w[i].native[k]) k++;
                fprintf(stderr, "[hot] 0x00467740 differs in what it wrote at %08X+%u: %02X here, %02X the original's\n",
                        (unsigned)w[i].at, (unsigned)k, w[i].native[k], o[k]);
            }
            break;
        }
    g_eax = eax;                                    /* the original's memory stays; the two agreed or it was said */
    g_esp = esp + 24;
#else
    fc_orig_00467740();                             /* D3DX is the real one here: its arithmetic is its own */
#endif
}
