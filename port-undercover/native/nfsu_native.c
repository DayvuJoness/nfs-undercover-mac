/* nfsu_native.c - see nfsu_native.h. Each replacement computes exactly what the instructions do: the same
 * float/double conversions in the same order (this file is built with -ffp-contract=off, so no fused
 * multiply-add), so the results are bit-identical to the translation; NFSU_NATIVE_CHECK=1 runs both and logs any
 * difference. */
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs.h"

void fn_0074a1d0(X86 *c);
void fn_1003cee0(X86 *c);
void fn_004b6e60(X86 *c);
void fn_00780390(X86 *c);
void fn_008251b0(X86 *c);
void fn_006a3060(X86 *c);
void fn_00463280(X86 *c);
void fn_004636d0(X86 *c);
void fn_006cea20(X86 *c);
void fn_007063c0(X86 *c);
void fn_00706380(X86 *c);
void fn_0076d720(X86 *c);
void fn_0082a7d0(X86 *c);
void fn_00541730(X86 *c);
void fn_007674e0(X86 *c);
void fn_00763ef0(X86 *c);

/* FUN_0074a1d0 (__thiscall, 3 stack arguments, RET 0xc): an axis-aligned box (min, max) against six planes;
 * 0 outside, 1 crossing, 2 inside. The main thread calls it for every object every frame (11% of it at 120 fps,
 * 2026-11-16). Replaced here: the case with no transform (argument 3 null) and a plane set (the short at
 * [*this + 0x1e0] non-zero), i.e. the loop at 0074a5fe; everything else is the translation. */
static int box_vs_planes(uint32_t self, uint32_t pmin, uint32_t pmax, uint32_t xform, uint32_t *out) {
    if (xform != 0)
        return 0;
    const uint32_t planes = rd32(self);
    if (rd16(planes + 0x1e0) == 0)
        return 0;
    const double half = rdf64(0x00bd97d0u);
    float ctr[3], ext[3];
    for (int k = 0; k < 3; ++k) /* 0074a208..: (min + max) * half, rounded to float */
        ctr[k] = (float)(((double)rdf32(pmin + 4 * k) + (double)rdf32(pmax + 4 * k)) * half);
    for (int k = 0; k < 3; ++k) /* FUN_00410170 -> FUN_00410100: max - centre */
        ext[k] = (float)((double)rdf32(pmax + 4 * k) - (double)ctr[k]);
    uint32_t crossing = 0;
    for (uint32_t p = planes + 0x188, i = 1; i <= 6; ++i, p += 0x10) {
        const float n0 = rdf32(p - 8), n1 = rdf32(p - 4), n2 = rdf32(p), d = rdf32(p + 4);
        double r = (double)(float)fabs((double)n0) * (double)ext[0];
        r += (double)(float)fabs((double)n1) * (double)ext[1];
        r += (double)(float)fabs((double)n2) * (double)ext[2];
        const float rf = (float)r;
        double s = (double)n0 * (double)ctr[0];
        s += (double)n1 * (double)ctr[1];
        s += (double)n2 * (double)ctr[2];
        s += (double)d;
        const float sf = (float)s;
        if (0.0 > (double)sf + (double)rf) {
            *out = 0;
            return 1;
        }
        if (0.0 > (double)sf - (double)rf)
            crossing = 1;
    }
    *out = crossing ? 1u : 2u;
    return 1;
}

/* The game's small vector helpers, as their listings compute them (each result is a float in memory). */
static void v_add(float d[3], const float a[3], const float b[3]) { /* FUN_004572d0 */
    for (int k = 0; k < 3; ++k)
        d[k] = (float)((double)a[k] + (double)b[k]);
}
static void v_sub(float d[3], const float a[3], const float b[3]) { /* FUN_00410100 */
    for (int k = 0; k < 3; ++k)
        d[k] = (float)((double)a[k] - (double)b[k]);
}
static void v_scale(float d[3], const float a[3], float s) { /* FUN_00457390 */
    for (int k = 0; k < 3; ++k)
        d[k] = (float)((double)a[k] * (double)s);
}
static void v_neg(float d[3], const float a[3]) { /* FUN_00457340: [0x00bd9770] - a, in single precision */
    const float k0 = rdf32(0x00bd9770u);
    const float x = k0 - a[0], y = k0 - a[1], z = k0 - a[2];
    d[0] = x, d[1] = y, d[2] = z;
}
static float v_len(const float a[3]) { /* the length both FUN_004b2420 and FUN_0074a1d0 compute */
    double s = (double)a[0] * (double)a[0];
    s += (double)a[1] * (double)a[1];
    s += (double)a[2] * (double)a[2];
    return (float)sqrt((double)(float)s);
}
static void v_normalize(float d[3], const float a[3]) { /* FUN_004b2420 */
    const float len = v_len(a);
    if (len == 0.0f) { /* UCOMISS equal (not unordered): [0x00bd97b0], 0, 0 */
        d[0] = rdf32(0x00bd97b0u), d[1] = 0.0f, d[2] = 0.0f;
        return;
    }
    const float inv = (float)(rdf64(0x00bd9750u) / (double)len);
    const float x = (float)((double)a[0] * (double)inv), y = (float)((double)a[1] * (double)inv),
                z = (float)((double)a[2] * (double)inv);
    d[0] = x, d[1] = y, d[2] = z;
}
static void v_cross(float d[3], const float a[3], const float b[3]) { /* FUN_004b2960 */
    const float y = (float)((double)b[0] * (double)a[2] - (double)a[0] * (double)b[2]);
    const float z = (float)((double)b[1] * (double)a[0] - (double)a[1] * (double)b[0]);
    const float x = (float)((double)a[1] * (double)b[2] - (double)b[1] * (double)a[2]);
    d[0] = x, d[1] = y, d[2] = z;
}
static void rd_v3(float d[3], uint32_t a) {
    d[0] = rdf32(a), d[1] = rdf32(a + 4), d[2] = rdf32(a + 8);
}
/* The runtime's D3DXVec3Transform (kit/dx/d3dx9.cpp), which FUN_007436f0 calls; xyz kept. */
void recomp_d3dx_vec3_transform(const float v[3], const float m[16], float out[4]);
static void transform_point(float d[3], uint32_t matrix, const float p[3]) {
    float m[16], r[4];
    for (int i = 0; i < 16; ++i)
        m[i] = rdf32(matrix + 4u * (uint32_t)i);
    recomp_d3dx_vec3_transform(p, m, r);
    d[0] = r[0], d[1] = r[1], d[2] = r[2];
}
static float abs_masked(float v) { /* CVTPS2PD, ANDPD [0x00bd9760], CVTSD2SS */
    double x = (double)v;
    uint64_t bits;
    memcpy(&bits, &x, 8);
    bits &= ((uint64_t)rd32(0x00bd9764u) << 32) | rd32(0x00bd9760u);
    memcpy(&x, &bits, 8);
    return (float)x;
}

/* FUN_0074a1d0 with no plane set ([*this + 0x1e0] zero), from 0074a2b8: the box's bounding sphere, pushed along
 * the view's direction and its side, is projected with the view's matrix ([0x00ea0390 + (view - 0x1f) * 64])
 * and its screen extent tested against [0x00d4b3f0]: 0 off screen, 1 crossing, 2 inside. 85% of the calls
 * (~700k a second, 2026-11-17). Only for argument 3 null: with a transform the box centre comes from
 * FUN_00744770, which this does not reproduce (the branch itself never reads it, but the call may have effects). */
/* The bounding sphere of a box (min, max), pushed back along the camera direction (0x00d5ea60) by k_push * r and
 * out along its side by k_reach * r, projected with a view matrix: |x|, |y| of the centre on screen and the
 * projected radius. FUN_0074a1d0's no-plane branch and FUN_00780390 both compute it, instruction for instruction
 * the same apart from their constants (the [S+..] notes are FUN_0074a1d0's frame). */
static void sphere_on_screen(const float bmin[3], const float bmax[3], uint32_t m, uint32_t k_push, uint32_t k_reach,
                             float *ax, float *ay, float *rad) {
    float dir[3], side[3], up[3], t0[3], ctr[3], half[3];
    rd_v3(dir, 0x00d5ea60u);                  /* FUN_00842aa0 -> 0x00d5ea40, FUN_00441fe0 -> +0x20 */
    v_normalize(t0, dir);                     /* [S+0x60] */
    float back[3];
    v_neg(back, t0);                          /* FUN_00457410 -> [S+0x70] */
    up[0] = 0.0f, up[1] = 0.0f, up[2] = rdf32(0x00bd97b0u); /* [S+0x40] */
    v_cross(t0, back, up);                    /* FUN_004c7e80 -> [S+0x60] */
    v_normalize(side, t0);                    /* [S+0x90] */
    float sum[3];
    v_add(sum, bmin, bmax);                   /* [S+0x60] */
    v_scale(ctr, sum, rdf32(0x00bdeb38u));    /* [S+0x20]; FLD/FSTP of a float constant is exact */
    v_sub(half, bmax, ctr);                   /* [S+0x10] */
    const float r = v_len(half);              /* [S+0x3c] */
    const float push = (float)((double)rdf32(k_push) * (double)r);
    float nb[3], nn[3], off[3], p[3];
    v_normalize(nb, back);                    /* [S+0xb0] */
    v_neg(nn, nb);                            /* [S+0x60] */
    v_scale(off, nn, push);                   /* [S+0xc0] */
    v_add(p, ctr, off);                       /* [S+0xa0] -> [S+0x20] */
    const float reach = (float)((double)rdf32(k_reach) * (double)r);
    float q[3];
    v_scale(q, side, reach);                  /* [S+0xa0] -> [S+0x10] */
    float tp[3], tq[3];
    transform_point(tp, m, p);                /* [S+0x80] */
    transform_point(tq, m, q);                /* [S+0x50] */
    const float dx = (float)((double)tq[0] - (double)rdf32(m + 0x30));
    const float dy = (float)((double)tq[1] - (double)rdf32(m + 0x34));
    double s2 = (double)dy * (double)dy;
    s2 += (double)dx * (double)dx;
    s2 += (double)tq[2] * (double)tq[2];
    *rad = (float)sqrt((double)(float)s2);
    *ax = abs_masked(tp[0]);
    *ay = abs_masked(tp[1]);
}

