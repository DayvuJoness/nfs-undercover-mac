#include "profile.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "../platform/os.h"
// imports.cpp: the label of trampoline `idx` ("d3d9.dll!IDirect3DDevice9::DrawIndexedPrimitive"), or null.
const char *imports_label_by_index(uint32_t idx);

extern "C" const int recomp_profile_enabled = [] {
    const char *value = recomp_env("PROFILE");
    return value && std::strcmp(value, "1") == 0;
}();
// Generated table supplies names; runtime-only tests need no generated table.
extern "C" __attribute__((weak)) const char *recomp_profile_name(uint32_t) {
    return nullptr;
}
namespace {
struct ThreadState {
    ProfileSlot slot;
    std::vector<uint32_t> stack;
};
// Intentionally process-lifetime: a sampler snapshot can outlive thread exit.
thread_local ThreadState *local = nullptr;
struct State {
    std::atomic<bool> stop{false};
    std::thread sampler;
    std::mutex mutex;
    std::map<uint32_t, uint64_t> counts;
    uint64_t idle = 0;
    // The load journal's window (RECOMP_PROFILE_EVERY): samples by guest thread, by (thread, function) and by
    // (thread, import), and the frames presented, so a slow stretch says which thread and what work it was.
    std::map<uint32_t, uint64_t> by_thread, by_thread_free;
    std::map<uint64_t, uint64_t> by_thread_fn, by_thread_import;
    uint64_t frames_total = 0, frames_at_window = 0;
    double guest_ms = 0;
    uint64_t guest_frames = 0;
};
State &state() {
    static State *s = new State;
    return *s;
}
void report() {
    profile_stop();
    auto rows = profile_snapshot();
    uint64_t total = 0;
    for (auto row : rows)
        total += row.samples;
    fprintf(stderr,
            "profile: 1000 Hz baton-holder wall samples; includes shims/host waits in caller\n");
    fprintf(stderr, "profile: attributed=%llu idle=%llu\n", (unsigned long long)total,
            (unsigned long long)state().idle);
    for (size_t i = 0; i < rows.size() && i < 30; ++i) {
        auto row = rows[i];
        const char *name = recomp_profile_name(row.index);
        if (!name) {
            fprintf(stderr, "profile: unresolved index=%u samples=%llu\n", row.index,
                    (unsigned long long)row.samples);
            continue;
        }
        fprintf(stderr, "profile: %s %.2f%% %llu\n", name,
                total ? 100.0 * row.samples / total : 0.0, (unsigned long long)row.samples);
    }
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.guest_frames)
        fprintf(stderr, "profile: guest_ms_per_frame=%.3f\n", s.guest_ms / s.guest_frames);
    else
        fprintf(stderr, "profile: guest_ms_per_frame omitted: no phase stats supplied\n");
}
void start() {
    static const bool started = [] {
        std::atexit(report);
        state().sampler = std::thread([] {
            auto &s = state();
            auto next = std::chrono::steady_clock::now();
            while (!s.stop.load(std::memory_order_relaxed)) {
                next += std::chrono::milliseconds(1);
                std::this_thread::sleep_until(next);
                auto *slot = sched_current_holder_slot();
                uint32_t top = slot ? slot->top.load(std::memory_order_relaxed) : PROFILE_IDLE;
                const uint32_t imp = slot ? slot->import.load(std::memory_order_relaxed) : PROFILE_IDLE;
                std::lock_guard<std::mutex> lock(s.mutex);
                if (top == PROFILE_IDLE && imp == PROFILE_IDLE)
                    ++s.idle;
                else if (top != PROFILE_IDLE)
                    ++s.counts[top];
                // Threads running beside the holder (RECOMP_FREE_THREADS) are sampled too, as "thread N (own core)".
                static std::vector<ProfileSlot *> all;
                static int refresh = 0;
                if (++refresh % 1000 == 1) {
                    s.mutex.unlock();
                    auto fresh = sched_all_profile_slots();
                    s.mutex.lock();
                    all = fresh;
                }
                for (ProfileSlot *o : all) {
                    if (o == slot)
                        continue;
                    const uint32_t ot = o->top.load(std::memory_order_relaxed);
                    const uint32_t oi = o->import.load(std::memory_order_relaxed);
                    if (ot == PROFILE_IDLE && oi == PROFILE_IDLE)
                        continue;
                    const uint32_t tag = o->tid | 0x10000u; // own core
                    ++s.by_thread_free[tag];
                    if (oi != PROFILE_IDLE)
                        ++s.by_thread_import[(uint64_t)tag << 32 | oi];
                    else
                        ++s.by_thread_fn[(uint64_t)tag << 32 | ot];
                }
                if (slot) {
                    ++s.by_thread[slot->tid];
                    if (imp != PROFILE_IDLE)
                        ++s.by_thread_import[(uint64_t)slot->tid << 32 | imp];
                    else if (top != PROFILE_IDLE)
                        ++s.by_thread_fn[(uint64_t)slot->tid << 32 | top];
                }
                // A stalled sampler must not manufacture catch-up samples.
                auto now = std::chrono::steady_clock::now();
                if (now > next + std::chrono::milliseconds(1))
                    next = now;
                // RECOMP_PROFILE_EVERY=N: also print the top functions of each N-second window and start a new
                // one, so a run that never exits normally (killed by a timer while it spins without presenting
                // a frame, NFS Undercover 2026-11-14) still says where its time goes.
                static const int every = [] {
                    const char *e = recomp_env("PROFILE_EVERY");
                    return e ? std::atoi(e) : 0;
                }();
                static auto window_end = now + std::chrono::seconds(every > 0 ? every : 1);
                if (every > 0 && now >= window_end) {
                    window_end = now + std::chrono::seconds(every);
                    std::vector<std::pair<uint64_t, uint32_t>> rows;
                    uint64_t total = 0;
                    for (auto [index, samples] : s.counts) {
                        rows.push_back({samples, index});
                        total += samples;
                    }
                    std::sort(rows.rbegin(), rows.rend());
                    // The load journal: RECOMP_PROFILE_LOG=<file> (appended), stderr otherwise.
                    static FILE *out = [] {
                        const char *p = recomp_env("PROFILE_LOG");
                        FILE *f = p ? fopen(p, "a") : nullptr;
                        return f ? f : stderr;
                    }();
                    static auto t0 = now;
                    const double t = std::chrono::duration<double>(now - t0).count();
                    const uint64_t frames = s.frames_total - s.frames_at_window;
                    s.frames_at_window = s.frames_total;
                    uint64_t all = s.idle;
                    for (auto [tid, n] : s.by_thread)
                        all += n;
                    fprintf(out, "load t=%.0fs: %.1f fps; baton busy %.0f%%;", t, (double)frames / every,
                            all ? 100.0 * (all - s.idle) / all : 0.0);
                    for (auto [tid, n] : s.by_thread)
                        fprintf(out, " thread %u %.0f%%", tid, all ? 100.0 * n / all : 0.0);
                    for (auto [tid, n] : s.by_thread_free)
                        fprintf(out, " thread %u (own core) busy %.0f%%", tid & 0xffffu, all ? 100.0 * n / all : 0.0);
                    fprintf(out, "\n");
                    for (auto *m : {&s.by_thread_fn, &s.by_thread_import}) {
                        std::vector<std::pair<uint64_t, uint64_t>> top_rows;
                        for (auto [key, n] : *m)
                            top_rows.push_back({n, key});
                        std::sort(top_rows.rbegin(), top_rows.rend());
                        // RECOMP_PROFILE_TOP=N: rows per list (12 by default).
                        static const size_t top_n = [] {
                            const char *e = recomp_env("PROFILE_TOP");
                            return e && std::atoi(e) > 0 ? (size_t)std::atoi(e) : (size_t)12;
                        }();
                        for (size_t i = 0; i < top_rows.size() && i < top_n; ++i) {
                            const uint32_t tid = (uint32_t)(top_rows[i].second >> 32);
                            const uint32_t idx = (uint32_t)top_rows[i].second;
                            const char *name = m == &s.by_thread_fn ? recomp_profile_name(idx)
                                                                    : imports_label_by_index(idx);
                            fprintf(out, "load   %s thread %u%s %s %.1f%%\n", m == &s.by_thread_fn ? "code" : "host",
                                    tid & 0xffffu, tid & 0x10000u ? "(own)" : "", name ? name : "?",
                                    all ? 100.0 * top_rows[i].first / all : 0.0);
                        }
                    }
                    fflush(out);
                    for (size_t i = 0; i < rows.size() && i < 15; ++i) {
                        const char *name = recomp_profile_name(rows[i].second);
                        fprintf(stderr, "profile window: %s %.1f%%\n", name ? name : "?",
                                total ? 100.0 * rows[i].first / total : 0.0);
                    }
                    s.counts.clear();
                    s.by_thread.clear();
                    s.by_thread_free.clear();
                    s.by_thread_fn.clear();
                    s.by_thread_import.clear();
                    s.idle = 0;
                }
            }
        });
        return true;
    }();
    (void)started;
}
} // namespace
// One per IDirect3DDevice9::Present (d3d9.cpp), for the load journal's frame rate.
void profile_note_frame() {
    if (!recomp_profile_enabled)
        return;
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.frames_total;
}
void profile_note_guest_ms(double milliseconds) {
    if (!recomp_profile_enabled || !std::isfinite(milliseconds) || milliseconds < 0)
        return;
    start();
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.guest_ms += milliseconds;
    ++s.guest_frames;
}
extern "C" void recomp_profile_import(uint32_t trampoline_index) {
    if (!recomp_profile_enabled)
        return;
    if (!local) {
        local = new ThreadState;
        sched_register_profile_slot(&local->slot);
        start();
    }
    local->slot.import.store(trampoline_index, std::memory_order_relaxed);
}
extern "C" void recomp_profile_push(uint32_t index) {
    if (!recomp_profile_enabled)
        return;
    if (!local) {
        local = new ThreadState;
        sched_register_profile_slot(&local->slot);
        start();
    }
    local->stack.push_back(index);
    local->slot.top.store(index, std::memory_order_relaxed);
}
extern "C" uint32_t recomp_profile_depth() {
    return local ? static_cast<uint32_t>(local->stack.size()) : 0;
}
extern "C" void recomp_profile_truncate(uint32_t depth) {
    if (!local)
        return;
    if (depth < local->stack.size())
        local->stack.resize(depth);
    local->slot.top.store(local->stack.empty() ? PROFILE_IDLE : local->stack.back(),
                          std::memory_order_relaxed);
}
extern "C" void recomp_profile_pop() {
    uint32_t depth = recomp_profile_depth();
    if (depth)
        recomp_profile_truncate(depth - 1);
}
void profile_stop() {
    auto &s = state();
    s.stop.store(true, std::memory_order_relaxed);
    if (s.sampler.joinable())
        s.sampler.join();
}
std::vector<ProfileSample> profile_snapshot() {
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<ProfileSample> rows;
    for (auto [index, samples] : s.counts)
        rows.push_back({index, samples});
    std::sort(rows.begin(), rows.end(), [](auto a, auto b) {
        return a.samples != b.samples ? a.samples > b.samples : a.index < b.index;
    });
    return rows;
}
