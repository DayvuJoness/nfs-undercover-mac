/* The game's attribute database, read and written from core.nfsu (2026-11-19) - the layer that applies VltEd mod
 * scripts (.nfsms "update_field class collection field value" lines) to the live database, switched by a setting.
 *
 * What the game does (nfs.exe 1.0.0.1 listings):
 * - names are hashed with Bob Jenkins' lookup2, initval 0xABCDEF00 (checked: "pvehicle", "default" ... as immediates);
 * - the database is [0x00d834d0]; its classes are the hash table at [db+4]+8 (FUN_00464a20 -> FUN_00463be0);
 * - a class's collections are the hash table at [cls+4]+0x14 (FUN_004645d0);
 * - a hash table is {entries, u16 count, u16 used, ...}: 12-byte entries {key, value, u8 probes at +8}, the slot
 *   key % count, linear probing for the first slot's probe count; an entry whose value points at itself is empty;
 * - a collection's own fields are its own table (rotated-key variant: shift at +0xb; FUN_00463280/004636d0, the
 *   natives in native/nfsu_native.c), then each table's +0xc, then the class's layout table [[coll+0x14]+4]+8;
 *   a field node is {key, value, u16 type index, u8 probes, u8 flags}; flags 0x80 used, 0x02 array, 0x40 value in
 *   the node, 0x10 value at the collection's layout [coll+0x18] + value, 0x20 at the class layout
 *   [[[coll+0x14]+4]+0x2c] + value, else value is a pointer (FUN_00464680, the data getter);
 * - an array is {u16 alloc, u16 count, u16 element size, u16 type flags}, elements at +8 (+8 more when the flags'
 *   bit 15 is set);
 * - type records are 16 bytes at [0x00d834ec] + 16 * type index; +0xc & 0xffffff is the size. */

#define ATTRIB_DB 0x00d834d0u
#define ATTRIB_TYPES 0x00d834ecu

static uint32_t attrib_hash(const char *s) {
    const uint8_t *k = (const uint8_t *)s;
    uint32_t len = (uint32_t)strlen(s), a = 0x9e3779b9u, b = 0x9e3779b9u, c = 0xABCDEF00u, n = len;
#define ATTRIB_MIX(a, b, c)                                                                                          \
    do {                                                                                                             \
        a -= b; a -= c; a ^= (c >> 13);                                                                              \
        b -= c; b -= a; b ^= (a << 8);                                                                               \
        c -= a; c -= b; c ^= (b >> 13);                                                                              \
        a -= b; a -= c; a ^= (c >> 12);                                                                              \
        b -= c; b -= a; b ^= (a << 16);                                                                              \
        c -= a; c -= b; c ^= (b >> 5);                                                                               \
        a -= b; a -= c; a ^= (c >> 3);                                                                               \
        b -= c; b -= a; b ^= (a << 10);                                                                              \
        c -= a; c -= b; c ^= (b >> 15);                                                                              \
    } while (0)
    while (n >= 12) {
        a += k[0] | k[1] << 8 | k[2] << 16 | (uint32_t)k[3] << 24;
        b += k[4] | k[5] << 8 | k[6] << 16 | (uint32_t)k[7] << 24;
        c += k[8] | k[9] << 8 | k[10] << 16 | (uint32_t)k[11] << 24;
        ATTRIB_MIX(a, b, c);
        k += 12;
        n -= 12;
    }
    c += len;
    switch (n) {
    case 11: c += (uint32_t)k[10] << 24; /* fall through */
    case 10: c += (uint32_t)k[9] << 16; /* fall through */
    case 9: c += (uint32_t)k[8] << 8; /* fall through */
    case 8: b += (uint32_t)k[7] << 24; /* fall through */
    case 7: b += (uint32_t)k[6] << 16; /* fall through */
    case 6: b += (uint32_t)k[5] << 8; /* fall through */
    case 5: b += k[4]; /* fall through */
    case 4: a += (uint32_t)k[3] << 24; /* fall through */
    case 3: a += (uint32_t)k[2] << 16; /* fall through */
    case 2: a += (uint32_t)k[1] << 8; /* fall through */
    case 1: a += k[0];
    }
    ATTRIB_MIX(a, b, c);
#undef ATTRIB_MIX
    return c;
}
/* "0x1234abcd" names a hash directly (VltEd writes unnamed collections so) */
static uint32_t attrib_key(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && strlen(s) == 10) {
        char *end = NULL;
        const unsigned long v = strtoul(s + 2, &end, 16);
        if (end && !*end)
            return (uint32_t)v;
    }
    return attrib_hash(s);
}

