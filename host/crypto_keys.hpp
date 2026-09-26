#pragma once

// The console's fixed keys: the ones its crypto engine (KIRK) holds in key
// slots, and the save-data keys. The framework never names a key value outside
// the one place that supplies them, so a program can be built without any and
// read them from a file the player provides instead.
//
// Ports built today link crypto_keys_builtin.cpp, which supplies the published
// values as before. The desktop app (apps/portablekit) links its own
// definition, which reads the player's keys file and returns null without one.
// Everything that needs a key asks here and says clearly what is missing when
// the answer is null.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace portablekit {

using Key16 = std::array<std::uint8_t, 16>;

struct CryptoKeys {
    // AES keys of KIRK commands 4 and 7, by key slot ("seed"). The executable
    // header uses slot 0x5D; save data uses 0x03, 0x04, 0x0C, 0x0E, 0x10,
    // 0x12, 0x53, 0x57 and 0x64.
    std::map<std::uint8_t, Key16> kirk_aes;
    // The AES key of KIRK command 1 (signed and encrypted blocks), which wraps
    // an executable's payload key.
    std::optional<Key16> kirk_cmd1;
    // Save-data keys 2 to 7, in the order they are usually published.
    std::map<int, Key16> savedata;
    // What an executable's "~PSP" tag selects: a 16-byte key for the newer
    // header layout, or the 0x90-byte table for the older one. A port's
    // profile names its own release's key instead (GameProfile).
    struct TagKey {
        std::optional<Key16> key;
        std::vector<std::uint8_t> table;
    };
    std::map<std::uint32_t, TagKey> tags;

    [[nodiscard]] const Key16 *kirk(std::uint8_t slot) const {
        const auto it = kirk_aes.find(slot);
        return it != kirk_aes.end() ? &it->second : nullptr;
    }
    [[nodiscard]] const Key16 *savedata_key(int index) const {
        const auto it = savedata.find(index);
        return it != savedata.end() ? &it->second : nullptr;
    }
};

// The keys this program has; null when it has none. Defined exactly once:
// by crypto_keys_builtin.cpp, or by a program that loads them from a file.
[[nodiscard]] const CryptoKeys *crypto_keys();

// True when every key save-data encryption needs is present.
[[nodiscard]] bool savedata_keys_available();

} // namespace portablekit
