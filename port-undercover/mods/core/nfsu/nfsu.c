/* nfsu.c - Need for Speed: Undercover's core mod (core.nfsu), for nfs.exe 1.0.0.1 (SHA-256 d1ecded0...c7d2e9).
 *
 * It gives the kit's own features a core mod to read their settings from (game.toml [settings] core_mod:
 * pause_in_menu, upscaling) and the launcher its options (render_scale, mixer_own_core), and it carries the
 * game hooks bring-up needs, each with the listing it came from (see CLAUDE.md).
 *
 * The hooks were found on nfs.exe 1.1.2.1 (2026-11-13/14; that version's mod, with its diagnostics, is kept in
 * docs/nfsu-1.1.2.1/nfsu.c.txt) and moved to 1.0.0.1 on 2026-11-14 by matching each function's bytes with
 * addresses and branch offsets masked (one unique match each, instructions identical):
 *   render wait step 007666f0 -> 00792180, its flag [0x013b31e4] -> [0x00ea0164] (the first instruction of both),
 *   command ring run 00687070 -> 006a2cd0, file size query 0068fe30 -> 006abab0,
 *   KERNEL32!Sleep's import slot 0x00b610a0 -> 0x00bd60a4 (the import directory).
 *
 * RENDER-THREAD WAIT. The main thread queues a request for the render thread and spins `while (!reply)
 * step();`. Once the flag is set, the step returns at once without calling anything, so on Windows the loop is a
 * busy wait the preemptive render thread ends. The kit runs one guest thread at a time and hands over only at a
 * blocking import, so the main thread spun forever and the render thread never answered (RECOMP_PROFILE on
 * 1.1.2.1: the wait 54%, the step 46% of every 10 s window, no frame). A BEFORE hook on the step calls the game's
 * own Sleep(0) when the flag is set, which yields the baton the way a Windows thread's time slice would end.
 */
#include <time.h>
#include "pop_mod_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

POP_MOD_DECLARE_ABI();

#define RENDER_WAIT_STEP 0x00792180u /* void step(void): the body of the main thread's render-request wait loop */
#define RENDER_WAIT_FLAG 0x00ea0164u /* set: the step returns at once (the busy-wait case) */
#define IAT_SLEEP 0x00bd60a4u        /* KERNEL32!Sleep in nfs.exe's import table */
/* The mirror image on the render thread: its loop keeps calling this thiscall (the command ring: +0 commands
 * queued, +4 run, +0xc enabled, +0x14 write position, +0x18 read position) until the main thread answers; with
 * nothing queued the call does nothing, so it spun while the main thread could not run. */
#define RENDER_RING_RUN 0x006a2cd0u
/* MISSING REGION EXTRA. LoadRegionInit loads TRACKS\<track>\MakeSceneryExtra.bin and asks this function for its
 * size (-1 when absent). Neither install has the file. On 1.1.2.1 the -1 became a "-1 byte" allocation that the
 * pool's clearalloc=0xAA memset over 4 GB (the crash after the save loaded; that exe crashed on Windows too). The
 * game's own rule for a file shorter than 8 bytes is "no data" (no allocation, no read), so a missing extra file
 * is reported as size 0 to this one query - only for this name and only when the original said -1. */
#define FILE_SIZE_QUERY 0x006abab0u

static const PopModApi *g_api;
static uint32_t g_hooks[4];
static unsigned long g_yields, g_extra_missing;

static uint32_t get_u32(uint32_t addr) {
    uint32_t v = 0;
    g_api->guest_read_u32(g_api, addr, &v);
    return v;
}

static void read_guest_str(uint32_t p, char *out, unsigned cap) {
    for (unsigned i = 0; p && i + 1 < cap; ++i) {
        const uint32_t w = get_u32((p + i) & ~3u);
        out[i] = (char)(w >> (8 * ((p + i) & 3)));
        if (!out[i])
            return;
    }
    out[cap - 1] = 0;
}

static void yield_baton(const PopModApi *api) {
    const uint32_t sleep = get_u32(IAT_SLEEP);
    if (!sleep)
        return;
    const uint32_t args[1] = {0};
    api->guest_call(api, sleep, 0, args, 1, NULL);
}

/* The menus' frame rate (core.nfsu/menu_fps: 0 = 45, as on the person's Windows laptop; 1 = 30; 2 = 60; 3 = not
 * limited). Their animations keep real time: the frontend (EA's APT) takes each update's frame time from RDTSC
 * (FUN_00a1c960). For a day they ran 3x fast at any frame rate: a translator aliasing bug (see x86.h, RecompG32)
 * fed APT garbage that its 100 ms cap turned into 100 ms per update (2026-11-16, measured against the person's
 * Windows recording: 0.25 of the selected row's width per second for its scrolling label, 0.24 now). This limit only
 * decides how many frames the menus draw. The world keeps time on its own: its simulation ticks at 60 Hz
 * (FUN_006a2980 counts the ticks in 0x00d3c108, +120 in 2 s while driving at any frame rate) and stops while the
 * game is paused, so while that counter has not moved for two seconds (g_menu_idle_ns) - the pause menu over the city,
 * the frontend screens, videos, loading - each main-loop pass (FUN_006ae4c0) waits for its slot through the game's
 * own Sleep. (The chase camera, the signal used before, keeps ticking under the pause menu.) */
#define MAIN_LOOP_STEP 0x006ae4c0u
#define WORLD_TICKS 0x00d3c108u
static int64_t g_menu_due_ns, g_menu_period_ns = 33333333;
/* How long the world's tick counter must stand still before the main loop is paced as a menu. 250 ms caught the
 * game's short waits for the city to stream in while driving (2026-11-17: 1.5 s at 45 passes a second mid-drive -
 * and the main loop is also what services the streamer, so pacing it drew the wait out). A pause menu or the
 * frontend stays still far longer. NFSU_MENU_IDLE_MS=<ms> (A/B). */
static int64_t g_menu_idle_ns = 2000000000LL;
static int g_menu_fps_env; /* NFSU_MENU_FPS fixes the rate (A/B) */
static uint32_t g_world_ticks;
static int64_t g_world_tick_ns;
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#include "attrib.inc"
#include "extraoptions.inc"
#include "addoncars.inc"
#include "trackpad.inc"
#include "padlayout.inc"
#include "stream.inc"
#include "heli.inc"
#include "aiguard.inc"
/* NFSU_MEMDUMP=<seconds> (DIAGNOSTIC): the image's data and bss (0x00b5c000..0x01410000) written to
 * /tmp/nfsu-mem-a.bin at that many seconds after start and /tmp/nfsu-mem-b.bin two seconds later, each with the
 * host time it was taken, from the main loop, to find counters that run fast against the clock. */
static void mem_dump(const PopModApi *api) {
    static const char *e = (const char *)1;
    if (e == (const char *)1)
        e = getenv("NFSU_MEMDUMP");
    if (!e)
        return;
    static int64_t start = 0;
    static int stage = 0;
    const int64_t now = now_ns();
    if (!start)
        start = now;
    const int64_t at = (int64_t)atoi(e) * 1000000000LL + (stage ? 2000000000LL : 0);
    if (stage > 1 || now - start < at)
        return;
    void *p = NULL;
    uint32_t lo = 0x00b5c000u, hi = 0x01410000u;
    if (getenv("NFSU_MEMDUMP_RANGE")) /* LO-HI in hex */
        sscanf(getenv("NFSU_MEMDUMP_RANGE"), "%x-%x", &lo, &hi);
    if (api->guest_ptr(api, lo, hi - lo, &p) != POP_OK || !p)
        return;
    FILE *f = fopen(stage ? "/tmp/nfsu-mem-b.bin" : "/tmp/nfsu-mem-a.bin", "wb");
    if (f) {
        fwrite(&now, 8, 1, f);
        fwrite(p, 1, hi - lo, f);
        fclose(f);
    }
    ++stage;
}
/* NFSU_SUBSYS=<seconds> (DIAGNOSTIC): log the objects the main-loop pass updates through vtable slot 3
 * (the list 0x00d3c12c..0x00d3c130) and each one's update function, once, that many seconds in. */
static void subsys_log(const PopModApi *api) {
    static const char *e = (const char *)1;
    static int64_t start;
    static int done;
    if (e == (const char *)1)
        e = getenv("NFSU_SUBSYS");
    if (!e || done)
        return;
    if (!start)
        start = now_ns();
    if (now_ns() - start < (int64_t)atoi(e) * 1000000000LL)
        return;
    done = 1;
    const uint32_t b = get_u32(0x00d3c12cu), en = get_u32(0x00d3c130u);
    char line[160];
    {
        const uint32_t opt = get_u32(0x00df1df4u);
        uint32_t flag = opt ? get_u32(opt + 0x198) : 0, raw = get_u32(0x00df22d8u);
        float stored;
        memcpy(&stored, &raw, 4);
        snprintf(line, sizeof line, "core.nfsu: aspect: options %08x flag %02x stored %.4f", opt, flag & 0xff, stored);
        api->log(api, line);
    }
    {
        const uint32_t fe = get_u32(0x013335ecu), apt = fe ? get_u32(fe + 0x34) : 0, avt = apt ? get_u32(apt) : 0;
        snprintf(line, sizeof line, "core.nfsu: ui manager %08x, its +0x34 %08x vtable %08x: %08x %08x %08x %08x %08x",
                 fe, apt, avt, avt ? get_u32(avt) : 0, avt ? get_u32(avt + 4) : 0, avt ? get_u32(avt + 8) : 0,
                 avt ? get_u32(avt + 12) : 0, avt ? get_u32(avt + 16) : 0);
        api->log(api, line);
    }
    for (uint32_t p = b; p < en && p - b < 4 * 64; p += 4) {
        const uint32_t obj = get_u32(p), vt = get_u32(obj);
        snprintf(line, sizeof line, "core.nfsu: subsystem %u obj %08x vtable %08x update %08x", (p - b) / 4, obj, vt,
                 get_u32(vt + 12));
        api->log(api, line);
    }
}
/* NFSU_DTLOG=1 (DIAGNOSTIC): once a second, what the last main-loop pass got as time (FUN_006aded0's results:
 * 0x00da52f0 pass seconds, 0x00da52a0 its 1/3600 s ticks), the 60 Hz frames it counted (0x00d3c0f4) and the
 * passes and real time since the last line. Read before FUN_006a66f0, which consumes them. */
static unsigned long g_menu_paced; /* main-loop passes the menu pacer held (NFSU_DTLOG prints them per second) */
static void dt_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    static int64_t last;
    static uint32_t passes;
    static double sum_dt;
    uint32_t raw = get_u32(0x00da52f0u);
    float dt;
    memcpy(&dt, &raw, 4);
    ++passes;
    sum_dt += dt;
    const int64_t now = now_ns();
    static int64_t prev_pass, longest;
    static unsigned long over;
    if (prev_pass) {
        const int64_t gap = now - prev_pass;
        longest = gap > longest ? gap : longest;
        over += gap > 9000000LL; /* longer than a 120 Hz refresh, with a little room */
    }
    prev_pass = now;
    if (!last)
        last = now;
    if (now - last < 1000000000LL)
        return;
    char line[320];
    snprintf(line, sizeof line,
             "core.nfsu: dt %u passes in %.3f s real, game dt sum %.3f s, last dt %.4f ticks %u frames60 %u "
             "fixedstep %u clampframes %u menu-paced %lu",
             passes, (now - last) / 1e9, sum_dt, dt, get_u32(0x00da52a0u), get_u32(0x00d3c0f4u),
             get_u32(0x00daa0fcu), get_u32(0x00d3c0e8u), g_menu_paced);
    g_menu_paced = 0;
    {
        char more[96];
        snprintf(more, sizeof more, " longest-pass %.1f ms over-9ms %lu", longest / 1e6, over);
        strncat(line, more, sizeof line - strlen(line) - 1);
        longest = 0;
        over = 0;
    }
    api->log(api, line);
    last = now;
    passes = 0;
    sum_dt = 0;
}
/* NFSU_APTLOG=1 (DIAGNOSTIC): the frame time the frontend (APT, FUN_00a0d5d0's 2nd argument, ms, from RDTSC
 * through FUN_00a1c960) receives, summed per second, beside the stopwatch calibration (0x00d71428 CPU Hz,
 * 0x00d71430 QPC Hz, 0x0135bf30.. unit factors). */
static void apt_dt(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    static int64_t last;
    static uint32_t n, sum;
    uint32_t ms = 0;
    api->guest_read_u32(api, cpu->esp + 8, &ms);
    ++n;
    sum += ms;
    const int64_t now = now_ns();
    if (!last)
        last = now;
    if (now - last < 1000000000LL)
        return;
    char line[400];
    {
        const uint32_t fe = get_u32(0x013335ecu), apt = fe ? get_u32(fe + 0x34) : 0, cfg = apt ? get_u32(apt + 4) : 0;
        snprintf(line, sizeof line, "core.nfsu: apt object %08x cfg %08x timed %08x fixed ms %u stamp %08x:%08x", apt,
                 cfg, cfg ? get_u32(cfg + 0x28) : 0, cfg ? get_u32(cfg + 0x2c) : 0, apt ? get_u32(apt + 0x14) : 0,
                 apt ? get_u32(apt + 0x10) : 0);
        api->log(api, line);
    }
    int k = snprintf(line, sizeof line, "core.nfsu: apt %u updates, %u ms in %.3f s; cpu Hz %u:%u qpc Hz %u:%u; f",
                     n, sum, (now - last) / 1e9, get_u32(0x00d7142cu), get_u32(0x00d71428u), get_u32(0x00d71434u),
                     get_u32(0x00d71430u));
    for (uint32_t a = 0x0135bf30u; a < 0x0135bf90u && k < (int)sizeof line - 20; a += 4) {
        uint32_t raw = get_u32(a);
        float f;
        memcpy(&f, &raw, 4);
        k += snprintf(line + k, sizeof line - k, " %g", f);
    }
    api->log(api, line);
    last = now;
    n = sum = 0;
}
static void apt_frame_time(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t obj = cpu->ecx;
    const uint32_t lo0 = get_u32(obj + 0x10), hi0 = get_u32(obj + 0x14);
    api->call_next(api, inv, cpu);
    static int64_t last;
    const int64_t now = now_ns();
    if (now - last < 1000000000LL)
        return;
    last = now;
    const uint32_t lo1 = get_u32(obj + 0x10), hi1 = get_u32(obj + 0x14);
    const uint64_t d = (((uint64_t)hi1 << 32) | lo1) - (((uint64_t)hi0 << 32) | lo0);
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: apt frame time: obj %08x stamp moved %llu ticks, returned %u", obj,
             (unsigned long long)d, cpu->eax);
    api->log(api, line);
}
/* NFSU_WAITLOG=1 (DIAGNOSTIC): how long the main thread waits for the render thread's reply flag [0x00ea0164]
 * in FUN_00769390 and FUN_00769460, per second. */
static void render_wait_time(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    const int which = (int)(uintptr_t)user;
    const int64_t t0 = now_ns();
    const uint32_t flag = get_u32(0x00ea0164u);
    api->call_next(api, inv, cpu);
    static int64_t total[2], last;
    static uint32_t calls[2], waited[2];
    total[which] += now_ns() - t0;
    ++calls[which];
    if (flag == 1)
        ++waited[which];
    const int64_t now = now_ns();
    if (!last)
        last = now;
    if (now - last >= 2000000000LL) {
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsu: render waits per s: 769390 %u calls (%u waited) %.1f ms, 769460 %u calls (%u waited) %.1f ms",
                 calls[0] / 2, waited[0] / 2, total[0] / 2e6, calls[1] / 2, waited[1] / 2, total[1] / 2e6);
        api->log(api, line);
        total[0] = total[1] = 0;
        calls[0] = calls[1] = waited[0] = waited[1] = 0;
        last = now;
    }
}
/* Loading screens run unpaced (2026-11-17): the menu limit below held them to 45 main-loop passes a second, and the
 * game's loader moves on once per pass, so a scene load took ~2 s longer (measured: Enter -> driving 4.5 s paced,
 * 2.5 s not). The world tick stands still on a loading screen as in a menu; what tells them apart is file reads:
 * the kit counts the bytes the game reads (recomp_file_bytes_read). Menus stream music in small amounts; a load
 * reads megabytes a second. Over LOAD_READ_BYTES in a 100 ms window counts as loading for the next 500 ms. */
extern uint64_t recomp_file_bytes_read(void) __attribute__((weak));
extern void recomp_present_unpaced_for(uint32_t ms) __attribute__((weak));
/* HDR (2026-11-18): the 3D scene is live - the world ticking, so a race runs; menus and pauses stop it (kit
 * d3d9_metal.mm). Only then does the frame's composite lift its highlights above white. */