static uint32_t get_u16(uint32_t a) {
    return (get_u32(a & ~3u) >> (8 * (a & 2))) & 0xffffu;
}
static uint32_t get_u8(uint32_t a) {
    return (get_u32(a & ~3u) >> (8 * (a & 3))) & 0xffu;
}

/* FUN_00463be0's table: the value for key, or 0 */
static uint32_t attrib_table_value(uint32_t t, uint32_t key) {
    if (!t)
        return 0;
    const uint32_t n = get_u16(t + 4);
    if (!n || !get_u16(t + 6))
        return 0;
    const uint32_t ents = get_u32(t);
    uint32_t slot = key % n;
    const uint32_t probes = get_u32(ents + slot * 12u + 8);
    /* the game checks the first slot and then `probes` more (FUN_00463be0) */
    for (uint32_t i = 0; i <= probes && i < n; ++i) {
        const uint32_t e = ents + slot * 12u;
        const uint32_t v = get_u32(e + 4);
        if (v != e && get_u32(e) == key)
            return v;
        slot = (slot + 1u) % n;
    }
    return 0;
}
static uint32_t attrib_collection(uint32_t cls_key, uint32_t coll_key) {
    const uint32_t db = get_u32(ATTRIB_DB);
    if (!db)
        return 0;
    const uint32_t cls = attrib_table_value(get_u32(db + 4) + 8, cls_key);
    if (!cls)
        return 0;
    return attrib_table_value(get_u32(cls + 4) + 0x14, coll_key);
}
/* a field table (FUN_00463280): the node for key, or 0 */
static uint32_t attrib_node_in(uint32_t t, uint32_t key) {
    const uint32_t n = get_u16(t + 4);
    if (!n)
        return 0;
    const uint32_t ents = get_u32(t);
    const unsigned sh = get_u8(t + 0xb) & 31u;
    const uint32_t h = sh ? (key << sh) | (key >> (32u - sh)) : key;
    uint32_t slot = h % n;
    const uint32_t probes = get_u8(ents + slot * 12u + 0xa);
    for (uint32_t i = 0; i <= probes && i < n; ++i) {
        const uint32_t e = ents + slot * 12u;
        if ((get_u8(e + 0xb) & 0x80u) && get_u32(e) == key)
            return e;
        slot = (slot + 1u) % n;
    }
    return 0;
}
/* the collection's own node for key (not a parent's, not the class layout's: writing those would change other
 * collections too); *where says which table had it: 0 own, 1 a parent, 2 the class layout, -1 none */
static uint32_t attrib_node(uint32_t coll, uint32_t key, int *where) {
    *where = -1;
    int depth = 0;
    for (uint32_t t = coll; t && depth < 16; t = get_u32(t + 0xc), ++depth) {
        const uint32_t e = attrib_node_in(t, key);
        if (e) {
            *where = depth ? 1 : 0;
            return e;
        }
    }
    if (get_u32(coll + 0x18)) {
        const uint32_t e = attrib_node_in(get_u32(get_u32(coll + 0x14) + 4) + 8, key);
        if (e) {
            *where = 2;
            return e;
        }
    }
    return 0;
}
/* FUN_00464680: where a field's value (or its array header) lives */
static uint32_t attrib_value_addr(uint32_t coll, uint32_t node) {
    const uint32_t fl = get_u8(node + 0xb);
    if (fl & 0x40u)
        return node + 4;
    if (fl & 0x10u)
        return get_u32(coll + 0x18) + get_u32(node + 4);
    if (fl & 0x20u)
        return get_u32(get_u32(get_u32(coll + 0x14) + 4) + 0x2c) + get_u32(node + 4);
    return get_u32(node + 4);
}
static uint32_t attrib_type_size(uint32_t node) {
    const uint32_t rec = get_u32(ATTRIB_TYPES) + 16u * get_u16(node + 8);
    return get_u32(rec + 0xc) & 0xffffffu;
}

