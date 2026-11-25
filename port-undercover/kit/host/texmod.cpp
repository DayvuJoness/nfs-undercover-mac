// TexMod texture packs (.tpf), as a host service for the D3D9 shim (2026-11-19).
//
// A TexMod pack replaces a game texture by the CRC32 of its top level as the game wrote it. The file is a ZIP whose
// every 32-bit word is XORed with 0x3FA43FA4 (a tail shorter than a word with 0xA4), its entries ZipCrypto-encrypted
// with TexMod's fixed password; "texmod.def" lists "0x<hash>|<entry name>" (entries are DDS images).
//
// RECOMP_D3D9_TEXMOD="<setting>=<path>|<setting>=<path>|..." names the packs and the core-mod setting that switches
// each on (live: looked up when asked). A pack is opened at start (its directory only); an image is decoded the first
// time its texture is seen. The D3D9 shim (dx/d3d9.cpp) asks host_texmod_find() for each texture it hashes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../third_party/miniz/miniz.h"
#include "../runtime/mods_seam.h"

namespace {

struct Entry {
    uint32_t local_offset = 0, comp_size = 0, size = 0, crc = 0;
    uint16_t method = 0, flags = 0, mod_time = 0;
    bool tried = false;
    std::atomic<bool> ready{false}; // dds is complete (mips made when the file had none)
    std::vector<uint8_t> dds;       // decoded once
    Entry() = default;
    Entry(const Entry &o)
        : local_offset(o.local_offset), comp_size(o.comp_size), size(o.size), crc(o.crc), method(o.method),
          flags(o.flags), mod_time(o.mod_time) {}
    Entry &operator=(const Entry &o) {
        local_offset = o.local_offset, comp_size = o.comp_size, size = o.size, crc = o.crc, method = o.method;
        flags = o.flags, mod_time = o.mod_time;
        return *this;
    }
};

struct Pack {
    std::string setting, path;
    std::vector<uint8_t> bytes; // the whole file, un-XORed
    std::unordered_map<uint32_t, Entry> by_hash;
    std::vector<uint32_t> skip; // hashes left out
};

std::mutex g_mu;
std::vector<Pack> g_packs;
std::atomic<uint32_t> g_finished{0}; // images made in the background so far (part of host_texmod_state)
bool g_loaded = false;

const uint8_t kPassword[] = {0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD, 0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A,
                             0x7F, 0x7F, 0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74, 0x5C, 0x22, 0x74, 0x5D,
                             0x6E, 0x6A, 0x73, 0x41, 0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B, 0x46, 0x6F};

uint32_t rd16(const uint8_t *p) {
    return p[0] | p[1] << 8;
}
uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// ZipCrypto (the traditional PKWARE stream cipher).
struct ZipKeys {
    uint32_t k0 = 0x12345678, k1 = 0x23456789, k2 = 0x34567890;
    void update(uint8_t c) {
        k0 = (uint32_t)mz_crc32(k0 ^ 0xffffffffu, &c, 1) ^ 0xffffffffu;
        k1 = (k1 + (k0 & 0xff)) * 134775813u + 1;
        const uint8_t b = (uint8_t)(k1 >> 24);
        k2 = (uint32_t)mz_crc32(k2 ^ 0xffffffffu, &b, 1) ^ 0xffffffffu;
    }
    uint8_t decrypt(uint8_t c) {
        const uint16_t t = (uint16_t)(k2 | 2);
        const uint8_t p = c ^ (uint8_t)((t * (t ^ 1)) >> 8);
        update(p);
        return p;
    }
};

// One entry's bytes: decrypted when flagged, inflated when deflated.
bool extract(const Pack &pk, const Entry &e, std::vector<uint8_t> &out) {
    const std::vector<uint8_t> &b = pk.bytes;
    if ((size_t)e.local_offset + 30 > b.size() || rd32(&b[e.local_offset]) != 0x04034b50)
        return false;
    const size_t data = e.local_offset + 30 + rd16(&b[e.local_offset + 26]) + rd16(&b[e.local_offset + 28]);
    if (data + e.comp_size > b.size())
        return false;
    std::vector<uint8_t> comp(b.begin() + data, b.begin() + data + e.comp_size);
    if (e.flags & 1) {
        ZipKeys k;
        for (uint8_t c : kPassword)
            k.update(c);
        if (comp.size() < 12)
            return false;
        for (uint8_t &c : comp)
            c = k.decrypt(c);
        comp.erase(comp.begin(), comp.begin() + 12);
    }
    if (e.method == 0) {
        out = std::move(comp);
    } else if (e.method == 8) {
        out.resize(e.size);
        const size_t n = tinfl_decompress_mem_to_mem(out.data(), out.size(), comp.data(), comp.size(), 0);
        if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || n != e.size)
            return false;
    } else {
        return false;
    }
    return (uint32_t)mz_crc32(MZ_CRC32_INIT, out.data(), out.size()) == e.crc;
}

