/* heli.inc - included by nfsu.c. A police helicopter you fly yourself (the person, 2026-11-22: in police mode,
 * take off from where the car is, simple controls without aerodynamics, the chase keeps going, arrest a racer by
 * hovering right over them, leave by changing car).
 *
 * The game's own helicopter (static evidence, 2026-11-22):
 *  - model CARS/COP_HEL_FED_01, vehicle "cop_heli" (string 0x00bde680), AI AIVehicleHelicopter, camera
 *    ECameraHeliCopCam;
 *  - the cop manager [0x00de6dac] (ICopMgr, AICopManager's interface at +0x40, vtable 0x00bdb9dc) slot 8 (+0x20) =
 *    FUN_0043e1e0 spawns a cop: thiscall (type key, flags, &position, &direction, &vector, int, bool, float) - the
 *    scripted ESpawnCop event (handler FUN_0067bd60) calls it with flags 0x400 for "cop_heli" (0x10 / 0x40 for cars);
 *    the key is FUN_00463a80(name) (cdecl);
 *  - every AI vehicle is in the list [0x00de956c] (count [0x00de9574]); FUN_0061ccc0 finds the helicopter there as
 *    the one whose vtable +0x60 result points at [0x00de6ef8].
 * Step 1 (this file now): J while driving (police role, or NFSU_HELI_ANY=1 for tests) asks for a helicopter over the
 * car and logs where it appears. NFSU_HELI_LOG=1 logs the list. */

#define HELI_COP_MGR 0x00de6dacu
#define HELI_NAME 0x00bde680u /* "cop_heli" */
#define HELI_LIST 0x00de956cu
#define HELI_COUNT 0x00de9574u
#define HELI_KEY_CELL 0x00de6ef8u

static uint32_t g_heli_buf; /* guest scratch: position, direction (12 bytes each) */


/* A PVehicle's RigidBodyPart from its ISimable interface (+0x24 of PVehicle; the player's EngineRacer+0x2c is one):
 * [ISimable + 0x48], vtable 0x00c1d1a4. Its Havok body [rb + 0x24]: rotation columns +0xe0/+0xf0/+0x100 (x,y,z,w
 * each), position +0x110, linear velocity +0x1a0. Slots: 22 SetPosition(&v3), 23 SetLinearVelocity(&v3), 24
 * SetAngularVelocity(&v3), 28 SetOrientation(&quat x,y,z,w). */
static uint32_t heli_rb(uint32_t simable) {
    const uint32_t rb = simable >= 0x01400000u ? get_u32(simable + 0x48u) : 0;
    return rb >= 0x01400000u && get_u32(rb) == 0x00c1d1a4u ? rb : 0;
}
static int heli_rb_pos(uint32_t rb, float p[3]) {
    const uint32_t hk = get_u32(rb + 0x24u);
    if (hk < 0x01400000u)
        return 0;
    for (int k = 0; k < 3; ++k)
        p[k] = tp_f32(hk + 0x110u + 4u * (uint32_t)k);
    return 1;
}
static int heli_rb_rot(uint32_t rb, float c[3][3]) {
    const uint32_t hk = get_u32(rb + 0x24u);
    if (hk < 0x01400000u)
        return 0;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            c[i][k] = tp_f32(hk + 0xe0u + 16u * (uint32_t)i + 4u * (uint32_t)k);
    return 1;
}
/* calls a vector setter slot of the body with up to 4 floats (a scratch block per slot in g_heli_buf) */
static void heli_rb_set(const PopModApi *api, uint32_t rb, int slot, const float *v) {
    if (!g_heli_buf && (!api->guest_alloc || api->guest_alloc(api, 256, &g_heli_buf) != POP_OK || !g_heli_buf))
        return;
    const uint32_t at = g_heli_buf + 64u + 16u * (uint32_t)(slot & 7);
    for (int k = 0; k < (slot == 28 ? 4 : 3); ++k) {
        uint32_t u;
        memcpy(&u, &v[k], 4);
        api->guest_write_u32(api, at + 4u * (uint32_t)k, u);
    }
    uint32_t r;
    const uint32_t a[1] = {at};
    api->guest_call(api, get_u32(get_u32(rb) + 4u * (uint32_t)slot), rb, a, 1, &r);
}

/* ---- THE CAMERA ----
 * DrivingCameraMover's update FUN_004e0e30 ends with FUN_00749920(out, &eye, &target, &up) (a look-at) and
 * FUN_004dcb70 (Camera::SetCameraMatrix). A BEFORE hook on the look-at, when called from that update, logs (and
 * while flying replaces) eye and target. */
static void heli_lookat(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user);
/* ---- FLIGHT (2026-11-22) ----
 * The helicopter is the player's own car with the helicopter's model (pvehicle MODEL = cop_hel_fed_01's; checked:
 * drawn on the road with its rotor turning, the chase camera behind it, the car's physics, pursuit and HUD as for
 * any car). Flying = the mod sets the car's Havok body each main-loop pass: a linear velocity from the keys (so the
 * world's collisions still stop it), a yaw/pitch/roll orientation and no spin. No aerodynamics:
 *   Up / Down arrows: forward / back (up to 45 m/s), Left / Right: turn, Space or E: climb, Q or Ctrl: descend.
 * Take-off: Space/E on the ground. Landing: descend until the ground stops the body; the car's own physics then
 * holds it (it can roll on the road like a car). The player's PVehicle: the players' list [0x00de93d4] (count
 * [0x00de93dc]) holds its IVehicle interface (PVehicle + 0xb8); ISimable is PVehicle + 0x24. */
