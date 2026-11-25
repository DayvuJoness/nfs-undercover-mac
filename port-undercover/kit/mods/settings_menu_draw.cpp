// settings_menu_draw.cpp - draws the tabbed settings menu (settings_menu.h) as a straight-alpha
// RGBA page at the size the presenter asks for (the game's rectangle on screen), in the kit's
// own Open Sans. It is only redrawn when the menu's content or the size changed; the presenter
// keeps the last texture otherwise. Called on the thread that seals a frame, so it uses its own
// glyph cache (gdi's is the guest's) under its own lock.
//
// A host can give it a look of its own (mods_menu_set_style): the macOS host puts a native glass
// panel behind the page, so the page leaves the panel clear and draws only what is on it, with
// rounded shapes, switches for on/off rows and the system's accent colour, and draws its text in
// the system font (mods_menu_set_text_backend).
#include "settings_menu.h"
#include "menu_lang.h"
#include "mods_internal.h"
#include "../runtime/gdi32_truetype.h"
#include "../runtime/mods_seam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <mutex>
#include <tuple>
#include <vector>

namespace {

struct Rgba {
    uint8_t r, g, b, a;
};
const Rgba kDim = {0, 0, 0, 150};
const Rgba kPanel = {18, 20, 24, 238};
const Rgba kEdge = {64, 68, 78, 255};
const Rgba kInk = {236, 238, 242, 255};
const Rgba kSoft = {168, 174, 184, 255};
const Rgba kFaint = {112, 116, 124, 255};
const Rgba kAccent = {255, 168, 24, 255};
const Rgba kSelect = {255, 255, 255, 26};

// The host's look and text, set by mods_menu_set_style / mods_menu_set_text_backend; under g_mu.
bool g_glass = false;
Rgba g_glass_accent = {10, 132, 255, 255};
const MenuTextBackend *g_text = nullptr;
float g_panel[5] = {0, 0, 0, 0, 0}; // x0 y0 x1 y1 as fractions of the page, radius / page height
bool g_panel_valid = false;

struct Canvas {
    int w, h;
    std::vector<uint8_t> *px;
    void blend(int x, int y, Rgba c, int cov) { // cov 0-255
        if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0)
            return;
        uint8_t *p = px->data() + (size_t(y) * w + x) * 4;
        const int a = c.a * cov / 255;
        p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
        p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
        p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
        p[3] = uint8_t(a + p[3] * (255 - a) / 255);
    }
    void fill(int x0, int y0, int x1, int y1, Rgba c) {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, w), y1 = std::min(y1, h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                blend(x, y, c, 255);
    }
};

// UTF-8 to codepoints; a malformed byte becomes '?'.
std::vector<uint32_t> decode(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n == 0)) {
            out.push_back('?');
            ++i;
            continue;
        }
        uint32_t cp = n == 0 ? c : n == 1 ? (c & 31) : n == 2 ? (c & 15) : (c & 7);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            const uint8_t d = uint8_t(s[i + k]);
            if ((d >> 6) != 2)
                ok = false;
            cp = (cp << 6) | (d & 63);
        }
        out.push_back(ok ? cp : '?');
        i += ok ? n + 1 : 1;
    }
    return out;
}

struct Font {
    const gdi::TrueTypeFace *face = nullptr;
    void *native = nullptr; // the host's font (g_text), which then draws and measures the text
    double scale = 0;
    int ascent = 0, height = 0;
};

std::mutex g_mu;
std::map<std::tuple<const gdi::TrueTypeFace *, double, uint32_t>, gdi::TrueTypeGlyph> g_glyphs;

const gdi::TrueTypeGlyph &glyph(const Font &f, uint32_t cp) {
    auto key = std::make_tuple(f.face, f.scale, cp);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end())
        return it->second;
    if (g_glyphs.size() > 2048)
        g_glyphs.clear();
    gdi::TrueTypeGlyph g;
    gdi::truetype_render_glyph(f.face, f.scale, cp, &g);
    return g_glyphs.emplace(key, std::move(g)).first->second;
}

// A whole string drawn by the host's text backend, cached like the glyphs.
struct NativeRun {
    int w = 0, h = 0, x = 0, y = 0, advance = 0;
    std::vector<uint8_t> coverage;
};
std::map<std::pair<void *, std::string>, NativeRun> g_runs;

const NativeRun &native_run(const Font &f, const std::string &s) {
    auto key = std::make_pair(f.native, s);
    auto it = g_runs.find(key);
    if (it != g_runs.end())
        return it->second;
    if (g_runs.size() > 1024)
        g_runs.clear();
    NativeRun r;
    r.advance = g_text->measure(f.native, s.c_str());
    uint8_t *cov = nullptr;
    if (g_text->render(f.native, s.c_str(), &cov, &r.w, &r.h, &r.x, &r.y) && cov && r.w > 0 && r.h > 0)
        r.coverage.assign(cov, cov + size_t(r.w) * size_t(r.h));
    else
        r.w = r.h = 0;
    std::free(cov);
    return g_runs.emplace(key, std::move(r)).first->second;
}

Font font(int px, bool bold) {
    Font f;
    if (g_text && (f.native = g_text->font(std::max(px, 6), bold ? 600 : 400))) {
        g_text->metrics(f.native, &f.ascent, &f.height);
        return f;
    }
    f.face = gdi::truetype_windows_substitute("Segoe UI", bold ? 600 : 400);
    if (!f.face)
        return f;
    f.scale = gdi::truetype_scale(f.face, -std::max(px, 6));
    const gdi::TrueTypeMetrics m = gdi::truetype_metrics(f.face, f.scale);
    f.ascent = m.ascent;
    f.height = m.height;
    return f;
}

int measure(const Font &f, const std::vector<uint32_t> &cps) {
    if (!f.face)
        return 0;
    int x = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        x += gdi::truetype_advance(f.face, f.scale, cps[i]);
        if (i + 1 < cps.size())
            x += gdi::truetype_kern(f.face, f.scale, cps[i], cps[i + 1]);
    }
    return x;
}
int measure(const Font &f, const std::string &s) {
    if (f.native)
        return native_run(f, s).advance;
    return measure(f, decode(s));
}

