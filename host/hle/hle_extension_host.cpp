// The host's side of the HLE extension interface: what a module's handlers
// call (include/portablekit/hle_extension.hpp) and installing the modules the
// program was built with.

#include "../profile.hpp"
#include "extension_keys.hpp"
#include "hle_common.hpp"
#include "hle_extensions.hpp"
#include "install/user_data.hpp"

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

namespace {

// A port's keys file for declared keys: <prefix>_KEYS, or keys.txt in the
// data folder (<prefix>_DATA_DIR, else the port's own). Read once.
const DeclaredKeysFile &port_declared_keys() {
    static const DeclaredKeysFile file = [] {
        const std::vector<DeclaredKey> &declared = declared_extension_keys();
        if (declared.empty()) return DeclaredKeysFile{};
        std::filesystem::path path;
        if (const char *keys = env("KEYS"); keys != nullptr && *keys != '\0') path = install::path_from_utf8(keys);
        else if (const char *data = env("DATA_DIR"); data != nullptr && *data != '\0')
            path = install::path_from_utf8(data) / "keys.txt";
        else path = install::user_data_directory() / "keys.txt";
        return read_declared_keys_file(path, declared);
    }();
    return file;
}

// The program's keys: the player's keys file in the desktop app; compiled-in
// keys and the port's own keys file for declared ones in a port.
std::optional<std::vector<std::uint8_t>> find_key(std::string_view name) {
    if (auto value = key_value(crypto_keys(), name)) return value;
    if (keys_come_from_keys_file()) return std::nullopt;
    return key_value(&port_declared_keys().keys, name);
}

} // namespace

const std::vector<DeclaredKey> &declared_extension_keys() {
    static const std::vector<DeclaredKey> keys = [] {
        std::vector<std::string> notes;
        std::vector<DeclaredKey> collected = collect_declared_keys(linked_hle_extensions(), notes);
        for (const std::string &note : notes) std::cerr << "[hle-extension] " << note << "\n";
        return collected;
    }();
    return keys;
}

namespace hle_extension {

std::optional<std::vector<std::uint8_t>> key_bytes(std::string_view name) { return find_key(name); }

std::optional<Key16> key(std::string_view name) {
    const auto value = find_key(name);
    if (!value || value->size() != 16u) return std::nullopt;
    Key16 out{};
    std::copy(value->begin(), value->end(), out.begin());
    return out;
}

std::optional<Key16> kirk_key(std::uint8_t slot) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const std::string name = std::string("kirk.aes.") + kDigits[slot >> 4u] + kDigits[slot & 0xFu];
    return key(name);
}

std::size_t read_open_file(std::uint32_t fd, std::uint64_t offset, std::span<std::uint8_t> output) {
    return ::portablekit::read_open_file(fd, offset, output.data(), output.size());
}

bool set_file_filter(std::uint32_t fd, std::shared_ptr<FileFilter> filter) {
    return set_open_file_filter(fd, std::move(filter));
}

} // namespace hle_extension

HleExtensionReport install_linked_hle_extensions(Runtime &runtime, HleRegistrar &hle) {
    const std::span<const HleExtensionModule> modules = linked_hle_extensions();
    if (modules.empty()) return {};
    print_hle_extension_modules(std::cout, modules);
    // Declared keys: what a port found in its keys file (the desktop app says
    // it with `portablekit keys status`).
    if (const std::vector<DeclaredKey> &declared = declared_extension_keys(); !declared.empty()) {
        std::size_t present = 0u;
        for (const DeclaredKey &key : declared)
            if (find_key(key.name)) ++present;
        std::cout << "HLE extension keys: " << present << " of " << declared.size() << " declared keys present";
        if (!keys_come_from_keys_file()) {
            const DeclaredKeysFile &file = port_declared_keys();
            std::cout << " (" << install::path_to_utf8(file.path) << (file.found ? "" : ", not found") << ")";
            std::cout << "\n";
            for (const std::string &problem : file.problems) std::cout << "  problem: " << problem << "\n";
        } else {
            std::cout << " (portablekit keys status lists them)\n";
        }
    }
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
