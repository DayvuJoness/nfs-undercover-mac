// settings_menu.cpp - see settings_menu.h. Test72 (NFSMW graphics/mods plan, Revision 3, slice 1).
#include "settings_menu.h"
#include "controls_settings.h"
#include "display_settings.h"
#include "game_config.h"
#include "mods_internal.h"
#include "menu_lang.h"
#include "../runtime/mods_seam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <ctime>

#ifndef RECOMP_SETTINGS_PAGE_TABS
#define RECOMP_SETTINGS_PAGE_TABS 0
#endif

extern "C" __attribute__((weak)) int host_pad_kind(char *name, int cap);

namespace {

enum Source : uint8_t {
    SRC_HEADER,
    SRC_DISPLAY,
    SRC_CONTROLS,
    SRC_SETTING,
    SRC_MENU,
    SRC_APPLY,
    SRC_DISCARD,
    SRC_RESTORE_LOOK, // Test85: Restore appearance defaults (group graphics.appearance)
    SRC_RESTORE_GFX,  // Test85: Restore graphics defaults (image quality + appearance, not display/resolution)
    SRC_RESTORE_GROUP, // Test85: a Mods page section's Restore defaults (index: kModGroups entry)
    SRC_VSYNC,         // 2026-11-17: vertical sync on/off, after the frame limit (host.display/vsync)
    SRC_SAVE,          // 2026-11-20: Saves page: delete one save folder (title: its name; empty = none found)
    SRC_PAD,           // 2026-11-22: Controls page: the connected controller, its picture and buttons
    SRC_DEFAULTS,      // 2026-11-24: the page's "Defaults" button (drafts every default of the page; Save commits)
    SRC_CLOSE,         // 2026-11-24: the "Close" button (in the floating bar with Defaults, Save and Discard)
};
struct Item {
    Source src;
    uint32_t index = 0; // display/controls row, settings entry, menu entry
    std::string title;  // headers
};

bool g_open = false;
int g_tab = 0;
size_t g_cursor = 0; // index into the current tab's items
// The Graphics draft: "d:<display row>" or "s:<mod id>/<key>" -> the value Apply will commit.
std::map<std::string, int64_t> g_draft;
// the entry field being typed into (its draft key), empty when none
std::string g_typing_key;
std::string g_status;
// 2026-11-20: restart-only settings' values as this run first saw them ("restart needed" when changed).
std::map<std::string, int64_t> g_start_values;
// The keyboard and pad reach the menu on the input thread; the presenter snapshots it on the thread
// that seals a frame. One lock, recursive because a mod action may open or close the page.
std::recursive_mutex g_mu;
bool g_applying = false; // Apply was chosen off the main thread and waits for its next frame
std::mutex g_main_mu;    // the calls waiting for the main thread
std::vector<std::function<void()>> g_main_calls;
std::atomic<bool> g_main_waiting{false};
#ifdef POPM_TESTING
bool g_force_enabled = false;
#endif
const char *const kTabs[] = {"Graphics", "Mods", "Controls", "Saves"};
// 2026-11-20: the Saves page (save folders under RECOMP_SAVES_DIR, set by the launcher) shows only when
// the game has a saves folder; without one there are the three pages as before.
const char *saves_dir() {
    const char *d = getenv("RECOMP_SAVES_DIR");
    return d && *d ? d : nullptr;
}
int tab_count() {
    return saves_dir() ? 4 : 3;
}
// 2026-11-24: settings belong to a save (settings.cpp); until the game has opened one, F10 has only the Saves page.
extern "C" __attribute__((weak)) const char *mods_settings_active_save(void) {
    return "x";
}
extern "C" __attribute__((weak)) int mods_settings_entry_per_save(uint32_t) {
    return 0;
}
bool saves_only() {
    return saves_dir() && !*mods_settings_active_save();
}
int allowed_tab(int tab) {
    return saves_only() ? 3 : tab;
}
#define kTabCount tab_count()
// The save whose delete was pressed once: the next press on the same line deletes it.
std::string g_save_armed;
std::vector<std::string> save_names() {
    std::vector<std::string> out;
    const char *dir = saves_dir();
    if (!dir)
        return out;
    if (DIR *d = opendir(dir)) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            std::string full = std::string(dir) + "/" + e->d_name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct Setting {
    uint32_t entry = 0, owner = 0;
    const char *mod_id = "", *key = "", *label = "", *group = "", *help = "", *apply = "", *choices = "",
               *unit = "";
    int32_t kind = 0;
    int64_t value = 0, mn = 0, mx = 0, step = 1;
};
bool setting(uint32_t i, Setting *s) {
    s->entry = i;
    if (!mods_settings_entry(i, &s->owner, &s->mod_id, &s->key, &s->label, &s->kind, &s->value, &s->mn,
                             &s->mx))
        return false;
    mods_settings_presentation(i, &s->step, &s->unit);
    mods_settings_meta(i, &s->group, &s->help, &s->apply, &s->choices);
    for (const char **p : {&s->mod_id, &s->key, &s->label, &s->group, &s->help, &s->apply, &s->choices, &s->unit})
        if (!*p)
            *p = "";
    if (s->step < 1)
        s->step = 1;
    return true;
}
std::string draft_key(const Setting &s) {
    return std::string("s:") + s.mod_id + "/" + s.key;
}
std::string draft_key(int display_row) {
    return "d:" + std::to_string(display_row);
}
bool drafted(const Setting &s) {
    return !strcmp(s.apply, "renderer");
}
bool drafted(int display_row) {
    return display_row == DISPLAY_FILTERING;
}
// Test85: the Graphics page's Appearance section (manifest group "graphics.appearance").
bool appearance_setting(const Setting &s) {
    return !strcmp(s.group, "graphics.appearance");
}
// What "Restore graphics defaults" covers: World filtering, and every Graphics setting that applies now or at
// Apply. Not the display rows (window, frame limit, overlay) and not settings that need a restart (resolution,
// widescreen): those are the player's setup, not graphics quality.
bool restorable(const Setting &s) {
    return !strcmp(s.apply, "renderer") || !strcmp(s.apply, "live");
}
int tab_of(const Setting &s) {
    if (!strncmp(s.group, "graphics", 8))
        return 0;
    if (!strncmp(s.group, "controls", 8)) // "controls" and its subcategories ("controls.pad"...)
        return 2;
    return 1;
}

// 2026-11-24 (the person: "sort F10 by categories and subcategories"): every page is categories (large headers) made
// of subcategories (small headers over a card of rows), in this order. A setting's manifest group names its
// subcategory; "#display" is the kit's window and frame-rate rows, "#detail" begins with World filtering. A page's
// catch-all ("graphics", "" and "controls") takes the settings whose group no row here names.
struct Sub {
    int tab;
    const char *group, *category, *sub;
};
const Sub kSubs[] = {
    {0, "#display", "Screen", "Window and frame rate"},
    {0, "graphics.resolution", "Screen", "Resolution and sharpness"},
    {0, "graphics.layout", "Screen", "Screen layout"},
    {0, "#detail", "Image", "Detail and distance"},
    {0, "graphics.light", "Image", "Light and colour"},
    {0, "graphics.effects", "Image", "Camera effects"},
    {0, "graphics.hdr", "Image", "HDR"},
    {0, "graphics.appearance", "Image", "Appearance"},
    {0, "graphics", "Image", "Other"},
    {1, "visual", "Game mods", "Graphics"},
    {1, "gameplay", "Game mods", "Races and police"},
    {1, "cars", "Game mods", "Cars"},
    {1, "extraoptions.game", "Extra Options", "Game and money"},
    {1, "extraoptions.police", "Extra Options", "Police and heat"},
    {1, "extraoptions.driving", "Extra Options", "Driving, camera and keys"},
    {1, "extraoptions", "Extra Options", "Other"},
    {1, "career", "Game", "Start-up and menus"},
    {1, "feedback", "Controller feedback", ""},
    {1, "unlocks", "Unlocks", ""},
    {1, "races", "Races and pursuits", ""},
    {1, "driving", "Driving", ""},
    {1, "world", "World and weather", ""},
    {1, "cheats", "Cheats", ""},
    {1, "audio", "Audio", ""},
    {1, "fixes", "Verified fixes and compatibility", ""},
    {1, "", "Other", ""},
    {1, "diagnostics", "Diagnostics", ""},
    {2, "controls.pad", "Controller", "Buttons and vibration"},
    {2, "controls.trackpad", "Trackpad", "Vibration"},
    {2, "controls", "Controller", "Other"},
};
// True when a row of kSubs names this setting's group on its page (else it goes to the page's catch-all).
bool named_group(const Setting &s) {
    const int t = tab_of(s);
    for (const Sub &k : kSubs)
        if (k.tab == t && *k.group && *k.group != '#' && !strcmp(k.group, s.group) &&
            strcmp(k.group, t == 0 ? "graphics" : t == 2 ? "controls" : ""))
            return true;
    if (t == 0 && !strcmp(s.group, "graphics.display"))
        return true;
    if (t == 0 && !strcmp(s.group, "graphics.detail"))
        return true;
    return false;
}

std::vector<Item> build(int tab) {
    std::vector<Item> v;
    auto header = [&](const char *t, uint32_t level = 0) { v.push_back({SRC_HEADER, level, t}); };
    const uint32_t count = mods_settings_entry_count();
    auto settings_in = [&](std::vector<Item> &out, auto pred) {
        for (uint32_t i = 0; i < count; ++i) {
            Setting s;
            if (setting(i, &s) && s.owner != MODS_OWNER_RUNTIME && pred(s))
                out.push_back({SRC_SETTING, i, {}});
        }
    };
    if (tab <= 2) {
        if (tab == 2) {
            v.push_back({SRC_PAD, 0, {}});
            if (mods_settings_row_listed(DISPLAY_CONTROLS_BIT)) {
                header("On-screen controls");
                for (int r = 0; r < CONTROLS_ROW_COUNT; ++r)
                    v.push_back({SRC_CONTROLS, uint32_t(r), {}});
            }
        }
        const char *shown = nullptr; // the category whose header is up
        for (const Sub &k : kSubs) {
            if (k.tab != tab)
                continue;
            std::vector<Item> rows;
            if (!strcmp(k.group, "#display")) {
                for (int r : {DISPLAY_WINDOW, DISPLAY_FPS, DISPLAY_OVERLAY})
                    if (mods_display_row_applies(DisplayRow(r))) {
                        rows.push_back({SRC_DISPLAY, uint32_t(r), {}});
                        if (r == DISPLAY_FPS)
                            rows.push_back({SRC_VSYNC, 0, {}});
                    }
                settings_in(rows, [](const Setting &s) { return !strcmp(s.group, "graphics.display"); });
            } else if (!strcmp(k.group, "#detail")) {
                if (mods_display_row_applies(DISPLAY_FILTERING))
                    rows.push_back({SRC_DISPLAY, uint32_t(DISPLAY_FILTERING), {}});
                settings_in(rows, [](const Setting &s) { return !strcmp(s.group, "graphics.detail"); });
            } else {
                const bool catch_all = !strcmp(k.group, tab == 0 ? "graphics" : tab == 2 ? "controls" : "");
                settings_in(rows, [&](const Setting &s) {
                    return tab_of(s) == tab && (catch_all ? !named_group(s) : !strcmp(s.group, k.group));
                });
            }
            if (rows.empty())
                continue;
            if (!shown || strcmp(shown, k.category)) {
                header(k.category, 0);
                shown = k.category;
            }
            if (*k.sub)
                header(k.sub, 1);
            v.insert(v.end(), rows.begin(), rows.end());
        }
        if (tab == 1 && mods_menu_entry_count()) {
            header("Mod actions");
            for (uint32_t i = 0; i < mods_menu_entry_count(); ++i)
                v.push_back({SRC_MENU, i, {}});
        }
    } else {
        if (saves_only())
            header("Load a save to see its settings");
        header("Save files");
        const std::vector<std::string> names = save_names();
        for (const std::string &n : names)
            v.push_back({SRC_SAVE, 0, n});
        if (names.empty())
            v.push_back({SRC_SAVE, 0, {}});
    }
    // The page's buttons (drawn in the floating bar, not in the list): Defaults, Save, Close, and Discard while
    // something waits for Save. Discard comes last so the others keep their places when it goes.
    if (tab <= 2) {
        v.push_back({SRC_DEFAULTS, 0, {}});
        v.push_back({SRC_APPLY, 0, {}});
    }
    v.push_back({SRC_CLOSE, 0, {}});
    if (tab <= 2 && !g_draft.empty())
        v.push_back({SRC_DISCARD, 0, {}});
    return v;
}
bool selectable(const Item &it) {
    return it.src != SRC_HEADER && it.src != SRC_PAD;
}

const char *const kFiltering[] = {"Game's own", "Trilinear", "4x anisotropic", "8x anisotropic",
                                  "16x anisotropic"};
int display_draft_or_value(int row) {
    auto it = g_draft.find(draft_key(row));
    return it != g_draft.end() ? int(it->second) : mods_display_value(DisplayRow(row));
}
int64_t setting_draft_or_value(const Setting &s) {
    auto it = g_draft.find(draft_key(s));
    return it != g_draft.end() ? it->second : s.value;
}
std::string choice(const char *choices, int64_t index) {
    const char *p = choices;
    for (int64_t i = 0; *p && i < index; ++i) {
        p = strchr(p, '|');
        if (!p)
            return "";
        ++p;
    }
    const char *end = strchr(p, '|');
    return end ? std::string(p, end) : std::string(p);
}
std::string amount(int64_t value, const char *unit) {
    char digits[32];
    const uint64_t m = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
    snprintf(digits, sizeof digits, "%llu", (unsigned long long)m);
    std::string grouped;
    const size_t n = strlen(digits);
    for (size_t i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0)
            grouped += ',';
        grouped += digits[i];
    }
    return std::string(value < 0 ? "-" : "") + unit + grouped;
}
std::string setting_value_text(const Setting &s, int64_t v) {
    if (s.kind == POP_SETTING_BOOL)
        return v ? "On" : "Off";
    if (*s.choices)
        return choice(s.choices, v - s.mn);
    if (*s.unit)
        return amount(v, s.unit);
    return std::to_string(v);
}
// What the mark under a row says about when a change takes effect (2026-11-24: short, it sits under the label).
const char *timing_text(const char *apply) {
    if (!strcmp(apply, "live"))
        return "Applies immediately";
    if (!strcmp(apply, "renderer"))
        return "Applies on Save";
    if (!strcmp(apply, "next_screen"))
        return "From the next load";
    if (!strcmp(apply, "next_event"))
        return "From the next race";
    if (!strcmp(apply, "new_career"))
        return "New careers only";
    if (!strcmp(apply, "restart"))
        return "Needs a restart";
    if (!strcmp(apply, "game_video"))
        return "After the game's video setting";
    return "";
}
int8_t timing_kind(const char *apply) {
    return !strcmp(apply, "live") ? 1 : !strcmp(apply, "restart") ? 2 : !strcmp(apply, "renderer") ? 3 : *apply ? 4 : 0;
}
// 2026-11-24: when each value was last changed from the menu and took effect at once (key: the row's draft key,
// "vsync" or "c:<controls row>"), for the row's "Saved" mark for a couple of seconds after.
std::map<std::string, double> g_changed_at;
double menu_seconds();
void note_changed(const std::string &key) {
    g_changed_at[key] = menu_seconds();
}
// The row's state mark: waiting for a restart, not saved yet (a draft), or saved just now.
void mark_state(MenuLine &l, const std::string &key, bool waiting_restart) {
    if (waiting_restart) {
        l.state_kind = 2, l.state_text = "Waiting for restart";
        return;
    }
    if (l.pending) {
        l.state_kind = 3, l.state_text = "Not saved";
        return;
    }
    // "Saved": in over 0.22 s, held, out over 0.4 s, gone 2.6 s after the change (eased both ways)
    auto it = g_changed_at.find(key);
    const double t = it != g_changed_at.end() ? menu_seconds() - it->second : 1e9;
    if (t >= 0 && t < 2.6) {
        auto ease = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); };
        l.state_kind = 1, l.state_text = "Saved";
        l.state_alpha = float(ease(t / 0.22) * ease((2.6 - t) / 0.4));
    }
}
// Display rows read "Label: value"; controls rows pad the label to a column ("%-22s %s").
void split_line(const std::string &line, std::string *label, std::string *value) {
    size_t c = line.find(": "), skip = 2;
    if (c == std::string::npos) {
        c = line.find("  ");
        skip = 2;
    }
    if (c == std::string::npos) {
        *label = line;
        value->clear();
        return;
    }
    *label = line.substr(0, c);
    size_t v = c + skip;
    while (v < line.size() && line[v] == ' ')
        ++v;
    *value = line.substr(v);
}

