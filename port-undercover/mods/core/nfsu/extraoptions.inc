#include <dlfcn.h>
/* ---- ExtraOptions, built in (2026-11-19) ----
 * The NFSUC ExtraOptions plugin was made for this very exe (1.0.0.1, 10,584,064 bytes). What it changes was found
 * by running its start-up under an emulator beside the exe's image and recording every byte it wrote (a tool kept
 * outside the repository), once per ini key; its hotkey routine was read from its code. Built in here, switched
 * live in F10's Mods tab:
 * - Collector's Edition: the game's own check stores its answer in [0x00daa166] (FUN_00525xxx, setne dl at
 *   0x0052555e); the plugin forces that answer to 1. Here the byte is held at 1 while the setting is on and given
 *   back when it goes off.
 * - No reverse on a double tap of the brake: the game compares the time between taps with 0.5 s (0x00bd90f8, read at
 *   0x00428dc5); the plugin points that read at a 0. game.toml redirects the read to a slot set here.
 * - Hotkeys, the plugin's defaults: F5 unlock everything, F6 auto drive, F7 the heat level hack (then Page Up the
 *   next heat level, Scroll Lock cops on/off), O the car's emergency lights, Pause freezes the camera. As in the
 *   plugin they work while driving (game state 6, not paused). Headlights (H) is not built in: the plugin rewrites
 *   game code for it at run time. */
#define EO_SLOT_TAP 0x01375810u
#define EO_CE_FLAG 0x00daa166u
#define EO_UNLOCK_FLAG 0x00daa15au
#define EO_FREEZE_CAM 0x00d89950u
#define EO_GAME_STATE 0x00da57b8u
#define EO_HEAT_MIN 0x00d1f0c8u
#define EO_HEAT_MAX 0x00d1f0ccu

/* Keys (2026-11-20, the person's MacBook has no F-key row without fn, no Page Up, Scroll Lock or Pause): 6 everything
 * unlocked, 7 autopilot, 8 choose the heat level, 9 next heat level, 0 police in the city, O emergency lights, -
 * freeze the camera. Each key flips its F10 switch (Mods page, "Extra Options"), and the switches are what acts. */
static volatile int g_eo_pending[8]; /* 0 unlock, 1 autopilot, 2 heat hack, 3 next heat, 4 cops, 5 lights, 6 camera */
/* Police Career (2026-11-20, the person lost the racer and could not leave the pursuit): H, the plugin's emergency
 * lights, pressed again while its pursuit controller ([0x101d69a0] + 0x13e4, state dword: 1 invitation, 2 dispatch,
 * 3 pursuit) is chasing ends that pursuit (nfsu.c police_career_pass). The state is taken when the key goes down,
 * before the plugin reads the key, so the press that STARTS a pursuit (state 1) is never taken for one that ends it. */
static volatile int g_pc_end_chase;
static int32_t eo_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)api, (void)dik, (void)user;
    if (!down)
        return 0;
    if ((vk >= 0x30 && vk <= 0x39) || vk == 0x4f || vk == 0xbd) { /* 2026-11-21: a hang after 9/0 on the map */
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: key %02x down (game state %u, paused byte %u)", (unsigned)vk,
                 get_u32(EO_GAME_STATE), get_u8(0x00deb99au));
        api->log(api, l);
    }
    switch (vk) {
    case 0x36: g_eo_pending[0] = 1; break; /* 6 */
    case 0x37: g_eo_pending[1] = 1; break; /* 7 */
    case 0x38: g_eo_pending[2] = 1; break; /* 8 */
    case 0x39: g_eo_pending[3] = 1; break; /* 9 */
    case 0x30: g_eo_pending[4] = 1; break; /* 0 */
    case 0x4f: g_eo_pending[5] = 1; break; /* O */
    case 0xbd: g_eo_pending[6] = 1; break; /* - */
    case 0x48: { /* H */
        const uint32_t pr = get_u32(0x101d69a0u);
        if (pr && get_u8(pr + 0x20u + 0x18u) == 1) {
            const uint32_t st = get_u32(pr + 0x13e4u);
            if (st == 2 || st == 3)
                g_pc_end_chase = 1;
        }
        break;
    }
    default: break;
    }
    return 0; /* the game sees the key as before */
}

