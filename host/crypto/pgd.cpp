#include "crypto/pgd.hpp"

#include "save_data/aes128.hpp"
#include "save_data/savedata_crypto.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

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

std::string CipherScheme::describe() const {
    char text[96];
    std::snprintf(text, sizeof(text), "h%02X-m%d-%c-s%02X-%c-%c-c%u-%c", header_slot, header_mask, vkey_after ? 'a' : 'b',
                  stream_slot, stream_encrypts ? 'e' : 'd', chained ? 'x' : 'n', counter_start,
                  block_counters_restart ? 'r' : 'f');
    return text;
}

std::vector<std::uint8_t> kirk_slots_used() { return {0x38u, 0x39u, 0x3Au, 0x63u}; }

std::vector<CipherScheme> candidate_schemes() {
    std::vector<CipherScheme> schemes;
    for (const std::uint8_t header : kirk_slots_used())
        for (int mask = 0; mask <= kFixedKeysUsed; ++mask)
            for (const bool after : {false, true})
                for (const std::uint8_t stream : kirk_slots_used())
                    for (const bool encrypts : {false, true})
                        for (const bool chained : {false, true})
                            for (const std::uint32_t start : {0u, 1u})
                                schemes.push_back(CipherScheme{header, mask, after, stream, encrypts, chained, start, false});
    return schemes;
}

CipherScheme active_scheme() {
    if (const char *text = std::getenv("PORTABLEKIT_PGD_SCHEME"); text != nullptr && *text != '\0') {
        for (CipherScheme scheme : candidate_schemes()) {
            for (const bool restart : {false, true}) {
                scheme.block_counters_restart = restart;
                if (scheme.describe() == text) return scheme;
            }
        }
    }
    // Not yet established against a real file: see the file comment.
    return CipherScheme{0x39u, 0, false, 0x39u, false, true, 1u, false};
}

std::optional<std::string> bb_cipher(const CipherScheme &scheme, const KeySource &keys, const Block &key,
                                     const Block &vkey, std::uint32_t seed, std::span<std::uint8_t> data) {
    const Key16 *header_key = keys.kirk ? keys.kirk(scheme.header_slot) : nullptr;
    if (header_key == nullptr) return slot_name(scheme.header_slot);
    const Key16 *stream_key = keys.kirk ? keys.kirk(scheme.stream_slot) : nullptr;
    if (stream_key == nullptr) return slot_name(scheme.stream_slot);
    Block context = key;
    if (scheme.header_mask != 0) {
        const Key16 *mask = keys.fixed ? keys.fixed(scheme.header_mask) : nullptr;
        if (mask == nullptr) return "amctrl." + std::to_string(scheme.header_mask);
        context = xor_block(context, *mask);
    }
    if (!scheme.vkey_after) context = xor_block(context, vkey);
    Block prefix = Aes128(*header_key).decrypt(context);
    if (scheme.vkey_after) prefix = xor_block(prefix, vkey);
    const Aes128 stream(*stream_key);
    Block previous{};
    for (std::size_t offset = 0, index = 0; offset < data.size(); offset += 16u, ++index) {
        Block counter = prefix;
        put32(counter, 12u, scheme.counter_start + seed + static_cast<std::uint32_t>(index));
        Block key_stream = scheme.stream_encrypts ? stream.encrypt(counter) : stream.decrypt(counter);
        if (scheme.chained) key_stream = xor_block(key_stream, previous);
        previous = counter;
        for (std::size_t i = 0; i < 16u && offset + i < data.size(); ++i) data[offset + i] ^= key_stream[i];
    }
    return std::nullopt;
}

std::optional<Block> bb_mac(const KeySource &keys, std::uint8_t slot, std::span<const std::uint8_t> data,
                            const Block &vkey) {
    const Key16 *key = keys.kirk ? keys.kirk(slot) : nullptr;
    if (key == nullptr) return std::nullopt;
    const Aes128 cipher(*key);
    return cipher.encrypt(xor_block(savedata::cmac(cipher, data), vkey));
}

