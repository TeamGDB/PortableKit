// Prepares the game's executable from the encrypted EBOOT.BIN on the player's
// own disc. Written for this project from public descriptions of the PSP
// executable header ("~PSP") and of the PSP's AES-based crypto engine; see
// executable_preparation.hpp for what it accepts.
//
// The tag at 0xD0 selects a key; the framework knows the layout and the
// engine's fixed keys, and the profile supplies the key its release's tag
// selects (GameProfile::decryption_tag, and decryption_key or, for the older
// header layout, decryption_key_table).
//
// The file is an 0x150-byte header followed by the executable, encrypted with
// AES-128 in CBC mode under a per-file key. The header keeps that key wrapped
// twice: under the engine's fixed decryption key, and inside header fields that
// are encrypted and masked with material derived from the key its tag selects.

#include "install/executable_preparation.hpp"

#include "profile.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/sha256.hpp"

#include "tiny_aes/aes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace portablekit::install {
namespace {

using Block = std::array<std::uint8_t, 16>;

constexpr std::size_t kHeaderSize = 0x150u;

// Header fields used here.
constexpr std::size_t kMagicOffset = 0x00u;           // "~PSP"
constexpr std::size_t kAttributesOffset = 0x06u;      // bit 0 set: compressed
constexpr std::size_t kImageSizeOffset = 0x28u;       // size of the decrypted image
constexpr std::size_t kFileTypeOffset = 0x7Cu;        // 9: UMD game executable
constexpr std::size_t kPayloadSizeOffset = 0xB0u;     // size of the encrypted payload
constexpr std::size_t kTagOffset = 0xD0u;

constexpr std::uint8_t kUmdGameExecutable = 9u;

// Crypto-engine key slot 0x5D (plain AES decryption, command 7), which this tag
// uses for the header.
constexpr Block kHeaderKey = {0x11, 0x5A, 0x5D, 0x20, 0xD5, 0x3A, 0x8D, 0xD3,
                              0x9C, 0xC5, 0xAF, 0x41, 0x0F, 0x0F, 0x18, 0x6F};
// The engine's fixed key for signed and encrypted blocks (command 1), which
// wraps the payload key.
constexpr Block kPayloadWrappingKey = {0x98, 0xC9, 0x40, 0x97, 0x5C, 0x1D, 0x10, 0xE8,
                                       0x7F, 0xE6, 0x0E, 0xA3, 0xFD, 0x03, 0xA8, 0xBA};

std::uint32_t read_le32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(data[offset + 3u]) << 24u);
}

// AES-128-CBC decryption with a zero IV, in place. The length must be a
// multiple of the block size.
void cbc_decrypt(const Block &key, std::span<std::uint8_t> data) {
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, key.data(), zero_iv.data());
    AES_CBC_decrypt_buffer(&context, data.data(), data.size());
}

Block block_at(std::span<const std::uint8_t> data, std::size_t offset) {
    Block block{};
    std::memcpy(block.data(), data.data() + offset, block.size());
    return block;
}

void xor_into(Block &target, std::span<const std::uint8_t> mask) {
    for (std::size_t i = 0; i < target.size(); ++i) target[i] ^= mask[i];
}