/* NFSU_ATTRIB_DUMP="class/collection/field,..." (diagnostic): what the game holds for each. */
static void attrib_dump(const PopModApi *api) {
    const char *spec = getenv("NFSU_ATTRIB_DUMP");
    if (!spec || !get_u32(ATTRIB_DB))
        return;
    static int done;
    static int64_t start;
    if (!start)
        start = now_ns();
    const char *at = getenv("NFSU_ATTRIB_DUMP_AT");
    if (done || now_ns() - start < (int64_t)(at ? atoi(at) : 30) * 1000000000LL)
        return;
    done = 1;
    {
        const uint32_t db = get_u32(ATTRIB_DB);
        char l[200];
        snprintf(l, sizeof l, "core.nfsu: attrib db %08x classes table %08x (count %u) types %08x", db,
                 get_u32(db + 4) + 8, get_u16(get_u32(db + 4) + 8 + 4), get_u32(ATTRIB_TYPES));
        api->log(api, l);
    }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    { /* the class table's keys, and postfx's collection keys (diagnostic) */
        const uint32_t db = get_u32(ATTRIB_DB), t = get_u32(db + 4) + 8, n = get_u16(t + 4), ents = get_u32(t);
        char l[400];
        int m = snprintf(l, sizeof l, "core.nfsu: attrib class keys (n %u used %u):", n, get_u16(t + 6));
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t e = ents + i * 12u;
            if (get_u32(e + 4) != e)
                m += snprintf(l + m, sizeof l - m, " %08x@%u/%u", get_u32(e), i, get_u32(e + 8));
            if (m > 300 || i + 1 == n) {
                api->log(api, l);
                m = snprintf(l, sizeof l, "core.nfsu: attrib class keys:");
            }
        }
    }
    for (char *item = strtok(buf, ","); item; item = strtok(NULL, ",")) {
        char cls[96] = "", coll[96] = "", field[96] = "";
        if (sscanf(item, "%95[^/]/%95[^/]/%95s", cls, coll, field) != 3)
            continue;
        const uint32_t db = get_u32(ATTRIB_DB);
        const uint32_t cl = attrib_table_value(get_u32(db + 4) + 8, attrib_key(cls));
        const uint32_t c = attrib_collection(attrib_key(cls), attrib_key(coll));
        {
            char l[200];
            snprintf(l, sizeof l, "core.nfsu: attrib class %s %08x -> %08x, collections table count %u", cls,
                     attrib_key(cls), cl, cl ? get_u16(get_u32(cl + 4) + 0x14 + 4) : 0);
            api->log(api, l);
        }
        int where = -1;
        const uint32_t node = c ? attrib_node(c, attrib_key(field), &where) : 0;
        char line[1024];
        int n = snprintf(line, sizeof line, "core.nfsu: attrib %s/%s/%s: collection %08x node %08x where %d", cls, coll,
                         field, c, node, where);
        if (node) {
            const uint32_t fl = get_u8(node + 0xb), addr = attrib_value_addr(c, node);
            const uint32_t rec = get_u32(ATTRIB_TYPES) + 16u * get_u16(node + 8);
            n += snprintf(line + n, sizeof line - n, " flags %02x type %u [%08x %08x %08x %08x] at %08x:", fl,
                          get_u16(node + 8), get_u32(rec), get_u32(rec + 4), get_u32(rec + 8), get_u32(rec + 12), addr);
            const int words = (fl & 2) ? 2 + (int)(get_u16(addr + 2) * get_u16(addr + 4) / 4) : 12;
            for (int k = 0; k < words && k < 40 && n < (int)sizeof line - 12; ++k)
                n += snprintf(line + n, sizeof line - n, " %08x", get_u32(addr + 4u * (uint32_t)k));
        }
        api->log(api, line);
    }
}

/* ---- VltEd scripts applied live (2026-11-19) ----
 * RECOMP_NFSU_VLT="<setting>=<script.nfsms>|..." (launch.c): each script's update_field / resize_field lines (for a
 * script with "ui_option installation_type ..." sections, the "install" section only) are applied to the live
 * database about every 2 s while the core.nfsu setting is on - so collections the game loads later get them too -
 * and every value written is put back when the setting goes off. add_field / delete_field (a field the collection
 * lacks) and values in shared storage (a parent collection's, the class defaults) are left out and counted.
 * Struct members: X/Y/Z/W are floats at 0/4/8/12; Class/Collection of a reference at 0/4 (its cached pointer at 8
 * cleared); the pursuit records as the game holds them (checked in memory 2026-11-19): cops {name, CopType = hash
 * of the car's collection name, Count, Chance}, Heavy/Leader/AirSupportOptions {strategy, Chance, Duration (float),
 * ChanceBigSUV | PriorityChance, PriorityTime (float)}. */
