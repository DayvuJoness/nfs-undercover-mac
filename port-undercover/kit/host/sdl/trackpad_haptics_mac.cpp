// trackpad_haptics_mac.cpp - taps on a MacBook's Force Touch trackpad, for games played without a pad.
//
// Why: NFS Undercover computes force feedback only for a DirectInput wheel; the person plays on the keyboard and
// asked for the trackpad to give a feel of crashes, broken objects, gear changes, timers and menu moves. The
// trackpad's actuator can only make short clicks (three useful strengths, no long rumble), so a "rumble" here is a
// pattern of clicks: an impact is an envelope that starts at the hit's strength with clicks close together and
// decays - weaker clicks, further apart - like a real knock dying out (the person: a head-on crash starts sharp, fast
// and strong and quickly falls away; knocking over a road sign is faint and short).
//
// How: Apple's private MultitouchSupport framework (the same calls HapticKey and similar tools use) actuates the
// trackpad whether or not a finger is on it. It is loaded with dlopen, so a Mac without it (or a future macOS that
// drops it) just gets no taps: the calls return 0 and the reason is logged once.
//
// host_trackpad_haptic(level, count, interval_ms): level 1 weak, 2 medium, 3 strong; count even clicks.
// host_trackpad_impact(intensity 0..1, length): the decaying envelope; length scales its duration (1 = normal).
// Neither blocks the caller: a worker thread plays the pattern. A new pattern replaces the one playing when it is at
// least about as strong as what is left of it (a harder hit cuts in; a weak tap during a crash is dropped).
// Env: RECOMP_TRACKPAD_IDS="weak,medium,strong" actuation ids (default 3,4,6), RECOMP_TRACKPAD_LOG=1 logs patterns.
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {

typedef void *MTDeviceRef;
typedef const void *CFTypeRef;
typedef int32_t IOReturnT;
using Clock = std::chrono::steady_clock;

struct Api {
    MTDeviceRef (*device_create_default)(void) = nullptr;
    int32_t (*device_get_id)(MTDeviceRef, uint64_t *) = nullptr;
    CFTypeRef (*actuator_create)(uint64_t) = nullptr;
    IOReturnT (*actuator_open)(CFTypeRef) = nullptr;
    IOReturnT (*actuator_actuate)(CFTypeRef, int32_t, uint32_t, float, float) = nullptr;
};

struct Click {
    int at_ms; // from the pattern's start
    int level; // 1..3
};

std::mutex g_m;
std::condition_variable g_cv;
bool g_started = false, g_failed = false;
std::vector<Click> g_pat;
size_t g_pos = 0;
Clock::time_point g_start;
float g_strength = 0.0f, g_decay_s = 1.0f; // for "what is left" of the playing pattern
bool g_playing_impact = false;              // a knock: plain clicks never cut it while it is still felt
uint64_t g_serial = 0;
int g_ids[4] = {0, 3, 4, 6};
bool g_log = false;

// Opens the built-in trackpad's actuator. Returns nullptr (and says why, once) when it can't.
CFTypeRef open_actuator(Api &a) {
    void *lib = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",
                       RTLD_LAZY | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "[host] trackpad taps: MultitouchSupport is not available on this Mac\n");
        return nullptr;
    }
    a.device_create_default = (MTDeviceRef(*)(void))dlsym(lib, "MTDeviceCreateDefault");
    a.device_get_id = (int32_t(*)(MTDeviceRef, uint64_t *))dlsym(lib, "MTDeviceGetDeviceID");
    a.actuator_create = (CFTypeRef(*)(uint64_t))dlsym(lib, "MTActuatorCreateFromDeviceID");
    a.actuator_open = (IOReturnT(*)(CFTypeRef))dlsym(lib, "MTActuatorOpen");
    a.actuator_actuate = (IOReturnT(*)(CFTypeRef, int32_t, uint32_t, float, float))dlsym(lib, "MTActuatorActuate");
    if (!a.device_create_default || !a.device_get_id || !a.actuator_create || !a.actuator_open ||
        !a.actuator_actuate) {
        fprintf(stderr, "[host] trackpad taps: MultitouchSupport lacks the actuator calls on this macOS\n");
        return nullptr;
    }
    MTDeviceRef dev = a.device_create_default();
    uint64_t id = 0;
    if (!dev || a.device_get_id(dev, &id) != 0 || !id) {
        fprintf(stderr, "[host] trackpad taps: no built-in trackpad found\n");
        return nullptr;
    }
    CFTypeRef act = a.actuator_create(id);
    if (!act) {
        fprintf(stderr, "[host] trackpad taps: the trackpad has no actuator (not a Force Touch trackpad?)\n");
        return nullptr;
    }
    const IOReturnT r = a.actuator_open(act);
    if (r != 0) {
        fprintf(stderr, "[host] trackpad taps: opening the actuator failed (0x%x)\n", (unsigned)r);
        return nullptr;
    }
    fprintf(stderr, "[host] trackpad taps: ready (trackpad %llx, ids %d/%d/%d)\n", (unsigned long long)id, g_ids[1],
            g_ids[2], g_ids[3]);
    return act;
}

