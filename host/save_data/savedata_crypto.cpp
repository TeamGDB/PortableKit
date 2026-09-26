#include "save_data/savedata_crypto.hpp"

#include "crypto_keys.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace portablekit::savedata {
namespace {

[[noreturn]] void missing_key(const char *what) {
    throw psprecomp::Error(std::string("Save-data encryption needs ") + what +
                           ", which is not in the keys this program has");
}

// KIRK command 4/7 keys by key seed, from crypto_keys().
const Aes128 &kirk_key(std::uint8_t seed) {
    static std::map<std::uint8_t, Aes128> ciphers;
    static std::mutex lock;
    const std::lock_guard<std::mutex> guard(lock);
    if (const auto it = ciphers.find(seed); it != ciphers.end()) return it->second;
    const CryptoKeys *keys = crypto_keys();
    const Key16 *key = keys != nullptr ? keys->kirk(seed) : nullptr;
    if (key == nullptr) missing_key("a KIRK key");
    return ciphers.emplace(seed, Aes128(*key)).first->second;
}

// Save-data keys 2 to 7, numbered in the order they are usually published
// (key 1 is not used by these modes).
const Block &sd_key(int index) {
    const CryptoKeys *keys = crypto_keys();
    const Key16 *key = keys != nullptr ? keys->savedata_key(index) : nullptr;
    if (key == nullptr) missing_key("a save-data key");
    return *key;
}

// What each mode uses. A zero mask stands for "no mask".
struct ModeParameters {
    std::uint8_t hash_seed;
    const Block *hash_mask;
    std::uint8_t header_seed;
    const Block *header_mask_in;   // applied before decrypting the header
    const Block *header_mask_out;  // applied after
    std::uint8_t stream_seed;
};

ModeParameters parameters(CryptMode mode) {
    switch (mode) {
    case CryptMode::Mode1: return {0x03, nullptr, 0x04, nullptr, nullptr, 0x53};
    case CryptMode::Mode3: return {0x0C, &sd_key(2), 0x0E, &sd_key(4), &sd_key(3), 0x57};
    case CryptMode::Mode5: break;
    }
    return {0x10, &sd_key(5), 0x12, &sd_key(7), &sd_key(6), 0x64};
}

Block masked(const Block &value, const Block *mask) { return mask != nullptr ? xor_blocks(value, *mask) : value; }

Block double_block(const Block &in) {
    Block out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>((in[i] << 1u) | (i + 1 < out.size() ? in[i + 1] >> 7u : 0u));
    if ((in[0] & 0x80u) != 0u) out[15] ^= 0x87u;
    return out;
}

Block read_block(std::span<const std::uint8_t> bytes, std::size_t offset) {
    Block out{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
    return out;
}

Block mode_hash(std::span<const std::uint8_t> data, CryptMode mode, const Block *game_key) {
    const ModeParameters p = parameters(mode);
    const Aes128 &cipher = kirk_key(p.hash_seed);
    Block hash = masked(cmac(cipher, data), p.hash_mask);
    if (game_key != nullptr) hash = cipher.encrypt(xor_blocks(hash, *game_key));
    return hash;
}

// XORs `data` in place with the key stream that follows `header`.
void apply_stream(std::span<std::uint8_t> data, const Block &header, CryptMode mode, const Block *game_key) {
    const ModeParameters p = parameters(mode);
    const Block context = game_key != nullptr ? xor_blocks(header, *game_key) : header;
    const Block prefix = masked(kirk_key(p.header_seed).decrypt(masked(context, p.header_mask_in)), p.header_mask_out);
    const Aes128 &stream = kirk_key(p.stream_seed);
    Block previous{};  // CBC chaining value: zero before the first counter block
    for (std::size_t offset = 0, counter = 1; offset < data.size(); offset += 16u, ++counter) {
        Block counter_block = prefix;
        for (int i = 0; i < 4; ++i) counter_block[12 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
        const Block key_stream = xor_blocks(stream.decrypt(counter_block), previous);
        previous = counter_block;
        for (std::size_t i = 0; i < 16u && offset + i < data.size(); ++i) data[offset + i] ^= key_stream[i];
    }
}

// The hashes in SAVEDATA_PARAMS are computed over the whole file with the
// later hashes still zero; the flag byte is set first.
Block sfo_hash(std::vector<std::uint8_t> sfo, CryptMode mode) { return mode_hash(sfo, mode, nullptr); }

} // namespace

bool is_zero(const Block &block) {
    return std::all_of(block.begin(), block.end(), [](std::uint8_t b) { return b == 0u; });
}

std::optional<CryptMode> mode_from_flags(std::uint8_t flags) {
    switch (flags) {
    case 0x01: return CryptMode::Mode1;
    case 0x21: return CryptMode::Mode3;
    case 0x41: return CryptMode::Mode5;
    default: return std::nullopt;
    }
}

std::uint8_t flags_for_mode(CryptMode mode) {
    switch (mode) {
    case CryptMode::Mode1: return 0x01u;
    case CryptMode::Mode3: return 0x21u;
    case CryptMode::Mode5: break;
    }
    return 0x41u;
}

Block cmac(const Aes128 &cipher, std::span<const std::uint8_t> data) {
    const Block k1 = double_block(cipher.encrypt(Block{}));
    const Block k2 = double_block(k1);
    const std::size_t blocks = data.empty() ? 1u : (data.size() + 15u) / 16u;
    const bool complete = !data.empty() && data.size() % 16u == 0u;
    Block chain{};
    for (std::size_t i = 0; i + 1 < blocks; ++i) chain = cipher.encrypt(xor_blocks(chain, read_block(data, 16u * i)));
    Block last{};
    const std::size_t tail = data.size() - 16u * (blocks - 1u);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(16u * (blocks - 1u)), tail, last.begin());
    if (complete) {
        last = xor_blocks(last, k1);
    } else {
        last[tail] = 0x80u;
        last = xor_blocks(last, k2);
    }
    return cipher.encrypt(xor_blocks(chain, last));
}

Block data_file_hash(std::span<const std::uint8_t> encrypted_file, CryptMode mode, const Block *game_key) {
    return mode_hash(encrypted_file, mode, mode == CryptMode::Mode1 ? nullptr : game_key);
}

std::vector<std::uint8_t> encrypt_data(std::span<const std::uint8_t> plain, CryptMode mode, const Block *game_key,
                                       const Block &random) {
    const ModeParameters p = parameters(mode);
    // The PSP draws 12 random bytes, clears the last four, and stores them
    // encrypted as the header; decryption undoes exactly this.
    Block seed = random;
    std::fill(seed.begin() + 12, seed.end(), std::uint8_t{0});
    const Block header = masked(kirk_key(p.header_seed).encrypt(masked(seed, p.header_mask_out)), p.header_mask_in);

    std::vector<std::uint8_t> out(kEncryptedHeaderSize + ((plain.size() + 15u) & ~std::size_t{15u}), 0u);
    std::copy(header.begin(), header.end(), out.begin());
    std::copy(plain.begin(), plain.end(), out.begin() + kEncryptedHeaderSize);
    apply_stream(std::span(out).subspan(kEncryptedHeaderSize), header, mode,
                 mode == CryptMode::Mode1 ? nullptr : game_key);
    return out;
}

std::optional<std::vector<std::uint8_t>> decrypt_data(std::span<const std::uint8_t> file, CryptMode mode,
                                                     const Block *game_key) {
    if (file.size() < kEncryptedHeaderSize) return std::nullopt;
    std::vector<std::uint8_t> plain(file.begin() + kEncryptedHeaderSize, file.end());
    apply_stream(plain, read_block(file, 0), mode, mode == CryptMode::Mode1 ? nullptr : game_key);
    return plain;
}

void sign_param_sfo(std::vector<std::uint8_t> &sfo, std::size_t params_offset, CryptMode mode) {
    std::uint8_t *params = sfo.data() + params_offset;
    std::fill_n(params, kParamsSize, std::uint8_t{0});
    // The hash at 0x20 needs a key unique to each PSP and cannot be made
    // here. A PSP does not check it when a save is copied from another
    // console, so it carries a filler value.
    std::fill_n(params + kParamsHashConsoleOffset, 16u, std::uint8_t{0x01});
    params[kParamsFlagsOffset] = flags_for_mode(mode);
    if (mode != CryptMode::Mode1) {
        const Block hash = sfo_hash(sfo, mode);
        std::copy(hash.begin(), hash.end(), sfo.begin() + static_cast<std::ptrdiff_t>(params_offset + kParamsHashModeOffset));
    }
    const Block hash1 = sfo_hash(sfo, CryptMode::Mode1);
    std::copy(hash1.begin(), hash1.end(), sfo.begin() + static_cast<std::ptrdiff_t>(params_offset + kParamsHashMode1Offset));
}

bool verify_param_sfo(std::span<const std::uint8_t> sfo, std::size_t params_offset) {
    if (params_offset + kParamsSize > sfo.size()) return false;
    const auto mode = mode_from_flags(sfo[params_offset + kParamsFlagsOffset]);
    if (!mode) return false;
    std::vector<std::uint8_t> copy(sfo.begin(), sfo.end());
    const auto clear = [&](std::size_t offset) {
        std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(params_offset + offset), 16, std::uint8_t{0});
    };
    clear(kParamsHashMode1Offset);
    if (sfo_hash(copy, CryptMode::Mode1) != read_block(sfo, params_offset + kParamsHashMode1Offset)) return false;
    if (*mode == CryptMode::Mode1) return true;
    clear(kParamsHashModeOffset);
    return sfo_hash(copy, *mode) == read_block(sfo, params_offset + kParamsHashModeOffset);
}

} // namespace portablekit::savedata

namespace portablekit {

bool savedata_keys_available() {
    const CryptoKeys *keys = crypto_keys();
    if (keys == nullptr) return false;
    for (const std::uint8_t slot : {0x03, 0x04, 0x0C, 0x0E, 0x10, 0x12, 0x53, 0x57, 0x64})
        if (keys->kirk(slot) == nullptr) return false;
    for (int index = 2; index <= 7; ++index)
        if (keys->savedata_key(index) == nullptr) return false;
    return true;
}

} // namespace portablekit
