/* ADD-ON CARS (2026-11-20, setting mod_addon_cars, restart): the five add-on cars of the person's mods folder
 * (Koenigsegg Regera / CCXR, Ferrari FXX-K Evo, Lamborghini Aventador SVJ / Miura SV). tools/addon_cars.py made,
 * at packaging time, what VltEd and Ed make on Windows:
 *  - GLOBAL\ADDONCARS.BIN, an attribute vault "addon_cars" with the cars' collections (pvehicle, car, car_parts,
 *    car_tuning, ecar, frontend, caraudio, engineaudio_ng): mounted here right after the game's car vault
 *    (after FUN_006ac7d0 LoadCarsVault), with the game's own loader as the Police Career plugin mounts its police cars:
 *    buffer = FUN_006abbc0(path, &size, 0x1040); FUN_006ac6e0(buffer, name, &handle, 0, [0x00d3bdb0], 1);
 *    FUN_0069e670(buffer);
 *  - the GLOBALB bundle with their car types, collisions and parts, read in place of GLOBAL\GLOBALB.LZC (kit file
 *    overlay, launch.c);
 *  - addoncars.txt (RECOMP_NFSU_ADDON_CARS): "list car_list <collection> <field> <class> <collection>" = append that
 *    car to the game's car list (done here, after the mount), "name <label hash> <text>" = a string the game's text
 *    lookup FUN_00571b80(hash) answers (the car names Ed adds to the language files). */
#define CARS_VAULT_LOAD 0x006ac7d0u /* LoadCarsVault: [0x00da51c4] = the cars vault (005f77d0, its stage, is not a
                                     listed function, so the kit does not hook it) */
#define FILE_LOAD 0x006abbc0u   /* cdecl (path, &size, flags) -> buffer */
#define VAULT_MOUNT 0x006ac6e0u /* cdecl (buffer, name, &handle, 0, pool, 1) -> handle */
#define BUFFER_FREE 0x0069e670u /* cdecl (buffer) */
#define TEXT_LOOKUP 0x00571b80u /* cdecl (label hash) -> char* or 0 */

typedef struct {
    uint32_t hash, text; /* text: a guest string, made at the mount (the guest heap is up by then) */
    char host[64];
} AddonName;
typedef struct {
    char coll[48], field[48], cls[48], car[48];
    int done;
} AddonListAdd;
typedef struct {
    uint32_t pv, fe, label; /* label: the car's name label hash (0 = none) */
} AddonDealer;
static AddonDealer g_addon_dealer[96];
static uint32_t g_addon_rekey[8][2]; /* a car's pvehicle key renamed: rows a save holds with the old key get the new */
static int g_addon_nrekey;
static int g_addon_ndealer;
static AddonName g_addon_names[240];
static int g_addon_nnames;
static AddonListAdd g_addon_lists[240];
static int g_addon_nlists;
static int g_addon_on = -1;
static int g_addon_mounted; /* the vault is in: only then may the lists name its cars */
static int g_addon_group_on[2]; /* per career: add-on cars, Porsche Pack (addon_groups_refresh below) */
static int addon_off_car(uint32_t pv);

static uint32_t guest_string(const PopModApi *api, const char *s) {
    const uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t at = 0;
    if (!api->guest_alloc || api->guest_alloc(api, n + 4, &at) != POP_OK || !at)
        return 0;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4 && i + b < n; ++b)
            w |= (uint32_t)(uint8_t)s[i + b] << (8 * b);
        api->guest_write_u32(api, at + i, w);
    }
    return at;
}

static uint32_t g_addon_text_calls, g_addon_text_hits;
static void addon_text(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t hash = get_u32(cpu->esp + 4);
    ++g_addon_text_calls;
    for (int i = 0; i < g_addon_nnames; ++i)
        g_addon_text_hits += g_addon_names[i].hash == hash;
    for (int i = 0; i < g_addon_nnames; ++i)
        if (g_addon_names[i].hash == hash && g_addon_names[i].text) {
            api->hook_return(api, cpu, g_addon_names[i].text, 0); /* __cdecl */
            return;
        }
    const uint32_t ret = get_u32(cpu->esp);
    /* NFSU_TEXT_WHO=<hex hash> (diagnostic): who asks for this label (each caller once) */
    {
        static uint32_t who = 1, seen[16];
        if (who == 1)
            who = getenv("NFSU_TEXT_WHO") ? (uint32_t)strtoul(getenv("NFSU_TEXT_WHO"), NULL, 16) : 0;
        if (who && hash == who) {
            int k = 0;
            while (k < 16 && seen[k] && seen[k] != ret)
                ++k;
            if (k < 16 && !seen[k]) {
                seen[k] = ret;
                char l[120];
                snprintf(l, sizeof l, "core.nfsu: label %08x asked from %08x (stack %08x %08x)", hash, ret,
                         get_u32(cpu->esp + 0x20), get_u32(cpu->esp + 0x40));
                api->log(api, l);
            }
        }
    }
    api->call_next(api, inv, cpu);
    /* NFSU_TEXT_WHO_TEXT=<hex bytes> (diagnostic): who gets this text back, whatever hash it asked with */
    {
        static int on = -1;
        static uint8_t want[32];
        static int wn;
        static uint32_t seen[16];
        if (on < 0) {
            const char *e = getenv("NFSU_TEXT_WHO_TEXT");
            on = 0;
            if (e)
                for (; e[0] && e[1] && wn < 32; e += 2) {
                    unsigned v = 0;
                    sscanf(e, "%2x", &v);
                    want[wn++] = (uint8_t)v;
                    on = 1;
                }
        }
        if (on && cpu->eax) {
            int same = 1;
            for (int i = 0; i <= wn && same; ++i)
                same = get_u8(cpu->eax + (uint32_t)i) == (i < wn ? want[i] : 0u);
            int k = 0;
            while (same && k < 16 && seen[k] && seen[k] != ret)
                ++k;
            if (same && k < 16 && !seen[k]) {
                seen[k] = ret;
                char l[120];
                snprintf(l, sizeof l, "core.nfsu: text asked with %08x from %08x", hash, ret);
                api->log(api, l);
            }
        }
    }
    /* NFSU_TEXT_MISS=1 (diagnostic): labels the game asks for and has no text for, with the caller (once each) */
    static int miss = -1;
    if (miss < 0)
        miss = getenv("NFSU_TEXT_MISS") ? 1 : 0;
    if (miss && !cpu->eax) {
        static uint32_t seen[256];
        static int n;
        for (int i = 0; i < n; ++i)
            if (seen[i] == hash)
                return;
        if (n < 256)
            seen[n++] = hash;
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: text miss %08x from %08x", hash, ret);
        api->log(api, l);
    }
}

/* FUN_00525070 (cdecl, frontend key -> name label hash) builds "CARNAME_<maker>_<model>" from the frontend
 * collection's names and checks the game's strings for it; an add-on car's label (its Ed string, answered by
 * addon_text) is given straight for its frontend key, whatever names its new collections carry (2026-11-20: the
 * car lot showed MISSING STRING for all five). */
#define CAR_NAME_LABEL 0x00525070u
static void addon_name_label(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t fe = get_u32(cpu->esp + 4);
    for (int i = 0; i < g_addon_ndealer; ++i)
        if (g_addon_dealer[i].fe == fe && g_addon_dealer[i].label && !getenv("NFSU_ADDON_NAMECHECK")) {
            api->hook_return(api, cpu, g_addon_dealer[i].label, 0); /* __cdecl */
            return;
        }
    api->call_next(api, inv, cpu);
}

/* A fourth shop section, "Россия" (2026-11-21, the person). The shop's "Америка / Европа / Япония" are rows of the
 * game's static menu tree at 0x00d2c140..0x00d2d52c (20-byte rows {id, parent id, label hash, 0, 0}): ids 0xcd,
 * 0xce, 0xcf under parent 0xcc. FUN_00597340(parent, list) makes a node (FUN_006a1d30(0x2c, "StandardFETreeDataNode",
 * [0x00d3bd9c]) + FUN_005972b0) for each row of that parent and appends it to the list. The shop's choice handler
 * FUN_00575d30 already knows a fourth id, 0xdb (its car list at +0x78), that no row names. With an "region 3 <label>"
 * line in addoncars.txt (written under "in addon": only while the add-on cars are on) a row {0xdb, 0xcc, label} is
 * linked in before Japan, and the "regioncar <pv>" cars are moved into that list (below). */
/* 2026-11-21: the section has its own id and lists. 0xdb with the shop's fourth list (+0x78, nodes at +0xa4) is the
 * game's own "Призовые машины" (bonus cars; FUN_005bb910 adds that row when a reward car is owned, and case 0xdb sets
 * byte +0x1f2 so its cars are given, not sold) - the person got a reward car and both rows showed the same four cars. */
