/* trackpad.inc - included by nfsu.c. Trackpad taps for a MacBook played without a pad (setting trackpad_haptics).
 *
 * The person plays on the keyboard and asked (2026-11-22) for the Force Touch trackpad to answer: crashes (strong and
 * frequent, like a ratchet, harder for harder hits), broken objects, gear changes, a race's or checkpoint timer
 * running out and other timers, and every item switch in every menu. The kit's host
 * (kit/host/sdl/trackpad_haptics_mac.cpp) clicks the trackpad: host_trackpad_haptic(level 1..3, clicks, ms apart),
 * found with dlsym so other hosts simply have none. The trackpad only clicks; a "rumble" is a burst of clicks.
 *
 * Where the game says so (2026-11-22):
 *  - The game's own force feedback is NO source: FUN_0069ad30 (the "SteeringWheelDevice" update, vtable 0x00c0d378)
 *    is not even called without a DirectInput wheel (measured: 0 calls in a drive), and its event setter 0x0069a4c0
 *    is not a listed function. So the game's events ("E..." classes; tools/dev/nfsu_event_ctors.py lists every
 *    one with the listed function that writes its vtable) are hooked at their constructors:
 *  - CRASHES: ECollision (vtable 0x00c0b7d4), made by hkNFSCollisionModifierECollision slot 0 (FUN_006d7130; the
 *    modifier is created per contact by FUN_006de5b0 from 006e2ab0/006e3570/006e3960) with the 0xd8-byte collision
 *    info copied by value into the constructor FUN_00667d60 (RET 0xd8). Info +0x00 the contact point (sim
 *    coordinates, y up), +0x20 the closing speed in m/s, +0x2c the impulse in N s (measured: 9 m/s -> ~22 000 against
 *    a wall, ~2400 x speed; a car pressed against a wall: 13 000-48 000 at 0-0.8 m/s). The ids at +0x0c/+0x7c are
 *    surface types, not bodies (both sides can carry the same one). Every crash in five test drives followed the
 *    player's path; menu-background traffic crashes also come through, so only while driving (tp_driving). Whether
 *    two AI cars crashing far away can come through was not settled (the "crashes" seen before the script drove were
 *    the person playing in the test window). A knock's strength follows the impulse: a hit at speed = a strong,
 *    close, long knock dying away; a sign = a faint tick.
 *  - BROKEN OBJECTS: ESpawnExplosion FUN_0066e1a0 (pursuit breakers and smashed props: five at once, then more while
 *    the debris falls - one knock per smash, 2 s quiet after).
 *  - TIMERS: EShowRaceOverMessage FUN_0066d920 (race over - also when a checkpoint timer runs out), EShowTimeExtension
 *    FUN_0066d9f0 (checkpoint time added), EShowRaceCountdown FUN_0066d8c0 (start countdown). Not seen in a race
 *    yet (the test route is free roam).
 *  - GEARS: FUN_007192e0 (a transmission's shift) makes EPlayerShift only for the player's car: its constructor
 *    FUN_0066b130 (thiscall event, a, b, flag, old gear, new gear). Measured: one per shift.
 *  - MENUS: every front-end sound is looked up by its name hash (FUN_004b3ed0: h = -1, h = h*33 + c) through
 *    FUN_00834350 or FUN_00834250 (thiscall bank, hash) and played when found (FUN_0049fb80) - the APT menus queue
 *    theirs through FUN_00838480. A found sound = a tap, except the continuous ones (car rotation, slider movement,
 *    notifications, zoom).
 * NFSU_TRACKPAD_LOG=1 logs every event, NFSU_EVENT_LOG=1 every game event constructor (diagnostic). */
#include <dlfcn.h>

typedef int (*trackpad_fn)(int level, int count, int interval_ms);
typedef int (*trackpad_impact_fn)(float intensity, float length);
static trackpad_fn g_tp_fn;
static trackpad_impact_fn g_tp_impact;
static int g_tp_level = 2; /* 0 off, 1 light, 2 strong (setting, re-read every second) */
static int64_t g_tp_level_ns = -1;
static int64_t g_tp_hit_ns, g_tp_scrape_ns, g_tp_menu_ns, g_tp_break_ns;
static float g_tp_hit_j;
static int g_tp_log;
static float g_tp_cpos[3]; /* the last contact point */
/* the engine's beat pauses until then: a crash knock playing out, a gear change's cut */
static int64_t g_tp_engine_quiet_until;

/* Four switches (the person, 2026-11-22): crashes/smashes/gears on the trackpad (trackpad_haptics) and on a pad
 * (pad_haptics), the engine on the trackpad (engine_vibration) and on a pad (pad_engine). While a pad is connected
 * the trackpad stays quiet (the kit's host_pad_kind says whether one is: 0 none). */
