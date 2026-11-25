/* aiguard.inc - no empty AI actions (2026-11-23, the person: "the game quit during a police chase, I was the racer").
 *
 * The crash: main thread "call to 00000000 (return 004239b5)" then a jump to 0x3e800000 (a float, 0.25) in
 * FUN_00423950 - an AI goal choosing its next action: it walks its list of actions (list at goal+0x10, nodes
 * {next, prev, action}) and calls each action's vtable +0x18 without a null check. The goal constructors
 * (FUN_00444770 ... FUN_00445490: AIGoalFleePursuit, AIGoalRam, AIGoalHeliPursuit, ...) add their actions with
 * FUN_004417b0 (thiscall goal, name): it finds the action type by the name's hash in the registry [0x00d82680] (28
 * types, static registrations at 0x00bbb100 ...: {hash, factory, next}) and calls its factory - and appends the
 * result to the list even when it is 0 (a type not found, or a factory that made nothing). One such 0 crashed it.
 *
 * Here: an AFTER hook on FUN_004417b0 takes a node whose action is 0 off the goal's list again (it is the list's
 * last node: the function appends) and logs the action's name once each, so the goal simply has one action fewer.
 * NFSU_AIGUARD_OFF=1 = the game's behaviour. */
static uint32_t g_aig_stack[16][2]; /* (goal, name) per nested call (a factory may add actions too) */
static int g_aig_depth;
static void aiguard_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_aig_depth < 16) {
        g_aig_stack[g_aig_depth][0] = cpu->ecx;
        g_aig_stack[g_aig_depth][1] = get_u32(cpu->esp + 4);
    }
    ++g_aig_depth;
}
static void aiguard_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    if (g_aig_depth <= 0)
        return;
    --g_aig_depth;
    if (g_aig_depth >= 16)
        return;
    const uint32_t goal = g_aig_stack[g_aig_depth][0], name = g_aig_stack[g_aig_depth][1];
    if (!goal || cpu->eax)
        return;
    const uint32_t list = goal + 0x10, tail = get_u32(list + 4);
    if (!tail || tail == list || get_u32(tail) != list || get_u32(tail + 8) != 0)
        return;
    const uint32_t prev = get_u32(tail + 4);
    if (!prev)
        return;
    api->guest_write_u32(api, prev, list);     /* prev->next = head */
    api->guest_write_u32(api, list + 4, prev); /* head->prev = prev */
    const uint32_t n = get_u32(list + 8);
    if (n)
        api->guest_write_u32(api, list + 8, n - 1);
    static uint32_t seen[16];
    int k = 0;
    while (k < 16 && seen[k] && seen[k] != name)
        ++k;
    if (k < 16 && !seen[k]) {
        seen[k] = name;
        char nm[48] = {0};
        for (uint32_t i = 0; name && i < sizeof nm - 1; ++i) {
            const uint32_t c = get_u8(name + i);
            if (!c || c < 32 || c > 126)
                break;
            nm[i] = (char)c;
        }
        char l[140];
        snprintf(l, sizeof l, "core.nfsu: AI: action \"%s\" could not be made for goal %08x (vtable %08x) - left out "
                 "instead of crashing", nm, goal, get_u32(goal));
        api->log(api, l);
    }
}
/* The real cause: those factories (0x004393d0 ...) return 0 only when FUN_005f6340 - the game's "Fastmem" small-
 * block allocator (object 0x00da5320, 1880 call sites: AI actions, list nodes, physics bits ...) - runs out: it
 * then prints "Out of Fastmem memory" and stores to address 0 (a deliberate crash on Windows). Its whole region is
 * fixed: FUN_0069e4c0 sets it to 0x271000 bytes (2.4 MB) before the first allocation (FUN_005f5c20 -> FUN_005f59d0
 * takes it from the main allocator FUN_005f4d10). Long pursuits with more police (Revamped Pursuits, on by default)
 * fill it. An AFTER hook (cdecl: the size cell's address is the first argument, at [esp]) makes it 4x as large;
 * NFSU_FASTMEM_X=<n> changes the factor (1 = the game's). */