// --- Mip levels for images that come without them (2026-11-19). 54 of Redux's 76 DDS files hold only the top
// level; drawn far away without smaller levels they shimmered as grain (the person's report, pics/18). The chain is
// made once in the background: each level decoded from block compression, averaged 2x2 and encoded again in the
// same format (bounding-box endpoints, nearest palette entry - plain, but only for the smaller levels).
struct Rgba {
    uint8_t c[4];
};
void color565(uint16_t v, int out[3]) {
    out[0] = ((v >> 11) & 31) * 255 / 31;
    out[1] = ((v >> 5) & 63) * 255 / 63;
    out[2] = (v & 31) * 255 / 31;
}
// fourcc 1 = DXT1, 3 = DXT3, 5 = DXT5; block = 8 or 16 bytes; writes 16 texels
void decode_block(int kind, const uint8_t *b, Rgba out[16]) {
    const uint8_t *cb = kind == 1 ? b : b + 8;
    const uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    int p[4][4] = {};
    color565(c0, p[0]);
    color565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (kind != 1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        }
    } else {
        for (int k = 0; k < 3; ++k)
            p[2][k] = (p[0][k] + p[1][k]) / 2;
        p[3][0] = p[3][1] = p[3][2] = 0;
        p[3][3] = 0;
    }
    const uint32_t idx = rd32(cb + 4);
    for (int i = 0; i < 16; ++i) {
        const int *q = p[(idx >> (2 * i)) & 3];
        for (int k = 0; k < 4; ++k)
            out[i].c[k] = (uint8_t)q[k];
    }
    if (kind == 3) {
        for (int i = 0; i < 16; ++i) {
            const int a = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i].c[3] = (uint8_t)(a * 17);
        }
    } else if (kind == 5) {
        int a[8];
        a[0] = b[0], a[1] = b[1];
        if (a[0] > a[1]) {
            for (int k = 1; k < 7; ++k)
                a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
        } else {
            for (int k = 1; k < 5; ++k)
                a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
            a[6] = 0, a[7] = 255;
        }
        uint64_t bits = 0;
        for (int k = 0; k < 6; ++k)
            bits |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; ++i)
            out[i].c[3] = (uint8_t)a[(bits >> (3 * i)) & 7];
    }
}
uint16_t to565(const int c[3]) {
    return (uint16_t)(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}
void encode_block(int kind, const Rgba in[16], uint8_t *b) {
    // alpha
    uint8_t *cb = kind == 1 ? b : b + 8;
    if (kind == 3) {
        memset(b, 0, 8);
        for (int i = 0; i < 16; ++i)
            b[i / 2] |= (uint8_t)(((in[i].c[3] * 15 + 127) / 255) << (4 * (i & 1)));
    } else if (kind == 5) {
        int lo = 255, hi = 0;
        for (int i = 0; i < 16; ++i)
            lo = std::min<int>(lo, in[i].c[3]), hi = std::max<int>(hi, in[i].c[3]);
        b[0] = (uint8_t)hi, b[1] = (uint8_t)lo;
        int a[8] = {hi, lo};
        for (int k = 1; k < 7; ++k)
            a[k + 1] = ((7 - k) * hi + k * lo) / 7;
        uint64_t bits = 0;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; ++k) {
                const int d = std::abs(a[k] - in[i].c[3]);
                if (d < bd)
                    bd = d, best = k;
            }
            bits |= (uint64_t)best << (3 * i);
        }
        for (int k = 0; k < 6; ++k)
            b[2 + k] = (uint8_t)(bits >> (8 * k));
    }
    // colour: the bounding box's corners, inset a little
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    bool transparent = false;
    for (int i = 0; i < 16; ++i) {
        if (kind == 1 && in[i].c[3] < 128) {
            transparent = true;
            continue;
        }
        for (int k = 0; k < 3; ++k)
            mn[k] = std::min<int>(mn[k], in[i].c[k]), mx[k] = std::max<int>(mx[k], in[i].c[k]);
    }
    if (mn[0] > mx[0])
        for (int k = 0; k < 3; ++k)
            mn[k] = mx[k] = 0;
    for (int k = 0; k < 3; ++k) {
        const int inset = (mx[k] - mn[k]) / 16;
        mn[k] += inset, mx[k] -= inset;
    }
    uint16_t c0 = to565(mx), c1 = to565(mn);
    if (transparent) { // three colours + transparent: c0 <= c1
        if (c0 > c1)
            std::swap(c0, c1);
    } else if (c0 < c1) {
        std::swap(c0, c1);
    }
    int p[4][3];
    color565(c0, p[0]);
    color565(c1, p[1]);
    const bool four = !(kind == 1 && c0 <= c1);
    for (int k = 0; k < 3; ++k) {
        p[2][k] = four ? (2 * p[0][k] + p[1][k]) / 3 : (p[0][k] + p[1][k]) / 2;
        p[3][k] = four ? (p[0][k] + 2 * p[1][k]) / 3 : 0;
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bd = 1 << 30;
        if (kind == 1 && !four && in[i].c[3] < 128) {
            best = 3;
        } else {
            for (int k = 0; k < (four ? 4 : 3); ++k) {
                const int dr = p[k][0] - in[i].c[0], dg = p[k][1] - in[i].c[1], db = p[k][2] - in[i].c[2];
                const int d = dr * dr + dg * dg + db * db;
                if (d < bd)
                    bd = d, best = k;
            }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    cb[0] = (uint8_t)c0, cb[1] = (uint8_t)(c0 >> 8), cb[2] = (uint8_t)c1, cb[3] = (uint8_t)(c1 >> 8);
    memcpy(cb + 4, &idx, 4);
}
// A DDS of DXT1/3/5 with one level -> the same with the whole mip chain; false when it is not such a file.
bool add_mips(std::vector<uint8_t> &dds) {
    if (dds.size() < 128)
        return false;
    const uint32_t h = rd32(&dds[12]), w = rd32(&dds[16]), mips = rd32(&dds[28]), flags = rd32(&dds[80]),
                   fourcc = rd32(&dds[84]);
    if (!(flags & 4) || mips > 1 || w < 8 || h < 8 || (w & 3) || (h & 3))
        return false;
    const int kind = fourcc == 0x31545844 ? 1 : fourcc == 0x33545844 ? 3 : fourcc == 0x35545844 ? 5 : 0;
    if (!kind)
        return false;
    const uint32_t bs = kind == 1 ? 8 : 16;
    if (dds.size() < 128 + size_t(w / 4) * (h / 4) * bs)
        return false;
    // decode the top level
    std::vector<Rgba> img(size_t(w) * h);
    for (uint32_t by = 0; by < h / 4; ++by)
        for (uint32_t bx = 0; bx < w / 4; ++bx) {
            Rgba t[16];
            decode_block(kind, &dds[128 + (size_t(by) * (w / 4) + bx) * bs], t);
            for (int i = 0; i < 16; ++i)
                img[size_t(by * 4 + i / 4) * w + bx * 4 + (i & 3)] = t[i];
        }
    std::vector<uint8_t> out(dds.begin(), dds.begin() + 128 + size_t(w / 4) * (h / 4) * bs);
    uint32_t lw = w, lh = h, levels = 1;
    while (lw > 1 || lh > 1) {
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        std::vector<Rgba> next(size_t(nw) * nh);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                int acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const Rgba &q = img[size_t(std::min(lh - 1, y * 2 + dy)) * lw + std::min(lw - 1, x * 2 + dx)];
                        for (int k = 0; k < 4; ++k)
                            acc[k] += q.c[k];
                    }
                for (int k = 0; k < 4; ++k)
                    next[size_t(y) * nw + x].c[k] = (uint8_t)((acc[k] + 2) / 4);
            }
        img.swap(next);
        lw = nw, lh = nh;
        const uint32_t bw = std::max(1u, (lw + 3) / 4), bh = std::max(1u, (lh + 3) / 4);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                Rgba t[16];
                for (int i = 0; i < 16; ++i)
                    t[i] = img[size_t(std::min(lh - 1, by * 4 + i / 4)) * lw + std::min(lw - 1, bx * 4 + (i & 3))];
                uint8_t blk[16];
                encode_block(kind, t, blk);
                out.insert(out.end(), blk, blk + bs);
            }
        ++levels;
    }
    const uint32_t f = rd32(&out[8]) | 0x20000; // DDSD_MIPMAPCOUNT
    memcpy(&out[8], &f, 4);
    memcpy(&out[28], &levels, 4);
    const uint32_t caps = rd32(&out[108]) | 0x400008; // DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    memcpy(&out[108], &caps, 4);
    dds.swap(out);
    return true;
}

