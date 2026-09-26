#pragma once

#include "../profile.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace portablekit::install {

// Turns PSP_GAME/SYSDIR/EBOOT.BIN of the supported disc into the executable
// the recompiled code was generated from.
//
// This is deliberately not a general tool: it accepts only the one file whose
// SHA-256 is portablekit::game().encrypted_executable_sha256, handles only the header layout that
// file uses, and checks the result against portablekit::game().executable_sha256. Anything else is
// refused with psprecomp::Error. Only the installer calls it, with a callback
// that receives the bytes decrypted so far and the total.
[[nodiscard]] std::vector<std::uint8_t>
prepare_executable(std::span<const std::uint8_t> eboot_bin,
                   const std::function<void(std::uint64_t done, std::uint64_t total)> &progress = {});

// What an executable's "~PSP" tag selects: a 16-byte key for the newer header
// layout, or, when `table` is not empty, the 0x90-byte table of the older one.
struct TagKeyMaterial {
    std::uint32_t tag;
    std::array<std::uint8_t, 16> key;
    std::span<const std::uint8_t> table;
    // The crypto engine's key slot that decrypts the header: 0x5D for every
    // release supported so far.
    std::uint8_t kirk_slot = 0x5Du;
};

// The tag at 0xD0 of an encrypted executable; empty when the bytes are not one.
[[nodiscard]] std::optional<std::uint32_t> executable_tag(std::span<const std::uint8_t> eboot_bin);

// Decrypts a UMD game executable of the layout above with the given tag
// material and the console's fixed keys (crypto_keys()), with no check of which
// game it is. prepare_executable() is this plus the profile's hashes; a
// program that identifies the game from its disc calls this directly.
[[nodiscard]] std::vector<std::uint8_t>
decrypt_executable(std::span<const std::uint8_t> eboot_bin, const TagKeyMaterial &tag,
                   const std::function<void(std::uint64_t done, std::uint64_t total)> &progress = {});

} // namespace portablekit::install
