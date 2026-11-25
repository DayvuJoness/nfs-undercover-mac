/* stream.inc - the world loaded all round the helicopter and far ahead of it (2026-11-23; the person: "the whole
 * map on screen must be loaded before I look at it, so I never see anything load - flying like in GTA").
 *
 * How the game streams (nfs.exe 1.0.0.1): the track streamer is the object at 0x01317f30 (pool 168 MB at
 * [+0xe50], size [+0xe54]; FUN_0082a020). Its section table [+0] / count [+4] has 0x5c-byte rows: +8 the section
 * number (short), +0xb the "wanted" bits, +0xc state (4 = loaded), +0x18 its size. The visible-section manager at
 * 0x012fe7e0 has two lists of zones (circular, head +0 and +8; node: +8 the zone's section number, +0x18 / +0x20
 * its 2D box min / max in render coordinates, +0x30 its outline) and, at +0x10, one load list per zone (node: +0xc
 * the zone's number, +0x12 a count, +0x14 the section numbers to have loaded while in that zone). FUN_0082a730 /
 * FUN_0082a1d0 find the zone under the car (FUN_00829110: none when the car is far from every zone), and
 * FUN_0082a410 marks what to load: a few always-loaded sections and the current zone's list, each through
 * FUN_00828570 (thiscall streamer, (short *list, count, slot), RET 0xc). It runs again only when the zone
 * changes; FUN_0082e1e0 (fastcall streamer) is the game's own "refresh now".
 *
 * So the zones were made for a car on the road: from the air the view reaches zones whose sections were never
 * asked for, and off the roads the car is in no zone at all. While flying, every half second the zones whose box is
 * within NEAR metres of the helicopter, or within FAR metres and in front of the camera (±75°), are picked, their
 * load lists joined (nearest zones first), and the sections added after the game's own marking (AFTER hook on
 * FUN_0082a410) up to a memory budget (the pool less what is already wanted, less a margin); when the set changes
 * FUN_0082e1e0 refreshes it. On the ground, or with setting off, nothing is added.
 * Env: NFSU_STREAM_OFF=1 (old), NFSU_STREAM_RANGE=near,far metres (default 450,1400), NFSU_STREAM_LOG=1. */

#define STREAMER 0x01317f30u
#define VISMGR 0x012fe7e0u
static uint32_t g_stream_buf;   /* guest buffer for the section list */
static int g_stream_n;          /* sections in it */
static uint32_t g_stream_sig;   /* to see a change */
static uint32_t g_stream_hook;
static uint32_t *g_sec_row; /* a section's row address by its number (stream_sizes) */
static int64_t g_stream_used;   /* bytes of ours in the streamer's wanted total */

static void stream_after_mark(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (g_stream_n <= 0 || !g_stream_buf || !api->guest_call)
        return;
    /* the wanted list is 512 entries ([+0x650] up to the pool pointer at +0xe50) and FUN_00828570 doesn't check:
     * 611 sections ran over it and hung the game. Ours (nearest first) only fill what the game left, less a margin */
    const int have = (int)get_u32(STREAMER + 0x64c);
    const int room = 500 - have;
    if (room <= 0)
        return;
    /* only sections not wanted yet (row +0xb == 0) take a place: the game's own zone list is usually most of ours, and
     * passing those too used up the room (339 of 500 filled downtown). They go to the buffer's second half. */
    const uint32_t out = g_stream_buf + 4096u;
    int n = 0;
    for (int i = 0; i < g_stream_n && n < room && n < 2000; ++i) {
        const int s = (int16_t)get_u16(g_stream_buf + 2u * (uint32_t)i);
        const uint32_t row = g_sec_row && s > 0 ? g_sec_row[s] : 0;
        if (!row || ((get_u32(row + 8) >> 24) & 0xffu))
            continue;
        const uint32_t a = out + 2u * (uint32_t)(n & ~1), w = get_u32(a);
        api->guest_write_u32(api, a, (n & 1) ? ((w & 0xffffu) | ((uint32_t)(uint16_t)s << 16)) : ((w & 0xffff0000u) | (uint16_t)s));
        ++n;
    }
    if (n <= 0)
        return;
    const uint32_t args[3] = {out, (uint32_t)n, 0};
    uint32_t r = 0;
    api->guest_call(api, 0x00828570u, STREAMER, args, 3, &r);
}