MenuLine line_for(const Item &it) {
    MenuLine l;
    switch (it.src) {
    case SRC_HEADER:
        l.kind = MenuLine::HEADER;
        l.label = it.title;
        l.enabled = false;
        break;
    case SRC_DISPLAY: {
        const int row = int(it.index);
        l.adjustable = true;
        if (row == DISPLAY_FILTERING) {
            const int v = std::clamp(display_draft_or_value(row), 0, 4);
            l.label = "World filtering";
            l.value = kFiltering[v];
            l.pending = v != mods_display_value(DISPLAY_FILTERING);
            l.help = "Sharper road, wall and ground textures seen at an angle.\n"
                     "Game's own - as the game asks.\n"
                     "Trilinear - without anisotropic filtering.\n"
                     "4x-16x - the higher, the sharper.\n"
                     "Text, the HUD and movies are not affected.";
            l.timing = timing_text("renderer");
            l.timing_kind = 3;
            l.scope = saves_dir() ? 1 : 0;
            mark_state(l, draft_key(row), false);
        } else {
            split_line(mods_display_line(DisplayRow(row)), &l.label, &l.value);
            if (row == DISPLAY_WINDOW)
                l.label = "Display mode"; // the section above is "Display" too
            l.timing = timing_text("live");
            l.timing_kind = 1;
            l.scope = saves_dir() ? 1 : 0;
            mark_state(l, draft_key(row), false);
            if (row == DISPLAY_WINDOW)
                l.help = "How the game is shown:\n"
                         "Windowed - in a window.\n"
                         "Borderless - the whole screen, quick to switch to other apps.\n"
                         "Full screen - the real full-screen mode.";
            else if (row == DISPLAY_FPS)
                l.help = "The most frames a second the game draws.\n"
                         "Screen - your display's rate (120 on a MacBook Pro, 60 on other Macs).\n"
                         "40 and 60 - less heat and power.\n"
                         "Maximum - as many as it can (up to 960).\n"
                         "Original - the game sets the pace.";
            else if (row == DISPLAY_OVERLAY)
                l.help = "Frame counters or a frame-time graph in a corner.\nF11 switches it too.";
        }
        break;
    }
    case SRC_VSYNC:
        l.label = "Vertical sync";
        l.scope = saves_dir() ? 1 : 0;
        l.value = mods_display_vsync_setting() ? "On" : "Off";
        l.adjustable = true;
        l.timing = timing_text("live");
        l.timing_kind = 1;
        mark_state(l, "vsync", false);
        l.help = "On - frames come evenly, one per screen refresh, none thrown away.\n"
                 "Off - a little less delay, less even.\n"
                 "No effect with the frame limit \"original\".";
        break;
    case SRC_CONTROLS:
        split_line(mods_controls_line(ControlsRow(it.index)), &l.label, &l.value);
        l.scope = saves_dir() ? 1 : 0;
        l.adjustable = it.index != CONTROLS_EDIT_ROW;
        l.timing = timing_text("live");
        l.timing_kind = 1;
        mark_state(l, "c:" + std::to_string(it.index), false);
        break;
    case SRC_SETTING: {
        Setting s;
        if (!setting(it.index, &s))
            break;
        const int64_t v = setting_draft_or_value(s);
        l.label = s.label;
        l.value = setting_value_text(s, v);
        if (s.kind == POP_SETTING_BOOL)
            l.switch_on = v ? 1 : 0; // a choice named "Off" (Off|Light|Strong) keeps its arrows
        l.pending = v != s.value;
        l.help = s.help;
        l.timing = timing_text(s.apply);
        l.timing_kind = timing_kind(s.apply);
        l.scope = saves_dir() ? (mods_settings_entry_per_save(it.index) ? 2 : 1) : 0;
        l.adjustable = true;
        // An amount (a setting with a unit, e.g. money; the person 2026-11-22: "a field with a caret and an Apply
        // button beside it, not a slider"): typed into a field.
        if (s.kind != POP_SETTING_BOOL && !*s.choices && *s.unit) {
            l.entry = true;
            l.typing = g_typing_key == draft_key(s);
            l.pending = g_draft.count(draft_key(s)) && g_draft[draft_key(s)] != s.value;
        }
        // A number with many steps and no named choices: a slider.
        else if (s.kind != POP_SETTING_BOOL && !*s.choices && s.mx > s.mn && (s.mx - s.mn) / s.step >= 8)
            l.slider = float(double(v - s.mn) / double(s.mx - s.mn));
        // What a restart-only setting was when this run started: a change shows "restart needed".
        if (!strcmp(s.apply, "restart")) {
            const std::string k = draft_key(s);
            auto at = g_start_values.find(k);
            if (at == g_start_values.end())
                at = g_start_values.emplace(k, s.value).first;
            l.restart = s.value != at->second;
        }
        mark_state(l, draft_key(s), l.restart);
        break;
    }
    case SRC_MENU: {
        uint32_t owner = 0;
        const char *path = nullptr, *label = nullptr;
        mods_menu_entry(it.index, &owner, &path, &label);
        l.label = label ? label : "";
        l.value = ">";
        break;
    }
    case SRC_PAD: {
        // The host (gamepad_sdl.cpp) says what is connected; weak so other hosts and tests have none.
        char name[96] = "";
        const int kind = host_pad_kind ? host_pad_kind(name, sizeof name) : 0;
        l.kind = MenuLine::HEADER;
        l.pad_kind = (int8_t)kind;
        l.label = kind ? mods_menu_tr("Controller connected: ") + std::string(name) : mods_menu_tr("No controller connected");
        break;
    }
    case SRC_SAVE:
        l.kind = MenuLine::ACTION;
        l.list_only = true;
        if (it.title.empty()) {
            l.label = "No save files";
            l.enabled = false;
            break;
        }
        l.label = (g_save_armed == it.title ? "Press again to delete \"" : "Delete save \"") + it.title + "\"";
        l.help = "Moves this save to the Trash (Finder can put it back).\n"
                 "Don't delete the save you are playing: the game writes it again.";
        break;
    case SRC_APPLY:
        l.kind = MenuLine::ACTION;
        l.footer = l.primary = true;
        l.label = "Save";
        l.enabled = !g_draft.empty();
        l.help = "Saves the changes marked \"Not saved\".\nEverything else is saved the moment you change it.";
        break;
    case SRC_DISCARD:
        l.kind = MenuLine::ACTION;
        l.footer = true;
        l.label = "Discard";
        l.enabled = !g_draft.empty();
        l.help = "Forgets the changes marked \"Not saved\".";
        break;
    case SRC_DEFAULTS:
        l.kind = MenuLine::ACTION;
        l.footer = true;
        l.label = "Defaults";
        l.help = "Marks every setting of this page at its default value.\nSave uses them, Discard forgets them.";
        break;
    case SRC_CLOSE:
        l.kind = MenuLine::ACTION;
        l.footer = true;
        l.label = "Close";
        l.help = "Closes the menu (Esc or F10 too).\nChanges not saved yet are forgotten.";
        break;
    case SRC_RESTORE_LOOK:
        l.kind = MenuLine::ACTION;
        l.label = "Restore appearance defaults";
        l.help = "Marks the Appearance section's default values (the Definitive look) as changes; Apply commits them. "
                 "Quality, anti-aliasing, filtering, resolution and the frame limit stay as they are.";
        break;
    case SRC_RESTORE_GROUP:
        break;
    case SRC_RESTORE_GFX:
        l.kind = MenuLine::ACTION;
        l.label = "Restore graphics defaults";
        l.help = "Marks the default values of World filtering, Quality, anti-aliasing and the Appearance section as "
                 "changes; Apply commits them. The window mode, resolution and frame limit stay as they are.";
        break;
    }
    return l;
}