extern void recomp_hdr_scene_live(int live) __attribute__((weak));
/* Reads over 2 MB in the last ~100 ms (a scene load), sampled each main-loop pass. */
static int fast_reads(int64_t now) {
    if (!recomp_file_bytes_read)
        return 0;
    static uint64_t base;
    static int64_t base_ns, fast_until;
    const uint64_t b = recomp_file_bytes_read();
    if (!base_ns || now - base_ns > 300000000LL) { /* first call, or not asked for a while: start again */
        base = b;
        base_ns = now;
        return now < fast_until;
    }
    if (now - base_ns >= 100000000LL) {
        if (b - base > 2u * 1024u * 1024u)
            fast_until = now + 300000000LL;
        base = b;
        base_ns = now;
    }
    return now < fast_until;
}
#define LOAD_READ_BYTES (256u * 1024u)
static int64_t g_load_seen_ns;
static int loading_now(const PopModApi *api, int64_t now) {
    if (!recomp_file_bytes_read || getenv("NFSU_PACE_LOADS"))
        return 0;
    static uint64_t window_bytes;
    static int64_t window_ns;
    const uint64_t bytes = recomp_file_bytes_read();
    if (!window_ns) {
        window_ns = now;
        window_bytes = bytes;
    }
    if (now - window_ns >= 100000000LL) {
        /* a window much longer than 100 ms (the limit was not asked while driving) is only restarted */
        if (now - window_ns <= 300000000LL && bytes - window_bytes > LOAD_READ_BYTES)
            g_load_seen_ns = now;
        static int log = -1;
        if (log < 0)
            log = getenv("NFSU_READLOG") != NULL;
        if (log && bytes != window_bytes) {
            char line[120];
            snprintf(line, sizeof line, "core.nfsu: read %llu KB in %.0f ms%s",
                     (unsigned long long)((bytes - window_bytes) / 1024), (now - window_ns) / 1e6,
                     bytes - window_bytes > LOAD_READ_BYTES ? " (loading)" : "");
            api->log(api, line);
        }
        window_ns = now;
        window_bytes = bytes;
    }
    return g_load_seen_ns && now - g_load_seen_ns < 500000000LL;
}
/* NFSU_DIALOG_LOG=1 (DIAGNOSTIC): every front-end message box the game builds (FUN_005a3be0 with buttons,
 * FUN_005a3d80 without) - its text and the code addresses on the stack above it, to find who asks for one. */
static void dialog_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    char text[160] = {0}, line[512];
    read_guest_str(get_u32(cpu->esp + 4), text, sizeof text);
    int n = snprintf(line, sizeof line, "core.nfsu: dialog %s \"%s\" from", user ? "005a3d80" : "005a3be0", text);
    int found = 0;
    for (uint32_t i = 0; i < 400 && found < 10 && n < (int)sizeof line - 12; ++i) {
        const uint32_t v = get_u32(cpu->esp + 4 * i);
        if (v >= 0x00401000u && v < 0x00bd6000u) {
            n += snprintf(line + n, sizeof line - n, " %08x", v);
            ++found;
        }
    }
    api->log(api, line);
}
/* NFSU_DIALOG_LOG: the front-end flow state FUN_005965f0 is entered with ([this+8]). */
static void fe_state_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    char line[160];
    snprintf(line, sizeof line, "core.nfsu: front-end state %u (this %08x, from %08x)", get_u32(cpu->ecx + 8), cpu->ecx,
             get_u32(cpu->esp));
    api->log(api, line);
}
/* NFSU_DIALOG_LOG: the online sign-in's message step FUN_005b1830 (fastcall, ecx = the sign-in object). */
static void login_msg_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: sign-in message: this %08x vtable %08x state %u error %u, from %08x %08x",
             cpu->ecx, get_u32(cpu->ecx), get_u32(cpu->ecx + 4), get_u32(cpu->ecx + 0x4c), get_u32(cpu->esp),
             get_u32(cpu->esp + 4));
    api->log(api, line);
}
static void login_ctor_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    char line[160];
    snprintf(line, sizeof line, "core.nfsu: %s this %08x arg %u from %08x", (const char *)user, cpu->ecx,
             get_u32(cpu->esp + 4), get_u32(cpu->esp));
    api->log(api, line);
}
/* EA NATION SIGN-IN (2026-11-18; setting ea_nation_signin, default off). After a save loads, the start flow's step
 * FUN_005e7b40 (__fastcall, ecx = the flow object, its step in [ecx+4]) reaches step 0xb and creates the PC sign-in
 * (FUN_00750310(2), "FEOnlineLoginStateManager FEPCOnlineLoginStateManager") as child step 0xd: "Connecting to EA
 * Nation servers", then "Could not connect to EA Nation" (FUN_005b1830 state 42), waiting for Enter - every start,
 * as the service is gone. The same step has its own way past the sign-in when there is nothing to sign in with: the
 * flow object's method at vtable +0x180, also what steps 1 and 4 call to go on. With the setting off, step 0xb does
 * what the function does in every case first (the owner [ecx+0x58]'s vtable +0xc; an 8-byte object from
 * FUN_00544fd0(8,0) built by FUN_00669720) and then takes that way on; other steps run unchanged. NFSU_EA_NATION=1
 * forces the game's own behaviour. */
#define START_FLOW_STEP 0x005e7b40u
static void start_flow_step(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    static int64_t read_at;
    static int64_t on = -1;
    const int64_t now = now_ns();
    if (on < 0 || now - read_at > 1000000000LL) {
        read_at = now;
        on = 0;
        if (getenv("NFSU_EA_NATION"))
            on = atoi(getenv("NFSU_EA_NATION")) != 0;
        else if (api->settings_get)
            api->settings_get(api, "ea_nation_signin", &on);
    }
    const uint32_t self = cpu->ecx;
    if (on || get_u32(self + 4) != 0x0bu) {
        api->call_next(api, inv, cpu);
        return;
    }
    const uint32_t owner = get_u32(self + 0x58);
    if (owner)
        api->guest_call(api, get_u32(get_u32(owner) + 0x0c), owner, NULL, 0, NULL);
    const uint32_t args[2] = {8, 0};
    uint32_t obj = 0;
    api->guest_call(api, 0x00544fd0u, 0, args, 2, &obj);
    if (obj)
        api->guest_call(api, 0x00669720u, obj, NULL, 0, NULL);
    api->guest_call(api, get_u32(get_u32(self) + 0x180), self, NULL, 0, NULL);
    static int logged;
    if (!logged++)
        api->log(api, "core.nfsu: EA Nation sign-in after the save load skipped (setting ea_nation_signin off)");
    api->hook_return(api, cpu, 0, 0); /* __fastcall, no stack arguments */
}
/* The play journal (2026-11-21, the person: fps, heat, what lowers fps, every key, every action of the game, the
 * closing time - "everything"). The runtime's recomp_journal (RECOMP_JOURNAL, Logs/<stamp>.journal.txt) gets, from
 * here: the game state [0x00da57b8] when it changes, the world running or standing (menus, the pause, the map,
 * videos and loads stop its tick counter 0x00d3c108), loading screens (the game reading > 2 MB per 100 ms with the
 * world standing), and once a second the main loop's passes, the longest pass and the passes over 10 ms (the
 * main thread's share of a slow frame). The host writes keys, pad, window, perf and render lines; mod log lines and
 * the game's own debug text go there too. */
extern void recomp_journal(const char *fmt, ...) __attribute__((weak));
static void journal_pass(int64_t now) {
    if (!recomp_journal)
        return;
    static int64_t last_pass, sec_at, longest;
    static unsigned passes, slow;
    if (last_pass) {
        const int64_t d = now - last_pass;
        if (d > longest)
            longest = d;
        slow += d > 10000000LL;
    }
    last_pass = now;
    ++passes;
    static uint32_t state = 0xffffffffu;
    const uint32_t st = get_u32(0x00da57b8u);
    if (st != state) {
        if (state == 0xffffffffu)
            recomp_journal("game: state %u at start", st);
        else
            recomp_journal("game: state %u -> %u%s", state, st, st == 6 ? " (in the world)" : "");
        state = st;
    }
    static int running = -1, loading = 0;
    const int run = now - g_world_tick_ns < 500000000LL;
    if (run != running) {
        recomp_journal(run ? "game: world running (driving)" : "game: world stopped (menu, pause, map, video or load)");
        running = run;
    }
    static int64_t load_at, load_seen;
    if (!run && fast_reads(now)) {
        load_seen = now;
        if (!loading)
            load_at = now, loading = 1, recomp_journal("game: loading started");
    } else if (loading && now - load_seen > 1500000000LL) {
        recomp_journal("game: loading finished after %.1f s", (load_seen - load_at) / 1e9);
        loading = 0;
    }
    if (!sec_at)
        sec_at = now;
    if (now - sec_at >= 1000000000LL) {
        recomp_journal("main: %u passes/s, longest %.1f ms, %u over 10 ms, game state %u, %s", passes,
                       longest / 1e6, slow, st, run ? "driving" : "world stopped");
        sec_at = now, passes = 0, slow = 0, longest = 0;
    }
}
static void menu_pace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    mem_dump(api);
    subsys_log(api);
    { /* menu_fps applies live (2026-11-17): read once a second */
        static int64_t read_at;
        const int64_t t0 = now_ns();
        if (!g_menu_fps_env && (!read_at || t0 - read_at > 1000000000LL) && api->settings_get) {
            read_at = t0;
            int64_t menu = 0;
            api->settings_get(api, "menu_fps", &menu);
            static const int fps_of[4] = {45, 30, 60, 0};
            const int fps = fps_of[menu >= 0 && menu <= 3 ? menu : 0];
            g_menu_period_ns = fps > 0 ? 1000000000LL / fps : 0;
        }
    }
    const int64_t period = g_menu_period_ns, now = now_ns();
    const uint32_t ticks = get_u32(WORLD_TICKS);
    if (ticks != g_world_ticks) {
        g_world_ticks = ticks;
        g_world_tick_ns = now;
    }
    /* A loading screen's frames go out without waiting for the screen (2026-11-18): the loader advances once per
     * main-loop pass and each pass waited for vertical sync (Enter -> driving 3.5 s, the main thread ~70% idle at
     * first). The world stopped for 250 ms and the game reading over 2 MB per 100 ms - a scene load reads 10-22 MB,
     * the warning screen's video 0.34 MB, the menus 0.02 MB; the city streaming in while driving keeps the world
     * ticking. NFSU_LOAD_PACED=1 = old. */
    journal_pass(now);
    if (recomp_hdr_scene_live)
        recomp_hdr_scene_live(now - g_world_tick_ns < 250000000LL);
    if (recomp_present_unpaced_for && now - g_world_tick_ns > 250000000LL && fast_reads(now) &&
        !getenv("NFSU_LOAD_PACED"))
        recomp_present_unpaced_for(300);
    if (!period)
        return; /* "not limited" */
    if (now - g_world_tick_ns < g_menu_idle_ns || loading_now(api, now)) {
        /* the world runs, or a loading screen reads files: the frame limit alone decides */
        g_menu_due_ns = 0;
        return;
    }
    const uint32_t sleep = get_u32(IAT_SLEEP);
    ++g_menu_paced;
    if (g_menu_due_ns && now < g_menu_due_ns && sleep) {
        const uint32_t args[1] = {(uint32_t)((g_menu_due_ns - now) / 1000000)};
        if (args[0])
            api->guest_call(api, sleep, 0, args, 1, NULL);
        while (now_ns() < g_menu_due_ns - 200000) /* the last part of a millisecond: give the baton away */
            yield_baton(api);
    }
    /* The next slot one period on; a pass that came late starts the count again rather than catching up. */
    const int64_t t = now_ns();
    g_menu_due_ns = g_menu_due_ns && t - g_menu_due_ns < period ? g_menu_due_ns + period : t + period;
}

/* The render thread on its own core (RECOMP_FREE_THREADS lists "RenderThread"). Waiting for work it loops
 * (00791fc0): WaitForSingleObject(work, 0), then the window's message pump (FUN_0074be20: SendInput of a zero mouse
 * move to keep the display awake, PeekMessage...), then SleepEx(0) (FUN_009a9f80). On Windows that is a cheap spin;
 * here the pump's user32 calls take the baton from the main thread, which they did ~100000 times a second. So on
 * a free render thread the pump runs at most every 10 ms and the idle SleepEx(0) sleeps 0.05 ms (2026-11-15). */
#define MESSAGE_PUMP 0x0074be20u
#define SLEEP_HELPER 0x009a9f80u
#define RENDER_IDLE_RETURN 0x00791ff6u
static int g_render_free;
static int64_t pump_ns = 10000000LL; /* NFSU_PUMP_MS (A/B) */
static long idle_ns = 50000;         /* NFSU_IDLE_US (A/B) */
/* NEVER BLOCK IN THE PUMP (2026-11-17). FUN_0074be20 calls GetMessageA - waits for the next window message -
 * instead of draining with PeekMessageA while [0x00df9a94] and [0x00df2e48] are both set; the window procedure
 * FUN_00753100 sets [0x00df2e48] on WM_ACTIVATEAPP(0) (the Mac display sleeping, the window minimised or another
 * app in front) and clears it on WM_ACTIVATEAPP(1). This pump runs on the render thread; the host hands the game
 * input, the reactivation included, from the main thread's clock reads, and the main thread waits for the render
 * thread - so GetMessageA waited forever and the game hung until force-quit (reproduced: display sleep, sample
 * showed the render thread in GetMessageA, the main thread in its Sleep loop). Here, while the game is inactive,
 * [0x00df9a94] (read only by this pump; FUN_0074bb10 sets it) is cleared for the one call, so the pump drains
 * with PeekMessageA, then put back, and the thread naps 16 ms through the game's Sleep so an inactive game idles.
 * [0x00df2e48] is left as the game set it: its own pause and the WM_ACTIVATEAPP(1) that ends it work as before. */
#define PUMP_BLOCKS 0x00df9a94u
#define PUMP_INACTIVE 0x00df2e48u
static void guest_sleep(const PopModApi *api, uint32_t ms) {
    const uint32_t sleep = get_u32(IAT_SLEEP);
    if (!sleep)
        return;
    const uint32_t args[1] = {ms};
    api->guest_call(api, sleep, 0, args, 1, NULL);
}
static void pump_throttle(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    static int64_t last;
    const int64_t now = now_ns();
    const uint32_t blocks = get_u32(PUMP_BLOCKS);
    const int inactive = blocks && get_u32(PUMP_INACTIVE);
    if (!inactive && g_render_free && now - last < pump_ns) {
        api->hook_return(api, cpu, 0, 0); /* __thiscall, no stack arguments */
        return;
    }
    last = now;
    if (!inactive) {
        api->call_next(api, inv, cpu);
        return;
    }
    static int told;
    if (!told) {
        told = 1;
        api->log(api, "core.nfsu: the game is inactive: its message pump drains instead of waiting (first time)");
    }
    api->guest_write_u32(api, PUMP_BLOCKS, 0);
    api->call_next(api, inv, cpu);
    if (!get_u32(PUMP_BLOCKS)) /* unless the game set it meanwhile */
        api->guest_write_u32(api, PUMP_BLOCKS, blocks);
    guest_sleep(api, 16);
}
/* The other half: the WM_ACTIVATEAPP(1) that ends the pause is input the host queued, and the host hands queued
 * input to the game from the main thread's clock reads (or when no hook frame is live). While inactive, the main
 * thread only sleeps in FUN_009a9f80 waiting for the render thread, inside hooked functions, so the message stayed
 * queued and the game stayed paused after the display woke (reproduced). While the game is inactive this reads the
 * game's clock (timeGetTime through the import table) before each of those sleeps, which is the read that hands it
 * over; on other threads the host does nothing with it. */
#define IAT_TIMEGETTIME 0x00bd65e4u /* WINMM!timeGetTime in nfs.exe's import table */
static void inactive_heartbeat(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (!get_u32(PUMP_BLOCKS) || !get_u32(PUMP_INACTIVE))
        return;
    const uint32_t clock = get_u32(IAT_TIMEGETTIME);
    if (clock)
        api->guest_call(api, clock, 0, NULL, 0, NULL);
}
static void render_idle_sleep(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (get_u32(cpu->esp) != RENDER_IDLE_RETURN)
        return;
    const struct timespec ts = {0, idle_ns};
    nanosleep(&ts, NULL);
}

/* MAC RESOLUTIONS. The game's resolution menu (Options > Display) is its own table of 14 sizes at 0x00be9658
 * ({width, height, label hash}, 640x480 .. 2560x1600); FUN_00555f70 keeps those the adapter lists (and the
 * accepted-modes table 0x00c1f2a0, {width, height} x15, FUN_00752fc0, also has), and the menu shows each one's
 * caption through FUN_00586630(label hash) (0x0059d10b). The whole table is rewritten: the MacBook Pro screens in
 * pixels and in points, each also as the part a full-screen window fills below the notch (14-inch 3024x1964 /
 * 3024x1900 / 1512x982 / 1512x950, 16-inch 3456x2234 / 3456x2160 / 1728x1117 / 1728x1080), beside the usual
 * 16:9 and 16:10 sizes; sizes no Mac has (640x480 .. 1280x1024, 1600x1200) give their places up. The new rows
 * carry label hashes of their own that the caption hook answers. The runtime lists the same sizes as adapter
 * modes. Off with core.nfsu/mac_resolutions = false. */