static float stream_f(uint32_t a) {
    const uint32_t u = get_u32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* the section table's sizes by section number (read once, the table is fixed) */
static uint32_t *g_sec_size;
static void stream_sizes(void) {
    if (g_sec_size)
        return;
    const uint32_t rows = get_u32(STREAMER), n = get_u32(STREAMER + 4);
    if (!rows || !n || n > 20000)
        return;
    g_sec_size = (uint32_t *)calloc(32768, sizeof(uint32_t));
    g_sec_row = (uint32_t *)calloc(32768, sizeof(uint32_t));
    if (!g_sec_size || !g_sec_row)
        return;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t r = rows + i * 0x5cu;
        const int num = (int16_t)get_u16(r + 8);
        if (num > 0) {
            g_sec_size[num] = get_u32(r + 0x18);
            g_sec_row[num] = r;
        }
    }
}

struct StreamZone {
    float d;
    int num;
};
static int stream_zone_cmp(const void *a, const void *b) {
    const float x = ((const struct StreamZone *)a)->d, y = ((const struct StreamZone *)b)->d;
    return x < y ? -1 : x > y;
}

static struct StreamZone g_zones[2048];
static int g_nz; /* zones picked by the last stream pass, nearest first (0 when not flying) */

/* ---- WHAT IS DRAWN FROM THE AIR ----
 * The game draws, per view, only the sections FUN_0082ae10 lists (stdcall (short *out, max 128, view), RET 0xc): the load list
 * of the zone under the camera (FUN_00829110 at the view's position), the ones loaded (the section's render record
 * [0x012fe83c + 4 * number] and its +4 set), after the always-on 25000..25999. That is the zone's view from the
 * ROAD: from the air half the buildings and the roofs were missing although loaded (pics/25). While flying the list
 * is instead the always-on 25000.. and then the loaded sections nearest the helicopter that are not far behind the
 * camera, up to the same 128 the caller's buffer holds (more is not possible: the render-info pool FUN_00401900
 * writes to address 0 when it runs out). Rebuilt ten times a second; NFSU_STREAM_DRAW_OFF=1 = the game's. */
static int16_t g_draw[128];
static int g_draw_n = -1; /* -1: the game's own list */
static int stream_loaded(int s) {
    if (s < 0 || s > 32767)
        return 0;
    const uint32_t rec = get_u32(0x012fe83cu + 4u * (uint32_t)s);
    return rec && get_u32(rec + 4);
}
/* the helicopter's place and the camera's forward in render coordinates, for the draw list */
static float g_dr_x, g_dr_y, g_dr_z, g_dr_fx, g_dr_fy;
struct DrawCand {
    float score;
    int16_t num;
};
static int draw_cand_cmp(const void *a, const void *b) {
    const float x = ((const struct DrawCand *)a)->score, y = ((const struct DrawCand *)b)->score;
    return x < y ? -1 : x > y;
}
/* every loaded section with a box ([[0x012fe83c + 4 * n] + 4] + 0x2c: min at +0, max at +0x10, what the game's own
 * box-vs-view test FUN_0074a1d0 reads): the ones in front of the camera (or around it) by distance from the
 * helicopter to the box, nearest first; the 128 nearest are drawn */
