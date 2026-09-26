// The host's side of the HLE extension interface: what a module's handlers
// call (include/portablekit/hle_extension.hpp) and installing the modules the
// program was built with.

#include "../profile.hpp"
#include "hle_common.hpp"
#include "hle_extensions.hpp"

#include <iostream>

namespace portablekit {

namespace hle_extension {

void finish(AllegrexContext &ctx, std::uint32_t result) { kernel().finish(ctx, result); }
void finish64(AllegrexContext &ctx, std::uint64_t result) { kernel().finish64(ctx, result); }
std::uint64_t now_us() { return kernel().now_us(); }
void wait_host(AllegrexContext &ctx, std::optional<std::uint64_t> timeout_us, HostPoll poll) {
    kernel().wait_host(ctx, timeout_us, std::move(poll));
}
void delay(AllegrexContext &ctx, std::uint64_t microseconds, std::uint32_t result) {
    kernel().delay_current(ctx, microseconds, result);
}
void call_guest(AllegrexContext &ctx, std::uint32_t function, std::span<const std::uint32_t> arguments,
                GuestReturn on_return) {
    const auto [packed, count] = guest_call_arguments(arguments);
    kernel().call_guest(ctx, function, packed, std::move(on_return), count);
}
void queue_guest_call(std::uint32_t function, std::span<const std::uint32_t> arguments, QueuedReturn on_return) {
    const auto [packed, count] = guest_call_arguments(arguments);
    InterruptCall call{};
    call.function = function;
    call.arguments = packed;
    call.argument_count = count;
    if (on_return) call.on_return = std::move(on_return);
    kernel().queue_interrupt(std::move(call));
}
std::int32_t current_thread_id() { return kernel().in_interrupt() ? 0 : kernel().current_uid(); }
void log_once(std::string_view key, std::string_view message) {
    portablekit::log_once("hle-extension:" + std::string(key), std::string(message));
}
const char *env(const char *name) { return portablekit::env(name); }

} // namespace hle_extension

HleExtensionReport install_linked_hle_extensions(Runtime &runtime, HleRegistrar &hle) {
    const std::span<const HleExtensionModule> modules = linked_hle_extensions();
    if (modules.empty()) return {};
    print_hle_extension_modules(std::cout, modules);
    if (env_set("NO_HLE_EXTENSIONS")) {
        std::cout << "HLE extensions: " << modules.size() << (modules.size() == 1u ? " module" : " modules")
                  << " linked, ignored (" << env_name("NO_HLE_EXTENSIONS") << ")\n";
        return {.disabled = true, .linked = modules.size()};
    }
    const HleExtensionTarget target{
        .implemented = [&hle](const std::string &library, std::uint32_t nid) { return hle.bound(library, nid); },
        .bind = [&hle](const std::string &library, std::uint32_t nid, hle_extension::Handler handler) {
            hle.bind(library, nid, std::move(handler));
        },
        .current = [&hle](const std::string &library, std::uint32_t nid) { return hle.bound_function(library, nid); },
    };
    HleExtensionReport report = apply_hle_extensions(runtime, modules, target);
    print_hle_extension_summary(std::cout, report);
    return report;
}

} // namespace portablekit