static int64_t eo_setting(const PopModApi *api, const char *key, int64_t dflt) {
    int64_t v = dflt;
    if (api->settings_get)
        api->settings_get(api, key, &v);
    return v;
}
static uint32_t eo_call(const PopModApi *api, uint32_t fn, uint32_t ecx, const uint32_t *args, uint32_t n) {
    uint32_t r = 0;
    if (api->guest_call)
        api->guest_call(api, fn, ecx, args, n, &r);
    return r;
}
/* a message line the way the plugin shows them (the game's FUN_00661bb0) */
static void eo_message(const PopModApi *api, const char *text) {
    /* 2026-11-25 (release check, every mod on: a call to address 0 from 0x00661c71 when a save loaded): the game's
     * message function reads the player's HUD through [[0x00de99dc]] when [0x00de99e4] is set and otherwise calls
     * through a null pointer. A switch that a newly loaded save turned on ("Everything is unlocked.") asked for a
     * message before the player existed. No player, no message. */
    {
        const uint32_t list = get_u32(0x00de99dcu);
        const uint32_t pl = get_u32(0x00de99e4u) && list ? get_u32(list) : 0;
        if (!pl || !get_u32(pl) || !get_u32(get_u32(pl) + 0x1cu))
            return;
    }
    static uint32_t buf;
    if (!buf && (!api->guest_alloc || api->guest_alloc(api, 128, &buf) != POP_OK))
        return;
    char t[128];
    snprintf(t, sizeof t, "^%s", text);
    for (uint32_t i = 0; i < sizeof t; i += 4) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4; ++b)
            w |= (uint32_t)(uint8_t)t[i + b] << (8 * b);
        api->guest_write_u32(api, buf + i, w);
        if (memchr(t + i, 0, 4))
            break;
    }
    const uint32_t a[2] = {buf, 0};
    eo_call(api, 0x00661bb0u, 0, a, 2);
}
/* the player's vehicle: the one whose simulation object is the local player's (as the plugin finds it) */
/* The car whose light bits FUN_006eafd0 / FUN_006cc110 / FUN_006cc120 read and set ([this + 0x9c], 0xf000 = the
 * emergency lights): the player's vehicle's IVehicle interface (vtable 0x00c19594), as the Police Career plugin
 * passes it (its FUN_1014c690). 2026-11-24: the first entry of the player vehicle list [0x00de93d4] (the
 * helicopter code's lookup too); the old search below, from the Extra Options trace, found nothing in the person's
 * game ("your car was not found"), so the lights never switched. */
static uint32_t eo_player_car(const PopModApi *api) {
    if (get_u32(0x00de93dcu)) {
        const uint32_t iv = get_u32(get_u32(0x00de93d4u));
        if (iv >= 0x01400000u && get_u32(iv) == 0x00c19594u)
            return iv;
    }
    const uint32_t n = get_u32(0x00de93dcu), pl = get_u32(0x00de99e8u);
    if (!pl || n > 64)
        return 0;
    const uint32_t ps = eo_call(api, get_u32(get_u32(pl) + 4), pl, NULL, 0);
    for (uint32_t i = 0; i < n && ps; ++i) {
        const uint32_t v = get_u32(0x00de93e0u + 4 * i);
        if (v && eo_call(api, get_u32(get_u32(v) + 4), v, NULL, 0) == ps)
            return v;
    }
    return 0;
}

/* Driving = game state 6, not in the game's pause (byte and FUN_00521960), and the world ticking (0x00d3c108 moved in
 * the last 300 ms). The world map and the menus keep state 6 but stop the world, so they are not driving
 * (2026-11-21: 9, 0 and - on the map are the map's own keys and flipped these switches too; - froze the camera and
 * the garage menu then showed the sky). */
static int eo_driving_now(const PopModApi *api) {
    return get_u32(EO_GAME_STATE) == 6 && get_u8(0x00deb99au) == 0 && now_ns() - g_world_tick_ns < 300000000LL &&
           (eo_call(api, 0x00521960u, 0, NULL, 0) & 0xffu) == 0;
}