#define REGION_ID 0x4dbu
static uint32_t g_region_label, g_region_n;
static void addon_region_tree(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t parent = get_u32(cpu->esp + 4), list = get_u32(cpu->esp + 8);
    api->call_next(api, inv, cpu);
    if (parent != 0xccu || !list || !g_addon_group_on[0]) /* the Ladas' section: add-on cars on for this career */
        return;
    static uint32_t row;
    if (!row && (api->guest_alloc(api, 32, &row) != POP_OK || !row))
        return;
    const uint32_t r[5] = {REGION_ID, 0xccu, g_region_label, 0, 0};
    for (uint32_t k = 0; k < 5; ++k)
        api->guest_write_u32(api, row + 4u * k, r[k]);
    uint32_t node = 0;
    const uint32_t aa[3] = {0x2cu, 0x00bf5a60u, get_u32(0x00d3bd9cu)};
    if (api->guest_call(api, 0x006a1d30u, 0, aa, 3, &node) != POP_OK || !node)
        return;
    const uint32_t ra[1] = {row};
    uint32_t made = 0;
    if (api->guest_call(api, 0x005972b0u, node, ra, 1, &made) != POP_OK || !made)
        return;
    /* linked in as the game links its nodes (a node's link at +4: [link] next, [link + 4] previous; the list head is
     * the sentinel), before Japan (id 0xcf at node +0x18) so the sections stay alphabetical in Russian - Америка,
     * Европа, Россия, Япония (2026-11-21, the person) - or last when there is no Japan */
    const uint32_t link = made + 4u;
    uint32_t before = list;
    for (uint32_t x = get_u32(list), n = 0; x && x != list && n < 64; x = get_u32(x), ++n)
        if (get_u32(x + 0x14u) == 0xcfu) {
            before = x;
            break;
        }
    const uint32_t prev = get_u32(before + 4u);
    api->guest_write_u32(api, prev, link);
    api->guest_write_u32(api, link + 4u, prev);
    api->guest_write_u32(api, link, before);
    api->guest_write_u32(api, before + 4u, link);
    static int said;
    if (!said++) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: shop section 0x%x (region %u, label %08x) added to the menu", REGION_ID, g_region_n,
                 g_region_label);
        api->log(api, l);
    }
}
/* The shop's car lists are vectors {begin, end, capacity} of 36-byte entries at +0x48 / +0x58 /
 * +0x68 / +0x78 of the shop object (+0x10 region, +0x18 career row id); FUN_005b3bb0 / FUN_005b5010 fill the first three by region and none fills +0x78
 * (callsite hooks inside their loops never fired). The add-on cars of the added section are listed as European
 * (their frontend region 1), and when a section is chosen (FUN_00575d30, thiscall (shop, id)) their entries move from
 * the European list to +0x78 - pushed with the game's own FUN_0052f570 (thiscall, the vector's push), taken out of
 * +0x58 by shifting the rest down. */
static uint32_t g_region_cars[16];
static int g_region_ncars;
/* an entry names its car by the career row's id (+0x18; the rows: [0x00d8e1a4] + 0x88, 28 bytes, pvehicle at +8) */
static int region_car(uint32_t id) {
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (!obj)
        return 0;
    for (uint32_t i = 0; i < 0x14au; ++i) {
        const uint32_t r = obj + 0x88u + 28u * i;
        if (get_u32(r) != id)
            continue;
        const uint32_t pv = get_u32(r + 8u);
        for (int k = 0; k < g_region_ncars; ++k)
            if (g_region_cars[k] == pv)
                return 1;
        return 0;
    }
    return 0;
}
/* FUN_005b5010 (run when the shop opens; the shop object is [0x00d9a4a0]) pushes each European entry with
 * FUN_0052f570(thiscall list = shop + 0x58, entry): an entry of a car of the added section goes to shop + 0x78
 * instead, before the shop builds its section children from the four lists */
static void addon_region_push(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const uint32_t shop = get_u32(0x00d9a4a0u);
    if (!shop || cpu->ecx != shop + 0x58u)
        return;
    const uint32_t entry = get_u32(cpu->esp + 4);
    if (!entry || !region_car(get_u32(entry + 0x18u)))
        return;
    cpu->ecx = shop + 0x78u;
    static int said;
    if (said++ < 4) {
        char l[100];
        snprintf(l, sizeof l, "core.nfsu: shop entry of row %08x goes to section 4", get_u32(entry + 0x18u));
        api->log(api, l);
    }
}
/* After FUN_005b5010 (fastcall shop) has filled the lists and made a menu node per car, the section's cars - pushed
 * into the fourth list by addon_region_push, so the game made their nodes with their names - move out of it: their
 * entries into this section's own vector g_rvec ({begin, end, capacity}, entries in g_rstore), their nodes (link at
 * node + 4: [link] next, [link + 4] previous; the car's index at link + 0x14) into this section's own list g_rlist
 * (a sentinel {next, previous}), renumbered; the bonus cars stay in the fourth list, renumbered too. Nodes left in
 * g_rlist from the last time the shop opened are dropped (three small blocks). */
static uint32_t g_rvec, g_rstore, g_rlist;
#define RSTORE_MAX 32u
static void addon_region_split(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t shop = cpu->ecx;
    api->call_next(api, inv, cpu);
    if (!shop)
        return;
    if (!g_rvec && (api->guest_alloc(api, 12, &g_rvec) != POP_OK || !g_rvec))
        return;
    if (!g_rstore && (api->guest_alloc(api, RSTORE_MAX * 0x24u, &g_rstore) != POP_OK || !g_rstore))
        return;
    if (!g_rlist && (api->guest_alloc(api, 8, &g_rlist) != POP_OK || !g_rlist))
        return;
    api->guest_write_u32(api, g_rvec, g_rstore);
    api->guest_write_u32(api, g_rvec + 4, g_rstore);
    api->guest_write_u32(api, g_rvec + 8, g_rstore + RSTORE_MAX * 0x24u);
    api->guest_write_u32(api, g_rlist, g_rlist);
    api->guest_write_u32(api, g_rlist + 4, g_rlist);
    const uint32_t b = get_u32(shop + 0x78u), e = get_u32(shop + 0x7cu);
    if (!b || e <= b)
        return;
    const uint32_t n = (e - b) / 0x24u;
    if (n > 256)
        return;
    int32_t newidx[256];
    uint32_t nr = 0, nb = 0;
    for (uint32_t j = 0; j < n; ++j)
        newidx[j] = region_car(get_u32(b + 0x24u * j + 0x18u)) && nr < RSTORE_MAX ? (int32_t)(0x10000u | nr++) : (int32_t)nb++;
    if (!nr)
        return;
    /* entries: the section's to g_rstore, the bonus cars packed at the front of the fourth list */
    for (uint32_t j = 0; j < n; ++j) {
        const uint32_t from = b + 0x24u * j;
        const uint32_t to = (newidx[j] & 0x10000) ? g_rstore + 0x24u * (uint32_t)(newidx[j] & 0xffff)
                                                    : b + 0x24u * (uint32_t)newidx[j];
        if (to != from)
            for (uint32_t k = 0; k < 0x24u; k += 4)
                api->guest_write_u32(api, to + k, get_u32(from + k));
    }
    api->guest_write_u32(api, shop + 0x7cu, b + 0x24u * nb);
    api->guest_write_u32(api, g_rvec + 4, g_rstore + 0x24u * nr);
    /* nodes */
    const uint32_t head = shop + 0xa4u;
    uint32_t moved = 0;
    for (uint32_t link = get_u32(head), guard = 0; link && link != head && guard < 512; ++guard) {
        const uint32_t next = get_u32(link), idx = get_u32(link + 0x14u);
        if (idx < n) {
            if (newidx[idx] & 0x10000) {
                const uint32_t prev = get_u32(link + 4);
                api->guest_write_u32(api, prev, next);
                api->guest_write_u32(api, next + 4, prev);
                const uint32_t last = get_u32(g_rlist + 4);
                api->guest_write_u32(api, last, link);
                api->guest_write_u32(api, g_rlist + 4, link);
                api->guest_write_u32(api, link + 4, last);
                api->guest_write_u32(api, link, g_rlist);
                api->guest_write_u32(api, link + 0x14u, (uint32_t)(newidx[idx] & 0xffff));
                ++moved;
            } else
                api->guest_write_u32(api, link + 0x14u, (uint32_t)newidx[idx]);
        }
        link = next;
    }
    static int said;
    if (said++ < 4) {
        char l[140];
        snprintf(l, sizeof l, "core.nfsu: shop: %u cars in section 0x%x (%u menu rows), %u left in the bonus list", nr,
                 REGION_ID, moved, nb);
        api->log(api, l);
    }
}
/* The section chosen (FUN_00575d30, thiscall (shop, id, ?, ?), RET 0xc): what the game's cases do (0x00575d6e..),
 * with this section's lists: [shop+0xac] = 0, [shop+0x88] = its vector, FUN_0056f520(its node list, 0) on the menu
 * object FUN_00551dc0([0x00d99354], 0x28) gives, [shop+0x1f3] = 1; and [shop+0x1f2] = 0 (sold, not given). */