void fix_cursor(const std::vector<Item> &v, int dir) {
    if (v.empty()) {
        g_cursor = 0;
        return;
    }
    if (g_cursor >= v.size())
        g_cursor = v.size() - 1;
    for (size_t n = 0; n < v.size() && !selectable(v[g_cursor]); ++n)
        g_cursor = dir >= 0 ? (g_cursor + 1) % v.size() : (g_cursor + v.size() - 1) % v.size();
}

int64_t stepped(int64_t value, int64_t mn, int64_t mx, int64_t step, int delta, bool wrap) {
    if (delta > 0)
        return value >= mx ? (wrap ? mn : mx) : (uint64_t(mx) - uint64_t(value) <= uint64_t(step) ? mx : value + step);
    return value <= mn ? (wrap ? mx : mn) : (uint64_t(value) - uint64_t(mn) <= uint64_t(step) ? mn : value - step);
}

void set_draft(const std::string &key, int64_t value, int64_t committed) {
    if (value == committed)
        g_draft.erase(key);
    else
        g_draft[key] = value;
}

void adjust(const Item &it, int delta, bool from_enter) {
    switch (it.src) {
    case SRC_DISPLAY: {
        const int row = int(it.index);
        if (drafted(row)) {
            const int v = int(stepped(display_draft_or_value(row), 0, 4, 1, delta, from_enter));
            set_draft(draft_key(row), v, mods_display_value(DisplayRow(row)));
        } else {
            mods_menu_main_thread_call([row, delta] { mods_display_nudge(DisplayRow(row), delta); });
            note_changed(draft_key(row));
        }
        break;
    }
    case SRC_VSYNC: {
        const int on = !mods_display_vsync_setting();
        mods_menu_main_thread_call([on] { mods_display_set_vsync(on); });
        note_changed("vsync");
        break;
    }
    case SRC_CONTROLS:
        if (it.index == CONTROLS_EDIT_ROW) {
            if (from_enter)
                mods_controls_nudge(ControlsRow(it.index), +1);
        } else {
            mods_controls_nudge(ControlsRow(it.index), delta);
            note_changed("c:" + std::to_string(it.index));
        }
        break;
    case SRC_SETTING: {
        Setting s;
        if (!setting(it.index, &s))
            break;
        const int64_t cur = drafted(s) ? setting_draft_or_value(s) : s.value;
        const bool wrap = from_enter && *s.choices;
        int64_t want = s.kind == POP_SETTING_BOOL ? !cur : stepped(cur, s.mn, s.mx, s.step, delta, wrap);
        // A retired choice keeps its value slot (saved profiles still hold it) with an empty label; stepping passes
        // over it (for example a look a game has since removed).
        for (int64_t guard = s.mx - s.mn; s.kind != POP_SETTING_BOOL && *s.choices && guard > 0 && want != cur &&
                                          choice(s.choices, std::clamp(want, s.mn, s.mx) - s.mn).empty();
             --guard)
            want = stepped(want, s.mn, s.mx, s.step, delta, wrap);
        want = std::clamp(want, s.mn, s.mx);
        if (drafted(s))
            set_draft(draft_key(s), want, s.value);
        else {
            g_draft.erase(draft_key(s)); // a Restore's draft of this live setting gives way to the edit
            if (mods_settings_set(s.owner, s.key, want) == POP_OK && want != s.value)
                note_changed(draft_key(s));
        }
        break;
    }
    default:
        break;
    }
}

