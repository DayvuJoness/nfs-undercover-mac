/* padlayout.inc - the console layout for an Xbox / PlayStation pad while driving (setting pad_layout).
 *
 * Why (the person, 2026-11-22: "I can move through the menus with the pad but can't drive: gas, brake, left, right,
 * nitro, slow-down don't work"): the PC game ships pad defaults only for the menus. Its input table (TInputSpec,
 * built by FUN_006774c0 from the static rows at 0x00d37f98: 0x34 bytes each, name + keyboard primary/secondary +
 * pad primary/secondary, 12-byte specs {type, 0, code}) has no pad binding for any driving action; what a profile
 * had (X gas, B brake, A handbrake, Y speedbreaker, LB look back, RB reset) came from somewhere else and left nitro
 * on button 6, which an Xbox pad never reports here. Both triggers were unused.
 *
 * Spec types (FUN_00677be0, jump table 0x00677f9c): 2 button [state + 0xc + 4*code] / 128, 3 POV, 4/5 axis X/Y/Z
 * (code 0/1/2) low/high half, 6/7 rotation axis Rx/Ry/Rz [state + 0x20c + 4*code] low/high half, 8/9 sliders.
 * A half is max(0, +-(32767.5 - v)) / 32767.5. Measured on the game's pad state ([0x00da38e0] vt+4): LT -> Rx
 * (+0x20c), RT -> Ry (+0x210), 0 at rest, 65535 pulled; buttons X 0, A 1, B 2, Y 3, LB 4, RB 5, Start 9.
 * Measured with a half-pulled trigger: the game makes that axis (t - 127) * 514 from the XInput trigger byte t (so a
 * byte below 128 reads 0) and the high half of the axis then counts only t 191..255 - three quarters of the pull did
 * nothing. The kit's XInput remap ("lt=lt@190:255,rt=rt@190:255") reports each trigger as 190 at rest (axis 32382: exactly 0) .. 255
 * pulled: the high half runs 0..1 over the whole pull - an analog throttle and brake.
 *
 * Layout written (pad 0..n-1 primary slot, [0x00da39a4 + 8*action] + 0x18 + 0x18*pad; secondary cleared):
 *   RT gas, LT brake/reverse, left stick steer, A nitro, B speedbreaker, X handbrake, Y look back, RB camera,
 *   LB reset. Start pauses (the game's own). The table is re-checked every second and rewritten only where it
 *   differs (a profile load restores the game's). pad_layout 0 = the game's own bindings are left alone. */

static const struct PadSpec {
    uint32_t action, type, code;
} k_pad_console[] = {
    {0, 7, 1},  /* GAME_ACTION_GAS: RT (Ry high half) */
    {1, 7, 0},  /* GAME_ACTION_BRAKE: LT (Rx high half) */
    {2, 4, 0},  /* STEERLEFT: left stick X low half */
    {3, 5, 0},  /* STEERRIGHT: X high half */
    {4, 2, 0},  /* HANDBRAKE: X */
    {5, 2, 2},  /* GAMEBREAKER (speedbreaker): B */
    {6, 2, 1},  /* NOS: A */
    {17, 2, 4}, /* RESET: LB */
    {18, 2, 5}, /* CAM_ACTION_CHANGE: RB */
    {19, 2, 3}, /* CAM_ACTION_LOOKBACK: Y */
};
static int g_pad_layout_set = -1;
static uint32_t g_pad_orig[25][2][3]; /* the game's pad 0 specs before ours (primary, secondary) */
static int g_pad_orig_saved;

static void pad_layout_pass(const PopModApi *api) {
    static int64_t at;
    const int64_t now = now_ns();
    if (now - at < 1000000000LL)
        return;
    at = now;
    int64_t v = 1;
    if (api->settings_get)
        api->settings_get(api, "pad_layout", &v);
    const int on = v != 0;
    static int (*remap)(const char *);
    static int looked;
    if (!looked) {
        looked = 1;
        remap = (int (*)(const char *))dlsym(RTLD_DEFAULT, "recomp_xinput_remap");
        if (!remap)
            api->log(api, "core.nfsu: pad layout: this host has no XInput remap (triggers stay as the game reads them)");
    }
    if (on != g_pad_layout_set && remap)
        remap(on ? "lt=lt@190:255,rt=rt@190:255" : "");
    const uint32_t pads = get_u32(0x00da3be0u);
    const uint32_t gas = get_u32(0x00da39a4u);
    if (!gas || pads == 0 || pads > 4) {
        g_pad_layout_set = on;
        return;
    }
    if (!g_pad_orig_saved) {
        g_pad_orig_saved = 1;
        for (size_t i = 0; i < sizeof k_pad_console / sizeof k_pad_console[0]; ++i) {
            const uint32_t sp = get_u32(0x00da39a4u + 8 * k_pad_console[i].action);
            for (int s = 0; s < 2; ++s)
                for (int w = 0; w < 3; ++w)
                    g_pad_orig[k_pad_console[i].action][s][w] = sp ? get_u32(sp + 0x18 + 12 * s + 4 * w) : 0;
        }
    }
    int wrote = 0;
    for (size_t i = 0; i < sizeof k_pad_console / sizeof k_pad_console[0]; ++i) {
        const struct PadSpec *ps = &k_pad_console[i];
        const uint32_t sp = get_u32(0x00da39a4u + 8 * ps->action);
        if (sp < 0x01400000u)
            continue;
        for (uint32_t p = 0; p < pads; ++p) {
            const uint32_t at_ = sp + 0x18 + 0x18 * p;
            uint32_t want[2][3];
            if (on) {
                want[0][0] = ps->type, want[0][1] = 0, want[0][2] = ps->code;
                want[1][0] = want[1][1] = want[1][2] = 0;
            } else {
                if (g_pad_layout_set != 1) /* off from the start: the game's own, untouched */
                    continue;
                memcpy(want, g_pad_orig[ps->action], sizeof want);
            }
            for (int s = 0; s < 2; ++s)
                for (int w = 0; w < 3; ++w)
                    if (get_u32(at_ + 12 * s + 4 * w) != want[s][w]) {
                        api->guest_write_u32(api, at_ + 12 * s + 4 * w, want[s][w]);
                        ++wrote;
                    }
        }
    }
    if (wrote || on != g_pad_layout_set) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: pad layout: %s (%d values written, %u pad%s)",
                 on ? "console (RT gas, LT brake, A nitro, B speedbreaker, X handbrake)" : "the game's own", wrote, pads,
                 pads == 1 ? "" : "s");
        api->log(api, l);
    }
    g_pad_layout_set = on;
}