static void addon_region_pick(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t shop = cpu->ecx, id = get_u32(cpu->esp + 4);
    if (!shop || id != REGION_ID || !g_rvec || !g_rlist) {
        api->call_next(api, inv, cpu);
        return;
    }
    uint32_t fe = 0;
    const uint32_t a28[1] = {0x28u};
    api->guest_call(api, 0x00551dc0u, get_u32(0x00d99354u), a28, 1, &fe);
    if (fe) {
        api->guest_write_u32(api, shop + 0xacu, 0);
        api->guest_write_u32(api, shop + 0x88u, g_rvec);
        const uint32_t la[2] = {g_rlist, 0};
        uint32_t r = 0;
        api->guest_call(api, 0x0056f520u, fe, la, 2, &r);
        api->guest_write_u8(api, shop + 0x1f2u, 0);
        api->guest_write_u8(api, shop + 0x1f3u, 1);
    }
    static int said;
    if (said++ < 4)
        api->log(api, fe ? "core.nfsu: shop section Russia chosen (its own lists)" : "core.nfsu: shop section: no menu object");
    api->hook_return(api, cpu, 1, 0xc); /* __thiscall, RET 0xc */
}

/* No second copy of an owned car (2026-11-21, the person: "a car marked bought can be bought again in any section").
 * The game itself allows it: the shop's CarSelected native (APT "CarSelected" -> 0x00569b40 -> FUN_005678c0, thiscall
 * (shop [0x00d9a4a0], index into the current list [shop + 0x88])) checks only the garage size (FUN_00556ff0), the
 * money (value 6006) and whether the car is on sale (FUN_0051df10: value 6001 of the car's frontend key, or the
 * class 0xa13753eb flag) - never ownership. The "КУПЛЕНО" mark is the list's isOwned (FUN_005913a0): a query object
 * {vtable 0x00be9ad0, 0, frontend key = entry[0], result byte +0xc, 0} run by FUN_00527c50 (fastcall), which lists
 * the owned cars (query flags 0xf0002) and compares their frontend keys. The same query is asked here; an owned car
 * gets the game's "not available" sound instead of the purchase dialog, as a greyed-out button does. Shop mode 2
 * ([shop + 8], price 0 and its own dialogs: not the car lot) is left to the game. NFSU_SHOP_OWNED_OFF=1 = old. */
static uint32_t g_owned_query;
static int shop_car_owned(const PopModApi *api, uint32_t fe) {
    if (!g_owned_query && (api->guest_alloc(api, 16, &g_owned_query) != POP_OK || !g_owned_query))
        return 0;
    api->guest_write_u32(api, g_owned_query, 0x00be9ad0u);
    api->guest_write_u32(api, g_owned_query + 4u, 0);
    api->guest_write_u32(api, g_owned_query + 8u, fe);
    api->guest_write_u32(api, g_owned_query + 12u, 0);
    uint32_t r = 0;
    if (api->guest_call(api, 0x00527c50u, g_owned_query, NULL, 0, &r) != POP_OK)
        return 0;
    return (get_u32(g_owned_query + 12u) & 0xffu) != 0;
}
static int heli_row_is(uint32_t row);   /* heli.inc: a police helicopter's career row */
static uint32_t heli_owned_row(void);   /* heli.inc: a bought helicopter's row, or -1 */
static void shop_play_disabled(const PopModApi *api);
static void shop_car_selected(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t shop = cpu->ecx, index = get_u32(cpu->esp + 4);
    const uint32_t list = shop ? get_u32(shop + 0x88u) : 0;
    /* The police helicopter (2026-11-24: bought and sold like any police car): its entry shares the copcar frontend
     * with the interceptor, so "owned" is asked by its own rows - one already bought: no second one; else the
     * game's purchase. */
    if (shop && list && index < 0x1000u && get_u32(list) + 0x24u * (index + 1u) <= get_u32(list + 4u) &&
        heli_row_is(get_u32(get_u32(list) + 0x24u * index + 0x18u))) {
        if (heli_owned_row() != 0xffffffffu) {
            shop_play_disabled(api);
            api->log(api, "core.nfsu: shop: a police helicopter is already in the police garage");
            api->hook_return(api, cpu, 0, 4);
            return;
        }
        api->log(api, "core.nfsu: shop: buying the police helicopter");
        api->call_next(api, inv, cpu);
        return;
    }
    if (!shop || !list || get_u32(shop + 4u) != 0 || (get_u8(shop + 0x1f4u)) != 0 || get_u32(shop + 8u) == 2u) {
        api->call_next(api, inv, cpu);
        return;
    }
    const uint32_t b = get_u32(list), e = get_u32(list + 4u);
    if (index >= 0x1000u || b + 0x24u * (index + 1u) > e) {
        api->call_next(api, inv, cpu);
        return;
    }
    const uint32_t fe = get_u32(b + 0x24u * index);
    if (!shop_car_owned(api, fe)) {
        api->call_next(api, inv, cpu);
        return;
    }
    shop_play_disabled(api);
    static int said;
    if (said++ < 8) {
        char l[140];
        snprintf(l, sizeof l, "core.nfsu: shop: car %08x is already owned - no second purchase", fe);
        api->log(api, l);
    }
    api->hook_return(api, cpu, 0, 4); /* __thiscall, RET 4 */
}
static void shop_play_disabled(const PopModApi *api) {
    uint32_t snd = 0;
    const uint32_t na[1] = {0x00bf71e8u}; /* "SOUND_ACCEPT_DISABLED" */
    if (api->guest_call(api, 0x004b3ed0u, 0, na, 1, &snd) == POP_OK) {
        const uint32_t sa[1] = {snd};
        uint32_t r = 0;
        api->guest_call(api, 0x00836ff0u, get_u32(0x01333770u), sa, 1, &r);
    }
}
static void shop_owned_install(const PopModApi *api) {
    if (getenv("NFSU_SHOP_OWNED_OFF"))
        return;
    static uint32_t h;
    const PopModStatus s = api->hook_install(api, 0x005678c0u, shop_car_selected, POP_HOOK_REPLACE, NULL, &h);
    if (s != POP_OK)
        api->log(api, "core.nfsu: shop owned-car check NOT installed");
}

/* NFSU_SHOP_TRACE=1 (diagnostic): when the shop's list fillers and the list push run */
static void shop_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    static int n;
    if (n++ > 40)
        return;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: shop trace %s ecx %08x esi %08x ret %08x", (const char *)user, cpu->ecx, cpu->esi,
             get_u32(cpu->esp));
    api->log(api, l);
}
/* NFSU_APT_TRACE=1 (diagnostic): the callers of the game's Flash object helpers (FUN_009e0900 new object/array,
 * FUN_00830a80 set number, FUN_008309a0 set text, FUN_00830930 set member), each return address once */
static void apt_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    static uint32_t seen[256];
    static int n;
    const uint32_t ret = get_u32(cpu->esp);
    for (int i = 0; i < n; ++i)
        if (seen[i] == ret)
            return;
    if (n >= 256)
        return;
    seen[n++] = ret;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: apt %s from %08x (arg2 %08x)", (const char *)user, ret, get_u32(cpu->esp + 8));
    api->log(api, l);
}
static void apt_trace_install(const PopModApi *api) {
    if (!getenv("NFSU_APT_TRACE"))
        return;
    static uint32_t h[4];
    api->hook_install(api, 0x009e0900u, apt_trace, POP_HOOK_BEFORE, (void *)"new", &h[0]);
    api->hook_install(api, 0x00830a80u, apt_trace, POP_HOOK_BEFORE, (void *)"setnum", &h[1]);
    api->hook_install(api, 0x008309a0u, apt_trace, POP_HOOK_BEFORE, (void *)"settext", &h[2]);
    api->hook_install(api, 0x00830930u, apt_trace, POP_HOOK_BEFORE, (void *)"setmember", &h[3]);
}