// Text with its top at y. Returns the advance. Clipped at max_x when given.
int text(Canvas &c, const Font &f, int x, int y, const std::string &s, Rgba ink, int max_x = 1 << 30) {
    if (f.native) {
        const NativeRun &r = native_run(f, s);
        const int base = y + f.ascent;
        for (int gy = 0; gy < r.h; ++gy)
            for (int gx = 0; gx < r.w; ++gx) {
                const int px = x + r.x + gx;
                if (px < max_x)
                    c.blend(px, base + r.y + gy, ink, r.coverage[size_t(gy) * r.w + gx]);
            }
        return r.advance;
    }
    if (!f.face)
        return 0;
    const std::vector<uint32_t> cps = decode(s);
    const int x0 = x, base = y + f.ascent;
    for (size_t i = 0; i < cps.size(); ++i) {
        const gdi::TrueTypeGlyph &g = glyph(f, cps[i]);
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const int px = x + g.x + gx;
                if (px < max_x)
                    c.blend(px, base + g.y + gy, ink, g.coverage[size_t(gy) * g.w + gx]);
            }
        x += gdi::truetype_advance(f.face, f.scale, cps[i]);
        if (i + 1 < cps.size())
            x += gdi::truetype_kern(f.face, f.scale, cps[i], cps[i + 1]);
    }
    return x - x0;
}

// Word-wrapped lines no wider than width.
std::vector<std::string> wrap(const Font &f, const std::string &s, int width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty())
            return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(f, trial) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ' || ch == '\n') {
            flush_word();
            if (ch == '\n') {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word += ch;
        }
    }
    flush_word();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

namespace {
std::vector<uint8_t> g_page;

// Where the last drawn page put what the pointer can reach, as fractions of the page, so a pointer
// event (on the input thread) finds what it is over without redrawing anything. Under g_mu.
enum HitKind : uint8_t { HIT_TAB, HIT_CLOSE, HIT_ACTION, HIT_DECREASE, HIT_INCREASE, HIT_ROW, HIT_SLIDER, HIT_ENTRY,
                         HIT_ENTRY_APPLY };
struct Hit {
    float x0, y0, x1, y1;
    HitKind kind;
    size_t index; // the tab, or the line in the snapshot
};
std::vector<Hit> g_hits;
int g_hits_tab = -1;
// The list's scroll position persists between draws and moves only as far as keeps the selected
// line in view, so pointing at a visible row never scrolls the list under the pointer.
size_t g_first = 0;
int g_first_tab = -1;
// What the pointer is over that the menu model does not know about (a page name, Close), and a
// counter of such changes for the redraw key.
int g_hover_kind = -1;
size_t g_hover_index = 0;
uint64_t g_view_serial = 0;
int g_wheel = 0; // a trackpad's partial notches, accumulated
// The mouse wheel scrolls the list itself (2026-11-17): rows to move the view by at the next draw. It used to move
// the selection a row, and the pointer's next motion chose the row under it again, so the list hardly moved. The
// view follows the selection only when the selection changes (keys, or pointing at a row), never on a scroll.
int g_scroll_rows = 0;
size_t g_last_sel = size_t(-1);
// 2026-11-20 (the person: scrolling went the wrong way and jumped a row at a time): the list scrolls by pixels, in
// 1920x1080 reference units so a resize keeps the place; wheel units wait here for the next draw. A row the pointer
// chose by hovering is not scrolled to (only a key's choice is), so the list never moves under the pointer.
double g_scroll_ref = 0;
int g_wheel_units = 0;
size_t g_hover_sel = size_t(-1);
std::vector<uint8_t> g_list_page;
// A slider being dragged: its line, the page it is on and its track (fractions of the page).
struct Drag {
    bool on = false;
    int tab = -1;
    size_t line = 0;
    float x0 = 0, x1 = 1;
} g_drag;

// A small filled triangle pointing up (dir < 0) or down, centred on (cx, cy).
void triangle(Canvas &c, int cx, int cy, int half, int dir, Rgba ink) {
    for (int r = 0; r <= half; ++r) {
        const int y = cy + (dir < 0 ? r - half / 2 : half / 2 - r); // r = 0 is the tip
        for (int x = cx - r; x <= cx + r; ++x)
            c.blend(x, y, ink, 255);
    }
}
void outline(Canvas &c, int x0, int y0, int x1, int y1, int t, Rgba ink) {
    c.fill(x0, y0, x1, y0 + t, ink);
    c.fill(x0, y1 - t, x1, y1, ink);
    c.fill(x0, y0, x0 + t, y1, ink);
    c.fill(x1 - t, y0, x1, y1, ink);
}
// A rounded rectangle with smooth edges (r up to half the shorter side: a capsule), filled, or
// only its edge `stroke` pixels wide.
void round_rect(Canvas &c, int x0, int y0, int x1, int y1, double r, Rgba ink, double stroke = 0) {
    r = std::max(0.0, std::min({r, (x1 - x0) / 2.0, (y1 - y0) / 2.0}));
    if (stroke <= 0) {
        // 2026-11-24 (the menu's fades redraw the page many times a second): filled shapes take the work only where
        // an edge or a corner is - whole interior spans are blended without a distance per pixel.
        const int rc = int(std::ceil(r)) + 1;
        for (int y = std::max(y0, 0); y < std::min(y1, c.h); ++y) {
            const bool corner_row = y < y0 + rc || y >= y1 - rc;
            const int inner0 = corner_row ? x0 + rc : x0 + 1, inner1 = corner_row ? x1 - rc : x1 - 1;
            for (int x = std::max(x0, 0); x < std::min(x1, c.w); ++x) {
                if (x >= inner0 && x < inner1 && y > y0 && y < y1 - 1) {
                    const int span_end = std::min(inner1, c.w);
                    for (; x < span_end; ++x)
                        c.blend(x, y, ink, 255);
                    --x;
                    continue;
                }
                const double px = x + 0.5, py = y + 0.5;
                const double dx = std::max({x0 + r - px, px - (x1 - r), 0.0});
                const double dy = std::max({y0 + r - py, py - (y1 - r), 0.0});
                const double d = dx > 0 || dy > 0 ? std::sqrt(dx * dx + dy * dy) - r
                                                  : -std::min({px - x0, x1 - px, py - y0, y1 - py});
                const double cov = std::clamp(0.5 - d, 0.0, 1.0);
                if (cov > 0)
                    c.blend(x, y, ink, int(cov * 255 + 0.5));
            }
        }
        return;
    }
    for (int y = std::max(y0, 0); y < std::min(y1, c.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, c.w); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double dx = std::max({x0 + r - px, px - (x1 - r), 0.0});
            const double dy = std::max({y0 + r - py, py - (y1 - r), 0.0});
            // The distance to the edge, negative inside.
            const double d = dx > 0 || dy > 0 ? std::sqrt(dx * dx + dy * dy) - r
                                              : -std::min({px - x0, x1 - px, py - y0, y1 - py});
            double cov = std::clamp(0.5 - d, 0.0, 1.0);
            if (stroke > 0)
                cov = std::min(cov, std::clamp(0.5 + d + stroke, 0.0, 1.0));
            if (cov > 0)
                c.blend(x, y, ink, int(cov * 255 + 0.5));
        }
}
// An on/off switch, macOS style, its left edge at x and centred on cy.
void toggle(Canvas &c, int x, int cy, int w, int h, bool on, Rgba accent) {
    round_rect(c, x, cy - h / 2, x + w, cy + h - h / 2, h / 2.0, on ? accent : Rgba{255, 255, 255, 52});
    const int inset = std::max(2, h / 12), d = h - 2 * inset;
    const int kx = on ? x + w - inset - d : x + inset, ky = cy - h / 2 + inset;
    round_rect(c, kx, ky + 1, kx + d, ky + d + 1, d / 2.0, Rgba{0, 0, 0, 60}); // its shadow
    round_rect(c, kx, ky, kx + d, ky + d, d / 2.0, Rgba{255, 255, 255, 255});
}
} // namespace