static int box_vs_view(uint32_t self, uint32_t pmin, uint32_t pmax, uint32_t xform, uint32_t *out) {
    if (xform != 0)
        return 0;
    const uint32_t planes = rd32(self);
    if (rd16(planes + 0x1e0) != 0)
        return 0;
    float bmin[3], bmax[3], ax, ay, rad;
    rd_v3(bmin, pmin);
    rd_v3(bmax, pmax);
    const uint32_t m = 0x00ea0390u + ((rd32(self + 4) - 0x1fu) << 6);
    sphere_on_screen(bmin, bmax, m, 0x00d4b3f8u, 0x00d4b3f4u, &ax, &ay, &rad);
    const float hi_x = (float)((double)ax + (double)rad), hi_y = (float)((double)ay + (double)rad);
    const float lo_x = (float)((double)ax - (double)rad), lo_y = (float)((double)ay - (double)rad);
    const double lim = (double)rdf32(0x00d4b3f0u);
    if ((double)lo_x > lim || (double)lo_y > lim)
        *out = 0;
    else if (!(lim > (double)hi_x) || !(lim > (double)hi_y))
        *out = 1;
    else
        *out = 2;
    return 1;
}

void nfsu_bounds_vs_frustum(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL; /* the translation only, for A/B runs */
    }
    if (off) {
        fn_0074a1d0(c);
        return;
    }
    const uint32_t esp = c->r[R_ESP];
    static int stats = -1;
    if (stats < 0)
        stats = getenv("NFSU_NATIVE_STATS") != NULL;
    if (stats) { /* DIAGNOSTIC: which case the calls are, every 5 s */
        static unsigned long n[4];
        static struct timespec last;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!last.tv_sec)
            last = now;
        const uint32_t xf = rd32(esp + 12), planes = rd32(c->r[R_ECX]);
        ++n[xf ? 1 : rd16(planes + 0x1e0) ? 0 : 2];
        ++n[3];
        const double s = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) * 1e-9;
        if (s >= 5.0) {
            fprintf(stderr, "nfsu native: FUN_0074a1d0 calls/s %.0f: planes %.0f, transform %.0f, no planes %.0f\n",
                    n[3] / s, n[0] / s, n[1] / s, n[2] / s);
            n[0] = n[1] = n[2] = n[3] = 0;
            last = now;
        }
    }
    uint32_t result = 0;
    const uint32_t self = c->r[R_ECX], a1 = rd32(esp + 4), a2 = rd32(esp + 8), a3 = rd32(esp + 12);
    if (!box_vs_planes(self, a1, a2, a3, &result) && !box_vs_view(self, a1, a2, a3, &result)) {
        fn_0074a1d0(c);
        return;
    }
    if (check) {
        fn_0074a1d0(c); /* the translation: its frame is below ESP, nothing above it changes */
        static unsigned long calls, bad;
        ++calls;
        if (c->r[R_EAX] != result && bad++ < 50)
            fprintf(stderr, "nfsu native: FUN_0074a1d0 gave %u, the translation %u (call %lu)\n", result,
                    c->r[R_EAX], calls);
        if (calls % 100000 == 0)
            fprintf(stderr, "nfsu native: FUN_0074a1d0 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    c->r[R_EAX] = result;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 16; /* the return address and three arguments */
    recomp_return(c);
}

/* msvcr80.dll's qsort (1003cee0, __cdecl base, num, width, compare). The render thread sorts its draw lists with
 * it every frame, comparing with FUN_00763970: 9% of that thread at 115 fps, a third of it the translated
 * cross-module call to the comparator for every comparison (2026-11-16). For that comparator this runs the same
 * algorithm natively, step for step as the listing has it, so equal keys end up in the same order as before; any
 * other comparator, or a range outside guest memory, goes to the translation. NFSU_NATIVE_OFF=1 keeps the
 * translation only (A/B); NFSU_NATIVE_CHECK=1 sorts a copy natively, runs the translation and logs any byte that
 * differs.
 *
 * FUN_00763970: elements start with two unsigned 64-bit keys (+0, +8), compared in that order: -1, 0 or 1. */
#define DRAW_KEY_CMP 0x00763970u
static int draw_key_cmp(const uint8_t *a, const uint8_t *b) {
    uint64_t a0, a1, b0, b1;
    memcpy(&a0, a, 8);
    memcpy(&a1, a + 8, 8);
    memcpy(&b0, b, 8);
    memcpy(&b1, b + 8, 8);
    if (a0 != b0)
        return a0 > b0 ? 1 : -1;
    if (a1 != b1)
        return a1 > b1 ? 1 : -1;
    return 0;
}
static void elem_swap(uint8_t *a, uint8_t *b, uint32_t width) { /* 1003d190: bytes, nothing when a == b */
    if (a == b)
        return;
    for (uint32_t i = 0; i < width; ++i) {
        const uint8_t t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}
static void short_sort(uint8_t *lo, uint8_t *hi, uint32_t width) { /* 1003ce50: selection of the largest */
    while (hi > lo) {
        uint8_t *max = lo;
        for (uint8_t *p = lo + width; p <= hi; p += width)
            if (draw_key_cmp(p, max) > 0)
                max = p;
        elem_swap(max, hi, width);
        hi -= width;
    }
}
/* 1003cee0 from 1003cf61 on (num >= 2): median of three, partition, the larger part on a 30-entry stack. */
static void draw_qsort(uint8_t *base, uint32_t num, uint32_t width) {
    uint8_t *lostk[30], *histk[30];
    int sp = 0;
    uint8_t *lo = base, *hi = base + (size_t)(num - 1) * width;
    for (;;) {
        const uint32_t size = (uint32_t)((uint32_t)(hi - lo) / width) + 1;
        if (size <= 8) {
            short_sort(lo, hi, width);
        } else {
            uint8_t *mid = lo + (size_t)(size >> 1) * width;
            if (draw_key_cmp(lo, mid) > 0)
                elem_swap(lo, mid, width);
            if (draw_key_cmp(lo, hi) > 0)
                elem_swap(lo, hi, width);
            if (draw_key_cmp(mid, hi) > 0)
                elem_swap(mid, hi, width);
            uint8_t *loguy = lo, *higuy = hi;
            for (;;) {
                if (mid > loguy)
                    do
                        loguy += width;
                    while (loguy < mid && draw_key_cmp(loguy, mid) <= 0);
                if (mid <= loguy)
                    do
                        loguy += width;
                    while (loguy <= hi && draw_key_cmp(loguy, mid) <= 0);
                do
                    higuy -= width;
                while (higuy > mid && draw_key_cmp(higuy, mid) > 0);
                if (higuy < loguy)
                    break;
                elem_swap(loguy, higuy, width);
                if (mid == higuy)
                    mid = loguy;
            }
            higuy += width;
            if (mid < higuy)
                do
                    higuy -= width;
                while (higuy > mid && draw_key_cmp(higuy, mid) == 0);
            if (mid >= higuy)
                do
                    higuy -= width;
                while (higuy > lo && draw_key_cmp(higuy, mid) == 0);
            if ((int32_t)(higuy - lo) >= (int32_t)(hi - loguy)) {
                if (lo < higuy && sp < 30) {
                    lostk[sp] = lo;
                    histk[sp++] = higuy;
                }
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi && sp < 30) {
                    lostk[sp] = loguy;
                    histk[sp++] = hi;
                }
                if (lo < higuy) {
                    hi = higuy;
                    continue;
                }
            }
        }
        if (--sp < 0)
            return;
        lo = lostk[sp];
        hi = histk[sp];
    }
}
void nfsu_qsort(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL;
    }
    const uint32_t esp = c->r[R_ESP];
    const uint32_t base = rd32(esp + 4), num = rd32(esp + 8), width = rd32(esp + 12), comp = rd32(esp + 16);
    const uint64_t bytes = (uint64_t)num * width;
    if (off || comp != DRAW_KEY_CMP || width < 16 || width > 4096 || !base || bytes > GUEST_SIZE ||
        (uint64_t)base + bytes > GUEST_SIZE) {
        fn_1003cee0(c);
        return;
    }
    uint8_t *p = g_mem + base;
    if (num >= 2) {
        uint8_t *mine = check ? (uint8_t *)malloc((size_t)bytes) : NULL;
        if (mine) {
            memcpy(mine, p, (size_t)bytes);
            draw_qsort(mine, num, width);
            fn_1003cee0(c); /* the translation returns to the caller itself */
            static unsigned long calls, bad;
            ++calls;
            if (memcmp(mine, p, (size_t)bytes) != 0 && bad++ < 20)
                fprintf(stderr, "nfsu native: qsort differs from the translation (call %lu, %u x %u bytes)\n", calls,
                        num, width);
            if (calls % 20000 == 0)
                fprintf(stderr, "nfsu native: qsort checked %lu sorts, %lu differ\n", calls, bad);
            free(mine);
            return;
        }
        draw_qsort(p, num, width);
    }
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4; /* __cdecl: the return address only */
    recomp_return(c);
}

/* FUN_004b6e60 (__cdecl sprintf(buf, fmt, ...) through the game's vsnprintf FUN_004b5130, timed by FUN_004b6900).
 * The draw-list routine FUN_0078aaa0 formats "eDataRender: %s" (0x00c22de0) with the pass name into a stack buffer
 * at 0078ab54 for every item it draws - a profiler label left in the release build. Nothing reads it: the buffer
 * is [esp+0x24].. after `push ebx`, and every later stack read of that function is at +0x10..+0x20; EAX's upper
 * bits are rewritten before use. FUN_004b6900's other effects are its own call count and time (0x00d86f6c,
 * 0x00d86f68), which nothing else references. 16% of the render thread (2026-11-17), so that one call is skipped;
 * every other caller gets the game's sprintf. NFSU_NATIVE_OFF=1 formats it as before. */