#ifdef POPM_TESTING
double g_test_seconds = -1;   // mods_menu_clock_for_test: a set clock for the watchdog tests
int g_test_pause_setting = -1;
#endif
double menu_seconds() {
#ifdef POPM_TESTING
    if (g_test_seconds >= 0)
        return g_test_seconds;
#endif
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// NFSMW 3.0.4 (D-202 item 4, runtime/guest_pause.h): when the current pause began (0: not paused). The Apply
// watchdog below counts only time the game ran: it does not fire during a pause, and a pause moves the Apply's
// time forward by the paused part, so 20 s of running still prove it.
double g_paused_since = 0;
// Test85: last-known-good. When the Apply that recorded them happened (0: none pending).
double g_pending_since = 0;
void record_previous() {
    std::vector<std::pair<std::string, int64_t>> previous;
    const uint32_t count = mods_settings_entry_count();
    for (const auto &kv : g_draft) {
        const std::string &key = kv.first;
        if (key.rfind("d:", 0) == 0) {
            if (std::stoi(key.substr(2)) == DISPLAY_FILTERING)
                previous.push_back({std::string("host.display/") + RECOMP_SETTINGS_FILTERING_KEY,
                                    mods_display_value(DISPLAY_FILTERING)});
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            Setting s;
            if (setting(i, &s) && draft_key(s) == key) {
                previous.push_back({std::string(s.mod_id) + "/" + s.key, s.value});
                break;
            }
        }
    }
    mods_cpp_settings_mark_pending(previous);
    g_pending_since = previous.empty() ? 0 : menu_seconds();
}

void apply_draft() {
    if (g_draft.empty())
        return;
    record_previous();
    int applied = 0;
    const uint32_t count = mods_settings_entry_count();
    for (const auto &[key, value] : g_draft) {
        if (key.rfind("d:", 0) == 0) {
            const PopModStatus st = mods_display_set(DisplayRow(std::stoi(key.substr(2))), int(value));
            if (st == POP_OK)
                ++applied, note_changed(key);
            else
                printf("mods: settings menu could not apply %s=%lld (status %d)\n", key.c_str(), (long long)value,
                       int(st));
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            Setting s;
            if (setting(i, &s) && draft_key(s) == key && mods_settings_set(s.owner, s.key, value) == POP_OK) {
                ++applied;
                note_changed(key);
                break;
            }
        }
    }
    const size_t total = g_draft.size();
    g_draft.clear();
    char line[96];
    if (applied == int(total))
        snprintf(line, sizeof line, "Saved %d change%s.", applied, applied == 1 ? "" : "s");
    else
        snprintf(line, sizeof line, "Saved %d of %zu changes.", applied, total);
    g_status = line;
    printf("mods: settings menu applied %d of %zu graphics changes\n", applied, total);
    fflush(stdout);
}

void request_apply() {
    if (g_draft.empty())
        return;
    if (mods_host_on_main_thread()) {
        apply_draft();
        return;
    }
    g_applying = true;
    g_status = "Saving...";
    mods_menu_main_thread_call([] {
        std::lock_guard lock(g_mu);
        g_applying = false;
        apply_draft();
    });
}

// Test85: draft the defaults (Apply commits them, Discard forgets them).
void restore_defaults(bool appearance_only) {
    int n = 0;
    if (!appearance_only && mods_display_row_applies(DISPLAY_FILTERING)) {
        set_draft(draft_key(DISPLAY_FILTERING), 0, mods_display_value(DISPLAY_FILTERING));
        ++n;
    }
    const uint32_t count = mods_settings_entry_count();
    for (uint32_t i = 0; i < count; ++i) {
        Setting s;
        int64_t def = 0;
        if (!setting(i, &s) || s.owner == MODS_OWNER_RUNTIME || tab_of(s) != 0 || !mods_settings_default(i, &def))
            continue;
        if (appearance_only ? !appearance_setting(s) : !restorable(s))
            continue;
        set_draft(draft_key(s), std::clamp(def, s.mn, s.mx), s.value);
        ++n;
    }
    g_status = g_draft.empty() ? "Already at the defaults."
                               : std::string(appearance_only ? "Appearance" : "Graphics") +
                                     " defaults marked; choose Apply to use them.";
    printf("mods: settings menu restore %s defaults: %d settings, %zu changes drafted\n",
           appearance_only ? "appearance" : "graphics", n, g_draft.size());
    fflush(stdout);
}

// 2026-11-24: the page's Defaults button - every setting of the page (Graphics: World filtering too; not the window
// mode or frame limit) drafted at its default; Save commits, Discard forgets.
void restore_tab(int tab) {
    if (tab == 0 && mods_display_row_applies(DISPLAY_FILTERING))
        set_draft(draft_key(DISPLAY_FILTERING), 0, mods_display_value(DISPLAY_FILTERING));
    const uint32_t count = mods_settings_entry_count();
    int n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Setting s;
        int64_t def = 0;
        if (!setting(i, &s) || s.owner == MODS_OWNER_RUNTIME || tab_of(s) != tab || !mods_settings_default(i, &def))
            continue;
        set_draft(draft_key(s), std::clamp(def, s.mn, s.mx), s.value);
        ++n;
    }
    g_status = g_draft.empty() ? "Already at the defaults." : "Defaults marked: Save uses them, Discard forgets them.";
    printf("mods: settings menu defaults of page %d: %d settings, %zu changes drafted\n", tab, n, g_draft.size());
    fflush(stdout);
}

} // namespace
// The macOS host (menu_glass_mac.mm) defines the real one, which wins over this weak default; -2 = no host.
extern "C" __attribute__((weak)) int host_trash_path(const char *) {
    return -2;
}
namespace {

void delete_save(const std::string &name) {
    if (name.empty())
        return;
    if (g_save_armed != name) {
        g_save_armed = name;
        g_status = "Press again to delete \"" + name + "\".";
        return;
    }
    g_save_armed.clear();
    const std::string path = std::string(saves_dir() ? saves_dir() : "") + "/" + name;
    int rc = host_trash_path(path.c_str());
    if (rc == -2) {
        rc = -1;
        const char *home = getenv("HOME");
        // No Finder call (tests): a plain move into ~/.Trash, never a delete.
        if (home) {
            std::string dst = std::string(home) + "/.Trash/" + name;
            struct stat st;
            if (stat(dst.c_str(), &st) == 0)
                dst += " " + std::to_string((long long)time(nullptr));
            rc = rename(path.c_str(), dst.c_str());
        }
    }
    g_status = rc == 0 ? "Moved \"" + name + "\" to the Trash." : "Could not move \"" + name + "\" to the Trash.";
    if (rc == 0) { // its own F10 settings go with it (settings.cpp; a new save of that name starts from the defaults)
        remove((std::string(mods_overlay_profile_dir()) + "/SaveSettings/" + name + ".json").c_str());
    }
    printf("mods: settings menu: save \"%s\" %s\n", name.c_str(), rc == 0 ? "moved to the Trash" : "NOT moved (error)");
    fflush(stdout);
}

// Enter, or a click on the line.
bool entry_setting(const Item &it, Setting *s) {
    return it.src == SRC_SETTING && setting(it.index, s) && s->kind != POP_SETTING_BOOL && !*s->choices && *s->unit;
}
// Editing an entry field: the caret after the number as it is now; digits are added to it, Backspace takes one off.
void begin_typing(const Setting &s) {
    const std::string key = draft_key(s);
    if (!g_draft.count(key))
        g_draft[key] = s.value;
    g_typing_key = key;
    g_status = "Type the number, then press Return or Apply.";
}
// The field's own Apply: commits what was typed there.
void apply_typed(const Setting &s) {
    const std::string key = draft_key(s);
    g_typing_key.clear();
    if (!g_draft.count(key) || g_draft[key] == s.value) {
        g_draft.erase(key);
        g_status.clear();
        return;
    }
    g_draft[key] = std::clamp(g_draft[key], s.mn, s.mx);
    request_apply();
}
void activate(const Item &it) {
    if (it.src == SRC_SAVE) {
        delete_save(it.title);
        return;
    }
    g_save_armed.clear();
    if (it.src == SRC_RESTORE_LOOK || it.src == SRC_RESTORE_GFX) {
        restore_defaults(it.src == SRC_RESTORE_LOOK);
        return;
    }
    if (it.src == SRC_DEFAULTS) {
        restore_tab(g_tab);
        return;
    }
    if (it.src == SRC_CLOSE) {
        mods_menu_close();
        return;
    }
    if (it.src == SRC_APPLY) {
        if (g_draft.empty())
            g_status = "Nothing to save: every change here was saved when you made it.";
        request_apply();
    }
    else if (it.src == SRC_DISCARD) {
        g_status = g_draft.empty() ? "" : "Discarded the changes.";
        g_draft.clear();
    } else if (it.src == SRC_MENU)
        mods_menu_activate(it.index);
    else {
        Setting s;
        if (entry_setting(it, &s)) {
            begin_typing(s);
            return;
        }
        adjust(it, +1, true);
    }
}

void show_tab(int tab) {
    g_tab = allowed_tab(std::clamp(tab, 0, kTabCount - 1));
    g_cursor = 0;
    fix_cursor(build(g_tab), +1);
    g_status.clear();
    g_save_armed.clear();
}

} // namespace

