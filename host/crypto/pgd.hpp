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
//   0x60  table MAC        0x70  header MAC (device key)   0x80  header MAC (fixed key)
//   0x90  data: blocks of blockSize, each encrypted with dataKey and the
//         version key, with a block-number counter
//
// The cipher is the console's "BB" cipher: a key stream of AES blocks under
// a KIRK key slot, from a prefix made by decrypting the header key; the MAC
// is a CMAC under a KIRK key slot. Which slots, masks and counter conventions
// a PSP uses are not given by the public descriptions, so they are
// parameters here (CipherScheme). The one the app uses is the one that turns
// a real PGD header's desc into its known values (version 0, block size
// 0x400, data offset 0x90); `portablekit keys pgd-check <file>` finds it.
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

// Where the keys come from: KIRK AES key slots, and the fixed "amctrl" keys
// (1 to 3). Null when the player's file does not have it.
struct KeySource {
    std::function<const Key16 *(std::uint8_t slot)> kirk;
    std::function<const Key16 *(int index)> fixed;
};

// The keys from crypto_keys().
[[nodiscard]] KeySource player_keys();

// One way of running the BB cipher; see the file comment.
struct CipherScheme {
    std::uint8_t header_slot{};     // KIRK slot that decrypts the header key into the stream prefix
    int header_mask{};              // amctrl key XORed into the header key first (0: none)
    bool vkey_after{};              // the version key XORed into the decrypted prefix, else into the header key
    std::uint8_t stream_slot{};     // KIRK slot of the key stream
    bool stream_encrypts{};         // key stream = AES-encrypt(counter block), else AES-decrypt
    bool chained{};                 // each key stream block is also XORed with the previous counter block
    std::uint32_t counter_start{};  // counter of the first 16 bytes of a run
    bool block_counters_restart{};  // each data block's counter starts afresh, else runs on through the file

    [[nodiscard]] std::string describe() const;
};

// The slots and masks every scheme may use: what `keys status` checks.
[[nodiscard]] std::vector<std::uint8_t> kirk_slots_used();
inline constexpr int kFixedKeysUsed = 3;

// Every scheme `pgd-check` tries.
[[nodiscard]] std::vector<CipherScheme> candidate_schemes();

// The scheme the app decrypts with: PORTABLEKIT_PGD_SCHEME (as `describe`
// prints it) or the built-in default.
[[nodiscard]] CipherScheme active_scheme();

// XORs `data` with the key stream of (key, vkey) starting at 16-byte
// counter `seed`. Its own inverse. Empty on success, else the missing key.
[[nodiscard]] std::optional<std::string> bb_cipher(const CipherScheme &scheme, const KeySource &keys, const Block &key,
                                                   const Block &vkey, std::uint32_t seed, std::span<std::uint8_t> data);

// The BB MAC of `data` under KIRK slot `slot`, finished with the version key:
// CMAC, XOR vkey, encrypt once more.
[[nodiscard]] std::optional<Block> bb_mac(const KeySource &keys, std::uint8_t slot, std::span<const std::uint8_t> data,
                                          const Block &vkey);

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
    // `mac_slot` set: the header MAC at 0x80 must match (the MAC check is
    // off until the slot is known). Empty with `error` set when the header
    // is not PGD, a key is missing, or it does not decrypt to a valid desc.
    static std::optional<PgdFile> open(std::span<const std::uint8_t> header, const Block &vkey, std::uint64_t file_size,
                                       const KeySource &keys, const CipherScheme &scheme,
                                       std::optional<std::uint8_t> mac_slot, std::string &error);

    [[nodiscard]] const Desc &desc() const noexcept { return desc_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return desc_.data_size; }

    // Decrypts `count` bytes at plain offset `offset`, reading the encrypted
    // file through `read_raw(offset, out)` (which returns the bytes read).
    using RawReader = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> out, const RawReader &read_raw);

    // A PGD made here, for tests: `plain` encrypted with `data_key` and
    // `vkey` under `scheme`, with a desc key and the MAC at 0x80 under
    // `mac_slot`.
    static std::vector<std::uint8_t> build(std::span<const std::uint8_t> plain, const Block &desc_key,
                                           const Block &data_key, const Block &vkey, const KeySource &keys,
                                           const CipherScheme &scheme, std::uint8_t mac_slot);

private:
    Desc desc_;
    Block vkey_{};
    KeySource keys_;
    CipherScheme scheme_;
    std::uint64_t cached_block_{~0ull};
    std::vector<std::uint8_t> cache_;
};

} // namespace portablekit::pgd