#define RES_TABLE 0x00be9658u
#define MODE_TABLE 0x00c1f2a0u
#define RES_LABEL 0x00586630u
#define MAC_HASH 0x4d414300u /* "MAC\0" + row: no game label hashes this way */
static const struct {
    uint32_t w, h, hash; /* hash 0: one of ours */
} k_res_rows[14] = {
    {1280, 720, 0xcf0b8101u}, {1440, 900, 0x5fab603fu}, {1512, 950, 0},  {1512, 982, 0},
    {1680, 1050, 0x617fa2b2u}, {1728, 1080, 0},         {1728, 1117, 0}, {1920, 1080, 0x5867b532u},
    {1920, 1200, 0x5867bcacu}, {2560, 1600, 0x4fc5de31u}, {3024, 1900, 0}, {3024, 1964, 0},
    {3456, 2160, 0},           {3456, 2234, 0},
};
static uint32_t g_res_labels[14]; /* guest addresses of our captions */
static void res_label(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t hash = get_u32(cpu->esp + 4);
    if ((hash & 0xffffff00u) == MAC_HASH && (hash & 0xffu) < 14 && g_res_labels[hash & 0xffu]) {
        api->hook_return(api, cpu, g_res_labels[hash & 0xffu], 0); /* __cdecl */
        return;
    }
    api->call_next(api, inv, cpu);
}
static void mac_resolutions(const PopModApi *api) {
    void *p = NULL;
    if (api->guest_ptr(api, RES_TABLE, 14 * 12, &p) != POP_OK || !p)
        return;
    uint32_t *res = (uint32_t *)p;
    if (api->guest_ptr(api, MODE_TABLE, 15 * 8, &p) != POP_OK || !p)
        return;
    uint32_t *acc = (uint32_t *)p;
    char line[400];
    int n = snprintf(line, sizeof line, "core.nfsu: resolution menu:");
    for (int i = 0; i < 14; ++i) {
        uint32_t hash = k_res_rows[i].hash;
        if (!hash) {
            char text[32];
            const int len = snprintf(text, sizeof text, "%ux%u", k_res_rows[i].w, k_res_rows[i].h);
            uint32_t at = 0;
            void *dst = NULL;
            if (api->guest_alloc && api->guest_alloc(api, (uint32_t)len + 1, &at) == POP_OK && at &&
                api->guest_ptr(api, at, (uint32_t)len + 1, &dst) == POP_OK && dst) {
                memcpy(dst, text, (size_t)len + 1);
                g_res_labels[i] = at;
                hash = MAC_HASH | (uint32_t)i;
            } else {
                continue; /* no caption: keep the row as it was */
            }
        }
        res[3 * i] = k_res_rows[i].w;
        res[3 * i + 1] = k_res_rows[i].h;
        res[3 * i + 2] = hash;
        acc[2 * i] = k_res_rows[i].w;
        acc[2 * i + 1] = k_res_rows[i].h;
        n += snprintf(line + n, sizeof line - n, " %ux%u", k_res_rows[i].w, k_res_rows[i].h);
    }
    acc[2 * 14] = 1280, acc[2 * 14 + 1] = 800; /* the 15th accepted size */
    static uint32_t label_hook;
    api->hook_install(api, RES_LABEL, res_label, POP_HOOK_REPLACE, NULL, &label_hook);
    api->log(api, line);
}

/* WIDESCREEN. The game lays its 3D view and HUD out for 4:3 or for a wide screen from GetAspect (FUN_0076b7f0):
 * the options' width / height only while a flag nothing sets is on ([[0x00df1df4]+0x198]), otherwise a stored 4/3
 * (0x00df22d8). So every resolution was drawn as 4:3 and stretched to the screen: the car and the HUD circles 1.33x,
 * 1.20x and 1.16x too wide at 16:9, 16:10 and 3024x1964 (measured 2026-11-16: their width/height divided by the
 * screen's was the same constant at all three). Now GetAspect answers the real width / height, which switches the
 * game's own widescreen layout on (IsWidescreen FUN_007454e0: aspect > 1.34).
 *
 * The 3D view (FUN_0074a710, the projection): its widescreen branch was tuned for 16:9 - the vertical half-angle is
 * the field-of-view angle x 0.5 x 1.48 / aspect, the horizontal one the angle x 0.5 x (1.175 - (degrees - 10) x
 * 0.003) x 1.24 - which is near right at 16:9 only. Hooked at their call sites, the sine and cosine it takes of the
 * two half-angles get: vertical = the game's at 16:9 (angle x 0.5 x 1.48 / (16/9)), the same picture height on every
 * screen; horizontal = atan(tan(vertical) x aspect), exactly the screen's shape (a wider screen sees more at the
 * sides). The HUD's wide layout (1280 x 720 units: 0x00c21b00 width, 0x00c22620 / 0x00c21f20 height) is fitted
 * to the screen at one scale, so its circles stay round and nothing leaves the screen: narrower than 16:9 it is
 * 1280 / aspect tall (top-anchored, so the bottom gauges sit a little higher), wider it is 720 x aspect wide.
 * Off with core.nfsu/widescreen = false. */
#define GET_ASPECT 0x0076b7f0u
#define OPTIONS_GET 0x00745970u /* thiscall (0x00df1de0, setting): 0x14 width, 0x15 height */
#define PROJECTION_WIDE_CHECK_RET 0x0074a811u
#define SIN_ANGLE 0x004b0cf0u   /* float bSin(uint16 angle) */
#define COS_ANGLE 0x004b0b80u   /* float bCos(uint16 angle) */
#define HUD_WIDE_WIDTH 0x00c21b00u
#define HUD_Y_SCALE 0x00c22620u /* float -2/720: the HUD matrix's y scale (FUN_0076cfd0) */
#define HUD_HEIGHT 0x00c21f20u  /* double 720: the HUD vertices' height (FUN_007643c0, 0077ad21, 0077b330) */
static float g_aspect = 4.0f / 3.0f;
static int g_hud_fill = 1;     /* setting hud_fill: menus may fill the height */
static int g_hud_filling = -1; /* menus now: 1 filled (720 units stretched to the height), 0 not, -1 not set */
static uint32_t g_screen_rows = 1080;
static uint32_t g_hud_ticks;
static int64_t g_hud_tick_ns;
static int g_proj_wide;            /* this projection took the widescreen branch */
static uint32_t g_proj_h, g_proj_v; /* the half-angles it gets instead, 65536 = 360 degrees */
static void update_aspect(const PopModApi *api) {
    uint32_t w = 0, h = 0, arg = 0x14;
    if (api->guest_call(api, OPTIONS_GET, 0x00df1de0u, &arg, 1, &w) != POP_OK)
        return;
    arg = 0x15;
    if (api->guest_call(api, OPTIONS_GET, 0x00df1de0u, &arg, 1, &h) != POP_OK)
        return;
    if (w >= 320 && h >= 200 && w <= 16384 && h <= 16384) {
        const float a = (float)w / (float)h;
        /* Menus, the pause map and loading screens (the world's tick counter has stood still for 300 ms) fill
         * the height; driving keeps the whole 16:9 layout, whose gauges are placed for 1280 units across. */
        const uint32_t ticks = get_u32(WORLD_TICKS);
        const int64_t now = now_ns();
        if (ticks != g_hud_ticks || !g_hud_tick_ns) {
            g_hud_ticks = ticks;
            g_hud_tick_ns = now;
        }
        (void)now;
        if (a != g_aspect) {
            g_aspect = a;
            /* The wide HUD layout spans 1280 x 720. Narrower than 16:9 it keeps all 1280 across and gets taller
             * instead (1280 / aspect), so nothing leaves the screen and its circles stay round; wider, it gets
             * wider (720 x aspect). */
            /* g_hud_fill (setting hud_fill, 2026-11-17): narrower than 16:9 too, the layout keeps 720 units of
             * height and is 720 x aspect wide, so the menus' and map's full-screen backgrounds (drawn for 720)
             * reach the top and bottom instead of leaving bands; the game's wide layout places its elements by
             * that width. */
            /* The layout's numbers always follow the real aspect: the game fixes its HUD's places and sizes when
             * it builds the HUD during loading, so a 720-tall layout then made the driving HUD 15% too tall
             * (2026-11-17, the person's oval minimap). Filling the height in menus is done in the HUD matrix
             * only (hud_matrix_leave), which the game rebuilds every frame. */
            const float wide = a > 16.0f / 9.0f ? 720.0f * a : 1280.0f;
            const float tall = a > 16.0f / 9.0f ? 720.0f : (a > 1.34f ? 1280.0f / a : 720.0f);
            g_screen_rows = h;
            const float yscale = -2.0f / tall;
            const double tall_d = tall;
            uint32_t bits;
            memcpy(&bits, &wide, 4);
            api->guest_write_u32(api, HUD_WIDE_WIDTH, bits);
            memcpy(&bits, &yscale, 4);
            api->guest_write_u32(api, HUD_Y_SCALE, bits);
            uint32_t d[2];
            memcpy(d, &tall_d, 8);
            api->guest_write_u32(api, HUD_HEIGHT, d[0]);
            api->guest_write_u32(api, HUD_HEIGHT + 4, d[1]);
            char line[160];
            snprintf(line, sizeof line, "core.nfsu: widescreen: %ux%u, aspect %.4f, HUD %.1f x %.1f", w, h, a, wide,
                     tall);
            api->log(api, line);
        }
    }
}
static void get_aspect(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t ret = get_u32(cpu->esp);
    { /* NFSU_ASPECT_LOG=1 (DIAGNOSTIC): who asks for the aspect, in menus (world still) and driving */
        static int on = -1;
        if (on < 0)
            on = getenv("NFSU_ASPECT_LOG") != NULL;
        if (on) {
            static uint32_t seen[128];
            static int n;
            const uint32_t still = (now_ns() - g_hud_tick_ns > 300000000LL) ? 1u : 0u;
            const uint32_t key = ret | still << 31;
            int found = 0;
            for (int i = 0; i < n; ++i)
                found |= seen[i] == key;
            if (!found && n < 128) {
                seen[n++] = key;
                char line[120];
                snprintf(line, sizeof line, "core.nfsu: GetAspect from %08x (%s)", ret, still ? "menu" : "driving");
                api->log(api, line);
            }
        }
    }
    api->call_next(api, inv, cpu);
    update_aspect(api);
    cpu->st[cpu->fpu_top & 7u] = (double)g_aspect;
    if (ret == PROJECTION_WIDE_CHECK_RET)
        g_proj_wide = g_aspect > 1.34f;
}
static uint32_t angle_units(double radians) {
    long v = lround(radians * 65536.0 / (2.0 * M_PI));
    return (uint32_t)(v < 0 ? 0 : v > 0x7fff ? 0x7fff : v);
}
/* The horizontal half-angle's sine (call at 0x0074a929) and cosine (0x0074a933). */
static void proj_h(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (!g_proj_wide || (get_u32(cpu->edi + 4) == 0x0fu))
        return;
    if (user) { /* the sine comes first: work both angles out */
        const double half = (double)(cpu->ebp & 0xffffu) * 0.5;
        const uint32_t v = (uint32_t)(half * (1.48 / (16.0 / 9.0)));
        const double tv = tan((double)(v & 0xffffu) * 2.0 * M_PI / 65536.0);
        g_proj_v = v & 0xffffu;
        g_proj_h = angle_units(atan(tv * (double)g_aspect));
    }
    api->guest_write_u32(api, cpu->esp + 4, g_proj_h);
}
/* The vertical half-angle's sine (0x0074a98d) and cosine (0x0074a997). */
static void proj_v(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (!g_proj_wide || (get_u32(cpu->edi + 4) == 0x0fu))
        return;
    api->guest_write_u32(api, cpu->esp + 4, g_proj_v);
    if (!user)
        g_proj_wide = 0; /* the cosine is the projection's last use of them */
}
/* The HUD matrix (FUN_0076cfd0 builds it at [this+0x30]: translation (-1, +1) times the scale 2/width,
 * -2/height): moved down so a HUD taller than 720 units is centred rather than hung from the top. */
#define HUD_MATRIX_BUILD 0x0076cfd0u
static uint32_t g_hud_matrix_this;
static void hud_matrix_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    g_hud_matrix_this = cpu->ecx;
}
/* The game builds this matrix only when it builds its HUD (at a load, or when the widescreen mode changes), not
 * every frame (2026-11-17: a menu stretch written here during a load stayed in the driving HUD - oval minimap). So
 * the driving values are remembered here and hud_apply puts the right ones in each main-loop pass. */
static uint32_t g_hud_m;                 /* the matrix's guest address (0: none built yet) */
static float g_hud_drive_ys, g_hud_drive_ty; /* its y scale and y translation for driving */
static float g_hud_drive_xs, g_hud_drive_tx; /* its x scale and x translation for driving */
static int g_hud_applied = -1;           /* 1 menu stretch in place, 0 driving values, -1 to be written */
static void hud_apply(const PopModApi *api) {
    if (!g_hud_m)
        return;
    { /* hud_fill applies live (2026-11-17): read once a second */
        static int64_t read_at;
        const int64_t t = now_ns();
        if ((!read_at || t - read_at > 1000000000LL) && api->settings_get) {
            read_at = t;
            int64_t fill = 1;
            api->settings_get(api, "hud_fill", &fill);
            if (!getenv("NFSU_HUD_FILL") && (fill != 0) != g_hud_fill) {
                g_hud_fill = fill != 0;
                g_hud_applied = -1;
            }
        }
    }
    const uint32_t ticks = get_u32(WORLD_TICKS);
    const int64_t now = now_ns();
    if (ticks != g_hud_ticks || !g_hud_tick_ns) {
        g_hud_ticks = ticks;
        g_hud_tick_ns = now;
    }
    /* Menus, the map and loading screens (the world's tick counter has stood still for 300 ms). */
    const int fill = g_hud_fill && g_aspect > 1.34f && g_aspect < 16.0f / 9.0f && now - g_hud_tick_ns > 300000000LL;
    /* The change between the driving layout and the menus' fill is a short zoom, not a jump (2026-11-18, the
     * person: the jump on entering and leaving a menu): the matrix moves from one to the other over HUD_ZOOM_NS with
     * an ease in and out, one step each main-loop pass. A forced re-apply (g_hud_applied -1: a new aspect, the
     * setting) still snaps. NFSU_HUD_ZOOM_MS=0 = the old jump. */
    static double zoom = -1.0; /* 0 driving .. 1 menus */
    static int64_t zoom_ns;
    static int64_t zoom_len = -1;
    if (zoom_len < 0)
        zoom_len = getenv("NFSU_HUD_ZOOM_MS") ? (int64_t)atoi(getenv("NFSU_HUD_ZOOM_MS")) * 1000000LL : 320000000LL;
    const double goal = fill ? 1.0 : 0.0;
    if (g_hud_applied == -1 || zoom < 0.0 || zoom_len <= 0) {
        zoom = goal;
    } else {
        if (zoom == goal && fill == g_hud_applied)
            return;
        const double step = zoom_ns ? (double)(now - zoom_ns) / (double)zoom_len : 1.0;
        zoom = goal > zoom ? (zoom + step < goal ? zoom + step : goal) : (zoom - step > goal ? zoom - step : goal);
    }
    zoom_ns = now;
    g_hud_applied = fill;
    g_hud_filling = fill;
    float ys = g_hud_drive_ys, ty = g_hud_drive_ty, xs = g_hud_drive_xs, tx = g_hud_drive_tx;
    if (zoom > 0.0) {
        /* Their 720 units fill the whole height, one pixel past each edge so no row of the band is left, at the
         * SAME scale across (2026-11-17: stretched 12% taller the EA logo was an oval): the 1280-unit layout is
         * centred and loses (1280 - 720 x aspect) / 2 units at each side - 67 of 1280 on 16:10, inside the
         * margins the game's menus keep. */
        /* Checked 2026-11-18 against the game's own 16:9 layout (the same save screen at 1920x1080, where the
         * widescreen code changes nothing): its panel is 330 x 303 units there and here alike, so this mapping keeps
         * the menus' proportions. (Mapping the layout's 1280/aspect height instead made it 15% too flat and left the
         * loading screens' backgrounds short of the bottom.) */
        const float d = 2.0f / (float)(g_screen_rows ? g_screen_rows : 1080);
        const float fys = -(2.0f + 2.0f * d) / 720.0f, fty = 1.0f + d;
        const float fxs = (2.0f + 2.0f * d) / (720.0f * g_aspect), ftx = -640.0f * fxs;
        const float e = (float)(zoom * zoom * (3.0 - 2.0 * zoom)); /* ease in and out */
        ys += (fys - ys) * e;
        ty += (fty - ty) * e;
        xs += (fxs - xs) * e;
        tx += (ftx - tx) * e;
    }
    uint32_t b;
    memcpy(&b, &xs, 4);
    api->guest_write_u32(api, g_hud_m + 0 * 4, b);
    memcpy(&b, &tx, 4);
    api->guest_write_u32(api, g_hud_m + 12 * 4, b);
    memcpy(&b, &ys, 4);
    api->guest_write_u32(api, g_hud_m + 5 * 4, b);
    memcpy(&b, &ty, 4);
    api->guest_write_u32(api, g_hud_m + 13 * 4, b);
}
/* Draw distance (core.nfsu draw_distance, live; 2026-11-19, the person: shadows and small objects drawn much farther).
 * game.toml moves FUN_00780dc0's three stores of the shadow cascade distances (near, mid, far: 5/30/170 m as the game
 * sets them) from 0x00d53f38..40 to the mod slots 0x01375800..08, and FUN_0082b070's per-view scenery layer mask read
 * to the slot 0x0137580c. Each main-loop pass this puts into the array the game's own distances (copied from the
 * slots) or farther ones, and into the slot the game's mask 0x0e or 0x0f (layer bit 0 as well - GenericFix's
 * ImproveSceneryLOD). Level 1 "farther": cascades at least 15/70/320 m; level 2 "much farther": 25/120/500 m. */