bool mods_menu_enabled() {
#ifdef POPM_TESTING
    return g_force_enabled;
#else
    return RECOMP_SETTINGS_PAGE_TABS != 0;
#endif
}

void mods_menu_open(int tab) {
    std::lock_guard lock(g_mu);
    g_open = true;
    g_tab = allowed_tab(std::clamp(tab, 0, kTabCount - 1));
    g_cursor = 0;
    g_status.clear();
    fix_cursor(build(g_tab), +1);
}

void mods_menu_close() {
    std::lock_guard lock(g_mu);
    if (g_applying) {
        g_open = false; // the chosen Apply still runs at the next frame
        return;
    }
    if (!g_draft.empty()) {
        printf("mods: settings menu closed; %zu unapplied graphics change%s discarded\n", g_draft.size(),
               g_draft.size() == 1 ? "" : "s");
        fflush(stdout);
    } else if (g_open) {
        printf("mods: settings menu closed\n");   // Test93: the record that it closed, and when
        fflush(stdout);
    }
    g_draft.clear();
    g_open = false;
}

bool mods_menu_is_open() {
    std::lock_guard lock(g_mu);
    return g_open;
}

extern "C" bool mods_menu_pause_wanted(void) {
    if (!mods_host_on_main_thread() || !mods_menu_is_open())
        return false;
#ifdef POPM_TESTING
    if (g_test_pause_setting >= 0)
        return g_test_pause_setting != 0;
#endif
    return mods_core_setting("pause_in_menu", 0) != 0;
}

