// settings_menu.h - the tabbed settings menu (Test72; NFSMW graphics/mods plan, Revision 3):
// Graphics, Mods and Controls pages over the EXISTING settings owners (display rows, controls
// rows, mod manifest settings). It keeps no state of its own besides the open page, the cursor
// and the Graphics draft: a value lives where it always lived and persists the way it always did.
// Graphics changes whose manifest says `apply = "renderer"` (and the World filtering row) are
// collected in a draft and committed together by Apply; everything else applies the way its owner
// already applies it, with that timing shown. A game opts in with game.toml [settings] page = "tabs".
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct MenuLine {
    enum Kind : uint8_t { HEADER, ROW, ACTION } kind = ROW;
    std::string label, value, help, timing;
    bool selected = false, pending = false, enabled = true, adjustable = false;
    // Test128: an action that stays in the list only (a Mods section's restore): the footer's buttons are the
    // page-wide actions, so a page with many sections does not run its buttons off the panel.
    bool list_only = false;
    // An on/off row (drawn as a switch on glass): 1 on, 0 off, -1 not one. Set before the menu's strings are
    // translated, so a switch does not depend on the words "On"/"Off" (menu_lang.cpp).
    int8_t switch_on = -1;
    // 2026-11-20: a number with a wide range is drawn as a slider: where its value sits, 0..1 (-1: not one).
    float slider = -1.0f;
    // 2026-11-20: a setting that takes effect at the next start whose value changed in this run.
    bool restart = false;
    // 2026-11-22: a number typed into a field (a setting with a unit, e.g. money) with its own Apply button beside
    // it, instead of a slider; `typing` while it is being edited (the field shows a caret).
    bool entry = false, typing = false;
    // 2026-11-22: the Controls page's controller line: 0 none connected, 1 Xbox, 2 PlayStation, 3 another kind
    // (drawn with a picture of the pad and what its buttons do); -1 not that line.
    int8_t pad_kind = -1;
    // 2026-11-24: where the setting lives (the person: mark shared and save-own settings, in their own blocks):
    // 0 not said, 1 shared by every save, 2 this save's own (settings.cpp per-save files).
    int8_t scope = 0;
    // 2026-11-24 redesign (the person: "marks under each row, not beside it"): the small line under a row's label.
    // scope_text "Shared" / "Save «name»"; timing_kind 1 applies immediately, 2 needs a restart, 3 applies on Save,
    // 4 another moment (timing says which); state_kind 1 "Saved" (a couple of seconds after a change that applied at
    // once), 2 "Waiting for restart" (a restart-only value differs from the one this run started with), 3 "Not saved"
    // (waits for Save). The texts are already in the menu's language.
    std::string scope_text, state_text;
    int8_t timing_kind = 0, state_kind = 0;
    // How visible the state mark is, 0..1: "Saved" fades in quickly and fades out quickly (the person, 2026-11-24).
    float state_alpha = 1.0f;
    // A section header's level: 0 a category (large), 1 a subcategory (small, above its card of rows).
    int8_t level = 0;
    // A page button (Defaults, Discard, Save, Close): kept out of the list and drawn in the bar that floats at the
    // bottom of the panel; Up/Down still reach it after the last row. primary: the accent-filled default button.
    bool footer = false, primary = false;
};
struct MenuSnapshot {
    uint64_t generation = 0; // changes whenever anything drawn changes
    int tab = 0;
    std::vector<std::string> tabs;
    std::vector<MenuLine> lines;
    std::string help, status, prompts;
    std::string help_title; // the chosen row's name, over its hint
};

bool mods_menu_enabled(void);
void mods_menu_open(int tab);
// Closing discards an unapplied Graphics draft (the status said so while it was pending).
void mods_menu_close(void);
bool mods_menu_is_open(void);
// A DirectInput key code, or the menu's own codes below. True when consumed.
bool mods_menu_key(int dik);
constexpr int MENU_KEY_TAB_PREV = 0xc9; // Page Up / left shoulder
constexpr int MENU_KEY_TAB_NEXT = 0xd1; // Page Down / right shoulder
constexpr int MENU_KEY_TAB = 0x0f;      // Tab: the next page, wrapping
void mods_menu_set_tab(int tab);
// The pointer, on line `line` of page `tab` as the last snapshot listed it (settings_menu_draw.cpp
// keeps where each line was drawn). False when that page is no longer showing or the line cannot
// be chosen.
// PRESS acts like ACTIVATE but leaves the cursor where it is (the Apply/Discard buttons beside the
// status line, which must not scroll the list away from the row being changed).
enum MenuPointerAction {
    MENU_POINT_HOVER,
    MENU_POINT_ACTIVATE,
    MENU_POINT_DECREASE,
    MENU_POINT_INCREASE,
    MENU_POINT_PRESS,
    MENU_POINT_EDIT,     // an entry field clicked: edit its number (caret at the end)
    MENU_POINT_APPLY_ROW // an entry field's own Apply button
};
bool mods_menu_pointer_line(int tab, size_t line, int action);
// 2026-11-20: a slider row set by the pointer: `frac` 0..1 along its track (rounded to the setting's step).
bool mods_menu_pointer_slider(int tab, size_t line, float frac);
bool mods_menu_snapshot(MenuSnapshot *out);
// The draft as "key=value" lines and the number of pending changes (tests, logs).
int mods_menu_pending_count(void);
// Display changes belong to the thread that booted (mods_display_set refuses any other), but input
// reaches the menu on whichever thread holds the guest baton when it is delivered, sometimes a guest
// worker. Such a change runs now on the main thread, else at the next game frame there (the game's
// frame hook drains it). Test74.
void mods_menu_main_thread_call(std::function<void()> fn);
void mods_menu_main_thread_drain(void);
#ifdef POPM_TESTING
void mods_menu_reset_for_test(void);
void mods_menu_clock_for_test(double seconds);   // the Apply watchdog's clock; -1 = the real one
void mods_menu_pause_setting_for_test(int on);    // pause_in_menu without a core mod; -1 = the real setting
double mods_menu_pending_since_for_test(void);
void mods_menu_enable_for_test(bool on); // off by default, so the older page's tests keep it
#endif