void open_pack(Pack &pk) {
    FILE *f = fopen(pk.path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[host] texmod: %s: not found\n", pk.path.c_str());
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    pk.bytes.resize(size > 0 ? (size_t)size : 0);
    const size_t got = fread(pk.bytes.data(), 1, pk.bytes.size(), f);
    fclose(f);
    if (got != pk.bytes.size() || got < 22)
        return;
    std::vector<uint8_t> &b = pk.bytes;
    const size_t words = b.size() / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t v = rd32(&b[i * 4]) ^ 0x3FA43FA4u;
        memcpy(&b[i * 4], &v, 4);
    }
    for (size_t i = words * 4; i < b.size(); ++i)
        b[i] ^= 0xA4;
    // End of central directory: the last "PK\5\6".
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i < 70000;)
        if (rd32(&b[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        fprintf(stderr, "[host] texmod: %s: not a TexMod pack\n", pk.path.c_str());
        return;
    }
    const uint32_t count = rd16(&b[eocd + 10]), cd = rd32(&b[eocd + 16]);
    std::unordered_map<std::string, Entry> by_name;
    size_t p = cd;
    for (uint32_t i = 0; i < count && p + 46 <= b.size() && rd32(&b[p]) == 0x02014b50; ++i) {
        Entry e;
        e.flags = (uint16_t)rd16(&b[p + 8]);
        e.method = (uint16_t)rd16(&b[p + 10]);
        e.crc = rd32(&b[p + 16]);
        e.comp_size = rd32(&b[p + 20]);
        e.size = rd32(&b[p + 24]);
        const uint32_t nlen = rd16(&b[p + 28]), xlen = rd16(&b[p + 30]), clen = rd16(&b[p + 32]);
        e.local_offset = rd32(&b[p + 42]);
        if (p + 46 + nlen > b.size())
            break;
        by_name[std::string((const char *)&b[p + 46], nlen)] = e;
        p += 46 + nlen + xlen + clen;
    }
    // texmod.def: "0x<hash>|<name>" per line.
    auto def = by_name.find("texmod.def");
    std::vector<uint8_t> text;
    if (def == by_name.end() || !extract(pk, def->second, text)) {
        fprintf(stderr, "[host] texmod: %s: no readable texmod.def\n", pk.path.c_str());
        return;
    }
    std::string s(text.begin(), text.end());
    size_t at = 0;
    while (at < s.size()) {
        size_t eol = s.find_first_of("\r\n", at);
        if (eol == std::string::npos)
            eol = s.size();
        const std::string line = s.substr(at, eol - at);
        at = eol + 1;
        const size_t bar = line.find('|');
        if (bar == std::string::npos)
            continue;
        const uint32_t hash = (uint32_t)strtoul(line.substr(0, bar).c_str(), nullptr, 16);
        std::string name = line.substr(bar + 1);
        while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
            name.pop_back();
        // A def line may name a path inside the zip or only its file name.
        auto it = by_name.find(name);
        if (it == by_name.end()) {
            const size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos)
                it = by_name.find(name.substr(slash + 1));
        }
        if (it != by_name.end() && hash && std::find(pk.skip.begin(), pk.skip.end(), hash) == pk.skip.end())
            pk.by_hash[hash] = it->second;
    }
    fprintf(stderr, "[host] texmod: %s: %zu textures (setting %s)\n", pk.path.c_str(), pk.by_hash.size(),
            pk.setting.c_str());
}

