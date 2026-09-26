// The host side of include/portablekit/hle_extension.hpp for
// tests/hle_extension_tests.cpp, which runs handlers with no kernel: a result
// goes to v0 and the rest does as little as it can while staying observable.

#include "hle/hle_extensions.hpp"
#include "portablekit/hle_extension.hpp"

#include <cstdio>
#include <set>
#include <string>

namespace portablekit {
InterruptCall &test_last_queued_call() {
    static InterruptCall call;
    return call;
}
} // namespace portablekit

namespace portablekit::hle_extension {

void finish(AllegrexContext &ctx, std::uint32_t result) { ctx.set_gpr(2, result); }
void finish64(AllegrexContext &ctx, std::uint64_t result) {
    ctx.set_gpr(3, static_cast<std::uint32_t>(result >> 32u));
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));
}
std::uint64_t now_us() { return 0u; }
void log_once(std::string_view key, std::string_view message) {
    static std::set<std::string> seen;
    if (seen.insert(std::string(key)).second) std::printf("%.*s\n", static_cast<int>(message.size()), message.data());
}
const char *env(const char *) { return nullptr; }
void wait_host(AllegrexContext &ctx, std::optional<std::uint64_t>, HostPoll poll) {
    if (const auto result = poll(true)) ctx.set_gpr(2, *result);
}
void delay(AllegrexContext &ctx, std::uint64_t, std::uint32_t result) { ctx.set_gpr(2, result); }
// A call into the game loads the registers the kernel would and returns at
// once; the test reads them from ctx.
void call_guest(AllegrexContext &ctx, std::uint32_t function, std::span<const std::uint32_t> arguments,
                GuestReturn on_return) {
    const auto [packed, count] = portablekit::guest_call_arguments(arguments);
    portablekit::load_guest_call_arguments(ctx, packed, count);
    ctx.pc = function;
    on_return(ctx, 0u);
}
// A queued call is kept for the test to look at.
void queue_guest_call(std::uint32_t function, std::span<const std::uint32_t> arguments, QueuedReturn) {
    const auto [packed, count] = portablekit::guest_call_arguments(arguments);
    portablekit::InterruptCall &call = portablekit::test_last_queued_call();
    call.function = function;
    call.arguments = packed;
    call.argument_count = count;
}
std::int32_t current_thread_id() { return 0; }

} // namespace portablekit::hle_extension