static int g_pad_level = 2, g_pad_present;
static int tp_setting(const PopModApi *api, int64_t now) {
    if (g_tp_level_ns < 0 || now - g_tp_level_ns > 1000000000LL) {
        int64_t v = 2, pv = 2;
        if (api->settings_get) {
            api->settings_get(api, "trackpad_haptics", &v);
            api->settings_get(api, "pad_haptics", &pv);
        }
        g_tp_level = v < 0 ? 0 : v > 2 ? 2 : (int)v;
        g_pad_level = pv < 0 ? 0 : pv > 2 ? 2 : (int)pv;
        static int (*kind)(char *, int);
        static int looked;
        if (!looked) {
            looked = 1;
            kind = (int (*)(char *, int))dlsym(RTLD_DEFAULT, "host_pad_kind");
        }
        g_pad_present = kind ? kind(NULL, 0) != 0 : 0;
        g_tp_level_ns = now;
    }
    return g_tp_level;
}
/* even clicks; level for "strong", the light setting is one step weaker (the trackpad: not while a pad is in) */
static void tp_play(int setting, int level, int clicks, int ms) {
    if (!setting || !g_tp_fn || g_pad_present)
        return;
    if (setting == 1)
        level = level > 1 ? level - 1 : 1;
    g_tp_fn(level, clicks, ms);
}
/* A pad (Xbox / PlayStation through SDL; the kit's rumble_pulse) gets the same knocks: its motors can hold a level,
 * so the knock is an envelope the main-loop pass (tp_pad_pass) lowers smoothly, the heavy motor for the body of the
 * hit and the light one for its sharp start. */