void load_all() {
    if (g_loaded)
        return;
    g_loaded = true;
    const char *e = getenv("RECOMP_D3D9_TEXMOD");
    if (!e || !*e)
        return;
    std::string spec(e);
    size_t at = 0;
    while (at < spec.size()) {
        size_t bar = spec.find('|', at);
        if (bar == std::string::npos)
            bar = spec.size();
        const std::string item = spec.substr(at, bar - at);
        at = bar + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        g_packs.emplace_back();
        g_packs.back().setting = item.substr(0, eq);
        // "<path>!<hash>,<hash>": images of the pack left out (2026-11-19: the ProStreet sky mod's lens-streak
        // image, a flat band over the whole quad, drew a wide light band down the screen near the sun)
        std::string path = item.substr(eq + 1);
        const size_t bang = path.find('!');
        if (bang != std::string::npos) {
            std::string list = path.substr(bang + 1);
            path.resize(bang);
            for (size_t at = 0; at < list.size();) {
                size_t comma = list.find(',', at);
                if (comma == std::string::npos)
                    comma = list.size();
                g_packs.back().skip.push_back((uint32_t)strtoul(list.substr(at, comma - at).c_str(), nullptr, 16));
                at = comma + 1;
            }
        }
        g_packs.back().path = path;
    }
    for (Pack &pk : g_packs)
        open_pack(pk);
}

} // namespace

