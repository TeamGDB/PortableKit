#pragma once

// The interface an HLE extension module is written against.
//
// An extension module is a library built separately from PortableKit's own
// sources and linked into a program at build time (PORTABLEKIT_HLE_EXTENSION_DIRS,
// see docs/HLE_EXTENSIONS.md). It implements PSP system functions -- the HLE
// the game's imports reach, named by library and NID -- that PortableKit
// leaves as logging stubs, and, where it asks to, replaces PortableKit's own
// implementation of a function.
//
// PortableKit knows nothing about any module. A module defines one entry
// function with PORTABLEKIT_HLE_EXTENSION(name); the build lists the modules
// it was given and the host calls each entry once at startup, after its own
// HLE is registered, with a Registry to add functions to.
//
// This header is the stable part of the interface. A handler may also include
// headers from PortableKit's host/ directory, which the build puts on the
// module's include path, but those change without notice.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Raised when this header changes incompatibly, so a module can check what it
// is being built against.
#define PORTABLEKIT_HLE_EXTENSION_INTERFACE 1

namespace portablekit::hle_extension {

using Runtime = psprecomp::Runtime;
using AllegrexContext = psprecomp::AllegrexContext;
using GuestMemory = psprecomp::GuestMemory;

// The signature of every HLE handler, PortableKit's own included. The handler
// runs when the game calls the import, with `ctx` holding the caller's
// registers; it reads its arguments from them and ends with finish().
using Handler = std::function<void(Runtime &, AllegrexContext &)>;

// How a function relates to one PortableKit already implements.
enum class Mode {
    // Only where PortableKit has no implementation of its own: the function
    // replaces the logging stub. A function PortableKit implements keeps
    // PortableKit's implementation, and the startup report says so.
    FillIn,
    // Replaces PortableKit's implementation. The host logs each override once
    // at startup, so it is always visible which implementation is active.
    Override,
};

struct Function {
    std::string library; // the PSP library the game imports it from, e.g. "sceHttp"
    std::uint32_t nid{};
    std::string name;    // for logs; may be empty when PortableKit's NID table names it
    Handler handler;
    Mode mode = Mode::FillIn;
};

// What a module's entry function fills in. Adding a function does nothing yet;
// the host applies what was added after the entry returns.
class Registry {
public:
    void add(std::string_view library, std::uint32_t nid, std::string_view name, Handler handler,
             Mode mode = Mode::FillIn) {
        functions_.push_back(Function{std::string(library), nid, std::string(name), std::move(handler), mode});
    }
    void override_builtin(std::string_view library, std::uint32_t nid, std::string_view name, Handler handler) {
        add(library, nid, name, std::move(handler), Mode::Override);
    }