void nfsu_sprintf(X86 *c) {
    static int off = -1;
    if (off < 0)
        off = getenv("NFSU_NATIVE_OFF") != NULL;
    const uint32_t esp = c->r[R_ESP];
    if (!off && rd32(esp) == 0x0078ab59u) {
        c->eip = 0x0078ab59u;
        c->r[R_ESP] = esp + 4; /* __cdecl: the caller pops its three arguments */
        recomp_return(c);
        return;
    }
    fn_004b6e60(c);
}

/* FUN_00780390 (__cdecl (object, view), returns AL): is the bounding sphere of the box at object+0x20/+0x30 on
 * screen in view `view` (matrix 0x00ea0390 + view * 64)? 1 when |x| - r and |y| - r are both not above
 * [0x00d543c8]. The render thread calls it per object and pass (5% of it, 2026-11-17). EAX's upper 24 bits are
 * what the translation leaves there: the address FUN_007436f0 returned last, its frame's [S+0x30], where S is
 * ESP after `push edi` in a frame aligned down to 16 bytes. */
void nfsu_sphere_visible(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL;
    }
    if (off) {
        fn_00780390(c);
        return;
    }
    const uint32_t esp = c->r[R_ESP];
    const uint32_t obj = rd32(esp + 4), view = rd32(esp + 8);
    float bmin[3], bmax[3], ax, ay, rad;
    rd_v3(bmin, obj + 0x20);
    rd_v3(bmax, obj + 0x30);
    sphere_on_screen(bmin, bmax, 0x00ea0390u + (view << 6), 0x00d543d0u, 0x00d543ccu, &ax, &ay, &rad);
    const float ex = (float)((double)ax - (double)rad), ey = (float)((double)ay - (double)rad);
    const double lim = (double)rdf32(0x00d543c8u);
    const uint32_t visible = ((double)ex > lim || (double)ey > lim) ? 0u : 1u;
    const uint32_t frame = (((esp - 4u) & ~15u) - 0xc0u) + 0x30u;
    const uint32_t eax = (frame & 0xffffff00u) | visible;
    if (check) {
        fn_00780390(c);
        static unsigned long calls, bad;
        ++calls;
        if (c->r[R_EAX] != eax && bad++ < 50)
            fprintf(stderr, "nfsu native: FUN_00780390 gave %08x, the translation %08x (call %lu)\n", eax,
                    c->r[R_EAX], calls);
        if (calls % 100000 == 0)
            fprintf(stderr, "nfsu native: FUN_00780390 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    c->r[R_EAX] = eax;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4; /* __cdecl */
    recomp_return(c);
}

/* FUN_008251b0 (__thiscall occluder, (point, radius), RET 8, AL): does the occluder hide the sphere? Its five planes
 * (normal at +0, a point on it at +0x10, 0x20 apart): hidden when for every plane the float dot product of the
 * normal with point - plane point is not above the radius (COMISD JC: r < dot, or unordered, means visible).
 * FUN_00825b70 calls it for every occluder of the main thread's scenery visibility pass (7% of that thread,
 * 2026-11-17). EAX's upper 24 bits are what the translation leaves: FUN_00410170's result, the frame's [S+0x10]
 * (S = ESP after `push edi` in a frame aligned down to 16 bytes); ECX is the point, as FUN_00410170 leaves it. */
void nfsu_occluder_hides(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL;
    }
    if (off) {
        fn_008251b0(c);
        return;
    }
    const uint32_t esp = c->r[R_ESP], occ = c->r[R_ECX];
    const uint32_t pt = rd32(esp + 4);
    const float r = rdf32(esp + 8);
    float p[3];
    rd_v3(p, pt);
    uint32_t hidden = 1;
    for (uint32_t i = 0; i < 5; ++i) {
        const uint32_t pl = occ + i * 0x20u;
        float q[3], d[3];
        rd_v3(q, pl + 0x10);
        v_sub(d, p, q); /* FUN_00410170 -> FUN_00410100 */
        double dot = (double)rdf32(pl) * (double)d[0];
        dot += (double)rdf32(pl + 4) * (double)d[1];
        dot += (double)rdf32(pl + 8) * (double)d[2];
        const double df = (double)(float)dot, rd = (double)r;
        if (rd < df || df != df || rd != rd) {
            hidden = 0;
            break;
        }
    }
    const uint32_t frame = (((esp - 4u) & ~15u) - 0x20u) + 0x10u;
    const uint32_t eax = (frame & 0xffffff00u) | hidden;
    if (check) {
        fn_008251b0(c);
        static unsigned long calls, bad;
        ++calls;
        if ((c->r[R_EAX] != eax || c->r[R_ECX] != pt) && bad++ < 50)
            fprintf(stderr, "nfsu native: FUN_008251b0 gave %08x, the translation %08x (ecx %08x vs %08x, call %lu)\n",
                    eax, c->r[R_EAX], pt, c->r[R_ECX], calls);
        if (calls % 1000000 == 0)
            fprintf(stderr, "nfsu native: FUN_008251b0 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    c->r[R_EAX] = eax;
    c->r[R_ECX] = pt;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 12; /* the return address and two arguments */
    recomp_return(c);
}

/* FUN_006a3060 (__cdecl (x, lo, hi, out_lo, out_hi, knots, values, second_derivs, count), result in ST0): a cubic
 * spline through (knots[i], values[i]) at t = (x - lo) / (hi - lo), mapped to out_lo + s * (out_hi - out_lo). The
 * bracketing and the weights are SSE doubles rounded to floats; the cubic is x87, every operation rounded as the
 * precision-control field says (fx87, the same helper the translation uses), in the listing's order (006a3188..).
 * The main thread calls it for every animation key it blends while a scene loads (FUN_006a3650; 15% of that
 * thread in the slowest second of a race load, 2026-11-18).
 * Effects reproduced: ST0 (pushed), EAX/ECX/EDX as each path leaves them (LAHF writes AH), the argument slots it
 * stores into ([esp+8] = the result, or h on the zero-width path; [esp+0xc] = the weight B) and the x87 status
 * flags fx87 raises. Not reproduced: XMM0-4 and the scratch below the caller's ESP, which a caller cannot read
 * (scratch registers after a call; stack beneath ESP after RET), and the stale values in the x87 slots it popped.
 * NFSU_SPLINE_OFF=1 uses the translation for this one function only. */
static inline uint32_t lahf_ah(int unordered, double a, double b) {
    if (unordered)
        return 0x47u; /* ZF PF CF and the reserved bit */
    return 0x02u | (a == b ? 0x40u : 0u) | (a < b ? 0x01u : 0u);
}
typedef struct {
    float x, lo, hi, out_lo, out_hi;
    uint32_t knots, vals, d2;
    int32_t n;
} SplineArgs;
static double spline_core(X86 *c, const SplineArgs *g, uint32_t *eax, uint32_t *ecx, uint32_t *edx,
                          uint32_t *slot8, uint32_t *slotc, int *wrote_c) {
    const float x = g->x, lo = g->lo, hi = g->hi;
    const uint32_t vals = g->vals;
    const int32_t n = g->n;
    const double dlo = (double)lo, dhi = (double)hi, dx = (double)x;
    const int un = dlo != dlo || dhi != dhi;
    uint32_t a = (*eax & 0xffff00ffu) | (lahf_ah(un, dlo, dhi) << 8); /* 006a3079 */
    *wrote_c = 0;
    double r;
    if (!un && dlo == dhi) { /* 006a307f */
        r = fx87(c, (double)g->out_lo + (double)g->out_hi);
        r = fx87(c, r * rdf64(0x00bd97d0u));
        *eax = a;
    } else if (dx > dhi) { /* 006a30ab */
        const double olo = (double)g->out_lo, ohi = (double)g->out_hi;
        const double d = fx87(c, ohi - olo);
        const double m = fx87(c, d * (double)rdf32(vals + (uint32_t)n * 4u - 4u));
        r = fx87(c, olo + m);
        *eax = (uint32_t)n;
        *ecx = vals;
    } else if (dlo > dx) { /* 006a30df */
        const double olo = (double)g->out_lo, ohi = (double)g->out_hi;
        const double d = fx87(c, ohi - olo);
        const double m = fx87(c, d * (double)rdf32(vals));
        r = fx87(c, olo + m);
        *eax = a;
        *edx = vals;
    } else {
        const uint32_t knots = g->knots, d2 = g->d2;
        const float t = (float)((dx - dlo) / (dhi - dlo)); /* 006a3101..006a312a */
        const double dt = (double)t;
        int32_t ih = n - 1, il = 0;
        if (ih > 1) { /* 006a3130 */
            do {
                const int32_t m = (ih + il) >> 1;
                if ((double)rdf32(knots + (uint32_t)m * 4u) > dt)
                    ih = m;
                else
                    il = m;
                a = (uint32_t)(ih - il);
            } while ((int32_t)a > 1);
        }
        const float k1 = rdf32(knots + (uint32_t)ih * 4u), k0 = rdf32(knots + (uint32_t)il * 4u);
        const float h = (float)((double)k1 - (double)k0);
        *ecx = (uint32_t)ih;
        *edx = (uint32_t)il;
        if (h == 0.0f) { /* 006a3182 (UCOMISS with 0: ordered and equal) */
            *eax = (a & 0xffff00ffu) | (0x42u << 8);
            uint32_t hb;
            memcpy(&hb, &h, 4);
            *slot8 = hb;
            return 0.0;
        }
        const double dh = (double)h;
        const float A = (float)((dt - (double)k0) / dh); /* 006a31a7..006a31b2 */
        const float B = (float)(((double)k1 - dt) / dh); /* 006a3190..006a31c3 */
        memcpy(slotc, &B, 4);
        *wrote_c = 1;
        const double dA = (double)A, dB = (double)B;
        const double B2 = fx87(c, dB * dB), B3 = fx87(c, B2 * dB), B3m = fx87(c, B3 - dB);
        const double P = fx87(c, B3m * (double)rdf32(d2 + (uint32_t)il * 4u));
        const double A2 = fx87(c, dA * dA), A3 = fx87(c, A2 * dA), A3m = fx87(c, A3 - dA);
        const double Q = fx87(c, A3m * (double)rdf32(d2 + (uint32_t)ih * 4u));
        const double PQ = fx87(c, Q + P);
        const double h2 = fx87(c, dh * dh);
        const double R = fx87(c, h2 * PQ);
        const double R6 = fx87(c, fdivz(c, R, rdf64(0x00bda198u)));
        const double Bv = fx87(c, dB * (double)rdf32(vals + (uint32_t)il * 4u));
        const double Av = fx87(c, dA * (double)rdf32(vals + (uint32_t)ih * 4u));
        const double S1 = fx87(c, Av + Bv);
        const double S2 = fx87(c, R6 + S1);
        const double olo = (double)g->out_lo, ohi = (double)g->out_hi;
        const double D = fx87(c, ohi - olo);
        const double DS = fx87(c, D * S2);
        r = fx87(c, olo + DS);
        *eax = vals;
    }
    const float f = fto_float(c, r);
    memcpy(slot8, &f, 4);
    return (double)f;
}
static void spline_eval(X86 *c, uint32_t E, uint32_t *eax, uint32_t *ecx, uint32_t *edx, uint32_t *slot8,
                        uint32_t *slotc, int *wrote_c) {
    const SplineArgs g = {rdf32(E + 4),   rdf32(E + 8),   rdf32(E + 0xc), rdf32(E + 0x10),         rdf32(E + 0x14),
                          rd32(E + 0x18), rd32(E + 0x1c), rd32(E + 0x20), (int32_t)rd32(E + 0x24)};
    fpush(c, spline_core(c, &g, eax, ecx, edx, slot8, slotc, wrote_c));
}
void nfsu_spline(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_SPLINE_OFF") != NULL;
    }
    if (off) {
        fn_006a3060(c);
        return;
    }
    const uint32_t E = c->r[R_ESP];
    uint32_t eax = c->r[R_EAX], ecx = c->r[R_ECX], edx = c->r[R_EDX], slot8 = 0, slotc = 0;
    int wrote_c;
    if (check) {
        const uint16_t sw0 = c->fpu_sw;
        spline_eval(c, E, &eax, &ecx, &edx, &slot8, &slotc, &wrote_c);
        const double mine = ST(c, 0);
        const uint16_t sw1 = c->fpu_sw;
        fdrop(c);
        c->fpu_sw = sw0;
        fn_006a3060(c);
        static unsigned long calls, bad;
        ++calls;
        uint64_t mb, tb;
        const double theirs = ST(c, 0);
        memcpy(&mb, &mine, 8);
        memcpy(&tb, &theirs, 8);
        if ((mb != tb || sw1 != c->fpu_sw || eax != c->r[R_EAX] || ecx != c->r[R_ECX] || edx != c->r[R_EDX] ||
             slot8 != rd32(E + 8) || (wrote_c && slotc != rd32(E + 0xc))) &&
            bad++ < 50)
            fprintf(stderr,
                    "nfsu native: FUN_006a3060 gave %016llx sw %04x eax %08x ecx %08x edx %08x [8] %08x [c] %08x, the "
                    "translation %016llx sw %04x eax %08x ecx %08x edx %08x [8] %08x [c] %08x (call %lu)\n",
                    (unsigned long long)mb, sw1, eax, ecx, edx, slot8, slotc, (unsigned long long)tb, c->fpu_sw,
                    c->r[R_EAX], c->r[R_ECX], c->r[R_EDX], rd32(E + 8), rd32(E + 0xc), calls);
        if (calls % 200000 == 0)
            fprintf(stderr, "nfsu native: FUN_006a3060 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    spline_eval(c, E, &eax, &ecx, &edx, &slot8, &slotc, &wrote_c);
    wr32(E + 8, slot8);
    if (wrote_c)
        wr32(E + 0xc, slotc);
    c->r[R_EAX] = eax;
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = edx;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 4; /* __cdecl */
    recomp_return(c);
}

/* FUN_00463280 (__thiscall table, (key), RET 4, EAX): the slot of `key` in an open-addressed hash table of
 * 12-byte entries (key at +0, probe length at +0xa, "used" bit 7 at +0xb), or the slot count when absent. Table:
 * entries at +0, slot count (u16) at +4, entry count (u16) at +6, rotate amount at +0xb. The attribute lookups
 * of FUN_004636d0 and the game's other attribute readers call it; with FUN_004636d0 5% of the main thread while a
 * scene loads (2026-11-18). Each probe is a 32-bit DIV in the translation. ECX and EDX end as the listing leaves
 * them (slot * 3 and slot; untouched on the empty-table path). Returns 0 when the listing would divide by zero
 * (entries but no slots), for the caller to hand the call to the translation. */
typedef struct {
    uint32_t eax, ecx, edx;
} AttrRegs;
static int attr_slot(uint32_t table, uint32_t key, AttrRegs *r) {
    const uint32_t n = rd16(table + 4);
    if (rd16(table + 6) == 0) { /* 00463287 */
        r->eax = n;
        return 1;
    }
    if (n == 0)
        return 0;
    const uint32_t ents = rd32(table);
    const unsigned sh = rd8(table + 0xb) & 31u;
    const uint32_t h = sh ? (key << sh) | (key >> (32u - sh)) : key;
    uint32_t slot = h % n;
    const uint32_t probes = rd8(ents + slot * 12u + 0xa);
    for (uint32_t i = 0; i < probes;) {
        const uint32_t e = ents + slot * 12u;
        const uint32_t k = (rd8(e + 0xb) & 0x80u) ? rd32(e) : 0u;
        if (k == key)
            break;
        slot = (slot + 1u) % n;
        ++i;
    }
    const uint32_t e = ents + slot * 12u;
    const uint32_t k = (rd8(e + 0xb) & 0x80u) ? rd32(e) : 0u;
    r->eax = k == key ? slot : n;
    r->ecx = slot * 3u;
    r->edx = slot;
    return 1;
}
static void attr_report(const char *fn, const AttrRegs *m, const AttrRegs *t, uint32_t mo, uint32_t to, int has_out) {
    static unsigned long bad;
    if (bad++ < 50)
        fprintf(stderr, "nfsu native: %s gave eax %08x ecx %08x edx %08x%s, the translation eax %08x ecx %08x edx %08x\n",
                fn, m->eax, m->ecx, m->edx, has_out ? (mo == to ? "" : " (out differs)") : "", t->eax, t->ecx,
                t->edx);
}
void nfsu_attr_slot(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_ATTR_OFF") != NULL;
    }
    const uint32_t esp = c->r[R_ESP];
    AttrRegs m = {c->r[R_EAX], c->r[R_ECX], c->r[R_EDX]};
    if (off || !attr_slot(c->r[R_ECX], rd32(esp + 4), &m)) {
        fn_00463280(c);
        return;
    }
    if (check) {
        fn_00463280(c);
        static unsigned long calls;
        const AttrRegs t = {c->r[R_EAX], c->r[R_ECX], c->r[R_EDX]};
        if (m.eax != t.eax || m.ecx != t.ecx || m.edx != t.edx)
            attr_report("FUN_00463280", &m, &t, 0, 0, 0);
        if (++calls % 1000000 == 0)
            fprintf(stderr, "nfsu native: FUN_00463280 checked %lu calls\n", calls);
        return;
    }
    c->r[R_EAX] = m.eax;
    c->r[R_ECX] = m.ecx;
    c->r[R_EDX] = m.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8; /* the return address and the key */
    recomp_return(c);
}

/* FUN_004636d0 (__thiscall collection, (key, out), RET 8, EAX): the entry for `key` in the collection's chain of
 * tables (this, then each table's +0xc), else in its class's table ([[this+0x14]+4] + 8, when [this+0x18] is set);
 * *out = the table that had it, or 0 with EAX 0. A found entry must have the "used" bit and a non-null address.
 * EAX, ECX and EDX end as the listing leaves them. */
static int attr_find(uint32_t self, uint32_t key, uint32_t out, AttrRegs *r, uint32_t *outv) {
    for (uint32_t t = self; t; t = rd32(t + 0xc)) {
        if (!attr_slot(t, key, r))
            return 0;
        const uint32_t s = r->eax;
        r->ecx = rd16(t + 4);
        if (s >= r->ecx)
            continue;
        r->edx = s * 3u;
        const uint32_t e = rd32(t) + s * 12u;
        r->eax = e;
        if (!(rd8(e + 0xb) & 0x80u) || e == 0)
            continue;
        r->ecx = out;
        *outv = t;
        return 1;
    }
    if (rd32(self + 0x18) != 0) {
        r->edx = rd32(self + 0x14);
        const uint32_t t = rd32(r->edx + 4) + 8u;
        if (!attr_slot(t, key, r))
            return 0;
        const uint32_t s = r->eax;
        r->ecx = rd16(t + 4);
        if (s < r->ecx) {
            r->edx = s * 3u;
            const uint32_t e = rd32(t) + s * 12u;
            r->eax = e;
            if ((rd8(e + 0xb) & 0x80u) && e != 0) {
                r->ecx = out;
                *outv = self;
                return 1;
            }
        }
    }
    r->edx = out;
    r->eax = 0;
    *outv = 0;
    return 1;
}
void nfsu_attr_find(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_ATTR_OFF") != NULL;
    }
    const uint32_t esp = c->r[R_ESP];
    const uint32_t key = rd32(esp + 4), out = rd32(esp + 8);
    AttrRegs m = {c->r[R_EAX], c->r[R_ECX], c->r[R_EDX]};
    uint32_t outv = 0;
    if (off || !attr_find(c->r[R_ECX], key, out, &m, &outv)) {
        fn_004636d0(c);
        return;
    }
    if (check) {
        fn_004636d0(c);
        static unsigned long calls;
        const AttrRegs t = {c->r[R_EAX], c->r[R_ECX], c->r[R_EDX]};
        if (m.eax != t.eax || m.ecx != t.ecx || m.edx != t.edx || outv != rd32(out))
            attr_report("FUN_004636d0", &m, &t, outv, rd32(out), 1);
        if (++calls % 1000000 == 0)
            fprintf(stderr, "nfsu native: FUN_004636d0 checked %lu calls\n", calls);
        return;
    }
    wr32(out, outv);
    c->r[R_EAX] = m.eax;
    c->r[R_ECX] = m.ecx;
    c->r[R_EDX] = m.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 12; /* the return address and two arguments */
    recomp_return(c);
}

/* FUN_006cea20 (__cdecl (curve, lo, hi, ratios, gears, w0, w1, shift, ratio_out, slots), AL): a car's shift points.
 * For each gear k from 2 to gears-2 it walks the engine speed x from (lo + hi) / 2 up to hi in steps of [0x00bd9c60]
 * and compares the wheel torque in this gear (the curve FUN_00706380 at x, scaled down below the curve's first
 * key, times a blend of w0..w1 plus one) with the next gear's at x * ratio[k+1] / ratio[k]; shift[k] is the first
 * x where the next gear pulls harder (else hi - [0x00bd9c48]), ratio_out[k+1] the matching ratio-scaled value. The
 * main thread runs it for every car a scene loads (with the spline, 18% of the race load's busiest half second,
 * 2026-11-18). Same operations in the listing's order: SSE doubles rounded to floats, the x87 FDIV/FMUL under the
 * precision-control field, the spline through spline_core. Effects reproduced: both arrays, EAX (AL the answer,
 * the upper bits what the listing leaves) and the x87 status flags; not ECX, EDX or XMM (scratch after a call:
 * both callers, 006cef31 and 006d4393, return or pop without reading them) nor the frame below the caller's ESP. */
static float curve_at(X86 *c, uint32_t curve, float x) { /* FUN_00706380 */
    const SplineArgs g = {x,
                          rdf32(curve),
                          rdf32(curve + 4),
                          rdf32(curve + 8),
                          rdf32(curve + 0xc),
                          rd32(curve + 0x14),
                          rd32(curve + 0x1c),
                          rd32(curve + 0x24),
                          (int32_t)rd32(curve + 0x18)};
    uint32_t r[3] = {0, 0, 0}, s8, sc;
    int wc;
    return (float)spline_core(c, &g, &r[0], &r[1], &r[2], &s8, &sc, &wc); /* already a float */
}
static float wheel_torque(X86 *c, uint32_t curve, float x) { /* 006ceb33..006ceb7f (and 006cec43..006cec94) */
    const float c0 = rdf32(curve);
    double t;
    if ((double)c0 > (double)x && c0 > 0.0f) {
        t = (double)curve_at(c, curve, c0);
        const double q = fx87(c, fdivz(c, (double)x, (double)c0));
        t = fx87(c, t * q);
    } else
        t = (double)curve_at(c, curve, x);
    return fto_float(c, t);
}
static float blend01(float u, double one, float w0, float w1) { /* 006ceb95..006cebea */
    const float top = rdf32(0x00bd97b0u);
    if (!(top > u))
        u = top;
    if (0.0 > (double)u)
        u = 0.0f;
    return (float)((one - (double)u) * (double)w0 + (double)u * (double)w1);
}
static uint32_t shift_points(X86 *c, uint32_t E) {
    const uint32_t curve = rd32(E + 4), ratios = rd32(E + 0x10), n = rd32(E + 0x14), shift = rd32(E + 0x20),
                   rout = rd32(E + 0x24), m = rd32(E + 0x28);
    const float lo = rdf32(E + 8), hi = rdf32(E + 0xc), w0 = rdf32(E + 0x18), w1 = rdf32(E + 0x1c);
    for (uint32_t i = 0; i < m; ++i) { /* 006cea44 */
        wr32(shift + 4u * i, 0);
        wr32(rout + 4u * i, 0);
    }
    if (m < n)
        return n & 0xffffff00u;
    const uint32_t last = n - 1u;
    if ((int32_t)last < 0)
        return last & 0xffffff00u;
    if ((int32_t)last > 2) {
        const float mid = (float)(((double)lo + (double)hi) * rdf64(0x00bd97d0u));
        const double one = rdf64(0x00bd9750u), step = rdf64(0x00bd9c60u), below = rdf64(0x00bd9c48u),
                     scale = rdf64(0x00c17e60u);
        const float zero_c = rdf32(0x00bd9770u), min_abs = rdf32(0x00bd9ee0u);
        for (uint32_t k = 0; k + 3u <= last; ++k) {
            const float rn = rdf32(ratios + 4u * (k + 3u)), rc = rdf32(ratios + 4u * (k + 2u));
            const uint32_t sk = shift + 4u * (k + 2u);
            float x = mid;
            const double dhi = (double)hi;
            int found = 0;
            if (dhi > (double)x) {
                const float absr = 0.0f > rc ? zero_c - rc : rc; /* FUN_00401690 */
                const double span = dhi - (double)lo;
                for (;;) {
                    const float t1 = wheel_torque(c, curve, x);
                    const float u1 = (float)(((double)x - (double)lo) / span);
                    const float v1 = blend01(u1, one, w0, w1);
                    const float g1 = (float)(((double)v1 + one) * (double)t1);
                    float g2 = 0.0f;
                    if (absr > min_abs) {
                        const float q2 = (float)((double)rn / (double)rc);
                        const float y = (float)((double)q2 * (double)x);
                        const float t2 = wheel_torque(c, curve, y);
                        const float u2 = (float)(((double)y - (double)lo) / span);
                        const float v2 = blend01(u2, one, w0, w1);
                        g2 = (float)(((double)v2 + one) * ((double)t2 * (double)rn / (double)rc));
                    }
                    if ((double)g2 > (double)g1) { /* 006ceda7 */
                        wrf32(sk, x);
                        found = 1;
                        break;
                    }
                    x = (float)((double)x + step);
                    if (!(dhi > (double)x))
                        break;
                }
            }
            if (!found)
                wrf32(sk, (float)(dhi - below)); /* 006ced80 */
            const float a = 0.0f > rc ? zero_c - rc : rc; /* 006ced94 */
            const uint32_t ok = rout + 4u * (k + 3u);
            if (a > min_abs)
                wrf32(ok, (float)((double)rdf32(sk) * (double)rn / (double)rc * scale));
            else
                wr32(ok, 0);
        }
    }
    wrf32(shift + 4u * last, hi); /* 006cee05 */
    return (last & 0xffffff00u) | 1u;
}
void nfsu_shift_points(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_SHIFT_OFF") != NULL;
    }
    const uint32_t E = c->r[R_ESP];
    const uint32_t m = rd32(E + 0x28), shift = rd32(E + 0x20), rout = rd32(E + 0x24);
    if (off || m > 4096u) {
        fn_006cea20(c);
        return;
    }
    if (check) {
        /* mine first, then the arrays put back and the translation run on the same input */
        uint32_t *keep = (uint32_t *)malloc(16u * (m ? m : 1u));
        uint32_t *mine = keep + 2u * (m ? m : 1u);
        for (uint32_t i = 0; i < m; ++i) {
            keep[i] = rd32(shift + 4u * i);
            keep[m + i] = rd32(rout + 4u * i);
        }
        const uint16_t sw0 = c->fpu_sw;
        const uint32_t eax = shift_points(c, E);
        const uint16_t sw1 = c->fpu_sw;
        for (uint32_t i = 0; i < m; ++i) {
            mine[i] = rd32(shift + 4u * i);
            mine[m + i] = rd32(rout + 4u * i);
            wr32(shift + 4u * i, keep[i]);
            wr32(rout + 4u * i, keep[m + i]);
        }
        c->fpu_sw = sw0;
        fn_006cea20(c);
        static unsigned long calls, bad;
        ++calls;
        uint32_t diff = 0;
        for (uint32_t i = 0; i < m; ++i)
            diff += (mine[i] != rd32(shift + 4u * i)) + (mine[m + i] != rd32(rout + 4u * i));
        if ((diff || eax != c->r[R_EAX] || sw1 != c->fpu_sw) && bad++ < 50)
            fprintf(stderr,
                    "nfsu native: FUN_006cea20 gave eax %08x sw %04x, the translation eax %08x sw %04x; %u of %u "
                    "values differ (call %lu)\n",
                    eax, sw1, c->r[R_EAX], c->fpu_sw, diff, 2u * m, calls);
        if (calls % 200 == 0)
            fprintf(stderr, "nfsu native: FUN_006cea20 checked %lu calls, %lu differ\n", calls, bad);
        free(keep);
        return;
    }
    c->r[R_EAX] = shift_points(c, E);
    c->eip = rd32(E);
    c->r[R_ESP] = E + 4; /* __cdecl */
    recomp_return(c);
}