static int g_fly_on;            /* the player's car is the helicopter (set by the model swap) */
static int g_fly_flying;
static float g_fly_h, g_fly_s, g_fly_vs, g_fly_roll, g_cam_h, g_fly_turn, g_fly_ground, g_fly_last_p[3];
static int64_t g_fly_stuck_ns;
static int g_fly_blocked; /* held by something while moving: climb over it */
static float g_fly_hold_y;
/* the black below the sky when high up -> a haze (kit d3d9 void colour) */
static void heli_void(int on) {
    static void (*fn)(uint32_t, int);
    static int looked, last = -1;
    if (!looked) {
        looked = 1;
        fn = (void (*)(uint32_t, int))dlsym(RTLD_DEFAULT, "recomp_d3d9_set_void_colour");
    }
    if (fn && on != last)
        fn(0xffa58776u, on);
    last = on;
}
static int64_t g_fly_last_ns, g_fly_blocked_ns;
static float g_fly_last_y;
static volatile uint8_t g_fly_keys[8]; /* up, down, left, right, climb, descend */
static int32_t heli_fly_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)api, (void)dik, (void)user;
    int k = -1;
    switch (vk) {
    case 0x26: case 0x57: k = 0; break;              /* Up, W */
    case 0x28: case 0x53: k = 1; break;              /* Down, S */
    case 0x25: case 0x41: k = 2; break;              /* Left, A */
    case 0x27: case 0x44: k = 3; break;              /* Right, D */
    case 0x20: case 0x45: k = 4; break;              /* Space, E */
    case 0x51: case 0x11: case 0xa2: k = 5; break;   /* Q, Ctrl */
    default: return 0;
    }
    g_fly_keys[k] = down ? 1 : 0;
    return 0;
}
static uint32_t heli_player_simable(void) {
    if (!get_u32(0x00de93dcu))
        return 0;
    const uint32_t v = get_u32(get_u32(0x00de93d4u));
    return v >= 0x01400000u && get_u32(v) == 0x00c19594u ? v - 0xb8u + 0x24u : 0;
}
static void quat_mul(float o[4], const float a[4], const float b[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* ---- ARREST BY HOVERING (the person: "I can still arrest the racer if I simply hang right over them") ----
 * The Police Career plugin's pursuit controller is [0x101d69a0] + 0x13e4 (state dword: 2 dispatch, 3 pursuit); its
 * racer = FUN_1014fe40 (fastcall controller + 0x28), the racer's PVehicle IVehicle interface (+0xb8). The plugin
 * arrests in its update (FUN_1013b560) when FUN_10150560 sees the racer's native pursuit busted: racer vt+0xdc ->
 * its AI, AI vt+0xc0 -> the pursuit, pursuit [+0xcc]+0x1c = the racer's handle and [pursuit + 0x2a8] == 3. So: the
 * helicopter within 8 m across and 2-35 m above the racer for 2.5 s -> that field is set to 3 and the plugin does the
 * rest (its "Racer arrested!", radio, removal), exactly as for a bust by a car. */
static void heli_arrest_pass(const PopModApi *api, const float hp[3], float dt) {
    static float held;
    const uint32_t pr = get_u32(0x101d69a0u);
    if (!pr || !api->guest_call) {
        held = 0;
        return;
    }
    const uint32_t ctrl = pr + 0x13e4u, state = get_u32(ctrl);
    if (state != 2 && state != 3) {
        held = 0;
        return;
    }
    uint32_t racer = 0;
    if (api->guest_call(api, 0x1014fe40u, ctrl + 0x28u, NULL, 0, &racer) != POP_OK || racer < 0x01400000u ||
        get_u32(racer) != 0x00c19594u) {
        held = 0;
        return;
    }
    float rp[3];
    const uint32_t rrb = heli_rb(racer - 0xb8u + 0x24u);
    if (!rrb || !heli_rb_pos(rrb, rp)) {
        held = 0;
        return;
    }
    const float dx = hp[0] - rp[0], dz = hp[2] - rp[2], up = hp[1] - rp[1];
    const int over = dx * dx + dz * dz < 64.0f && up > 2.0f && up < 35.0f;
    held = over ? held + dt : fmaxf(0.0f, held - 2.0f * dt);
    static int said;
    if (over && held > 0.3f && !said) {
        said = 1;
        eo_message(api, "Hold over the racer to arrest");
    }
    if (!over)
        said = 0;
    if (held < 2.5f)
        return;
    held = 0;
    uint32_t ai = 0, pursuit = 0;
    const uint32_t vt = get_u32(racer);
    if (api->guest_call(api, get_u32(vt + 0xdc), racer, NULL, 0, &ai) != POP_OK || ai < 0x01400000u)
        return;
    if (api->guest_call(api, get_u32(get_u32(ai) + 0xc0), ai, NULL, 0, &pursuit) != POP_OK || pursuit < 0x01400000u)
        return;
    api->guest_write_u32(api, pursuit + 0x2a8u, 3);
    char l[120];
    snprintf(l, sizeof l, "core.nfsu: heli: racer %08x arrested from the air (pursuit %08x)", racer, pursuit);
    api->log(api, l);
}

/* The pad for flying (the person: "I can't fly the helicopter with the pad"): the merged pad as the host has it
 * (kit recomp_pad_raw, before the driving remap): left stick forward/back and turn, right trigger climb, left
 * trigger descend. Sticks have a 20% dead zone; values come back -1..1 / 0..1. */
struct HeliPad {
    uint16_t buttons;
    uint8_t hat, reserved;
    int16_t lx, ly, rx, ry; /* -32767..32767, +y down */
    uint8_t l2, r2;
};
static int heli_pad(float *fwd, float *turn, float *climb, float *descend) {
    static uint32_t (*raw)(struct HeliPad *);
    static int looked;
    if (!looked) {
        looked = 1;
        raw = (uint32_t(*)(struct HeliPad *))dlsym(RTLD_DEFAULT, "recomp_pad_raw");
    }
    *fwd = *turn = *climb = *descend = 0.0f;
    if (!raw)
        return 0;
    struct HeliPad p;
    memset(&p, 0, sizeof p);
    raw(&p);
    const float dz = 0.2f;
    const float lx = (float)p.lx / 32767.0f, ly = -(float)p.ly / 32767.0f;
    *turn = fabsf(lx) > dz ? (lx - copysignf(dz, lx)) / (1.0f - dz) : 0.0f;
    *fwd = fabsf(ly) > dz ? (ly - copysignf(dz, ly)) / (1.0f - dz) : 0.0f;
    *climb = p.r2 > 30 ? (float)p.r2 / 255.0f : 0.0f;
    *descend = p.l2 > 30 ? (float)p.l2 / 255.0f : 0.0f;
    return 1;
}

/* No invisible walls for the helicopter (the person: it must not touch them at all). Every Havok body has a
 * collision filter info (hkpCollidable at entity + 0x10, the info at collidable + 0x1c); the game's own filter
 * (hkNFSCollidableManager, decision FUN_006d7bd0, jump tables at 0x006d7d28..: low 6 bits a layer, the top byte
 * flags) always lets a car (layer 3) hit the world (layer 6). For a layer-1 body the top byte is the bounds'
 * kFlag_NoCollision_* bits (0x20 ground, 0x40 barrier, 0x80 object): against world shapes with a sub-type in bits
 * 6..9 it collides unless 0x40, against sub-type 0 (ground, buildings) unless 0x20; against cars unless 0x80, props
 * and layers 2/4/7 always. Measured (NFSU_HELI_FILTERLOG): world shapes come as sub-type 0 and sub-type 1 - the
 * barriers. So the helicopter's body becomes layer 1 with 0x40: no invisible walls, everything else as before. */
static void heli_no_barriers(const PopModApi *api, uint32_t rb) {
    const uint32_t hk = get_u32(rb + 0x24);
    if (hk < 0x01400000u)
        return;
    const uint32_t info = get_u32(hk + 0x2c);
    static uint32_t said;
    if (said != info) {
        said = info;
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: heli: collision filter info %08x (layer %u)%s", info, info & 0x3fu,
                 (info & 0x3fu) == 1 && (info & 0x40000000u) ? ", walls off" : "");
        api->log(api, l);
    }
    if ((info & 0x3fu) == 3 && !getenv("NFSU_HELI_WALLS"))
        api->guest_write_u32(api, hk + 0x2c, (info & 0x00ffffc0u) | 1u | 0x40000000u);
}

static void heli_fly_pass(const PopModApi *api) {
    const int64_t now = now_ns();
    float dt = g_fly_last_ns ? (float)(now - g_fly_last_ns) / 1e9f : 0.0f;
    g_fly_last_ns = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (!g_fly_on) {
        if (g_fly_flying)
            heli_void(0);
        g_fly_flying = 0;
        return;
    }
    /* a menu, a pause or the world waiting for streaming: hold the controls, keep flying (measured: flying fast
     * the world's tick stops for a moment while it streams; ending the flight then dropped the helicopter) */
    if (!tp_driving())
        return;
    const uint32_t rb = heli_rb(heli_player_simable());
    float p[3], rot[3][3];
    if (!rb || !heli_rb_pos(rb, p) || !heli_rb_rot(rb, rot))
        return;
    heli_no_barriers(api, rb);
    float pad_fwd, pad_turn, pad_up, pad_down;
    heli_pad(&pad_fwd, &pad_turn, &pad_up, &pad_down);
    if (!g_fly_flying) {
        if (!g_fly_keys[4] && pad_up < 0.5f)
            return;
        g_fly_flying = 1; /* take off: keep the heading the car has (its forward = rotation column 2) */
        g_fly_h = atan2f(rot[2][0], rot[2][2]);
        g_cam_h = g_fly_h;
        g_fly_s = 0, g_fly_vs = 0, g_fly_roll = 0, g_fly_turn = 0;
        g_fly_ground = p[1];
        g_fly_hold_y = 0;
        memcpy(g_fly_last_p, p, sizeof g_fly_last_p);
        g_fly_stuck_ns = 0;
        g_fly_blocked_ns = 0;
        g_fly_blocked = 0;
        g_fly_last_y = p[1];
        api->log(api, "core.nfsu: heli: take-off");
    }
    /* Easy, level flying (the person: "like Spore's planet flight"): the helicopter stays level and holds its
     * height by itself, speeds and turns ease in and out, the nose dips a little going forward. */
    float want_s = g_fly_keys[0] ? 45.0f : g_fly_keys[1] ? -15.0f : 0.0f;
    if (want_s == 0.0f && pad_fwd != 0.0f)
        want_s = pad_fwd > 0 ? 45.0f * pad_fwd : 15.0f * pad_fwd;
    float want_vs = g_fly_keys[4] ? 10.0f : g_fly_keys[5] ? -10.0f : 0.0f;
    if (want_vs == 0.0f && (pad_up > 0.0f || pad_down > 0.0f))
        want_vs = 10.0f * (pad_up - pad_down);
    /* a ceiling: 160 m over the place it took off from, softly (the world isn't made to be seen from higher) */
    const float ceiling = g_fly_ground + 160.0f;
    if (p[1] > ceiling - 20.0f && want_vs > 0)
        want_vs *= fmaxf(0.0f, (ceiling - p[1]) / 20.0f);
    if (p[1] > ceiling)
        want_vs = -4.0f;
    /* right turns the heading up (the person: left and right were swapped - the old sign turned left on Right) */
    float turn_in = (g_fly_keys[3] ? 1.0f : 0.0f) - (g_fly_keys[2] ? 1.0f : 0.0f);
    if (turn_in == 0.0f)
        turn_in = pad_turn;
    const float turn_want = turn_in * 1.25f;
    const float ease = fminf(1.0f, 2.2f * dt), vease = fminf(1.0f, 4.0f * dt);
    g_fly_s += (want_s - g_fly_s) * ease;
    g_fly_vs += (want_vs - g_fly_vs) * vease;
    g_fly_turn += (turn_want - g_fly_turn) * fminf(1.0f, 5.0f * dt);
    g_fly_h += g_fly_turn * dt;
    g_fly_roll += (-g_fly_turn * 0.22f * fminf(1.0f, fabsf(g_fly_s) / 20.0f) - g_fly_roll) * fminf(1.0f, 4.0f * dt);
    /* landing: descending but the ground holds the body for 0.4 s -> the car's physics again */
    const float measured_vy = dt > 0 ? (p[1] - g_fly_last_y) / dt : 0;
    g_fly_last_y = p[1];
    if (want_vs < 0 && measured_vy > -0.5f && fabsf(g_fly_s) < 3.0f && p[1] < g_fly_ground + 30.0f) {
        if (!g_fly_blocked_ns)
            g_fly_blocked_ns = now;
        else if (now - g_fly_blocked_ns > 400000000LL) {
            g_fly_flying = 0;
            heli_void(0);
            api->log(api, "core.nfsu: heli: landed");
            return;
        }
    } else
        g_fly_blocked_ns = 0;
    /* Something in the way (the person: the helicopter went through mountains and buildings - the old 2.5 m step
     * through anything that held it - and still stopped at the invisible walls): no more stepping through. If over
     * 0.3 s it covers under a quarter of the distance it is asked to, it climbs (6 m/s, on top of what is asked) for
     * as long as it stays held: a building or a slope is flown over, as a real helicopter would. The game's
     * invisible walls are its barrier lines, which Havok-side position changes don't cross either; they still stop
     * the helicopter (not solved). */
    if (fabsf(g_fly_s) > 6.0f) {
        if (!g_fly_stuck_ns) {
            g_fly_stuck_ns = now;
            memcpy(g_fly_last_p, p, sizeof g_fly_last_p);
        } else if (now - g_fly_stuck_ns > 300000000LL) {
            const float mx = p[0] - g_fly_last_p[0], mz = p[2] - g_fly_last_p[2];
            const float span = (float)(now - g_fly_stuck_ns) / 1e9f;
            /* two short windows in a row (a single one was also seen in open air while the world streamed) */
            static int shorts;
            const int short_ = sqrtf(mx * mx + mz * mz) < 0.25f * fabsf(g_fly_s) * span;
            shorts = short_ ? shorts + 1 : 0;
            g_fly_blocked = shorts >= 2;
            g_fly_stuck_ns = 0;
        }
    } else {
        g_fly_stuck_ns = 0;
        g_fly_blocked = 0;
    }
    heli_void(p[1] > g_fly_ground + 8.0f);
    if (getenv("NFSU_HELI_LOG")) {
        static int64_t at;
        if (now - at > 1000000000LL) {
            at = now;
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: heli: at %.1f %.1f %.1f (ground %.1f) speed %.1f up %.1f blocked %d", (double)p[0],
                     (double)p[1], (double)p[2], (double)g_fly_ground, (double)g_fly_s, (double)g_fly_vs, g_fly_blocked);
            api->log(api, l);
        }
    }
    /* height hold: with no climb/descend key the helicopter keeps the height it was left at (gravity between
     * Havok's steps made it sink ~0.5 m/s) */
    if (g_fly_blocked && p[1] < ceiling)
        want_vs = fmaxf(want_vs, 0.0f) + 12.0f;
    if (want_vs != 0.0f || g_fly_hold_y == 0.0f)
        g_fly_hold_y = p[1];
    const float hold = want_vs == 0.0f ? fmaxf(-4.0f, fminf(4.0f, (g_fly_hold_y - p[1]) * 2.0f)) : 0.0f;
    const float fx = sinf(g_fly_h), fz = cosf(g_fly_h);
    const float vel[3] = {fx * g_fly_s, g_fly_vs + hold, fz * g_fly_s};
    /* orientation: yaw about up (y), the nose down a little with speed (about the side axis), roll in turns */
    const float pitch = 0.18f * g_fly_s / 45.0f;
    const float qy[4] = {0, sinf(g_fly_h / 2), 0, cosf(g_fly_h / 2)};
    const float qp[4] = {sinf(pitch / 2), 0, 0, cosf(pitch / 2)};
    const float qr[4] = {0, 0, sinf(g_fly_roll / 2), cosf(g_fly_roll / 2)};
    float t[4], q[4];
    quat_mul(t, qy, qp);
    quat_mul(q, t, qr);
    const float zero[3] = {0, 0, 0};
    heli_arrest_pass(api, p, dt);
    heli_rb_set(api, rb, 28, q);
    heli_rb_set(api, rb, 23, vel);
    heli_rb_set(api, rb, 24, zero);
}

/* ---- WHICH CAR IS THE HELICOPTER (setting police_heli) ----
 * The Police Career plugin's player police cars are their own pvehicle collections (AI cops use cop_car_*):
 * player_cop_car_suv_04 c1733734, player_cop_car_mid_05 1bb4725e, player_cop_car_mus_08 088eee9d,
 * player_cop_car_911_tur_06 92dba446, pc_player_cop_car_por_06 eff67758 (keys from the plugin's FUN_10135f20, names
 * matched by hash). The chosen one's MODEL (a StringKey {hash, name}) becomes the helicopter's (pvehicle
 * cop_hel_fed_01): choosing that car in the police garage brings the helicopter, standing where the car would. The
 * original MODEL is put back when the setting changes (the next time the car is made). Flight is on while the
 * career's active car is that car. */
static const uint32_t k_heli_hosts[6] = {0, 0xc1733734u, 0x1bb4725eu, 0x088eee9du, 0x92dba446u, 0xeff67758u};
static uint32_t g_heli_host_addr, g_heli_host_orig[2], g_heli_model[2], g_heli_host_coll;
static uint32_t g_heli_sound_node, g_heli_sound_val[2]; /* [0] the car's, [1] the helicopter's */
static int g_heli_look = -1;
static int g_heli_host = -1;
static uint32_t heli_active_pvehicle(const PopModApi *api) {
    static uint32_t car = 0xfffffffeu, pv;
    static int64_t at;
    const int64_t now = now_ns();
    if (now - at < 1000000000LL && car != 0xfffffffeu)
        return pv;
    at = now;
    car = pc_active_car(api);
    pv = 0;
    const uint32_t obj = get_u32(0x00d8e1a4u);
    for (uint32_t i = 0; obj && car != 0xffffffffu && i < 0x14au; ++i)
        if (get_u32(obj + 0x88u + 28u * i) == car) {
            pv = get_u32(obj + 0x88u + 28u * i + 8);
            break;
        }
    return pv;
}
/* Gives a collection its own node for a field, copied from another collection's node (type, flags and value - a
 * value that is a pointer keeps pointing at the source's data, which is never freed). The collection's own field
 * table (attrib.inc: 12-byte entries {key, value, u16 type, u8 probes of the chains starting here, u8 flags 0x80
 * used}, slot = rotated key % count, linear probing) gets the node in the first free slot from the key's home slot,
 * the home slot's probe count raised to reach it, the used count +1. An own node already there is overwritten.
 * Returns 1 when done, 0 when the table is full or something is missing. */