#define CSM_GAME_SLOT 0x01375800u
#define CSM_ARRAY 0x00d53f38u
#define SCENERY_MASK_SLOT 0x0137580cu
static void draw_distance_apply(const PopModApi *api) {
    static int64_t level = -1, read_at;
    const int64_t t = now_ns();
    if ((level < 0 || t - read_at > 1000000000LL) && api->settings_get) {
        read_at = t;
        int64_t v = 2;
        api->settings_get(api, "draw_distance", &v);
        if (getenv("NFSU_DRAW_DISTANCE"))
            v = atoi(getenv("NFSU_DRAW_DISTANCE"));
        if (v != level) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsu: draw distance level %d", (int)v);
            api->log(api, line);
        }
        level = v;
    }
    static const float lo[3][3] = {{0, 0, 0}, {15.0f, 70.0f, 320.0f}, {25.0f, 120.0f, 500.0f}};
    const int l = level < 0 ? 0 : level > 2 ? 2 : (int)level;
    for (int k = 0; k < 3; ++k) {
        const uint32_t raw = get_u32(CSM_GAME_SLOT + 4u * (uint32_t)k);
        float g;
        memcpy(&g, &raw, 4);
        /* the slot holds the game's value once FUN_00780dc0 has run; "PADDINGXX" text before that */
        if (!(g > 0.5f && g < 100000.0f))
            g = k == 0 ? 5.0f : k == 1 ? 30.0f : 170.0f;
        const float v = g > lo[l][k] ? g : lo[l][k];
        uint32_t b;
        memcpy(&b, &v, 4);
        if (get_u32(CSM_ARRAY + 4u * (uint32_t)k) != b)
            api->guest_write_u32(api, CSM_ARRAY + 4u * (uint32_t)k, b);
    }
    const uint32_t mask = l > 0 ? 0x0fu : 0x0eu;
    if ((get_u32(SCENERY_MASK_SLOT) & 0xffu) != mask)
        api->guest_write_u32(api, SCENERY_MASK_SLOT, mask);
}
static void police_career_pass(const PopModApi *api);
/* THE BACKGROUND NaN (2026-11-24, found with RECOMP_D3D9_NAN_MARK_BITS + RECOMP_D3D9_PS_NAN_CALLER): FUN_00769d60
 * (stdcall size, out, deviation, multiplier, horizontal) fills 15 blur taps {x, y, 0, weight} with a normalised
 * Gaussian; FUN_00798200 asks it with deviation [0x00c21a80 + 4 x type], which the exe holds as 0.0 for type 1, so
 * the centre weight is 0/0 and after the normalisation all 15 are NaN - every frame, on Windows too (x87 invalid
 * masked). The 15-tap blur (ps code 61e40bbdebf3dade) then writes NaN into its 756x491 8-bit targets, which the
 * hardware stores as 0. Here such weights are made 0 at the source: the layer comes out 0 exactly as before,
 * without NaN in the pipeline (NFSU_GAUSS_NAN_KEEP=1 = old). NFSU_GAUSS_LOG=1 logs inputs and weights (first 12
 * calls) and each _CIexp (thunk 0x008ae6b0, x87 argument and result) called meanwhile. */
static int g_gauss_in, g_gauss_calls;
static uint32_t g_gauss_args[5];
static void gauss_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    for (int i = 0; i < 5; ++i)
        g_gauss_args[i] = get_u32(cpu->esp + 4 + 4 * i);
    g_gauss_in = 1;
}
static void gauss_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    g_gauss_in = 0;
    static int keep = -1;
    if (keep < 0)
        keep = getenv("NFSU_GAUSS_NAN_KEEP") != NULL;
    const uint32_t out = g_gauss_args[1];
    if (!keep && out)
        for (uint32_t i = 0; i < 15; ++i) {
            const uint32_t b = get_u32(out + 16 * i + 12);
            if ((b & 0x7f800000u) == 0x7f800000u) /* NaN or infinity */
                api->guest_write_u32(api, out + 16 * i + 12, 0);
        }
    if (!getenv("NFSU_GAUSS_LOG") || g_gauss_calls++ >= 12)
        return;
    char l[900];
    float sd, mu;
    memcpy(&sd, &g_gauss_args[2], 4), memcpy(&mu, &g_gauss_args[3], 4);
    int n = snprintf(l, sizeof l, "core.nfsu: gauss size %u dev %g mult %g horiz %u w:", g_gauss_args[0], sd, mu,
                     g_gauss_args[4] & 0xff);
    for (uint32_t i = 0; i < 15 && n < 860; ++i) {
        const uint32_t b = get_u32(g_gauss_args[1] + 16 * i + 12);
        float w;
        memcpy(&w, &b, 4);
        n += snprintf(l + n, sizeof l - n, " %g", w);
    }
    api->log(api, l);
}
static int g_exp_logged;
static void gauss_exp(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (!g_gauss_in || g_exp_logged >= 40)
        return;
    ++g_exp_logged;
    char l[300];
    snprintf(l, sizeof l, "core.nfsu: gauss exp %s top %u cw %04x st %g %g %g %g %g %g %g %g", user ? "out" : "in",
             cpu->fpu_top, cpu->fpu_cw, cpu->st[0], cpu->st[1], cpu->st[2], cpu->st[3], cpu->st[4], cpu->st[5], cpu->st[6],
             cpu->st[7]);
    api->log(api, l);
}
static void hud_pass(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    hud_apply(api);
    draw_distance_apply(api);
    attrib_dump(api);
    vlt_pass(api);
    extraoptions_pass(api);
    addon_pass(api);
    police_career_pass(api);
    tp_pad_pass(api);
    pad_layout_pass(api);
    heli_pass(api);
    police_sale_pass(api);
}
static void hud_matrix_leave(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t m = g_hud_matrix_this + 0x30;
    g_hud_matrix_this = 0;
    if (m == 0x30 || !(g_aspect > 1.34f && g_aspect < 16.0f / 9.0f))
        return;
    static int logged;
    if (!logged) {
        logged = 1;
        char line[400];
        int n = snprintf(line, sizeof line, "core.nfsu: HUD matrix");
        for (int i = 0; i < 16; ++i) {
            uint32_t raw = get_u32(m + 4u * (uint32_t)i);
            float f;
            memcpy(&f, &raw, 4);
            n += snprintf(line + n, sizeof line - n, " %g", f);
        }
        api->log(api, line);
    }
    /* What the game built is the driving matrix (the layout numbers follow the real aspect): its y scale as built,
     * and row 3's y (+1, the top edge) moved so the 720 units of content are centred: 720 / height. */
    uint32_t raw = get_u32(m + 5 * 4);
    memcpy(&g_hud_drive_ys, &raw, 4);
    raw = get_u32(m + 0 * 4);
    memcpy(&g_hud_drive_xs, &raw, 4);
    raw = get_u32(m + 12 * 4);
    memcpy(&g_hud_drive_tx, &raw, 4);
    raw = get_u32(m + 13 * 4);
    float cur;
    memcpy(&cur, &raw, 4);
    g_hud_drive_ty = cur > 0.99f && cur < 1.01f ? 720.0f / (1280.0f / g_aspect) : cur;
    g_hud_m = m;
    g_hud_applied = -1;
    hud_apply(api);
}
static void widescreen_install(const PopModApi *api) {
    static uint32_t hooks[5];
    if (!api->hook_install_at_callsite) {
        api->log(api, "core.nfsu: widescreen needs call-site hooks; not installed");
        return;
    }
    api->hook_install(api, GET_ASPECT, get_aspect, POP_HOOK_REPLACE, NULL, &hooks[0]);
    static uint32_t mhooks[2];
    api->hook_install(api, HUD_MATRIX_BUILD, hud_matrix_enter, POP_HOOK_BEFORE, NULL, &mhooks[0]);
    api->hook_install(api, HUD_MATRIX_BUILD, hud_matrix_leave, POP_HOOK_AFTER, NULL, &mhooks[1]);
    static uint32_t pass_hook;
    api->hook_install(api, MAIN_LOOP_STEP, hud_pass, POP_HOOK_BEFORE, NULL, &pass_hook);
    api->hook_install_at_callsite(api, SIN_ANGLE, 0x0074a92eu, proj_h, POP_HOOK_BEFORE, (void *)1, &hooks[1]);
    api->hook_install_at_callsite(api, COS_ANGLE, 0x0074a938u, proj_h, POP_HOOK_BEFORE, NULL, &hooks[2]);
    api->hook_install_at_callsite(api, SIN_ANGLE, 0x0074a992u, proj_v, POP_HOOK_BEFORE, (void *)1, &hooks[3]);
    api->hook_install_at_callsite(api, COS_ANGLE, 0x0074a99cu, proj_v, POP_HOOK_BEFORE, NULL, &hooks[4]);
    api->log(api, "core.nfsu: widescreen view installed");
}

static void render_wait_yield(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    if (!get_u32(RENDER_WAIT_FLAG))
        return; /* the step that really runs the render pass: nothing to wait for */
    yield_baton(api);
    if (++g_yields == 1)
        api->log(api, "core.nfsu: render-thread wait yields the baton (first time)");
}

// Before the ring run: when its ring holds nothing to run, the caller is waiting for the other thread, so yield.
static void ring_wait_yield(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ring = cpu->ecx;
    if (!ring)
        return;
    const int nothing = get_u32(ring + 0xc) == 0 || get_u32(ring + 0x18) >= get_u32(ring + 0x14) ||
                        get_u32(ring) == get_u32(ring + 4);
    if (!nothing)
        return;
    yield_baton(api);
    static unsigned long ring_yields;
    if (++ring_yields == 1)
        api->log(api, "core.nfsu: render command-ring wait yields the baton (first time)");
}

static void region_extra_size(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    char name[128] = {0};
    read_guest_str(get_u32(cpu->esp + 4), name, sizeof name);
    api->call_next(api, inv, cpu);
    if (cpu->eax != 0xffffffffu || !strstr(name, "\\MakeSceneryExtra.bin"))
        return;
    cpu->eax = 0;
    if (++g_extra_missing == 1) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsu: %s is not installed; loading it as an empty file", name);
        api->log(api, line);
    }
}

/* DIAGNOSTIC, opt-in with NFSU_DIAG=1. __security_check_cookie (008ae230: `CMP ECX,[0x00d61c88]; JNZ; REP RET`,
 * the only match of that byte pattern) with a cookie that does not match: log the function that is failing (its
 * return address) and the code addresses on the stack above it, before the CRT ends the process with 0xC0000409. */
#define GS_CHECK 0x008ae230u
#define GS_COOKIE 0x00d61c88u
static void gs_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (cpu->ecx == get_u32(GS_COOKIE))
        return;
    char line[700];
    /* The frame stored cookie^ESP at entry and passes stored^ESP now, so cookie^ECX is the entry ESP if only the
     * stack pointer moved (an unbalanced call), and garbage if the slot itself was overwritten. */
    int n = snprintf(line, sizeof line, "core.nfsu: stack cookie mismatch (ecx=%08x, cookie=%08x, cookie^ecx=%08x)"
                     " in the function returning to %08x, esp=%08x ebp=%08x; code words:", cpu->ecx,
                     get_u32(GS_COOKIE), cpu->ecx ^ get_u32(GS_COOKIE), get_u32(cpu->esp), cpu->esp, cpu->ebp);
    for (uint32_t i = 1; i < 120 && n < (int)sizeof line - 16; ++i) {
        const uint32_t w = get_u32(cpu->esp + 4 * i);
        if (w >= 0x00401000u && w < 0x00bd6000u)
            n += snprintf(line + n, sizeof line - n, " +%x:%08x", 4 * i, w);
    }
    api->log(api, line);
}

/* Same opt-in: the failing frame is FUN_00597900's; its cookie slot gets 0x00bee110, the string pushed at
 * 0057181d inside FUN_005717b0 (stdcall, 1 argument), which only lands there if ESP has drifted. Log ESP at the
 * entry of 00597900, 005717b0 and every call 005717b0 makes, for the first few passes, to see which call returns
 * with the stack unbalanced (expected entry ESPs relative to 005717b0's: 69da40 -4, 55c3b0 -8, 55c3e0 -8,
 * 563320 -0x14, 55c350 -0x18, 55c380 -0x1c). */
static const uint32_t g_trace_addrs[] = {0x00597900u, 0x005717b0u, 0x0069da40u, 0x0055c3b0u,
                                         0x0055c3e0u, 0x00563320u, 0x0055c350u, 0x0055c380u};
static uint32_t g_trace_hooks[sizeof g_trace_addrs / sizeof g_trace_addrs[0]];
static int g_trace_on;
static unsigned g_trace_lines;
static void esp_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    const uint32_t addr = (uint32_t)(uintptr_t)user;
    if (addr == 0x005717b0u)
        g_trace_on = 1;
    if ((!g_trace_on && addr != 0x00597900u) || g_trace_lines > 120)
        return;
    ++g_trace_lines;
    char line[160];
    snprintf(line, sizeof line, "core.nfsu: esp trace %08x entered with esp=%08x (returns to %08x)", addr, cpu->esp,
             get_u32(cpu->esp));
    api->log(api, line);
    if (addr == 0x0055c380u)
        g_trace_on = 0;
}

/* Same opt-in: memset through the thunk at 008ae1a6 (JMP [0x00bd6238] = MSVCR80!memset) whose end leaves the guest
 * arena - the "SIGSEGV at the thunk, addr = guest 0x80000000" signature. Logs the caller, the arguments and the
 * code addresses on the stack. */
#define MEMSET_THUNK 0x008ae1a6u
static uint32_t g_memset_hook;
static void memset_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t dst = get_u32(cpu->esp + 4), val = get_u32(cpu->esp + 8), size = get_u32(cpu->esp + 12);
    if ((uint64_t)dst + size <= 0x7ffff000u && size < 0x20000000u)
        return;
    char line[900];
    int n = snprintf(line, sizeof line, "core.nfsu: wild memset(dst=%08x, val=%02x, size=%08x) from %08x, esp=%08x "
                     "ebx=%08x esi=%08x edi=%08x ebp=%08x; code words:", dst, val & 0xff, size, get_u32(cpu->esp),
                     cpu->esp, cpu->ebx, cpu->esi, cpu->edi, cpu->ebp);
    for (uint32_t i = 1; i < 300 && n < (int)sizeof line - 16; ++i) {
        const uint32_t w = get_u32(cpu->esp + 4 * i);
        if (w >= 0x00401000u && w < 0x00bd6000u)
            n += snprintf(line + n, sizeof line - n, " +%x:%08x", 4 * i, w);
    }
    api->log(api, line);
}

/* Same opt-in: FUN_0049ff20(key, folder index) builds "sound\<folder>\<name>" with the name from the attribute
 * database (FUN_004649b0, ECX = [[0x00d83cd0]+0x9c], class 0x4166fa80, key); a failed lookup leaves the name empty,
 * the folder itself is "opened", its size is -1 and the pool memsets -1 bytes. Log each call and the lookup. */
