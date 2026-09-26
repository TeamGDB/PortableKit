#include "extension_keys.hpp"

#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace portablekit {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// Hex digits, spaces allowed between them; empty on anything else.
std::optional<std::vector<std::uint8_t>> parse_hex(std::string_view text) {
    std::string digits;
    for (const char c : text) {
        if (c == ' ' || c == '\t') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2u != 0u) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::strtoul(digits.substr(i, 2).c_str(), nullptr, 16)));
    return bytes;
}

// "kirk.aes.<hex slot>" -> slot.
std::optional<std::uint8_t> kirk_slot(std::string_view name) {
    constexpr std::string_view kPrefix = "kirk.aes.";
    if (!name.starts_with(kPrefix) || name.size() == kPrefix.size() || name.size() > kPrefix.size() + 2u)
        return std::nullopt;
    const std::string digits(name.substr(kPrefix.size()));
    char *end = nullptr;
    const unsigned long slot = std::strtoul(digits.c_str(), &end, 16);
    if (*end != '\0') return std::nullopt;
    return static_cast<std::uint8_t>(slot);
}

} // namespace

std::vector<DeclaredKey> collect_declared_keys(std::span<const HleExtensionModule> modules,
                                               std::vector<std::string> &notes) {
    std::vector<DeclaredKey> keys;
    for (const HleExtensionModule &module : modules) {
        hle_extension::Registry registry;
        module.entry(registry);
        for (const hle_extension::KeyDeclaration &declaration : registry.keys()) {
            DeclaredKey key{lower(trim(declaration.name)), lower(trim(declaration.sha256)), declaration.size,
                            module.title};
            if (key.name.empty() || key.size == 0u || key.name.starts_with("tag.")) {
                notes.push_back(std::string(module.title) + " declares a key \"" + declaration.name +
                                "\" that cannot be one; it is ignored.");
                continue;
            }
            if (const DeclaredKey *earlier = find_declared_key(keys, key.name)) {
                if (earlier->sha256 != key.sha256 || earlier->size != key.size)
                    notes.push_back(std::string(module.title) + " declares " + key.name + " differently from " +
                                    earlier->module + "; " + earlier->module + "'s declaration is used.");
                continue;
            }
            keys.push_back(std::move(key));
        }
    }
    return keys;
}

const DeclaredKey *find_declared_key(std::span<const DeclaredKey> keys, std::string_view name) {
    const std::string wanted = lower(name);
    for (const DeclaredKey &key : keys)
        if (key.name == wanted) return &key;
    return nullptr;
}

std::string check_declared_key(const DeclaredKey &key, std::span<const std::uint8_t> value,
                               std::string_view written_name) {
    if (value.size() != key.size)
        return std::string(written_name) + " must be " + std::to_string(key.size) + " bytes (" + key.module +
               " declares it).";
    if (!key.sha256.empty() && psprecomp::sha256_bytes(value) != key.sha256)
        return std::string(written_name) + " is not the right key: its fingerprint does not match (" + key.module +
               " declares it).";
    return {};
}

void store_named_key(CryptoKeys &keys, const std::string &name, std::vector<std::uint8_t> value) {
    if (const auto slot = kirk_slot(name); slot && value.size() == 16u) {
        Key16 key{};
        std::copy(value.begin(), value.end(), key.begin());
        keys.kirk_aes[*slot] = key;
    }
    keys.named[name] = std::move(value);
}

std::optional<std::vector<std::uint8_t>> key_value(const CryptoKeys *keys, std::string_view name) {
    if (keys == nullptr) return std::nullopt;
    const std::string wanted = lower(trim(name));
    const auto bytes = [](const Key16 &key) { return std::vector<std::uint8_t>(key.begin(), key.end()); };
    if (const auto found = keys->named.find(wanted); found != keys->named.end()) return found->second;
    if (const auto slot = kirk_slot(wanted)) {
        if (const Key16 *key = keys->kirk(*slot)) return bytes(*key);
        return std::nullopt;
    }
    if (wanted == "kirk.cmd1") {
        if (keys->kirk_cmd1) return bytes(*keys->kirk_cmd1);
        return std::nullopt;
    }
    if (wanted.starts_with("savedata.")) {
        char *end = nullptr;
        const long index = std::strtol(wanted.c_str() + 9, &end, 10);
        if (*end == '\0')
            if (const Key16 *key = keys->savedata_key(static_cast<int>(index))) return bytes(*key);
        return std::nullopt;
    }
    if (wanted.starts_with("tag.")) {
        char *end = nullptr;
        const unsigned long tag = std::strtoul(wanted.c_str() + 4, &end, 16);
        if (*end != '\0') return std::nullopt;
        const auto found = keys->tags.find(static_cast<std::uint32_t>(tag));
        if (found == keys->tags.end()) return std::nullopt;
        if (found->second.key) return bytes(*found->second.key);
        if (!found->second.table.empty()) return found->second.table;
    }
    return std::nullopt;
}

DeclaredKeysFile read_declared_keys_file(const std::filesystem::path &path, std::span<const DeclaredKey> declared) {
    DeclaredKeysFile file;
    file.path = path;
    std::ifstream in(path);
    if (!in) return file;
    file.found = true;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string written = trim(std::string_view(line).substr(0, equals));
        const DeclaredKey *key = find_declared_key(declared, written);
        if (key == nullptr) continue;
        const auto value = parse_hex(trim(std::string_view(line).substr(equals + 1)));
        if (!value) {
            file.problems.push_back(written + " is not written in hex.");
            continue;
        }
        if (std::string problem = check_declared_key(*key, *value, written); !problem.empty()) {
            file.problems.push_back(std::move(problem));
            continue;
        }
        store_named_key(file.keys, key->name, *value);
    }
    return file;
}

} // namespace portablekit