static int heli_attrib_give(const PopModApi *api, uint32_t coll, uint32_t key, uint32_t src_coll) {
    int ws = -1, wd = -1;
    const uint32_t sn = src_coll ? attrib_node(src_coll, key, &ws) : 0;
    if (!coll || !sn)
        return 0;
    uint32_t value = get_u32(sn + 4), flags = get_u8(sn + 0xb);
    const uint32_t type = get_u16(sn + 8);
    if (flags & 0x32u) { /* stored in a layout (or an array): the new node gets its own copy */
        const uint32_t size = get_u32(get_u32(ATTRIB_TYPES) + 16u * type + 0xc) & 0xffffffu;
        const uint32_t from = attrib_value_addr(src_coll, sn);
        if ((flags & 0x02u) || !from || !size || size > 64)
            return 0;
        if (size <= 4) {
            value = get_u32(from) & (size == 4 ? 0xffffffffu : ((1u << (8 * size)) - 1u));
            flags = (flags & ~0x30u) | 0x40u;
        } else {
            uint32_t copy = 0;
            if (!api->guest_alloc || api->guest_alloc(api, (size + 15u) & ~15u, &copy) != POP_OK || !copy)
                return 0;
            for (uint32_t o = 0; o < size; o += 4)
                api->guest_write_u32(api, copy + o, get_u32(from + o));
            value = copy;
            flags &= ~0x70u;
        }
    }
    const uint32_t own = attrib_node(coll, key, &wd);
    if (own && wd == 0) {
        api->guest_write_u32(api, own + 4, value);
        return 1;
    }
    uint32_t n = get_u16(coll + 4), used = get_u16(coll + 6), ents = get_u32(coll);
    if (!n || !ents)
        return 0;
    const unsigned sh = get_u8(coll + 0xb) & 31u;
    if (used + 1 >= n) { /* full (the game sizes them exactly): a bigger table, every node put back by its hash */
        const uint32_t nn = 2u * n + 3u;
        uint32_t big = 0;
        if (!api->guest_alloc || api->guest_alloc(api, nn * 12u, &big) != POP_OK || !big)
            return 0;
        for (uint32_t i = 0; i < nn; ++i) { /* empty: not used, no probes */
            api->guest_write_u32(api, big + i * 12u, 0);
            api->guest_write_u32(api, big + i * 12u + 4, 0);
            api->guest_write_u32(api, big + i * 12u + 8, 0);
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t o = ents + i * 12u;
            if (!(get_u8(o + 0xb) & 0x80u))
                continue;
            const uint32_t k = get_u32(o), hh = sh ? (k << sh) | (k >> (32u - sh)) : k, hm = hh % nn;
            for (uint32_t d = 0; d < nn; ++d) {
                const uint32_t e = big + ((hm + d) % nn) * 12u;
                if (get_u8(e + 0xb) & 0x80u)
                    continue;
                api->guest_write_u32(api, e, k);
                api->guest_write_u32(api, e + 4, get_u32(o + 4));
                api->guest_write_u32(api, e + 8, (get_u32(e + 8) & 0x00ff0000u) | (get_u32(o + 8) & 0xff00ffffu));
                const uint32_t he = big + hm * 12u, hw = get_u32(he + 8);
                if (((hw >> 16) & 0xffu) < d)
                    api->guest_write_u32(api, he + 8, (hw & 0xff00ffffu) | (d << 16));
                break;
            }
        }
        api->guest_write_u32(api, coll, big);
        api->guest_write_u32(api, coll + 4, (get_u32(coll + 4) & 0xffff0000u) | nn);
        n = nn, ents = big;
    }
    const uint32_t h = sh ? (key << sh) | (key >> (32u - sh)) : key;
    const uint32_t home = h % n;
    for (uint32_t d = 0; d < n && d < 255; ++d) {
        const uint32_t e = ents + ((home + d) % n) * 12u;
        if (get_u8(e + 0xb) & 0x80u)
            continue;
        api->guest_write_u32(api, e, key);
        api->guest_write_u32(api, e + 4, value);
        /* +8 type (u16), +0xa this slot's own probe count (kept), +0xb flags */
        const uint32_t w2 = get_u32(e + 8);
        api->guest_write_u32(api, e + 8, (w2 & 0x00ff0000u) | (type & 0xffffu) | ((flags | 0x80u) << 24));
        const uint32_t he = ents + home * 12u, hw = get_u32(he + 8);
        if (((hw >> 16) & 0xffu) < d)
            api->guest_write_u32(api, he + 8, (hw & 0xff00ffffu) | (d << 16));
        const uint32_t uw = get_u32(coll + 4);
        api->guest_write_u32(api, coll + 4, (uw & 0x0000ffffu) | ((used + 1) << 16));
        return 1;
    }
    return 0;
}

/* The helicopter sounds like one: pvehicle's BEHAVIOR_MECHANIC_AUDIO picks the sound behaviour made with the car
 * (registered by name at 0x00bc52e0: SoundRacer FUN_00736fc0, SoundHelicopter FUN_00737150 - the AI helicopter's,
 * from its parent collection "choppers"). The host car gets the choppers' value as its own node, so a car made from
 * it gets the helicopter's sound behaviour instead of the engine's. NFSU_HELI_NOSOUND=1 leaves the car's. */
static void heli_give_sound(const PopModApi *api, uint32_t host) {
    if (getenv("NFSU_HELI_NOSOUND"))
        return;
    const uint32_t pk = attrib_key("pvehicle");
    const int ok = heli_attrib_give(api, host, attrib_key("BEHAVIOR_MECHANIC_AUDIO"), attrib_collection(pk, attrib_key("choppers")));
    char l[200];
    int w = -1;
    const uint32_t ch = attrib_collection(pk, attrib_key("choppers"));
    const uint32_t sn = ch ? attrib_node(ch, attrib_key("BEHAVIOR_MECHANIC_AUDIO"), &w) : 0;
    snprintf(l, sizeof l, "core.nfsu: heli: the helicopter's sound %s (choppers %08x node %08x flags %02x type %u, "
             "table %u/%u)", ok ? "given" : "could NOT be given - the car keeps its engine", ch, sn,
             sn ? get_u8(sn + 0xb) : 0, sn ? get_u16(sn + 8) : 0, host ? get_u16(host + 6) : 0, host ? get_u16(host + 4) : 0);
    api->log(api, l);
}

static void heli_host_apply(const PopModApi *api) {
    int64_t want = 1;
    if (api->settings_get)
        api->settings_get(api, "police_heli", &want);
    if (getenv("NFSU_POLICE_HELI"))
        want = atoi(getenv("NFSU_POLICE_HELI"));
    if (want < 0 || want > 5)
        want = 0;
    if ((int)want == g_heli_host || !get_u32(ATTRIB_DB))
        return;
    const uint32_t pk = attrib_key("pvehicle"), mk = attrib_key("MODEL");
    if (g_heli_host_addr) { /* put the previous car's model back */
        api->guest_write_u32(api, g_heli_host_addr, g_heli_host_orig[0]);
        api->guest_write_u32(api, g_heli_host_addr + 4, g_heli_host_orig[1]);
        g_heli_host_addr = 0;
    }
    g_heli_host = (int)want;
    if (!want)
        return;
    const uint32_t host = attrib_collection(pk, k_heli_hosts[want]), heli = attrib_collection(pk, attrib_key("cop_hel_fed_01"));
    int w1 = -1, w2 = -1;
    const uint32_t n1 = host ? attrib_node(host, mk, &w1) : 0, n2 = heli ? attrib_node(heli, mk, &w2) : 0;
    char l[160];
    if (!n1 || !n2) {
        snprintf(l, sizeof l, "core.nfsu: heli: car %08x not loaded yet (police career off?)", k_heli_hosts[want]);
        api->log(api, l);
        g_heli_host = -1; /* try again later: the plugin's vault mounts after the game's */
        static int64_t tries;
        if (++tries > 30)
            g_heli_host = (int)want; /* give up quietly */
        return;
    }
    g_heli_host_addr = attrib_value_addr(host, n1);
    const uint32_t a2 = attrib_value_addr(heli, n2);
    g_heli_host_orig[0] = get_u32(g_heli_host_addr), g_heli_host_orig[1] = get_u32(g_heli_host_addr + 4);
    g_heli_model[0] = get_u32(a2), g_heli_model[1] = get_u32(a2 + 4);
    g_heli_host_coll = host;
    snprintf(l, sizeof l, "core.nfsu: heli: police car %08x carries the helicopter when its helicopter row is chosen (model %08x / %08x)",
             k_heli_hosts[want], g_heli_host_orig[0], g_heli_model[0]);
    api->log(api, l);
    heli_give_sound(api, host);
    /* the sound node now exists in the host's own table: remember the helicopter's value and the car's (racers') */
    {
        const uint32_t key = attrib_key("BEHAVIOR_MECHANIC_AUDIO");
        int w = -1, wr = -1;
        const uint32_t own = attrib_node(host, key, &w);
        const uint32_t rc = attrib_collection(pk, attrib_key("racers"));
        const uint32_t rn = rc ? attrib_node(rc, key, &wr) : 0;
        if (own && w == 0 && rn && get_u8(rn + 0xb) == get_u8(own + 0xb)) {
            g_heli_sound_node = own;
            g_heli_sound_val[1] = get_u32(own + 4);
            g_heli_sound_val[0] = get_u32(rn + 4);
        }
    }
    g_heli_look = -1;
}

/* The police car looks, sounds and flies as the helicopter only while its helicopter row is the active car (on: the
 * helicopter's MODEL and sound behaviour in the host collection, off: the car's own). Set before the world makes the
 * car, which reads them then; called every pass, writes only on a change. */
static void heli_look_set(const PopModApi *api, int on) {
    if (!g_heli_host_addr || on == g_heli_look)
        return;
    g_heli_look = on;
    api->guest_write_u32(api, g_heli_host_addr, on ? g_heli_model[0] : g_heli_host_orig[0]);
    api->guest_write_u32(api, g_heli_host_addr + 4, on ? g_heli_model[1] : g_heli_host_orig[1]);
    if (g_heli_sound_node)
        api->guest_write_u32(api, g_heli_sound_node + 4, g_heli_sound_val[on ? 1 : 0]);
    api->log(api, on ? "core.nfsu: heli: the police SUV is the helicopter now (its own row)"
                     : "core.nfsu: heli: the police SUV is an SUV again");
}

static void heli_lookat(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const uint32_t ret = get_u32(cpu->esp);
    if (getenv("NFSU_HELI_CAMLOG")) {
        static uint32_t seen[16];
        int known = 0;
        for (int i = 0; i < 16 && seen[i]; ++i)
            known |= seen[i] == ret;
        if (!known) {
            for (int i = 0; i < 16; ++i)
                if (!seen[i]) {
                    seen[i] = ret;
                    break;
                }
            char l[80];
            snprintf(l, sizeof l, "core.nfsu: heli: lookat from %08x", ret);
            api->log(api, l);
        }
    }
    if (getenv("NFSU_HELI_CAMLOG") && g_fly_flying && ret != 0x004e0d20u) {
        static unsigned n;
        if (n++ < 40) {
            char l[80];
            snprintf(l, sizeof l, "core.nfsu: heli: another look-at while flying, from %08x", ret);
            api->log(api, l);
        }
    }
    if (ret != 0x004e0d20u) /* FUN_004deb10, the chase camera */
        return;
    const uint32_t pe = get_u32(cpu->esp + 8), pt = get_u32(cpu->esp + 12), pu = get_u32(cpu->esp + 16);
    /* while flying: a steady camera of our own. The camera works in render coordinates (x = sim z, y = -sim x,
     * z = sim y; measured: car sim -1060.6 184.4 1933.0 = target render 1933.0 1060.6 ...). Target: the helicopter;
     * eye: 16 m behind along a heading that follows the helicopter's smoothly, 6 m above. */
    if (g_fly_flying) {
        float hp[3];
        const uint32_t rb = heli_rb(heli_player_simable());
        if (rb && heli_rb_pos(rb, hp)) {
            static int64_t last;
            const int64_t now = now_ns();
            float dt = last ? (float)(now - last) / 1e9f : 0.0f;
            last = now;
            if (dt > 0.1f || dt <= 0.0f)
                dt = 0.016f;
            float d = g_fly_h - g_cam_h;
            while (d > 3.14159265f)
                d -= 6.2831853f;
            while (d < -3.14159265f)
                d += 6.2831853f;
            g_cam_h += d * fminf(1.0f, 3.5f * dt);
            const float tx = hp[2], ty = -hp[0], tz = hp[1] + 2.0f;
            const float fx = cosf(g_cam_h), fy = -sinf(g_cam_h);
            const float dist = 16.0f + 0.12f * fabsf(g_fly_s), high = 6.0f;
            const float eye[3] = {tx - fx * dist, ty - fy * dist, tz + high}, tgt[3] = {tx, ty, tz}, upv[3] = {0, 0, 1};
            for (int k = 0; k < 3; ++k) {
                uint32_t u;
                memcpy(&u, &eye[k], 4);
                api->guest_write_u32(api, pe + 4u * (uint32_t)k, u);
                memcpy(&u, &tgt[k], 4);
                api->guest_write_u32(api, pt + 4u * (uint32_t)k, u);
                memcpy(&u, &upv[k], 4);
                api->guest_write_u32(api, pu + 4u * (uint32_t)k, u);
            }
        }
    }
    if (getenv("NFSU_HELI_CAMLOG")) {
        static int64_t at;
        if (now_ns() - at > 1000000000LL) {
            at = now_ns();
            float sp[3] = {0, 0, 0};
            const uint32_t rb = heli_rb(heli_player_simable());
            if (rb)
                heli_rb_pos(rb, sp);
            char l[300];
            snprintf(l, sizeof l, "core.nfsu: heli: cam eye %.1f %.1f %.1f target %.1f %.1f %.1f up %.2f %.2f %.2f; car %.1f %.1f %.1f",
                     (double)tp_f32(pe), (double)tp_f32(pe + 4), (double)tp_f32(pe + 8), (double)tp_f32(pt),
                     (double)tp_f32(pt + 4), (double)tp_f32(pt + 8), (double)tp_f32(pu), (double)tp_f32(pu + 4),
                     (double)tp_f32(pu + 8), (double)sp[0], (double)sp[1], (double)sp[2]);
            api->log(api, l);
        }
    }
}