#define SOUND_BANK_LOAD 0x0049ff20u
static uint32_t g_bank_hook;
static void bank_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t key = get_u32(cpu->esp + 4), folder = get_u32(cpu->esp + 8);
    /* [obj+0x9c] is a handle (from FUN_0049edf0(class 0x6367e03d, key 0)); its first word is the collection, which
     * FUN_004649b0 (`MOV ECX,[ECX]`) dereferences, as the game's call at 0049ff40 does with ECX = the handle. */
    const uint32_t db = get_u32(get_u32(0x00d83cd0u) + 0x9c);
    uint32_t rec = 0;
    const uint32_t a[2] = {0x4166fa80u, key};
    if (db)
        api->guest_call(api, 0x004649b0u, db, a, 2, &rec);
    {
        char l2[160];
        snprintf(l2, sizeof l2, "core.nfsu: sound bank handle %08x -> collection %08x (+4 %08x +8 %08x)", db,
                 db ? get_u32(db) : 0, db ? get_u32(db + 4) : 0, db ? get_u32(db + 8) : 0);
        api->log(api, l2);
        /* The collection lookup FUN_00464a20(class 0xd3c18f03, key 0x6367e03d): the class index from FUN_00463be0
         * against the class table's count ([[0x00d834d0]+4]+0xc), the class's table slot, and the result. */
        uint32_t idx = 0, coll = 0;
        const uint32_t ca[1] = {0xd3c18f03u};
        api->guest_call(api, 0x00463be0u, 0, ca, 1, &idx);
        const uint32_t tbl = get_u32(get_u32(0x00d834d0u) + 4);
        const uint32_t count = tbl ? (get_u32(tbl + 0xc) & 0xffffu) : 0;
        const uint32_t slot = tbl ? get_u32(tbl + 8) + idx * 0xc : 0;
        const uint32_t la[3] = {0xd3c18f03u, 0x6367e03du, 0};
        api->guest_call(api, 0x00464a20u, 0, la, 3, &coll);
        snprintf(l2, sizeof l2, "core.nfsu: class d3c18f03 -> index %u of %u, slot %08x (+4 %08x); collection "
                 "6367e03d -> %08x", idx, count, slot, slot ? get_u32(slot + 4) : 0, coll);
        api->log(api, l2);
    }
    char name[64] = {0}, dir[40] = {0};
    if (rec)
        read_guest_str(get_u32(rec + 4), name, sizeof name);
    read_guest_str(get_u32(0x00d25cb0u + 4 * folder), dir, sizeof dir);
    char line[240];
    snprintf(line, sizeof line, "core.nfsu: sound bank key %08x folder %u (%s): db %08x -> record %08x name \"%s\"",
             key, folder, dir, db, rec, name);
    api->log(api, line);
}

/* Same opt-in: FUN_006ac6e0(pack, name, ...) loads the attribute vault "<name>.bin"/"<name>.vlt" from a pack in
 * memory ("Attributes - %s.bin"). Log each vault name, to see whether the one holding the sound collection (class
 * 0x6367e03d) is loaded before FUN_004aff10 starts the sound banks. */
#define VAULT_LOAD 0x006ac6e0u
static uint32_t g_vault_hook;
static void vault_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    char name[64] = {0};
    read_guest_str(get_u32(cpu->esp + 8), name, sizeof name);
    char line[160];
    snprintf(line, sizeof line, "core.nfsu: attribute vault \"%s\" loaded (from %08x)", name, get_u32(cpu->esp));
    api->log(api, line);
}

/* Same opt-in: the attribute database ([0x00d834d0]) is made only by Attrib::DatabaseExportPolicy's slot 2
 * (004683e0, vtable 0x00bdf060) when a vault exports it; the vault loader 006ac470 gets the policy from 00468df0.
 * In the first 1.0.0.1 runs [0x00d834d0] was never written, so no attribute class existed and the sound collection
 * lookup failed. Log entries to these three to see how far the export dispatch gets. */
static const uint32_t g_attr_addrs[] = {0x006ac470u, 0x00468df0u, 0x004683e0u, 0x00466c70u};
static uint32_t g_attr_hooks[sizeof g_attr_addrs / sizeof g_attr_addrs[0]];
static unsigned g_attr_lines;
static void attr_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (g_attr_lines++ > 60)
        return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: attrib %08x entered (ret %08x, ecx %08x, args %08x %08x %08x %08x)",
             (uint32_t)(uintptr_t)user, get_u32(cpu->esp), cpu->ecx, get_u32(cpu->esp + 4), get_u32(cpu->esp + 8),
             get_u32(cpu->esp + 12), get_u32(cpu->esp + 16));
    api->log(api, line);
}

/* Same opt-in: the vault fix-up FUN_00466ec0 (ECX = vault) calls, for each of its [vault+0x48] exports (16-byte
 * records from [vault+0x34]+0x10, type hash at +0), the policy found by binary search in the sorted (type, policy)
 * table {array, ?, count} at [vault+0x10] - slot 2 at 00467017. Dump both, to see why the database's policy
 * (Attrib::DatabaseExportPolicy, vtable 0x00bdf060) is never called. */
static void fixup_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t v = cpu->ecx, n = get_u32(v + 0x48), tab = get_u32(v + 0x10);
    const uint32_t arr = tab ? get_u32(tab) : 0, cnt = tab ? get_u32(tab + 8) : 0;
    char line[900];
    int k = snprintf(line, sizeof line, "core.nfsu: vault %08x fix-up (type hashes [d83508] %08x [d83504] %08x "
                     "[d8350c] %08x [d83510] %08x): %u exports, policy table %08x (array %08x, %u entries):", v,
                     get_u32(0x00d83508u), get_u32(0x00d83504u), get_u32(0x00d8350cu), get_u32(0x00d83510u), n, tab, arr, cnt);
    for (uint32_t i = 0; i < cnt && i < 12 && k < (int)sizeof line - 40; ++i)
        k += snprintf(line + k, sizeof line - k, " %08x->%08x(vt %08x)", get_u32(arr + 8 * i),
                      get_u32(arr + 8 * i + 4), get_u32(get_u32(arr + 8 * i + 4)));
    k += snprintf(line + k, sizeof line - k, "; export types:");
    const uint32_t ex = get_u32(v + 0x34) + 0x10;
    for (uint32_t i = 0; i < n && i < 12 && k < (int)sizeof line - 16; ++i)
        k += snprintf(line + k, sizeof line - k, " %08x", get_u32(ex + 0x10 * i));
    api->log(api, line);
}

/* Same opt-in: Attrib collection lookups (FUN_00464a20(class hash, collection key, ...) -> collection or 0) that
 * find nothing, each distinct (class, key) pair logged once with its caller - to see which settings the game asks
 * for and does not get (the free-roam camera, 2026-11-15). */
static void attrib_lookup_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t cls = get_u32(cpu->esp + 4), key = get_u32(cpu->esp + 8), ret = get_u32(cpu->esp);
    api->call_next(api, inv, cpu);
    if (cpu->eax)
        return;
    static uint32_t seen[512][2];
    static unsigned n_seen;
    for (unsigned i = 0; i < n_seen; ++i)
        if (seen[i][0] == cls && seen[i][1] == key)
            return;
    if (n_seen < 512) {
        seen[n_seen][0] = cls;
        seen[n_seen][1] = key;
        ++n_seen;
    }
    char line[160];
    snprintf(line, sizeof line, "core.nfsu: attrib lookup class %08x key %08x found nothing (from %08x)", cls, key,
             ret);
    api->log(api, line);
}

/* Same opt-in: which camera mover runs each frame (slot 3 of each CameraMover subclass vtable: DrivingCameraMover
 * 004cf050, TrackCarCameraMover 004cdf90, TrackCopCameraMover 004ce120, the base 00758960 for the cinematic movers),
 * a line every 600 calls of each with the mover object - the free-roam camera looked straight down at the car. */
static const uint32_t g_mover_addrs[] = {0x004cf050u, 0x004cdf90u, 0x004ce120u, 0x00758960u};
static uint32_t g_mover_hooks[4];
static unsigned long g_mover_calls[4];
static void mover_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    const unsigned k = (unsigned)(uintptr_t)user;
    if (g_mover_calls[k]++ % 600)
        return;
    char line[240];
    const uint32_t a1 = get_u32(cpu->esp + 4), a2 = get_u32(cpu->esp + 8);
    float f1, f2;
    memcpy(&f1, &a1, 4);
    memcpy(&f2, &a2, 4);
    snprintf(line, sizeof line, "core.nfsu: camera mover %08x update #%lu (this %08x, vtable %08x) from %08x args %08x "
             "(%g) %08x (%g)", g_mover_addrs[k], g_mover_calls[k], cpu->ecx, get_u32(cpu->ecx), get_u32(cpu->esp),
             a1, (double)f1, a2, (double)f2);
    api->log(api, line);
}

/* Same opt-in: the driving camera's named parameters (FUN_004cb3e0(this, name) -> float10 in ST0: "Pitch" and the
 * rest the update 004e0e30 reads). Each name's first three answers and then every 600th, with the caller. */
static void cam_param_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    char name[48] = {0};
    const uint32_t np = get_u32(cpu->esp + 4), ret = get_u32(cpu->esp);
    read_guest_str(np, name, sizeof name);
    api->call_next(api, inv, cpu);
    static struct {
        char name[48];
        unsigned long n;
    } seen[64];
    static unsigned n_seen;
    unsigned i = 0;
    for (; i < n_seen; ++i)
        if (!strcmp(seen[i].name, name))
            break;
    if (i == n_seen && n_seen < 64)
        strcpy(seen[n_seen++].name, name);
    if (i < 64) {
        const unsigned long k = seen[i].n++;
        if (k >= 3 && k % 600)
            return;
    }
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: camera param \"%s\" -> st0 %g (phys %g, top %u) from %08x this %08x",
             name, cpu->st[0], cpu->st[cpu->fpu_top & 7], cpu->fpu_top, ret, cpu->ecx);
    api->log(api, line);
}

/* DIAGNOSTIC, opt-in with NFSU_CAM_SNAP=<n> (and NFSU_CAM_SNAP_DIR, default /tmp): on the n-th call of the driving
 * camera's update (DrivingCameraMover slot 1, 004e0e30) write every non-zero page of the guest arena and the
 * registers before the call (cam-before.bin) and after it (cam-after.bin), so the same call can be replayed in
 * Unicorn from the first file and compared with the second (/tmp tool cam_diff.py). File: "NFSS", then u32 eax ecx
 * edx ebx esp ebp esi edi, u32 fpu_cw, then (u32 page address, 4096 bytes) records to the end. */
static void snapshot_arena(const PopModApi *api, const pop_cpu_v1 *cpu, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("NFSS", 1, 4, f);
    const uint32_t regs[9] = {cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi,
                              cpu->fpu_cw};
    fwrite(regs, 4, 9, f);
    static const uint8_t zero[4096];
    unsigned long pages = 0;
    for (uint32_t a = 0x1000; a < 0x7ffff000u; a += 0x1000) {
        void *p = NULL;
        if (api->guest_ptr(api, a, 0x1000, &p) != POP_OK || !p || !memcmp(p, zero, 4096))
            continue;
        fwrite(&a, 4, 1, f);
        fwrite(p, 1, 4096, f);
        ++pages;
    }
    fclose(f);
    char line[300];
    snprintf(line, sizeof line, "core.nfsu: snapshot %s: %lu pages", path, pages);
    api->log(api, line);
}
/* While the snapshot call runs (g_in_snap), probes on the functions NFSU_PROBE lists (hex, comma separated) log
 * their entry: ECX, four stack arguments and the 16 dwords at ECX+0x100 - the same lines cam_diff.py prints when
 * Unicorn enters them, so the first function whose inputs differ is the one to read. */
static int g_in_snap;
static void probe(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (!g_in_snap)
        return;
    char line[400];
    int n = snprintf(line, sizeof line, "P %08x ecx=%08x args %08x %08x %08x %08x ecx+100:", (uint32_t)(uintptr_t)user,
                     cpu->ecx, get_u32(cpu->esp + 4), get_u32(cpu->esp + 8), get_u32(cpu->esp + 12),
                     get_u32(cpu->esp + 16));
    for (int i = 0; i < 16 && cpu->ecx; ++i)
        n += snprintf(line + n, sizeof line - n, " %08x", get_u32(cpu->ecx + 0x100 + 4 * i));
    api->log(api, line);
}
/* NFSU_COUNT=<hex>,... (DIAGNOSTIC): count the calls of these guest functions and log the counts every 5 s, so a
 * loop's rate can be set beside the presented frame rate (the frontend ran too fast once the port got quicker). */
static uint32_t g_count_addr[16];
static unsigned long g_count_n[16];
static unsigned g_count_k;
static void counter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    const unsigned i = (unsigned)(uintptr_t)user;
    ++g_count_n[i];
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!last.tv_sec)
        last = now;
    if (now.tv_sec - last.tv_sec >= 5) {
        char line[400];
        int n = snprintf(line, sizeof line, "core.nfsu: calls per second:");
        const double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
        for (unsigned k = 0; k < g_count_k; ++k) {
            n += snprintf(line + n, sizeof line - n, " %08x %.1f", g_count_addr[k], g_count_n[k] / dt);
            g_count_n[k] = 0;
        }
        api->log(api, line);
        last = now;
    }
}
static void install_counters(const PopModApi *api) {
    const char *e = getenv("NFSU_COUNT");
    static uint32_t hooks[16];
    while (e && *e && g_count_k < 16) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(e, &end, 16);
        if (end == e)
            break;
        g_count_addr[g_count_k] = a;
        api->hook_install(api, a, counter, POP_HOOK_BEFORE, (void *)(uintptr_t)g_count_k, &hooks[g_count_k]);
        ++g_count_k;
        e = *end == ',' ? end + 1 : end;
    }
}
static void install_probes(const PopModApi *api) {
    const char *e = getenv("NFSU_PROBE");
    static uint32_t hooks[32];
    unsigned k = 0;
    while (e && *e && k < 32) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(e, &end, 16);
        if (end == e)
            break;
        api->hook_install(api, a, probe, POP_HOOK_BEFORE, (void *)(uintptr_t)a, &hooks[k++]);
        e = *end == ',' ? end + 1 : end;
    }
}
static void cam_snap(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    static unsigned long calls;
    const char *e = getenv("NFSU_CAM_SNAP");
    const unsigned long want = e ? strtoul(e, NULL, 10) : 0;
    if (++calls % 1000 == 0) {
        char line[80];
        snprintf(line, sizeof line, "core.nfsu: camera update call %lu", calls);
        api->log(api, line);
    }
    if (calls != want) {
        const uint32_t self = cpu->ecx;
        api->call_next(api, inv, cpu);
        /* NFSU_CAM_LOG: the rope direction (this+0xe0), its two angles (+0x108 polar, +0x114 azimuth), the car's
         * position (car+0x20) and last position (+0xd0), every call for the first 600, then every 30th. */
        if (getenv("NFSU_CAM_LOG") && (calls <= 600 || calls % 30 == 0)) {
            const uint32_t car = get_u32(self + 0xc0);
            uint32_t v[11] = {get_u32(self + 0xe0), get_u32(self + 0xe4), get_u32(self + 0xe8), get_u32(self + 0x108),
                              get_u32(self + 0x114), car ? get_u32(car + 0x20) : 0, car ? get_u32(car + 0x24) : 0,
                              car ? get_u32(car + 0x28) : 0, get_u32(self + 0xd0), get_u32(self + 0xd4),
                              get_u32(self + 0xd8)};
            float f[11];
            memcpy(f, v, sizeof f);
            uint32_t vv[3] = {car ? get_u32(car + 0x80) : 0, car ? get_u32(car + 0x84) : 0,
                              car ? get_u32(car + 0x88) : 0};
            float vel[3];
            memcpy(vel, vv, sizeof vel);
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            char line[300];
            snprintf(line, sizeof line,
                     "C %lu t=%.3f this=%08x dir %.4f %.4f %.4f pol %.4f az %.4f car %.3f %.3f %.3f vel %.3f %.3f %.3f",
                     calls, ts.tv_sec + ts.tv_nsec / 1e9, self, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], vel[0],
                     vel[1], vel[2]);
            api->log(api, line);
        }
        return;
    }
    const char *dir = getenv("NFSU_CAM_SNAP_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/cam-before.bin", dir ? dir : "/tmp");
    snapshot_arena(api, cpu, path);
    g_in_snap = 1;
    api->call_next(api, inv, cpu);
    g_in_snap = 0;
    snprintf(path, sizeof path, "%s/cam-after.bin", dir ? dir : "/tmp");
    snapshot_arena(api, cpu, path);
}

/* Same opt-in: msvcr80's _CIcos (10053f90) and _CIsin (100546f0) take and return ST0. Compare the translated answer
 * with the host's cos/sin and log the first 40 answers that differ by more than 1e-6 (the camera's angles differed
 * from a Unicorn replay by ~0.5%, 2026-11-15). */