static void fastmem_size(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const uint32_t cell = get_u32(cpu->esp);
    if (!cell || get_u32(cell) != 0x271000u)
        return;
    const char *e = getenv("NFSU_FASTMEM_X");
    int x = e ? atoi(e) : 4;
    if (x < 1 || x > 16)
        x = 4;
    api->guest_write_u32(api, cell, 0x271000u * (uint32_t)x);
    char l[100];
    snprintf(l, sizeof l, "core.nfsu: AI: Fastmem pool %u KB (the game's 2500 KB x %d)", 0x271000u * (uint32_t)x / 1024u, x);
    api->log(api, l);
}
static int32_t police_sedan_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user);
static void aiguard_install(const PopModApi *api) {
    static uint32_t kid;
    if (api->on_key && api->on_key(api, police_sedan_key, NULL, &kid) != POP_OK)
        api->log(api, "core.nfsu: police sedan sale guard (key) NOT installed");
    static uint32_t hf;
    if (api->hook_install(api, 0x0069e4c0u, fastmem_size, POP_HOOK_AFTER, NULL, &hf) != POP_OK)
        api->log(api, "core.nfsu: Fastmem size hook NOT installed");
    if (getenv("NFSU_AIGUARD_OFF"))
        return;
    static uint32_t h1, h2;
    if (api->hook_install(api, 0x004417b0u, aiguard_before, POP_HOOK_BEFORE, NULL, &h1) != POP_OK ||
        api->hook_install(api, 0x004417b0u, aiguard_after, POP_HOOK_AFTER, NULL, &h2) != POP_OK)
        api->log(api, "core.nfsu: AI action guard NOT installed");
}

/* NFSU_SELL_LOG=1 (diagnostic): the sale command (0x0052e010), its row removal (0x00528da0) and 0x0052d480 */
static void sell_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: sell log %s: ecx %08x args %08x %08x %08x ret %08x", (const char *)user, cpu->ecx,
             get_u32(cpu->esp + 4), get_u32(cpu->esp + 8), get_u32(cpu->esp + 12), get_u32(cpu->esp));
    api->log(api, l);
}
static void sell_log_install(const PopModApi *api) {
    if (!getenv("NFSU_SELL_LOG"))
        return;
    static uint32_t a, b, c;
    api->hook_install(api, 0x0052e010u, sell_log, POP_HOOK_BEFORE, (void *)"command 52e010", &a);
    api->hook_install(api, 0x00528da0u, sell_log, POP_HOOK_BEFORE, (void *)"remove 528da0", &b);
    api->hook_install(api, 0x0052d480u, sell_log, POP_HOOK_BEFORE, (void *)"52d480", &c);
}

/* POLICE GARAGE SALES (2026-11-23, the person: "sold cars come back"). Selling in the police garage (key 2 ->
 * "Продать эту машину за N $?" -> Да) is the Police Career plugin's own: it pays and drops the entry from the
 * garage list ([0x00d9a4a4] + 0x58, 36-byte entries, +0x18 the career row) but never runs the game's sale command
 * (DALVehicleCommands::RemoveCustomizableCar, vtable 0x00be6e08, execute FUN_0052e010 = FUN_00528da0(rows, row id,
 * mask 2, 0): frees the row and its customization slots) - so the car stayed in the career and was listed again.
 * Here, while the police garage is open: when one police row leaves its list and the money went up in the same
 * moment, that row is removed the game's way. The helicopter's row is never removed (it is not an owned car).
 * NFSU_POLICE_SALE_OFF=1 = the plugin's behaviour. */
