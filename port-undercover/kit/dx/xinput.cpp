// xinput.cpp - the virtual pad as XInput controller 0.
//
// XInput has no COM objects and no per-device handle: a game addresses users
// by index, and this kit serves exactly one, user 0, built from the same
// merged pad DirectInput's joystick reads (dinput_joystick.cpp) -
// host_pad_state for the polled state, host_pad_next_event for the edges
// XInputGetKeystroke replays.
//
// One shim table answers XINPUT1_3.dll, XINPUT1_4.dll and XINPUT9_1_0.dll:
// a game imports whichever it shipped with, and the entries a given DLL
// never actually exported (say XInputGetAudioDeviceIds from 1_3) are simply
// never registered for it, so the loader's IAT patch for that ordinal/name
// fails exactly as it would against the real DLL.
//
// XInputEnable and the keystroke replay position are per-process state,
// matching the real DLL: one game process, one set of XInput globals, no
// per-device object to hang them from.
#include "com.h"
#include "dx.h"
#include "host_api.h"

#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../platform/os.h"
#include <stdio.h>

#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

// XInput reports plain Win32 error codes in EAX, not HRESULTs.
const uint32_t XI_ERROR_SUCCESS = 0;
const uint32_t XI_ERROR_BAD_ARGUMENTS = 160;
const uint32_t XI_ERROR_DEVICE_NOT_CONNECTED = 1167;
const uint32_t XI_ERROR_EMPTY = 4306;
// XInputGetKeystroke alone accepts this in place of a real user index.
const uint32_t XUSER_INDEX_ANY = 0xFF;

// XINPUT_GAMEPAD.wButtons bits (xinput.h).
const uint32_t XINPUT_GAMEPAD_DPAD_UP = 0x0001;
const uint32_t XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
const uint32_t XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
const uint32_t XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
const uint32_t XINPUT_GAMEPAD_START = 0x0010;
const uint32_t XINPUT_GAMEPAD_BACK = 0x0020;
const uint32_t XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
const uint32_t XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
const uint32_t XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
const uint32_t XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
const uint32_t XINPUT_GAMEPAD_A = 0x1000;
const uint32_t XINPUT_GAMEPAD_B = 0x2000;
const uint32_t XINPUT_GAMEPAD_X = 0x4000;
const uint32_t XINPUT_GAMEPAD_Y = 0x8000;

// XINPUT_GAMEPAD (12 bytes): wButtons, bLeftTrigger, bRightTrigger,
// sThumbLX, sThumbLY, sThumbRX, sThumbRY.
enum {
    GP_OFF_wButtons = 0,
    GP_OFF_bLeftTrigger = 2,
    GP_OFF_bRightTrigger = 3,
    GP_OFF_sThumbLX = 4,
    GP_OFF_sThumbLY = 6,
    GP_OFF_sThumbRX = 8,
    GP_OFF_sThumbRY = 10,
    GP_SIZE = 12,
};
// XINPUT_STATE: dwPacketNumber, then a XINPUT_GAMEPAD.
enum { STATE_OFF_dwPacketNumber = 0, STATE_OFF_Gamepad = 4, STATE_SIZE = 16 };
// XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed.
enum { VIB_OFF_wLeftMotorSpeed = 0, VIB_OFF_wRightMotorSpeed = 2, VIB_SIZE = 4 };
// XINPUT_CAPABILITIES: Type, SubType, Flags, a XINPUT_GAMEPAD, a
// XINPUT_VIBRATION.
enum {
    CAPS_OFF_Type = 0,
    CAPS_OFF_SubType = 1,
    CAPS_OFF_Flags = 2,
    CAPS_OFF_Gamepad = 4,
    CAPS_OFF_Vibration = 16,
    CAPS_SIZE = 20,
};
const uint32_t XINPUT_DEVTYPE_GAMEPAD = 1;
const uint32_t XINPUT_DEVSUBTYPE_GAMEPAD = 1;
// XINPUT_KEYSTROKE: VirtualKey (WORD), Unicode (WCHAR), Flags (WORD -
// XINPUT_KEYSTROKE_KEYDOWN/KEYUP/REPEAT are bit flags, not a byte enum),
// UserIndex (BYTE), HidCode (BYTE): 2+2+2+1+1 = 8 bytes exactly, no padding.
enum {
    KS_OFF_VirtualKey = 0,
    KS_OFF_Unicode = 2,
    KS_OFF_Flags = 4,
    KS_OFF_UserIndex = 6,
    KS_OFF_HidCode = 7,
    KS_SIZE = 8,
};
const uint32_t XINPUT_KEYSTROKE_KEYDOWN = 1;
const uint32_t XINPUT_KEYSTROKE_KEYUP = 2;
// The virtual pad is wired, not battery powered.
enum { BATTERY_OFF_BatteryType = 0, BATTERY_OFF_BatteryLevel = 1, BATTERY_SIZE = 2 };
const uint32_t BATTERY_TYPE_WIRED = 1;
const uint32_t BATTERY_LEVEL_FULL = 3;