static void trig_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    static const char *const names[] = {"cos", "sin", "acos", "sqrt", "atan2", "asin", "pow", "fmod",
                                        "exp", "log", "log10", "tan", "floor"};
    const unsigned kind = (unsigned)(uintptr_t)user;
    const double x = cpu->st[cpu->fpu_top & 7], y = cpu->st[(cpu->fpu_top + 1) & 7];
    static int sin_snapped;
    const int snap_this = g_in_snap && kind == 1 && !sin_snapped && getenv("NFSU_SIN_SNAP");
    if (snap_this) {
        sin_snapped = 1;
        char l2[300];
        snprintf(l2, sizeof l2, "core.nfsu: sin snap x=%.17g cw %04x sw %04x tag %04x top %u esp %08x [10095944]=%08x",
                 x, cpu->fpu_cw, cpu->fpu_sw, cpu->fpu_tag, cpu->fpu_top, cpu->esp, get_u32(0x10095944u));
        api->log(api, l2);
        snapshot_arena(api, cpu, "/tmp/nfsu-sin-before.bin");
    }
    api->call_next(api, inv, cpu);
    if (snap_this)
        snapshot_arena(api, cpu, "/tmp/nfsu-sin-after.bin");
    const double got = cpu->st[cpu->fpu_top & 7];
    double fx = x;
    if (kind == 12) { /* floor(double) is cdecl: its argument is on the stack */
        uint32_t lo = get_u32(cpu->esp + 4), hi = get_u32(cpu->esp + 8);
        uint64_t b = (uint64_t)hi << 32 | lo;
        memcpy(&fx, &b, 8);
    }
    double want = 0;
    switch (kind) {
    case 0: want = cos(x); break;
    case 1: want = sin(x); break;
    case 2: want = acos(x); break;
    case 3: want = sqrt(x); break;
    case 4: want = atan2(y, x); break;
    case 5: want = asin(x); break;
    case 6: want = pow(y, x); break;
    case 7: want = fmod(y, x); break;
    case 8: want = exp(x); break;
    case 9: want = log(x); break;
    case 10: want = log10(x); break;
    case 11: want = tan(x); break;
    default: want = floor(fx); break;
    }
    static unsigned n_bad, n_all[13];
    if (g_in_snap) {
        char l1[200];
        snprintf(l1, sizeof l1, "core.nfsu: in snap _CI%s(%.9g, %.9g) = %.9g, host %.9g, ret %08x top %u", names[kind],
                 x, y, got, want, get_u32(cpu->esp - 4), cpu->fpu_top);
        api->log(api, l1);
    }
    if (++n_all[kind] == 1 || n_all[kind] % 5000 == 0) {
        char l0[160];
        snprintf(l0, sizeof l0, "core.nfsu: trig call %u: _CI%s(%.9g, %.9g) = %.9g, host %.9g", n_all[kind],
                 names[kind], x, y, got, want);
        api->log(api, l0);
    }
    if (fabs(got - want) <= 1e-6 * (fabs(want) > 1 ? fabs(want) : 1) || (isnan(got) && isnan(want)) || n_bad >= 200)
        return;
    ++n_bad;
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: _CI%s(%.9g, %.9g) = %.9g, host %.9g (call %u)", names[kind],
             kind == 12 ? fx : x, y, got, want, n_all[kind]);
    api->log(api, line);
}

/* Colour filter (setting colour_filter, 2026-11-17). The game grades the whole picture through 32x32x32 colour
 * cubes (VisualTreatment): FUN_00766f40 (fastcall, ecx = the owner) turns the cube data at [owner+4+4k] (k 0..8)
 * and [owner+0x5c] into volume textures, and the frame's blended cube is mixed from that same data (neutralising
 * only the textures changed nothing on screen, measured). Each cube is 3 bytes a texel at data + 3 * ((z * 32 + y) *
 * 32 + x): byte 2 red (x), byte 1 green (y), byte 0 blue (z); NFSU_LUT_LOG showed them 0.06-0.36 off neutral on
 * average. Before the game builds anything from them, this moves every texel toward its untinted colour
 * (x, y, z) * 255 / 31 by g_tint_mix: 0.5 = half the tint, 1 = none. Light, shadows and bloom are separate. */
#define LUT_BUILD 0x00766f40u
static float g_tint_mix;
static void neutral_cube(const PopModApi *api, uint32_t data) {
    static uint32_t done[32];
    static unsigned n_done;
    for (unsigned i = 0; i < n_done; ++i)
        if (done[i] == data)
            return; /* "softer" must not halve the tint again when the cubes are rebuilt */
    if (n_done < 32)
        done[n_done++] = data;
    double off = 0;
    for (uint32_t t = 0; t < 32768; ++t) {
        const uint32_t plain[3] = {(t >> 10) & 31, (t >> 5) & 31, t & 31}; /* bytes 0, 1, 2: blue, green, red */
        for (int b = 0; b < 3; ++b) {
            uint8_t v = 0;
            api->guest_read_u8(api, data + 3 * t + b, &v);
            const float want = plain[b] * 255.0f / 31.0f;
            off += fabsf(v - want);
            api->guest_write_u8(api, data + 3 * t + b, (uint8_t)(v + (want - v) * g_tint_mix + 0.5f));
        }
    }
    if (getenv("NFSU_LUT_LOG")) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsu: colour cube %08x was %.1f/255 off neutral on average", data,
                 off / (3 * 32768.0));
        api->log(api, line);
    }
}
static void lut_build(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    for (int k = 0; k < 9; ++k)
        if (get_u32(cpu->ecx + 4 + 4 * k))
            neutral_cube(api, get_u32(cpu->ecx + 4 + 4 * k));
    if (get_u32(cpu->ecx + 0x5c))
        neutral_cube(api, get_u32(cpu->ecx + 0x5c));
}

/* Motion blur (setting motion_blur, 2026-11-17). At speed the game smears the screen edges (MotionBlur): FUN_00798360
 * draws MotionBlur::DrawMotionBlurQuad (FUN_0077df40, stdcall: owner, target, strength) twice, each pass an 8-tap
 * blur along a per-corner vector (zero in the middle, longest at the edges) scaled by the strength, 0.5 for the
 * first pass ([0x00bdeb38]) and 1.4 for the second ([0x00c21af8]). Steps of 0.5 and 1.4 leave gaps, so the two
 * passes give about 15 separate, evenly spaced copies of the picture instead of a smear - the doubled, stepped
 * look at the edges (seen at 119 km/h in a frame dump). "Smooth" keeps the blur's length (7 * (0.5 + 1.4) steps)
 * and makes the first pass's step exactly 1/8 of the second's, so its 8 taps fill each gap of the second evenly:
 * 64 evenly spaced samples, one continuous smear. g_blur_scale shortens it for "lighter". */
#define MOTION_BLUR_QUAD 0x0077df40u
static float get_f32(uint32_t a) {
    const uint32_t u = get_u32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static float g_blur_scale = 1.0f;
/* motion_blur applies live (2026-11-17): read at most twice a second. 0 smooth, 1 as in the game (stepped), 2
 * lighter, 3 off. */
static int g_blur_env = -1, g_blur_mode = 0;
static float g_blur_weight = 1.0f;
static void blur_settings(const PopModApi *api, int now_too) {
    static int64_t read_at;
    const int64_t t = now_ns();
    if (!now_too && read_at && t - read_at < 500000000LL)
        return;
    read_at = t;
    int64_t blur = 0;
    if (g_blur_env >= 0)
        blur = g_blur_env;
    else if (api->settings_get)
        api->settings_get(api, "motion_blur", &blur);
    const int mode = blur >= 0 && blur <= 3 ? (int)blur : 0;
    if (mode != g_blur_mode || now_too) {
        g_blur_mode = mode;
        g_blur_scale = mode == 2 ? 0.6f : mode == 3 ? 0.0f : 1.0f; /* 0: zero-length blur vectors, no blur */
        g_blur_weight = mode == 2 ? 0.6f : mode == 3 ? 0.0f : 1.0f;
        api->log(api, mode == 1   ? "core.nfsu: motion blur: as in the game"
                      : mode == 2 ? "core.nfsu: motion blur: smooth, lighter"
                      : mode == 3 ? "core.nfsu: motion blur: off"
                                  : "core.nfsu: motion blur: smooth");
    }
}
static void motion_blur_quad(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    blur_settings(api, 0);
    if (g_blur_mode == 1)
        return; /* as in the game: its own steps */
    const float total = (0.5f + 1.4f) * g_blur_scale; /* sum of both passes' steps */
    float strength = get_f32(cpu->esp + 0xc), want;
    if (fabsf(strength - 0.5f) < 1e-4f)
        want = total / 9.0f; /* first pass: 1/8 of the second */
    else if (fabsf(strength - 1.4f) < 1e-4f)
        want = total * 8.0f / 9.0f;
    else
        return; /* not one of the two known passes: leave it as the game has it */
    uint32_t u;
    memcpy(&u, &want, 4);
    api->guest_write_u32(api, cpu->esp + 0xc, u);
}

/* GLOW AND DEPTH OF FIELD (settings glow, depth_of_field; 2026-11-17). The person: "too soapy even standing still
 * with track effects at maximum". The final picture is VisualTreatment's draw FUN_00882340 (thiscall, ESI/ECX =
 * the effect, RET 0x20): its shader (ShadersLibrary.bin CTAB: c1 = cvVisualTreatmentParams, ...) mixes the quarter-
 * size blurred glow (BLOOM_SAMPLER) over the picture with a screen blend, scaled by cvVisualTreatmentParams.x =
 * max([effect+0x114], 0), or [0x00d5e154] when that is >= 0. The glow hook scales both for the one draw and puts
 * them back (0 = the plain picture). Depth of field: the "vt_enhanced" variant (with the depth buffer) is chosen when
 * FUN_0077bf50(view) (stdcall, RET 4) sees a view's blur amounts above 0.001; FUN_0077c000(view, out) (RET 0xc)
 * writes cvDepthOfFieldParams = {near blur * 2, far blur, focus, ...}. Weaker halves the two blur amounts; Off
 * answers 0 from FUN_0077bf50, so the plain variant draws. */
#define VT_DRAW 0x00882340u
#define VT_DOF_ACTIVE 0x0077bf50u
#define VT_DOF_PARAMS 0x0077c000u
#define VT_GLOW_OVERRIDE 0x00d5e154u
static float g_glow_scale = 1.0f, g_dof_scale = 1.0f;
static int g_dof_mode = 1;
static int g_glow_env = -1, g_dof_env = -1; /* NFSU_GLOW / NFSU_DOF: fixed values for A/B runs */
/* The two settings apply live: read at most twice a second from whichever hook runs. */
static void vt_settings(const PopModApi *api) {
    static int64_t last;
    const int64_t now = now_ns();
    if (last && now - last < 500000000LL)
        return;
    last = now;
    int64_t glow = 0, dof = 1;
    if (api->settings_get) {
        api->settings_get(api, "depth_of_field", &dof);
    }
    if (g_glow_env >= 0)
        glow = g_glow_env;
    if (g_dof_env >= 0)
        dof = g_dof_env;
    static const float glow_of[4] = {1.0f, 0.6f, 0.3f, 0.0f};
    g_glow_scale = glow >= 0 && glow < 4 ? glow_of[glow] : 1.0f;
    g_dof_mode = dof >= 0 && dof <= 2 ? (int)dof : 1;
    g_dof_scale = g_dof_mode == 1 ? 0.5f : 1.0f;
}
static float get_f32_at(uint32_t a) {
    const uint32_t u = get_u32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void put_f32_at(uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    g_api->guest_write_u32(g_api, a, u);
}
static void vt_draw(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    vt_settings(api);
    const uint32_t fx = cpu->ecx;
    if (getenv("NFSU_VT_LOG")) { /* once a second: what the draw is about to use */
        static int64_t last;
        const int64_t now = now_ns();
        if (now - last > 1000000000LL) {
            last = now;
            char line[200];
            snprintf(line, sizeof line, "core.nfsu: vt draw fx %08x glow %.3f override %.3f params %.3f %.3f %.3f",
                     fx, get_f32_at(fx + 0x114), get_f32_at(VT_GLOW_OVERRIDE), get_f32_at(fx + 0x14),
                     get_f32_at(fx + 0x18), get_f32_at(fx + 0x58));
            api->log(api, line);
        }
    }
    if (g_glow_scale == 1.0f) {
        api->call_next(api, inv, cpu);
        return;
    }
    const float own = get_f32_at(fx + 0x114), over = get_f32_at(VT_GLOW_OVERRIDE);
    put_f32_at(fx + 0x114, own * g_glow_scale);
    if (over >= 0.0f)
        put_f32_at(VT_GLOW_OVERRIDE, over * g_glow_scale);
    api->call_next(api, inv, cpu);
    put_f32_at(fx + 0x114, own);
    if (over >= 0.0f)
        put_f32_at(VT_GLOW_OVERRIDE, over);
}
static void dof_active(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    vt_settings(api);
    if (g_dof_mode != 2) {
        api->call_next(api, inv, cpu);
        return;
    }
    api->hook_return(api, cpu, 0, 4); /* no depth of field: the plain VisualTreatment variant */
}
static void dof_params(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t out = get_u32(cpu->esp + 8);
    api->call_next(api, inv, cpu);
    static int told;
    if (!told && out) {
        told = 1;
        char line[160];
        snprintf(line, sizeof line, "core.nfsu: depth of field in use (first time): near %.3f far %.3f focus %.3f",
                 get_f32_at(out), get_f32_at(out + 4), get_f32_at(out + 8));
        api->log(api, line);
    }
    if (out && g_dof_scale != 1.0f) {
        put_f32_at(out, get_f32_at(out) * g_dof_scale);
        put_f32_at(out + 4, get_f32_at(out + 4) * g_dof_scale);
    }
}

/* The blur's weight over the whole picture. FUN_00798360 (stdcall, 14 arguments, RET 0x38; first = the motion
 * blur effect) ends with "composite_blur" (FUN_007936f0): out = lerp(picture, blurred image, cvBlurParams.x) for
 * EVERY pixel, cvBlurParams.x = [effect+4]. The blurred image is half the screen's size, so wherever that weight is
 * above 0 the sharp centre is partly a half-resolution copy too - soft even where the blur vectors are zero. Lighter
 * scales the weight by g_blur_weight for the one draw, Off makes it 0 (the picture as rendered). */
#define MOTION_BLUR_DRAW 0x00798360u
static void motion_blur_draw(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    blur_settings(api, 0);
    const uint32_t fx = get_u32(cpu->esp + 4);
    const float w = get_f32(fx + 4);
    if (getenv("NFSU_MB_LOG")) {
        static int64_t last;
        const int64_t now = now_ns();
        if (now - last > 1000000000LL) {
            last = now;
            char line[120];
            snprintf(line, sizeof line, "core.nfsu: motion blur weight %.3f (x%.2f)", w, g_blur_weight);
            api->log(api, line);
        }
    }
    if (g_blur_weight == 1.0f || !fx) {
        api->call_next(api, inv, cpu);
        return;
    }
    uint32_t u;
    float scaled = w * g_blur_weight;
    memcpy(&u, &scaled, 4);
    api->guest_write_u32(api, fx + 4, u);
    api->call_next(api, inv, cpu);
    memcpy(&u, &w, 4);
    api->guest_write_u32(api, fx + 4, u);
}

/* Police Career (2026-11-19): its adapted plugin (game.toml [modules.aux.c_policecareer], made by
 * tools/police_career.py) is loaded the way an ASI loader loads it - LoadLibraryA from the game's own import, before
 * the game sets itself up (its main, FUN_0079bb20, called by the CRT start once static constructors have run) - and
 * only when mod_police_career is on (NFSU_POLICE_CAREER=0/1 overrides). Its DllMain starts the thread that hooks
 * the game; its code rewrites become call-site redirects in the kit (kernel32 FlushInstructionCache). */
#define GAME_MAIN 0x0079bb20u
#define IAT_LOADLIBRARYA 0x00bd6070u
static uint32_t g_pc_module; /* the plugin's module handle once loaded */
static void police_career_load(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    static int done;
    if (done)
        return;
    done = 1;
    /* 2026-11-24: loaded always - mod_police_career is each save's own and switched while the game runs
     * (pc_switch_pass); NFSU_POLICE_CAREER=0 leaves the plugin out of the run altogether */
    int64_t on = 1;
    if (getenv("NFSU_POLICE_CAREER"))
        on = atoi(getenv("NFSU_POLICE_CAREER"));
    if (!on)
        return;
    if (getenv("NFSU_PC_TRACE")) {
        uint32_t w = 0, w2 = 0;
        api->guest_read_u32(api, 0x00639200u, &w);
        api->guest_read_u32(api, 0x005f77d0u, &w2);
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: pc trace: [00639200] %08x [005f77d0] %08x", w, w2);
        api->log(api, l);
    }
    static const char name[] = "PoliceCareer.asi";
    uint32_t s = 0, h = 0;
    if (!api->guest_alloc || api->guest_alloc(api, sizeof name + 3, &s) != POP_OK || !s)
        return;
    for (uint32_t i = 0; i < sizeof name; i += 4) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4 && i + b < sizeof name; ++b)
            w |= (uint32_t)(uint8_t)name[i + b] << (8 * b);
        api->guest_write_u32(api, s + i, w);
    }
    uint32_t tramp = 0;
    api->guest_read_u32(api, IAT_LOADLIBRARYA, &tramp);
    const PopModStatus st = api->guest_call ? api->guest_call(api, tramp, 0, &s, 1, &h) : POP_E_STATE;
    if (st == POP_OK)
        g_pc_module = h;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: Police Career plugin %s (module %08x)", st == POP_OK && h ? "loaded" : "failed to load",
             h);
    api->log(api, l);
}