#define ATTRIB_FLOAT 0x3c16ec5eu
#define ATTRIB_TEXT 0xa3f0c234u /* EA::Reflection::Text */
typedef struct {
    uint32_t cls, coll, field;
    int index;           /* -1: not an array element */
    char member[32];     /* struct member or "" */
    char value[64];
    int resize;          /* resize_field: new count */
    char coll_name[48], field_name[48];
    uint32_t text_mem;   /* a Text value's own copy in guest memory (when the game holds a pointer to its text) */
} VltEdit;
typedef struct {
    char setting[48];
    int64_t want;        /* "<setting>#<n>=<script>": on while the setting is n (a choice); -1: while it is not 0 */
    VltEdit *e;
    int n, cap;
    int skipped_cmds;
    int on;              /* applied last pass */
} VltScript;
typedef struct {
    uint32_t addr, size;
    uint8_t orig[16], want[16];
    uint32_t cls, coll, where; /* the collection it belongs to and where it was loaded when written */
} VltWrite;
/* gameplay.bin is many vaults (2026-11-20): gpcore always (3377 gameplay collections), each race bin's
 * ("race_bin_career" with race_bin_career/E020 ...) only while the game has it loaded - so a write is put back
 * only where its collection still is */
static uint32_t g_vlt_cur_cls, g_vlt_cur_coll, g_vlt_cur_where;
static VltScript g_vlt[8];
static int g_vlt_n = -1;
static VltWrite *g_vlt_w;
static int g_vlt_wn, g_vlt_wcap;

static void vlt_load(const PopModApi *api) {
    g_vlt_n = 0;
    const char *spec = getenv("RECOMP_NFSU_VLT");
    if (!spec || !*spec)
        return;
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *item = strtok_r(buf, "|", &save); item && g_vlt_n < 8; item = strtok_r(NULL, "|", &save)) {
        char *eq = strchr(item, '=');
        if (!eq)
            continue;
        *eq = 0;
        VltScript *s = &g_vlt[g_vlt_n];
        memset(s, 0, sizeof *s);
        s->want = -1;
        char *hash = strchr(item, '#');
        if (hash) {
            *hash = 0;
            s->want = atoi(hash + 1);
        }
        snprintf(s->setting, sizeof s->setting, "%s", item);
        FILE *f = fopen(eq + 1, "rb");
        if (!f) {
            char l[600];
            snprintf(l, sizeof l, "core.nfsu: vlt script %s not found", eq + 1);
            api->log(api, l);
            continue;
        }
        char line[512], section[32] = "";
        int has_sections = 0;
        while (fgets(line, sizeof line, f)) {
            char a[8][96];
            int k = sscanf(line, "%95s %95s %95s %95s %95s %95s %95s %95s", a[0], a[1], a[2], a[3], a[4], a[5], a[6],
                           a[7]);
            if (k <= 0)
                continue;
            if (!strcmp(a[0], "ui_option") && k >= 3) {
                has_sections = 1;
                /* a section starts and ends with the same marker */
                if (!strcmp(section, a[2]))
                    section[0] = 0;
                else
                    snprintf(section, sizeof section, "%s", a[2]);
                continue;
            }
            if (has_sections && strcmp(section, "install"))
                continue;
            const int upd = !strcmp(a[0], "update_field"), rsz = !strcmp(a[0], "resize_field");
            if (!upd && !rsz) {
                if (a[0][0] != '#' && strcmp(a[0], "ui_text") && strcmp(a[0], "ui_control"))
                    ++s->skipped_cmds;
                continue;
            }
            if (k < 5)
                continue;
            if (s->n == s->cap) {
                s->cap = s->cap ? s->cap * 2 : 1024;
                s->e = (VltEdit *)realloc(s->e, sizeof(VltEdit) * (size_t)s->cap);
            }
            VltEdit *e = &s->e[s->n++];
            memset(e, 0, sizeof *e);
            e->cls = attrib_key(a[1]);
            e->coll = attrib_key(a[2]);
            snprintf(e->coll_name, sizeof e->coll_name, "%s", a[2]);
            char fname[96];
            snprintf(fname, sizeof fname, "%s", a[3]);
            e->index = -1;
            char *br = strchr(fname, '[');
            if (br) {
                e->index = atoi(br + 1);
                *br = 0;
            }
            e->field = attrib_key(fname);
            snprintf(e->field_name, sizeof e->field_name, "%s", fname);
            if (rsz) {
                e->resize = atoi(a[4]);
            } else if (k >= 6) {
                snprintf(e->member, sizeof e->member, "%s", a[4]);
                snprintf(e->value, sizeof e->value, "%s", a[5]);
            } else {
                snprintf(e->value, sizeof e->value, "%s", a[4]);
            }
        }
        fclose(f);
        char l[400];
        snprintf(l, sizeof l, "core.nfsu: vlt script %s: %d edits (%d other commands left out) for setting %s", eq + 1,
                 s->n, s->skipped_cmds, s->setting);
        api->log(api, l);
        ++g_vlt_n;
    }
}