/* append the car to a car list: the array is copied one element larger into mod memory (the node points at it) */
static int addon_list_add(const PopModApi *api, AddonListAdd *a) {
    const uint32_t coll = attrib_collection(attrib_key("car_list"), attrib_key(a->coll));
    int where = -1;
    const uint32_t node = coll ? attrib_node(coll, attrib_key(a->field), &where) : 0;
    if (!node)
        return 0;
    const uint32_t fl = get_u8(node + 0xb);
    if (!(fl & 2u) || (fl & 0x50u) || where != 0)
        return -1;
    const uint32_t arr = attrib_value_addr(coll, node);
    const uint32_t count = get_u16(arr + 2), esz = get_u16(arr + 4), tfl = get_u16(arr + 6);
    const uint32_t head = (tfl & 0x8000u) ? 16u : 8u, ck = attrib_key(a->cls), kk = attrib_key(a->car);
    if (esz != 12)
        return -1;
    for (uint32_t i = 0; i < count; ++i)
        if (get_u32(arr + head + i * 12) == ck && get_u32(arr + head + i * 12 + 4) == kk)
            return 1; /* there already */
    uint32_t mem = 0;
    if (!api->guest_alloc || api->guest_alloc(api, head + (count + 1) * 12 + 16, &mem) != POP_OK || !mem)
        return -1;
    mem = (mem + 15u) & ~15u;
    for (uint32_t b = 0; b < head + count * 12; b += 4)
        api->guest_write_u32(api, mem + b, get_u32(arr + b));
    api->guest_write_u32(api, mem + head + count * 12, ck);
    api->guest_write_u32(api, mem + head + count * 12 + 4, kk);
    api->guest_write_u32(api, mem + head + count * 12 + 8, 0);
    api->guest_write_u32(api, mem, ((count + 1) << 16) | (count + 1)); /* alloc, count */
    api->guest_write_u32(api, node + 4, mem);
    return 1;
}

static void addon_lists(const PopModApi *api) {
    int added = 0, left = 0;
    for (int i = 0; i < g_addon_nlists; ++i) {
        AddonListAdd *a = &g_addon_lists[i];
        if (a->done)
            continue;
        const int r = addon_list_add(api, a);
        if (r)
            a->done = 1;
        if (r > 0)
            ++added;
        else
            ++left;
    }
    if (added || left) {
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: add-on cars: %d car list entries added, %d not possible", added, left);
        api->log(api, l);
    }
}

static void addon_mount(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    static int done;
    if (done || !get_u32(0x00da51c4u))
        return;
    done = 1;
    for (int i = 0; i < g_addon_nnames; ++i)
        g_addon_names[i].text = guest_string(api, g_addon_names[i].host);
    const uint32_t path = guest_string(api, "GLOBAL\\ADDONCARS.BIN"), name = guest_string(api, "addon_cars");
    uint32_t cells = 0;
    if (!path || !name || api->guest_alloc(api, 16, &cells) != POP_OK || !cells)
        return;
    api->guest_write_u32(api, cells, 0);
    api->guest_write_u32(api, cells + 4, 0);
    uint32_t la[3] = {path, cells, 0x1040}, buf = 0;
    api->guest_call(api, FILE_LOAD, 0, la, 3, &buf);
    uint32_t handle = 0;
    if (buf) {
        uint32_t ma[6] = {buf, name, cells + 4, 0, get_u32(0x00d3bdb0u), 1};
        api->guest_call(api, VAULT_MOUNT, 0, ma, 6, &handle);
        uint32_t fa[1] = {buf};
        api->guest_call(api, BUFFER_FREE, 0, fa, 1, NULL);
    }
    char l[200];
    snprintf(l, sizeof l, "core.nfsu: add-on cars: vault %s (%u bytes, handle %08x); %u car types in GLOBALB",
             !buf ? "file not found" : handle ? "mounted" : "not mounted", get_u32(cells), handle, get_u32(0x00d5aae8u));
    api->log(api, l);
    if (handle) {
        g_addon_mounted = 1;
        addon_lists(api);
    }
}


/* NFSU_PRESET_TEST=<preset hash> (diagnostic): 60 s after start, FUN_00524d60 (thiscall: the car-record list, a
 * preset ride key -> a new record) is called the way the Police Career plugin calls it, and its answer logged */

static int g_preset_trace;
static void preset_trace(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    if (!g_preset_trace)
        return;
    const uint32_t which = (uint32_t)(uintptr_t)user;
    char l[300];
    if (which == 1)
        snprintf(l, sizeof l, "core.nfsu: preset trace 51d410 %s ecx %08x key %08x inst %08x [inst] %08x %08x %08x",
                 cpu->phase == POP_PHASE_AFTER ? "after" : "before", cpu->ecx, get_u32(cpu->esp + 4),
                 get_u32(cpu->esp + 8), get_u32(get_u32(cpu->esp + 8)), get_u32(get_u32(cpu->esp + 8) + 4),
                 get_u32(get_u32(cpu->esp + 8) + 8));
    else if (which == 2) {
        static uint32_t obj;
        if (cpu->phase != POP_PHASE_AFTER)
            obj = cpu->ecx;
        snprintf(l, sizeof l, "core.nfsu: preset trace 4574e0 %s obj %08x: %08x %08x %08x %08x; [[obj+4]+0xc] %08x",
                 cpu->phase == POP_PHASE_AFTER ? "after" : "before", obj, get_u32(obj), get_u32(obj + 4),
                 get_u32(obj + 8), get_u32(obj + 12), get_u32(obj + 4) > 0x10000 ? get_u32(get_u32(obj + 4) + 0xc) : 0);
    }
    else
        snprintf(l, sizeof l, "core.nfsu: preset trace 4649b0 %s ecx %08x key %08x -> eax %08x [eax+4] %08x",
                 cpu->phase == POP_PHASE_AFTER ? "after" : "before", cpu->ecx, get_u32(cpu->esp + 4), cpu->eax,
                 cpu->eax > 0x10000 ? get_u32(cpu->eax + 4) : 0);
    api->log(api, l);
}
static void preset_test(const PopModApi *api) {
    const char *e = getenv("NFSU_PRESET_TEST");
    static int64_t start;
    static int done;
    if (!e || done)
        return;
    if (!start)
        start = now_ns();
    if (now_ns() - start < (int64_t)(getenv("NFSU_PRESET_TEST_AT") ? atoi(getenv("NFSU_PRESET_TEST_AT")) : 60) * 1000000000LL)
        return;
    done = 1;
    static uint32_t th[6];
    api->hook_install(api, 0x0051d410u, preset_trace, POP_HOOK_BEFORE, (void *)1, &th[0]);
    api->hook_install(api, 0x0051d410u, preset_trace, POP_HOOK_AFTER, (void *)1, &th[1]);
    api->hook_install(api, 0x004574e0u, preset_trace, POP_HOOK_BEFORE, (void *)2, &th[2]);
    api->hook_install(api, 0x004574e0u, preset_trace, POP_HOOK_AFTER, (void *)2, &th[3]);
    g_preset_trace = 1;
    const uint32_t list = get_u32(0x00d8e1a4u) + 0x88u;
    {
        int used = 0, junk = 0;
        for (int i = 0; i < 0x14a; ++i) {
            const uint32_t r = list + 28u * (uint32_t)i;
            if (get_u32(r) == 0xffffffffu)
                continue;
            ++used;
            if (!get_u32(r + 4) && !get_u32(r + 8) && !get_u32(r + 12))
                ++junk;
            if (i >= 0xd4) {
                char l2[200];
                snprintf(l2, sizeof l2, "core.nfsu: preset test: list row %d: %08x %08x %08x %08x %08x %08x", i,
                         get_u32(r), get_u32(r + 4), get_u32(r + 8), get_u32(r + 12), get_u32(r + 16), get_u32(r + 20));
                api->log(api, l2);
            }
        }
        char l1[120];
        snprintf(l1, sizeof l1, "core.nfsu: preset test: list %08x: %d rows used, %d empty-looking", list, used, junk);
        api->log(api, l1);
    }
    uint32_t arg = (uint32_t)strtoul(e, NULL, 16), rec = 0;
    if (getenv("NFSU_PRESET_TEST_PC")) { /* the plugin's own FUN_10170840(list, vehicle, preset) */
        uint32_t a2[2] = {(uint32_t)strtoul(getenv("NFSU_PRESET_TEST_PC"), NULL, 16), arg};
        api->guest_call(api, 0x10170840u, list, a2, 2, &rec);
    } else
        api->guest_call(api, 0x00524d60u, list, &arg, 1, &rec);
    char l[300];
    snprintf(l, sizeof l, "core.nfsu: preset test %08x -> record %08x: %08x %08x %08x %08x %08x %08x %08x", arg, rec,
             rec ? get_u32(rec) : 0, rec ? get_u32(rec + 4) : 0, rec ? get_u32(rec + 8) : 0, rec ? get_u32(rec + 12) : 0,
             rec ? get_u32(rec + 16) : 0, rec ? get_u32(rec + 20) : 0, rec ? get_u32(rec + 24) : 0);
    api->log(api, l);
    for (int i = 0; i < 0x14a; ++i) {
        const uint32_t r = list + 28u * (uint32_t)i;
        if (get_u32(r) == arg || get_u32(r + 12) == arg) {
            snprintf(l, sizeof l, "core.nfsu: preset test: row %d at %08x: %08x %08x %08x %08x %08x", i, r, get_u32(r),
                     get_u32(r + 4), get_u32(r + 8), get_u32(r + 12), get_u32(r + 16));
            api->log(api, l);
        }
    }
    g_preset_trace = 0;
}