namespace {
// The colours of one look. The glass look's are white at various strengths, so the glass shows
// through, with the host's accent.
struct Palette {
    Rgba ink, soft, faint, edge, select, accent, row;
};
Palette palette() {
    if (!g_glass)
        return {kInk, kSoft, kFaint, kEdge, kSelect, kAccent, {214, 216, 222, 255}};
    return {{255, 255, 255, 255}, {255, 255, 255, 190}, {255, 255, 255, 150}, {255, 255, 255, 46},
            {255, 255, 255, 40},  g_glass_accent,        {255, 255, 255, 235}};
}
} // namespace

// 2026-11-24 redesign (the person: categories and subcategories, the marks under each row, a "Saved" / "Waiting for
// restart" state, buttons floating at the bottom, hints set apart and short). The panel, from the top: the page
// tabs (a segmented control) with the key hint at the right; the list - category headers, subcategory headers over
// a card of rows, each row two lines (its name, then its marks as chips: shared or this save's own, when it takes
// effect, and its state); then, fixed at the bottom whatever the scroll, the hint card of the chosen row and the bar
// of the page's buttons (Defaults at the left; Discard, Save, Close at the right) with the menu's status between.
int mods_menu_render(int w, int h, uint64_t *generation, const uint8_t **pixels) {
    if (!generation || !pixels || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return 0;
    MenuSnapshot m;
    if (!mods_menu_snapshot(&m))
        return 0;
    std::lock_guard lock(g_mu);
    const uint64_t key = (m.generation ^ (uint64_t(uint32_t(w)) << 40) ^ (uint64_t(uint32_t(h)) << 20)) +
                         g_view_serial * 0x9e3779b97f4a7c15ull + (g_glass ? 0x5bd1e995ull : 0);
    if (*generation == key)
        return 1; // unchanged: the caller keeps what it has
    if (getenv("RECOMP_MENU_FADE_LOG")) // diagnostic (2026-11-24): each redraw with a "Saved" mark and its strength
        for (const MenuLine &l : m.lines)
            if (l.state_kind == 1)
                fprintf(stderr, "mods: menu redraw, \"%s\" saved mark %.2f\n", l.label.c_str(), l.state_alpha);
    const Palette P = palette();
    const bool glass = g_glass;
    std::vector<uint8_t> *rgba = &g_page;
    rgba->assign(size_t(w) * size_t(h) * 4, 0);
    Canvas c{w, h, rgba};
    if (!glass)
        c.fill(0, 0, w, h, kDim);
    std::vector<Hit> hits;
    auto hit = [&](int x0, int y0, int x1, int y1, HitKind kind, size_t index) {
        hits.push_back({float(x0) / w, float(y0) / h, float(x1) / w, float(y1) / h, kind, index});
    };
    const bool hover_tab_kind = g_hover_kind == HIT_TAB;
    const bool hover_action_kind = g_hover_kind == HIT_ACTION;
    auto mix = [](Rgba a, Rgba b, int t) { // t/255 of b over a
        return Rgba{uint8_t(a.r + (b.r - a.r) * t / 255), uint8_t(a.g + (b.g - a.g) * t / 255),
                    uint8_t(a.b + (b.b - a.b) * t / 255), a.a};
    };
    const Rgba accent = P.accent;
    const Rgba accent_text = mix(accent, Rgba{255, 255, 255, 255}, 120);
    const Rgba green = {52, 199, 89, 255}, orange = {255, 159, 10, 255};

    // Laid out on a 1920x1080 reference and scaled to fit the page.
    const double s = std::min(w / 1920.0, h / 1080.0);
    auto u = [s](double v) { return int(std::lround(v * s)); };
    const int pw = std::min(w - u(40), u(1560)), ph = std::min(h - u(30), u(1020));
    const int px = (w - pw) / 2, py = (h - ph) / 2;
    const int radius = u(40);
    if (glass) {
        // The host's glass goes here; the page leaves it clear but for a dark tint, so text reads on any picture.
        g_panel[0] = float(px) / w, g_panel[1] = float(py) / h;
        g_panel[2] = float(px + pw) / w, g_panel[3] = float(py + ph) / h;
        g_panel[4] = float(radius) / h;
        g_panel_valid = true;
        round_rect(c, px, py, px + pw, py + ph, radius, Rgba{8, 10, 16, 120});
    } else {
        round_rect(c, px - u(2), py - u(2), px + pw + u(2), py + ph + u(2), radius + u(2), kEdge);
        round_rect(c, px, py, px + pw, py + ph, radius, Rgba{16, 18, 24, 248});
    }
    const int left = px + u(60), right = px + pw - u(60);

    // ---- the page tabs: a segmented control, the key hint at the right ----
    const Font tab_font = font(u(27), true);
    const int tab_y = py + u(32);
    const int seg_pad = u(28), seg_gap = u(4);
    const int seg_h = tab_font.height + u(18);
    {
        int track = 0;
        for (size_t i = 0; i < m.tabs.size(); ++i)
            track += measure(tab_font, m.tabs[i]) + 2 * seg_pad + (i ? seg_gap : 0);
        round_rect(c, left - u(5), tab_y - u(5), left + track + u(5), tab_y + seg_h + u(5), 1e9, Rgba{255, 255, 255, 22});
    }
    int tx = left;
    for (size_t i = 0; i < m.tabs.size(); ++i) {
        const bool on = int(i) == m.tab;
        const bool hover = !on && hover_tab_kind && g_hover_index == i;
        const int adv = measure(tab_font, m.tabs[i]);
        const int bx0 = tx, by0 = tab_y, bx1 = tx + adv + 2 * seg_pad, by1 = tab_y + seg_h;
        if (on)
            round_rect(c, bx0, by0, bx1, by1, 1e9, Rgba{accent.r, accent.g, accent.b, 235});
        else if (hover)
            round_rect(c, bx0, by0, bx1, by1, 1e9, Rgba{255, 255, 255, 30});
        text(c, tab_font, bx0 + seg_pad, tab_y + (seg_h - tab_font.height) / 2, m.tabs[i], on || hover ? P.ink : P.soft);
        hit(bx0, by0, bx1, by1, HIT_TAB, i);
        tx = bx1 + seg_gap;
    }
    const Font hint_font = font(u(19), false);
    {
        const int pw2 = measure(hint_font, m.prompts);
        const int hx = std::max(tx + u(24), right - pw2);
        text(c, hint_font, hx, tab_y + (seg_h - hint_font.height) / 2, m.prompts, P.faint, right);
    }
    const int rule_y = tab_y + seg_h + u(22);

    // ---- the bottom: the button bar, the hint card above it ----
    const Font button_font = font(u(23), true), status_font = font(u(20), false);
    const int bar_h = button_font.height + u(22);
    const int bar_y1 = py + ph - u(30), bar_y0 = bar_y1 - bar_h;
    std::vector<size_t> buttons;
    for (size_t i = 0; i < m.lines.size(); ++i)
        if (m.lines[i].footer)
            buttons.push_back(i);
    auto draw_button = [&](size_t i, int x0) {
        const MenuLine &b = m.lines[i];
        const bool hover = hover_action_kind && g_hover_index == i;
        const int bw = measure(button_font, b.label) + u(44);
        const bool primary = b.primary && b.enabled;
        Rgba fill = primary ? Rgba{accent.r, accent.g, accent.b, 245} : Rgba{255, 255, 255, uint8_t(hover ? 52 : 30)};
        round_rect(c, x0, bar_y0, x0 + bw, bar_y1, 1e9, fill);
        if (primary && hover)
            round_rect(c, x0, bar_y0, x0 + bw, bar_y1, 1e9, Rgba{255, 255, 255, 36});
        if (b.selected) // the keyboard's choice: a ring
            round_rect(c, x0 - u(4), bar_y0 - u(4), x0 + bw + u(4), bar_y1 + u(4), 1e9, accent_text, std::max(2, u(3)));
        text(c, button_font, x0 + u(22), bar_y0 + (bar_h - button_font.height) / 2, b.label,
             b.enabled || !b.primary ? P.ink : P.faint);
        hit(x0, bar_y0, x0 + bw, bar_y1, HIT_ACTION, i);
        return bw;
    };
    int bl = left; // Defaults at the left, the rest from the right: Close, Save, Discard
    int br = right;
    for (size_t i : buttons)
        if (m.lines[i].label == mods_menu_tr("Defaults"))
            bl += draw_button(i, bl) + u(16);
    for (const char *name : {"Close", "Save", "Discard"})
        for (size_t i : buttons)
            if (m.lines[i].label == mods_menu_tr(name)) {
                const int bw = measure(button_font, m.lines[i].label) + u(44);
                br -= bw;
                draw_button(i, br);
                br -= u(16);
            }
    if (!m.status.empty())
        text(c, status_font, bl + u(10), bar_y0 + (bar_h - status_font.height) / 2, m.status, accent_text, br - u(10));

    // the hint card: the chosen row's name and its hint, line by line
    const Font help_title_font = font(u(23), true), help_font = font(u(21), false);
    std::vector<std::string> help;
    {
        const std::vector<std::string> lines = wrap(help_font, m.help, right - left - u(56));
        for (const std::string &l : lines)
            if (!l.empty() || (!help.empty() && !help.back().empty()))
                help.push_back(l);
        while (!help.empty() && help.back().empty())
            help.pop_back();
        if (help.size() > 7)
            help.resize(7);
    }
    const int help_line = help_font.height + u(3);
    const int card_inner = (m.help_title.empty() ? 0 : help_title_font.height + u(6)) + int(help.size()) * help_line;
    const int card_h = std::max(u(96), card_inner + u(32));
    const int card_y1 = bar_y0 - u(22), card_y0 = card_y1 - card_h;
    round_rect(c, left - u(20), card_y0, right + u(20), card_y1, u(20), Rgba{255, 255, 255, 18});
    round_rect(c, left - u(20), card_y0, right + u(20), card_y1, u(20), Rgba{255, 255, 255, 26}, std::max(1, u(1)));
    {
        // a small "i" in an accent circle, then the text
        const int ic = u(15), icx = left + ic - u(4), icy = card_y0 + u(16) + help_title_font.height / 2;
        round_rect(c, icx - ic, icy - ic, icx + ic, icy + ic, 1e9, Rgba{accent.r, accent.g, accent.b, 220});
        const Font ifont = font(u(20), true);
        text(c, ifont, icx - measure(ifont, "i") / 2, icy - ifont.height / 2, "i", Rgba{255, 255, 255, 255});
        const int hx = left + u(40);
        int hy = card_y0 + u(16);
        if (!m.help_title.empty()) {
            text(c, help_title_font, hx, hy, m.help_title, P.ink, right);
            hy += help_title_font.height + u(6);
        }
        for (const std::string &l : help) {
            text(c, help_font, hx, hy, l, P.soft, right);
            hy += help_line;
        }
    }

    // ---- the list ----
    const Font row_font = font(u(26), false), value_font = font(u(25), true);
    const Font cat_font = font(u(31), true), sub_font = font(u(19), true), chip_font = font(u(16), true);
    const Font head_font = font(u(21), true);
    const int cat_h = u(66), sub_h = u(40), head_h = u(56);
    const int row1_h = u(60), row2_h = u(88);
    const int list_top = rule_y, list_bottom = card_y0 - u(14), avail = std::max(1, list_bottom - list_top);
    const int pad_h = u(420); // the controller picture under its line (2026-11-22; 9 legend rows)
    auto two_lines = [](const MenuLine &l) { return !l.scope_text.empty() || l.timing_kind || l.state_kind; };
    auto height_of = [&](const MenuLine &l) {
        if (l.footer)
            return 0;
        if (l.pad_kind >= 0)
            return l.pad_kind > 0 ? head_h + pad_h : head_h;
        if (l.kind == MenuLine::HEADER)
            return l.level == 1 ? sub_h : cat_h;
        return two_lines(l) ? row2_h : row1_h;
    };
    const size_t n = m.lines.size();
    std::vector<int> top(n + 1, 0);
    for (size_t i = 0; i < n; ++i)
        top[i + 1] = top[i] + height_of(m.lines[i]);
    const int content_h = top[n] + u(12);
    size_t sel = 0;
    for (size_t i = 0; i < n; ++i)
        if (m.lines[i].selected)
            sel = i;
    if (m.tab != g_first_tab) {
        g_first_tab = m.tab;
        g_scroll_ref = 0;
        g_wheel_units = 0;
        g_last_sel = size_t(-1);
    }
    double scroll = g_scroll_ref * s;
    if (g_wheel_units) {
        scroll -= g_wheel_units / 120.0 * u(90); // one notch: about a row
        g_wheel_units = 0;
    }
    const double max_scroll = std::max(0, content_h - avail);
    static int last_avail = 0;
    const bool follow = (sel != g_last_sel || (avail < last_avail && sel == g_last_sel)) && sel != g_hover_sel &&
                        sel < n && !m.lines[sel].footer;
    last_avail = avail;
    g_last_sel = sel;
    if (follow && n) {
        // a key moved the choice: bring the row (and the headers just above it) into view
        int want_top = top[sel], want_bottom = top[sel + 1];
        for (size_t k = sel; k > 0 && m.lines[k - 1].kind == MenuLine::HEADER && m.lines[k - 1].pad_kind < 0; --k)
            want_top = top[k - 1];
        want_bottom += u(30); // clear of the fade at the list's bottom edge
        if (want_top < scroll)
            scroll = want_top;
        else if (want_bottom > scroll + avail)
            scroll = want_bottom - avail;
    }
    scroll = std::clamp(scroll, 0.0, max_scroll);
    g_scroll_ref = s > 0 ? scroll / s : 0;
    const int off = int(std::lround(scroll));

    // Into a page of its own, then only the list's band onto the page: rows half out of view are cut cleanly.
    g_list_page.assign(size_t(w) * size_t(h) * 4, 0);
    Canvas lc{w, h, &g_list_page};
    auto band_hit = [&](int x0, int y0, int x1, int y1, HitKind kind, size_t index) {
        y0 = std::max(y0, list_top), y1 = std::min(y1, list_bottom);
        if (y1 > y0)
            hit(x0, y0, x1, y1, kind, index);
    };
    // the cards: one behind each run of rows between two headers
    for (size_t i = 0; i < n;) {
        const MenuLine &l = m.lines[i];
        if (l.footer || l.kind == MenuLine::HEADER || l.pad_kind >= 0) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && !m.lines[j].footer && m.lines[j].kind != MenuLine::HEADER && m.lines[j].pad_kind < 0)
            ++j;
        const int y0 = list_top + top[i] - off, y1 = list_top + top[j] - off;
        if (y1 > list_top && y0 < list_bottom) {
            round_rect(lc, left - u(20), y0, right + u(20), y1, u(18), Rgba{255, 255, 255, uint8_t(glass ? 16 : 12)});
            for (size_t k = i + 1; k < j; ++k) { // hairlines between the rows
                const int yy = list_top + top[k] - off;
                lc.fill(left, yy, right, yy + std::max(1, u(1)), Rgba{255, 255, 255, 20});
            }
        }
        i = j;
    }
    const int col_w = u(380); // the value column
    const int col_x0 = right - col_w;
    const int arrow_w = measure(value_font, "<") + u(20);
    const int switch_w = u(58), switch_h = u(32);
    const Rgba sel_fill = Rgba{accent.r, accent.g, accent.b, uint8_t(glass ? 70 : 60)};
    // a small rounded label: returns its width
    auto chip = [&](int x, int y, const std::string &t, Rgba fill, Rgba ink) {
        const int tw = measure(chip_font, t) + u(20), th = chip_font.height + u(8);
        round_rect(lc, x, y, x + tw, y + th, 1e9, fill);
        text(lc, chip_font, x + u(10), y + u(4), t, ink);
        return tw;
    };
    for (size_t i = 0; i < n; ++i) {
        const MenuLine &l = m.lines[i];
        if (l.footer)
            continue;
        const int lh = height_of(l);
        const int y = list_top + top[i] - off;
        if (y + lh <= list_top || y >= list_bottom)
            continue;
        if (l.pad_kind >= 0) {
            // The Controls page's controller line: what is connected, and (when one is) a picture of the pad with
            // its buttons in the maker's own marks and colours, beside a legend of what they do in the game.
            text(lc, head_font, left, y + head_h - head_font.height - u(8), l.label,
                 l.pad_kind > 0 ? P.accent : P.soft, right);
            if (l.pad_kind <= 0)
                continue;
            const bool ps = l.pad_kind == 2;
            const int top_y = y + head_h;
            const int cx = left + u(260), cy = top_y + u(170);
            const Rgba body = glass ? Rgba{255, 255, 255, 46} : Rgba{P.edge.r, P.edge.g, P.edge.b, 255};
            const Rgba part = glass ? Rgba{0, 0, 0, 110} : Rgba{0, 0, 0, 140};
            const Rgba ink2 = P.ink;
            const Font small = font(u(18), true), mid = font(u(22), true);
            auto circle = [&](int x, int yy, int r, Rgba c) { round_rect(lc, x - r, yy - r, x + r, yy + r, 1e9, c); };
            auto centred = [&](const Font &f, int x, int yy, const std::string &t, Rgba c) {
                text(lc, f, x - measure(f, t) / 2, yy - f.height / 2, t, c);
            };
            // shoulders and triggers
            round_rect(lc, cx - u(180), cy - u(150), cx - u(95), cy - u(122), u(10), part);
            round_rect(lc, cx + u(95), cy - u(150), cx + u(180), cy - u(122), u(10), part);
            round_rect(lc, cx - u(175), cy - u(122), cx - u(100), cy - u(104), u(8), part);
            round_rect(lc, cx + u(100), cy - u(122), cx + u(175), cy - u(104), u(8), part);
            centred(small, cx - u(137), cy - u(136), ps ? "L2" : "LT", ink2);
            centred(small, cx + u(137), cy - u(136), ps ? "R2" : "RT", ink2);
            centred(small, cx - u(137), cy - u(113), ps ? "L1" : "LB", ink2);
            centred(small, cx + u(137), cy - u(113), ps ? "R1" : "RB", ink2);
            // body and grips
            round_rect(lc, cx - u(200), cy - u(105), cx + u(200), cy + u(60), u(70), body);
            round_rect(lc, cx - u(205), cy - u(30), cx - u(95), cy + u(140), u(55), body);
            round_rect(lc, cx + u(95), cy - u(30), cx + u(205), cy + u(140), u(55), body);
            // sticks and d-pad: Xbox has the left stick high, PlayStation the d-pad high
            const int ls_x = ps ? cx - u(65) : cx - u(120), ls_y = ps ? cy + u(30) : cy - u(40);
            const int dp_x = ps ? cx - u(120) : cx - u(65), dp_y = ps ? cy - u(40) : cy + u(30);
            const int rs_x = cx + u(65), rs_y = cy + u(30);
            circle(ls_x, ls_y, u(30), part);
            circle(ls_x, ls_y, u(18), body);
            circle(rs_x, rs_y, u(30), part);
            circle(rs_x, rs_y, u(18), body);
            round_rect(lc, dp_x - u(28), dp_y - u(9), dp_x + u(28), dp_y + u(9), u(3), part);
            round_rect(lc, dp_x - u(9), dp_y - u(28), dp_x + u(9), dp_y + u(28), u(3), part);
            // view/share and menu/options
            round_rect(lc, cx - u(42), cy - u(62), cx - u(18), cy - u(48), 1e9, part);
            round_rect(lc, cx + u(18), cy - u(62), cx + u(42), cy - u(48), 1e9, part);
            // face buttons
            struct Face {
                int dx, dy;
                const char *mark;
                Rgba c;
            };
            const Face xb[4] = {{0, -32, "Y", {230, 190, 30, 255}}, {32, 0, "B", {215, 50, 50, 255}},
                                {0, 32, "A", {40, 160, 60, 255}}, {-32, 0, "X", {40, 110, 220, 255}}};
            const Face pf[4] = {{0, -32, "\xe2\x96\xb3", {60, 190, 150, 255}}, {32, 0, "\xe2\x97\x8b", {225, 75, 75, 255}},
                                {0, 32, "\xc3\x97", {100, 140, 230, 255}}, {-32, 0, "\xe2\x96\xa1", {220, 120, 200, 255}}};
            const int fx = cx + u(122), fy = cy - u(40);
            for (int k = 0; k < 4; ++k) {
                const Face &f = ps ? pf[k] : xb[k];
                circle(fx + u(f.dx), fy + u(f.dy), u(16), f.c);
                centred(mid, fx + u(f.dx), fy + u(f.dy), f.mark, Rgba{255, 255, 255, 255});
            }
            // the legend: what each button does (as checked in the game; the rest is the game's own setting)
            const int lx = left + u(560);
            int ly = top_y + u(30);
            const Font lf = font(u(22), false), lb = font(u(22), true);
            auto row = [&](const std::string &button, const char *what) {
                text(lc, lb, lx, ly, button, P.accent, lx + u(220));
                text(lc, lf, lx + u(230), ly, mods_menu_tr(what), ink2, right);
                ly += lf.height + u(9);
            };
            // NFS Undercover's console layout (core.nfsu pad_layout, the default; 2026-11-22)
            row(ps ? "R2" : "RT", "Gas");
            row(ps ? "L2" : "LT", "Brake, reverse");
            row("L stick", "Steering");
            row(ps ? "\xc3\x97 (cross)" : "A", "Select in menus, nitro");
            row(ps ? "\xe2\x97\x8b (circle)" : "B", "Back, speedbreaker");
            row(ps ? "\xe2\x96\xa1 (square)" : "X", "Handbrake");
            row(ps ? "\xe2\x96\xb3 (triangle)" : "Y", "Look back");
            row(ps ? "Options" : "Menu \xe2\x98\xb0", "Pause");
            row(ps ? "D-pad" : "D-pad", "Menu items");
            ly += u(4);
            text(lc, lf, lx, ly, mods_menu_tr("Helicopter: stick flies, right trigger up, left down."), P.soft, right);
            continue;
        }
        if (l.kind == MenuLine::HEADER) {
            if (l.level == 1)
                text(lc, sub_font, left, y + lh - sub_font.height - u(10), l.label, accent_text, right);
            else
                text(lc, cat_font, left - u(16), y + lh - cat_font.height - u(12), l.label, P.ink, right);
            continue;
        }
        if (l.selected) {
            round_rect(lc, left - u(16), y + u(4), right + u(16), y + lh - u(4), u(14), sel_fill);
            round_rect(lc, left - u(16), y + u(16), left - u(10), y + lh - u(16), u(3), accent_text);
        }
        const Rgba ink = !l.enabled ? P.faint : P.ink;
        const bool two = two_lines(l);
        const int ty = two ? y + u(12) : y + (lh - row_font.height) / 2;
        if (l.kind == MenuLine::ACTION) {
            text(lc, value_font, left, ty, l.label, l.enabled ? accent_text : P.faint, right);
            band_hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
            continue;
        }
        const int label_end = col_x0 - u(24);
        text(lc, row_font, left, ty, l.label, ink, label_end);
        if (two) {
            // the marks: where it is kept, when it takes effect, and what happened to the last change
            int cx = left;
            const int cy = ty + row_font.height + u(8);
            if (!l.scope_text.empty())
                cx += chip(cx, cy, l.scope_text,
                           l.scope == 2 ? Rgba{accent.r, accent.g, accent.b, 80} : Rgba{255, 255, 255, 30},
                           l.scope == 2 ? accent_text : P.soft) + u(8);
            if (l.timing_kind && !l.timing.empty()) {
                const Rgba f = l.timing_kind == 1   ? Rgba{green.r, green.g, green.b, 48}
                               : l.timing_kind == 2 ? Rgba{orange.r, orange.g, orange.b, 52}
                               : l.timing_kind == 3 ? Rgba{accent.r, accent.g, accent.b, 52}
                                                    : Rgba{255, 255, 255, 30};
                const Rgba t = l.timing_kind == 1   ? Rgba{125, 228, 150, 255}
                               : l.timing_kind == 2 ? Rgba{255, 196, 110, 255}
                               : l.timing_kind == 3 ? accent_text
                                                    : P.soft;
                cx += chip(cx, cy, l.timing, f, t) + u(8);
            }
            if (l.state_kind && l.state_alpha > 0.01f) {
                const float a = std::clamp(l.state_alpha, 0.0f, 1.0f);
                Rgba f = l.state_kind == 1   ? Rgba{green.r, green.g, green.b, 235}
                         : l.state_kind == 2 ? Rgba{orange.r, orange.g, orange.b, 240}
                                             : Rgba{accent.r, accent.g, accent.b, 240};
                Rgba t = l.state_kind == 3 ? Rgba{255, 255, 255, 255} : Rgba{20, 24, 16, 255};
                f.a = uint8_t(f.a * a), t.a = uint8_t(t.a * a);
                // it also slides in a few pixels from the left as it appears
                chip(cx - int(std::lround((1.0f - a) * u(10))), cy, (l.state_kind == 1 ? "\xe2\x9c\x93 " : "") + l.state_text, f, t);
            }
        }
        const bool is_switch = l.adjustable && l.switch_on >= 0;
        if (is_switch) {
            // the switch at the column's right; the row (or the switch) flips it
            band_hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
            toggle(lc, right - switch_w, y + lh / 2, switch_w, switch_h, l.switch_on == 1, l.enabled ? accent : P.faint);
            continue;
        }
        const int vy = y + (lh - value_font.height) / 2;
        if (l.adjustable && l.entry) {
            // a number typed into a field, its own Apply button at the right (click the field to type)
            const std::string btn = mods_menu_tr("Apply");
            const int bh = u(42), by0 = y + (lh - bh) / 2;
            const int bw = measure(row_font, btn) + u(30);
            const int bx1 = right, bx0 = right - bw;
            const bool can = l.pending;
            round_rect(lc, bx0, by0, bx1, by0 + bh, 1e9, can ? accent : Rgba{255, 255, 255, 36});
            text(lc, row_font, bx0 + u(15), by0 + (bh - row_font.height) / 2, btn, can ? Rgba{255, 255, 255, 255} : P.faint);
            const int fx0 = col_x0 - u(40), fx1 = bx0 - u(12);
            round_rect(lc, fx0, by0, fx1, by0 + bh, u(10), Rgba{0, 0, 0, 80});
            round_rect(lc, fx0, by0, fx1, by0 + bh, u(10), l.typing ? accent : Rgba{255, 255, 255, 70}, std::max(1, u(2)));
            const int vtw = measure(value_font, l.value);
            const int vx = std::max(fx0 + u(12), fx1 - u(20) - vtw);
            text(lc, value_font, vx, by0 + (bh - value_font.height) / 2, l.value, l.pending ? accent_text : ink, fx1 - u(16));
            if (l.typing) { // the caret
                const int cx = std::min(fx1 - u(12), vx + vtw + u(4));
                lc.fill(cx, by0 + u(8), cx + std::max(1, u(2)), by0 + bh - u(8), P.ink);
            }
            band_hit(bx0, y, bx1 + u(20), y + lh, HIT_ENTRY_APPLY, i);
            band_hit(fx0, y, fx1, y + lh, HIT_ENTRY, i);
            band_hit(left - u(20), y, fx0, y + lh, HIT_ROW, i);
            continue;
        }
        if (l.adjustable && l.slider >= 0) {
            // value text at the right, the track on its left; click or drag anywhere on the track
            const int vw = u(96);
            const int tx0 = col_x0 + u(10), tx1 = right - vw - u(16), cy = y + lh / 2;
            const float f = std::clamp(l.slider, 0.0f, 1.0f);
            const int kx = tx0 + int(std::lround(f * (tx1 - tx0)));
            round_rect(lc, tx0, cy - u(3), tx1, cy + u(3), 1e9, Rgba{255, 255, 255, 56});
            round_rect(lc, tx0, cy - u(3), kx, cy + u(3), 1e9, l.enabled ? accent : P.faint);
            const int kr = u(13);
            round_rect(lc, kx - kr, cy - kr + 1, kx + kr, cy + kr + 1, 1e9, Rgba{0, 0, 0, 60});
            round_rect(lc, kx - kr, cy - kr, kx + kr, cy + kr, 1e9, Rgba{255, 255, 255, 255});
            const int vtw = measure(value_font, l.value);
            text(lc, value_font, right - vtw, vy, l.value, l.pending ? accent_text : ink);
            band_hit(tx0 - kr, y, tx1 + kr, y + lh, HIT_SLIDER, i);
            band_hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
            continue;
        }
        // a value between fixed arrows in a soft capsule: "<" at its left edge, ">" at its right, the value centred
        const int vw = measure(value_font, l.value);
        const int inner0 = col_x0 + arrow_w, inner1 = right - arrow_w;
        const int vx = std::max(inner0, inner0 + (inner1 - inner0 - vw) / 2);
        if (l.adjustable) {
            const int ch = value_font.height + u(16);
            round_rect(lc, col_x0, y + (lh - ch) / 2, right, y + (lh + ch) / 2, 1e9, Rgba{255, 255, 255, uint8_t(l.selected ? 34 : 20)});
            const Rgba arrow = l.selected ? accent_text : Rgba{255, 255, 255, 120};
            text(lc, value_font, col_x0 + u(14), vy, "<", arrow);
            text(lc, value_font, right - arrow_w + u(4), vy, ">", arrow);
            band_hit(col_x0 - u(8), y, inner0, y + lh, HIT_DECREASE, i);
            band_hit(inner1, y, right + u(20), y + lh, HIT_INCREASE, i);
        }
        text(lc, value_font, vx, vy, l.value, l.pending ? accent_text : ink, inner1);
        band_hit(left - u(20), y, right + u(20), y + lh, HIT_ROW, i);
    }
    // the list's band onto the page (both premultiplied over transparent), its last rows fading under the hint card
    const int fade = u(28);
    for (int yy = std::max(list_top, 0); yy < std::min(list_bottom, h); ++yy) {
        const uint8_t *src = g_list_page.data() + size_t(yy) * w * 4;
        uint8_t *dst = rgba->data() + size_t(yy) * w * 4;
        const int keep = content_h - off > avail && list_bottom - yy < fade ? 255 * (list_bottom - yy) / fade : 255;
        for (int xx = 0; xx < w; ++xx) {
            const int a = src[4 * xx + 3] * keep / 255;
            if (!a)
                continue;
            for (int k = 0; k < 3; ++k)
                dst[4 * xx + k] =
                    uint8_t(std::min(255, src[4 * xx + k] * keep / 255 + dst[4 * xx + k] * (255 - a) / 255));
            dst[4 * xx + 3] = uint8_t(std::min(255, a + dst[4 * xx + 3] * (255 - a) / 255));
        }
    }
    // a thin scroll bar when the list is longer than its space
    if (content_h > avail) {
        const int bx = right + u(34), bw = u(5);
        const int bh = std::max(u(40), int(double(avail) * avail / content_h));
        const int by = list_top + int((avail - bh) * (max_scroll > 0 ? scroll / max_scroll : 0));
        round_rect(c, bx, list_top, bx + bw, list_bottom, 1e9, Rgba{255, 255, 255, 24});
        round_rect(c, bx, by, bx + bw, by + bh, 1e9, Rgba{255, 255, 255, 130});
    }
    g_hits = std::move(hits);
    g_hits_tab = m.tab;
    *generation = key;
    *pixels = g_page.data();
    return 2;
}