extern "C" bool mods_menu_pause_wanted_any_thread(void) {
    if (!mods_menu_is_open())
        return false;
#ifdef POPM_TESTING
    if (g_test_pause_setting >= 0)
        return g_test_pause_setting != 0;
#endif
    return mods_core_setting("pause_in_menu", 0) != 0;
}

extern "C" void mods_menu_note_pause(bool paused) {
    const double now = menu_seconds();
    if (paused) {
        g_paused_since = now > 0 ? now : 1e-9;
        return;
    }
    if (g_pending_since > 0 && g_paused_since > 0)
        g_pending_since += now - std::max(g_pending_since, g_paused_since);
    g_paused_since = 0;
}

int mods_menu_pending_count() {
    std::lock_guard lock(g_mu);
    return int(g_draft.size());
}

// Typing a number (2026-11-22, NFS Undercover's "money" row: "a number typed on the keyboard, and Apply"): on a
// whole-number setting without named choices, the digit keys (top row or keypad) build the value as a draft, Backspace
// takes the last digit off, Return or the footer's Apply applies it, Discard forgets it. The first digit on a row
// starts a fresh number; a digit that would pass the setting's maximum is ignored.
int typed_digit(int dik) {
    if (dik >= 0x02 && dik <= 0x0a)
        return dik - 0x01; // 1..9
    if (dik == 0x0b)
        return 0;
    switch (dik) {
    case 0x4f: return 1;
    case 0x50: return 2;
    case 0x51: return 3;
    case 0x4b: return 4;
    case 0x4c: return 5;
    case 0x4d: return 6;
    case 0x47: return 7;
    case 0x48: return 8;
    case 0x49: return 9;
    case 0x52: return 0;
    default: return -1;
    }
}
bool typed_number(const Item *cur, int dik) {
    if (!cur || cur->src != SRC_SETTING)
        return false;
    Setting s;
    if (!setting(cur->index, &s) || s.kind == POP_SETTING_BOOL || *s.choices)
        return false;
    const std::string key = draft_key(s);
    const int d = typed_digit(dik);
    if (d >= 0) {
        int64_t base = g_typing_key == key && g_draft.count(key) ? g_draft[key] : 0;
        if (base < 0)
            base = 0;
        const int64_t want = base * 10 + d;
        if (want > s.mx)
            return true; // too many digits: ignored
        g_typing_key = key;
        g_draft[key] = want;
        g_status = "Type the number, then press Return or Apply.";
        return true;
    }
    if (dik == 0x0e && g_typing_key == key && g_draft.count(key)) { // BACKSPACE
        g_draft[key] = setting_draft_or_value(s) / 10;
        g_status = "Type the number, then press Return or Apply.";
        return true;
    }
    return false;
}
bool mods_menu_key(int dik) {
    std::lock_guard lock(g_mu);
    if (!g_open)
        return false;
    std::vector<Item> v = build(g_tab);
    fix_cursor(v, +1);
    const Item *cur = g_cursor < v.size() ? &v[g_cursor] : nullptr;
    if (typed_number(cur, dik))
        return true;
    if (dik == 0x1c && cur && cur->src == SRC_SETTING && !g_typing_key.empty()) { // RETURN on a typed number
        Setting s;
        if (setting(cur->index, &s) && draft_key(s) == g_typing_key) {
            g_typing_key.clear();
            const int64_t want = std::clamp(setting_draft_or_value(s), s.mn, s.mx);
            g_draft[draft_key(s)] = want;
            request_apply();
            return true;
        }
    }
    if (dik == 0x1c && cur) { // RETURN on an entry field not being edited: edit it
        Setting s;
        if (entry_setting(*cur, &s) && g_typing_key != draft_key(s)) {
            begin_typing(s);
            return true;
        }
    }
    if (dik == 0xc8 || dik == 0xd0)
        g_typing_key.clear();
    switch (dik) {
    case 0xc8: // UP
        if (!v.empty()) {
            size_t c = g_cursor;
            do
                c = (c + v.size() - 1) % v.size();
            while (c != g_cursor && !selectable(v[c]));
            g_cursor = c;
        }
        if (!g_save_armed.empty())
            g_save_armed.clear(), g_status.clear();
        break;
    case 0xd0: // DOWN
        if (!v.empty()) {
            size_t c = g_cursor;
            do
                c = (c + 1) % v.size();
            while (c != g_cursor && !selectable(v[c]));
            g_cursor = c;
        }
        if (!g_save_armed.empty())
            g_save_armed.clear(), g_status.clear();
        break;
    case 0xcb: // LEFT
    case 0xcd: // RIGHT
        if (cur)
            adjust(*cur, dik == 0xcd ? +1 : -1, false);
        g_status.clear();
        break;
    case 0x1c: // RETURN
        if (cur)
            activate(*cur);
        break;
    case MENU_KEY_TAB_PREV:
    case MENU_KEY_TAB_NEXT:
    case MENU_KEY_TAB:
        show_tab((g_tab + (dik == MENU_KEY_TAB_PREV ? kTabCount - 1 : 1)) % kTabCount);
        break;
    case 0x01: // ESCAPE
        mods_menu_close();
        break;
    default:
        return true; // the menu owns the keyboard while it is open
    }
    return true;
}