/* The car lot's rows live in the career object ([0x00d8e1a4] + 0x88: 0x14a rows of 7 dwords {slot, frontend,
 * pvehicle, preset, flags, ...}), which a loaded save replaces with its own copy - one made without the add-on
 * cars (FUN_00528e40 builds the list at boot, with them). So rows for the cars the career lacks are added the way
 * FUN_00528e40 adds one: the first free row (slot -1), slot = its index, frontend and pvehicle keys, then
 * FUN_0051d7c0 (fastcall, ECX = row) fills its flags from the frontend record. */
#define ROW_INIT 0x0051d7c0u
static void addon_dealer_sync(const PopModApi *api) {
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (!obj || !g_addon_ndealer)
        return;
    const uint32_t list = obj + 0x88u;
    int added = 0;
    for (int r = 0; r < g_addon_nrekey; ++r)
        for (int i = 0; i < 0x14a; ++i) {
            const uint32_t row = list + 28u * (uint32_t)i;
            if (get_u32(row) != 0xffffffffu && get_u32(row + 8) == g_addon_rekey[r][0]) {
                api->guest_write_u32(api, row + 8, g_addon_rekey[r][1]);
                char l[120];
                snprintf(l, sizeof l, "core.nfsu: add-on cars: row %d moved from %08x to %08x", i, g_addon_rekey[r][0],
                         g_addon_rekey[r][1]);
                api->log(api, l);
            }
        }
    for (int c = 0; c < g_addon_ndealer; ++c) {
        if (addon_off_car(g_addon_dealer[c].pv))
            continue; /* this career has its group off */
        int have = 0, free_row = -1;
        for (int i = 0; i < 0x14a; ++i) {
            const uint32_t r = list + 28u * (uint32_t)i;
            if (get_u32(r) == 0xffffffffu) {
                if (free_row < 0)
                    free_row = i;
            } else if (get_u32(r + 8) == g_addon_dealer[c].pv)
                have = 1;
        }
        if (have || free_row < 0)
            continue;
        const uint32_t r = list + 28u * (uint32_t)free_row;
        api->guest_write_u32(api, r, (uint32_t)free_row);
        api->guest_write_u32(api, r + 4, g_addon_dealer[c].fe);
        api->guest_write_u32(api, r + 8, g_addon_dealer[c].pv);
        api->guest_write_u32(api, r + 12, 0);
        api->guest_call(api, ROW_INIT, r, NULL, 0, NULL);
        ++added;
    }
    if (added) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: add-on cars: %d car lot rows added to the career", added);
        api->log(api, l);
    }
}

/* NFSU_HASH_LOG=<from>,<to> (diagnostic, seconds of the run): every distinct string the game hashes in that window
 * (bStringHash FUN_004b3ed0, and bStringHash2 FUN_004b3f00 with the hash it continues), to see which names it
 * builds for a car's parts. */
static int64_t g_hash_from, g_hash_to, g_hash_t0;
static void hash_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    const int64_t t = now_ns() - g_hash_t0;
    if (t < g_hash_from || t > g_hash_to)
        return;
    const uint32_t sp = get_u32(cpu->esp + 4), prefix = user ? get_u32(cpu->esp + 8) : 0;
    char b[72] = {0};
    for (int i = 0; sp && i < 71; ++i)
        if (!(b[i] = (char)get_u8(sp + (uint32_t)i)))
            break;
    /* NFSU_HASH_PREFIX=<hex>,<hex>...: only continuations of these hashes, and plain strings holding NFSU_HASH_TEXT */
    static uint32_t want[8];
    static int nwant = -1;
    if (nwant < 0) {
        nwant = 0;
        const char *w = getenv("NFSU_HASH_PREFIX");
        while (w && *w && nwant < 8) {
            want[nwant++] = (uint32_t)strtoul(w, (char **)&w, 16);
            if (*w == ',')
                ++w;
        }
    }
    if (nwant) {
        int ok = 0;
        for (int i = 0; i < nwant; ++i)
            ok |= user && prefix == want[i];
        const char *tx = getenv("NFSU_HASH_TEXT");
        if (!ok && !(tx && !user && strstr(b, tx)))
            return;
    }
    static uint32_t seen[4096];
    static int n;
    uint32_t h = prefix * 31u + 7u;
    for (const char *c = b; *c; ++c)
        h = h * 33u + (uint8_t)*c;
    for (int i = 0; i < n; ++i)
        if (seen[i] == h)
            return;
    if (n >= 4096)
        return;
    seen[n++] = h;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: hash%s \"%s\" %08x from %08x", user ? "2" : "", b, prefix, get_u32(cpu->esp));
    api->log(api, l);
}
/* NFSU_SOLID_KEYS=<hex>,...: what the solid lookup FUN_0053f3b0(name hash) answers for these keys (each answer once) */
static uint32_t g_solid_keys[8];
static int g_nsolid;
static void solid_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t k = get_u32(cpu->esp + 4);
    api->call_next(api, inv, cpu);
    for (int i = 0; i < g_nsolid; ++i)
        if (g_solid_keys[i] == k) {
            static uint32_t seen[64][2];
            static int n;
            for (int j = 0; j < n; ++j)
                if (seen[j][0] == k && seen[j][1] == cpu->eax)
                    return;
            if (n < 64)
                seen[n][0] = k, seen[n][1] = cpu->eax, ++n;
            char l[120];
            snprintf(l, sizeof l, "core.nfsu: solid %08x -> %08x (t %lld s)", k, cpu->eax, (long long)((now_ns() - g_hash_t0) / 1000000000LL));
            api->log(api, l);
        }
}
/* NFSU_PRINTF_LOG=<from>,<to> (diagnostic): the game's debug printf FUN_004b6e20 (silent in the shipped game) -
 * its messages in that window of the run, %s/%d/%x/%u args filled in (each distinct line once) */
static int64_t g_pf_from, g_pf_to;
static void pf_log(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const int64_t t = now_ns() - g_hash_t0;
    if (t < g_pf_from || t > g_pf_to)
        return;
    char fmt[200] = {0}, out[400] = {0};
    const uint32_t fp = get_u32(cpu->esp + 4);
    for (int i = 0; fp && i < 199; ++i)
        if (!(fmt[i] = (char)get_u8(fp + (uint32_t)i)))
            break;
    uint32_t ap = cpu->esp + 8;
    size_t n = 0;
    for (const char *c = fmt; *c && n < sizeof out - 64; ++c) {
        if (*c != '%') {
            out[n++] = *c;
            continue;
        }
        ++c;
        while (*c && strchr("0123456789.-+ #l", *c))
            ++c;
        const uint32_t v = get_u32(ap);
        ap += 4;
        if (*c == 's') {
            for (int i = 0; v && i < 60 && n < sizeof out - 2; ++i) {
                const char ch = (char)get_u8(v + (uint32_t)i);
                if (!ch)
                    break;
                out[n++] = ch;
            }
        } else if (*c == 'x' || *c == 'X' || *c == 'p')
            n += (size_t)snprintf(out + n, sizeof out - n, "%x", v);
        else if (*c == 'f') {
            n += (size_t)snprintf(out + n, sizeof out - n, "<f>");
            ap += 4;
        } else
            n += (size_t)snprintf(out + n, sizeof out - n, "%d", (int)v);
        if (!*c)
            break;
    }
    static uint32_t seen[2048];
    static int ns;
    uint32_t h = 5381;
    for (const char *c = out; *c; ++c)
        h = h * 33u + (uint8_t)*c;
    for (int i = 0; i < ns; ++i)
        if (seen[i] == h)
            return;
    if (ns < 2048)
        seen[ns++] = h;
    char l[480];
    snprintf(l, sizeof l, "core.nfsu: game says [%lld s, from %08x]: %s", (long long)(t / 1000000000LL), get_u32(cpu->esp), out);
    api->log(api, l);
}
static void hash_log_install(const PopModApi *api) {
    const char *pf = getenv("NFSU_PRINTF_LOG");
    if (pf) {
        int a = 0, b = 0;
        if (sscanf(pf, "%d,%d", &a, &b) == 2) {
            g_hash_t0 = now_ns();
            g_pf_from = (int64_t)a * 1000000000LL, g_pf_to = (int64_t)b * 1000000000LL;
            static uint32_t ph;
            api->hook_install(api, 0x004b6e20u, pf_log, POP_HOOK_BEFORE, NULL, &ph);
        }
    }
    const char *sk = getenv("NFSU_SOLID_KEYS");
    if (sk) {
        g_hash_t0 = now_ns();
        while (*sk && g_nsolid < 8) {
            g_solid_keys[g_nsolid++] = (uint32_t)strtoul(sk, (char **)&sk, 16);
            if (*sk == ',')
                ++sk;
        }
        static uint32_t sh;
        api->hook_install(api, 0x0053f3b0u, solid_log, POP_HOOK_REPLACE, NULL, &sh);
    }
    const char *e = getenv("NFSU_HASH_LOG");
    if (!e)
        return;
    int a = 0, b = 0;
    if (sscanf(e, "%d,%d", &a, &b) != 2)
        return;
    g_hash_from = (int64_t)a * 1000000000LL, g_hash_to = (int64_t)b * 1000000000LL, g_hash_t0 = now_ns();
    static uint32_t h[2];
    api->hook_install(api, 0x004b3ed0u, hash_log, POP_HOOK_BEFORE, NULL, &h[0]);
    api->hook_install(api, 0x004b3f00u, hash_log, POP_HOOK_BEFORE, (void *)1, &h[1]);
}

