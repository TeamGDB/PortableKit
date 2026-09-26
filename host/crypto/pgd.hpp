#pragma once

// PGD ("Protected Game Data"): the encrypted container some games keep large
// data files in (Phantasy Star Portable 2 Infinity's INSDIR/MEDIA.FPB). A game
// opens such a file with flag 0x40000000 and hands the library the file's key
// with sceIoIoctl(fd, 0x04100001, key, 16); from then on it reads and seeks
// in the decrypted data.
//
// The layout, little-endian, as public descriptions of the format give it
// (psdevwiki "PGD"):
//
//   0x00  "\0PGD"          0x04  key index      0x08  DRM type   0x0C  0
//   0x10  descKey (16)     0x20  file path MAC (16)
//   0x30  desc (0x30 bytes, encrypted with descKey and the version key):
//           dataKey (16), version (0), dataSize, blockSize (0x400),
//           dataOffset (0x90), padding (16)
//   0x60  table MAC        0x70  header MAC     0x80  header MAC (device key)
//   0x90  data: blocks of blockSize, encrypted with dataKey and the version key
//
// The "BB" cipher and MAC, as established against PSP2i's MEDIA.FPB (key
// index 1, DRM type 1) with `portablekit keys pgd-check`; `vkey` is the key
// the game passes to the ioctl:
//
//   prefix  = AES-decrypt[kirk.aes.39](key ^ vkey ^ amctrl.1CF4) ^ amctrl.1CE4
//   counter block n = prefix[0..11], then n as 32 bits little-endian
//   16 bytes at stream position n (from 0) are XORed with
//       AES-decrypt[kirk.aes.63](counter block n + 1) ^ counter block n
//   (counter block 0 counts as zero). The desc is the stream of descKey
//   from position 0; the data is the stream of dataKey, position = its
//   offset / 16, running on through the whole file.
//
//   header MAC at 0x70 = AES-encrypt[kirk.aes.38](CMAC[kirk.aes.38](bytes
//   0x00-0x6F) ^ vkey ^ amctrl.1CD4)
//
// Other key indices and DRM types are refused: none has been checked.
//
// No key value appears here: every key comes from the player's keys file
// (crypto_keys()), and a missing one is named.

#include "crypto_keys.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace portablekit::pgd {

using Block = std::array<std::uint8_t, 16>;

inline constexpr std::uint32_t kHeaderSize = 0x90u;
inline constexpr std::uint32_t kIoctlSetKey = 0x04100001u;
inline constexpr std::uint32_t kOpenFlag = 0x40000000u;

// Where the keys come from: KIRK AES key slots, and the DRM library's fixed
// keys, numbered here 1 to 3: amctrl.1CD4, amctrl.1CE4 and amctrl.1CF4 in the
// keys file. Null when the player's file does not have it.
struct KeySource {
    std::function<const Key16 *(std::uint8_t slot)> kirk;
    std::function<const Key16 *(int index)> fixed;
};

// The keys from crypto_keys().
[[nodiscard]] KeySource player_keys();

// XORs `data` with the key stream of (key, vkey) from 16-byte stream position
// `position`. Its own inverse. Empty on success, else the missing key's name.
[[nodiscard]] std::optional<std::string> bb_cipher(const KeySource &keys, const Block &key, const Block &vkey,
                                                   std::uint32_t position, std::span<std::uint8_t> data);

// The header MAC of `data` (a header's first 0x70 bytes) with the version key.
// Empty with `missing` set when a key is missing.
[[nodiscard]] std::optional<Block> header_mac(const KeySource &keys, std::span<const std::uint8_t> data,
                                              const Block &vkey, std::string &missing);

struct Desc {
    Block data_key{};
    std::uint32_t version{};
    std::uint32_t data_size{};
    std::uint32_t block_size{};
    std::uint32_t data_offset{};
};

// The desc as a PSP writes it: version 0, 0x400-byte blocks from 0x90, and a
// size that fits in `file_size`.
[[nodiscard]] bool desc_plausible(const Desc &desc, std::uint64_t file_size);

class PgdFile {
public:
    // Reads the header (the file's first 0x90 bytes) with the version key.
    // Empty with `error` set when the header is not PGD or of a kind not
    // supported, a key is missing, the MAC does not match, or it does not
    // decrypt to a valid desc.
    static std::optional<PgdFile> open(std::span<const std::uint8_t> header, const Block &vkey, std::uint64_t file_size,
                                       const KeySource &keys, std::string &error);

    [[nodiscard]] const Desc &desc() const noexcept { return desc_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return desc_.data_size; }

    // Decrypts up to `out.size()` bytes at plain offset `offset`, reading the
    // encrypted file through `read_raw(offset, out)` (which returns the bytes
    // read). Returns how many, fewer at the end.
    using RawReader = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out, const RawReader &read_raw);

    // A PGD made here, for tests: `plain` encrypted with `data_key` and
    // `vkey`, with a desc key and the header MAC.
    static std::vector<std::uint8_t> build(std::span<const std::uint8_t> plain, const Block &desc_key,
                                           const Block &data_key, const Block &vkey, const KeySource &keys);

private:
    Desc desc_;
    Block vkey_{};
    KeySource keys_;
    std::uint64_t cached_block_{~0ull};
    std::vector<std::uint8_t> cache_;
};

} // namespace portablekit::pgd