static void extraoptions_pass(const PopModApi *api) {
    static int init, key_hooked;
    static uint8_t ce_orig, unlock_orig;
    static int ce_held, unlock_held;
    if (!key_hooked && api->on_key) {
        uint32_t id;
        key_hooked = api->on_key(api, eo_key, NULL, &id) == POP_OK ? 1 : -1;
    }
    (void)init;
    /* Collector's Edition */
    if (eo_setting(api, "mod_eo_collectors", 1)) {
        if (!ce_held)
            ce_orig = (uint8_t)get_u8(EO_CE_FLAG), ce_held = 1;
        if (get_u8(EO_CE_FLAG) != 1)
            api->guest_write_u8(api, EO_CE_FLAG, 1);
    } else if (ce_held) {
        api->guest_write_u8(api, EO_CE_FLAG, ce_orig);
        ce_held = 0;
    }
    /* the brake double-tap window */
    {
        const uint32_t want = eo_setting(api, "mod_eo_no_tap_reverse", 1) ? 0u : get_u32(0x00bd90f8u);
        if (get_u32(EO_SLOT_TAP) != want)
            api->guest_write_u32(api, EO_SLOT_TAP, want);
    }
    /* keys flip the switches (only while "Keys for these options" is on) */
    static const char *const flip[7] = {"mod_eo_unlock_all", "mod_eo_autodrive", "mod_eo_heat_hack", NULL, "mod_eo_cops",
                                        "mod_eo_lights", "mod_eo_freeze_camera"};
    static const int flip_dflt[7] = {0, 0, 0, 0, 1, 0, 0};
    const int hot = (int)eo_setting(api, "mod_eo_hotkeys", 1);
    int any_key = 0;
    for (int i = 0; i < 7; ++i)
        any_key |= g_eo_pending[i];
    if (any_key && !eo_driving_now(api)) { /* the keys act only while driving, nowhere else (the person's wish) */
        for (int i = 0; i < 7; ++i)
            g_eo_pending[i] = 0;
        static int said;
        if (said++ < 20)
            api->log(api, "core.nfsu: extra options key ignored: not driving");
    }
    for (int i = 0; i < 7; ++i) {
        if (!g_eo_pending[i])
            continue;
        g_eo_pending[i] = 0;
        if (!hot || !api->settings_set)
            continue;
        {
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: extra options key %d flips its switch (game state %u)", i,
                     get_u32(EO_GAME_STATE));
            api->log(api, l);
        }
        if (i == 3) {
            const int64_t lv = eo_setting(api, "mod_eo_heat_level", 1);
            api->settings_set(api, "mod_eo_heat_level", lv >= 5 ? 1 : lv + 1);
        } else
            api->settings_set(api, flip[i], !eo_setting(api, flip[i], flip_dflt[i]));
    }
    /* the switches that last only for this run start off */
    if (!init) {
        init = 1;
        if (api->settings_set)
            for (int i = 1; i < 7; ++i)
                if (i != 3 && i != 4 && eo_setting(api, flip[i], 0))
                    api->settings_set(api, flip[i], 0);
    }
    /* everything unlocked */
    if (eo_setting(api, "mod_eo_unlock_all", 0)) {
        if (!unlock_held)
            unlock_orig = (uint8_t)get_u8(EO_UNLOCK_FLAG), unlock_held = 1;
        if (get_u8(EO_UNLOCK_FLAG) != 1)
            api->guest_write_u8(api, EO_UNLOCK_FLAG, 1);
    } else if (unlock_held) {
        api->guest_write_u8(api, EO_UNLOCK_FLAG, unlock_orig);
        unlock_held = 0;
    }
    /* MONEY (value 6006 in the game's value store 0x00d2a178: FUN_00519540 reads, FUN_005195a0 sets; group 6 = 0
     * when no career is loaded). The F10 row mod_eo_cash (an entry field) SHOWS the career's money as it is now
     * (synced every 0.5 s; the person 2026-11-22: "why doesn't it read how much I have before I edit it?") and a
     * typed amount sets it exactly, whatever "Everything unlocked" is (their change "did nothing" when that was off).
     * While "Everything unlocked" is on the money is topped back up to the last amount set (10 000 000 at first,
     * the game's own producer-demo amount, FUN_005e76b0). Only a change of the row that this code did not write
     * counts as the person's. */
    {
        static uint32_t cash_buf;
        static int64_t cash_at, written = -1, target = 10000000;
        const int64_t t = now_ns();
        if (t - cash_at > 500000000LL && get_u32(0x00d2a178u + 8u + 6u * 4u) && api->settings_set &&
            (cash_buf || (api->guest_alloc && api->guest_alloc(api, 16, &cash_buf) == POP_OK && cash_buf))) {
            cash_at = t;
            api->guest_write_u32(api, cash_buf, 0);
            const uint32_t ga[5] = {0x1776u, cash_buf, 0xffffffffu, 0xffffffffu, 0xffffffffu};
            /* 2026-11-25 (release check: loading the save "Player1" set its money from 5 000 to 11 000): each save keeps
             * its own copy of this row, so a switch of save (or of the career object) brought another number into it,
             * and that looked like the person typing. A new save or career first adopts the game's money. */
            {
                static uint32_t career_seen;
                static char save_seen[96];
                typedef const char *(*save_fn)(void);
                static save_fn active_save;
                static int looked;
                if (!looked)
                    looked = 1, active_save = (save_fn)dlsym(RTLD_DEFAULT, "mods_settings_active_save");
                const char *sv = active_save ? active_save() : "";
                const uint32_t career = get_u32(0x00d2a178u + 8u + 6u * 4u);
                if (career != career_seen || strncmp(sv ? sv : "", save_seen, sizeof save_seen - 1)) {
                    career_seen = career;
                    snprintf(save_seen, sizeof save_seen, "%s", sv ? sv : "");
                    written = -1;
                }
            }
            if (eo_call(api, 0x00519540u, 0x00d2a178u, ga, 5)) {
                int64_t cash = (int32_t)get_u32(cash_buf);
                int64_t v = eo_setting(api, "mod_eo_cash", 10000000);
                v = v < 0 ? 0 : v > 999999999 ? 999999999 : v;
                const char *why = NULL;
                int typed = 0;
                if (written >= 0 && v != written) { /* the person typed an amount */
                    target = v;
                    typed = 1;
                    why = "the amount was changed";
                } else if (eo_setting(api, "mod_eo_unlock_all", 0) && cash < target) {
                    why = "topped up";
                }
                if (why) {
                    const int64_t want = typed ? v : target;
                    const uint32_t sa[5] = {0x1776u, (uint32_t)want, 0xffffffffu, 0xffffffffu, 0xffffffffu};
                    eo_call(api, 0x005195a0u, 0x00d2a178u, sa, 5);
                    char l[120];
                    snprintf(l, sizeof l, "core.nfsu: career money %d -> %d (%s)", (int)cash, (int)want, why);
                    if (typed || getenv("NFSU_EO_LOG"))
                        api->log(api, l);
                    cash = want;
                }
                if (cash != v) {
                    api->settings_set(api, "mod_eo_cash", cash);
                    static unsigned said;
                    if (said++ < 20 || getenv("NFSU_EO_LOG")) {
                        char l[96];
                        snprintf(l, sizeof l, "core.nfsu: money row shows the career's money: %d", (int)cash);
                        api->log(api, l);
                    }
                }
                written = cash;
            }
        }
    }
    static int unlock_said = -1;
    {
        const int u = (int)eo_setting(api, "mod_eo_unlock_all", 0);
        if (unlock_said >= 0 && u != unlock_said)
            eo_message(api, u ? "Everything is unlocked." : "Unlocking everything is off.");
        unlock_said = u;
    }
    /* the camera: the game's own freeze flag, held to the switch (outside the game's pause) */
    {
        static int cam_held;
        const int want = eo_setting(api, "mod_eo_freeze_camera", 0) != 0;
        if (want && get_u8(0x00deb99au) == 0 && eo_driving_now(api)) {
            if (get_u8(EO_FREEZE_CAM) != 1)
                api->guest_write_u8(api, EO_FREEZE_CAM, 1);
            cam_held = 1;
        } else if (cam_held) { /* menus, the map, a pause or the switch off: the camera is the game's again */
            api->guest_write_u8(api, EO_FREEZE_CAM, 0);
            cam_held = 0;
            if (want && api->settings_set) /* left driving: the freeze ends, like the other run-only switches */
                api->settings_set(api, "mod_eo_freeze_camera", 0);
        }
    }
    /* the rest acts while driving: game state 6, the game not in its pause (FUN_00521960 says 0) */
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 250000000LL)
        return;
    at = t;
    const int driving = eo_driving_now(api);
    static int autodrive, heat_hack, cops_on = 1, lights, heat_set;
    /* 2026-11-25 (release check: autopilot on, then police mode - the Police Career menu took no key at all, Return
     * and Escape both ignored, because the game's demo driver owns the input): leaving the world for the front end
     * (game state not 6 - garages, shops, the plugin's menus; the pause menu stays state 6 and works) ends the
     * autopilot, and its switch goes off with it. */
    if (autodrive && get_u32(EO_GAME_STATE) != 6) {
        const uint32_t one = 1;
        eo_call(api, 0x00663290u, 0, &one, 1);
        autodrive = 0;
        if (api->settings_set)
            api->settings_set(api, "mod_eo_autodrive", 0);
        api->log(api, "core.nfsu: autopilot off: the game left driving for its menus");
    }
    if (!driving)
        return;
    const int want_auto = (int)eo_setting(api, "mod_eo_autodrive", 0);
    if (want_auto != autodrive) {
        autodrive = want_auto;
        const uint32_t one = 1;
        eo_call(api, autodrive ? 0x00663220u : 0x00663290u, 0, &one, 1);
        eo_message(api, autodrive ? "Autopilot on." : "Autopilot off.");
    }
    const int want_hack = (int)eo_setting(api, "mod_eo_heat_hack", 0);
    if (want_hack != heat_hack) {
        heat_hack = want_hack;
        heat_set = 0;
        if (heat_hack) {
            api->guest_write_u32(api, EO_HEAT_MIN, 1);
            api->guest_write_u32(api, EO_HEAT_MAX, 5);
        }
        eo_message(api, heat_hack ? "Ultimate Force Heat Level hack is enabled."
                                  : "Ultimate Force Heat Level hack is disabled.");
    }
    if (heat_hack) {
        const int lv = (int)eo_setting(api, "mod_eo_heat_level", 1);
        /* the player's pursuit heat, as the plugin sets it: [0x00de99dc] -> its first object's second virtual
         * -> [+4] -> FUN_005f6700(0x00402240) -> its third virtual (the heat as a float); set again when the level
         * changes or a new pursuit object appears */
        uint32_t p = get_u32(0x00de99dcu);
        if (get_u32(0x00de99e4u) && p && (p = get_u32(p)) != 0) {
            const uint32_t r = eo_call(api, get_u32(get_u32(p) + 4), p, NULL, 0);
            if (r) {
                const uint32_t id = 0x00402240u;
                const uint32_t o = eo_call(api, 0x005f6700u, get_u32(r + 4), &id, 1);
                const int key = (int)((o & 0xfffff) << 3) | lv;
                if (o && key != heat_set) {
                    heat_set = key;
                    const float heat = (float)lv;
                    uint32_t hb;
                    memcpy(&hb, &heat, 4);
                    eo_call(api, get_u32(get_u32(o) + 8), o, &hb, 1);
                    char m[64];
                    snprintf(m, sizeof m, "Heat level %d.", lv);
                    eo_message(api, m);
                }
            }
        }
    }
    /* Police in the city (2026-11-24, the person: "police still came with the switch off"): FUN_0065c7c0 sends the
     * game's message 0xca7dccd6 to AICopManager (vtable 0xbdba88; its interface at +0x40, slot 1 = 0x00403590
     * EnableCops(bool) writes [manager + 0x200]). The game builds a new manager for each scene and race, and its
     * constructor (FUN_0043d8e0) sets that flag back to 1, so one message did not last: while the switch is off the
     * message is sent again every second while driving. */
    const int want_cops = (int)eo_setting(api, "mod_eo_cops", 1);
    static int64_t cops_sent;
    if (want_cops != cops_on || (!want_cops && t - cops_sent > 1000000000LL)) {
        const int changed = want_cops != cops_on;
        cops_on = want_cops;
        cops_sent = t;
        const uint32_t f = (uint32_t)cops_on;
        if (changed)
            api->log(api, cops_on ? "core.nfsu: cops on: calling the game" : "core.nfsu: cops off: calling the game (again every second while driving)");
        eo_call(api, 0x0065c7c0u, 0, &f, 1);
        if (changed)
            eo_message(api, cops_on ? "Cops are now enabled." : "Cops are now disabled.");
    }
    const int want_lights = (int)eo_setting(api, "mod_eo_lights", 0);
    if (want_lights != lights) {
        const uint32_t car = eo_player_car(api), bits = 0xf000;
        static int said_nocar;
        if (!car && !said_nocar++)
            api->log(api, "core.nfsu: emergency lights: your car was not found (nothing switched)");
        if (car) {
            lights = want_lights;
            const int lit = (eo_call(api, 0x006eafd0u, car, &bits, 1) & 0xffu) != 0;
            if (lights && !lit)
                eo_call(api, 0x006cc110u, car, &bits, 1);
            else if (!lights && lit)
                eo_call(api, 0x006cc120u, car, &bits, 1);
            const int now_lit = (eo_call(api, 0x006eafd0u, car, &bits, 1) & 0xffu) != 0;
            char l[160];
            snprintf(l, sizeof l, "core.nfsu: emergency lights %s: car %08x, lit before %d, after %d", lights ? "on" : "off",
                     car, lit, now_lit);
            api->log(api, l);
        }
    }
}
