#pragma once

// Keys HLE extension modules declare (Registry::declare_key in
// include/portablekit/hle_extension.hpp): which names they are, checking a
// value against a declaration, and looking any key up by its keys-file name.
// The desktop app's keys file reader and a port's optional keys file both use
// this; see docs/HLE_EXTENSIONS.md, "Keys".

#include "crypto_keys.hpp"
#include "hle/hle_extensions.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portablekit {

struct DeclaredKey {
    std::string name;    // lower case
    std::string sha256;  // lower case; empty: not checked
    std::size_t size{};
    std::string module;  // the module's title
};

// The keys `modules` declare, calling each module's entry into a registry of
// its own (an entry only adds to its registry, so calling it again is
// harmless). A name two modules declare differently keeps the first
// declaration, and `notes` says so.
[[nodiscard]] std::vector<DeclaredKey> collect_declared_keys(std::span<const HleExtensionModule> modules,
                                                             std::vector<std::string> &notes);
// Those of the modules this program links (linked_hle_extensions()), once.
[[nodiscard]] const std::vector<DeclaredKey> &declared_extension_keys();

[[nodiscard]] const DeclaredKey *find_declared_key(std::span<const DeclaredKey> keys, std::string_view name);
// Empty when `value` is what `key` declares; otherwise one sentence about
// `written_name` (the name as the file spells it).
[[nodiscard]] std::string check_declared_key(const DeclaredKey &key, std::span<const std::uint8_t> value,
                                             std::string_view written_name);
// Keeps `value` under `name` (lower case) in keys.named, and a 16-byte
// "kirk.aes.<slot>" also in keys.kirk_aes.
void store_named_key(CryptoKeys &keys, const std::string &name, std::vector<std::uint8_t> value);
// A key by its keys-file name, from what `keys` holds: kirk.aes.<slot>,
// kirk.cmd1, savedata.<n>, tag.<tag> (its key or table) and keys.named.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> key_value(const CryptoKeys *keys, std::string_view name);

// A keys file read for declared keys only, as a port reads one: names nothing
// declares are left alone (PortableKit's own keys are compiled into a port).
struct DeclaredKeysFile {
    std::filesystem::path path;
    bool found{};
    CryptoKeys keys;                    // keys.named (and kirk_aes) only
    std::vector<std::string> problems;  // one sentence each
};
[[nodiscard]] DeclaredKeysFile read_declared_keys_file(const std::filesystem::path &path,
                                                       std::span<const DeclaredKey> declared);

} // namespace portablekit