/* The game puts a car it thinks is lost (high in the air, off the drivable world) back on the road:
 * FUN_00720310 (thiscall vehicle reset to a position, 3 arguments, RET 0xc; called by FUN_007373d0 from the ring of
 * last good positions and by FUN_00730050; it raises EVehicleReset). Measured: at 273 m the helicopter was put back
 * at its take-off point. While flying, that reset is refused. */
static void heli_no_reset(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    if (g_fly_flying) {
        api->hook_return(api, cpu, 0, 0xc);
        return;
    }
    api->call_next(api, inv, cpu);
}

static int64_t g_road_check_ns; /* when to log where the car stands after the reset */
/* Back in a car (the person: the car appeared over the road at the helicopter's height and fell onto it). The new
 * car is made where the helicopter was, in the air; the game's reset (EResetPlayerCar, factory FUN_0066c340, what the
 * pause menu's reset uses) puts it in the middle of the nearest road, on it. It used to run 1.5 s later and the car
 * fell meanwhile: now it runs on the first driving pass, and until then the car is held still where it is.
 * (Tried and dropped: the game's put-on-road FUN_007302d0 never found a road from high above - its window is 6 m;
 * pressing the car down at 45 m/s pushed it through the ground.) */
static void heli_back_on_road(const PopModApi *api) {
    uint32_t r = 0;
    float p[3] = {0, 0, 0};
    const uint32_t rb = heli_rb(heli_player_simable());
    if (rb)
        heli_rb_pos(rb, p);
    api->guest_call(api, 0x0066c340u, 0, NULL, 0, &r);
    char l[120];
    snprintf(l, sizeof l, "core.nfsu: heli: the car is put on the nearest road (it was at %.1f %.1f %.1f)", (double)p[0],
             (double)p[1], (double)p[2]);
    api->log(api, l);
    g_road_check_ns = now_ns() + 1500000000LL;
}
static void heli_hold_still(const PopModApi *api) {
    const uint32_t rb = heli_rb(heli_player_simable());
    if (!rb)
        return;
    const float zero[3] = {0, 0, 0};
    heli_rb_set(api, rb, 23, zero);
    heli_rb_set(api, rb, 24, zero);
}

/* Distance haze while flying high (961f patch, c20): the world loads around the car as seen from the road; from
 * the air more is in view than is loaded, so the far distance fades into the game's own fog colour - none on the
 * ground, growing from 15 m above the take-off height to full at 90 m. Always sent (zero amount on the ground) so
 * the register the patch reads is always set. NFSU_HELI_HAZE=<start>,<end> metres (default 260,560) for tuning. */
static void heli_haze_pass(float height) {
    static int (*set)(uint64_t, uint32_t, float, float, float, float);
    static int looked;
    static float sent = -1.0f;
    if (!looked) {
        looked = 1;
        set = (int (*)(uint64_t, uint32_t, float, float, float, float))dlsym(RTLD_DEFAULT, "recomp_d3d9_ps_reg_set");
    }
    if (!set)
        return;
    float a = (height - 15.0f) / 75.0f;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    a = 0.9f * a * a * (3.0f - 2.0f * a);
    if (sent >= 0.0f && fabsf(a - sent) < 0.01f)
        return;
    sent = a;
    float st = 260.0f, en = 560.0f;
    const char *hz_env = getenv("NFSU_HELI_HAZE");
    if (hz_env)
        sscanf(hz_env, "%f,%f", &st, &en);
    set(0x961f166f1ae442c4ull, 20, a, st, en, 1.0f);
}

/* NFSU_HELI_FILTERLOG=1 (diagnostic): which collision filter infos meet (FUN_006d7bd0's two infos, args 0 and 2) -
 * a count per (layer, top byte) pair, printed every 5 s */
static void heli_filter_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static uint32_t keys[64], counts[64];
    static int64_t at;
    uint32_t a = get_u32(cpu->esp + 4), b = get_u32(cpu->esp + 12);
    if ((a & 0x3f) > (b & 0x3f)) {
        const uint32_t t = a;
        a = b, b = t;
    }
    /* with a world shape (layer 6): its sub-type bits 6..9 instead of the other's layer */
    const uint32_t k = (b & 0x3f) == 6 ? (((a & 0x3f) << 24) | ((a >> 24) << 16) | ((0x60u + ((b >> 6) & 0xf)) << 8) | (b >> 24))
                                       : (((a & 0x3f) << 24) | ((a >> 24) << 16) | ((b & 0x3f) << 8) | (b >> 24));
    for (int i = 0; i < 64; ++i)
        if (!counts[i] || keys[i] == k) {
            keys[i] = k, ++counts[i];
            break;
        }
    if (now_ns() - at > 5000000000LL) {
        at = now_ns();
        char l[1200];
        int m = snprintf(l, sizeof l, "core.nfsu: filter pairs (layer:flags layer:flags x count):");
        for (int i = 0; i < 64 && counts[i] && m < 1100; ++i)
            m += snprintf(l + m, sizeof l - m, " %u:%02x-%u:%02x x%u", keys[i] >> 24, (keys[i] >> 16) & 0xff,
                          (keys[i] >> 8) & 0xff, keys[i] & 0xff, counts[i]);
        api->log(api, l);
    }
}

/* NFSU_STREAM_DUMP=<s> (diagnostic): the track streamer's section table (streamer 0x01317f30: [+0] rows of 0x5c
 * bytes, count [+4]) - every row's dwords as hex and float, and the player's position, once at that run time */
static void stream_dump(const PopModApi *api) {
    static int done;
    const char *at = getenv("NFSU_STREAM_DUMP");
    if (!at || done || now_ns() - g_tp_start_ns < (int64_t)atoi(at) * 1000000000LL)
        return;
    done = 1;
    const uint32_t st = 0x01317f30u, rows = get_u32(st), n = get_u32(st + 4);
    float p[3] = {0, 0, 0};
    const uint32_t rb = heli_rb(heli_player_simable());
    if (rb)
        heli_rb_pos(rb, p);
    char l[1400];
    snprintf(l, sizeof l, "core.nfsu: stream dump: %u sections at %08x; player sim %.1f %.1f %.1f (render %.1f %.1f %.1f); wanted %u; pool %08x size %08x",
             n, rows, (double)p[0], (double)p[1], (double)p[2], (double)p[2], (double)-p[0], (double)p[1],
             get_u32(st + 0x64c), get_u32(st + 0xe50), get_u32(st + 0xe54));
    api->log(api, l);
    for (uint32_t i = 0; i < n && i < 2000; ++i) {
        const uint32_t r = rows + i * 0x5c;
        int m = snprintf(l, sizeof l, "core.nfsu: stream row %u:", i);
        for (uint32_t k = 0; k < 0x5c; k += 4) {
            const uint32_t v = get_u32(r + k);
            float f;
            memcpy(&f, &v, 4);
            if (fabsf(f) > 1.0f && fabsf(f) < 100000.0f)
                m += snprintf(l + m, sizeof l - m, " %x:%.1f", k, (double)f);
            else
                m += snprintf(l + m, sizeof l - m, " %x:%08x", k, v);
        }
        api->log(api, l);
    }
}

/* ---- THE HELICOPTER AS A POLICE CAR OF ITS OWN (2026-11-23, the person: "not instead of the police SUV") ----
 * The Police Career plugin knows its five police cars by their pvehicle key in ~11 functions, so a new key would not
 * be a police car to it. But a career car row (career object [0x00d8e1a4] + 0x88, 0x14a rows of 28 bytes) keeps the
 * frontend ([1]: the name and picture in the menus) apart from the pvehicle ([2]: the car that is built). So the
 * helicopter is a second row of the police SUV: pvehicle player_cop_car_suv_04 (a police SUV to the plugin, with its
 * menus, pursuits and arrests), frontend "copcar" (the game's own AI police car frontend, f0ed1c9b) named "Police
 * helicopter". It is made once per career with the plugin's own FUN_10170840 (thiscall rows, (pvehicle, preset 0)),
 * then [1] is set; it is saved with the career like any car. The SUV's MODEL and sound become the helicopter's only
 * while that row is the active car (heli_look_set). */
#define HELI_ROW_FE 0xf0ed1c9bu
#define HELI_NAME_LABEL 0x48454c49u
#define HELI_GARAGE_FE 0x48454c46u /* "FLEH": the helicopter's garage entry's name key (no such frontend exists) */ /* our own label hash, answered by heli_text */
static uint32_t g_heli_row_id = 0xffffffffu;
/* 2026-11-24 (the person: buy and sell the helicopter in the police career; it must not come back once sold): the
 * row heli_row_ensure keeps (g_heli_row_id, not owned: slot byte +0x15 = 0xff) is the car lot's helicopter only;
 * buying it makes an owned row with the same identity - the copcar frontend and the SUV's pvehicle - which is a
 * helicopter too, listed, driven, flown and sold like any police car. */
static uint32_t heli_rows(void);
static int heli_row_is(uint32_t row) { /* any helicopter row: the car lot's or a bought one */
    if (row == 0xffffffffu || g_heli_host <= 0)
        return 0;
    if (row == g_heli_row_id)
        return 1;
    const uint32_t base = heli_rows();
    for (uint32_t i = 0; base && i < 0x14au; ++i)
        if (get_u32(base + 28u * i) == row)
            return get_u32(base + 28u * i + 4) == HELI_ROW_FE && get_u32(base + 28u * i + 8) == k_heli_hosts[g_heli_host];
    return 0;
}
static int heli_row_catalog(uint32_t row) { return g_heli_row_id != 0xffffffffu && row == g_heli_row_id; }
static uint32_t heli_owned_row(void) { /* a bought helicopter's row id, or -1 */
    const uint32_t base = heli_rows();
    for (uint32_t i = 0; base && g_heli_host > 0 && i < 0x14au; ++i) {
        const uint32_t r = base + 28u * i, id = get_u32(r);
        if (id != 0xffffffffu && id != g_heli_row_id && get_u32(r + 4) == HELI_ROW_FE &&
            get_u32(r + 8) == k_heli_hosts[g_heli_host] && get_u8(r + 0x15) < 0x47u)
            return id;
    }
    return 0xffffffffu;
}
/* What the front end's car preview shows (2026-11-23, the person: "the helicopter has the SUV's look" in the shop):
 * the helicopter's row has the SUV's pvehicle, so the preview drew the SUV. The highlight handlers - shop
 * FUN_005aa390 (CarHighlighted 0x005b8320: list [shop+0x88], index [shop+0xac]) and garage FUN_0058d530
 * (CarHighlighted 0x0059dd50: list [garage+0xac], index [garage+0x140]) - read entry +0x18 (the career row) and
 * build the preview from it; a BEFORE hook switches the SUV's look to the helicopter's for the helicopter's row
 * and back for any other, and that state holds outside driving (-1 = none chosen: the active car decides). */
static int g_fe_heli = -1;
static void heli_look_set(const PopModApi *api, int on);
static void heli_fe_pick(const PopModApi *api, uint32_t list, uint32_t index) {
    if (!list || g_heli_row_id == 0xffffffffu)
        return;
    const uint32_t b = get_u32(list), e = get_u32(list + 4);
    if (!b || index >= 0x1000u || b + 36u * (index + 1u) > e)
        return;
    g_fe_heli = heli_row_is(get_u32(b + 36u * index + 0x18));
    if (getenv("NFSU_HELI_SHOPLOG")) {
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: heli: shop highlight %u of %u: row %08x fe %08x -> %d (catalog %08x)", index,
                 (e - b) / 36u, get_u32(b + 36u * index + 0x18), get_u32(b + 36u * index), g_fe_heli, g_heli_row_id);
        api->log(api, l);
    }
    heli_look_set(api, g_fe_heli);
}
static void heli_fe_shop(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (cpu->ecx)
        heli_fe_pick(api, get_u32(cpu->ecx + 0x88), get_u32(cpu->ecx + 0xac));
}
/* the garage the police "Гараж" opens: CarHighlighted 0x005b8370 -> FUN_005b60c0 (object [0x00d9a4a4], list
 * +0x58/+0x5c, index +0x80) -> FUN_005aa740 (thiscall, the entry's career row) builds the preview */