static int64_t police_cash(const PopModApi *api) {
    static uint32_t buf;
    if (!get_u32(0x00d2a178u + 8u + 6u * 4u) || (!buf && (!api->guest_alloc || api->guest_alloc(api, 16, &buf) != POP_OK)))
        return -1;
    api->guest_write_u32(api, buf, 0);
    const uint32_t ga[5] = {0x1776u, buf, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    if (!eo_call(api, 0x00519540u, 0x00d2a178u, ga, 5))
        return -1;
    return (int32_t)get_u32(buf);
}
/* The basic patrol sedan (pvehicle 0x1bb4725e, the plugin's starting police car) can never be sold (the person,
 * 2026-11-23: "or I can sell everything down to nothing and the save won't start"). */
#define POLICE_SEDAN_PV 0x1bb4725eu
static uint32_t police_row_pv(uint32_t id) {
    const uint32_t base = heli_rows();
    for (uint32_t i = 0; base && i < 0x14au; ++i)
        if (get_u32(base + 28u * i) == id)
            return get_u32(base + 28u * i + 8);
    return 0;
}
static uint32_t police_garage_highlighted(void) {
    const uint32_t obj = get_u32(0x00d9a4a4u);
    if (!obj)
        return 0xffffffffu;
    const uint32_t b = get_u32(obj + 0x58), e = get_u32(obj + 0x5c), idx = get_u32(obj + 0x80);
    return (b && e > b && (e - b) / 36u <= 64u && idx < (e - b) / 36u) ? get_u32(b + 36u * idx + 0x18) : 0xffffffffu;
}
static void shop_play_disabled(const PopModApi *api);
/* key 2 ("ПРОДАТЬ 2") on the sedan in the garage: swallowed, with the game's "not available" sound */
static int32_t police_sedan_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)user;
    if (!(vk == 0x32 || vk == 0x62 || dik == 0x03 || dik == 0x50))
        return 0;
    const uint32_t row = police_garage_highlighted();
    if (row == 0xffffffffu || police_row_pv(row) != POLICE_SEDAN_PV)
        return 0;
    if (down) {
        shop_play_disabled(api);
        api->log(api, "core.nfsu: police garage: the patrol sedan can't be sold (key 2 ignored)");
    }
    return 1;
}
static void police_sale_remove(const PopModApi *api, uint32_t gone, int64_t was, int64_t c) {
    if (gone != 0xffffffffu && police_row_pv(gone) == POLICE_SEDAN_PV) {
        /* sold some other way (a pad button): keep it in the career and take the money back */
        const uint32_t sa[5] = {0x1776u, (uint32_t)was, 0xffffffffu, 0xffffffffu, 0xffffffffu};
        eo_call(api, 0x005195a0u, 0x00d2a178u, sa, 5);
        api->log(api, "core.nfsu: police garage: the patrol sedan can't be sold - kept, money taken back");
        return;
    }
    if (gone == 0xffffffffu || heli_row_catalog(gone) || !(heli_row_is_police(gone) || heli_row_is(gone)))
        return;
    const uint32_t args[3] = {gone, 2, 0};
    const uint32_t ok = eo_call(api, 0x00528da0u, heli_rows(), args, 3) & 0xffu;
    char l[140];
    snprintf(l, sizeof l, "core.nfsu: police garage: car %08x sold (money %d -> %d) - removed from the career: %s", gone,
             (int)was, (int)c, ok ? "yes" : "NO");
    api->log(api, l);
}
/* The plugin leaves the garage list as it is (7 entries before and after) and only pays: the sold car is the one
 * highlighted ([obj+0x80] into the list) in the pass before the money went up. A rise to exactly the "Everything
 * unlocked" top-up amount (10 000 000 or the typed mod_eo_cash) is not a sale. */
static void police_sale_pass(const PopModApi *api) {
    static int64_t cash = -1;
    static uint32_t obj_was, row_was = 0xffffffffu;
    static int64_t at;
    const uint32_t obj = get_u32(0x00d9a4a4u);
    if (!obj || getenv("NFSU_POLICE_SALE_OFF")) {
        obj_was = 0, cash = -1, row_was = 0xffffffffu;
        return;
    }
    const int64_t t = now_ns();
    if (t - at < 50000000LL) /* 20 checks a second */
        return;
    at = t;
    const uint32_t b = get_u32(obj + 0x58), e = get_u32(obj + 0x5c), idx = get_u32(obj + 0x80);
    const uint32_t row = (b && e > b && idx < (e - b) / 36u && (e - b) / 36u <= 64u) ? get_u32(b + 36u * idx + 0x18)
                                                                                     : 0xffffffffu;
    const int64_t c = police_cash(api);
    if (obj == obj_was && cash >= 0 && c > cash && c != 10000000 && c != eo_setting(api, "mod_eo_cash", -1)) {
        if (getenv("NFSU_SALE_DEBUG")) {
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: sale debug: money %d -> %d, highlighted row %08x (now %08x)", (int)cash,
                     (int)c, row_was, row);
            api->log(api, l);
        }
        police_sale_remove(api, row_was, cash, c);
    }
    obj_was = obj;
    cash = c;
    row_was = row;
}