bool mods_menu_pointer_slider(int tab, size_t line, float frac) {
    std::lock_guard lock(g_mu);
    if (!g_open || tab != g_tab)
        return false;
    std::vector<Item> v = build(g_tab);
    if (line >= v.size() || v[line].src != SRC_SETTING)
        return false;
    Setting s;
    if (!setting(v[line].index, &s) || s.kind == POP_SETTING_BOOL || s.mx <= s.mn)
        return false;
    g_cursor = line;
    const double f = std::clamp(double(frac), 0.0, 1.0);
    int64_t want = s.mn + int64_t(std::llround(f * double(s.mx - s.mn) / double(s.step))) * s.step;
    want = std::clamp(want, s.mn, s.mx);
    if (drafted(s))
        set_draft(draft_key(s), want, s.value);
    else if (want != s.value) {
        g_draft.erase(draft_key(s));
        if (mods_settings_set(s.owner, s.key, want) == POP_OK)
            note_changed(draft_key(s));
    }
    return true;
}

void mods_menu_set_tab(int tab) {
    std::lock_guard lock(g_mu);
    if (g_open && tab != g_tab)
        show_tab(tab);
}

bool mods_menu_pointer_line(int tab, size_t line, int action) {
    std::lock_guard lock(g_mu);
    if (!g_open || tab != g_tab)
        return false;
    std::vector<Item> v = build(g_tab);
    if (line >= v.size() || !selectable(v[line]))
        return false;
    if (action == MENU_POINT_PRESS) {
        activate(v[line]);
        return true;
    }
    g_cursor = line;
    switch (action) {
    case MENU_POINT_EDIT:
    case MENU_POINT_APPLY_ROW: {
        Setting s;
        if (!entry_setting(v[line], &s))
            break;
        if (action == MENU_POINT_EDIT)
            begin_typing(s);
        else
            apply_typed(s);
        break;
    }
    case MENU_POINT_ACTIVATE:
        activate(v[line]);
        break;
    case MENU_POINT_DECREASE:
    case MENU_POINT_INCREASE:
        if (v[line].src == SRC_APPLY || v[line].src == SRC_DISCARD || v[line].src == SRC_MENU || v[line].src == SRC_SAVE ||
            v[line].src == SRC_RESTORE_LOOK || v[line].src == SRC_RESTORE_GFX || v[line].src == SRC_RESTORE_GROUP)
            activate(v[line]);
        else
            adjust(v[line], action == MENU_POINT_INCREASE ? +1 : -1, false);
        g_status.clear();
        break;
    default:
        break;
    }
    return true;
}

