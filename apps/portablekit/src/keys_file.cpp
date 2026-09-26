#include "keys_file.hpp"

#include "app_home.hpp"
#include "text_format.hpp"

#include "psprecomp/sha256.hpp"

#include <cstdlib>
#include <mutex>

namespace portablekit::app {
namespace {

// SHA-256 of each fixed key's 16 bytes: enough to tell a right key from a
// wrong one, and nothing more.
struct Fingerprint {
    const char *name;
    const char *sha256;
};
constexpr Fingerprint kFingerprints[] = {
    {"kirk.aes.5D", "b0e12026cdd9ff8b821a3c969116c839a65bac42f5daae3d5dcb34814a7f4d31"},
    {"kirk.aes.03", "4c7a2139d654b4f824f6f4ad8ceabe64ef6fcd820d8e5fdfb4f6333ee299e718"},
    {"kirk.aes.04", "cde0d9b867277dbc3e036eff35a8db4df4613392007f50a4907fa466f3e0ff26"},
    {"kirk.aes.0C", "1ba572cec5e2b8269704f2f7d2d5d7a0275e351d1085e961c191033632cc3ab1"},
    {"kirk.aes.0E", "a2bd9b8c819fc4bbfa58b804545a2df57fa0a961ab39fd11eb819b04536d93be"},
    {"kirk.aes.10", "127858b601267f1a9c2ae63fcc537b2caf68d692f3e11901a65f88e09f4c7ac4"},
    {"kirk.aes.12", "22e830fa0ac9b99cfd0706c7d907429f88f1de4b3605088d09e76c2ece69133f"},
    {"kirk.aes.53", "41250b98cc0f4fafd8bce7e0d81805eef7548f4300aac5f0e0a73c041d5d4fcd"},
    {"kirk.aes.57", "1f432ad17b09c699d536edbe334890d6b07012d8660f47aea3d94e6f3cde9485"},
    {"kirk.aes.64", "5855111a412d0aa6cf441adc1a80ea8db8a3fd8ba876e409b2dc10745775eeee"},
    {"kirk.cmd1", "cc8df6fadcd994f857a96c76e1e88d520f5ee5964a5d61b09946d21f6add0421"},
    {"savedata.2", "8919ede20cc185a3f1c2fd86eaa83d9df7076d73674363e07c14d84a4b337859"},
    {"savedata.3", "572076765f0929b07325e65d12b3dbcf849e35b9ab0e253bdc1647adaacaca9a"},
    {"savedata.4", "942864b793d1c6df20378c22ccc65db5023716cbb9e9adb067c8f1e25b02136c"},
    {"savedata.5", "9f7bfa1f5136d4dfdd3a8322578dbbe5243bf4eb70428b101f27eb1144027461"},
    {"savedata.6", "b58242561b16a8925126dbae5558dda53d6c275d712ee69f5bebce37c577af2d"},
    {"savedata.7", "a0bf478c1471e4115011e5e0f70d5e2d43c95e5ef2645d164b76ec3cd118ec10"},
};

// The keys the PGD format needs (host/crypto/pgd.hpp). Their fingerprints are
// added once they have been checked against a real PGD file; until then any
// 16-byte value is taken under these names, and a wrong one shows as a PGD
// header that does not decrypt.
bool pgd_key_name(const std::string &name) {
    return name == "kirk.aes.38" || name == "kirk.aes.39" || name == "kirk.aes.3a" || name == "kirk.aes.63" ||
           name == "amctrl.1" || name == "amctrl.2" || name == "amctrl.3";
}

const char *expected_fingerprint(const std::string &name) {
    for (const Fingerprint &entry : kFingerprints)
        if (lower(name) == lower(entry.name)) return entry.sha256;
    return nullptr;
}

std::string sha256_of(const std::vector<std::uint8_t> &bytes) { return psprecomp::sha256_bytes(bytes); }

Key16 to_key(const std::vector<std::uint8_t> &bytes) {
    Key16 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = bytes[i];
    return key;
}

} // namespace

KeysReport read_keys_file(const std::filesystem::path &path) {
    KeysReport report;
    report.path = path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return report;
    report.file_found = true;
    const KeyValues values = read_key_values(path);
    for (const auto &[raw_name, value] : values) {
        const std::string name = lower(raw_name);
        if (name.ends_with(".slot")) continue;  // read with its tag
        const auto bytes = from_hex(value);
        if (!bytes) {
            report.problems.push_back(raw_name + " is not written in hex.");
            continue;
        }
        if (name.starts_with("tag.")) {
            const std::string tag_text = name.substr(4);
            char *end = nullptr;
            const unsigned long tag = std::strtoul(tag_text.c_str(), &end, 16);
            if (tag_text.empty() || *end != '\0') {
                report.problems.push_back(raw_name + " does not name a tag in hex.");
                continue;
            }
            CryptoKeys::TagKey entry;
            if (bytes->size() == 16u) entry.key = to_key(*bytes);
            else if (bytes->size() == 0x90u) entry.table = *bytes;
            else {
                report.problems.push_back(raw_name + " must be 16 bytes or a 0x90-byte table.");
                continue;
            }
            report.keys.tags[static_cast<std::uint32_t>(tag)] = std::move(entry);
            continue;
        }
        if (bytes->size() != 16u) {
            report.problems.push_back(raw_name + " must be 16 bytes.");
            continue;
        }
        const char *expected = expected_fingerprint(name);
        if (expected == nullptr && !pgd_key_name(name)) {
            report.problems.push_back(raw_name + " is not a key this program uses.");
            continue;
        }
        if (expected != nullptr && sha256_of(*bytes) != expected) {
            report.problems.push_back(raw_name + " is not the right key: its fingerprint does not match.");
            continue;
        }
        if (name.starts_with("kirk.aes.")) {
            const unsigned long slot = std::strtoul(name.substr(9).c_str(), nullptr, 16);
            report.keys.kirk_aes[static_cast<std::uint8_t>(slot)] = to_key(*bytes);
        } else if (name == "kirk.cmd1") {
            report.keys.kirk_cmd1 = to_key(*bytes);
        } else if (name.starts_with("savedata.")) {
            report.keys.savedata[std::atoi(name.substr(9).c_str())] = to_key(*bytes);
        } else if (name.starts_with("amctrl.")) {
            report.keys.amctrl[std::atoi(name.substr(7).c_str())] = to_key(*bytes);
        }
    }
    report.tag_count = report.keys.tags.size();
    report.can_decrypt_executables = report.keys.kirk(0x5Du) != nullptr && report.keys.kirk_cmd1.has_value();
    report.can_encrypt_saves = true;
    for (const std::uint8_t slot : {0x03, 0x04, 0x0C, 0x0E, 0x10, 0x12, 0x53, 0x57, 0x64})
        if (report.keys.kirk(slot) == nullptr) report.can_encrypt_saves = false;
    for (int index = 2; index <= 7; ++index)
        if (report.keys.savedata_key(index) == nullptr) report.can_encrypt_saves = false;
    report.can_decrypt_pgd = true;
    for (const std::uint8_t slot : {0x38, 0x39, 0x3A, 0x63})
        if (report.keys.kirk(slot) == nullptr) report.can_decrypt_pgd = false;
    for (int index = 1; index <= 3; ++index)
        if (report.keys.amctrl.find(index) == report.keys.amctrl.end()) report.can_decrypt_pgd = false;
    return report;
}

std::filesystem::path active_keys_file() {
    if (const char *path = std::getenv("PORTABLEKIT_KEYS"); path != nullptr && *path != '\0') return path;
    return keys_file_path();
}

const KeysReport &active_keys() {
    static const KeysReport report = read_keys_file(active_keys_file());
    return report;
}

bool import_keys_file(const std::filesystem::path &source, KeysReport &report) {
    report = read_keys_file(source);
    if (!report.file_found) {
        report.problems.insert(report.problems.begin(), "There is no file at " + path_text(source) + ".");
        return false;
    }
    if (!report.problems.empty()) return false;
    if (report.keys.kirk_aes.empty() && !report.keys.kirk_cmd1 && report.keys.savedata.empty() &&
        report.keys.tags.empty() && report.keys.amctrl.empty()) {
        report.problems.push_back("The file holds no keys.");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(keys_file_path().parent_path(), ec);
    std::filesystem::copy_file(source, keys_file_path(), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        report.problems.push_back("Cannot copy it to " + path_text(keys_file_path()) + ": " + ec.message());
        return false;
    }
    return true;
}

} // namespace portablekit::app

namespace portablekit {

// The framework asks here for every key it uses (host/crypto_keys.hpp).
const CryptoKeys *crypto_keys() {
    const app::KeysReport &report = app::active_keys();
    if (report.keys.kirk_aes.empty() && !report.keys.kirk_cmd1 && report.keys.savedata.empty() &&
        report.keys.tags.empty() && report.keys.amctrl.empty())
        return nullptr;
    return &report.keys;
}

} // namespace portablekit