static void heli_fe_mycars(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (g_heli_row_id == 0xffffffffu)
        return;
    if (getenv("NFSU_HELI_SHOPLOG")) {
        char l[96];
        snprintf(l, sizeof l, "core.nfsu: heli: garage preview of row %08x", get_u32(cpu->esp + 4));
        api->log(api, l);
    }
    g_fe_heli = heli_row_is(get_u32(cpu->esp + 4));
    heli_look_set(api, g_fe_heli);
}
static void heli_fe_showlog(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: heli: FE show car %08x (from %08x) look %d fe %d", get_u32(cpu->esp + 4),
             get_u32(cpu->esp), g_heli_look, g_fe_heli);
    api->log(api, l);
}
static void heli_fe_garage(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (cpu->ecx && getenv("NFSU_HELI_SHOPLOG")) {
        const uint32_t list = get_u32(cpu->ecx + 0xac), b = list ? get_u32(list) : 0, e = list ? get_u32(list + 4) : 0;
        for (uint32_t x = b; b && x < e && x - b < 40u * 36u; x += 36u) {
            char l[200];
            int n = snprintf(l, sizeof l, "core.nfsu: heli garage entry %u (sel %u):", (x - b) / 36u,
                             get_u32(cpu->ecx + 0x140));
            for (uint32_t k = 0; k < 36u && n < 180; k += 4)
                n += snprintf(l + n, sizeof l - n, " %08x", get_u32(x + k));
            api->log(api, l);
        }
    }
    if (cpu->ecx)
        heli_fe_pick(api, get_u32(cpu->ecx + 0xac), get_u32(cpu->ecx + 0x140));
}
static uint32_t heli_rows(void) {
    const uint32_t obj = get_u32(0x00d8e1a4u);
    return obj ? obj + 0x88u : 0;
}
static uint32_t heli_row_find(uint32_t pv) {
    const uint32_t base = heli_rows();
    for (uint32_t i = 0; base && i < 0x14au; ++i) {
        const uint32_t row = base + 28u * i;
        if (get_u32(row) != 0xffffffffu && get_u32(row + 4) == HELI_ROW_FE && get_u32(row + 8) == pv &&
            get_u8(row + 0x15) == 0xffu) /* the car lot's (not owned) row; bought copies are owned */
            return get_u32(row);
    }
    return 0xffffffffu;
}
static void heli_row_ensure(const PopModApi *api) {
    static int64_t at;
    static int made;
    const int64_t now = now_ns();
    if (getenv("NFSU_HELI_LOG")) {
        static int64_t said;
        if (now - said > 5000000000LL) {
            said = now;
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: heli: garage row check: host %d rows %08x plugin %08x driving %d id %08x made %d",
                     g_heli_host, heli_rows(), get_u32(0x101d69a0u), tp_driving(), g_heli_row_id, made);
            api->log(api, l);
        }
    }
    /* the plugin is there when its police car collection was found (g_heli_host > 0); its profile block [0x101d69a0]
     * is 0 outside police mode */
    if (g_heli_host <= 0 || !heli_rows())
        return;
    if (getenv("NFSU_HELI_ROWLOG")) {
        static int64_t said;
        if (now - said > 5000000000LL) {
            said = now;
            for (uint32_t i = 0; i < 0x14au; ++i) {
                const uint32_t r = heli_rows() + 28u * i;
                const uint32_t id = get_u32(r);
                if (id == 0xffffffffu || !(get_u32(r + 4) == HELI_ROW_FE || id == 0x10cu || id == 0x11eu ||
                                           id == 0x11fu || id == 0x120u))
                    continue;
                char l[160];
                snprintf(l, sizeof l, "core.nfsu: heli: row %u: %08x %08x %08x %08x %08x %08x %08x", i, id,
                         get_u32(r + 4), get_u32(r + 8), get_u32(r + 12), get_u32(r + 16), get_u32(r + 20),
                         get_u32(r + 24));
                api->log(api, l);
            }
        }
    }
    /* found every pass (cheap), so the look is right before a saved helicopter is built at a career load */
    const uint32_t pv = k_heli_hosts[g_heli_host];
    const uint32_t id = heli_row_find(pv);
    if (id != 0xffffffffu) {
        if (id != g_heli_row_id) {
            g_heli_row_id = id;
            char l[96];
            snprintf(l, sizeof l, "core.nfsu: heli: the helicopter is career car %08x", id);
            api->log(api, l);
        }
        return;
    }
    if (made || !tp_driving() || !api->guest_call || now - at < 2000000000LL)
        return;
    at = now;
    made = 1;
    const uint32_t args[2] = {pv, 0};
    uint32_t row = 0;
    if (api->guest_call(api, 0x10170840u, heli_rows(), args, 2, &row) != POP_OK || row < 0x01400000u) {
        api->log(api, "core.nfsu: heli: the helicopter could NOT be added to the police garage");
        return;
    }
    api->guest_write_u32(api, row + 4, HELI_ROW_FE);
    g_heli_row_id = get_u32(row);
    char l[120];
    snprintf(l, sizeof l, "core.nfsu: heli: the helicopter added to the police garage (career car %08x)", g_heli_row_id);
    api->log(api, l);
}
/* its name: FUN_00525070 (cdecl, frontend -> label hash) for "copcar" gives our label, the text lookup FUN_00571b80
 * (cdecl, label hash -> text) answers it */
static void heli_name_label(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    static int nlog = -1;
    if (nlog < 0)
        nlog = getenv("NFSU_HELI_NAMELOG") != NULL;
    if (nlog) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: heli namelog: label of %08x -> %08x (from %08x)", get_u32(cpu->esp), cpu->eax,
                 cpu->eip);
        api->log(api, l);
    }
    /* No longer renames "copcar": that frontend is the police INTERCEPTOR's as well (the person's garage showed an
     * interceptor row named as the helicopter). The helicopter's garage entry carries its own key, HELI_GARAGE_FE,
     * whose label is ours. */
    if (get_u32(cpu->esp) == HELI_GARAGE_FE && g_heli_row_id != 0xffffffffu)
        cpu->eax = HELI_NAME_LABEL;
}
static void heli_text(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    static uint32_t text;
    static int nlog = -1;
    if (nlog < 0)
        nlog = getenv("NFSU_HELI_NAMELOG") != NULL;
    if (nlog && cpu->eax && getenv("NFSU_TEXT_ALL")) {
        static int n;
        if (n++ < 3000) {
            char l[140];
            snprintf(l, sizeof l, "core.nfsu: textall %08x from %08x -> %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     get_u32(cpu->esp), cpu->eip, get_u8(cpu->eax), get_u8(cpu->eax + 1), get_u8(cpu->eax + 2),
                     get_u8(cpu->eax + 3), get_u8(cpu->eax + 4), get_u8(cpu->eax + 5), get_u8(cpu->eax + 6),
                     get_u8(cpu->eax + 7), get_u8(cpu->eax + 8), get_u8(cpu->eax + 9), get_u8(cpu->eax + 10),
                     get_u8(cpu->eax + 11), get_u8(cpu->eax + 12));
            api->log(api, l);
        }
    }
    if (nlog && cpu->eax && cpu->eip == 0x005b54e2u) {
        char l[200];
        int n = snprintf(l, sizeof l, "core.nfsu: heli namelog: text %08x bytes", get_u32(cpu->esp));
        for (uint32_t k = 0; k < 40 && n < 190; ++k) {
            const uint32_t w = get_u32(cpu->eax + (k & ~3u));
            n += snprintf(l + n, sizeof l - n, " %02x", (w >> (8 * (k & 3))) & 0xff);
        }
        api->log(api, l);
    }
    if (nlog && cpu->eax && get_u32(cpu->eax) == 0x5353494du) { /* "MISS" */
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: heli namelog: MISSING text for %08x (from %08x, caller %08x)", get_u32(cpu->esp),
                 cpu->eip, get_u32(cpu->esp + 4));
        api->log(api, l);
    }
    /* HELI_NAME_LABEL from heli_name_label; 0x136e857f is the label the shop list itself asks (FUN_005251f0's
     * name of the copcar frontend, from 0x00525224 / 0x005b54e2), which the game has no text for */
    const uint32_t lab = get_u32(cpu->esp); /* cdecl, after RET: arg at esp */
    /* 0x136e857f is also the "no region" text FUN_00525200 asks for every police car (0x00525224): answered only
     * for the shop list's own lookup (0x005b54e2), which asks it for the helicopter's entry */
    /* 2026-11-24: while a helicopter row is highlighted in a shop or garage (g_fe_heli), the SUV's own name ("Полицейский
     * вне..." in the game's code) that the purchase dialog and the info panel take from its pvehicle is the
     * helicopter's */
    static const uint8_t suv_name[] = {0xa2, 0x81, 0x8a, 0x83, 0xb2, 0x84, 0x97, 0x88, 0x8b, 0x83, 0x97, 0x20, 0x89, 0x85, 0x84};
    int suv = 0;
    if (g_fe_heli == 1 && cpu->eax) {
        suv = 1;
        for (uint32_t k = 0; k < sizeof suv_name && suv; ++k)
            suv = get_u8(cpu->eax + k) == suv_name[k];
    }
    if (lab != HELI_NAME_LABEL && !suv &&
        !(lab == 0x136e857fu && g_heli_row_id != 0xffffffffu && (cpu->eip == 0x005b54e2u || cpu->eip == 0x005b622du)))
        return;
    if (!text) {
        /* the game's own single-byte Cyrillic order (CLAUDE.md "RUSSIAN TEXT CODE"; л 8a, ц b2 from its "Кольцевая
         * гонка" = ac 81 8a 93 b2 84 89 82 92 20 94 81 85 8b 82): "Полицейский вертолет" (no ё in the game's text) */
        const char *t = "\xa2\x81\x8a\x83\xb2\x84\x97\x88\x8b\x83\x97 \x89\x84\x87\x86\x81\x8a\x84\x86";
        const uint32_t n = (uint32_t)strlen(t) + 1;
        if (api->guest_alloc && api->guest_alloc(api, (n + 3u) & ~3u, &text) == POP_OK && text)
            for (uint32_t i = 0; i < n; i += 4) {
                uint32_t w = 0;
                for (uint32_t b = 0; b < 4 && i + b < n; ++b)
                    w |= (uint32_t)(uint8_t)t[i + b] << (8 * b);
                api->guest_write_u32(api, text + i, w);
            }
    }
    if (text)
        cpu->eax = text;
}

/* ---- THE HELICOPTER IN THE SHOP (2026-11-23, the person: "it isn't in the shop; add it at the bottom") ----
 * The shop (FUN_005b5010) takes its cars from the game's "cars for sale" query (vtable 0x00be7418, run by
 * FUN_00519850(store, query, -1), thiscall, RET 8): a vector at query + 8 (begin) / + 0xc (end) / + 0x10 (capacity)
 * of 36-byte entries: +0 frontend key, +0xc price, +0x10 the shop section (0x10000 America, 0x20000 Europe, 0x40000
 * Japan), +0x18 the career row id (measured with NFSU_HELI_SHOPLOG). The helicopter's row has the "copcar" frontend, which belongs to no section. After the query ran: its entry
 * (or, when the query left it out, a copy of the police SUV's entry with the helicopter's row id) gets the SUV's
 * section and goes to the END of the vector - the bottom of the police cars. NFSU_HELI_SHOPLOG=1 logs the entries. */
static uint32_t g_shop_query;
static void heli_shop_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    const uint32_t q = get_u32(cpu->esp + 4);
    g_shop_query = q && get_u32(q) == 0x00be7418u && (int32_t)get_u32(cpu->esp + 8) == -1 ? q : 0;
}
static void heli_shop_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    const uint32_t q = g_shop_query;
    g_shop_query = 0;
    if (!q || g_heli_row_id == 0xffffffffu || g_heli_host <= 0)
        return;
    uint32_t b = get_u32(q + 8), e = get_u32(q + 0xc);
    const uint32_t cap = get_u32(q + 0x10);
    if (!b || e < b || (e - b) % 36u || (e - b) / 36u > 2000u)
        return;
    const uint32_t n = (e - b) / 36u;
    /* the SUV's row id(s): rows with the host pvehicle that aren't the helicopter */
    const uint32_t pv = k_heli_hosts[g_heli_host], rows = heli_rows();
    uint32_t suv_entry = 0, heli_entry = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t en = b + 36u * i, id = get_u32(en + 0x18);
        if (getenv("NFSU_HELI_SHOPLOG") && i < 400) {
            char l[200];
            int m = snprintf(l, sizeof l, "core.nfsu: shop query entry %u:", i);
            for (uint32_t k = 0; k < 36; k += 4)
                m += snprintf(l + m, sizeof l - m, " %08x", get_u32(en + k));
            api->log(api, l);
        }
        if (id == g_heli_row_id) {
            heli_entry = en;
            continue;
        }
        for (uint32_t r = 0; rows && !suv_entry && r < 0x14au; ++r) {
            const uint32_t row = rows + 28u * r;
            if (get_u32(row) == id && get_u32(row + 8) == pv && get_u32(row + 4) != HELI_ROW_FE)
                suv_entry = en;
        }
    }
    if (!suv_entry) /* not in police mode (no police SUV for sale): nothing to do */
        return;
    uint32_t tmp[9];
    if (heli_entry) {
        for (int k = 0; k < 9; ++k)
            tmp[k] = get_u32(heli_entry + 4u * (uint32_t)k);
        /* take it out: shift the later entries down one */
        for (uint32_t a = heli_entry; a + 36u < e; a += 4)
            api->guest_write_u32(api, a, get_u32(a + 36u));
        e -= 36u;
    } else {
        if (e + 36u > cap)
            return; /* no room in the vector */
        for (int k = 0; k < 9; ++k)
            tmp[k] = get_u32(suv_entry + 4u * (uint32_t)k);
        tmp[6] = g_heli_row_id; /* +0x18 */
        tmp[0] = HELI_ROW_FE;   /* +0: the frontend - the name shown (entry: +0 frontend, +0xc price, +0x10 section) */
    }
    tmp[4] = get_u32(suv_entry + 0x10); /* the SUV's section */
    for (int k = 0; k < 9; ++k)
        api->guest_write_u32(api, e + 4u * (uint32_t)k, tmp[k]);
    api->guest_write_u32(api, q + 0xc, e + 36u);
    static int said;
    if (!said++)
        api->log(api, heli_entry ? "core.nfsu: heli: the helicopter is at the bottom of the police cars in the shop"
                                 : "core.nfsu: heli: the helicopter added at the bottom of the police cars in the shop");
}