static void stream_draw_build(void) {
    static int64_t at;
    const int64_t now = now_ns();
    if (now - at < 100000000LL)
        return;
    at = now;
    if (g_nz <= 0 || getenv("NFSU_STREAM_DRAW_OFF")) {
        g_draw_n = -1;
        return;
    }
    static struct DrawCand cand[4096];
    int nc = 0, n = 0;
    for (int s = 25000; s < 26000 && n < 128; ++s)
        if (stream_loaded(s))
            g_draw[n++] = (int16_t)s;
    for (int s = 0; s < 25000 && nc < 4096; ++s) {
        const uint32_t rec = get_u32(0x012fe83cu + 4u * (uint32_t)s);
        if (!rec)
            continue;
        const uint32_t sec = get_u32(rec + 4);
        if (!sec)
            continue;
        const uint32_t box = get_u32(sec + 0x2c);
        if (box < 0x00400000u)
            continue;
        const float x0 = stream_f(box), y0 = stream_f(box + 4), z0 = stream_f(box + 8);
        const float x1 = stream_f(box + 0x10), y1 = stream_f(box + 0x14), z1 = stream_f(box + 0x18);
        if (!(x1 >= x0) || !(y1 >= y0) || !(z1 >= z0))
            continue;
        const float cx = g_dr_x < x0 ? x0 : g_dr_x > x1 ? x1 : g_dr_x;
        const float cy = g_dr_y < y0 ? y0 : g_dr_y > y1 ? y1 : g_dr_y;
        const float cz = g_dr_z < z0 ? z0 : g_dr_z > z1 ? z1 : g_dr_z;
        const float d = sqrtf((cx - g_dr_x) * (cx - g_dr_x) + (cy - g_dr_y) * (cy - g_dr_y) + (cz - g_dr_z) * (cz - g_dr_z));
        /* behind the camera by more than 60 m: not drawn (the box's nearest point decides, so a big section the
         * camera is inside always counts) */
        const float ahead = (cx - g_dr_x) * g_dr_fx + (cy - g_dr_y) * g_dr_fy;
        if (ahead < -60.0f && d > 60.0f)
            continue;
        cand[nc].score = d;
        cand[nc].num = (int16_t)s;
        ++nc;
    }
    qsort(cand, (size_t)nc, sizeof cand[0], draw_cand_cmp);
    for (int i = 0; i < nc && n < 128; ++i)
        g_draw[n++] = cand[i].num;
    g_draw_n = n;
    static int64_t said;
    if (getenv("NFSU_STREAM_LOG") && now - said > 2000000000LL) {
        said = now;
        const uint32_t rows = get_u32(STREAMER), nrow = get_u32(STREAMER + 4);
        int wanted = 0, loaded = 0, wl = 0, states[8] = {0};
        for (uint32_t i = 0; i < nrow && i < 20000; ++i) {
            const uint32_t r = rows + i * 0x5cu;
            const int w = ((get_u32(r + 8) >> 24) & 0xffu) != 0, st = (int)get_u32(r + 0xc);
            wanted += w;
            loaded += st == 4;
            wl += w && st == 4;
            if (w && st >= 0 && st < 8)
                ++states[st];
        }
        fprintf(stderr, "[mod core.nfsu] stream: rows wanted %d, loaded %d, wanted and loaded %d; wanted by state 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d; list count %u\n",
                wanted, loaded, wl, states[0], states[1], states[2], states[3], states[4], states[5], get_u32(STREAMER + 0x64c));
        fprintf(stderr, "[mod core.nfsu] stream: draw list %d of %d loaded sections in view (farthest drawn %.0f m)\n",
                n, nc, nc ? (double)cand[(n > 0 ? n : 1) - 1 < nc ? (n > 0 ? n : 1) - 1 : nc - 1].score : 0.0);
    }
}
static void stream_draw_list(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const int n0 = g_draw_n;
    const uint32_t out = get_u32(cpu->esp + 4), max = get_u32(cpu->esp + 8);
    if (n0 < 0 || !out || max == 0) {
        api->call_next(api, inv, cpu);
        return;
    }
    const int n = n0 < (int)max ? n0 : (int)max;
    for (int i = 0; i + 1 < n; i += 2)
        api->guest_write_u32(api, out + 2u * (uint32_t)i, (uint16_t)g_draw[i] | ((uint32_t)(uint16_t)g_draw[i + 1] << 16));
    if (n & 1) {
        const uint32_t a = out + 2u * (uint32_t)(n - 1), w = get_u32(a);
        api->guest_write_u32(api, a, (w & 0xffff0000u) | (uint16_t)g_draw[n - 1]);
    }
    api->hook_return(api, cpu, (uint32_t)n, 0xc); /* RET 0xc (stdcall, not the cdecl Ghidra shows) */
}