static float g_pad_i, g_pad_dur_s = 1.0f;
static int64_t g_pad_start_ns, g_pad_sent_ns;
static void tp_pad_knock(float intensity, float length) {
    const int64_t now = now_ns();
    const float t = (float)(now - g_pad_start_ns) / 1e9f;
    const float left = g_pad_i * expf(-3.0f * t / g_pad_dur_s);
    if (intensity < left * 0.8f)
        return;
    g_pad_i = intensity;
    g_pad_dur_s = (0.05f + 0.85f * powf(intensity, 1.2f)) * length;
    g_pad_start_ns = now;
    g_pad_sent_ns = 0;
}
static void tp_engine_dump(const PopModApi *api);
static void tp_engine_pass(const PopModApi *api);
static int g_tp_engine_dump;
static void tp_pad_pass(const PopModApi *api) {
    if (g_tp_engine_dump)
        tp_engine_dump(api);
    tp_engine_pass(api);
    if (!api->rumble_pulse || g_pad_i <= 0.0f)
        return;
    const int64_t now = now_ns();
    if (now - g_pad_sent_ns < 30000000LL)
        return;
    g_pad_sent_ns = now;
    const float t = (float)(now - g_pad_start_ns) / 1e9f;
    const float a = g_pad_i * expf(-3.0f * t / g_pad_dur_s);
    if (a < 0.04f) {
        g_pad_i = 0.0f;
        api->rumble_pulse(api, 0, 0, 0);
        return;
    }
    const float sharp = t < 0.08f ? 1.0f : expf(-(t - 0.08f) * 12.0f); /* the light motor: the first instant */
    api->rumble_pulse(api, (uint16_t)(a * 65535.0f), (uint16_t)(a * sharp * 65535.0f), 60);
}
/* a decaying knock (kit: strength sets the start, the clicks' closeness and the length); light = 60% */
static void tp_knock(int setting, float intensity, float length) {
    if (setting && g_tp_impact && !g_pad_present)
        g_tp_impact(setting == 1 ? intensity * 0.6f : intensity, length);
    if (g_pad_level)
        tp_pad_knock(g_pad_level == 1 ? intensity * 0.6f : intensity, length);
    /* the engine's beat stays out of a knock until it has died away, so a crash feels like a crash */
    const int64_t until = now_ns() + (int64_t)((0.05f + 0.85f * powf(intensity, 1.2f)) * length * 1e9f) + 150000000LL;
    if (until > g_tp_engine_quiet_until)
        g_tp_engine_quiet_until = until;
}
/* a short even buzz on the pad for clicks (gears, menus, timers) */
static void tp_pad_tap(const PopModApi *api, int setting, float strength, int ms) {
    (void)setting;
    setting = g_pad_level;
    if (!setting || !api->rumble_pulse || g_pad_i > 0.0f) /* a knock still dying away keeps the motors */
        return;
    if (setting == 1)
        strength *= 0.6f;
    api->rumble_pulse(api, (uint16_t)(strength * 30000.0f), (uint16_t)(strength * 65535.0f), (uint32_t)ms);
}
static int64_t g_tp_start_ns;
static void tp_say(const PopModApi *api, const char *what, unsigned *said) {
    if (g_tp_log || (*said)++ < 10) {
        char l[480];
        snprintf(l, sizeof l, "%s (at %.1f s)", what, (double)(now_ns() - g_tp_start_ns) / 1e9);
        api->log(api, l);
    }
}
static float tp_f32(uint32_t a) {
    const uint32_t u = get_u32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
/* The player's car (2026-11-22, measured): its EngineRacer (g_tp_engine, found at a shift, see tp_shift_enter) has its
 * owner vehicle at +0x2c; the owner's +0xcc points to the body whose +0xac is the car's position in the same
 * (sim) coordinates as the contact points (it moved 3128,7,-2009 -> 1772,13,-897 along a drive). */
static uint32_t g_tp_engine;
static int tp_player_pos(float p[3]) {
    if (!g_tp_engine)
        return 0;
    const uint32_t owner = get_u32(g_tp_engine + 0x2c);
    const uint32_t body = owner ? get_u32(owner + 0xcc) : 0;
    if (!body)
        return 0;
    for (int k = 0; k < 3; ++k)
        p[k] = tp_f32(body + 0xac + 4u * (uint32_t)k);
    return p[0] == p[0] && fabsf(p[0]) < 1e6f && fabsf(p[2]) < 1e6f;
}
/* metres from the player's car to a point, or -1 when the car isn't known yet (before the first shift) */
static float tp_player_dist(const float q[3]) {
    float p[3];
    if (!tp_player_pos(p))
        return -1.0f;
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
/* driving (not a menu's background traffic, a pause or a cutscene stop): the game state 6, no pause, the world's
 * tick moving (the same test as the Extra Options keys, without its guest call - this runs inside a hook) */
static int tp_driving(void) {
    return get_u32(0x00da57b8u) == 6 && get_u8(0x00deb99au) == 0 &&
           now_ns() - g_world_tick_ns < 300000000LL;
}
static void tp_collision(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static unsigned said, scraped;
    const uint32_t info = cpu->esp + 4;
    const float v = tp_f32(info + 0x20), j = tp_f32(info + 0x2c);
    g_tp_cpos[0] = tp_f32(info), g_tp_cpos[1] = tp_f32(info + 4), g_tp_cpos[2] = tp_f32(info + 8);
    if (!(j > 0.0f) || j > 1e8f) /* the impulse decides (the speed field is ~0 in car-to-car crashes) */
        return;
    const int64_t now = now_ns();
    const int setting = tp_setting(api, now);
    const int driving = tp_driving();
    if (!driving)
        return;
    /* only your car's contacts (the person: not when someone else crashes somewhere): the contact point lies on
     * your car's body, within a few metres of its centre (a car is ~4.5 m long); another car's crash 40 m away was
     * seen in a test. Before the first gear change the car isn't known and every contact counts. */
    const float dist = tp_player_dist(g_tp_cpos);
    if (dist > 5.5f)
        return;
    /* The strength follows the impulse (mass x change of speed), not the "closing speed" field: the person rammed
     * cars and walls at speed and the field read 0.0-0.8 m/s while the impulse was 13 000-48 000 N s; with the field
     * in the formula those crashes were almost not felt. ~30 000 N s and more is a full knock (a hit at speed);
     * a few thousand a light one; a road sign or a cone, hundreds, a faint tick. */
    float i = powf(j / 30000.0f, 0.6f);
    if (i > 1.0f)
        i = 1.0f;
    if (i >= 0.12f) {
        /* one crash is many contacts over a few frames: the first sets the knock, a clearly harder one restarts it */
        if (now - g_tp_hit_ns < 100000000LL && j < g_tp_hit_j * 1.3f)
            return;
        g_tp_hit_ns = now;
        g_tp_hit_j = j;
        char l[96];
        snprintf(l, sizeof l, "core.nfsu: trackpad: crash %.1f m/s, impulse %.0f -> %.2f", (double)v, (double)j,
                 (double)i);
        tp_say(api, l, &said);
        tp_knock(setting, i, 1.0f);
    } else if (j >= 400.0f) { /* scraping or a light knock: faint ticks while it goes on */
        if (now - g_tp_scrape_ns < 70000000LL)
            return;
        g_tp_scrape_ns = now;
        if (g_tp_log && scraped++ < 20) {
            char l[96];
            snprintf(l, sizeof l, "core.nfsu: trackpad: scrape %.1f m/s, impulse %.0f", (double)v, (double)j);
            api->log(api, l);
        }
        tp_knock(setting, i < 0.06f ? 0.06f : i, 1.0f);
    }
}

/* The player's engine: FUN_007192e0 is EngineRacer's shift (vtable 0x00c1c86c slot 43, ECX = the object); it makes
 * EPlayerShift only for the player's car, so the object of the shift running when EPlayerShift is made is the
 * player's engine. */
static uint32_t g_tp_shifting;
static void tp_shift_enter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    g_tp_shifting = cpu->ecx;
}
static void tp_engine_dump(const PopModApi *api) {
    static int64_t last;
    const int64_t now = now_ns();
    if (!g_tp_engine || now - last < 500000000LL)
        return;
    last = now;
    char l[3000];
    int n = snprintf(l, sizeof l, "core.nfsu: engine %08x vt %08x:", g_tp_engine, get_u32(g_tp_engine));
    for (uint32_t o = 0x40; o < 0x220 && n < (int)sizeof l - 32; o += 4) {
        const float f = tp_f32(g_tp_engine + o);
        if (f > -1e6f && f < 1e6f && (f > 1e-3f || f < -1e-3f))
            n += snprintf(l + n, sizeof l - n, " %x:%.3g", o, (double)f);
    }
    const uint32_t attr = get_u32(g_tp_engine + 0x4c + 0xe0);
    if (attr) {
        static const uint32_t ks[] = {0xadc, 0xaf0, 0xafc, 0xb10, 0xb28};
        for (size_t k = 0; k < 5; ++k)
            n += snprintf(l + n, sizeof l - n, " attr%x:%.4g", ks[k], (double)tp_f32(attr + ks[k]));
    }
    api->log(api, l);
    api->log(api, l);
}

static void tp_shift(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static unsigned said;
    const uint32_t from = get_u32(cpu->esp + 0x10), to = get_u32(cpu->esp + 0x14);
    if (g_tp_shifting)
        g_tp_engine = g_tp_shifting;
    char l[64];
    snprintf(l, sizeof l, "core.nfsu: trackpad: gear %d -> %d", (int)from, (int)to);
    tp_say(api, l, &said);
    if (from != to) {
        /* a gear change feels like one: the engine's beat cuts out for a moment (the clutch), then a kick - an
         * upshift a firm double thump, a downshift a quick triple blip (the throttle matching the revs); into or
         * out of neutral/reverse just a click */
        const int setting = tp_setting(api, now_ns());
        const int up = (int)to > (int)from && (int)from >= 2, down = (int)to < (int)from && (int)to >= 2;
        if (up) {
            tp_play(setting, 3, 2, 45);
            tp_pad_tap(api, setting, 0.6f, 110);
        } else if (down) {
            tp_play(setting, 2, 3, 28);
            tp_pad_tap(api, setting, 0.45f, 90);
        } else {
            tp_play(setting, 1, 1, 30);
            tp_pad_tap(api, setting, 0.25f, 50);
        }
        const int64_t until = now_ns() + (up ? 220000000LL : 160000000LL);
        if (until > g_tp_engine_quiet_until)
            g_tp_engine_quiet_until = until;
    }
}

enum { TP_BREAK, TP_RACE_OVER, TP_TIME_EXT, TP_COUNTDOWN };
static void tp_event(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    static unsigned said;
    static const char *const names[] = {"broken object", "race over / timer ran out", "checkpoint time added",
                                        "race countdown"};
    const int kind = (int)(uintptr_t)user;
    const int64_t now = now_ns();
    char l[80];
    snprintf(l, sizeof l, "core.nfsu: trackpad: %s", names[kind]);
    tp_say(api, l, &said);
    const int setting = tp_setting(api, now);
    switch (kind) {
    case TP_BREAK: {
        /* only what your car smashed: the explosion's place (FUN_0066e1a0's argument +8/+0xc/+0x10) within 8 m of the
         * car; tried both as sim coordinates and as render ones (x = sim z, y = -sim x, z = sim y) */
        const uint32_t a = get_u32(cpu->esp + 4);
        if (a) {
            const float r[3] = {tp_f32(a + 8), tp_f32(a + 0xc), tp_f32(a + 0x10)};
            const float sim[3] = {-r[1], r[2], r[0]};
            const float d1 = tp_player_dist(r), d2 = tp_player_dist(sim);
            if (g_tp_log) {
                char m[128];
                snprintf(m, sizeof m, "core.nfsu: trackpad: break at %.1f %.1f %.1f, %.1f / %.1f m from the car",
                         (double)r[0], (double)r[1], (double)r[2], (double)d1, (double)d2);
                api->log(api, m);
            }
            if (d1 > 8.0f && d2 > 8.0f)
                return;
        }
    }
        /* one knock per smash: the person felt a pursuit breaker go on far too long - its debris keeps spawning
         * pieces while it falls and swings after the car has gone. So the first piece knocks, the next 2 s are quiet. */
        if (!tp_driving() || now - g_tp_break_ns < 2000000000LL)
            return;
        g_tp_break_ns = now;
        tp_knock(setting, 0.7f, 0.8f);
        break;
    case TP_RACE_OVER: /* the end of a race or its timer: a long knock that dies away */
        tp_knock(setting, 0.85f, 2.0f);
        break;
    case TP_TIME_EXT:
        tp_play(setting, 2, 2, 80);
        tp_pad_tap(api, setting, 0.4f, 160);
        break;
    default:
        tp_play(setting, 2, 1, 30);
        tp_pad_tap(api, setting, 0.4f, 80);
    }
}

/* ENGINE VIBRATION (setting engine_vibration, 0 off / 1 light / 2 strong): the person asked for a "vroom" while
 * the car accelerates, different per car class, class 1 the strongest - on the trackpad and on pads.
 * From the player's EngineRacer (measured on a drive, 2026-11-22): +0x138 the engine's RPM (800 idling, up to the
 * redline before each shift), +0x174 the throttle (0..1), +0x178 the brake; its tuning record [+0x4c+0xe0]: +0xadc
 * the redline (6500 on the test car), +0xafc the top RPM (7500), +0xb10 idle (800).
 * The class: the career's active car (pc_active_car: the game's user-data query) -> its career row ([0x00d8e1a4] +
 * 0x88, 28 bytes, id at +0) -> the car lot's own FUN_0051dd30 (fastcall row: the pvehicle at row+8 -> its frontend
 * tier, field +0x20), the number the car lot shows as the class. Class 1 shakes hardest and fires fastest.
 * The vibration: while on the throttle, a pulse train whose rate follows the RPM (150 ms apart just off idle down to
 * ~40 ms near the redline; a fast "brrr" on the rev limiter) and whose strength grows with the RPM; it drops off the
 * moment you lift. The trackpad gets the pulses as clicks, a pad as a held rumble (heavy motor = the body, the light
 * one rising with the RPM). A crash knock still dying away keeps the pad's motors. */
static uint32_t pc_active_car(const PopModApi *api);
static int g_eng_setting = 2, g_eng_tier = -1, g_eng_pad_setting = 2;
static int64_t g_eng_setting_ns = -1, g_eng_class_ns, g_eng_click_ns, g_eng_pad_ns;
static uint32_t g_eng_car = 0xfffffffeu;
static float g_eng_k = 0.7f;
static int g_eng_pad_on;
static void tp_engine_class(const PopModApi *api) {
    const uint32_t car = pc_active_car(api);
    if (car == g_eng_car)
        return;
    g_eng_car = car;
    g_eng_tier = -1;
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (obj && car != 0xffffffffu && api->guest_call)
        for (uint32_t i = 0; i < 0x14au; ++i) {
            const uint32_t row = obj + 0x88u + 28u * i;
            if (get_u32(row) != car)
                continue;
            uint32_t t = 0;
            if (api->guest_call(api, 0x0051dd30u, row, NULL, 0, &t) == POP_OK)
                g_eng_tier = (int)t;
            break;
        }
    static const float k[5] = {0.6f, 1.0f, 0.8f, 0.6f, 0.4f}; /* unknown, class 1, 2, 3, 4 (for the log) */
    g_eng_k = g_eng_tier >= 1 && g_eng_tier <= 4 ? k[g_eng_tier] : k[0];
    char l[96];
    snprintf(l, sizeof l, "core.nfsu: engine vibration: car %08x, class %d (strength x%.2f)", car, g_eng_tier,
             (double)g_eng_k);
    api->log(api, l);
}
static void tp_engine_quiet(const PopModApi *api) {
    if (g_eng_pad_on && api->rumble_pulse && g_pad_i <= 0.0f)
        api->rumble_pulse(api, 0, 0, 0);
    g_eng_pad_on = 0;
}
static int heli_rb_pos(uint32_t rb, float p[3]);
static int heli_rb_rot(uint32_t rb, float c[3][3]);
static uint32_t heli_rb(uint32_t simable);
/* NFSU_PAD_DUMP=<s> (DIAGNOSTIC): the input devices (manager 0x00da4928: count +4, devices +8; a device's inputs:
 * count = vt+0x14, 16-byte rows at [dev+0x18], name hash at +4) and every input queue's bindings (queues
 * [0x00d3ba74], count [0x00d3ba7c]; port +0x26c, binding list +0x274: nodes {next, prev, 8 bytes, +0x10, action id
 * +0x14, input +0x18, second input +0x1c}) - which pad input each game action reads. */
static void tp_pad_dump(const PopModApi *api, int64_t now) {
    static int done, shots;
    const char *at = getenv("NFSU_PAD_DUMP");
    if (!at)
        return;
    char l[256];
    { /* NFSU_PAD_DUMP="first,t2,t3...": at each later time the pad state the game reads (DIJOYSTATE-like, from
       * [0x00da38e0] vt+4) and the TInputSpec table's pad slots ([0x00da39a4 + 8*action], 12-byte specs: keyboard
       * 0/1, pad 0 primary +0x18, secondary +0x24) */
        int k = 0;
        const char *p = at;
        int want = -1;
        while (*p) {
            const int t = atoi(p);
            if (k == shots + 1)
                want = t;
            ++k;
            p = strchr(p, ',');
            if (!p)
                break;
            ++p;
        }
        if (want >= 0 && now - g_tp_start_ns >= (int64_t)want * 1000000000LL) {
            ++shots;
            const uint32_t joy = get_u32(0x00da38e0u);
            uint32_t st = 0;
            if (joy)
                api->guest_call(api, get_u32(get_u32(joy) + 4), joy, NULL, 0, &st);
            if (st) {
                int m = snprintf(l, sizeof l, "core.nfsu: pad state at %d s: xyz %d %d %d btn", want, (int)get_u32(st),
                                 (int)get_u32(st + 4), (int)get_u32(st + 8));
                for (int b = 0; b < 14; ++b)
                    m += snprintf(l + m, sizeof l - m, " %d", (int)get_u32(st + 0xc + 4 * b));
                snprintf(l + m, sizeof l - m, " r %d %d %d sl %d %d pov %d", (int)get_u32(st + 0x20c),
                         (int)get_u32(st + 0x210), (int)get_u32(st + 0x214), (int)get_u32(st + 0x218),
                         (int)get_u32(st + 0x21c), (int)get_u32(st + 0x220));
                api->log(api, l);
            }
            if (shots == 1)
                for (uint32_t a = 0; a < 25; ++a) {
                    const uint32_t sp = get_u32(0x00da39a4u + 8 * a);
                    snprintf(l, sizeof l, "core.nfsu: pad spec %u: kb %u/%u %u/%u pad %u/%u/%u %u/%u/%u", a,
                             get_u32(sp), get_u32(sp + 4), get_u32(sp + 12), get_u32(sp + 16), get_u32(sp + 0x18),
                             get_u32(sp + 0x1c), get_u32(sp + 0x20), get_u32(sp + 0x24), get_u32(sp + 0x28),
                             get_u32(sp + 0x2c));
                    api->log(api, l);
                }
        }
    }
    if (done || now - g_tp_start_ns < (int64_t)atoi(at) * 1000000000LL)
        return;
    done = 1;
    const uint32_t mgr = 0x00da4928u, n = get_u32(mgr + 4);
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        const uint32_t dev = get_u32(mgr + 8 + 4 * i);
        if (!dev)
            continue;
        uint32_t cnt = 0;
        api->guest_call(api, get_u32(get_u32(dev) + 0x14), dev, NULL, 0, &cnt);
        snprintf(l, sizeof l, "core.nfsu: pad dump: device %u %08x vt %08x inputs %u at %08x", i, dev, get_u32(dev), cnt,
                 get_u32(dev + 0x18));
        api->log(api, l);
        for (uint32_t k = 0; k < cnt && k < 200; ++k) {
            const uint32_t r = get_u32(dev + 0x18) + 16 * k;
            snprintf(l, sizeof l, "core.nfsu: pad dump:   input %u: %08x %08x %08x %08x", k, get_u32(r), get_u32(r + 4),
                     get_u32(r + 8), get_u32(r + 12));
            api->log(api, l);
        }
    }
    const uint32_t qs = get_u32(0x00d3ba74u), qn = get_u32(0x00d3ba7cu);
    for (uint32_t i = 0; i < qn && i < 16; ++i) {
        const uint32_t q = get_u32(qs + 4 * i);
        const uint32_t b = get_u32(q + 0x274);
        snprintf(l, sizeof l, "core.nfsu: pad dump: queue %u %08x port %d bindings %08x", i, q, (int)get_u32(q + 0x26c), b);
        api->log(api, l);
        if (!b)
            continue;
        uint32_t node = get_u32(b), guard = 0;
        while (node && node != b && guard++ < 300) {
            snprintf(l, sizeof l, "core.nfsu: pad dump:   action %u input %d / %d  (%08x %08x %08x)", get_u32(node + 0x14),
                     (int)get_u32(node + 0x18), (int)get_u32(node + 0x1c), get_u32(node + 8), get_u32(node + 12),
                     get_u32(node + 0x10));
            api->log(api, l);
            node = get_u32(node);
        }
    }
}
static void tp_engine_pass(const PopModApi *api) {
    const int64_t now = now_ns();
    tp_pad_dump(api, now);
    if (getenv("NFSU_PROBE_INPUT") && g_tp_engine) { /* what the controls do: throttle, brake, heading, speed */
        static int64_t at;
        static float lp[3], lh;
        if (now - at > 250000000LL) {
            const float dt = at ? (float)(now - at) / 1e9f : 0.25f;
            at = now;
            const uint32_t rb = heli_rb(get_u32(g_tp_engine + 0x2c));
            float p[3] = {0, 0, 0}, r[3][3];
            if (rb && heli_rb_pos(rb, p) && heli_rb_rot(rb, r)) {
                const float h = atan2f(r[2][0], r[2][2]);
                const float dx = p[0] - lp[0], dz = p[2] - lp[2];
                char l[200];
                snprintf(l, sizeof l, "core.nfsu: probe t=%.2f thr %.2f brake %.2f rpm %.0f speed %.1f turn %.2f f17c %.2f f15c %.2f",
                         (double)(now - g_tp_start_ns) / 1e9, (double)tp_f32(g_tp_engine + 0x174),
                         (double)tp_f32(g_tp_engine + 0x178), (double)tp_f32(g_tp_engine + 0x138),
                         (double)(sqrtf(dx * dx + dz * dz) / dt), (double)((h - lh) / dt),
                         (double)tp_f32(g_tp_engine + 0x17c), (double)tp_f32(g_tp_engine + 0x15c));
                api->log(api, l);
                lh = h;
                memcpy(lp, p, sizeof lp);
            }
        }
    }
    if (g_eng_setting_ns < 0 || now - g_eng_setting_ns > 1000000000LL) {
        int64_t v = 2, pv = 2;
        if (api->settings_get) {
            api->settings_get(api, "engine_vibration", &v);
            api->settings_get(api, "pad_engine", &pv);
        }
        g_eng_setting = v < 0 ? 0 : v > 2 ? 2 : (int)v;
        g_eng_pad_setting = pv < 0 ? 0 : pv > 2 ? 2 : (int)pv;
        tp_setting(api, now); /* refreshes whether a pad is connected */
        g_eng_setting_ns = now;
    }
    const int pad_on = g_eng_pad_setting != 0, pad_on_tp = g_eng_setting != 0 && !g_pad_present;
    if ((!pad_on && !pad_on_tp) || !g_tp_engine || !tp_driving()) {
        tp_engine_quiet(api);
        return;
    }
    if (now - g_eng_class_ns > 2000000000LL) {
        g_eng_class_ns = now;
        tp_engine_class(api);
    }
    const float rpm = tp_f32(g_tp_engine + 0x138), thr = tp_f32(g_tp_engine + 0x174);
    const uint32_t tune = get_u32(g_tp_engine + 0x4c + 0xe0);
    float red = tune ? tp_f32(tune + 0xadc) : 0.0f, idle = tune ? tp_f32(tune + 0xb10) : 0.0f;
    if (!(red > 2000.0f && red < 25000.0f))
        red = 7000.0f;
    if (!(idle > 200.0f && idle < 3000.0f))
        idle = 900.0f;
    if (!(rpm == rpm) || !(thr > 0.2f) || thr > 1.5f) { /* off the throttle: quiet at once */
        tp_engine_quiet(api);
        return;
    }
    float x = (rpm - idle) / (red - idle);
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    /* pulling (the revs climbing) vs cruising: the RPM's climb per second, smoothed; a hard pull is up to twice
     * as strong as holding the revs, so setting off and each gear's pull feel different from cruising */
    static float last_rpm, climb;
    static int64_t last_ns;
    if (last_ns && now > last_ns) {
        const float dt = (float)(now - last_ns) / 1e9f;
        const float c = (rpm - last_rpm) / (dt > 0.001f ? dt : 0.001f);
        climb += (c - climb) * (dt * 6.0f < 1.0f ? dt * 6.0f : 1.0f);
    }
    last_rpm = rpm, last_ns = now;
    float pull = climb / 2500.0f; /* rpm per second: ~2500 = a strong pull */
    pull = pull < 0.0f ? 0.0f : pull > 1.0f ? 1.0f : pull;
    if (now < g_tp_engine_quiet_until) { /* a knock or a gear change has the floor */
        if (g_eng_pad_on && api->rumble_pulse && g_pad_i <= 0.0f)
            api->rumble_pulse(api, 0, 0, 0), g_eng_pad_on = 0;
        return;
    }
    /* Each class its own engine (the person: "class 4 must be the vaguest - weak pulses", class 1 the strongest):
     * class 4 = only weak clicks, slow and uneven (a soft, vague hum); class 3 = weak to medium, a little steadier;
     * class 2 = medium, firm and regular; class 1 = medium to strong, fast, and a double hit on every beat high up. */
    struct EngineFeel {
        float gap_lo, gap_hi; /* ms between beats just off idle / at the redline */
        int lvl_lo, lvl_hi;   /* click strength low / high in the revs */
        float jitter;         /* how uneven the beat is (share of the gap) */
        float skip;           /* share of beats left out (vagueness) */
        float pad;            /* pad strength */
    };
    static const struct EngineFeel feel[5] = {
        {150, 55, 1, 2, 0.15f, 0.10f, 0.60f}, /* class unknown */
        {105, 30, 2, 3, 0.03f, 0.00f, 1.00f}, /* class 1 */
        {125, 42, 2, 3, 0.06f, 0.00f, 0.80f}, /* class 2 */
        {150, 60, 1, 2, 0.15f, 0.12f, 0.60f}, /* class 3 */
        {175, 85, 1, 1, 0.35f, 0.30f, 0.40f}, /* class 4 */
    };
    const struct EngineFeel *f = &feel[g_eng_tier >= 1 && g_eng_tier <= 4 ? g_eng_tier : 0];
    const int limiter = rpm >= red * 0.97f;
    const float tf = thr > 1.0f ? 1.0f : thr;
    float amp = f->pad * tf * (0.25f + 0.75f * powf(x, 1.3f)) * (0.55f + 0.45f * pull);
    if (g_eng_setting == 1)
        amp *= 0.55f;
    static uint32_t rnd = 0x9e3779b9u;
    rnd = rnd * 1664525u + 1013904223u;
    const float r01 = (float)(rnd >> 8) / 16777216.0f;
    float gap = f->gap_lo + (f->gap_hi - f->gap_lo) * x;
    gap *= 1.0f + f->jitter * (r01 * 2.0f - 1.0f);
    if (limiter)
        gap = g_eng_tier == 4 ? 45.0f : 24.0f;
    if (g_tp_fn && pad_on_tp && now - g_eng_click_ns >= (int64_t)(gap * 1e6f)) {
        g_eng_click_ns = now;
        rnd = rnd * 1664525u + 1013904223u;
        const int skip = (float)(rnd >> 8) / 16777216.0f < f->skip * (1.0f - x * 0.5f);
        /* stronger with the revs and the pull; the light setting one step down */
        int lvl = f->lvl_lo + (int)((float)(f->lvl_hi - f->lvl_lo + 1) * (0.6f * x + 0.4f * pull));
        lvl = lvl > f->lvl_hi ? f->lvl_hi : lvl;
        if (limiter && g_eng_tier != 4)
            lvl = f->lvl_hi;
        if (g_eng_setting == 1 && lvl > 1)
            --lvl;
        if (!skip) {
            if (g_eng_tier == 1 && x > 0.6f && !limiter)
                g_tp_fn(lvl, 2, 12); /* class 1 high up: a double hit per beat */
            else
                g_tp_fn(lvl, 1, 10);
        }
    }
    /* pads: a held rumble refreshed every 30 ms (not over a crash knock) */
    if (api->rumble_pulse && pad_on && g_pad_i <= 0.0f && now - g_eng_pad_ns >= 30000000LL) {
        g_eng_pad_ns = now;
        if (g_eng_pad_setting == 1 && g_eng_setting != 1)
            amp *= 0.55f;
        else if (g_eng_pad_setting == 2 && g_eng_setting == 1)
            amp /= 0.55f;
        const float lo = amp * 0.8f, hi = limiter ? amp : amp * x * (g_eng_tier == 4 ? 0.2f : 0.6f);
        api->rumble_pulse(api, (uint16_t)(lo * 65535.0f), (uint16_t)(hi * 65535.0f), 70);
        g_eng_pad_on = 1;
    }
}