/* remember the original bytes at addr once, then write `want` */
static void vlt_write(const PopModApi *api, uint32_t addr, const uint8_t *want, uint32_t size) {
    for (int i = 0; i < g_vlt_wn; ++i)
        if (g_vlt_w[i].addr == addr) {
            VltWrite *w = &g_vlt_w[i];
            uint8_t cur[16];
            for (uint32_t b = 0; b < size; ++b)
                cur[b] = (uint8_t)get_u8(addr + b);
            if (memcmp(cur, want, size) == 0)
                return;
            /* the game rewrote it (a collection loaded again): that is the new original */
            if (memcmp(cur, w->want, size) != 0)
                memcpy(w->orig, cur, size);
            w->cls = g_vlt_cur_cls, w->coll = g_vlt_cur_coll, w->where = g_vlt_cur_where;
            memcpy(w->want, want, size);
            goto write;
        }
    if (g_vlt_wn == g_vlt_wcap) {
        g_vlt_wcap = g_vlt_wcap ? g_vlt_wcap * 2 : 4096;
        g_vlt_w = (VltWrite *)realloc(g_vlt_w, sizeof(VltWrite) * (size_t)g_vlt_wcap);
    }
    {
        VltWrite *w = &g_vlt_w[g_vlt_wn++];
        w->addr = addr;
        w->size = size;
        w->cls = g_vlt_cur_cls, w->coll = g_vlt_cur_coll, w->where = g_vlt_cur_where;
        for (uint32_t b = 0; b < size; ++b)
            w->orig[b] = (uint8_t)get_u8(addr + b);
        if (memcmp(w->orig, want, size) == 0)
            return;
        memcpy(w->want, want, size);
    }
write:
    for (uint32_t b = 0; b < size; ++b) {
        const uint32_t a4 = (addr + b) & ~3u;
        uint32_t v = get_u32(a4);
        const unsigned sh = 8u * ((addr + b) & 3u);
        v = (v & ~(0xffu << sh)) | ((uint32_t)want[b] << sh);
        api->guest_write_u32(api, a4, v);
    }
}
static void vlt_put_u32(const PopModApi *api, uint32_t addr, uint32_t v, uint32_t size) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    vlt_write(api, addr, b, size > 4 ? 4 : size);
}
/* a decimal number with '.' whatever the process locale is (strtod follows it) */
static double vlt_number(const char *t) {
    double sign = 1, v = 0, scale = 1;
    if (*t == '-' || *t == '+')
        sign = *t++ == '-' ? -1 : 1;
    for (; *t >= '0' && *t <= '9'; ++t)
        v = v * 10 + (*t - '0');
    if (*t == '.')
        for (++t; *t >= '0' && *t <= '9'; ++t)
            v += (*t - '0') * (scale /= 10);
    if (*t == 'e' || *t == 'E') {
        const int e = atoi(t + 1);
        for (int i = 0; i < (e < 0 ? -e : e); ++i)
            v = e < 0 ? v / 10 : v * 10;
    }
    return sign * v;
}
static uint32_t vlt_parse(const char *text, int as_float) {
    if (as_float) {
        float f = (float)vlt_number(text);
        uint32_t b;
        memcpy(&b, &f, 4);
        return b;
    }
    if (!strcasecmp(text, "true"))
        return 1;
    if (!strcasecmp(text, "false"))
        return 0;
    if (strchr(text, '.'))
        return (uint32_t)(int32_t)vlt_number(text);
    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-')
        return (uint32_t)strtol(text, NULL, 0);
    return attrib_hash(text); /* a name: a collection or enum key */
}
/* a member's offset and whether it is a float; -1 when unknown */
static int vlt_member(const char *field, const char *m, int *is_float) {
    *is_float = 0;
    if (!strcmp(m, "X") || !strcmp(m, "Y") || !strcmp(m, "Z") || !strcmp(m, "W")) {
        *is_float = 1;
        return m[0] == 'X' ? 0 : m[0] == 'Y' ? 4 : m[0] == 'Z' ? 8 : 12;
    }
    if (!strcmp(m, "Class"))
        return 0;
    if (!strcmp(m, "Collection"))
        return 4;
    /* a graph (Attrib::Types graph, 48 bytes; checked in GLOBAL/attributes.bin 2026-11-20: glue CatchupVsSkill
     * {MinX 0, MaxX 1, MinY 0, MaxY 1 as floats, GraphScale 0 at 16, then its points}); a pair of them
     * (SpreadOverRace, StrengthOverRace, 96 bytes) names them Easy_ and Hard_ */
    {
        const char *g = m;
        int base = 0;
        if (!strncmp(g, "Easy_", 5))
            g += 5;
        else if (!strncmp(g, "Hard_", 5))
            g += 5, base = 48;
        if (!strcmp(g, "MinX") || !strcmp(g, "MaxX") || !strcmp(g, "MinY") || !strcmp(g, "MaxY")) {
            *is_float = 1;
            return base + (g[1] == 'i' ? 0 : 4) + (g[3] == 'X' ? 0 : 8);
        }
        if (!strcmp(g, "GraphScale"))
            return base + 16;
    }
    if (!strcmp(field, "cops"))
        return !strcmp(m, "CopType") ? 4 : !strcmp(m, "Count") ? 8 : !strcmp(m, "Chance") ? 12 : -1;
    if (strstr(field, "SupportOptions")) {
        if (!strcmp(m, "HeavyStrategy") || !strcmp(m, "LeaderStrategy") || !strcmp(m, "HeliStrategy"))
            return 0;
        if (!strcmp(m, "Chance"))
            return 4;
        if (!strcmp(m, "Duration"))
            return *is_float = 1, 8;
        if (!strcmp(m, "ChanceBigSUV") || !strcmp(m, "PriorityChance"))
            return 12;
        if (!strcmp(m, "PriorityTime"))
            return *is_float = 1, 16;
    }
    return -1;
}