bool desc_plausible(const Desc &desc, std::uint64_t file_size) {
    return desc.version == 0u && desc.block_size == 0x400u && desc.data_offset == kHeaderSize && desc.data_size != 0u &&
           desc.data_size <= file_size;
}

std::optional<PgdFile> PgdFile::open(std::span<const std::uint8_t> header, const Block &vkey, std::uint64_t file_size,
                                     const KeySource &keys, const CipherScheme &scheme,
                                     std::optional<std::uint8_t> mac_slot, std::string &error) {
    if (header.size() < kHeaderSize || header[0] != 0u || header[1] != 'P' || header[2] != 'G' || header[3] != 'D') {
        error = "not a PGD file";
        return std::nullopt;
    }
    if (mac_slot) {
        const auto mac = bb_mac(keys, *mac_slot, header.subspan(0, 0x80u), vkey);
        if (!mac) {
            error = "the keys file has no " + slot_name(*mac_slot);
            return std::nullopt;
        }
        if (*mac != read_block(header, 0x80u)) {
            error = "the header MAC does not match: a wrong key, or not this file's key";
            return std::nullopt;
        }
    }
    std::array<std::uint8_t, 0x30> desc_bytes{};
    std::copy_n(header.begin() + 0x30, desc_bytes.size(), desc_bytes.begin());
    if (const auto missing = bb_cipher(scheme, keys, read_block(header, 0x10u), vkey, 0u, desc_bytes)) {
        error = "the keys file has no " + *missing;
        return std::nullopt;
    }
    PgdFile file;
    std::copy_n(desc_bytes.begin(), 16, file.desc_.data_key.begin());
    file.desc_.version = le32(desc_bytes, 0x10u);
    file.desc_.data_size = le32(desc_bytes, 0x14u);
    file.desc_.block_size = le32(desc_bytes, 0x18u);
    file.desc_.data_offset = le32(desc_bytes, 0x1Cu);
    if (!desc_plausible(file.desc_, file_size)) {
        error = "the header does not decrypt to a valid description: a wrong key, or not this file's key";
        return std::nullopt;
    }
    file.vkey_ = vkey;
    file.keys_ = keys;
    file.scheme_ = scheme;
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
            const auto seed = scheme_.block_counters_restart ? 0u : static_cast<std::uint32_t>(block * block_size / 16u);
            if (bb_cipher(scheme_, keys_, desc_.data_key, vkey_, seed, cache_)) break;
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
                                         const Block &data_key, const Block &vkey, const KeySource &keys,
                                         const CipherScheme &scheme, std::uint8_t mac_slot) {
    constexpr std::uint32_t kBlock = 0x400u;
    const std::size_t blocks = (plain.size() + kBlock - 1u) / kBlock;
    std::vector<std::uint8_t> file(kHeaderSize + blocks * kBlock, 0u);
    file[1] = 'P';
    file[2] = 'G';
    file[3] = 'D';
    put32(file, 0x04u, 1u);
    put32(file, 0x08u, 1u);
    std::copy(desc_key.begin(), desc_key.end(), file.begin() + 0x10);
    std::span<std::uint8_t> desc(file.data() + 0x30, 0x30);
    std::copy(data_key.begin(), data_key.end(), desc.begin());
    put32(desc, 0x14u, static_cast<std::uint32_t>(plain.size()));
    put32(desc, 0x18u, kBlock);
    put32(desc, 0x1Cu, kHeaderSize);
    (void)bb_cipher(scheme, keys, desc_key, vkey, 0u, desc);
    std::copy(plain.begin(), plain.end(), file.begin() + kHeaderSize);
    for (std::size_t b = 0; b < blocks; ++b) {
        std::span<std::uint8_t> block(file.data() + kHeaderSize + b * kBlock, kBlock);
        const auto seed = scheme.block_counters_restart ? 0u : static_cast<std::uint32_t>(b * kBlock / 16u);
        (void)bb_cipher(scheme, keys, data_key, vkey, seed, block);
    }
    if (const auto mac = bb_mac(keys, mac_slot, std::span<const std::uint8_t>(file.data(), 0x80u), vkey))
        std::copy(mac->begin(), mac->end(), file.begin() + 0x80);
    return file;
}

} // namespace portablekit::pgd