/* The POLICE car shop is the Police Career plugin's own: FUN_101365a0 (stdcall (rows, vector), RET 8; vector {begin,
 * end, ...}) fills it with its five stock cars (table 0x101a52ac -> FUN_10136250 stock row -> FUN_10170ef0 entry, +0
 * the frontend from FUN_10134ca0, section 0x10000), each pushed by FUN_101710b0, which tail-jumps to the game's
 * FUN_0052f570 (thiscall vector, entry; RET 4) - so in that push [esp] is the plugin's return address 0x10136646.
 * The kit hooks only the game's own functions (not the plugin's: the first try was refused), so the push is hooked:
 * at the first push of a fill (BEFORE) the helicopter's entry - a copy of the pushed police car's with the copcar
 * frontend and the helicopter's row id - is pushed too, and after every push of the fill (AFTER) the helicopter's
 * entry is swapped to the end, so it ends the last car of the police shop. */
#define PSHOP_PUSH_RET 0x10136646u
static uint32_t g_pshop_entry;
static int g_pshop_inside;
/* The shop then SORTS each section's list (FUN_005b5010 -> std::sort FUN_005a1b60 (cdecl first, last, pred) on
 * 36-byte entries; preds FUN_00556ee0 by FUN_0051dd30 of the career rows, FUN_005570a0 by name). The helicopter (a
 * copy of the SUV: same class) came out second. The preds are not hookable (not listed functions), so an AFTER hook
 * on the sort moves the helicopter's entry (+0x18 = its row) to the end of the sorted range - the bottom. */
static void heli_sort_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (g_heli_row_id == 0xffffffffu)
        return;
    const uint32_t first = get_u32(cpu->esp), last = get_u32(cpu->esp + 4); /* cdecl, after RET: args at esp */
    if (!first || last <= first || (last - first) % 36u || (last - first) / 36u > 512u)
        return;
    /* the police garage's list: sorted after the plugin's filter gave every police entry its frontend - the
     * helicopter's gets its own name key back (heli_gq_after is too early: the filter runs after the query) */
    {
        const uint32_t mc = get_u32(0x00d9a4a4u);
        if (mc && first == get_u32(mc + 0x58))
            for (uint32_t en = first; en < last; en += 36u)
                if (heli_row_is(get_u32(en + 0x18)))
                    api->guest_write_u32(api, en, HELI_GARAGE_FE);
    }
    for (uint32_t en = first; en + 36u < last; en += 36u)
        if (heli_row_is(get_u32(en + 0x18))) {
            uint32_t keep[9];
            for (uint32_t k = 0; k < 9; ++k)
                keep[k] = get_u32(en + 4 * k);
            for (uint32_t x = en; x + 36u < last; x += 4)
                api->guest_write_u32(api, x, get_u32(x + 36u));
            for (uint32_t k = 0; k < 9; ++k)
                api->guest_write_u32(api, last - 36u + 4 * k, keep[k]);
            break;
        }
}
/* The garage the police "Гараж" opens (object [0x00d9a4a4], list +0x58) is filled by the game's owned-cars fill
 * (push return 0x0052fad6) with every owned career row (flags 0xf0102 ...; 30 is the game's limit, FUN_00556ff0) and
 * shows the police ones in police mode. The helicopter's row is not an owned row (it must not fill a garage place),
 * so its entry is added here: when a police car's entry is pushed (its row has a police pvehicle) that car goes in
 * and this push becomes the helicopter's entry (a copy with its row and its name label); later pushes keep it last
 * as in the shop. Choosing it in the garage makes its row the active car. */
static uint32_t g_garage_entry;
static int heli_row_is_police(uint32_t id) { /* a row of one of the plugin's five police cars */
    const uint32_t base = heli_rows();
    for (uint32_t i = 0; base && i < 0x14au; ++i)
        if (get_u32(base + 28u * i) == id) {
            const uint32_t pv = get_u32(base + 28u * i + 8);
            for (int k = 1; k < 6; ++k)
                if (pv == k_heli_hosts[k])
                    return 1;
            return 0;
        }
    return 0;
}
static void heli_garage_push(const PopModApi *api, pop_cpu_v1 *cpu) {
    /* 2026-11-24: no longer - a bought helicopter is an owned row, listed by the game itself */
    if (!getenv("NFSU_HELI_GARAGE_ALWAYS") || g_heli_row_id == 0xffffffffu || g_heli_host <= 0)
        return;
    const uint32_t vec = cpu->ecx, entry = get_u32(cpu->esp + 4);
    if (!entry || entry == g_garage_entry)
        return;
    const uint32_t b = get_u32(vec), e = get_u32(vec + 4);
    if (b && (e < b || (e - b) % 36u || (e - b) / 36u > 512u))
        return;
    for (uint32_t en = b; b && en < e; en += 36u)
        if (get_u32(en + 0x18) == g_heli_row_id) { /* already in: the car takes its slot, the push re-adds it last */
            for (uint32_t k = 0; k < 36u; k += 4)
                api->guest_write_u32(api, en + k, get_u32(entry + k));
            api->guest_write_u32(api, cpu->esp + 4, g_garage_entry);
            return;
        }
    if (!heli_row_is_police(get_u32(entry + 0x18)))
        return;
    if (!g_garage_entry && (!api->guest_alloc || api->guest_alloc(api, 48, &g_garage_entry) != POP_OK || !g_garage_entry))
        return;
    uint32_t r = 0;
    g_pshop_inside = 1;
    const int ok = api->guest_call(api, 0x0052f570u, vec, &entry, 1, &r) == POP_OK;
    g_pshop_inside = 0;
    if (!ok)
        return;
    for (uint32_t k = 0; k < 36u; k += 4)
        api->guest_write_u32(api, g_garage_entry + k, get_u32(entry + k));
    api->guest_write_u32(api, g_garage_entry + 0, HELI_NAME_LABEL);
    api->guest_write_u32(api, g_garage_entry + 0x18, g_heli_row_id);
    api->guest_write_u32(api, cpu->esp + 4, g_garage_entry);
    static int said;
    if (said++ < 3)
        api->log(api, "core.nfsu: heli: the helicopter is in the police garage");
}
/* The plugin's garage filter (its FUN_101363c0, run on the owned-cars query after the game filled it) rewrites
 * every police entry's +0 with its own frontend for the row's pvehicle - the helicopter's entry got the SUV's, so it
 * was listed as "Полицейский внедорожник" (the person took it for a leftover). After the query (FUN_00519850)
 * filled the police garage's list ([0x00d9a4a4] + 0x58 = query + 8), the helicopter's entry gets HELI_GARAGE_FE,
 * which heli_name_label turns into our label. */
static uint32_t g_gq_query[8];
static int g_gq_depth;
static void heli_gq_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_gq_depth < 8)
        g_gq_query[g_gq_depth] = get_u32(cpu->esp + 4);
    ++g_gq_depth;
}
static void heli_gq_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (g_gq_depth <= 0)
        return;
    --g_gq_depth;
    if (g_gq_depth >= 8 || g_heli_row_id == 0xffffffffu)
        return;
    const uint32_t q = g_gq_query[g_gq_depth], mc = get_u32(0x00d9a4a4u);
    if (!q || !mc || q + 8u != mc + 0x58u)
        return;
    const uint32_t b = get_u32(q + 8), e = get_u32(q + 0xc);
    for (uint32_t en = b; b && en < e && en - b < 64u * 36u; en += 36u)
        if (heli_row_is(get_u32(en + 0x18))) {
            if (getenv("NFSU_HELI_SHOPLOG")) {
                char l[96];
                snprintf(l, sizeof l, "core.nfsu: heli: garage entry name key %08x -> ours", get_u32(en));
                api->log(api, l);
            }
            api->guest_write_u32(api, en, HELI_GARAGE_FE);
        }
}
/* "On sale" (2026-11-24): the shop asks FUN_0051df10 (thiscall on a career row: its class flag FUN_0051ddc0, else
 * value 6001 of its frontend > 0) whether a car may be bought; the helicopter's car-lot row was "not available".
 * An AFTER hook answers yes for that row. */
static uint32_t g_onsale_row[8];
static int g_onsale_depth;
static void heli_onsale_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_onsale_depth < 8)
        g_onsale_row[g_onsale_depth] = cpu->ecx;
    ++g_onsale_depth;
}
static void heli_onsale_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_onsale_depth <= 0)
        return;
    --g_onsale_depth;
    if (g_onsale_depth < 8 && g_onsale_row[g_onsale_depth] && heli_row_catalog(get_u32(g_onsale_row[g_onsale_depth])))
        cpu->eax = (cpu->eax & ~0xffu) | 1u;
}
/* THE HELICOPTER ON THE MINIMAP (2026-11-24, the person: show it as a helicopter, not the player's car). The minimap's
 * police pass FUN_005cca20 (thiscall minimap, the player's vehicle; RET 4) draws each nearby police vehicle the
 * game's way: an icon from the minimap's pool FUN_005c19e0, its colour FUN_0054de20, its texture FUN_0054ded0 -
 * HELICOPTER_MINIMAP_ICON (bStringHash 0x586874a0, with the icon size [minimap+0xe8/+0xec] x [0x00bd9ef8]) for a
 * helicopter, MINIMAP_ICON_CAR otherwise - placed by FUN_005c1b90(minimap, &map pos, &heading, icon, 0, 1) from
 * FUN_005cc170(&pos, &heading, vehicle). While you fly, an AFTER hook draws the same helicopter icon at your own
 * vehicle (its vt+4 object, as the pass uses for the police). NFSU_HELI_MAPICON=0 = none. */