/* The career/garage object [0x00d8e1a4] is laid out 0x80 bytes longer in this build than in the retail exe the
 * Police Career plugin was made for (its car-record list at +0x88 instead of +8; tools/police_career.py). The
 * adapted plugin reads the object from the cell 0x01375870 instead, which this keeps at the object + 0x80 each
 * main-loop pass (0 while there is none), so the plugin's +8 lands on our list. */
#define CAREER_OBJ 0x00d8e1a4u
#define PC_CAREER_CELL 0x01375870u
/* The plugin's role state (rebased): its profile block [0x101d69a0], role object at +0x20 with role byte +0x18
 * (1 = police), last police car +0x1c, last racer car +0x20, pending menu branch +0x24 (2 police, 1 the game's
 * own cars screen, set by its FE hook) - logged on every change, so a person's run shows what the role did. */
#define PC_PROFILE_CELL 0x101d69a0u
/* The career's active car the way the plugin reads it (its FUN_10172910): the game's user-data query
 * FUN_00820c70("UserData", "UDKey", "UDValue", 0x307d9226) -> car id, or -1. */
static uint32_t pc_active_car(const PopModApi *api) {
    static uint32_t names;
    if (!names && api->guest_alloc && api->guest_alloc(api, 32, &names) == POP_OK && names) {
        static const char t[32] = "UserData\0\0\0\0UDKey\0\0\0UDValue";
        for (uint32_t i = 0; i < 32; i += 4) {
            uint32_t w = 0;
            for (uint32_t b = 0; b < 4; ++b)
                w |= (uint32_t)(uint8_t)t[i + b] << (8 * b);
            api->guest_write_u32(api, names + i, w);
        }
    }
    if (!names || !get_u32(0x0128461cu) || !get_u32(get_u32(0x0128461cu) + 4))
        return 0xffffffffu;
    const uint32_t a[4] = {names, names + 12, names + 20, 0x307d9226u};
    uint32_t r = 0xffffffffu;
    if (api->guest_call(api, 0x00820c70u, 0, a, 4, &r) != POP_OK)
        return 0xffffffffu;
    return r;
}
/* a career car row is a police car: its pvehicle is one of the plugin's police models (its FUN_10135f20 list) */
static int pc_row_is_police(uint32_t id) {
    static const uint32_t police[] = {0x1bb4725eu, 0x088eee9du, 0xc1733734u, 0x92dba446u, 0xeff67758u,
                                      0x9c079fadu, 0xc77f6e3bu, 0xfb00d31au, 0x0f33b374u, 0x1458a450u};
    const uint32_t obj = get_u32(CAREER_OBJ);
    if (!obj)
        return -1;
    for (uint32_t i = 0; i < 0x14au; ++i) {
        const uint32_t r = obj + 0x88u + 28u * i;
        if (get_u32(r) != id)
            continue;
        const uint32_t pv = get_u32(r + 8);
        for (size_t k = 0; k < sizeof police / sizeof police[0]; ++k)
            if (pv == police[k])
                return 1;
        return 0;
    }
    return -1;
}
/* THE MAP HANG (2026-11-22, the person: Tab while driving -> the game froze, one core at 100%, force-quit). Tab opens
 * the map, which queues job 0x00888280 on the deferred-job queue FUN_006aec60: it runs the map loader's state machine
 * (object 0x013383c0, FUN_00881e80 -> FUN_0087fc70, a 150 ms budget) and, until that says done, puts itself back
 * (FUN_0069e0b0(0x00888280, arg, 1)). The loader waits for data the game's Job Manager threads read. On Windows those
 * run beside the main thread; here guest threads take turns, and the main thread re-ran the job every 5-15 ms (the
 * main loop's command ring kept it going: no main-loop pass ended) without ever blocking, so the job threads never got
 * a turn and the map never finished loading (reproduced: Tab 0.5 s after driving off, the queue trace showed the
 * re-queue every few ms, every other guest thread waiting for its turn). A BEFORE hook on FUN_00881e80 with the
 * loader object (the job's one call; the job itself is reached only through a pointer) gives the turn away first (the game's own Sleep(0)), as the two render waits above do. NFSU_MAP_YIELD_OFF=1 = old. */
static void map_job_yield(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (cpu->ecx == 0x013383c0u) /* the map loader (the job itself is not a listed function: hooked at its callee) */
        yield_baton(api);
}
/* NFSU_QUEUE_TRACE=1 (DIAGNOSTIC, 2026-11-22, the Tab/map hang): the deferred-job queue FUN_006aec60 (jobs in
 * [0x00da4c30], count [0x00da5194]; while it runs [0x00da518d] = 1 and FUN_0069e0b0(fn, arg, 1) puts a job on the
 * next pass's list [0x00da49d8], count [0x00da5198]) - its nesting depth and callers, and every re-queue of the
 * map's job 0x00888280 with the flag and the counts, rate-limited. */
static int g_queue_depth;
static void queue_trace_run(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    static unsigned said;
    const uint32_t ret = get_u32(cpu->esp);
    ++g_queue_depth;
    if (g_queue_depth > 1 && said++ < 40) {
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: queue run nested (depth %d) from %08x: flag %u, now %u, next %u", g_queue_depth,
                 ret, get_u8(0x00da518du), get_u32(0x00da5194u), get_u32(0x00da5198u));
        api->log(api, l);
    }
    api->call_next(api, inv, cpu);
    --g_queue_depth;
}
static void queue_trace_add(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static unsigned said;
    const uint32_t fn = get_u32(cpu->esp + 4), nxt = get_u32(cpu->esp + 12) & 0xffu;
    if (fn == 0x00888280u && said++ < 60) {
        char l[180];
        snprintf(l, sizeof l, "core.nfsu: queue add %08x (next pass %u) at depth %d: flag %u, now %u, next %u, from %08x",
                 fn, nxt, g_queue_depth, get_u8(0x00da518du), get_u32(0x00da5194u), get_u32(0x00da5198u),
                 get_u32(cpu->esp));
        api->log(api, l);
    }
}
static void police_role_log(const PopModApi *api) {
    const uint32_t pr = get_u32(PC_PROFILE_CELL);
    if (!pr)
        return;
    const uint32_t ro = pr + 0x20u;
    /* 2026-11-20, the person: choosing an ordinary car in the ordinary garage must end police mode. The plugin
     * ends it only from the pause menu's own cars screen; any other garage left its role on, so the racer car
     * the person chose went on as a police car. Here: police role on, the career's active car is not a police
     * car and not the police car the plugin keeps -> the role goes off and that car is the racer's car. */
    static int64_t at;
    const int64_t t = now_ns();
    if (get_u8(ro + 0x18) == 1 && (!at || t - at > 1000000000LL)) {
        at = t;
        const uint32_t car = pc_active_car(api), pcar = get_u32(ro + 0x1c);
        /* seen twice a second apart (the plugin restores its police car a moment after a profile loads) */
        static uint32_t seen = 0xffffffffu;
        const int ordinary = car != 0xffffffffu && pcar != 0xffffffffu && car != pcar && pc_row_is_police(car) == 0;
        const int act = ordinary && seen == car;
        seen = ordinary ? car : 0xffffffffu;
        if (act) {
            api->guest_write_u8(api, ro + 0x18, 0);
            api->guest_write_u32(api, ro + 0x20, car);
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: police mode ended: an ordinary car (%08x) was chosen", car);
            api->log(api, l);
        }
    }
    const uint32_t v[6] = {get_u8(pr + 9), get_u8(ro + 0x18), get_u32(ro + 0x1c), get_u32(ro + 0x20), get_u32(ro + 0x24),
                           eo_player_car(api)};
    static uint32_t last[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    if (!memcmp(v, last, sizeof v))
        return;
    memcpy(last, v, sizeof v);
    char l[200];
    snprintf(l, sizeof l, "core.nfsu: police role: enabled %u role %u police car %08x racer car %08x branch %u vehicle %08x",
             v[0], v[1], v[2], v[3], v[4], v[5]);
    api->log(api, l);
}

/* 2026-11-20, the person: the camera sits too close behind the police car (a long sedan on the racers' chase
 * camera). The chase views are the attribute class "camera", collections "close" and "far"; field 0x9f743a0e is a
 * two-point graph of the distance behind the car over speed ({?, speed, dist, dist, speed, dist, dist}: close 6.2 m
 * -> 6.8 m, far 6.2 -> 8 m). While police mode is on its distances are scaled (NFSU_POLICE_CAM=<percent>, default
 * 115), and given back when it ends. */
/* 2026-11-23 (pics/26): the tall police SUV and the long patrol sedan still had the camera low and close (the road
 * ahead hidden): their view is the same "close"/"far" as every car, made for low cars. For them the distance goes to
 * 145% and the height (field 0xba847865, the same {?, speed, h, h, speed, h, h} graph) to 170%; the Mustang and the
 * 911 keep 115% / 100%. Chosen by the active car's pvehicle (heli_active_pvehicle) and set again when it changes. */
static void police_camera(const PopModApi *api, int on) {
    static int applied; /* 0 off, 1 normal police cars, 2 the SUV / sedan */
    static uint32_t saved[2][2][4], addr_of[2][2];
    int want = 0;
    if (on) {
        const uint32_t pv = heli_active_pvehicle(api);
        want = pv == 0xc1733734u || pv == 0x1bb4725eu ? 2 : 1;
    }
    if (want == applied)
        return;
    static const char *const views[2] = {"close", "far"};
    static const uint32_t fields[2] = {0x9f743a0eu /* Position: distance */, 0xba847865u /* Height */};
    static const uint32_t at[4] = {8, 12, 20, 24};
    /* first put back what was changed, then apply the new scale to the game's own values */
    for (int v = 0; v < 2; ++v)
        for (int f = 0; f < 2; ++f)
            if (addr_of[v][f]) {
                for (int k = 0; k < 4; ++k)
                    api->guest_write_u32(api, addr_of[v][f] + at[k], saved[v][f][k]);
                addr_of[v][f] = 0;
            }
    const char *e = getenv("NFSU_POLICE_CAM");
    const float base = (e ? (float)atoi(e) : 115.0f) / 100.0f;
    const float scale[2] = {want == 2 ? 1.45f : base, want == 2 ? 1.7f : 1.0f};
    for (int v = 0; v < 2 && want; ++v)
        for (int f = 0; f < 2; ++f) {
            if (scale[f] == 1.0f)
                continue;
            const uint32_t coll = attrib_collection(attrib_key("camera"), attrib_key(views[v]));
            int where = -1;
            const uint32_t node = coll ? attrib_node(coll, fields[f], &where) : 0;
            if (!node || where != 0 || !(get_u8(node + 0xb) & 2u))
                continue;
            const uint32_t a = attrib_value_addr(coll, node);
            if (get_u16(a + 2) < 1 || get_u16(a + 4) != 0x1c)
                continue;
            const uint32_t el = a + ((get_u16(a + 6) & 0x8000u) ? 16u : 8u);
            addr_of[v][f] = el;
            for (int k = 0; k < 4; ++k) {
                saved[v][f][k] = get_u32(el + at[k]);
                float x;
                memcpy(&x, &saved[v][f][k], 4);
                if (x > 0.3f && x < 30.0f) {
                    x *= scale[f];
                    uint32_t w;
                    memcpy(&w, &x, 4);
                    api->guest_write_u32(api, el + at[k], w);
                }
            }
        }
    applied = want;
    char l[140];
    snprintf(l, sizeof l, "core.nfsu: police camera %s (distance %.0f%%, height %.0f%%)",
             want ? "moved back" : "as the game has it", want ? scale[0] * 100.0f : 100.0f,
             want ? scale[1] * 100.0f : 100.0f);
    api->log(api, l);
}

/* Police Career per save, switched live (2026-11-24, the person: these must be each save's own and work without a
 * restart). The plugin is always loaded; its features are its PoliceCareer.ini flags, copied at start into its
 * object [0x101d69a0] + 4 .. + 0x1f (FUN_1016dbe0: +9 PoliceRole - the police mode, its menus and garage -, +0xa
 * AmbientRacers, +0xb RacerRoutes ... +0x14 Interceptor) and read while it runs (FUN_10134970 and others test +9).
 * mod_police_career off for this save: those feature flags are 0 and a police role in progress ends (the career
 * gets a racer car back: addon_active_guard); on: the flags as the plugin read them. */
static void pc_switch_pass(const PopModApi *api) {
    const uint32_t pr = get_u32(PC_PROFILE_CELL);
    static uint32_t pr_seen;
    static uint8_t orig[0x17];
    static int applied = -1;
    if (!pr || !g_pc_module)
        return;
    if (pr != pr_seen) { /* the flags as the plugin read them (before anything here changed them) */
        pr_seen = pr;
        for (uint32_t k = 0; k < sizeof orig; ++k)
            orig[k] = (uint8_t)get_u8(pr + 9u + k);
        applied = -1;
    }
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 200000000LL)
        return;
    at = t;
    int64_t on = 0;
    if (api->settings_get)
        api->settings_get(api, "mod_police_career", &on);
    const int want = on != 0;
    if (want == applied)
        return;
    applied = want;
    if (!want && get_u8(pr + 0x20u + 0x18u) == 1)
        api->guest_write_u8(api, pr + 0x20u + 0x18u, 0); /* police mode ends */
    for (uint32_t k = 0; k < sizeof orig; ++k)
        api->guest_write_u8(api, pr + 9u + k, want ? orig[k] : 0);
    api->log(api, want ? "core.nfsu: Police Career on for this career" : "core.nfsu: Police Career off for this career");
}
int pc_police_off_now(const PopModApi *api) { /* used by addoncars.inc: a police car can't be the career's car */
    int64_t on = 0;
    if (!g_pc_module)
        return 1;
    if (api->settings_get)
        api->settings_get(api, "mod_police_career", &on);
    return on == 0;
}

static int64_t g_pc_check_at;
static void police_career_pass(const PopModApi *api) {
    pc_switch_pass(api);
    const uint32_t obj = get_u32(CAREER_OBJ), want = obj ? obj + 0x80u : 0;
    if (get_u32(PC_CAREER_CELL) != want)
        api->guest_write_u32(api, PC_CAREER_CELL, want);
    if (g_pc_module) {
        police_role_log(api);
        const uint32_t pr = get_u32(PC_PROFILE_CELL);
        police_camera(api, pr && get_u8(pr + 0x20u + 0x18u) == 1 ? 1 : 0);
        /* H again during a pursuit: the plugin's own pursuit reset (rebased FUN_1013ae90 on the controller; the
         * plugin runs it itself when the player leaves the world or an Interceptor job is called off): the native
         * pursuit, the police it dispatched and its HUD are released and the controller goes back to waiting for
         * an offending racer (its invitation needs the lights on again) */
        if (g_pc_end_chase) {
            g_pc_end_chase = 0;
            const uint32_t ctrl = pr ? pr + 0x13e4u : 0, st = ctrl ? get_u32(ctrl) : 0;
            if (ctrl && get_u8(pr + 0x20u + 0x18u) == 1 && st) {
                /* 2026-11-21: the person's log showed "state 3 -> 3" six times - the kit refused calls into the
                 * plugin's module (fixed in kit hooks.cpp mods_guest_call) and this said "called off" anyway. Now
                 * the message only follows a reset the controller really took. */
                uint32_t r = 0;
                const PopModStatus cs = api->guest_call(api, 0x1013ae90u, ctrl, NULL, 0, &r);
                const uint32_t after = get_u32(ctrl);
                char l[160];
                snprintf(l, sizeof l, "core.nfsu: police pursuit reset by H: call %s, state %u -> %u",
                         cs == POP_OK ? "ok" : "REFUSED", st, after);
                api->log(api, l);
                if (cs == POP_OK && after == 0) {
                    eo_message(api, "Pursuit called off.");
                    g_pc_check_at = now_ns();
                }
            }
        }
        /* a second later: did the plugin take the pursuit up again? (logged for the person's next run) */
        if (g_pc_check_at && now_ns() - g_pc_check_at > 1500000000LL) {
            g_pc_check_at = 0;
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: police pursuit 1.5 s after H: state %u", pr ? get_u32(pr + 0x13e4u) : 0);
            api->log(api, l);
        }
    }
    /* NFSU_PC_LISTCHECK=<s> (diagnostic): at that process age, what the plugin's own career getter (rebased
     * FUN_101711f0) and list accessor (FUN_10171230) answer, beside the game's list */
    static int64_t start;
    static int checked;
    const char *e = getenv("NFSU_PC_LISTCHECK");
    if (e && !checked && obj) {
        if (!start)
            start = now_ns();
        if (now_ns() - start > (int64_t)atoi(e) * 1000000000LL) {
            checked = 1;
            uint32_t got = 0, list = 0;
            api->guest_call(api, 0x101711f0u, 0, NULL, 0, &got);
            if (got)
                api->guest_call(api, 0x10171230u, got, NULL, 0, &list);
            char l[200];
            snprintf(l, sizeof l, "core.nfsu: pc list check: plugin career %08x list %08x; game list %08x (%s)", got,
                     list, obj + 0x88u, list == obj + 0x88u ? "same" : "DIFFERENT");
            api->log(api, l);
        }
    }
}