/* NFSU_ADDON_NAMECHECK=1 (diagnostic): after the profile loads, ask the game for each add-on car's name label
 * (FUN_00525070(frontend key) -> label hash) and log the strings it hashes on the way (FUN_005d5cb0). */
static int g_namecheck_on;
static void namecheck_hash(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    if (!g_namecheck_on)
        return;
    const uint32_t sp = get_u32(cpu->esp + 4);
    char b[96] = {0};
    for (int i = 0; sp && i < 95; ++i)
        if (!(b[i] = (char)get_u8(sp + (uint32_t)i)))
            break;
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: add-on cars: name check hashes \"%s\"", b);
    api->log(api, l);
}
static void addon_namecheck(const PopModApi *api) {
    static int done;
    if (done || !getenv("NFSU_ADDON_NAMECHECK") || !get_u32(0x00d8e1a4u))
        return;
    done = 1;
    static uint32_t h;
    api->hook_install(api, 0x005d5cb0u, namecheck_hash, POP_HOOK_BEFORE, NULL, &h);
    for (int c = 0; c < g_addon_ndealer; ++c) {
        uint32_t arg = g_addon_dealer[c].fe, hash = 0;
        g_namecheck_on = 1;
        api->guest_call(api, 0x00525070u, 0, &arg, 1, &hash);
        g_namecheck_on = 0;
        int known = 0;
        for (int i = 0; i < g_addon_nnames; ++i)
            known |= g_addon_names[i].hash == hash;
        char l[160];
        snprintf(l, sizeof l, "core.nfsu: add-on cars: name check fe %08x -> label %08x (%s)", arg, hash,
                 known ? "ours" : "not ours");
        api->log(api, l);
        uint32_t lab = g_addon_dealer[c].label, txt = 0;
        api->guest_call(api, TEXT_LOOKUP, 0, &lab, 1, &txt);
        char b[48] = {0};
        for (int i = 0; txt && i < 47; ++i)
            if (!(b[i] = (char)get_u8(txt + (uint32_t)i)))
                break;
        snprintf(l, sizeof l, "core.nfsu: add-on cars: text of %08x -> %08x \"%s\" (hook calls %u, ours %u)", lab, txt, b,
                 g_addon_text_calls, g_addon_text_hits);
        api->log(api, l);
    }
}

/* each main-loop pass: car lists not reached at the mount (their collection not loaded yet) */
/* A career whose active car is an add-on car whose switch is off (2026-11-21, the person): the career switches to the
 * Nissan 240SX - a car every career has - so the save loads and drives. The active car is the user-data value
 * 0x307d9226 (a car row id, FUN_00820c70 reads it, FUN_00821e30 writes it); the rows are [0x00d8e1a4] + 0x88, 28 bytes
 * ({id, frontend, pvehicle, ...}, byte +0x15 < 0x47 = owned). The add-on cars of switches that are off are the
 * "dealer" pvehicles of their groups in addoncars.txt. */
/* 2026-11-24 (the person: "these must be per save and work at once or at least when the save is chosen, not after
 * a restart"): every add-on car is in the game from the start (launch.c reads their files always); which ones a
 * career sees follows its save's switches - group 0 mod_addon_cars, group 1 mod_porsche_pack - read live. A car of a
 * group that is off: no car-lot row is added for it, the car lot's and garages' queries leave it out
 * (addon_query_filter), and a career driving one is given a game car. */
static uint32_t g_addon_off[160];  /* every add-on car's pvehicle ("dealer" lines) */
static uint8_t g_addon_group[160]; /* its group */
static int g_addon_noff;
#define PV_NISSAN_240SX 0xd3ff1140u
static void addon_groups_refresh(const PopModApi *api) {
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 200000000LL)
        return;
    at = t;
    int64_t a = 0, p = 0;
    if (api->settings_get) {
        api->settings_get(api, "mod_addon_cars", &a);
        api->settings_get(api, "mod_porsche_pack", &p);
    }
    if (a != g_addon_group_on[0] || p != g_addon_group_on[1]) {
        char l[120];
        snprintf(l, sizeof l, "core.nfsu: add-on cars for this career: add-ons %s, Porsche Pack %s", a ? "on" : "off",
                 p ? "on" : "off");
        api->log(api, l);
    }
    g_addon_group_on[0] = a != 0, g_addon_group_on[1] = p != 0;
}
static int addon_off_car(uint32_t pv) {
    for (int i = 0; i < g_addon_noff; ++i)
        if (g_addon_off[i] == pv)
            return !g_addon_group_on[g_addon_group[i] & 1];
    return 0;
}
static int addon_row_off(uint32_t id) { /* a career row id whose car is an add-on that is off */
    const uint32_t obj = get_u32(0x00d8e1a4u);
    for (uint32_t i = 0; obj && i < 0x14au; ++i)
        if (get_u32(obj + 0x88u + 28u * i) == id)
            return addon_off_car(get_u32(obj + 0x88u + 28u * i + 8u));
    return 0;
}
static uint32_t addon_pick_car(uint32_t obj);
static void addon_write_active(const PopModApi *api, uint32_t names, uint32_t id, uint32_t pv, uint32_t pick);
static int g_addon_guard_inside;
/* With Police Career on the plugin makes its police cars' rows itself (and moves the active car), so a missing row is
 * left to it; off, nobody will make it. */
int pc_police_off_now(const PopModApi *api);
static int pc_row_is_police(uint32_t id);
static int addon_police_on(const PopModApi *api) {
    return !pc_police_off_now(api);
}
/* the career's rows are loaded: some owned car exists (before a save loads they are all free) */
static int addon_rows_ready(uint32_t obj) {
    for (uint32_t i = 0; i < 0x14au; ++i) {
        const uint32_t r = obj + 0x88u + 28u * i;
        if (get_u32(r) != 0xffffffffu && get_u32(r + 8u) && get_u8(r + 0x15u) < 0x47u)
            return 1;
    }
    return 0;
}
/* (2026-11-23, the person turned the add-on cars off with one active: grey screen, then the game quit) The guard
 * below wrote the replacement only on its next 0.5 s turn - after the world had already asked for the active car and
 * found no such vehicle: no player car, and FUN_0042b650 called methods of a null car. Every read of the active car
 * (FUN_00820c70 "UserData"/"UDKey"/"UDValue", key 0x307d9226; cdecl) now gets the replacement at once (AFTER hook);
 * the guard still writes it into the career. */