    [[nodiscard]] const std::vector<Function> &functions() const noexcept { return functions_; }

private:
    std::vector<Function> functions_;
};

// --- What a handler needs --------------------------------------------------

// PSP EABI argument registers: a0-a3, then t0-t3.
[[nodiscard]] inline std::uint32_t arg(const AllegrexContext &ctx, unsigned index) noexcept {
    return index < 4u ? ctx.gpr[4u + index] : ctx.gpr[8u + (index - 4u)];
}

// A 64-bit argument occupies an even-aligned register pair, low word first.
[[nodiscard]] inline std::uint64_t arg64(const AllegrexContext &ctx, unsigned low_index) noexcept {
    return static_cast<std::uint64_t>(arg(ctx, low_index)) |
           (static_cast<std::uint64_t>(arg(ctx, low_index + 1u)) << 32u);
}

// Guest memory is Runtime::memory(): load8/16/32 and store8/16/32 by guest
// address. A NUL-terminated string at `address`, at most `max_length` bytes.
[[nodiscard]] inline std::string read_cstring(const GuestMemory &memory, std::uint32_t address,
                                              std::size_t max_length = 512u) {
    std::string text;
    if (address == 0u) return text;
    for (std::size_t i = 0; i < max_length; ++i) {
        const auto c = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0') break;
        text.push_back(c);
    }
    return text;
}

// Implemented by the host.

// Returns `result` to the game in v0 and lets the kernel switch threads or
// deliver an interrupt that became due, exactly as PortableKit's own handlers
// return. It is the last thing a handler does.
void finish(AllegrexContext &ctx, std::uint32_t result);
// The same for a 64-bit result, in v0 (low word) and v1.
void finish64(AllegrexContext &ctx, std::uint64_t result);
// Emulated time since boot, in microseconds.
[[nodiscard]] std::uint64_t now_us();

// Blocking. Each of these ends the handler in place of finish(): the calling
// guest thread waits, other threads run, and the game sees the import return
// when the wait is over.

// Waits until `poll` returns a value, which becomes the import's result.
// `poll` runs on the emulation thread whenever the scheduler looks for work,
// and at least every millisecond of emulated time; `timed_out` is true on the
// last call, once `timeout_us` of emulated time has passed (none: no limit).
// What a handler waits for on the host (a socket, a worker thread) it checks
// here; the poll must not block.
using HostPoll = std::function<std::optional<std::uint32_t>(bool timed_out)>;
void wait_host(AllegrexContext &ctx, std::optional<std::uint64_t> timeout_us, HostPoll poll);
// Puts the calling thread to sleep for `microseconds` of emulated time, then
// returns `result`.
void delay(AllegrexContext &ctx, std::uint64_t microseconds, std::uint32_t result = 0u);

// Calls into the game. `arguments` go in a0-a3 and then t0-t3, so a call takes
// up to kMaxGuestArguments; a0-a3 are always set (to 0 past the last given).
inline constexpr std::size_t kMaxGuestArguments = 8u;

// Calls the guest function at `function` in the calling thread, the way a
// library calls back into the game; it ends the handler in place of finish().
// When the function returns, `on_return` receives its v0 with `ctx` back at
// the import's return address, and either ends the import (finish()) or calls
// again. More than kMaxGuestArguments arguments throw.
using GuestReturn = std::function<void(AllegrexContext &ctx, std::uint32_t result)>;
void call_guest(AllegrexContext &ctx, std::uint32_t function, std::span<const std::uint32_t> arguments,
                GuestReturn on_return);
inline void call_guest(AllegrexContext &ctx, std::uint32_t function, std::initializer_list<std::uint32_t> arguments,
                       GuestReturn on_return) {
    call_guest(ctx, function, std::span<const std::uint32_t>(arguments.begin(), arguments.size()),
               std::move(on_return));
}
// Queues a call of `function` in interrupt context, as the system delivers a
// handler the game registered (an event, a state change): it runs when the
// game next allows interrupts, not inside this handler, which still ends
// with finish(). `on_return`, which may be null, receives its v0.
using QueuedReturn = std::function<void(std::uint32_t result)>;
void queue_guest_call(std::uint32_t function, std::span<const std::uint32_t> arguments,
                      QueuedReturn on_return = nullptr);
inline void queue_guest_call(std::uint32_t function, std::initializer_list<std::uint32_t> arguments,
                             QueuedReturn on_return = nullptr) {
    queue_guest_call(function, std::span<const std::uint32_t>(arguments.begin(), arguments.size()),
                     std::move(on_return));
}
// The kernel's id (SceUID) of the guest thread that made the call; 0 in
// interrupt context.
[[nodiscard]] std::int32_t current_thread_id();
// Writes `message` to the log the first time `key` is seen.
void log_once(std::string_view key, std::string_view message);
// The port's environment variable <prefix>_<name> (for example
// PORTABLEKIT_<name> in the desktop app), or null when it is not set.
[[nodiscard]] const char *env(const char *name);

} // namespace portablekit::hle_extension

// Defines a module's entry function. `name` is the module's name as its
// CMakeLists.txt gives it to portablekit_add_hle_extension(), a C identifier:
//
//   PORTABLEKIT_HLE_EXTENSION(my_module) {
//       registry.add("sceHttp", 0xAB1ABE07u, "sceHttpInit", [](auto &, auto &ctx) {
//           portablekit::hle_extension::finish(ctx, 0u);
//       });
//   }
#define PORTABLEKIT_HLE_EXTENSION(name)                                                                     \
    void portablekit_hle_extension_##name(::portablekit::hle_extension::Registry &registry)