// Mirrors joy_served() (dinput_joystick.cpp): user 0 exists only in native
// pad mode with the XInput bit set in RECOMP_CONTROLS_PAD's native-API mask.
// 2026-11-24 (NFS Undercover: with no controller plugged in the game still showed the pad's d-pad as the prompt for a
// new job instead of Tab): user 0 is connected only while the host has a real controller (host_pad_connected). A host
// that cannot tell (the weak default, -1: other hosts and the tests) keeps the old answer, always connected.
// RECOMP_XINPUT_ALWAYS=1 = old.
bool pad_present() {
    static const bool always = recomp_env("XINPUT_ALWAYS") != nullptr;
    return always || host_pad_connected() != 0;
}
bool served() {
    return host_pad_mode() == 2 && (host_pad_native_apis() & 2) != 0 && pad_present();
}

// XInputEnable's stored state. Starts enabled, matching a freshly loaded
// XInput DLL that no one has called XInputEnable(FALSE) on yet.
bool g_enabled = true;
// The last vibration SetState was asked for, kept even while disabled: a
// disabled SetState must not reach the motors, but a following Enable(TRUE)
// has to resend exactly what the game last asked for, not silence.
uint16_t g_vib_low = 0, g_vib_high = 0;

// The pad bits (controls::PadBit, host_api.h) XInput reports as a button,
// and which wButtons bit each becomes. l2 and r2's digital press has no
// XInput button of its own - only the analog trigger bytes below - so they
// are absent here on purpose; their edges still make keystrokes.
struct ButtonMap {
    int pad_bit;
    uint32_t xinput_bit;
};
const ButtonMap kButtons[] = {
    {0, XINPUT_GAMEPAD_A},
    {1, XINPUT_GAMEPAD_B},
    {2, XINPUT_GAMEPAD_X},
    {3, XINPUT_GAMEPAD_Y},
    {4, XINPUT_GAMEPAD_LEFT_SHOULDER},
    {5, XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {8, XINPUT_GAMEPAD_LEFT_THUMB},
    {9, XINPUT_GAMEPAD_RIGHT_THUMB},
    {10, XINPUT_GAMEPAD_BACK},
    {11, XINPUT_GAMEPAD_START},
};

int16_t clamp16(int32_t v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

// ---------------------------------------------------------------------------
// Button remap (recomp_xinput_remap, below). Why: a game's own pad layout can
// leave a pad's natural controls unused - NFS Undercover's PC layout drives
// with X (gas) and B (brake) and ignores both triggers - and the player wants
// the console layout. A core mod switches a remap on while the game is in the
// part that needs it (driving) and off for menus, where A/B must stay
// select/back. Spec: "src=dst,..." with names a b x y lb rb ls rs back start
// up down left right lt rt and dst also "none"; a listed source no longer
// reports itself, it reports dst (a trigger as a button when pulled past a
// third; "rt@lo:hi" makes a trigger destination run lo..hi over the pull). Unlisted inputs pass unchanged. "" or
// nullptr ends the remap.
// ---------------------------------------------------------------------------
struct Remap {
    uint32_t src_bit = 0; // wButtons bit, or 0 for a trigger
    int src_trigger = -1; // 0 left, 1 right
    uint32_t dst_bit = 0; // wButtons bit, 0 = none
    int dst_trigger = -1; // a source can also become a full trigger
    int dst_lo = 0, dst_hi = 255; // "rt@lo:hi": the destination trigger runs lo (released) .. hi (pulled)
};
std::vector<Remap> g_remap;
std::mutex g_remap_m; // set by a mod's thread, read by whichever thread polls
bool button_named(const std::string &n, uint32_t *bit, int *trig) {
    static const struct { const char *name; uint32_t bit; } k[] = {
        {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X}, {"y", XINPUT_GAMEPAD_Y},
        {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"ls", XINPUT_GAMEPAD_LEFT_THUMB}, {"rs", XINPUT_GAMEPAD_RIGHT_THUMB}, {"back", XINPUT_GAMEPAD_BACK},
        {"start", XINPUT_GAMEPAD_START}, {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN},
        {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT}};
    *bit = 0, *trig = -1;
    if (n == "lt" || n == "rt") {
        *trig = n[0] == 'l' ? 0 : 1;
        return true;
    }
    if (n == "none")
        return true;
    for (const auto &e : k)
        if (n == e.name) {
            *bit = e.bit;
            return true;
        }
    return false;
}
void apply_remap(uint16_t *buttons, uint8_t trig[2]) {
    std::lock_guard<std::mutex> lock(g_remap_m);
    if (g_remap.empty())
        return;
    const uint16_t in = *buttons;
    const uint8_t tin[2] = {trig[0], trig[1]};
    uint16_t out = in;
    uint8_t tout[2] = {tin[0], tin[1]};
    for (const Remap &r : g_remap) { // a listed source stops reporting itself
        if (r.src_bit)
            out &= (uint16_t)~r.src_bit;
        else
            tout[r.src_trigger] = 0;
    }
    for (const Remap &r : g_remap) {
        const bool on = r.src_bit ? (in & r.src_bit) != 0 : tin[r.src_trigger] > 85;
        const uint8_t level = r.src_bit ? (on ? 255 : 0) : tin[r.src_trigger];
        if (r.dst_bit && on)
            out |= (uint16_t)r.dst_bit;
        if (r.dst_trigger >= 0) {
            // a range: for a game that reads only part of a trigger's travel (NFS Undercover turns a trigger byte
            // t into an axis of (t - 127) * 514 and reads that axis's upper half, so only t 191..255 counts)
            const bool ranged = r.dst_lo != 0 || r.dst_hi != 255;
            const uint8_t v = (uint8_t)(r.dst_lo + (level * (r.dst_hi - r.dst_lo) + 127) / 255);
            if (v > tout[r.dst_trigger] || ranged)
                tout[r.dst_trigger] = v;
        }
    }
    *buttons = out;
    trig[0] = tout[0], trig[1] = tout[1];
}

// Writes a 12-byte XINPUT_GAMEPAD at `gp` from the merged pad state.
void fill_gamepad(uint32_t gp, const HostPadState &s) {
    uint16_t buttons = 0;
    if (s.hat & 1)
        buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (s.hat & 2)
        buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (s.hat & 4)
        buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (s.hat & 8)
        buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    for (const ButtonMap &b : kButtons)
        if (s.buttons & (1u << b.pad_bit))
            buttons |= b.xinput_bit;
    uint8_t trig[2] = {s.l2, s.r2};
    apply_remap(&buttons, trig);
    wr16(gp + GP_OFF_wButtons, buttons);
    wr8(gp + GP_OFF_bLeftTrigger, trig[0]);
    wr8(gp + GP_OFF_bRightTrigger, trig[1]);
    wr16(gp + GP_OFF_sThumbLX, (uint16_t)clamp16(s.lx));
    // XInput's Y axis points up; the pad's points down (host_api.h).
    wr16(gp + GP_OFF_sThumbLY, (uint16_t)clamp16(-(int32_t)s.ly));
    wr16(gp + GP_OFF_sThumbRX, (uint16_t)clamp16(s.rx));
    wr16(gp + GP_OFF_sThumbRY, (uint16_t)clamp16(-(int32_t)s.ry));
}

// ---------------------------------------------------------------------------
// XInputGetKeystroke's replay queue.
//
// One host edge can become more than one keystroke - a diagonal hat change
// touches two direction bits at once - so edges are pumped into a small
// per-process queue and GetKeystroke hands out one entry per call, exactly
// as the real API does. `g_hat` is the last hat value turned into
// keystrokes, kept apart from any DirectInput device's own last-hat (there
// is no XInput device object to keep it in).
// ---------------------------------------------------------------------------
struct Keystroke {
    uint16_t vk;
    uint8_t flags;
};
std::vector<Keystroke> g_keystroke_queue;
uint32_t g_keystroke_sequence = 0;
// True once the first XInputGetKeystroke call has established where the
// replay starts. Kept apart from `g_keystroke_sequence == 0` because 0 is
// also a legitimate "nothing consumed yet" value for a host that has not
// produced any edge at all.
bool g_keystroke_started = false;
// The pump's own notion of "last hat", independent of any DirectInput
// device's (there is no XInput device object to keep one in). If the host's
// edge ring ever drops edges before this pump reads them - it can, the same
// way a DirectInput device sees DI_BUFFEROVERFLOW, except GetKeystroke has
// no such error to report - a hat edge inside the gap is never seen and
// this drifts from the pad's real last direction until the hat next
// returns to centre (0), which resynchronises it.
uint8_t g_hat = 0;

// VK_PAD_DPAD_* for one hat direction bit (host_api.h: 1 up, 2 right, 4
// down, 8 left), or 0 for a bit this pad's hat does not have.
uint16_t hat_vk(uint32_t bit) {
    switch (bit) {
    case 1:
        return 0x5810; // up
    case 2:
        return 0x5813; // right
    case 4:
        return 0x5811; // down
    case 8:
        return 0x5812; // left
    default:
        return 0;
    }
}

// The VK a pad button bit's edge becomes, or 0 for "ps" (bit 12), which has
// no XInput keystroke.
uint16_t button_vk(int pad_bit) {
    switch (pad_bit) {
    case 0:
        return 0x5800; // cross
    case 1:
        return 0x5801; // circle
    case 2:
        return 0x5802; // square
    case 3:
        return 0x5803; // triangle
    case 5:
        return 0x5804; // r1
    case 4:
        return 0x5805; // l1
    case 6:
        return 0x5806; // l2
    case 7:
        return 0x5807; // r2
    case 11:
        return 0x5814; // start
    case 10:
        return 0x5815; // select
    case 8:
        return 0x5816; // l3
    case 9:
        return 0x5817; // r3
    default:
        return 0;
    }
}

// Pulls host edges into the queue until it is non-empty or the host has none
// left. A button edge becomes one keystroke; a hat edge becomes one per
// direction bit that changed since the last one seen here; an axis edge is
// not a keystroke and is simply consumed.
void keystroke_pump() {
    HostPadEvent e;
    if (!g_keystroke_started) {
        g_keystroke_started = true;
        // The first call establishes "now": whatever the host already
        // queued before any game ever asked for a keystroke is a backlog,
        // not something to dump all at once, so it is skipped rather than
        // replayed. A hat edge in that backlog still moves g_hat (silently,
        // no keystroke queued for it), so the first real edge afterwards is
        // compared against the pad's actual last direction rather than 0.
        while (host_pad_next_event(g_keystroke_sequence, &e)) {
            g_keystroke_sequence = e.sequence;
            if (e.kind == 1)
                g_hat = (uint8_t)e.value;
        }
    }
    while (g_keystroke_queue.empty() && host_pad_next_event(g_keystroke_sequence, &e)) {
        g_keystroke_sequence = e.sequence;
        if (e.kind == 0) {
            uint16_t vk = button_vk((int)e.index);
            if (vk)
                g_keystroke_queue.push_back({vk, e.value ? (uint8_t)XINPUT_KEYSTROKE_KEYDOWN
                                                         : (uint8_t)XINPUT_KEYSTROKE_KEYUP});
        } else if (e.kind == 1) {
            uint8_t now = (uint8_t)e.value;
            uint8_t changed = now ^ g_hat;
            for (uint32_t bit = 1; bit <= 8; bit <<= 1) {
                if (!(changed & bit))
                    continue;
                uint16_t vk = hat_vk(bit);
                if (vk)
                    g_keystroke_queue.push_back({vk, (now & bit)
                                                         ? (uint8_t)XINPUT_KEYSTROKE_KEYDOWN
                                                         : (uint8_t)XINPUT_KEYSTROKE_KEYUP});
            }
            g_hat = now;
        }
        // kind 2 (axis): no keystroke; the loop moves on to the next edge.
    }
}

} // namespace

// Sets (spec) or ends ("" / nullptr) the button remap described above. Returns the number of pairs taken; a bad
// name is skipped and logged once per call. Exported so a core mod can find it with dlsym.
extern "C" __attribute__((used, visibility("default"))) int recomp_xinput_remap(const char *spec) {
    std::vector<Remap> next;
    std::string s = spec ? spec : "";
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos)
            end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        Remap r;
        std::string dst = eq == std::string::npos ? "" : item.substr(eq + 1);
        const size_t at_ = dst.find('@');
        if (at_ != std::string::npos) {
            if (sscanf(dst.c_str() + at_ + 1, "%d:%d", &r.dst_lo, &r.dst_hi) != 2 || r.dst_lo < 0 || r.dst_hi > 255 ||
                r.dst_lo > r.dst_hi)
                r.dst_lo = 0, r.dst_hi = 255;
            dst = dst.substr(0, at_);
        }
        if (eq == std::string::npos || !button_named(item.substr(0, eq), &r.src_bit, &r.src_trigger) ||
            !button_named(dst, &r.dst_bit, &r.dst_trigger) || (!r.src_bit && r.src_trigger < 0)) {
            fprintf(stderr, "[xinput] remap: \"%s\" not understood\n", item.c_str());
            continue;
        }
        next.push_back(r);
    }
    std::lock_guard<std::mutex> lock(g_remap_m);
    g_remap.swap(next);
    return (int)g_remap.size();
}