/* menu sounds: the lookup's hash (BEFORE), a found sound (AFTER: EAX != 0) */
static uint32_t g_tp_sound_hash;
static uint32_t g_tp_quiet[8];
static uint32_t tp_name_hash(const char *s) {
    uint32_t h = 0xffffffffu;
    for (; *s; ++s)
        h = h * 33u + (uint8_t)*s;
    return h;
}
static void tp_sound_ask(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    g_tp_sound_hash = get_u32(cpu->esp + 4);
}
static void tp_sound_found(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static unsigned said;
    const uint32_t h = g_tp_sound_hash;
    g_tp_sound_hash = 0;
    if (!cpu->eax || !h)
        return;
    for (size_t i = 0; i < sizeof g_tp_quiet / sizeof g_tp_quiet[0]; ++i)
        if (g_tp_quiet[i] == h)
            return;
    const int64_t now = now_ns();
    if (now - g_tp_menu_ns < 35000000LL)
        return;
    g_tp_menu_ns = now;
    char l[64];
    snprintf(l, sizeof l, "core.nfsu: trackpad: menu sound %08x", h);
    tp_say(api, l, &said);
    const int setting = tp_setting(api, now);
    tp_play(setting, 2, 1, 30);
    tp_pad_tap(api, setting, 0.25f, 35);
}