typedef struct {
    int applied, missing, shared, unknown_member, out_of_range, grown;
} VltStats;
static void vlt_apply_one(const PopModApi *api, const VltEdit *e, VltStats *st) {
    const uint32_t coll = attrib_collection(e->cls, e->coll);
    g_vlt_cur_cls = e->cls, g_vlt_cur_coll = e->coll, g_vlt_cur_where = coll;
    int where = -1;
    const uint32_t node = coll ? attrib_node(coll, e->field, &where) : 0;
    if (!node) {
        ++st->missing;
        return;
    }
    const uint32_t fl = get_u8(node + 0xb);
    /* storage only this collection owns: its own layout block, or its own node (inline value or its own data) */
    if ((fl & 0x20u) || (!(fl & 0x10u) && where != 0)) {
        ++st->shared;
        return;
    }
    uint32_t addr = attrib_value_addr(coll, node);
    const uint32_t rec = get_u32(ATTRIB_TYPES) + 16u * get_u16(node + 8);
    const uint32_t thash = get_u32(rec), tsize = get_u32(rec + 0xc) & 0xffffffu;
    if (fl & 2u) { /* an array */
        const uint32_t alloc = get_u16(addr), count = get_u16(addr + 2), esz = get_u16(addr + 4),
                       tfl = get_u16(addr + 6);
        if (e->resize) {
            const uint32_t want = (uint32_t)e->resize;
            if (want <= alloc) {
                vlt_put_u32(api, addr + 2, want, 2);
                ++st->applied;
                return;
            }
            /* a bigger copy in mod-owned guest memory, the node pointed at it (only a node that holds a pointer) */
            if ((fl & 0x50u) || !api->guest_alloc || want > 64 || !esz) {
                ++st->out_of_range;
                return;
            }
            const uint32_t head = (tfl & 0x8000u) ? 16u : 8u;
            uint32_t mem = 0;
            if (api->guest_alloc(api, head + want * esz + 16u, &mem) != POP_OK || !mem) {
                ++st->out_of_range;
                return;
            }
            mem = (mem + 15u) & ~15u;
            for (uint32_t b = 0; b < head + count * esz; b += 4)
                api->guest_write_u32(api, mem + b, get_u32(addr + b));
            /* new elements start as copies of the last one */
            for (uint32_t i = count; i < want; ++i)
                for (uint32_t b = 0; b < esz; b += 4)
                    api->guest_write_u32(api, mem + head + i * esz + b,
                                         count ? get_u32(addr + head + (count - 1) * esz + b) : 0);
            api->guest_write_u32(api, mem, (want << 16) | want); /* alloc, count */
            vlt_put_u32(api, node + 4, mem, 4);
            ++st->grown;
            return;
        }
        if (e->index < 0 || (uint32_t)e->index >= count) {
            if (getenv("NFSU_VLT_LOG") && st->out_of_range < 12) {
                char l[200];
                snprintf(l, sizeof l, "core.nfsu: vlt out of range %s/%s[%d] (count %u)", e->coll_name, e->field_name,
                         e->index, count);
                api->log(api, l);
            }
            ++st->out_of_range;
            return;
        }
        addr += ((tfl & 0x8000u) ? 16u : 8u) + (uint32_t)e->index * esz;
    } else if (e->resize || e->index > 0) {
        ++st->out_of_range;
        return;
    }
    if (e->member[0]) {
        int is_float = 0;
        const int off = vlt_member(e->field_name, e->member, &is_float);
        if (off < 0) {
            if (getenv("NFSU_VLT_LOG") && st->unknown_member < 12) {
                char l[200];
                snprintf(l, sizeof l, "core.nfsu: vlt unknown member %s/%s %s", e->coll_name, e->field_name, e->member);
                api->log(api, l);
            }
            ++st->unknown_member;
            return;
        }
        vlt_put_u32(api, addr + (uint32_t)off, vlt_parse(e->value, is_float), 4);
        if (!strcmp(e->member, "Collection") || !strcmp(e->member, "Class"))
            vlt_put_u32(api, addr + 8, 0, 4); /* the reference's cached pointer: looked up again */
        ++st->applied;
        return;
    }
    const uint32_t size = (fl & 2u) ? get_u16(attrib_value_addr(coll, node) + 4) : tsize;
    if (thash == ATTRIB_TEXT && size == 4) {
        /* a number kept as short text in its own 4 bytes (gameplay CatchUpSkill holds "0.2", 2026-11-20): written
         * as text when it fits with its end, and only over what reads as text now */
        const size_t n = strlen(e->value);
        uint8_t now[4], want[4] = {0, 0, 0, 0};
        for (int b = 0; b < 4; ++b)
            now[b] = get_u8(addr + (uint32_t)b);
        int text = now[0] >= 0x20 && now[0] < 0x7f;
        for (int b = 1; b < 4 && text; ++b)
            text = !now[b] || (now[b] >= 0x20 && now[b] < 0x7f);
        if (!text && n < 32 && api->guest_alloc) {
            /* the loaded database holds a pointer to the text instead (seen live 2026-11-20: basicrace's
             * CatchUpSkill did not read as text in memory): the new text in a copy of its own, the pointer at it
             * (put back with the rest when the setting goes off) - never written over a shared string */
            const uint32_t ptr = (uint32_t)now[0] | (uint32_t)now[1] << 8 | (uint32_t)now[2] << 16 | (uint32_t)now[3] << 24;
            int ok = ptr >= 0x10000u && ptr == ((VltEdit *)e)->text_mem;
            if (!ok && ptr >= 0x10000u) {
                ok = 1;
                for (uint32_t b = 0; b < 16 && ok; ++b) {
                    const uint32_t c = get_u8(ptr + b);
                    if (!c)
                        break;
                    ok = c >= 0x20 && c < 0x7f;
                }
                ok = ok && get_u8(ptr) != 0;
            }
            if (ok) {
                VltEdit *me = (VltEdit *)e;
                if (!me->text_mem && (api->guest_alloc(api, 32, &me->text_mem) != POP_OK || !me->text_mem)) {
                    me->text_mem = 0;
                    ++st->unknown_member;
                    return;
                }
                for (uint32_t w = 0; w < 32; w += 4) {
                    uint32_t v = 0;
                    for (uint32_t b = 0; b < 4; ++b)
                        v |= (uint32_t)(uint8_t)(w + b < n ? e->value[w + b] : 0) << (8 * b);
                    api->guest_write_u32(api, me->text_mem + w, v);
                }
                vlt_put_u32(api, addr, me->text_mem, 4);
                ++st->applied;
                return;
            }
        }
        if (n >= 4 || !text) {
            ++st->unknown_member;
            return;
        }
        memcpy(want, e->value, n);
        vlt_write(api, addr, want, 4);
        ++st->applied;
        return;
    }
    if (size != 1 && size != 2 && size != 4) {
        ++st->unknown_member;
        return;
    }
    vlt_put_u32(api, addr, vlt_parse(e->value, thash == ATTRIB_FLOAT), size);
    ++st->applied;
}