bool mods_menu_snapshot(MenuSnapshot *out) {
    std::lock_guard lock(g_mu);
    if (!out || !g_open)
        return false;
    *out = MenuSnapshot{};
    if (saves_only() && g_tab != 3) {
        g_tab = 3;
        g_cursor = 0;
    }
    out->tab = g_tab;
    for (const char *t : kTabs)
        out->tabs.push_back(mods_menu_tr(t));
    std::vector<Item> v = build(g_tab);
    fix_cursor(v, +1);
    for (size_t i = 0; i < v.size(); ++i) {
        MenuLine l = line_for(v[i]);
        if (l.adjustable && l.switch_on < 0 && v[i].src != SRC_SETTING && (l.value == "On" || l.value == "Off"))
            l.switch_on = l.value == "On" ? 1 : 0;
        // In the game's language (menu_lang.cpp): English unless the game is Russian.
        l.label = mods_menu_tr(l.label);
        l.value = mods_menu_tr(l.value);
        l.help = mods_menu_tr(l.help);
        // 2026-11-24: a hint's lines - a mod's manifest cannot hold a line break, so "|" stands for one there
        for (char &ch : l.help)
            if (ch == '|')
                ch = '\n';
        l.timing = mods_menu_tr(l.timing);
        l.state_text = mods_menu_tr(l.state_text);
        if (l.scope == 2) {
            const char *save = mods_settings_active_save();
            l.scope_text = mods_menu_tr("Save file") + (save && *save ? " \xc2\xab" + std::string(save) + "\xc2\xbb" : "");
        } else if (l.scope == 1) {
            l.scope_text = mods_menu_tr("Shared");
        }
        if (l.kind == MenuLine::HEADER && v[i].src == SRC_HEADER)
            l.level = int8_t(v[i].index);
        l.selected = i == g_cursor;
        if (l.selected) {
            out->help = l.help;
            out->help_title = l.kind == MenuLine::HEADER ? "" : l.label;
        }
        out->lines.push_back(std::move(l));
    }
    if (g_applying) {
        out->status = g_status;
    } else if (!g_draft.empty()) {
        char line[96];
        snprintf(line, sizeof line, "%zu change%s not saved: Save or Discard.", g_draft.size(),
                 g_draft.size() == 1 ? "" : "s");
        out->status = line;
    } else {
        out->status = g_status;
    }
    // Restart-only settings changed in this run: say so until the game is restarted.
    int restart = 0;
    for (uint32_t i = 0; i < mods_settings_entry_count(); ++i) {
        Setting st;
        if (!setting(i, &st) || strcmp(st.apply, "restart"))
            continue;
        auto at = g_start_values.find(draft_key(st));
        restart += at != g_start_values.end() && at->second != st.value;
    }
    if (restart && out->status.empty()) {
        char line[120];
        snprintf(line, sizeof line, "Restart the game to use %d change%s waiting for a restart.", restart,
                 restart == 1 ? "" : "s");
        out->status = line;
    }
    out->status = mods_menu_tr(out->status);
    out->prompts = "\xe2\x86\x91\xe2\x86\x93 choose   \xe2\x86\x90\xe2\x86\x92 change   Tab page   Esc close";
    out->prompts = mods_menu_tr(out->prompts);
    // A content hash: the renderer redraws only when something it would draw changed.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string &s) {
        for (unsigned char c : s)
            h = (h ^ c) * 1099511628211ull;
        h = (h ^ 0xff) * 1099511628211ull;
    };
    mix(std::to_string(out->tab));
    for (const MenuLine &l : out->lines) {
        mix(l.label);
        mix(l.value);
        mix(l.timing);
        mix(std::string(1, char('0' + l.selected + 2 * l.pending + 4 * l.enabled)));
        mix(std::string(1, char('0' + l.entry + 2 * l.typing)));
        mix(std::string(1, char('5' + l.pad_kind)));
        mix(l.scope_text);
        mix(std::string(1, char('A' + l.state_kind + 4 * l.timing_kind)));
        mix(std::string(1, char('0' + int(l.state_alpha * 24.0f + 0.5f)))); // the fade redraws in small steps
        mix(std::string(1, char('a' + l.level + 2 * l.footer + 4 * l.primary)));
    }
    mix(out->help);
    mix(out->help_title);
    mix(out->status);
    out->generation = h;
    return true;
}

void mods_menu_main_thread_call(std::function<void()> fn) {
    if (mods_host_on_main_thread()) {
        fn();
        return;
    }
    std::lock_guard lock(g_main_mu);
    g_main_calls.push_back(std::move(fn));
    g_main_waiting.store(true, std::memory_order_release);
}

void mods_menu_main_thread_drain() {
    // Test85: 20 s of game frames after an Apply prove it; the recorded previous values are dropped.
    if (g_pending_since > 0 && g_paused_since == 0 && menu_seconds() - g_pending_since > 20.0) {
        g_pending_since = 0;
        mods_settings_clear_pending();
        fflush(stdout);
    }
    if (!g_main_waiting.load(std::memory_order_acquire) || !mods_host_on_main_thread())
        return;
    std::vector<std::function<void()>> calls;
    {
        std::lock_guard lock(g_main_mu);
        calls.swap(g_main_calls);
        g_main_waiting.store(false, std::memory_order_relaxed);
    }
    for (auto &fn : calls)
        fn();
    printf("mods: settings menu ran %zu deferred change%s on the main thread\n", calls.size(),
           calls.size() == 1 ? "" : "s");
    fflush(stdout);
}

#ifdef POPM_TESTING
void mods_menu_enable_for_test(bool on) {
    g_force_enabled = on;
}
void mods_menu_clock_for_test(double seconds) {
    g_test_seconds = seconds;
}
void mods_menu_pause_setting_for_test(int on) {
    g_test_pause_setting = on;
}
double mods_menu_pending_since_for_test() {
    return g_pending_since;
}
void mods_menu_reset_for_test() {
    g_paused_since = 0;
    g_test_seconds = -1;
    g_test_pause_setting = -1;
    std::lock_guard lock(g_mu);
    g_open = false;
    g_tab = 0;
    g_cursor = 0;
    g_draft.clear();
    g_status.clear();
    g_applying = false;
}
#endif