/* NFSU_EVENT_LOG=1 (DIAGNOSTIC): every game event constructor (events_table.inc, from tools/dev/nfsu_event_ctors.py)
 * logs its first 6 uses with the time - what fires on a crash, a broken prop, a timer. */
struct TpEvent {
    uint32_t fn;
    const char *name;
};
static const struct TpEvent k_tp_events[] = {
#include "events_table.inc"
};
static unsigned g_tp_event_hits[sizeof k_tp_events / sizeof k_tp_events[0]];
static void tp_event_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv;
    const size_t i = (size_t)(uintptr_t)user;
    /* NOS and Speedbreaker are logged every time (which pad button does what) */
    const int always = strstr(k_tp_events[i].name, "NOS") || strstr(k_tp_events[i].name, "SpeedBreaker");
    if (g_tp_event_hits[i]++ < 6 || (always && g_tp_event_hits[i] < 400)) {
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: event %s (%08x) #%u at %.2f s (run %.2f s)", k_tp_events[i].name,
                 k_tp_events[i].fn, g_tp_event_hits[i], (double)now_ns() / 1e9,
                 (double)(now_ns() - g_tp_start_ns) / 1e9);
        api->log(api, l);
    }
}

static void trackpad_install(const PopModApi *api) {
    g_tp_log = getenv("NFSU_TRACKPAD_LOG") != NULL;
    g_tp_start_ns = now_ns();
    g_tp_engine_dump = getenv("NFSU_ENGINE_DUMP") != NULL;
    {
        static uint32_t sh;
        if (api->hook_install(api, 0x007192e0u, tp_shift_enter, POP_HOOK_BEFORE, NULL, &sh) != POP_OK)
            api->log(api, "core.nfsu: engine shift hook NOT installed");
    }
    if (getenv("NFSU_EVENT_LOG")) {
        static uint32_t eh[sizeof k_tp_events / sizeof k_tp_events[0]];
        for (size_t i = 0; i < sizeof k_tp_events / sizeof k_tp_events[0]; ++i)
            api->hook_install(api, k_tp_events[i].fn, tp_event_log, POP_HOOK_BEFORE, (void *)(uintptr_t)i, &eh[i]);
    }
    g_tp_fn = (trackpad_fn)dlsym(RTLD_DEFAULT, "host_trackpad_haptic");
    g_tp_impact = (trackpad_impact_fn)dlsym(RTLD_DEFAULT, "host_trackpad_impact");
    if (!g_tp_fn || !g_tp_impact) {
        g_tp_fn = NULL, g_tp_impact = NULL;
        api->log(api, "core.nfsu: trackpad taps not available in this host (pads still rumble)");
    }
    static const char *const quiet[] = {"SOUND_ROTATE_MOVING", "SOUND_ROTATE_START", "SOUND_ROTATE_STOP",
                                        "SOUND_SLIDER_MOVING", "SOUND_CHYRON_NOTIFICATION", "SOUND_CHYRON_OUT",
                                        "SOUND_ZOOM_IN", "SOUND_ZOOM_OUT"};
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; ++i)
        g_tp_quiet[i] = tp_name_hash(quiet[i]);
    static uint32_t h[10];
    int bad = 0;
    bad |= api->hook_install(api, 0x00667d60u, tp_collision, POP_HOOK_BEFORE, NULL, &h[0]) != POP_OK;
    bad |= api->hook_install(api, 0x0066b130u, tp_shift, POP_HOOK_BEFORE, NULL, &h[1]) != POP_OK;
    bad |= api->hook_install(api, 0x0066e1a0u, tp_event, POP_HOOK_BEFORE, (void *)(uintptr_t)TP_BREAK, &h[2]) != POP_OK;
    bad |= api->hook_install(api, 0x0066d920u, tp_event, POP_HOOK_BEFORE, (void *)(uintptr_t)TP_RACE_OVER, &h[3]) !=
           POP_OK;
    bad |= api->hook_install(api, 0x0066d9f0u, tp_event, POP_HOOK_BEFORE, (void *)(uintptr_t)TP_TIME_EXT, &h[4]) !=
           POP_OK;
    bad |= api->hook_install(api, 0x0066d8c0u, tp_event, POP_HOOK_BEFORE, (void *)(uintptr_t)TP_COUNTDOWN, &h[5]) !=
           POP_OK;
    bad |= api->hook_install(api, 0x00834350u, tp_sound_ask, POP_HOOK_BEFORE, NULL, &h[6]) != POP_OK;
    bad |= api->hook_install(api, 0x00834350u, tp_sound_found, POP_HOOK_AFTER, NULL, &h[7]) != POP_OK;
    bad |= api->hook_install(api, 0x00834250u, tp_sound_ask, POP_HOOK_BEFORE, NULL, &h[8]) != POP_OK;
    bad |= api->hook_install(api, 0x00834250u, tp_sound_found, POP_HOOK_AFTER, NULL, &h[9]) != POP_OK;
    api->log(api, bad ? "core.nfsu: trackpad taps: a hook was NOT installed" : "core.nfsu: trackpad taps hooked");
}