// The replacement image (a whole DDS file) for a texture whose top level hashes to `hash`, from the first switched-on
// pack that has one; nullptr when none does. `known` (when given) says whether any pack has it at all, on or off.
// The bytes stay valid for the program's life.
extern "C" const uint8_t *host_texmod_find(uint32_t hash, uint32_t *size, int *known) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_all();
    if (known)
        *known = 0;
    for (Pack &pk : g_packs) {
        auto it = pk.by_hash.find(hash);
        if (it == pk.by_hash.end())
            continue;
        if (known)
            *known = 1;
        if (!pk.setting.empty() && mods_core_setting(pk.setting.c_str(), 1) == 0)
            continue;
        Entry &en = it->second;
        if (!en.tried) {
            en.tried = true;
            // read and (when it has no smaller levels) finished in the background; the game's own texture is drawn
            // until then
            Pack *ppk = &pk;
            Entry *pen = &en;
            std::thread([ppk, pen, hash] {
                std::vector<uint8_t> dds;
                if (!extract(*ppk, *pen, dds) || dds.size() < 128 || memcmp(dds.data(), "DDS ", 4)) {
                    fprintf(stderr, "[host] texmod: %s: texture %08x could not be read\n", ppk->path.c_str(), hash);
                    dds.clear();
                } else if (add_mips(dds)) {
                    fprintf(stderr, "[host] texmod: texture %08x: smaller levels made (it had none)\n", hash);
                }
                {
                    std::lock_guard<std::mutex> lock(g_mu);
                    pen->dds.swap(dds);
                }
                pen->ready.store(true);
                g_finished.fetch_add(1);
            }).detach();
        }
        if (!en.ready.load() || en.dds.empty())
            continue;
        if (size)
            *size = (uint32_t)en.dds.size();
        return en.dds.data();
    }
    return nullptr;
}

// Whether any pack is configured (so the shim hashes textures only then).
extern "C" int host_texmod_active(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_all();
    for (const Pack &pk : g_packs)
        if (!pk.by_hash.empty())
            return 1;
    return 0;
}

// A short serial that changes when a pack's setting changes, so the shim re-decides its replacements.
extern "C" uint32_t host_texmod_state(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_all();
    uint32_t s = g_finished.load() << 16;
    for (size_t i = 0; i < g_packs.size() && i < 16; ++i)
        if (g_packs[i].setting.empty() || mods_core_setting(g_packs[i].setting.c_str(), 1) != 0)
            s |= 1u << i;
    return s;
}