static void addon_active_read(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (getenv("NFSU_ACTIVE_LOG") && get_u32(cpu->esp + 12) == 0x307d9226u) {
        static uint32_t said = 0xfffffffeu;
        if (said != cpu->eax) {
            said = cpu->eax;
            const uint32_t obj = get_u32(0x00d8e1a4u);
            uint32_t pv = 0, fl = 0;
            for (uint32_t i = 0; obj && i < 0x14au; ++i)
                if (get_u32(obj + 0x88u + 28u * i) == cpu->eax)
                    pv = get_u32(obj + 0x88u + 28u * i + 8u), fl = get_u32(obj + 0x88u + 28u * i + 16u);
            char l[140];
            snprintf(l, sizeof l, "core.nfsu: active car read: %08x pv %08x flags %08x (noff %d)", cpu->eax, pv, fl,
                     g_addon_noff);
            api->log(api, l);
        }
    }
    if (g_addon_guard_inside || get_u32(cpu->esp + 12) != 0x307d9226u || cpu->eax == 0xffffffffu)
        return;
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (!obj || get_u32(obj + 0x88u) == 0 || !addon_rows_ready(obj))
        return;
    for (uint32_t i = 0; i <= 0x14au; ++i)
        if (i == 0x14au || get_u32(obj + 0x88u + 28u * i) == cpu->eax) {
            /* no such row (2026-11-23: active car 0x1b5 with Police Career off - a police car the plugin keeps in its
             * own data), or an add-on car whose switch is off */
            if (i < 0x14au ? !(addon_off_car(get_u32(obj + 0x88u + 28u * i + 8u)) ||
                               (pc_police_off_now(api) && pc_row_is_police(cpu->eax) == 1))
                           : addon_police_on(api))
                return;
            const uint32_t pick = addon_pick_car(obj);
            if (pick != 0xffffffffu) {
                static int said;
                if (said++ < 3) {
                    char l[120];
                    snprintf(l, sizeof l, "core.nfsu: active car %08x is an add-on that is off: the game gets %08x", cpu->eax,
                             pick);
                    api->log(api, l);
                }
                cpu->eax = pick;
            }
            return;
        }
}
static void addon_active_guard(const PopModApi *api) {
    if (!api->guest_call)
        return;
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 500000000LL)
        return;
    at = t;
    const uint32_t obj = get_u32(0x00d8e1a4u);
    if (!obj || !get_u32(0x0128461cu) || !get_u32(get_u32(0x0128461cu) + 4))
        return;
    static uint32_t names;
    if (!names) {
        if (!api->guest_alloc || api->guest_alloc(api, 32, &names) != POP_OK || !names)
            return;
        static const char tx[32] = "UserData\0\0\0\0UDKey\0\0\0UDValue";
        for (uint32_t i = 0; i < 32; i += 4) {
            uint32_t w = 0;
            for (uint32_t b = 0; b < 4; ++b)
                w |= (uint32_t)(uint8_t)tx[i + b] << (8 * b);
            api->guest_write_u32(api, names + i, w);
        }
    }
    const uint32_t qa[4] = {names, names + 12, names + 20, 0x307d9226u};
    uint32_t id = 0xffffffffu;
    g_addon_guard_inside = 1;
    const PopModStatus qs = api->guest_call(api, 0x00820c70u, 0, qa, 4, &id);
    g_addon_guard_inside = 0;
    if (qs != POP_OK || id == 0xffffffffu)
        return;
    uint32_t pv = 0;
    for (uint32_t i = 0; i < 0x14au; ++i)
        if (get_u32(obj + 0x88u + 28u * i) == id) {
            pv = get_u32(obj + 0x88u + 28u * i + 8u);
            break;
        }
    if (getenv("NFSU_ACTIVE_LOG")) {
        static uint32_t said = 0xfffffffeu;
        if (said != id) {
            said = id;
            char l[160];
            for (uint32_t i = 0; i < 0x14au; ++i) {
                const uint32_t r = obj + 0x88u + 28u * i;
                if (get_u32(r) != id)
                    continue;
                snprintf(l, sizeof l, "core.nfsu: active car %08x row: %08x %08x %08x %08x %08x %08x %08x", id,
                         get_u32(r), get_u32(r + 4), get_u32(r + 8), get_u32(r + 12), get_u32(r + 16), get_u32(r + 20),
                         get_u32(r + 24));
                api->log(api, l);
            }
        }
    }
    const int police_off_car = pv && pc_police_off_now(api) && pc_row_is_police(id) == 1;
    if (pv ? (!addon_off_car(pv) && !police_off_car) : (!addon_rows_ready(obj) || addon_police_on(api)))
        return;
    const uint32_t pick = addon_pick_car(obj);
    if (pick == 0xffffffffu)
        return;
    addon_write_active(api, names, id, pv, pick);
}
/* the replacement: an owned 240SX, else any owned car of the game, else the 240SX row */
static uint32_t addon_pick_car(uint32_t obj) {
    uint32_t pick = 0xffffffffu, owned_other = 0xffffffffu, any240 = 0xffffffffu;
    for (uint32_t i = 0; i < 0x14au; ++i) {
        const uint32_t r = obj + 0x88u + 28u * i, rid = get_u32(r), rpv = get_u32(r + 8u);
        if (rid == 0xffffffffu || !rpv)
            continue;
        const int owned = get_u8(r + 0x15u) < 0x47u;
        if (rpv == PV_NISSAN_240SX) {
            if (owned) {
                pick = rid;
                break;
            }
            any240 = rid;
        } else if (owned && owned_other == 0xffffffffu && !addon_off_car(rpv)) {
            owned_other = rid;
        }
    }
    if (pick == 0xffffffffu)
        pick = owned_other != 0xffffffffu ? owned_other : any240;
    return pick;
}
static void addon_write_active(const PopModApi *api, uint32_t names, uint32_t id, uint32_t pv, uint32_t pick) {
    /* FUN_00821e30 (a script call, op 0x4b) ADDS its value to the stored one (measured 2026-11-23: 0x1b5 + 0xd7 ->
     * 0x28c, each 0.5 s): the difference makes it the replacement */
    const uint32_t sa[5] = {names, names + 12, names + 20, 0x307d9226u, pick - id};
    uint32_t r = 0;
    const PopModStatus st = api->guest_call(api, 0x00821e30u, 0, sa, 5, &r);
    char l[160];
    snprintf(l, sizeof l, "core.nfsu: the career's car %08x (pv %08x) is an add-on that is off: car %08x instead (%s)", id,
             pv, pick, st == POP_OK ? "ok" : "REFUSED");
    api->log(api, l);
}

static void addon_pass(const PopModApi *api) {
    preset_test(api);
    addon_groups_refresh(api);
    addon_active_guard(api);
    if (g_addon_on != 1 || !g_addon_mounted)
        return;
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 2000000000LL)
        return;
    at = t;
    addon_dealer_sync(api);
    addon_namecheck(api);
    for (int i = 0; i < g_addon_nlists; ++i)
        if (!g_addon_lists[i].done) {
            addon_lists(api);
            return;
        }
}


/* the dealer's stock list (FUN_00528e40, ECX = [0x00d8e1a4] + 0x88: 0x14a rows of 7 dwords {slot or -1, key, key,
 * ...}) as built: logged once with its last rows, to see whether the add-on cars are in it */
#define DEALER_BUILD 0x00528e40u
static void addon_dealer(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv, (void)user;
    const uint32_t list = get_u32(0x00d8e1a4u) + 0x88u;
    int used = 0, last = -1;
    for (int i = 0; i < 0x14a; ++i)
        if (get_u32(list + 28u * (uint32_t)i) != 0xffffffffu)
            ++used, last = i;
    char l[600];
    int n = snprintf(l, sizeof l, "core.nfsu: add-on cars: dealer list built (ecx %08x): %d rows; last:", cpu->ecx,
                     used);
    api->log(api, l);
    /* every row whose pvehicle is CCX (0xa0cfe1ee) or an add-on, all 7 dwords */
    for (int i = 0; i < 0x14a; ++i) {
        const uint32_t r = list + 28u * (uint32_t)i, pv = get_u32(r + 8);
        if (pv == 0xa0cfe1eeu || i > last - 6) {
            n = snprintf(l, sizeof l, "core.nfsu: add-on cars: dealer row %d:", i);
            for (int k = 0; k < 7; ++k)
                n += snprintf(l + n, sizeof l - n, " %08x", get_u32(r + 4u * (uint32_t)k));
            api->log(api, l);
        }
    }
}