void worker() {
    Api a;
    CFTypeRef act = open_actuator(a);
    std::unique_lock<std::mutex> lk(g_m);
    if (!act) {
        g_failed = true;
        g_pat.clear();
        return;
    }
    unsigned errors = 0;
    for (;;) {
        g_cv.wait(lk, [] { return g_pos < g_pat.size(); });
        const uint64_t serial = g_serial;
        const Click c = g_pat[g_pos];
        const Clock::time_point when = g_start + std::chrono::milliseconds(c.at_ms);
        if (g_cv.wait_until(lk, when, [serial] { return g_serial != serial; }))
            continue; // replaced while waiting: start the new pattern
        ++g_pos;
        lk.unlock();
        const IOReturnT r = a.actuator_actuate(act, g_ids[c.level < 1 ? 1 : c.level > 3 ? 3 : c.level], 0, 0.0f, 0.0f);
        if (r != 0 && errors++ < 5)
            fprintf(stderr, "[host] trackpad tap failed (0x%x)\n", (unsigned)r);
        lk.lock();
    }
}

// What is left of the playing pattern's strength now (0 when it is over).
float remaining_locked() {
    if (g_pos >= g_pat.size())
        return 0.0f;
    const float t = std::chrono::duration<float>(Clock::now() - g_start).count();
    return g_strength * std::exp(-3.0f * t / g_decay_s);
}

// Starts a pattern unless a stronger one is still playing. Returns 0 when this Mac can't tap.
int start_locked(std::vector<Click> &&pat, float strength, float decay_s, const char *what) {
    if (g_failed)
        return 0;
    if (!g_started) {
        g_started = true;
        g_log = getenv("RECOMP_TRACKPAD_LOG") != nullptr;
        if (const char *e = getenv("RECOMP_TRACKPAD_IDS"))
            sscanf(e, "%d,%d,%d", &g_ids[1], &g_ids[2], &g_ids[3]);
        std::thread(worker).detach();
    }
    const bool impact = what[0] == 'i';
    const float left = remaining_locked();
    if (pat.empty() || strength < left * 0.8f || (!impact && g_playing_impact && left > 0.12f))
        return 1;
    g_playing_impact = impact;
    if (g_log) {
        fprintf(stderr, "[host] trackpad %s %.2f: %zu clicks over %d ms, levels", what, strength, pat.size(),
                pat.back().at_ms);
        for (const Click &c : pat)
            fprintf(stderr, " %d", c.level);
        fprintf(stderr, "\n");
    }
    g_pat = std::move(pat);
    g_pos = 0;
    g_start = Clock::now();
    g_strength = strength;
    g_decay_s = decay_s;
    ++g_serial;
    g_cv.notify_all();
    return 1;
}

} // namespace

extern "C" __attribute__((used, visibility("default"))) int host_trackpad_haptic(int level, int count,
                                                                                  int interval_ms) {
    if (level <= 0 || count <= 0)
        return 1;
    level = level > 3 ? 3 : level;
    count = count > 16 ? 16 : count;
    interval_ms = interval_ms < 8 ? 8 : interval_ms > 300 ? 300 : interval_ms;
    std::vector<Click> pat;
    for (int i = 0; i < count; ++i)
        pat.push_back({i * interval_ms, level});
    std::lock_guard<std::mutex> lk(g_m);
    // even clicks count as a modest strength, so a crash still playing isn't cut by a gear click
    return start_locked(std::move(pat), 0.15f * (float)level, 0.05f + count * interval_ms / 1000.0f, "taps");
}

extern "C" __attribute__((used, visibility("default"))) int host_trackpad_impact(float intensity, float length) {
    if (!(intensity > 0.0f))
        return 1;
    const float i = intensity > 1.0f ? 1.0f : intensity;
    const float len = length > 0.2f ? (length < 3.0f ? length : 3.0f) : 0.2f;
    std::vector<Click> pat;
    // the duration grows with the hit; clicks start close together (harder = closer) and spread out as it decays
    const float dur_s = (0.05f + 0.85f * std::pow(i, 1.2f)) * len;
    float gap_ms = 9.0f + 26.0f * (1.0f - i);
    float t_ms = 0.0f;
    while (pat.size() < 64) {
        const float a = i * std::exp(-3.0f * (t_ms / 1000.0f) / dur_s);
        if (a < 0.05f && !pat.empty())
            break;
        pat.push_back({(int)t_ms, a >= 0.45f ? 3 : a >= 0.2f ? 2 : 1});
        if (t_ms / 1000.0f > dur_s)
            break;
        t_ms += gap_ms;
        gap_ms *= 1.16f;
    }
    std::lock_guard<std::mutex> lk(g_m);
    return start_locked(std::move(pat), i, dur_s, "impact");
}
