#include "kernel/stall_watchdog.hpp"

#include "kernel/kernel.hpp"
#include "profile.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

namespace portablekit::stall_watchdog {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::int64_t kRepeatMs = 30000;
constexpr std::size_t kPadHistory = 16u;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// <prefix>_STALL_WATCHDOG: 0 or off turns it off; a number of seconds
// replaces the five.
std::int64_t stall_limit_ms() {
    static const std::int64_t value = [] {
        const char *text = portablekit::env("STALL_WATCHDOG");
        if (text == nullptr || *text == '\0') return std::int64_t{5000};
        if (std::strcmp(text, "off") == 0) return std::int64_t{0};
        const double seconds = std::atof(text);
        return seconds > 0.0 ? static_cast<std::int64_t>(seconds * 1000.0) : std::int64_t{0};
    }();
    return value;
}

bool enabled() { return stall_limit_ms() > 0; }

struct State {
    std::atomic<std::int64_t> last_frame_ms{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<bool> menu{false};
    std::atomic<std::int64_t> last_idle_ms{0};
    // Pad words and when they were handed over, written by the main thread
    // only; the watchdog thread's reads may be torn, which a dump can bear.
    std::array<std::atomic<std::uint32_t>, kPadHistory> pad{};
    std::array<std::atomic<std::int64_t>, kPadHistory> pad_ms{};
    std::atomic<std::uint64_t> pads{0};
    std::atomic<std::uint32_t> last_pad{0xFFFFFFFFu};
    // Main thread only.
    std::int64_t dumped_ms{0};
    std::uint64_t dumped_frames{~0ull};
    // The watchdog thread's.
    std::int64_t reported_ms{0};
    std::uint64_t reported_frames{~0ull};
};

State &state() {
    static State value;
    return value;
}

// Frames are what the watchdog waits for; a stall is time without one.
std::int64_t stalled_ms(const State &s, std::int64_t now) {
    if (s.menu.load() || s.frames.load() == 0u) return 0;
    return now - s.last_frame_ms.load();
}

std::string pad_history(const State &s, std::int64_t now) {
    std::ostringstream out;
    const std::uint64_t count = s.pads.load();
    out << "[stall] last pad words handed to the game (newest first; only changes are kept):";
    for (std::uint64_t i = 0; i < kPadHistory && i < count; ++i) {
        const std::size_t slot = static_cast<std::size_t>((count - 1u - i) % kPadHistory);
        out << " 0x" << std::hex << s.pad[slot].load() << std::dec << "@-" << (now - s.pad_ms[slot].load()) << "ms";
    }
    return out.str();
}

void watch() {
    State &s = state();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::int64_t now = now_ms();
        const std::int64_t stalled = stalled_ms(s, now);
        if (stalled < stall_limit_ms()) continue;
        const std::uint64_t frames = s.frames.load();
        if (frames == s.reported_frames && now - s.reported_ms < kRepeatMs) continue;
        s.reported_frames = frames;
        s.reported_ms = now;
        std::ostringstream out;
        out << "[stall] no frame for " << stalled / 1000.0 << " s after " << frames << " frames; the scheduler "
            << (s.last_idle_ms.load() == 0 ? std::string("never went idle")
                                           : "last went idle " + std::to_string(now - s.last_idle_ms.load()) +
                                                 " ms ago")
            << "\n"
            << pad_history(s, now) << "\n";
        std::cerr << out.str() << std::flush;
    }
}

void start_thread() {
    static const bool started = [] {
        std::thread(watch).detach();
        return true;
    }();
    (void)started;
}

} // namespace

void note_frame() {
    if (!enabled()) return;
    State &s = state();
    s.last_frame_ms.store(now_ms());
    if (s.frames.fetch_add(1u) == 0u) start_thread();
}

void note_menu(bool open) {
    if (!enabled()) return;
    State &s = state();
    s.menu.store(open);
    s.last_frame_ms.store(now_ms());
}

void note_pad(std::uint32_t buttons) {
    if (!enabled()) return;
    State &s = state();
    if (s.last_pad.exchange(buttons) == buttons) return;
    const std::uint64_t index = s.pads.load();
    const std::size_t slot = static_cast<std::size_t>(index % kPadHistory);
    s.pad[slot].store(buttons);
    s.pad_ms[slot].store(now_ms());
    s.pads.store(index + 1u);
}

void idle(const Kernel &kernel) {
    if (!enabled()) return;
    State &s = state();
    const std::int64_t now = now_ms();
    s.last_idle_ms.store(now);
    const std::int64_t stalled = stalled_ms(s, now);
    if (stalled < stall_limit_ms()) return;
    const std::uint64_t frames = s.frames.load();
    if (frames == s.dumped_frames && now - s.dumped_ms < kRepeatMs) return;
    s.dumped_frames = frames;
    s.dumped_ms = now;
    std::ostringstream out;
    out << "[stall] no frame for " << stalled / 1000.0 << " s after " << frames
        << " frames, and no guest thread can run. The emulated PSP:\n";
    kernel.dump_state(out);
    out << pad_history(s, now) << "\n";
    std::cerr << out.str() << std::flush;
}

} // namespace portablekit::stall_watchdog