static void addon_cars_install(const PopModApi *api) {
    /* two switches over one file (2026-11-20): "in addon" lines follow mod_addon_cars, "in porsche" lines the
     * Porsche Pack's mod_porsche_pack; the vault (every car's collections) mounts when either is on */
    int64_t on = 0, porsche = 0;
    if (api->settings_get) {
        api->settings_get(api, "mod_addon_cars", &on);
        api->settings_get(api, "mod_porsche_pack", &porsche);
    }
    if (getenv("NFSU_ADDON_CARS"))
        on = atoi(getenv("NFSU_ADDON_CARS"));
    if (getenv("NFSU_PORSCHE_PACK"))
        porsche = atoi(getenv("NFSU_PORSCHE_PACK"));
    (void)on, (void)porsche;
    g_addon_on = 1; /* always: the switches are per save and live (addon_groups_refresh) */
    const char *file = getenv("RECOMP_NFSU_ADDON_CARS");
    if (!file)
        return; /* read even with both switches off: the cars of a switch that is off are needed (guard above) */
    int group_on = 1;
    FILE *f = fopen(file, "rb");
    if (!f) {
        api->log(api, "core.nfsu: add-on cars: addoncars.txt not found");
        return;
    }
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char kind[16] = "", a[48] = "", b[48] = "", c[48] = "", d[48] = "", e[48] = "";
        if (sscanf(line, "%15s", kind) != 1)
            continue;
        if (!strcmp(kind, "in") && sscanf(line, "%15s %47s", kind, a) == 2) {
            group_on = !strcmp(a, "porsche") ? 1 : 0; /* the group number of the lines below */
            continue;
        }
        {
            unsigned pv = 0;
            if (!strncmp(line, "dealer ", 7) && sscanf(line + 7, "%x", &pv) == 1 && g_addon_noff < 160) {
                g_addon_group[g_addon_noff] = (uint8_t)group_on;
                g_addon_off[g_addon_noff++] = pv;
            }
        }
        if (!strcmp(kind, "list") && sscanf(line, "%15s %47s %47s %47s %47s %47s", kind, a, b, c, d, e) == 6 &&
            g_addon_nlists < 240) {
            AddonListAdd *x = &g_addon_lists[g_addon_nlists++];
            memset(x, 0, sizeof *x);
            snprintf(x->coll, sizeof x->coll, "%s", b);
            snprintf(x->field, sizeof x->field, "%s", c);
            snprintf(x->cls, sizeof x->cls, "%s", d);
            snprintf(x->car, sizeof x->car, "%s", e);
        } else if (!strcmp(kind, "region")) {
            unsigned n = 0, lab = 0;
            if (sscanf(line, "%15s %u %x", kind, &n, &lab) == 3 && n == 3)
                g_region_n = n, g_region_label = lab;
        } else if (!strcmp(kind, "regioncar") && g_region_ncars < 16) {
            unsigned pv = 0;
            if (sscanf(line, "%15s %x", kind, &pv) == 2)
                g_region_cars[g_region_ncars++] = pv;
        } else if (!strcmp(kind, "rekey") && g_addon_nrekey < 8) {
            unsigned o = 0, n = 0;
            if (sscanf(line, "%15s %x %x", kind, &o, &n) == 3)
                g_addon_rekey[g_addon_nrekey][0] = o, g_addon_rekey[g_addon_nrekey++][1] = n;
        } else if (!strcmp(kind, "dealer") && g_addon_ndealer < 96) {
            unsigned pv = 0, fe = 0, lab = 0;
            if (sscanf(line, "%15s %x %x %x", kind, &pv, &fe, &lab) >= 3) {
                g_addon_dealer[g_addon_ndealer].pv = pv, g_addon_dealer[g_addon_ndealer].fe = fe;
                g_addon_dealer[g_addon_ndealer++].label = lab;
            }
        } else if (!strcmp(kind, "name") && g_addon_nnames < 240) {
            unsigned hash = 0;
            int at = 0;
            if (sscanf(line, "%15s %x %n", kind, &hash, &at) >= 2 && at > 0) {
                g_addon_names[g_addon_nnames].hash = hash;
                snprintf(g_addon_names[g_addon_nnames].host, sizeof g_addon_names[0].host, "%s", line + at);
                ++g_addon_nnames;
            }
        }
    }
    fclose(f);
    if (!g_addon_on) {
        char l0[120];
        snprintf(l0, sizeof l0, "core.nfsu: add-on cars off: %d cars watched for a career that uses one", g_addon_noff);
        api->log(api, l0);
        return;
    }
    hash_log_install(api);
    static uint32_t hooks[2];
    static uint32_t dhook;
    api->hook_install(api, DEALER_BUILD, addon_dealer, POP_HOOK_AFTER, NULL, &dhook);
    const PopModStatus h0 = api->hook_install(api, CARS_VAULT_LOAD, addon_mount, POP_HOOK_AFTER, NULL, &hooks[0]);
    const PopModStatus h1 =
        g_addon_nnames ? api->hook_install(api, TEXT_LOOKUP, addon_text, POP_HOOK_REPLACE, NULL, &hooks[1]) : POP_OK;
    static uint32_t nhook;
    const PopModStatus h2 = api->hook_install(api, CAR_NAME_LABEL, addon_name_label, POP_HOOK_REPLACE, NULL, &nhook);
    apt_trace_install(api);
    if (getenv("NFSU_SHOP_TRACE")) {
        static uint32_t th[4];
        api->hook_install(api, 0x005b3bb0u, shop_trace, POP_HOOK_BEFORE, (void *)"fill_b3bb0", &th[0]);
        api->hook_install(api, 0x005b5010u, shop_trace, POP_HOOK_BEFORE, (void *)"fill_b5010", &th[1]);
        api->hook_install(api, 0x0052f570u, shop_trace, POP_HOOK_BEFORE, (void *)"push", &th[2]);
        api->hook_install(api, 0x00581750u, shop_trace, POP_HOOK_BEFORE, (void *)"shop_ctor", &th[3]);
    }
    if (g_region_n) {
        static uint32_t rh[1];
        const PopModStatus r0 = api->hook_install(api, 0x00597340u, addon_region_tree, POP_HOOK_REPLACE, NULL, &rh[0]);
        static uint32_t ph;
        const PopModStatus r1 = api->hook_install(api, 0x00575d30u, addon_region_pick, POP_HOOK_REPLACE, NULL, &ph);
        static uint32_t pu, sp;
        api->hook_install(api, 0x0052f570u, addon_region_push, POP_HOOK_BEFORE, NULL, &pu);
        api->hook_install(api, 0x005b5010u, addon_region_split, POP_HOOK_REPLACE, NULL, &sp);
        char l3[120];
        snprintf(l3, sizeof l3, "core.nfsu: shop section %u hooks: menu rows %d, choice %d (%d cars)", g_region_n,
                 (int)r0, (int)r1, g_region_ncars);
        api->log(api, l3);
    }
    char l[200];
    snprintf(l, sizeof l, "core.nfsu: add-on cars on: %d car list entries, %d names (hooks %s, %s, %s)", g_addon_nlists,
             g_addon_nnames, h0 == POP_OK ? "ok" : "FAILED", h1 == POP_OK ? "ok" : "FAILED", h2 == POP_OK ? "ok" : "FAILED");
    api->log(api, l);
}

/* The car lot's and the garages' car lists come from the game's queries (FUN_00519850, thiscall (store 0x00d2a178,
 * query, -1); a car query keeps 36-byte entries in a vector at query + 8 / + 0xc, + 0x18 the career row). After it
 * runs, the entries of add-on cars whose group is off for this career are taken out (2026-11-24, per-save switches),
 * so they are neither sold nor listed in a garage. */
static uint32_t g_aq_stack[8];
static int g_aq_depth;
static void addon_query_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (g_aq_depth < 8)
        g_aq_stack[g_aq_depth] = get_u32(cpu->esp + 4);
    ++g_aq_depth;
}
static void addon_query_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu, (void)inv, (void)user;
    if (g_aq_depth <= 0)
        return;
    --g_aq_depth;
    if (g_aq_depth >= 8 || (g_addon_group_on[0] && g_addon_group_on[1]))
        return;
    const uint32_t q = g_aq_stack[g_aq_depth];
    if (!q)
        return;
    const uint32_t b = get_u32(q + 8), e = get_u32(q + 0xc);
    if (!b || e <= b || (e - b) % 36u || (e - b) / 36u > 2000u)
        return;
    uint32_t w = b;
    int removed = 0;
    for (uint32_t r = b; r < e; r += 36u) {
        if (addon_row_off(get_u32(r + 0x18))) {
            ++removed;
            continue;
        }
        if (w != r)
            for (uint32_t k = 0; k < 36u; k += 4)
                api->guest_write_u32(api, w + k, get_u32(r + k));
        w += 36u;
    }
    if (removed) {
        api->guest_write_u32(api, q + 0xc, w);
        static int said;
        if (said++ < 6) {
            char l[100];
            snprintf(l, sizeof l, "core.nfsu: add-on cars off for this career: %d left out of a car list", removed);
            api->log(api, l);
        }
    }
}
static void addon_query_install(const PopModApi *api) {
    static uint32_t h1, h2;
    if (api->hook_install(api, 0x00519850u, addon_query_before, POP_HOOK_BEFORE, NULL, &h1) != POP_OK ||
        api->hook_install(api, 0x00519850u, addon_query_after, POP_HOOK_AFTER, NULL, &h2) != POP_OK)
        api->log(api, "core.nfsu: add-on car list filter NOT installed");
}
