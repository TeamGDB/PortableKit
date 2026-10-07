#pragma once

#include <cstdint>
#include <functional>

namespace portablekit {
class Kernel;
}

// The stall watchdog: when the game has shown no new frame for five seconds
// outside the paused menu, says what the emulated PSP is doing, on stderr
// and flushed, since a stalled game on a handheld cannot be attached to.
//
// - From the scheduler's idle loop, which is where the main thread waits
//   when every guest thread is blocked: every thread's state, what it waits
//   for, its pc and ra, the objects waited on, the clock and the pacing
//   (Kernel::dump_state). Once when the stall is noticed and again every 30
//   seconds while it lasts.
// - From a thread of its own, for a stall anywhere else: how long since the
//   last frame, when the scheduler last went idle, and the last pad words
//   handed to the game.
//
// <prefix>_STALL_WATCHDOG=0 turns it off; =N waits N seconds instead of five.
namespace portablekit::stall_watchdog {

// The game flipped its framebuffer.
void note_frame();
// The paused menu opened (true) or closed; no frames come meanwhile.
void note_menu(bool open);
// A pad sample handed to the game.
void note_pad(std::uint32_t buttons);
// The scheduler found nothing to run. On the main thread.
void idle(const Kernel &kernel);

// Called from idle() about ten times a second while a stall lasts: the
// window's events are otherwise only read when the game flips, so a stalled
// game would ignore closing the window and SIGTERM (which SDL turns into a
// quit event). Set once by whoever owns the window.
void set_stalled_pump(std::function<void()> pump);

} // namespace portablekit::stall_watchdog