/* flying: hx, hy the helicopter in render coordinates, fx, fy the camera's forward there */
static void stream_pass(const PopModApi *api, int flying, float hx, float hy, float fx, float fy, float hz) {
    g_dr_x = hx, g_dr_y = hy, g_dr_z = hz, g_dr_fx = fx, g_dr_fy = fy;
    /* Not flying: ours are simply dropped from the next marking - no refresh forced. (The crash 2026-11-23: leaving
     * the helicopter, the game reloads the world for the other car; the forced refresh unloaded our ~300 sections in
     * the middle of that, and the render thread's deferred-command list FUN_00791c40 then ran a freed object - a call
     * to 0 at 0x00791c61.) Refreshes happen only while driving in the world (state 6, world ticking). */
    if (!flying) {
        g_stream_n = 0;
        g_nz = 0;
        g_draw_n = -1;
        g_stream_sig = 0;
        g_stream_used = 0;
        return;
    }
    if (!tp_driving())
        return;
    static int64_t at;
    const int64_t now = now_ns();
    if (getenv("NFSU_STREAM_OFF") || !get_u32(STREAMER) || now - at < 500000000LL)
        return;
    at = now;
    static uint32_t dh;
    if (!dh && api->hook_install(api, 0x0082ae10u, stream_draw_list, POP_HOOK_REPLACE, NULL, &dh) != POP_OK) {
        api->log(api, "core.nfsu: stream: draw list hook NOT installed");
        dh = 1;
    }
    if (!g_stream_hook) {
        if (api->hook_install(api, 0x0082a410u, stream_after_mark, POP_HOOK_AFTER, NULL, &g_stream_hook) != POP_OK) {
            api->log(api, "core.nfsu: stream: hook NOT installed");
            g_stream_hook = 1;
            return;
        }
    }
    if (!g_stream_buf && (!api->guest_alloc || api->guest_alloc(api, 8192, &g_stream_buf) != POP_OK || !g_stream_buf))
        return;
    stream_sizes();
    struct StreamZone *zones = g_zones;
    int nz = 0;
    if (flying) {
        float nearr = 450.0f, farr = 1400.0f;
        const char *e = getenv("NFSU_STREAM_RANGE");
        if (e)
            sscanf(e, "%f,%f", &nearr, &farr);
        for (int li = 0; li < 2; ++li) {
            const uint32_t head = VISMGR + 8u * (uint32_t)li;
            uint32_t node = get_u32(head);
            for (int guard = 0; node && node != head && guard < 8000; ++guard, node = get_u32(node)) {
                const float x0 = stream_f(node + 0x18), y0 = stream_f(node + 0x1c);
                const float x1 = stream_f(node + 0x20), y1 = stream_f(node + 0x24);
                if (!(x1 >= x0) || !(y1 >= y0))
                    continue;
                const float cx = hx < x0 ? x0 : hx > x1 ? x1 : hx, cy = hy < y0 ? y0 : hy > y1 ? y1 : hy;
                const float d = sqrtf((cx - hx) * (cx - hx) + (cy - hy) * (cy - hy));
                int take = d <= nearr;
                if (!take && d <= farr) { /* in front: the box's centre within 75 degrees of the view */
                    const float mx = 0.5f * (x0 + x1) - hx, my = 0.5f * (y0 + y1) - hy;
                    const float ml = sqrtf(mx * mx + my * my);
                    take = ml < 1.0f || (mx * fx + my * fy) / ml > 0.26f;
                }
                if (take && nz < 2048) {
                    zones[nz].d = d;
                    zones[nz].num = (int16_t)get_u16(node + 8);
                    ++nz;
                }
            }
        }
        qsort(zones, (size_t)nz, sizeof zones[0], stream_zone_cmp);
    }
    g_nz = nz;
    if (getenv("NFSU_STREAM_ZONEDUMP") && nz > 0) { /* diagnostic: the nearest zones' lists, once */
        static int done;
        if (!done) {
            done = 1;
            for (int z = 0; z < nz && z < 6; ++z) {
                uint32_t ln = get_u32(VISMGR + 0x10);
                for (int guard = 0; ln && ln != VISMGR + 0x10 && guard < 8000; ++guard, ln = get_u32(ln))
                    if ((int16_t)get_u16(ln + 0xc) == zones[z].num)
                        break;
                char l[2400];
                const int cnt = ln && ln != VISMGR + 0x10 ? (int16_t)get_u16(ln + 0x12) : 0;
                int m = snprintf(l, sizeof l, "core.nfsu: zone %d at %.0f m, %d in its list:", zones[z].num,
                                 (double)zones[z].d, cnt);
                for (int k = 0; k < cnt && m < 2300; ++k) {
                    const int sn = (int16_t)get_u16(ln + 0x14 + 2u * (uint32_t)k);
                    m += snprintf(l + m, sizeof l - m, " %c%d:%uK", 'A' + sn / 1000, sn % 1000,
                                  g_sec_size && sn > 0 ? g_sec_size[sn] >> 10 : 0);
                }
                api->log(api, l);
            }
        }
    }
    /* their load lists, nearest zones first, deduplicated, within the memory budget */
    static uint8_t seen[4096];
    memset(seen, 0, sizeof seen);
    const uint32_t pool = get_u32(STREAMER + 0xe54), wanted = get_u32(STREAMER + 0x148);
    /* what the game wants by itself: the rows with a wanted bit, less ours (marked by the previous list); the
     * total stays within 70% of the pool (the loader waits for room it doesn't get when the pool is overfull: a
     * first try that trusted the slot counter at +0x148 hung the game) */
    static uint8_t ours[4096];
    int64_t own = 0;
    {
        const uint32_t rows = get_u32(STREAMER), nrow = get_u32(STREAMER + 4);
        for (uint32_t i = 0; i < nrow && i < 20000; ++i) {
            const uint32_t r = rows + i * 0x5cu;
            const int num = (int16_t)get_u16(r + 8);
            const int wanted_bits = (get_u32(r + 8) >> 24) & 0xffu; /* +0xb */
            if (wanted_bits && !(num > 0 && (ours[num >> 3] & (1u << (num & 7)))))
                own += get_u32(r + 0x18);
        }
    }
    (void)wanted;
    const int64_t budget = (int64_t)pool * 7 / 10 - own;
    memset(ours, 0, sizeof ours);
    int64_t used = 0;
    int n = 0;
    uint32_t sig = 2166136261u;
    for (int z = 0; z < nz && n < 2000; ++z) {
        uint32_t ln = get_u32(VISMGR + 0x10), found = 0;
        for (int guard = 0; ln && ln != VISMGR + 0x10 && guard < 8000; ++guard, ln = get_u32(ln))
            if ((int16_t)get_u16(ln + 0xc) == zones[z].num) {
                found = ln;
                break;
            }
        if (!found)
            continue;
        const int cnt = (int16_t)get_u16(found + 0x12);
        for (int k = 0; k < cnt && k < 1024 && n < 2000; ++k) {
            const int s = (int16_t)get_u16(found + 0x14 + 2u * (uint32_t)k);
            if (s <= 0 || (seen[s >> 3] & (1u << (s & 7))))
                continue;
            seen[s >> 3] |= (uint8_t)(1u << (s & 7));
            const uint32_t size = g_sec_size ? g_sec_size[s] : 0;
            if (used + size > budget)
                continue;
            used += size;
            ours[s >> 3] |= (uint8_t)(1u << (s & 7));
            const uint32_t w = get_u32(g_stream_buf + 2u * (uint32_t)(n & ~1)); /* shorts, two per dword */
            api->guest_write_u32(api, g_stream_buf + 2u * (uint32_t)(n & ~1),
                                 (n & 1) ? ((w & 0xffffu) | ((uint32_t)(uint16_t)s << 16))
                                         : ((w & 0xffff0000u) | (uint16_t)s));
            sig = (sig ^ (uint32_t)s) * 16777619u;
            ++n;
        }
    }
    if (sig == g_stream_sig && n == g_stream_n)
        return;
    g_stream_sig = sig;
    g_stream_n = n;
    g_stream_used = used;
    uint32_t r = 0;
    api->guest_call(api, 0x0082e1e0u, STREAMER, NULL, 0, &r); /* the game's refresh: marks again, with ours */
    if (getenv("NFSU_STREAM_LOG")) {
        char l[200];
        snprintf(l, sizeof l, "core.nfsu: stream: %d zones, %d extra sections (%.1f MB; the game's own %.1f MB, pool %.1f MB)",
                 nz, n, (double)used / 1048576.0, (double)own / 1048576.0, (double)pool / 1048576.0);
        api->log(api, l);
    }
}