void append(std::vector<std::uint8_t> &out, std::span<const std::uint8_t> header, std::size_t offset,
            std::size_t size) {
    out.insert(out.end(), header.begin() + static_cast<std::ptrdiff_t>(offset),
               header.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

// The tag key, repeated over nine blocks that each carry their own index in
// their first byte, decrypted with the header key.
std::vector<std::uint8_t> derive_mask() {
    const Block &tag_key = game().decryption_key;
    std::vector<std::uint8_t> mask;
    for (std::uint8_t index = 0; index < 9u; ++index) {
        Block block = tag_key;
        block[0] = index;
        mask.insert(mask.end(), block.begin(), block.end());
    }
    cbc_decrypt(kHeaderKey, mask);
    return mask;
}

// Recovers the payload key from the header.
Block unwrap_payload_key(std::span<const std::uint8_t> header) {
    // Four header fields form one 0x60-byte encrypted record. Decrypted, it
    // holds a check value and the header's hash, followed from offset 0x24 by
    // the payload key still wrapped under the fixed key and masked.
    std::vector<std::uint8_t> record;
    append(record, header, 0x140u, 0x10u);
    append(record, header, 0x12Cu, 0x14u);
    append(record, header, 0x80u, 0x30u);
    append(record, header, 0xC0u, 0x0Cu);
    cbc_decrypt(kHeaderKey, record);

    const std::vector<std::uint8_t> mask = derive_mask();
    Block key = block_at(record, 0x24u);
    xor_into(key, std::span(mask).subspan(0x10u, 16u));
    cbc_decrypt(kHeaderKey, key);
    xor_into(key, std::span(mask).subspan(0x50u, 16u));
    cbc_decrypt(kPayloadWrappingKey, key);
    return key;
}

// The older header layout, whose tag selects a 0x90-byte key table rather
// than a 16-byte key. Its fields are laid out differently, but the result is
// the same: the payload key, wrapped under the fixed key.
//
// Five header fields form one 0xA0-byte record, encrypted under the header key
// as a single chain: the last eight bytes of the header hash, the 0x28 bytes
// after it, then the key block, which is split between 0x110 and 0x80. The
// key block's first 0x70 bytes are masked on either side of a further
// decryption, with the table from offsets 0x14 and 0x20. CBC makes each block
// depend only on the one before it, so only the first block, which is the
// wrapped payload key, and the mode word need decrypting.
Block unwrap_payload_key_from_table(std::span<const std::uint8_t> header) {
    const std::span<const std::uint8_t> table = game().decryption_key_table;
    if (table.size() != 0x90u) throw psprecomp::Error("The profile's decryption key table is not 0x90 bytes long");

    std::vector<std::uint8_t> record;
    append(record, header, 0xE0u, 0x08u);
    append(record, header, 0xE8u, 0x28u);
    append(record, header, 0x110u, 0x40u);
    append(record, header, 0x80u, 0x30u);
    cbc_decrypt(kHeaderKey, record);

    // The key block starts at 0x30 in the record. Its first 0x70 bytes are
    // the part that is masked; decrypt it whole, since the mode word at 0x60
    // is the check that the table was the right one.
    std::vector<std::uint8_t> block(record.begin() + 0x30, record.begin() + 0xA0);
    for (std::size_t i = 0; i < block.size(); ++i) block[i] ^= table[0x14u + i];
    cbc_decrypt(kHeaderKey, block);
    for (std::size_t i = 0; i < block.size(); ++i) block[i] ^= table[0x20u + i];

    const std::uint32_t mode = read_le32(block, 0x60u);
    if (mode != 1u) throw psprecomp::Error("EBOOT.BIN did not decrypt with the profile's key table");
    Block key = block_at(block, 0x00u);
    cbc_decrypt(kPayloadWrappingKey, key);
    return key;
}

} // namespace

std::vector<std::uint8_t> prepare_executable(std::span<const std::uint8_t> eboot_bin,
                                             const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    if (psprecomp::sha256_bytes(eboot_bin) != game().encrypted_executable_sha256)
        throw psprecomp::Error("EBOOT.BIN is not the supported executable of " + std::string(game().game_title) + " (" +
                               game().disc_id_display + ")");

    // The hash already pins the file; these checks document the one layout
    // handled here.
    if (eboot_bin.size() < kHeaderSize || std::memcmp(eboot_bin.data() + kMagicOffset, "~PSP", 4u) != 0 ||
        read_le32(eboot_bin, kTagOffset) != game().decryption_tag || eboot_bin[kFileTypeOffset] != kUmdGameExecutable ||
        (eboot_bin[kAttributesOffset] & 1u) != 0u)
        throw psprecomp::Error("EBOOT.BIN has an unexpected header");
    const std::uint32_t size = read_le32(eboot_bin, kPayloadSizeOffset);
    const std::size_t padded_size = (static_cast<std::size_t>(size) + 15u) & ~std::size_t{15u};
    if (size == 0u || size != read_le32(eboot_bin, kImageSizeOffset) || padded_size > eboot_bin.size() - kHeaderSize)
        throw psprecomp::Error("EBOOT.BIN has an unexpected payload size");

    const Block key = game().decryption_key_table.empty() ? unwrap_payload_key(eboot_bin.first(kHeaderSize))
                                                          : unwrap_payload_key_from_table(eboot_bin.first(kHeaderSize));
    std::vector<std::uint8_t> executable(eboot_bin.begin() + kHeaderSize,
                                         eboot_bin.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + padded_size));
    // CBC carries its chaining value in the context, so the payload decrypts
    // in slices that report progress in between.
    constexpr std::size_t kSlice = 1024u * 1024u;
    const Block zero_iv{};
    AES_ctx context{};
    AES_init_ctx_iv(&context, key.data(), zero_iv.data());
    for (std::size_t offset = 0; offset < executable.size(); offset += kSlice) {
        const std::size_t length = std::min(kSlice, executable.size() - offset);
        AES_CBC_decrypt_buffer(&context, executable.data() + offset, length);
        if (progress) progress(offset + length, executable.size());
    }
    executable.resize(size);

    if (psprecomp::sha256_bytes(executable) != game().executable_sha256)
        throw psprecomp::Error("The prepared executable does not match the supported one");
    return executable;
}

} // namespace portablekit::install