/* NFSU_PC_TRACE=1 (diagnostic): the adapted plugin's "readable?" helper (rebased FUN_1016f6b0(ptr, size)) with its
 * answers, the first 400 calls. */
static void pc_trace_readable(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static int n;
    if (n++ > 400)
        return;
    uint32_t ret = 0, a0 = 0, a1 = 0;
    api->guest_read_u32(api, cpu->esp, &ret);
    if (cpu->phase == POP_PHASE_AFTER) {
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: pc readable -> %u (eax %08x, back to %08x)", cpu->eax & 0xff, cpu->eax, cpu->eip);
        api->log(api, l);
        return;
    }
    api->guest_read_u32(api, cpu->esp + 4, &a0);
    api->guest_read_u32(api, cpu->esp + 8, &a1);
    uint32_t w = 0;
    api->guest_read_u32(api, a0, &w);
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: pc readable(%08x, %u) from %08x, first dword %08x", a0, a1, ret, w);
    api->log(api, l);
}

PopModStatus pop_mod_init(const PopModApi *api) {
    g_api = api;
    if (getenv("NFSU_PC_TRACE")) {
        static uint32_t th[2];
        api->hook_install(api, 0x1016f6b0u, pc_trace_readable, POP_HOOK_BEFORE, NULL, &th[0]);
        api->hook_install(api, 0x1016f6b0u, pc_trace_readable, POP_HOOK_AFTER, NULL, &th[1]);
    }
    {
        static uint32_t pc_hook;
        api->hook_install(api, GAME_MAIN, police_career_load, POP_HOOK_BEFORE, NULL, &pc_hook);
    }
    addon_cars_install(api);
    addon_query_install(api);
    shop_owned_install(api);
    {
        static uint32_t ar;
        if (api->hook_install(api, 0x00820c70u, addon_active_read, POP_HOOK_AFTER, NULL, &ar) != POP_OK)
            api->log(api, "core.nfsu: add-on active car read hook NOT installed");
    }
    trackpad_install(api);
    stream_install(api);
    aiguard_install(api);
    sell_log_install(api);
    if (!getenv("NFSU_MAP_YIELD_OFF")) {
        static uint32_t mh;
        if (api->hook_install(api, 0x00881e80u, map_job_yield, POP_HOOK_BEFORE, NULL, &mh) != POP_OK)
            api->log(api, "core.nfsu: map job yield NOT installed");
    }
    if (getenv("NFSU_QUEUE_TRACE")) {
        static uint32_t qh[2];
        api->hook_install(api, 0x006aec60u, queue_trace_run, POP_HOOK_REPLACE, NULL, &qh[0]);
        api->hook_install(api, 0x0069e0b0u, queue_trace_add, POP_HOOK_BEFORE, NULL, &qh[1]);
    }
    if (getenv("NFSU_TRIG_CHECK")) {
        static uint32_t trig_hooks[13];
        static const uint32_t thunks[13] = {0x008ae1c2u, 0x008ae1d4u, 0x008ae1b0u, 0x008ae1dau, 0x008ae1bcu,
                                            0x008ae1b6u, 0x008ae1ceu, 0x008ae57eu, 0x008ae6b0u, 0x008ae89au,
                                            0x00b40b62u, 0x00b40b68u, 0x008ae1c8u};
        for (unsigned k = 0; k < 13; ++k)
            api->hook_install(api, thunks[k], trig_check, POP_HOOK_REPLACE, (void *)(uintptr_t)k, &trig_hooks[k]);
    }
    install_counters(api);
    {
        if (getenv("NFSU_GLOW")) /* A/B: 0 as in the game, 1 60%, 2 30%, 3 off */
            g_glow_env = atoi(getenv("NFSU_GLOW"));
        if (getenv("NFSU_DOF")) /* A/B: 0 as in the game, 1 weaker, 2 off */
            g_dof_env = atoi(getenv("NFSU_DOF"));
        static uint32_t vt_hooks[3];
        if (g_glow_env >= 0) /* glow scaling stays an A/B experiment: the game's glow field was 0 when measured */
            api->hook_install(api, VT_DRAW, vt_draw, POP_HOOK_REPLACE, NULL, &vt_hooks[0]);
        api->hook_install(api, VT_DOF_ACTIVE, dof_active, POP_HOOK_REPLACE, NULL, &vt_hooks[1]);
        api->hook_install(api, VT_DOF_PARAMS, dof_params, POP_HOOK_REPLACE, NULL, &vt_hooks[2]);
        vt_settings(api);
        char line[120];
        snprintf(line, sizeof line, "core.nfsu: glow %.0f%%, depth of field %s", g_glow_scale * 100.0,
                 g_dof_mode == 2 ? "off" : g_dof_mode == 1 ? "weaker" : "as in the game");
        api->log(api, line);
    }
    {
        static uint32_t mb_draw_hook;
        api->hook_install(api, MOTION_BLUR_DRAW, motion_blur_draw, POP_HOOK_REPLACE, NULL, &mb_draw_hook);
    }
    {
        int64_t blur = 0;
        if (api->settings_get)
            api->settings_get(api, "motion_blur", &blur);
        if (getenv("NFSU_MOTION_BLUR")) /* A/B: 0 smooth, 1 as in the game, 2 smooth and lighter, 3 off */
            blur = atoi(getenv("NFSU_MOTION_BLUR"));
        g_blur_env = getenv("NFSU_MOTION_BLUR") ? (int)blur : -1;
        blur_settings(api, 1);
        { /* always installed: the setting applies live (blur_settings) */
            static uint32_t blur_hook;
            api->hook_install(api, MOTION_BLUR_QUAD, motion_blur_quad, POP_HOOK_BEFORE, NULL, &blur_hook);
        }
    }
    {
        int64_t tint = 0;
        if (api->settings_get)
            api->settings_get(api, "colour_filter", &tint);
        if (getenv("NFSU_COLOUR_FILTER")) /* A/B: 0 as in the game, 1 softer, 2 neutral */
            tint = atoi(getenv("NFSU_COLOUR_FILTER"));
        if (tint == 1 || tint == 2) {
            static uint32_t lut_hook;
            g_tint_mix = tint == 2 ? 1.0f : 0.5f;
            api->hook_install(api, LUT_BUILD, lut_build, POP_HOOK_BEFORE, NULL, &lut_hook);
            api->log(api, tint == 2 ? "core.nfsu: colour filter: neutral" : "core.nfsu: colour filter: softer");
        }
    }
    {
        int64_t on = 1;
        if (api->settings_get)
            (void)0; /* "mac_resolutions": always on (2026-11-17, no longer a menu row) */
        if (on)
            mac_resolutions(api);
        on = 1;
        if (api->settings_get)
            (void)0; /* "widescreen": always on (2026-11-17, no longer a menu row) */
        int64_t fill = 1;
        if (api->settings_get)
            api->settings_get(api, "hud_fill", &fill);
        if (getenv("NFSU_HUD_FILL"))
            fill = atoi(getenv("NFSU_HUD_FILL"));
        g_hud_fill = fill != 0;
        if (on)
            widescreen_install(api);
    }
    {
        const char *ft = getenv("RECOMP_FREE_THREADS");
        g_render_free = ft && strstr(ft, "RenderThread") != NULL;
        if (getenv("NFSU_PUMP_MS"))
            pump_ns = atoll(getenv("NFSU_PUMP_MS")) * 1000000LL;
        if (getenv("NFSU_IDLE_US"))
            idle_ns = atol(getenv("NFSU_IDLE_US")) * 1000L;
        static uint32_t pump_hook, beat_hook;
        api->hook_install(api, MESSAGE_PUMP, pump_throttle, POP_HOOK_REPLACE, NULL, &pump_hook);
        api->hook_install(api, SLEEP_HELPER, inactive_heartbeat, POP_HOOK_BEFORE, NULL, &beat_hook);
        if (g_render_free) {
            static uint32_t free_hooks[2];
            api->hook_install(api, SLEEP_HELPER, render_idle_sleep, POP_HOOK_BEFORE, NULL, &free_hooks[1]);
            api->log(api, "core.nfsu: the render thread runs on its own core; its idle loop is throttled");
        }
    }
    {
        int64_t menu = 0;
        if (api->settings_get)
            api->settings_get(api, "menu_fps", &menu);
        static const int fps_of[4] = {45, 30, 60, 0};
        int fps = fps_of[menu >= 0 && menu <= 3 ? menu : 0];
        if (getenv("NFSU_MENU_FPS")) /* A/B: frames per second, 0 = not limited */
            fps = atoi(getenv("NFSU_MENU_FPS"));
        if (getenv("NFSU_MENU_IDLE_MS"))
            g_menu_idle_ns = (int64_t)atoi(getenv("NFSU_MENU_IDLE_MS")) * 1000000LL;
        g_menu_fps_env = getenv("NFSU_MENU_FPS") != NULL;
        g_menu_period_ns = fps > 0 ? 1000000000LL / fps : 0;
        { /* always installed: the setting applies live */
            static uint32_t pace_hooks[2];
            api->hook_install(api, MAIN_LOOP_STEP, menu_pace, POP_HOOK_BEFORE, NULL, &pace_hooks[1]);
        }
    }
    { /* always installed: the setting applies live */
        static uint32_t sf_hook;
        api->hook_install(api, START_FLOW_STEP, start_flow_step, POP_HOOK_REPLACE, NULL, &sf_hook);
    }
    if (getenv("NFSU_DIALOG_LOG")) {
        static uint32_t dh[2];
        api->hook_install(api, 0x005a3be0u, dialog_log, POP_HOOK_BEFORE, (void *)0, &dh[0]);
        api->hook_install(api, 0x005a3d80u, dialog_log, POP_HOOK_BEFORE, (void *)1, &dh[1]);
        static uint32_t fh;
        api->hook_install(api, 0x005965f0u, fe_state_log, POP_HOOK_BEFORE, NULL, &fh);
        static uint32_t lh[3];
        api->hook_install(api, 0x005b1830u, login_msg_log, POP_HOOK_BEFORE, NULL, &lh[0]);
        api->hook_install(api, 0x0057f5f0u, login_ctor_log, POP_HOOK_BEFORE, (void *)"sign-in base ctor 0057f5f0", &lh[1]);
        api->hook_install(api, 0x0074e2b0u, login_ctor_log, POP_HOOK_BEFORE, (void *)"pc sign-in ctor 0074e2b0", &lh[2]);
    }
    {
        static uint32_t gh[4];
        api->hook_install(api, 0x00769d60u, gauss_before, POP_HOOK_BEFORE, NULL, &gh[0]);
        api->hook_install(api, 0x00769d60u, gauss_after, POP_HOOK_AFTER, NULL, &gh[1]);
    }
    if (getenv("NFSU_GAUSS_LOG")) {
        static uint32_t gh[4];
        api->hook_install(api, 0x008ae6b0u, gauss_exp, POP_HOOK_BEFORE, NULL, &gh[2]);
        api->hook_install(api, 0x008ae6b0u, gauss_exp, POP_HOOK_AFTER, (void *)1, &gh[3]);
    }
    if (getenv("NFSU_WAITLOG")) {
        static uint32_t wh[2];
        api->hook_install(api, 0x00769390u, render_wait_time, POP_HOOK_REPLACE, (void *)0, &wh[0]);
        api->hook_install(api, 0x00769460u, render_wait_time, POP_HOOK_REPLACE, (void *)1, &wh[1]);
    }
    if (getenv("NFSU_APTLOG")) {
        static uint32_t apt_hook;
        api->hook_install(api, 0x00a0d5d0u, apt_dt, POP_HOOK_BEFORE, NULL, &apt_hook);
        static uint32_t apt_ft_hook;
        api->hook_install(api, 0x00a1c960u, apt_frame_time, POP_HOOK_REPLACE, NULL, &apt_ft_hook);
    }
    if (getenv("NFSU_DTLOG")) {
        static uint32_t dt_hook;
        api->hook_install(api, 0x006a66f0u, dt_log, POP_HOOK_BEFORE, NULL, &dt_hook);
    }
    if (getenv("NFSU_CAM_SNAP")) {
        static uint32_t cam_snap_hook;
        api->hook_install(api, 0x004e0e30u, cam_snap, POP_HOOK_REPLACE, 0, &cam_snap_hook);
        install_probes(api);
    }
    {
        const char *d = getenv("NFSU_DIAG");
        if (d && d[0] == '1') {
            static uint32_t cam_param_hook;
            api->hook_install(api, 0x004cb3e0u, cam_param_check, POP_HOOK_REPLACE, 0, &cam_param_hook);
            for (unsigned k = 0; k < 4; ++k)
                api->hook_install(api, g_mover_addrs[k], mover_trace, POP_HOOK_BEFORE, (void *)(uintptr_t)k,
                                  &g_mover_hooks[k]);
            static uint32_t attrib_lookup_hook;
            api->hook_install(api, 0x00464a20u, attrib_lookup_check, POP_HOOK_REPLACE, 0, &attrib_lookup_hook);
            static uint32_t fixup_hook, init_hooks[3];
            static const uint32_t inits[3] = {0x00bbcd00u, 0x00bbcd20u, 0x00bbcd40u};
            for (unsigned i = 0; i < 3; ++i)
                api->hook_install(api, inits[i], attr_trace, POP_HOOK_BEFORE, (void *)(uintptr_t)inits[i],
                                  &init_hooks[i]);
            api->hook_install(api, 0x00466ec0u, fixup_trace, POP_HOOK_BEFORE, 0, &fixup_hook);
            for (unsigned i = 0; i < sizeof g_attr_addrs / sizeof g_attr_addrs[0]; ++i)
                api->hook_install(api, g_attr_addrs[i], attr_trace, POP_HOOK_BEFORE,
                                  (void *)(uintptr_t)g_attr_addrs[i], &g_attr_hooks[i]);
            api->hook_install(api, VAULT_LOAD, vault_check, POP_HOOK_BEFORE, 0, &g_vault_hook);
            api->hook_install(api, MEMSET_THUNK, memset_check, POP_HOOK_BEFORE, 0, &g_memset_hook);
            api->hook_install(api, SOUND_BANK_LOAD, bank_check, POP_HOOK_BEFORE, 0, &g_bank_hook);
        }
    }
    const char *diag = getenv("NFSU_DIAG");
    if (diag && diag[0] == '1') {
        api->hook_install(api, GS_CHECK, gs_check, POP_HOOK_BEFORE, 0, &g_hooks[3]);
        for (unsigned i = 0; i < sizeof g_trace_addrs / sizeof g_trace_addrs[0]; ++i)
            api->hook_install(api, g_trace_addrs[i], esp_trace, POP_HOOK_BEFORE, (void *)(uintptr_t)g_trace_addrs[i],
                              &g_trace_hooks[i]);
    }
    PopModStatus s = api->hook_install(api, RENDER_WAIT_STEP, render_wait_yield, POP_HOOK_BEFORE, 0, &g_hooks[0]);
    PopModStatus r = api->hook_install(api, RENDER_RING_RUN, ring_wait_yield, POP_HOOK_BEFORE, 0, &g_hooks[1]);
    PopModStatus f = api->hook_install(api, FILE_SIZE_QUERY, region_extra_size, POP_HOOK_REPLACE, 0, &g_hooks[2]);
    char line[200];
    snprintf(line, sizeof line, "core.nfsu: loaded; hooks at %08x %s, %08x %s, %08x %s", RENDER_WAIT_STEP,
             s == POP_OK ? "installed" : "FAILED", RENDER_RING_RUN, r == POP_OK ? "installed" : "FAILED",
             FILE_SIZE_QUERY, f == POP_OK ? "installed" : "FAILED");
    api->log(api, line);
    return POP_OK;
}

PopModStatus pop_mod_exit(void) {
    for (unsigned i = 0; i < sizeof g_hooks / sizeof g_hooks[0]; ++i)
        if (g_hooks[i]) {
            g_api->hook_remove(g_api, g_hooks[i]);
            g_hooks[i] = 0;
        }
    return POP_OK;
}