// The merged pad as the host sees it (before any remap), for a core mod's own controls. Returns the packet number.
extern "C" __attribute__((used, visibility("default"))) uint32_t recomp_pad_raw(HostPadState *out) {
    return host_pad_state(out);
}

namespace {

// ===========================================================================
// Shims
// ===========================================================================
void XInputGetState_(X86 *c) {
    uint32_t user = arg(c, 0);
    uint32_t out = arg(c, 1);
    {
        static const bool trace_calls = recomp_env("TRACE_INPUT") != nullptr;
        static unsigned calls;
        if (trace_calls && (++calls == 1 || calls % 2000 == 0))
            fprintf(stderr, "[xinput] GetState user %u call %u (served %d)\n", user, calls, served() ? 1 : 0);
    }
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (!out || !gm_valid(out, STATE_SIZE)) {
        set_eax(c, XI_ERROR_BAD_ARGUMENTS);
        return;
    }
    HostPadState s;
    memset(&s, 0, sizeof s);
    uint32_t packet = host_pad_state(&s);
    wr32(out + STATE_OFF_dwPacketNumber, packet);
    // Disabled: the state reports zero, but the packet number is still
    // the host's real one (XInputEnable does not stop it changing).
    if (g_enabled)
        fill_gamepad(out + STATE_OFF_Gamepad, s);
    else
        gm_zero(out + STATE_OFF_Gamepad, GP_SIZE);
    {
        static const bool trace_in = recomp_env("TRACE_INPUT") != nullptr;
        static uint32_t last_packet = 0;
        if (trace_in && packet != last_packet) {
            last_packet = packet;
            fprintf(stderr, "[xinput] packet %u buttons %04x lx %d ly %d\n", packet, rd16(out + 4),
                    (int16_t)rd16(out + 8), (int16_t)rd16(out + 10));
        }
    }
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputSetState_(X86 *c) {
    uint32_t user = arg(c, 0);
    uint32_t vib = arg(c, 1);
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (!vib || !gm_valid(vib, VIB_SIZE)) {
        set_eax(c, XI_ERROR_BAD_ARGUMENTS);
        return;
    }
    g_vib_low = (uint16_t)rd16(vib + VIB_OFF_wLeftMotorSpeed);
    g_vib_high = (uint16_t)rd16(vib + VIB_OFF_wRightMotorSpeed);
    // While XInputEnable(FALSE) is in effect the motors stay muted: the
    // values are remembered and only reach the host once re-enabled.
    if (g_enabled)
        host_pad_rumble(g_vib_low, g_vib_high);
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputGetCapabilities_(X86 *c) {
    uint32_t user = arg(c, 0);
    // arg(c, 1) is dwFlags (XINPUT_FLAG_GAMEPAD): the pad has nothing else
    // to filter by, so it is read but ignored.
    uint32_t out = arg(c, 2);
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (!out || !gm_valid(out, CAPS_SIZE)) {
        set_eax(c, XI_ERROR_BAD_ARGUMENTS);
        return;
    }
    gm_zero(out, CAPS_SIZE);
    wr8(out + CAPS_OFF_Type, XINPUT_DEVTYPE_GAMEPAD);
    wr8(out + CAPS_OFF_SubType, XINPUT_DEVSUBTYPE_GAMEPAD);
    wr16(out + CAPS_OFF_Flags, 0);
    uint32_t gp = out + CAPS_OFF_Gamepad;
    // Every button bit except the two Windows leaves unused (0x0400,
    // 0x0800), full-range triggers, and the thumbsticks' rest value as
    // Windows itself reports it for a capabilities query (0xFFC0, not 0).
    wr16(gp + GP_OFF_wButtons, (uint16_t)(0xFFFFu & ~(0x0400u | 0x0800u)));
    wr8(gp + GP_OFF_bLeftTrigger, 255);
    wr8(gp + GP_OFF_bRightTrigger, 255);
    wr16(gp + GP_OFF_sThumbLX, 0xFFC0u);
    wr16(gp + GP_OFF_sThumbLY, 0xFFC0u);
    wr16(gp + GP_OFF_sThumbRX, 0xFFC0u);
    wr16(gp + GP_OFF_sThumbRY, 0xFFC0u);
    wr16(out + CAPS_OFF_Vibration + 0, 0xFFFFu);
    wr16(out + CAPS_OFF_Vibration + 2, 0xFFFFu);
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputEnable_(X86 *c) {
    bool now = arg(c, 0) != 0;
    // Only reaches the host while served: there is nothing to mute or
    // resume when there is no controller for this call to have addressed.
    if (served()) {
        // The falling edge sends the mute rumble once - calling
        // Enable(FALSE) twice in a row must not re-send it - and the rising
        // edge resends whatever SetState last asked for, which SetState
        // itself withheld from the host while disabled.
        if (g_enabled && !now)
            host_pad_rumble(0, 0);
        else if (!g_enabled && now)
            host_pad_rumble(g_vib_low, g_vib_high);
    }
    g_enabled = now;
    // XInputEnable is void in the real DLL; EAX is left defined anyway for a
    // caller that (incorrectly) reads a return value.
    set_eax(c, 0);
}

void XInputGetBatteryInformation_(X86 *c) {
    uint32_t user = arg(c, 0);
    // arg(c, 1) is BatteryDevType (gamepad vs. headset): the pad is the only
    // device, so every request answers the same.
    uint32_t out = arg(c, 2);
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (!out || !gm_valid(out, BATTERY_SIZE)) {
        set_eax(c, XI_ERROR_BAD_ARGUMENTS);
        return;
    }
    wr8(out + BATTERY_OFF_BatteryType, BATTERY_TYPE_WIRED);
    wr8(out + BATTERY_OFF_BatteryLevel, BATTERY_LEVEL_FULL);
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputGetKeystroke_(X86 *c) {
    uint32_t user = arg(c, 0);
    // arg(c, 1) is reserved (always 0).
    uint32_t out = arg(c, 2);
    // Unlike every other entry point, GetKeystroke also accepts
    // XUSER_INDEX_ANY: a game that does not care which user pressed a menu
    // key polls "any" instead of a specific index.
    if (!served() || (user != 0 && user != XUSER_INDEX_ANY)) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    // Checked before touching the queue: a bad pointer must not silently
    // consume a keystroke that a following, valid call would then miss.
    if (!out || !gm_valid(out, KS_SIZE)) {
        set_eax(c, XI_ERROR_BAD_ARGUMENTS);
        return;
    }
    keystroke_pump();
    if (g_keystroke_queue.empty()) {
        set_eax(c, XI_ERROR_EMPTY);
        return;
    }
    Keystroke k = g_keystroke_queue.front();
    g_keystroke_queue.erase(g_keystroke_queue.begin());
    gm_zero(out, KS_SIZE);
    wr16(out + KS_OFF_VirtualKey, k.vk);
    wr16(out + KS_OFF_Flags, k.flags);
    wr8(out + KS_OFF_UserIndex, 0);
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputGetDSoundAudioDeviceGuids_(X86 *c) {
    uint32_t user = arg(c, 0);
    uint32_t render = arg(c, 1);
    uint32_t capture = arg(c, 2);
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    // The pad has no DirectSound identity of its own: zero GUIDs, meaning
    // "use the default audio device", same as a real controller with no
    // onboard audio hardware.
    if (render && gm_valid(render, 16))
        gm_zero(render, 16);
    if (capture && gm_valid(capture, 16))
        gm_zero(capture, 16);
    set_eax(c, XI_ERROR_SUCCESS);
}

void XInputGetAudioDeviceIds_(X86 *c) {
    uint32_t user = arg(c, 0);
    // arg(c, 1) render id buffer, arg(c, 3) capture id buffer: unused, the
    // pad has neither.
    uint32_t render_len = arg(c, 2);
    uint32_t capture_len = arg(c, 4);
    if (!served() || user != 0) {
        set_eax(c, XI_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (render_len && gm_valid(render_len, 4))
        wr32(render_len, 0);
    if (capture_len && gm_valid(capture_len, 4))
        wr32(capture_len, 0);
    set_eax(c, XI_ERROR_SUCCESS);
}

// ---------------------------------------------------------------------------
// Export tables. GetState/SetState/GetCapabilities/Enable are common to all
// three DLLs; the rest match what each one really exports (task brief).
// ---------------------------------------------------------------------------
const ImportShim g_xinput13_exports[] = {
    {"XINPUT1_3.dll", "XInputGetState", 2, XInputGetState_},
    {"XINPUT1_3.dll", "XInputSetState", 2, XInputSetState_},
    {"XINPUT1_3.dll", "XInputGetCapabilities", 3, XInputGetCapabilities_},
    {"XINPUT1_3.dll", "XInputEnable", 1, XInputEnable_},
    {"XINPUT1_3.dll", "XInputGetBatteryInformation", 3, XInputGetBatteryInformation_},
    {"XINPUT1_3.dll", "XInputGetKeystroke", 3, XInputGetKeystroke_},
    {"XINPUT1_3.dll", "XInputGetDSoundAudioDeviceGuids", 3, XInputGetDSoundAudioDeviceGuids_},
    // The same exports by ordinal, as NFS Undercover imports them (#2 #4 #5): an import by ordinal finds only
    // "ordN", so XInputEnable (#5, called when the window loses focus) had no pop count, the stack drifted and the
    // game's /GS check ended it with 0xC0000409 when the player switched away (2026-11-14).
    {"XINPUT1_3.dll", "ord2", 2, XInputGetState_},
    {"XINPUT1_3.dll", "ord3", 2, XInputSetState_},
    {"XINPUT1_3.dll", "ord4", 3, XInputGetCapabilities_},
    {"XINPUT1_3.dll", "ord5", 1, XInputEnable_},
    {"XINPUT1_3.dll", "ord6", 3, XInputGetDSoundAudioDeviceGuids_},
    {"XINPUT1_3.dll", "ord7", 3, XInputGetBatteryInformation_},
    {"XINPUT1_3.dll", "ord8", 3, XInputGetKeystroke_},
};
const ImportShim g_xinput14_exports[] = {
    {"XINPUT1_4.dll", "XInputGetState", 2, XInputGetState_},
    {"XINPUT1_4.dll", "XInputSetState", 2, XInputSetState_},
    {"XINPUT1_4.dll", "XInputGetCapabilities", 3, XInputGetCapabilities_},
    {"XINPUT1_4.dll", "XInputEnable", 1, XInputEnable_},
    {"XINPUT1_4.dll", "XInputGetBatteryInformation", 3, XInputGetBatteryInformation_},
    {"XINPUT1_4.dll", "XInputGetKeystroke", 3, XInputGetKeystroke_},
    {"XINPUT1_4.dll", "XInputGetAudioDeviceIds", 5, XInputGetAudioDeviceIds_},
};
const ImportShim g_xinput910_exports[] = {
    {"XINPUT9_1_0.dll", "XInputGetState", 2, XInputGetState_},
    {"XINPUT9_1_0.dll", "XInputSetState", 2, XInputSetState_},
    {"XINPUT9_1_0.dll", "XInputGetCapabilities", 3, XInputGetCapabilities_},
    {"XINPUT9_1_0.dll", "XInputEnable", 1, XInputEnable_},
    {"XINPUT9_1_0.dll", "XInputGetDSoundAudioDeviceGuids", 3, XInputGetDSoundAudioDeviceGuids_},
};

} // namespace

// dx_register_shims calls this beside dinput_register(). Registration stays
// unconditional, never gated on host_pad_mode(): host_pad_mode() is decided
// once, from RECOMP_CONTROLS_PAD, so it is effectively compile-time for a
// running process, but that is beside the point anyway. The loader's
// patch_iat() (runtime/loader.cpp) walks every IAT slot unconditionally and
// calls imports_alloc_trampoline(dll, name, nullptr, ARGC_UNKNOWN) for a
// static import whose (dll, name) nothing has registered - it never leaves
// the slot at 0, so a game that statically imports an XInput function still
// gets a live trampoline either way. The difference is argc: a registered
// shim carries its real stdcall argument count, so imports_dispatch pops
// the guest stack correctly even when it just answers
// ERROR_DEVICE_NOT_CONNECTED; an unregistered import is stuck at
// ARGC_UNKNOWN forever, so every call only pops the return address and
// leaves the real arguments' bytes on the stack, drifting ESP a little more
// on every single XInput call the game ever makes - whether or not it asks
// for native pad input. Real Windows has the same shape: xinput1_4.dll (or
// whichever redistributable ships) is always loadable for a statically
// linked game, controller or none - "no controller" is
// ERROR_DEVICE_NOT_CONNECTED from every entry point, never a missing DLL,
// which is exactly what `served()` enforces here.
void xinput_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_xinput13_exports, std::size(g_xinput13_exports));
    imports_register(g_xinput14_exports, std::size(g_xinput14_exports));
    imports_register(g_xinput910_exports, std::size(g_xinput910_exports));
}

void xinput_reset_for_test() {
    g_enabled = true;
    g_vib_low = g_vib_high = 0;
    g_keystroke_queue.clear();
    g_keystroke_sequence = 0;
    g_keystroke_started = false;
    g_hat = 0;
}