static uint32_t g_mm_minimap;
static void heli_mapicon_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    g_mm_minimap = cpu->ecx;
}
static void heli_mapicon_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    const uint32_t mm = g_mm_minimap;
    g_mm_minimap = 0;
    if (!mm || !api->guest_call)
        return;
    /* the player's own marker is the minimap's image [minimap + 0x90] (FUN_005c6c80 turns it with the heading and
     * colours it); its texture hash at image + 0x24 (FUN_0054ded0 sets it and flags the image changed). While you fly
     * it shows the helicopter icon - turning with you as the arrow did - and the arrow again after. */
    const uint32_t arrow = get_u32(mm + 0x90u);
    if (!arrow)
        return;
    static uint32_t orig, orig_arrow;
    const uint32_t cur = get_u32(arrow + 0x24u);
    if (arrow != orig_arrow) {
        orig_arrow = arrow;
        orig = cur == 0x586874a0u ? 0 : cur;
    }
    if (!orig && cur != 0x586874a0u)
        orig = cur;
    const uint32_t want = g_fly_on ? 0x586874a0u : orig;
    if (!want || cur == want)
        return;
    const uint32_t t[2] = {arrow, want};
    uint32_t r = 0;
    api->guest_call(api, 0x0054ded0u, 0, t, 2, &r);
    char l[100];
    snprintf(l, sizeof l, "core.nfsu: heli: minimap marker %s (texture %08x)", g_fly_on ? "is the helicopter" : "is the arrow again",
             want);
    api->log(api, l);
}
static void heli_pshop_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (getenv("NFSU_HELI_PUSHLOG")) {
        static int n;
        if (n++ < 400) {
            const uint32_t en = get_u32(cpu->esp + 4);
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: heli push: ret %08x vec %08x mycars %08x (%u) fe %08x row %08x",
                     get_u32(cpu->esp), cpu->ecx, get_u32(0x00d9a4a4u), cpu->ecx ? (get_u32(cpu->ecx + 4) - get_u32(cpu->ecx)) / 36u : 0, en ? get_u32(en) : 0,
                     en ? get_u32(en + 0x18) : 0);
            api->log(api, l);
        }
    }
    if (!g_pshop_inside && api->guest_call && !getenv("NFSU_HELI_GARAGE_OFF") && get_u32(cpu->esp) == 0x0052fad6u && cpu->ecx && get_u32(0x00d9a4a4u) &&
        cpu->ecx == get_u32(0x00d9a4a4u) + 0x58u) {
        heli_garage_push(api, cpu);
        return;
    }
    if (g_pshop_inside || get_u32(cpu->esp) != PSHOP_PUSH_RET || g_heli_row_id == 0xffffffffu || !api->guest_call)
        return;
    const uint32_t vec = cpu->ecx, entry = get_u32(cpu->esp + 4);
    if (!vec || !entry || entry == g_pshop_entry)
        return;
    if (getenv("NFSU_HELI_SHOPLOG")) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: heli pshop push into %08x (%u entries) fe %08x row %08x", vec,
                 (get_u32(vec + 4) - get_u32(vec)) / 36u, get_u32(entry), get_u32(entry + 0x18));
        api->log(api, l);
    }
    const uint32_t b = get_u32(vec), e = get_u32(vec + 4);
    if (b && (e < b || (e - b) % 36u || (e - b) / 36u > 64u))
        return;
    /* Already in this fill: it is the last entry (pushes append). Its slot takes the car being pushed and the push
     * pushes the helicopter instead - so it stays last whatever the number of cars. (An AFTER hook that swapped it
     * down never ran here: the push is reached by the plugin's tail jump.) */
    for (uint32_t en = b; b && en < e; en += 36u)
        if (get_u32(en + 0x18) == g_heli_row_id) {
            for (uint32_t k = 0; k < 36u; k += 4) {
                const uint32_t x = get_u32(entry + k);
                api->guest_write_u32(api, en + k, x);
            }
            api->guest_write_u32(api, cpu->esp + 4, g_pshop_entry);
            if (getenv("NFSU_HELI_SHOPLOG"))
                for (uint32_t x = b; x < e; x += 36u) {
                    char l[200];
                    int n = snprintf(l, sizeof l, "core.nfsu: heli pshop entry %u:", (x - b) / 36u);
                    for (uint32_t k = 0; k < 36u && n < 180; k += 4)
                        n += snprintf(l + n, sizeof l - n, " %08x", get_u32(x + k));
                    api->log(api, l);
                }
            return;
        }
    if (!g_pshop_entry && (!api->guest_alloc || api->guest_alloc(api, 48, &g_pshop_entry) != POP_OK || !g_pshop_entry))
        return;
    /* first push of a fill: the pushed car goes in now, the helicopter's entry (a copy of it with the copcar
     * frontend and the helicopter's row) takes this push */
    uint32_t r = 0;
    g_pshop_inside = 1;
    const int ok = api->guest_call(api, 0x0052f570u, vec, &entry, 1, &r) == POP_OK;
    g_pshop_inside = 0;
    if (ok) {
        for (uint32_t k = 0; k < 36u; k += 4)
            api->guest_write_u32(api, g_pshop_entry + k, get_u32(entry + k));
        api->guest_write_u32(api, g_pshop_entry + 0, HELI_ROW_FE);
        api->guest_write_u32(api, g_pshop_entry + 0x18, g_heli_row_id);
        api->guest_write_u32(api, cpu->esp + 4, g_pshop_entry);
    }
    if (getenv("NFSU_HELI_SHOPLOG")) {
        char l[400];
        const uint32_t shop = get_u32(0x00d9a4a0u);
        int n = snprintf(l, sizeof l, "core.nfsu: heli pshop first push: vec %08x shop %08x (+48 %08x) stack:", vec, shop,
                         shop ? shop + 0x48 : 0);
        for (uint32_t k = 0; k < 0x400 && n < 380; k += 4) {
            const uint32_t v = get_u32(cpu->esp + k);
            if (v >= 0x00401000u && v < 0x00bd6000u)
                n += snprintf(l + n, sizeof l - n, " +%x:%08x", k, v);
        }
        api->log(api, l);
    }
    static int said;
    if (said++ < 3)
        api->log(api, ok ? "core.nfsu: heli: the helicopter is in the police shop" :
                           "core.nfsu: heli: the helicopter could NOT be put in the police shop");
}

/* NFSU_HELI_SFXLOG=1 (diagnostic): the sound behaviour and sound object factories, to see the helicopter's made */
static void heli_sfx_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv;
    static unsigned n;
    if (n++ < 20) {
        char l[96];
        snprintf(l, sizeof l, "core.nfsu: heli: sound factory %s called", (const char *)user);
        api->log(api, l);
    }
}

/* The camera stays where the helicopter camera puts it (the person: it jumped to the helicopter and back while
 * flying through an invisible barrier). After the look-at the chase camera (FUN_004deb10, at 0x004e0dd9) pulls
 * the camera in front of whatever lies between it and the car: FUN_004d1bd0 (thiscall, 5 args, RET 0x14) builds
 * the look-at again from a pulled-in eye - past our override, which only rewrites the first look-at. While flying
 * it isn't run (it returns 0, "not moved"); the helicopter camera is high and behind, nothing to pull it from. */
static void heli_no_cam_pull(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    if (g_fly_flying && get_u32(cpu->esp) == 0x004e0ddeu) {
        api->hook_return(api, cpu, 0, 0x14);
        return;
    }
    api->call_next(api, inv, cpu);
}
/* the same for FUN_004ca370 (thiscall, 3 args, RET 0x10, called at 0x004e0dad): it too makes the look-at again
 * (at 0x004ca43f) when the camera is blocked; its result (0, "not blocked") goes to [camera + 0x151] */
static void heli_no_cam_block(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    if (g_fly_flying && get_u32(cpu->esp) == 0x004e0db2u) {
        api->hook_return(api, cpu, 0, 0x10);
        return;
    }
    api->call_next(api, inv, cpu);
}

/* No car shadow under the helicopter (the person: remove the wide static shadow under it). Each car's render info
 * draws a soft blob under the body with texture CARSHADOW ([info + 0x444], wheel blobs WHEELSHADOW [info + 0x448],
 * both set by FUN_0087a230) in FUN_00851d50 (thiscall, ECX the render info, arg 2 the car's position in render
 * coordinates; it skips the blob when the texture is 0). The car whose position is the helicopter's gets both
 * textures zeroed for that draw and put back right after (AFTER hook): other cars keep theirs. The rotor's own
 * shadow from the sun stays. */
static uint32_t g_shadow_info, g_shadow_tex[2];
static void heli_shadow_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    g_shadow_info = 0;
    if (!g_fly_on || getenv("NFSU_HELI_SHADOW"))
        return;
    const uint32_t info = cpu->ecx, pos = get_u32(cpu->esp + 8);
    float hp[3];
    const uint32_t rb = heli_rb(heli_player_simable());
    if (info < 0x01400000u || pos < 0x00400000u || !rb || !heli_rb_pos(rb, hp))
        return;
    const float dx = tp_f32(pos) - hp[2], dy = tp_f32(pos + 4) + hp[0], dz = tp_f32(pos + 8) - hp[1];
    if (getenv("NFSU_HELI_LOG")) {
        static unsigned n;
        if (n++ % 2000 == 0) {
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: heli: car shadow draw %.1f m from the helicopter", (double)sqrtf(dx * dx + dy * dy + dz * dz));
            api->log(api, l);
        }
    }
    if (!(dx * dx + dy * dy + dz * dz < 36.0f)) /* within 6 m: the draw position is the body's base, ~3.5 m off */
        return;
    g_shadow_info = info;
    g_shadow_tex[0] = get_u32(info + 0x444), g_shadow_tex[1] = get_u32(info + 0x448);
    static int said;
    if (!said) {
        said = 1;
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: heli: its car shadow is not drawn (render info %08x, texture %08x)", info,
                 g_shadow_tex[0]);
        api->log(api, l);
    }
    api->guest_write_u32(api, info + 0x444, 0);
    api->guest_write_u32(api, info + 0x448, 0);
}
static void heli_shadow_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (!g_shadow_info)
        return;
    api->guest_write_u32(api, g_shadow_info + 0x444, g_shadow_tex[0]);
    api->guest_write_u32(api, g_shadow_info + 0x448, g_shadow_tex[1]);
    g_shadow_info = 0;
}

/* NFSU_HELI_MODELSCAN=1 (diagnostic): where the helicopter's model key sits in the career object (saved car records) */
static void heli_model_scan(const PopModApi *api) {
    static int done;
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (done || !obj || !g_heli_model[0] || g_heli_look == 1 || !getenv("NFSU_HELI_MODELSCAN"))
        return;
    done = 1;
    int n = 0;
    for (uint32_t a = 0x00d1f000u; a < 0x30000000u && n < 60; a += 4) {
        if (a == 0x01408000u)
            a = 0x01940000u;
        const uint32_t v = get_u32(a);
        if (v != g_heli_model[0] && v != g_heli_host_orig[0])
            continue;
        char l[200];
        snprintf(l, sizeof l, "core.nfsu: heli scan: %s at %08x: %08x %08x %08x [%08x] %08x %08x %08x",
                 v == g_heli_model[0] ? "HELI" : "SUV", a, get_u32(a - 12), get_u32(a - 8),
                 get_u32(a - 4), v, get_u32(a + 4), get_u32(a + 8), get_u32(a + 12));
        api->log(api, l);
        ++n;
    }
    for (uint32_t a = obj; a < obj && n < 40; a += 4)
        if (get_u32(a) == g_heli_model[0]) {
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: heli scan: model key at obj+%x (next %08x, prev %08x %08x)", a - obj,
                     get_u32(a + 4), get_u32(a - 8), get_u32(a - 4));
            api->log(api, l);
            ++n;
        }
    for (uint32_t a = obj; a < obj + 0x80000u && n < 80; a += 4)
        if (get_u32(a) == g_heli_host_orig[0]) {
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: heli scan: SUV model key at obj+%x (next %08x, prev %08x %08x)", a - obj,
                     get_u32(a + 4), get_u32(a - 8), get_u32(a - 4));
            api->log(api, l);
            ++n;
        }
    char l[80];
    snprintf(l, sizeof l, "core.nfsu: heli scan: done (%d hits)", n);
    api->log(api, l);
}
/* The purchase dialog ("Купить Полицейский внедорожник за 0 $?", 2026-11-24): the Police Career plugin names its
 * five police cars from its table 0x101ae140 (5 x 20 bytes {pvehicle, preset, frontend, text key, default}) but
 * copies each name ONCE, in the game's own text encoding, into 128-byte buffers of its role object
 * (FUN_101348a0, thiscall [0x101d69a0] + 0x20: +0xac + 0x80 x car). The helicopter is car 2's (the SUV's)
 * pvehicle, so while a helicopter row is highlighted in a shop or garage (g_fe_heli) car 2's buffer holds the
 * helicopter's name, else the plugin's own (saved the first time, only if it reads as the SUV's name). */