void mods_menu_set_text_backend(const MenuTextBackend *backend) {
    std::lock_guard lock(g_mu);
    g_text = backend && backend->font && backend->metrics && backend->measure && backend->render ? backend : nullptr;
    g_runs.clear();
    ++g_view_serial; // redraw with it
}

void mods_menu_set_style(int glass, uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard lock(g_mu);
    g_glass = glass != 0;
    g_glass_accent = {r, g, b, 255};
    g_panel_valid = false;
    ++g_view_serial;
}

int mods_menu_panel(float *x0, float *y0, float *x1, float *y1, float *radius) {
    std::lock_guard lock(g_mu);
    if (!g_panel_valid || !g_glass)
        return 0;
    *x0 = g_panel[0], *y0 = g_panel[1], *x1 = g_panel[2], *y1 = g_panel[3], *radius = g_panel[4];
    return 1;
}

// The pointer over the page, as fractions of the page (u, v in 0..1). button: -1 for motion,
// else 0 left / 1 right / 2 middle with down; wheel: WM_MOUSEWHEEL units, positive away from
// the user. Returns 1 (consumed) whenever the menu is open, so nothing reaches the game.
int mods_menu_pointer(float pu, float pv, int button, int down, int wheel) {
    if (!mods_menu_is_open())
        return 0;
    const Hit *found = nullptr;
    Hit copy{};
    int tab = -1;
    {
        std::lock_guard lock(g_mu);
        tab = g_hits_tab;
        for (const Hit &h : g_hits)
            if (pu >= h.x0 && pu < h.x1 && pv >= h.y0 && pv < h.y1) {
                copy = h;
                found = &copy;
                break;
            }
        const int kind =
            found && (found->kind == HIT_TAB || found->kind == HIT_CLOSE || found->kind == HIT_ACTION) ? int(found->kind)
                                                                                                    : -1;
        const size_t index = found ? found->index : 0;
        if (kind != g_hover_kind || (kind >= 0 && index != g_hover_index)) {
            g_hover_kind = kind;
            g_hover_index = index;
            ++g_view_serial;
        }
        if (wheel) {
            g_wheel_units += wheel; // pixels at the next draw (the host gives the direction the fingers moved)
            ++g_view_serial;
        }
    }
    // a slider being dragged follows the pointer until the button comes up
    if (g_drag.on) {
        if (button == 0 && !down) {
            g_drag.on = false;
            return 1;
        }
        if (button < 0 && !wheel) {
            const float f = (pu - g_drag.x0) / std::max(1e-6f, g_drag.x1 - g_drag.x0);
            mods_menu_pointer_slider(g_drag.tab, g_drag.line, f);
            return 1;
        }
    }
    if (wheel || !found)
        return 1;
    const bool row = found->kind == HIT_ROW || found->kind == HIT_DECREASE || found->kind == HIT_INCREASE ||
                     found->kind == HIT_SLIDER || found->kind == HIT_ENTRY || found->kind == HIT_ENTRY_APPLY;
    if (button < 0) { // motion: pointing at a row chooses it (and does not scroll the list to it)
        if (row) {
            {
                std::lock_guard lock(g_mu);
                g_hover_sel = found->index;
            }
            mods_menu_pointer_line(tab, found->index, MENU_POINT_HOVER);
        }
        return 1;
    }
    if (!down || button != 0)
        return 1; // the press acts; its release and the other buttons do nothing
    if (found->kind == HIT_SLIDER) {
        // the track's ends: the hit is padded by the knob's radius on both sides
        const float pad = (found->x1 - found->x0) * 0.0f;
        g_drag.on = true;
        g_drag.tab = tab;
        g_drag.line = found->index;
        g_drag.x0 = found->x0 + pad;
        g_drag.x1 = found->x1 - pad;
        mods_menu_pointer_slider(tab, found->index, (pu - g_drag.x0) / std::max(1e-6f, g_drag.x1 - g_drag.x0));
        return 1;
    }
    switch (found->kind) {
    case HIT_TAB:
        mods_menu_set_tab(int(found->index));
        break;
    case HIT_CLOSE:
        mods_menu_close();
        break;
    case HIT_DECREASE:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_DECREASE);
        break;
    case HIT_INCREASE:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_INCREASE);
        break;
    case HIT_ROW:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_ACTIVATE);
        break;
    case HIT_ACTION:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_PRESS);
        break;
    case HIT_ENTRY:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_EDIT);
        break;
    case HIT_ENTRY_APPLY:
        mods_menu_pointer_line(tab, found->index, MENU_POINT_APPLY_ROW);
        break;
    case HIT_SLIDER: // handled above
        break;
    }
    return 1;
}
