#include "crypto/pgd.hpp"

#include "save_data/aes128.hpp"
#include "save_data/savedata_crypto.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace portablekit::pgd {
namespace {

using savedata::Aes128;

Block xor_block(const Block &a, const Block &b) {
    Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(a[i] ^ b[i]);
    return out;
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1u]) << 8u |
           static_cast<std::uint32_t>(bytes[at + 2u]) << 16u | static_cast<std::uint32_t>(bytes[at + 3u]) << 24u;
}

void put32(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

Block read_block(std::span<const std::uint8_t> bytes, std::size_t at) {
    Block block{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), block.size(), block.begin());
    return block;
}

std::string slot_name(std::uint8_t slot) {
    char text[16];
    std::snprintf(text, sizeof(text), "kirk.aes.%02X", slot);
    return text;
}

// The fixed keys (KeySource::fixed): amctrl.1CD4, 1CE4, 1CF4.
constexpr int kMacMask = 1;
constexpr int kMaskOut = 2;
constexpr int kMaskIn = 3;

const char *fixed_key_name(int index) {
    switch (index) {
    case kMacMask: return "amctrl.1CD4";
    case kMaskOut: return "amctrl.1CE4";
    case kMaskIn: return "amctrl.1CF4";
    default: return "amctrl.?";
    }
}

// The only kind checked against a real file (see pgd.hpp).
constexpr std::uint32_t kKeyIndex = 1u;
constexpr std::uint32_t kDrmType = 1u;
constexpr std::size_t kMacSpan = 0x70u;

} // namespace

KeySource player_keys() {
    return KeySource{[](std::uint8_t slot) -> const Key16 * {
                         const CryptoKeys *keys = crypto_keys();
                         return keys != nullptr ? keys->kirk(slot) : nullptr;
                     },
                     [](int index) -> const Key16 * {
                         const CryptoKeys *keys = crypto_keys();
                         if (keys == nullptr) return nullptr;
                         const auto it = keys->amctrl.find(index);
                         return it != keys->amctrl.end() ? &it->second : nullptr;
                     }};
}

std::optional<std::string> bb_cipher(const KeySource &keys, const Block &key, const Block &vkey,
                                     std::uint32_t position, std::span<std::uint8_t> data) {
    const Key16 *header_key = keys.kirk ? keys.kirk(0x39u) : nullptr;
    if (header_key == nullptr) return slot_name(0x39u);
    const Key16 *stream_key = keys.kirk ? keys.kirk(0x63u) : nullptr;
    if (stream_key == nullptr) return slot_name(0x63u);
    const Key16 *mask_in = keys.fixed ? keys.fixed(kMaskIn) : nullptr;
    if (mask_in == nullptr) return std::string(fixed_key_name(kMaskIn));
    const Key16 *mask_out = keys.fixed ? keys.fixed(kMaskOut) : nullptr;
    if (mask_out == nullptr) return std::string(fixed_key_name(kMaskOut));
    const Block prefix = xor_block(Aes128(*header_key).decrypt(xor_block(xor_block(key, vkey), *mask_in)), *mask_out);
    const auto counter_block = [&prefix](std::uint32_t n) {
        Block block = prefix;
        put32(block, 12u, n);
        return block;
    };
    const Aes128 stream(*stream_key);
    Block previous = position == 0u ? Block{} : counter_block(position);
    for (std::size_t offset = 0; offset < data.size(); offset += 16u) {
        ++position;
        const Block counter = counter_block(position);
        const Block key_stream = xor_block(stream.decrypt(counter), previous);
        previous = counter;
        for (std::size_t i = 0; i < 16u && offset + i < data.size(); ++i) data[offset + i] ^= key_stream[i];
    }
    return std::nullopt;
}

std::optional<Block> header_mac(const KeySource &keys, std::span<const std::uint8_t> data, const Block &vkey,
                                std::string &missing) {
    const Key16 *key = keys.kirk ? keys.kirk(0x38u) : nullptr;
    if (key == nullptr) {
        missing = slot_name(0x38u);
        return std::nullopt;
    }
    const Key16 *mask = keys.fixed ? keys.fixed(kMacMask) : nullptr;
    if (mask == nullptr) {
        missing = fixed_key_name(kMacMask);
        return std::nullopt;
    }
    const Aes128 cipher(*key);
    return cipher.encrypt(xor_block(xor_block(savedata::cmac(cipher, data), vkey), *mask));
}

bool desc_plausible(const Desc &desc, std::uint64_t file_size) {
    return desc.version == 0u && desc.block_size == 0x400u && desc.data_offset == kHeaderSize && desc.data_size != 0u &&
           desc.data_size <= file_size;
}