/* A bigger streaming pool (so more of the world fits at once): at boot FUN_006ab7f0 allocates it with
 * FUN_004b3e30(0x0a052000 bytes, 0x2000, pool [0x00d3bdb4]) (cdecl, returns at 0x006ab835) and the streamer is told
 * the size by FUN_0082a020(pool, size) (thiscall, RET 8, called at 0x005f77a0). Both get the new size (BEFORE
 * hooks rewriting the argument). NFSU_STREAM_POOL_MB=<MB> (default 256; 0 = the game's 160). The hangs first blamed on
 * the pool were the wanted list overflowing (see stream_after_mark). */
static uint32_t g_pool_bytes;
static void stream_pool_alloc(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    (void)api, (void)cpu; /* the game's own allocation is left as it is (see stream_pool_size) */
}
/* the bigger pool is a block of its own (VirtualAlloc, 8 KB aligned) handed to the streamer in place of the game's
 * 160 MB one, which then lies unused: nothing else reads [0x00da51fc] */
static void stream_pool_size(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (!g_pool_bytes || get_u32(cpu->esp + 8) != 0x0a052000u)
        return;
    /* from the game's own address space: KERNEL32!VirtualAlloc through its import slot 0x00bd61b8 (the mod heap is
     * ~15 MB) - VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), page aligned */
    uint32_t blk = 0;
    char l[140];
    const uint32_t va_args[4] = {0, g_pool_bytes + 0x2000u, 0x3000u, 0x04u};
    if (!api->guest_call || api->guest_call(api, get_u32(0x00bd61b8u), 0, va_args, 4, &blk) != POP_OK || !blk) {
        snprintf(l, sizeof l, "core.nfsu: stream: no room for a %u MB streaming pool; the game's 160 MB", g_pool_bytes >> 20);
        api->log(api, l);
        return;
    }
    const uint32_t at = (blk + 0x1fffu) & ~0x1fffu;
    api->guest_write_u32(api, cpu->esp + 4, at);
    api->guest_write_u32(api, cpu->esp + 8, g_pool_bytes);
    snprintf(l, sizeof l, "core.nfsu: stream: streaming pool %u MB of its own at %08x", g_pool_bytes >> 20, at);
    api->log(api, l);
}
static void stream_install(const PopModApi *api) {
    const char *e = getenv("NFSU_STREAM_POOL_MB");
    const int mb = e ? atoi(e) : 256;
    g_pool_bytes = mb > 160 && mb <= 1024 ? (uint32_t)mb << 20 : 0;
    static uint32_t h1, h2;
    (void)h1, (void)stream_pool_alloc;
    if (api->hook_install(api, 0x0082a020u, stream_pool_size, POP_HOOK_BEFORE, NULL, &h2) != POP_OK)
        api->log(api, "core.nfsu: stream: pool hooks NOT installed");
}