/* FUN_007063c0 (__thiscall curve, (x), RET 4, ST0): the curve FUN_00706380 evaluates, with x clamped to the keys'
 * range [curve+0, curve+4] first and the result to [curve+8, curve+0xc] after (each clamp a double compare that keeps
 * the bound when it is beyond). The car setup code of a scene load calls it per tuning value (with the spline, 12%
 * of the race load's busiest stretch after FUN_006cea20 went native, 2026-11-18). Effects reproduced: ST0, the
 * argument slot [esp+4] (the clamped result), EAX/ECX/EDX as the spline leaves them from the values loaded at
 * 0070640e..00706417, and the x87 status flags; the spline's frame below the caller's ESP is not. */
void nfsu_curve_clamped(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_SPLINE_OFF") != NULL;
    }
    if (off) {
        fn_007063c0(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], cv = c->r[R_ECX];
    const float x = rdf32(E + 4), c0 = rdf32(cv), c1 = rdf32(cv + 4);
    float xc = (double)c1 > (double)x ? x : c1;
    if ((double)c0 > (double)xc)
        xc = c0;
    const SplineArgs g = {xc, c0, c1, rdf32(cv + 8), rdf32(cv + 0xc), rd32(cv + 0x14), rd32(cv + 0x1c), rd32(cv + 0x24),
                          (int32_t)rd32(cv + 0x18)};
    uint32_t eax = g.knots, ecx = g.d2, edx = g.vals, s8, sc;
    int wc;
    const uint16_t sw0 = c->fpu_sw;
    const float r = (float)spline_core(c, &g, &eax, &ecx, &edx, &s8, &sc, &wc);
    float res = (double)g.out_hi > (double)r ? r : g.out_hi;
    if ((double)g.out_lo > (double)res)
        res = g.out_lo;
    if (check) {
        const uint16_t sw1 = c->fpu_sw;
        c->fpu_sw = sw0;
        fn_007063c0(c);
        static unsigned long calls, bad;
        ++calls;
        uint64_t tb;
        const double theirs = ST(c, 0), mine = (double)res;
        memcpy(&tb, &theirs, 8);
        uint64_t mb;
        memcpy(&mb, &mine, 8);
        uint32_t rb;
        memcpy(&rb, &res, 4);
        if ((mb != tb || sw1 != c->fpu_sw || eax != c->r[R_EAX] || ecx != c->r[R_ECX] || edx != c->r[R_EDX] ||
             rb != rd32(E + 4)) &&
            bad++ < 50)
            fprintf(stderr,
                    "nfsu native: FUN_007063c0 gave %08x sw %04x eax %08x ecx %08x edx %08x, the translation %08x sw "
                    "%04x eax %08x ecx %08x edx %08x (call %lu)\n",
                    rb, sw1, eax, ecx, edx, rd32(E + 4), c->fpu_sw, c->r[R_EAX], c->r[R_ECX], c->r[R_EDX], calls);
        if (calls % 200000 == 0)
            fprintf(stderr, "nfsu native: FUN_007063c0 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    wrf32(E + 4, res);
    fpush(c, (double)res);
    c->r[R_EAX] = eax;
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = edx;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 8; /* the return address and x */
    recomp_return(c);
}

/* FUN_00706380 (__thiscall curve, (x), RET 4, ST0): the spline FUN_006a3060 over the curve's fields (x, [+0], [+4],
 * [+8], [+0xc], keys [+0x14], values [+0x1c], second derivatives [+0x24], count [+0x18]), no clamping. Effects
 * reproduced: ST0, EAX/ECX/EDX as the spline leaves them from the values loaded at 00706380..0070638e, the x87 status
 * flags; the spline's frame is below the caller's ESP. */
void nfsu_curve(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_SPLINE_OFF") != NULL;
    }
    if (off) {
        fn_00706380(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], cv = c->r[R_ECX];
    const SplineArgs g = {rdf32(E + 4),     rdf32(cv),        rdf32(cv + 4),    rdf32(cv + 8),
                          rdf32(cv + 0xc),  rd32(cv + 0x14),  rd32(cv + 0x1c),  rd32(cv + 0x24),
                          (int32_t)rd32(cv + 0x18)};
    uint32_t eax = g.vals, ecx = cv, edx = g.knots, s8, sc;
    int wc;
    const uint16_t sw0 = c->fpu_sw;
    const double r = spline_core(c, &g, &eax, &ecx, &edx, &s8, &sc, &wc);
    if (check) {
        const uint16_t sw1 = c->fpu_sw;
        c->fpu_sw = sw0;
        fn_00706380(c);
        static unsigned long calls, bad;
        ++calls;
        uint64_t tb, mb;
        const double theirs = ST(c, 0);
        memcpy(&tb, &theirs, 8);
        memcpy(&mb, &r, 8);
        if ((mb != tb || sw1 != c->fpu_sw || eax != c->r[R_EAX] || ecx != c->r[R_ECX] || edx != c->r[R_EDX]) &&
            bad++ < 50)
            fprintf(stderr,
                    "nfsu native: FUN_00706380 gave %016llx sw %04x eax %08x ecx %08x edx %08x, the translation %016llx "
                    "sw %04x eax %08x ecx %08x edx %08x (call %lu)\n",
                    (unsigned long long)mb, sw1, eax, ecx, edx, (unsigned long long)tb, c->fpu_sw, c->r[R_EAX],
                    c->r[R_ECX], c->r[R_EDX], calls);
        if (calls % 200000 == 0)
            fprintf(stderr, "nfsu native: FUN_00706380 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    fpush(c, r);
    c->r[R_EAX] = eax;
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = edx;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 8; /* the return address and x */
    recomp_return(c);
}

/* FUN_0076d720 (__thiscall vertex buffer, (pos, matrix, colour, uv, uv2), RET 0x14): adds one vertex of a screen
 * quad (FUN_007816d0 and its siblings call it six times per quad: the HUD's and menus' sprites and text). The
 * position (x, y, z, [0x00bd97b0]) goes through D3DXVec4Transform when a matrix is given and is written to row n =
 * [this+0x70] of the positions ([this+0x1c], [this+0x2c] floats a row); then the last six rows are searched for the
 * same position (FUN_0076d290 when a row is 8 floats, else FUN_0076d120, which also compares uv - [this+0x57c/0x580])
 * by truncating x * [0x00bebb28] (uv: * [0x00bd9ea0]) to integers, as CVTTSD2SI does. Found: its index goes into the
 * index list ([this+0xc4], count u16 [this+0xda]); otherwise n does, the colour ([this+0x20]), the clamped uv
 * ([this+0x24], rows of 10+) and uv2 ([this+0x28], rows of 12+) are written and n grows. In a race with rivals the
 * HUD makes so many of these that the function was 15-19% of the main thread (2026-11-21, the person's race).
 * Effects reproduced: every store, EAX/ECX/EDX as the translation leaves them; the XMM registers (volatile, and
 * the callers reload) and the callee's frame below ESP are not. */
void recomp_d3dx_vec4_transform(const float v[4], const float m[16], float out[4]);
static inline int32_t cvtt_sd(double d) { /* CVTTSD2SI: NaN or out of range -> 0x80000000 */
    if (!(d > -2147483649.0 && d < 2147483648.0))
        return INT32_MIN;
    return (int32_t)d;
}
static inline uint32_t fld_fstp(uint32_t b) { /* FLD float + FSTP float: a signalling NaN comes back quiet */
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) && !(b & 0x00400000u))
        b |= 0x00400000u;
    return b;
}
typedef struct {
    uint32_t addr[24], val[24];
    uint8_t wide[24];
    int n;
} VertexWrites;
static void vw_put(VertexWrites *w, uint32_t a, uint32_t v, int wide) {
    w->addr[w->n] = a, w->val[w->n] = v, w->wide[w->n] = (uint8_t)wide, ++w->n;
}
/* the search: returns 1 and the index when one of the last six rows holds the same position (and uv) */
static int vertex_find(uint32_t self, uint32_t pos, uint32_t uv, uint32_t *idx, uint32_t *edx_left) {
    const uint32_t stride = rd32(self + 0x2c);
    const double kp = rdf64(0x00bebb28u), ku = rdf64(0x00bd9ea0u);
    const int32_t px = cvtt_sd((double)rdf32(pos) * kp), py = cvtt_sd((double)rdf32(pos + 4) * kp),
                  pz = cvtt_sd((double)rdf32(pos + 8) * kp);
    int32_t i = (int32_t)rd32(self + 0x70) - 1;
    *edx_left = 0;
    for (int tries = 0; i >= 0 && tries < 6; --i, ++tries) {
        const uint32_t row = rd32(self + 0x1c) + (uint32_t)(stride * (uint32_t)i) * 4u;
        const int32_t dx = px - cvtt_sd((double)rdf32(row) * kp);
        const int32_t dy = py - cvtt_sd((double)rdf32(row + 4) * kp);
        const int32_t dz = pz - cvtt_sd((double)rdf32(row + 8) * kp);
        if (stride == 8) {
            *edx_left = (uint32_t)dx;
            if (dx == 0 && dy == 0 && dz == 0) {
                *idx = (uint32_t)i;
                return 1;
            }
            continue;
        }
        const uint32_t urow = rd32(self + 0x24) + (uint32_t)(stride * (uint32_t)i) * 4u;
        const int32_t du = cvtt_sd(((double)rdf32(uv) - (double)rdf32(self + 0x57c)) * ku) -
                           cvtt_sd((double)rdf32(urow) * ku);
        const int32_t dv = cvtt_sd(((double)rdf32(uv + 4) - (double)rdf32(self + 0x580)) * ku) -
                           cvtt_sd((double)rdf32(urow + 4) * ku);
        *edx_left = (uint32_t)dz;
        if (dx == 0 && dy == 0 && dz == 0 && du == 0 && dv == 0) {
            *idx = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}
static void vertex_add(uint32_t self, uint32_t E, VertexWrites *w, uint32_t regs[3]) {
    const uint32_t pos = rd32(E + 4), mat = rd32(E + 8), col = rd32(E + 0xc), uv = rd32(E + 0x10),
                   uv2 = rd32(E + 0x14);
    /* FUN_0076d370 */
    float v[4];
    uint32_t vb[4] = {rd32(pos), rd32(pos + 4), rd32(pos + 8), rd32(0x00bd97b0u)};
    if (mat) {
        float m[16], o[4];
        memcpy(v, vb, 16);
        for (int k = 0; k < 16; ++k)
            m[k] = rdf32(mat + 4u * (uint32_t)k);
        recomp_d3dx_vec4_transform(v, m, o);
        memcpy(vb, o, 16);
    }
    const uint32_t n = rd32(self + 0x70), stride = rd32(self + 0x2c), rowb = (uint32_t)(n * stride) * 4u;
    for (int k = 0; k < 4; ++k)
        vw_put(w, rd32(self + 0x1c) + rowb + 4u * (uint32_t)k, vb[k], 1);
    uint32_t idx = 0, edx_left = 0;
    /* the search reads the rows before n only, so the row just written doesn't change it */
    const int found = vertex_find(self, pos, uv, &idx, &edx_left);
    const uint32_t list = rd32(self + 0xc4), cnt = rd16(self + 0xda);
    if (found) {
        vw_put(w, list + cnt * 2u, idx & 0xffffu, 0);
        vw_put(w, self + 0xda, (cnt + 1u) & 0xffffu, 0);
        regs[0] = cnt, regs[1] = list, regs[2] = idx & 0xffffu;
        return;
    }
    vw_put(w, list + cnt * 2u, n & 0xffffu, 0);
    vw_put(w, self + 0xda, (cnt + 1u) & 0xffffu, 0);
    const uint32_t cbase = rd32(self + 0x20);
    for (int k = 0; k < 4; ++k)
        vw_put(w, cbase + rowb + 4u * (uint32_t)k, fld_fstp(rd32(col + 4u * (uint32_t)k)), 1);
    regs[1] = n * stride, regs[2] = cbase;
    if (stride >= 10) { /* FUN_0076d430 */
        const float one = rdf32(0x00bd97b0u);
        float u = (float)((double)rdf32(uv) - (double)rdf32(self + 0x57c));
        float t = (float)((double)rdf32(uv + 4) - (double)rdf32(self + 0x580));
        if (0.0f > u)
            u = 0.0f;
        else if (u > one)
            u = one;
        if (0.0f > t)
            t = 0.0f;
        else if (t > one)
            t = one;
        uint32_t ub, tb;
        memcpy(&ub, &u, 4);
        memcpy(&tb, &t, 4);
        const uint32_t ubase = rd32(self + 0x24);
        vw_put(w, ubase + rowb, ub, 1);
        vw_put(w, ubase + rowb + 4u, tb, 1);
        regs[1] = ubase, regs[2] = ubase;
    }
    regs[0] = stride;
    if (stride >= 12) {
        const uint32_t b2 = rd32(self + 0x28);
        vw_put(w, b2 + rowb, fld_fstp(rd32(uv2)), 1);
        vw_put(w, b2 + rowb + 4u, fld_fstp(rd32(uv2 + 4)), 1);
        regs[0] = uv2, regs[1] = n * stride, regs[2] = b2;
    }
    vw_put(w, self + 0x70, n + 1u, 1);
}
void nfsu_vertex_add(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_VERTEX_OFF") != NULL;
    }
    if (off) {
        fn_0076d720(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], self = c->r[R_ECX];
    VertexWrites w;
    w.n = 0;
    uint32_t regs[3];
    vertex_add(self, E, &w, regs);
    if (check) {
        fn_0076d720(c);
        static unsigned long calls, bad;
        ++calls;
        int diff = c->r[R_EAX] != regs[0] || c->r[R_ECX] != regs[1] || c->r[R_EDX] != regs[2];
        for (int k = 0; k < w.n; ++k)
            if ((w.wide[k] ? rd32(w.addr[k]) : rd16(w.addr[k])) != w.val[k])
                diff = 1;
        if (diff && bad++ < 50) {
            fprintf(stderr, "nfsu native: FUN_0076d720 differs (call %lu): regs %08x %08x %08x vs %08x %08x %08x;",
                    calls, regs[0], regs[1], regs[2], c->r[R_EAX], c->r[R_ECX], c->r[R_EDX]);
            for (int k = 0; k < w.n; ++k) {
                const uint32_t t = w.wide[k] ? rd32(w.addr[k]) : rd16(w.addr[k]);
                if (t != w.val[k])
                    fprintf(stderr, " [%08x] %08x vs %08x", w.addr[k], w.val[k], t);
            }
            fprintf(stderr, "\n");
        }
        if (calls % 500000 == 0)
            fprintf(stderr, "nfsu native: FUN_0076d720 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    for (int k = 0; k < w.n; ++k) {
        if (w.wide[k])
            wr32(w.addr[k], w.val[k]);
        else
            wr16(w.addr[k], (uint16_t)w.val[k]);
    }
    c->r[R_EAX] = regs[0];
    c->r[R_ECX] = regs[1];
    c->r[R_EDX] = regs[2];
    c->eip = rd32(E);
    c->r[R_ESP] = E + 0x18; /* the return address and five arguments */
    recomp_return(c);
}

/* FUN_0082a7d0 (__thiscall scenery section, (instance, view state, frustum flag), RET 0xc): decides whether one
 * scenery instance (section [this+0x20] + instance * 0x60) is drawn in a view and, if so, queues it. Its callers
 * (FUN_0082acc0 / FUN_0082ad10) run it for every instance of every visible section in every view of every frame,
 * and most instances are turned away by its first tests - before any of them it builds an exception frame and a
 * stack cookie, which made the function 13% of the main thread in a race on its own (2026-11-21). Here those first
 * tests run natively and an instance they turn away returns at once; everything else is the translation (which
 * repeats the tests). The tests, as at 0082a803..0082a894: the "skip" instance [0x012f7ea0]/[0x012f7ea4] on 4 of
 * every 8 frames ([0x00d3c0ec] % 8 >= 4); the view's layer mask [state+0x48] against [instance+0x3d]; then for the
 * views 0x1f..0x21 ([[state]+4], FUN_00538df0) the box (instance, instance+0x10) against the view (FUN_0053a7b0 ->
 * FUN_0074a1d0, through the same native tests as nfsu_bounds_vs_frustum; a case those don't take goes to the
 * translation), for the others the section's visibility bit [state+0x44] in the row [this+0x34] + (short)
 * [instance+0x1c] * 0x200. Turning away writes nothing above the caller's ESP and leaves FS:[0] as it was; the
 * callers reload EAX/ECX/EDX. NFSU_SCENERY_OFF=1 = the translation; NFSU_NATIVE_CHECK=1 runs the translation for
 * each instance turned away and logs it when the translation queued it or counted it ([state+0xc], [state+0x4c]). */
void nfsu_scenery_visible(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_SCENERY_OFF") != NULL;
    }
    if (off) {
        fn_0082a7d0(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], self = c->r[R_ECX], idx = rd32(E + 4), st = rd32(E + 8);
    const uint32_t inst = rd32(self + 0x20) + idx * 0x60u;
    int away = 0;
    const uint32_t skip = rd32(0x012f7ea0u);
    if (skip && rd32(self + 0xc) == skip && idx == rd32(0x012f7ea4u) && (int32_t)rd32(0x00d3c0ecu) % 8 >= 4)
        away = 1;
    if (!away && (rd8(st + 0x48) & rd8(inst + 0x3d)) == 0)
        away = 2;
    if (!away) {
        const uint32_t view = rd32(st);
        const int32_t kind = (int32_t)rd32(view + 4);
        if (kind >= 0x1f && kind <= 0x21) {
            uint32_t res = 0;
            if (!box_vs_planes(view, inst, inst + 0x10u, 0, &res) && !box_vs_view(view, inst, inst + 0x10u, 0, &res)) {
                fn_0082a7d0(c);
                return;
            }
            if (res == 0)
                away = 3;
        } else {
            const int32_t bit = (int32_t)rd32(st + 0x44);
            if (bit >= 0) {
                const uint32_t row = (uint32_t)((int32_t)(int16_t)rd16(inst + 0x1c) << 9) + rd32(self + 0x34);
                if (rd8(row + (uint32_t)(bit >> 3)) & (1u << (bit & 7)))
                    away = 4;
            }
        }
    }
    if (!away) {
        fn_0082a7d0(c);
        return;
    }
    if (check) {
        const uint32_t q0 = rd32(st + 0xc), n0 = rd32(st + 0x4c);
        fn_0082a7d0(c);
        static unsigned long calls, bad;
        ++calls;
        if ((rd32(st + 0xc) != q0 || rd32(st + 0x4c) != n0) && bad++ < 50)
            fprintf(stderr, "nfsu native: FUN_0082a7d0 turned instance %u away (test %d), the translation kept it "
                    "(call %lu)\n", idx, away, calls);
        if (calls % 1000000 == 0)
            fprintf(stderr, "nfsu native: FUN_0082a7d0 checked %lu instances turned away, %lu differ\n", calls, bad);
        return;
    }
    c->eip = rd32(E);
    c->r[R_ESP] = E + 16; /* the return address and three arguments */
    recomp_return(c);
}

/* FUN_007674e0 (__stdcall (stage, texture, state), RET 0xc): the render thread's "set texture": the stage's cached
 * value [0x00deb830 + stage * 4] against the texture's handle ([[texture] + 0x10], 0 for none); only a change calls
 * the device (SetTexture, vtable +0x104). The unchanged case runs here; a change goes to the translation. Effects
 * reproduced for the unchanged case: EAX = the handle, ECX = the stage (MOVZX of the word). 2026-11-21. */
void nfsu_set_texture(X86 *c) {
    static int off = -1;
    if (off < 0)
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_MATERIAL_OFF") != NULL;
    const uint32_t E = c->r[R_ESP];
    const uint32_t stage = rd16(E + 4), tex = rd32(E + 8);
    const uint32_t handle = tex ? rd32(rd32(tex) + 0x10) : 0u;
    if (off || rd32(0x00deb830u + stage * 4u) != handle) {
        fn_007674e0(c);
        return;
    }
    c->r[R_EAX] = handle;
    c->r[R_ECX] = stage;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 16; /* the return address and three arguments */
    recomp_return(c);
}

/* FUN_00541730 (__thiscall material layers [this + 4i], i < 4; (p2, p3, three 8-byte values, p7, state), RET 0x28):
 * binds a material's four layers before a draw. For each layer the state's slot [state + 0xd4 + 4i] remembers the
 * layer last bound: a slot whose layer id ([+0x24]) differs from the layer's is cleared; then a layer is set up by
 * FUN_00540c40 when [0x00d2ac0e] is 0 (never in this exe: the byte is 1 in .data and nothing writes it) or the slot
 * holds another id, by FUN_005410e0 (old and new argument blocks) when p2/p3 differ from [state + 0xa0/0xa4], and
 * not at all otherwise; the slot then takes the layer. The OR of the slots' [+0x20] masks says which of the 16
 * texture stages stay; the others are cleared with FUN_007674e0, and the arguments are kept at
 * [state + 0xa0 .. 0xc3]. In a race it runs ~3000 times a frame: the render thread's biggest function (2026-11-21).
 * Everything here is what the listing does, in its order, with the same stack layout for the calls (ESP, the
 * argument blocks, the return addresses after each CALL, EAX/ECX/EDX and the callee-saved registers as the
 * listing holds them), so the callees see the same machine. NFSU_MATERIAL_OFF=1 = the translation. */
static void mat_call(X86 *c, uint32_t fn, uint32_t ret) {
    c->r[R_ESP] -= 4;
    wr32(c->r[R_ESP], ret);
    recomp_call(c, fn);
}
void nfsu_material_bind(X86 *c) {
    static int off = -1;
    if (off < 0)
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_MATERIAL_OFF") != NULL;
    if (off) {
        fn_00541730(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], self = c->r[R_ECX], st = rd32(E + 0x28);
    const uint32_t save_ebx = c->r[R_EBX], save_ebp = c->r[R_EBP], save_esi = c->r[R_ESI], save_edi = c->r[R_EDI];
    const uint32_t body = E - 0x18u; /* SUB ESP,8 and four pushes */
    /* the listing pushes EBX, EBP, ESI, EDI: [E-0xc], [E-0x10], [E-0x14], [E-0x18] */
    wr32(E - 0x0cu, save_ebx), wr32(E - 0x10u, save_ebp), wr32(E - 0x14u, save_esi), wr32(E - 0x18u, save_edi);
    const int always = rd8(0x00d2ac0eu) == 0;
    const int changed = rd32(E + 4) != rd32(st + 0xa0) || rd32(E + 8) != rd32(st + 0xa4);
    uint32_t mask = 0, eax = 0, ecx = 0, edx = c->r[R_EDX];
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t sa = st + 0xd4u + 4u * i;
        uint32_t cur = rd32(sa);
        ecx = cur;
        if (cur) {
            eax = rd32(self + 4u * i);
            if (eax) {
                edx = rd32(cur + 0x24);
                if (edx != rd32(eax + 0x24)) {
                    wr32(sa, 0);
                    cur = 0;
                }
            } else {
                wr32(sa, 0);
                cur = 0;
            }
        }
        const uint32_t L = rd32(self + 4u * i);
        ecx = L;
        if (L) {
            uint32_t fn = 0, ret = 0;
            if (always) {
                if (rd32(sa) != 0)
                    wr32(sa, 0);
                fn = 0x00540c40u, ret = 0x005417fdu;
            } else if (!(cur = rd32(sa)) || rd32(cur + 0x24) != rd32(L + 0x24)) {
                fn = 0x00540c40u, ret = 0x00541855u;
                if (cur)
                    eax = rd32(cur + 0x24);
                else
                    eax = 0;
            } else if (changed) {
                fn = 0x005410e0u, ret = 0x005418e7u;
                eax = rd32(cur + 0x24);
            }
            if (fn) {
                /* the call's frame: PUSH ESI (i), PUSH EDI (state), then one or two 0x24-byte blocks */
                uint32_t sp = body;
                sp -= 4, wr32(sp, i);
                sp -= 4, wr32(sp, st);
                if (fn == 0x005410e0u) {
                    sp -= 0x24;
                    for (uint32_t k = 0; k < 0x24; k += 4)
                        wr32(sp + k, rd32(st + 0xa0u + k));
                }
                sp -= 0x24;
                for (uint32_t k = 0; k < 0x24; k += 4)
                    wr32(sp + k, rd32(E + 4 + k));
                c->r[R_ESP] = sp;
                c->r[R_EAX] = sp;
                c->r[R_ECX] = L;
                c->r[R_EDX] = ret == 0x005417fdu ? edx : rd32(E + 0x24);
                c->r[R_EBX] = self, c->r[R_ESI] = i, c->r[R_EDI] = st, c->r[R_EBP] = sa;
                mat_call(c, fn, ret);
                /* back in the listing: ESP = body (the callee popped its arguments) */
                eax = rd32(self + 4u * i);
                if (fn == 0x00540c40u && ret == 0x005417fdu)
                    edx = eax;
                wr32(sa, eax);
                ecx = c->r[R_ECX];
                if (ret != 0x005417fdu)
                    edx = c->r[R_EDX];
            }
        }
        eax = rd32(sa);
        if (eax) {
            ecx = rd32(eax + 0x20);
            mask |= ecx;
        }
    }
    for (uint32_t sidx = 0; sidx < 16; ++sidx) {
        edx = mask;
        if (mask & (1u << sidx))
            continue;
        uint32_t sp = body;
        sp -= 4, wr32(sp, st);
        sp -= 4, wr32(sp, 0);
        sp -= 4, wr32(sp, sidx);
        c->r[R_ESP] = sp;
        c->r[R_ECX] = body + 0x28u - 0x0cu; /* LEA ECX,[ESP+0x28] after the three pushes */
        c->r[R_EAX] = eax, c->r[R_EDX] = edx;
        c->r[R_EBX] = 1u << sidx, c->r[R_ESI] = sidx, c->r[R_EDI] = st, c->r[R_EBP] = st + 0xe4u;
        mat_call(c, 0x007674e0u, 0x00541926u);
        eax = c->r[R_EAX], ecx = c->r[R_ECX], edx = c->r[R_EDX];
    }
    for (uint32_t k = 0; k < 0x24; k += 4)
        wr32(st + 0xa0u + k, rd32(E + 4 + k));
    c->r[R_EAX] = rd32(E + 0x24);
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = edx;
    c->r[R_EBX] = save_ebx, c->r[R_EBP] = save_ebp, c->r[R_ESI] = save_esi, c->r[R_EDI] = save_edi;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 0x2c; /* the return address and 0x28 bytes of arguments */
    recomp_return(c);
}

/* FUN_00763ef0 (__stdcall (pp, min, max, matrix, matrix2, flip), RET 0x18, EAX): a draw's depth key - the box's
 * centre ((min + max) * [0x00bd97d0], as doubles rounded to floats), w = [0x00bd97b0], through D3DXVec4Transform by
 * matrix2 (when given), matrix and [*pp] + 0x80 (the guest's thunk 007436d0 -> the import each time), then
 * z * [0x00bd9c48] (as a float), or [0x00c21e88] minus that when flip; not above 0 -> 0; truncated to an integer
 * (FISTP qword with the rounding set to truncate; EAX its low half). The main thread calls it for every draw it
 * sorts (~4% of it in a race, 2026-11-22): three import round trips each. Effects reproduced: EAX, and ECX/EDX = the
 * vector's stack address, as the last thunk call leaves them. NFSU_DEPTHKEY_OFF=1 = the translation;
 * NFSU_NATIVE_CHECK=1 compares. */
void nfsu_depth_key(X86 *c) {
    static int check = -1, off = 0;
    if (check < 0) {
        check = getenv("NFSU_NATIVE_CHECK") != NULL;
        off = getenv("NFSU_NATIVE_OFF") != NULL || getenv("NFSU_DEPTHKEY_OFF") != NULL;
    }
    if (off) {
        fn_00763ef0(c);
        return;
    }
    const uint32_t E = c->r[R_ESP];
    const uint32_t pp = rd32(E + 4), pmin = rd32(E + 8), pmax = rd32(E + 12), m1 = rd32(E + 16), m2 = rd32(E + 20);
    const int flip = (rd32(E + 24) & 0xffu) != 0;
    const double half = rdf64(0x00bd97d0u);
    float v[4], o[4], m[16];
    for (int k = 0; k < 3; ++k)
        v[k] = (float)(((double)rdf32(pmin + 4u * (uint32_t)k) + (double)rdf32(pmax + 4u * (uint32_t)k)) * half);
    v[3] = rdf32(0x00bd97b0u);
    const uint32_t mats[3] = {m2, m1, rd32(pp) + 0x80u};
    for (int i = 0; i < 3; ++i) {
        if (i == 0 && !m2)
            continue;
        for (int k = 0; k < 16; ++k)
            m[k] = rdf32(mats[i] + 4u * (uint32_t)k);
        recomp_d3dx_vec4_transform(v, m, o);
        memcpy(v, o, sizeof v);
    }
    float z = (float)((double)v[2] * rdf64(0x00bd9c48u));
    if (flip)
        z = (float)(rdf64(0x00c21e88u) - (double)z);
    if (!(z > 0.0f))
        z = 0.0f;
    const double t = trunc((double)z);
    const uint32_t eax = t >= 9223372036854775808.0 ? 0u : (uint32_t)(uint64_t)(int64_t)t;
    const uint32_t vaddr = ((E - 4u) & ~15u) - 0x10u;
    if (check) {
        fn_00763ef0(c);
        static unsigned long calls, bad;
        ++calls;
        if ((c->r[R_EAX] != eax || c->r[R_ECX] != vaddr || c->r[R_EDX] != vaddr) && bad++ < 50)
            fprintf(stderr, "nfsu native: FUN_00763ef0 gave %08x (ecx/edx %08x), the translation %08x (%08x %08x), call %lu\n",
                    eax, vaddr, c->r[R_EAX], c->r[R_ECX], c->r[R_EDX], calls);
        if (calls % 1000000 == 0)
            fprintf(stderr, "nfsu native: FUN_00763ef0 checked %lu calls, %lu differ\n", calls, bad);
        return;
    }
    c->r[R_EAX] = eax;
    c->r[R_ECX] = vaddr;
    c->r[R_EDX] = vaddr;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 0x1c; /* the return address and six arguments */
    recomp_return(c);
}