std::optional<PgdFile> PgdFile::open(std::span<const std::uint8_t> header, const Block &vkey, std::uint64_t file_size,
                                     const KeySource &keys, std::string &error) {
    if (header.size() < kHeaderSize || header[0] != 0u || header[1] != 'P' || header[2] != 'G' || header[3] != 'D') {
        error = "not a PGD file";
        return std::nullopt;
    }
    const std::uint32_t key_index = le32(header, 0x04u), drm_type = le32(header, 0x08u);
    if (key_index != kKeyIndex || drm_type != kDrmType) {
        error = "a kind of PGD not supported yet (key index " + std::to_string(key_index) + ", DRM type " +
                std::to_string(drm_type) + ")";
        return std::nullopt;
    }
    std::string missing;
    const auto mac = header_mac(keys, header.subspan(0, kMacSpan), vkey, missing);
    if (!mac) {
        error = "the keys file has no " + missing;
        return std::nullopt;
    }
    if (*mac != read_block(header, kMacSpan)) {
        error = "the header MAC does not match: a wrong key, or not this file's key";
        return std::nullopt;
    }
    std::array<std::uint8_t, 0x30> desc_bytes{};
    std::copy_n(header.begin() + 0x30, desc_bytes.size(), desc_bytes.begin());
    if (const auto absent = bb_cipher(keys, read_block(header, 0x10u), vkey, 0u, desc_bytes)) {
        error = "the keys file has no " + *absent;
        return std::nullopt;
    }
    PgdFile file;
    std::copy_n(desc_bytes.begin(), 16, file.desc_.data_key.begin());
    file.desc_.version = le32(desc_bytes, 0x10u);
    file.desc_.data_size = le32(desc_bytes, 0x14u);
    file.desc_.block_size = le32(desc_bytes, 0x18u);
    file.desc_.data_offset = le32(desc_bytes, 0x1Cu);
    if (!desc_plausible(file.desc_, file_size)) {
        error = "the header does not decrypt to a valid description";
        return std::nullopt;
    }
    file.vkey_ = vkey;
    file.keys_ = keys;
    return file;
}

std::size_t PgdFile::read(std::uint64_t offset, std::span<std::uint8_t> out, const RawReader &read_raw) {
    std::size_t done = 0;
    const std::uint32_t block_size = desc_.block_size;
    while (done < out.size() && offset + done < desc_.data_size) {
        const std::uint64_t position = offset + done;
        const std::uint64_t block = position / block_size;
        if (block != cached_block_) {
            cache_.assign(block_size, 0u);
            const std::size_t got = read_raw(desc_.data_offset + block * block_size, cache_);
            if (got == 0u) break;
            if (bb_cipher(keys_, desc_.data_key, vkey_, static_cast<std::uint32_t>(block * block_size / 16u), cache_))
                break;
            cached_block_ = block;
        }
        const std::size_t within = static_cast<std::size_t>(position % block_size);
        const std::size_t take = std::min<std::size_t>({out.size() - done, block_size - within,
                                                        static_cast<std::size_t>(desc_.data_size - position)});
        std::copy_n(cache_.begin() + static_cast<std::ptrdiff_t>(within), take, out.begin() + static_cast<std::ptrdiff_t>(done));
        done += take;
    }
    return done;
}

std::vector<std::uint8_t> PgdFile::build(std::span<const std::uint8_t> plain, const Block &desc_key,
                                         const Block &data_key, const Block &vkey, const KeySource &keys) {
    constexpr std::uint32_t kBlock = 0x400u;
    const std::size_t blocks = (plain.size() + kBlock - 1u) / kBlock;
    std::vector<std::uint8_t> file(kHeaderSize + blocks * kBlock, 0u);
    file[1] = 'P';
    file[2] = 'G';
    file[3] = 'D';
    put32(file, 0x04u, kKeyIndex);
    put32(file, 0x08u, kDrmType);
    std::copy(desc_key.begin(), desc_key.end(), file.begin() + 0x10);
    std::span<std::uint8_t> desc(file.data() + 0x30, 0x30);
    std::copy(data_key.begin(), data_key.end(), desc.begin());
    put32(desc, 0x14u, static_cast<std::uint32_t>(plain.size()));
    put32(desc, 0x18u, kBlock);
    put32(desc, 0x1Cu, kHeaderSize);
    (void)bb_cipher(keys, desc_key, vkey, 0u, desc);
    std::copy(plain.begin(), plain.end(), file.begin() + kHeaderSize);
    (void)bb_cipher(keys, data_key, vkey, 0u, std::span<std::uint8_t>(file.data() + kHeaderSize, blocks * kBlock));
    std::string missing;
    if (const auto mac = header_mac(keys, std::span<const std::uint8_t>(file.data(), kMacSpan), vkey, missing))
        std::copy(mac->begin(), mac->end(), file.begin() + kMacSpan);
    return file;
}

} // namespace portablekit::pgd