static void heli_plugin_name(const PopModApi *api) {
    static const uint8_t suv[] = {0xa2, 0x81, 0x8a, 0x83, 0xb2, 0x84, 0x97, 0x88, 0x8b, 0x83, 0x97, 0x20, 0x89, 0x85};
    static const char heli[] = "\xa2\x81\x8a\x83\xb2\x84\x97\x88\x8b\x83\x97 \x89\x84\x87\x86\x81\x8a\x84\x86";
    static uint8_t orig[128];
    static uint32_t saved_at;
    static int shown = -1;
    const uint32_t pr = get_u32(0x101d69a0u);
    if (!pr || get_u32(0x101ae140u + 40u) != 0xc1733734u || g_heli_host != 1)
        return;
    const uint32_t buf = pr + 0x20u + 0xacu + 0x80u * 2u;
    if (saved_at != buf) { /* a new role object (another save): take its name if it is the SUV's */
        int m = 1;
        for (unsigned k = 0; k < sizeof suv && m; ++k)
            m = get_u8(buf + k) == suv[k];
        if (!m)
            return;
        for (uint32_t k = 0; k < 128u; ++k)
            orig[k] = get_u8(buf + k);
        saved_at = buf, shown = 0;
    }
    const int on = g_fe_heli == 1 && !tp_driving();
    if (on == shown && !(on && get_u8(buf + 12) == suv[12] && get_u8(buf + 13) == suv[13]))
        return; /* (the plugin may copy its names again: then the helicopter's goes back in) */
    for (uint32_t k = 0; k < 128u; k += 4) {
        uint32_t v = 0;
        for (uint32_t j = 0; j < 4; ++j)
            v |= (uint32_t)(on ? (k + j < sizeof heli ? (uint8_t)heli[k + j] : 0) : orig[k + j]) << (8 * j);
        api->guest_write_u32(api, buf + k, v);
    }
    shown = on;
    if (getenv("NFSU_HELI_SHOPLOG"))
        api->log(api, on ? "core.nfsu: heli: the plugin's SUV name now reads the helicopter's"
                         : "core.nfsu: heli: the plugin's SUV name is its own again");
}
static void heli_pass(const PopModApi *api) {
    heli_plugin_name(api);
    if (tp_driving())
        heli_model_scan(api);
    static int64_t host_at;
    if (now_ns() - host_at > 2000000000LL) {
        host_at = now_ns();
        heli_host_apply(api);
    }
    /* tests: NFSU_HELI_MODEL=1 makes the career's active car (whatever it is) the helicopter */
    if (getenv("NFSU_HELI_MODEL")) {
        static int done;
        const uint32_t pv = done ? 0 : heli_active_pvehicle(api);
        if (pv && get_u32(EO_GAME_STATE) != 6) {
            const uint32_t pk = attrib_key("pvehicle"), mk = attrib_key("MODEL");
            const uint32_t mine = attrib_collection(pk, pv), heli = attrib_collection(pk, attrib_key("cop_hel_fed_01"));
            int w1 = -1, w2 = -1;
            const uint32_t n1 = mine ? attrib_node(mine, mk, &w1) : 0, n2 = heli ? attrib_node(heli, mk, &w2) : 0;
            if (n1 && n2) {
                const uint32_t a1 = attrib_value_addr(mine, n1), a2 = attrib_value_addr(heli, n2);
                api->guest_write_u32(api, a1, get_u32(a2));
                api->guest_write_u32(api, a1 + 4, get_u32(a2 + 4));
                api->log(api, "core.nfsu: heli: test: the active car is drawn as the helicopter");
                heli_give_sound(api, mine);
            }
            done = 1;
        }
    }
    {
        const uint32_t pv = heli_active_pvehicle(api);
        const int was = g_fly_on;
        (void)pv;
        heli_row_ensure(api);
        const uint32_t active = g_heli_row_id != 0xffffffffu ? pc_active_car(api) : 0xffffffffu;
        g_fly_on = heli_row_is(active) || getenv("NFSU_HELI_MODEL") != NULL;
        /* In a shop or garage the SUV's look is the helicopter's ONLY while the helicopter's own row is highlighted
         * (2026-11-23, the person: "three police SUVs, two of them helicopters"): with the helicopter as the active
         * car the look stayed on there, and the garage built its real SUV rows as helicopters. Driving or loading
         * (no shop/garage object): the active car decides. */
        const int fe_open = get_u32(0x00d9a4a0u) || get_u32(0x00d9a478u) || get_u32(0x00d9a4a4u);
        if (tp_driving() || !fe_open)
            g_fe_heli = -1;
        heli_look_set(api, (tp_driving() || !fe_open) ? heli_row_is(active)
                                                     : g_fe_heli == 1);
        { /* test: NFSU_HELI_LEAVETEST=<s> - from that run time on, as if a car had been chosen instead */
            const char *lt = getenv("NFSU_HELI_LEAVETEST");
            if (lt && now_ns() - g_tp_start_ns > (int64_t)atoi(lt) * 1000000000LL)
                g_fly_on = 0;
        }
        if (g_fly_on != was)
            api->log(api, g_fly_on ? "core.nfsu: heli: you are in the helicopter (Space/E take off)" :
                                     "core.nfsu: heli: you are in a car");
        /* back in a car after the helicopter: the game's own "reset car" (EResetPlayerCar, factory FUN_0066c340,
         * what the pause menu's reset uses) puts it in the middle of the nearest road, once the new car is there */
        /* the helicopter's body while it was one, to see the new car arrive (a different body). The wait counts
         * driving time only: leaving police mode reloads the world, which took 11 s in the person's log (the old
         * 8 s wall-clock limit ran out first, so the car fell from the helicopter's height). */
        static uint32_t heli_body;
        static int pending;
        static int64_t driving_since;
        if (g_fly_on)
            heli_body = heli_player_simable();
        if (was && !g_fly_on) {
            pending = 1;
            driving_since = 0;
        }
        if (pending) {
            const uint32_t body = heli_player_simable();
            const int new_car = body && body != heli_body;
            if (!tp_driving())
                driving_since = 0;
            else {
                if (!driving_since)
                    driving_since = now_ns();
                const int64_t d = now_ns() - driving_since;
                if (new_car || d > 1000000000LL) {
                    pending = 0;
                    heli_back_on_road(api);
                } else
                    heli_hold_still(api);
            }
        }
    }
    static int keyed;
    if (!keyed && api->on_key) {
        uint32_t id;
        keyed = api->on_key(api, heli_fly_key, NULL, &id) == POP_OK ? 1 : -1;
        static uint32_t lh, rh;
        if (api->hook_install(api, 0x00720310u, heli_no_reset, POP_HOOK_REPLACE, NULL, &rh) != POP_OK)
            api->log(api, "core.nfsu: heli: reset guard NOT installed");
        if (api->hook_install(api, 0x00749920u, heli_lookat, POP_HOOK_BEFORE, NULL, &lh) != POP_OK)
            api->log(api, "core.nfsu: heli: camera hook NOT installed");
        static uint32_t sq1, sq2;
        if (api->hook_install(api, 0x00519850u, heli_shop_before, POP_HOOK_BEFORE, NULL, &sq1) != POP_OK ||
            api->hook_install(api, 0x00519850u, heli_shop_after, POP_HOOK_AFTER, NULL, &sq2) != POP_OK)
            api->log(api, "core.nfsu: heli: shop hooks NOT installed");
        static uint32_t ps1, ps2;
        (void)ps2;
        static uint32_t gq1, gq2;
        if (api->hook_install(api, 0x00519850u, heli_gq_before, POP_HOOK_BEFORE, NULL, &gq1) != POP_OK ||
            api->hook_install(api, 0x00519850u, heli_gq_after, POP_HOOK_AFTER, NULL, &gq2) != POP_OK)
            api->log(api, "core.nfsu: heli: garage name hooks NOT installed");
        static uint32_t fe1, fe2, fe3;
        if (api->hook_install(api, 0x005aa390u, heli_fe_shop, POP_HOOK_BEFORE, NULL, &fe1) != POP_OK ||
            api->hook_install(api, 0x0058d530u, heli_fe_garage, POP_HOOK_BEFORE, NULL, &fe2) != POP_OK ||
            api->hook_install(api, 0x005aa740u, heli_fe_mycars, POP_HOOK_BEFORE, NULL, &fe3) != POP_OK)
            api->log(api, "core.nfsu: heli: car preview hooks NOT installed (the shop shows the SUV)");
        if (getenv("NFSU_HELI_SHOWLOG")) {
            static uint32_t sl;
            api->hook_install(api, 0x005feef0u, heli_fe_showlog, POP_HOOK_BEFORE, NULL, &sl);
        }
        if (!getenv("NFSU_HELI_MAPICON") || atoi(getenv("NFSU_HELI_MAPICON"))) {
            static uint32_t mi1, mi2;
            if (api->hook_install(api, 0x005cca20u, heli_mapicon_before, POP_HOOK_BEFORE, NULL, &mi1) != POP_OK ||
                api->hook_install(api, 0x005cca20u, heli_mapicon_after, POP_HOOK_AFTER, NULL, &mi2) != POP_OK)
                api->log(api, "core.nfsu: heli: minimap icon hooks NOT installed");
        }
        static uint32_t os1, os2;
        if (api->hook_install(api, 0x0051df10u, heli_onsale_before, POP_HOOK_BEFORE, NULL, &os1) != POP_OK ||
            api->hook_install(api, 0x0051df10u, heli_onsale_after, POP_HOOK_AFTER, NULL, &os2) != POP_OK)
            api->log(api, "core.nfsu: heli: on-sale hooks NOT installed");
        static uint32_t so1;
        if (api->hook_install(api, 0x005a1b60u, heli_sort_after, POP_HOOK_AFTER, NULL, &so1) != POP_OK)
            api->log(api, "core.nfsu: heli: shop sort hooks NOT installed (the helicopter may not be last)");
        if (api->hook_install(api, 0x0052f570u, heli_pshop_before, POP_HOOK_BEFORE, NULL, &ps1) != POP_OK)
            api->log(api, "core.nfsu: heli: police shop hooks NOT installed");
        static uint32_t nh1, nh2;
        const int r1 = api->hook_install(api, 0x00525070u, heli_name_label, POP_HOOK_AFTER, NULL, &nh1);
        const int r2 = api->hook_install(api, 0x00571b80u, heli_text, POP_HOOK_AFTER, NULL, &nh2);
        if (r1 != POP_OK || r2 != POP_OK) {
            char l[100];
            snprintf(l, sizeof l, "core.nfsu: heli: name hooks NOT installed (%d, %d)", r1, r2);
            api->log(api, l);
        }
        static uint32_t ph;
        if (api->hook_install(api, 0x004d1bd0u, heli_no_cam_pull, POP_HOOK_REPLACE, NULL, &ph) != POP_OK)
            api->log(api, "core.nfsu: heli: camera pull hook NOT installed");
        static uint32_t sh1, sh2;
        if (api->hook_install(api, 0x00851d50u, heli_shadow_before, POP_HOOK_BEFORE, NULL, &sh1) != POP_OK ||
            api->hook_install(api, 0x00851d50u, heli_shadow_after, POP_HOOK_AFTER, NULL, &sh2) != POP_OK)
            api->log(api, "core.nfsu: heli: shadow hooks NOT installed");
        static uint32_t bh;
        if (api->hook_install(api, 0x004ca370u, heli_no_cam_block, POP_HOOK_REPLACE, NULL, &bh) != POP_OK)
            api->log(api, "core.nfsu: heli: camera block hook NOT installed");
        if (getenv("NFSU_HELI_FILTERLOG")) {
            static uint32_t fh;
            api->hook_install(api, 0x006d7bd0u, heli_filter_log, POP_HOOK_BEFORE, NULL, &fh);
        }
        if (getenv("NFSU_HELI_SFXLOG")) {
            static uint32_t sh[3];
            api->hook_install(api, 0x00737150u, heli_sfx_log, POP_HOOK_BEFORE, (void *)"SoundHelicopter", &sh[0]);
            api->hook_install(api, 0x00736fc0u, heli_sfx_log, POP_HOOK_BEFORE, (void *)"SoundRacer", &sh[1]);
            api->hook_install(api, 0x0048e970u, heli_sfx_log, POP_HOOK_BEFORE, (void *)"SFXObj_Helicopter", &sh[2]);
        }
    }
    heli_fly_pass(api);
    stream_dump(api);
    {
        float sp[3] = {0, 0, 0};
        const uint32_t srb = g_fly_flying ? heli_rb(heli_player_simable()) : 0;
        const int fl = srb && heli_rb_pos(srb, sp);
        stream_pass(api, fl, sp[2], -sp[0], cosf(g_cam_h), -sinf(g_cam_h), sp[1]);
        stream_draw_build();
        /* test: NFSU_HELI_TELEPORT="x,y,z@s" puts the flying helicopter at that sim position at that run time */
        const char *tp = getenv("NFSU_HELI_TELEPORT");
        static int tp_done;
        float tx, ty, tz;
        int tat;
        if (tp && !tp_done && srb && sscanf(tp, "%f,%f,%f@%d", &tx, &ty, &tz, &tat) == 4 &&
            now_ns() - g_tp_start_ns > (int64_t)tat * 1000000000LL) {
            tp_done = 1;
            const float np[3] = {tx, ty, tz};
            heli_rb_set(api, srb, 22, np);
            g_fly_hold_y = ty;
            api->log(api, "core.nfsu: heli: test teleport");
        }
    }
    if (g_road_check_ns && now_ns() > g_road_check_ns) {
        g_road_check_ns = 0;
        float p[3] = {0, 0, 0};
        const uint32_t rb = heli_rb(heli_player_simable());
        if (rb && heli_rb_pos(rb, p)) {
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: heli: 1.5 s after the reset the car is at %.1f %.1f %.1f", (double)p[0],
                     (double)p[1], (double)p[2]);
            api->log(api, l);
        }
    }
    {
        float hp[3];
        const uint32_t hrb = g_fly_flying ? heli_rb(heli_player_simable()) : 0;
        heli_haze_pass(hrb && heli_rb_pos(hrb, hp) ? hp[1] - g_fly_ground : 0.0f);
    }
    { /* test: NFSU_HELI_ROADTEST=<s> does at that run time what leaving the helicopter does (the flight ends) */
        static int done;
        const char *t = getenv("NFSU_HELI_ROADTEST");
        if (t && !done && now_ns() - g_tp_start_ns > (int64_t)atoi(t) * 1000000000LL) {
            done = 1;
            g_fly_flying = 0;
            heli_back_on_road(api);
        }
    }
}