/* each main-loop pass; does its work about every 2 s */
static void vlt_pass(const PopModApi *api) {
    if (g_vlt_n < 0)
        vlt_load(api);
    if (!g_vlt_n || !get_u32(ATTRIB_DB))
        return;
    static int64_t at;
    const int64_t t = now_ns();
    if (at && t - at < 2000000000LL)
        return;
    at = t;
    for (int si = 0; si < g_vlt_n; ++si) {
        VltScript *s = &g_vlt[si];
        int64_t on = 1;
        if (api->settings_get)
            api->settings_get(api, s->setting, &on);
        if (s->want >= 0)
            on = on == s->want;
        if (!on) {
            if (s->on) { /* put back what this script wrote: every remembered write (one script's set at a time) */
                for (int i = 0; i < g_vlt_wn; ++i) {
                    VltWrite *w = &g_vlt_w[i];
                    if (w->where && attrib_collection(w->cls, w->coll) != w->where)
                        continue; /* its vault was unloaded (or loaded elsewhere): nothing of ours is there */
                    for (uint32_t b = 0; b < w->size; ++b) {
                        const uint32_t a4 = (w->addr + b) & ~3u;
                        uint32_t v = get_u32(a4);
                        const unsigned sh = 8u * ((w->addr + b) & 3u);
                        v = (v & ~(0xffu << sh)) | ((uint32_t)w->orig[b] << sh);
                        api->guest_write_u32(api, a4, v);
                    }
                }
                g_vlt_wn = 0;
                s->on = 0;
                for (int sj = 0; sj < g_vlt_n; ++sj)
                    g_vlt[sj].on = 0; /* the others are applied again next pass */
                api->log(api, "core.nfsu: vlt script switched off: its values put back");
            }
            continue;
        }
        if (getenv("NFSU_VLT_TABLE") && s->n) {
            /* diagnostic: the class table of the script's first missing edit - size, count, and a full scan */
            for (int i = 0; i < s->n; ++i) {
                const VltEdit *e = &s->e[i];
                if (attrib_collection(e->cls, e->coll))
                    continue;
                const uint32_t db = get_u32(ATTRIB_DB);
                const uint32_t cls = db ? attrib_table_value(get_u32(db + 4) + 8, e->cls) : 0;
                const uint32_t t = cls ? get_u32(cls + 4) + 0x14 : 0;
                uint32_t n = t ? get_u16(t + 4) : 0, cnt = t ? get_u16(t + 6) : 0, found = 0, ents = t ? get_u32(t) : 0;
                for (uint32_t k = 0; k < n; ++k)
                    if (get_u32(ents + 12u * k) == e->coll)
                        found = 1 + k;
                char l[200];
                snprintf(l, sizeof l, "core.nfsu: vlt table %s/%s: class %08x, table size %u count %u, key %08x scan %u",
                         s->setting, e->coll_name, cls, n, cnt, e->coll, found);
                api->log(api, l);
                break;
            }
        }
        VltStats st = {0, 0, 0, 0, 0, 0};
        for (int i = 0; i < s->n; ++i)
            vlt_apply_one(api, &s->e[i], &st);
        if (!s->on || getenv("NFSU_VLT_LOG")) {
            char l[300];
            snprintf(l, sizeof l,
                     "core.nfsu: vlt %s: %d values set, %d arrays grown, %d not in the database (yet), %d in shared "
                     "storage, %d unknown members, %d out of range",
                     s->setting, st.applied, st.grown, st.missing, st.shared, st.unknown_member, st.out_of_range);
            api->log(api, l);
        }
        s->on = 1;
    }
}
