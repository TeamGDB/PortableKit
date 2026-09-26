#include "keys_file.hpp"

#include "app_home.hpp"
#include "text_format.hpp"

#include "extension_keys.hpp"
#include "psprecomp/sha256.hpp"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string_view>

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
    {"kirk.aes.38", "b5e2a84dbaaf99015874182a8609494d3c8e38f1837755ee29858c78b986ad17"},
    {"kirk.aes.39", "be3798d2e2a46c0018a4f7e41f5557e6cc2aeb64b0b15ae6b7063a8fa41c8559"},
    {"kirk.aes.63", "16df5079b5e31b07045b9d117c9db8bf9c45e606dbe4a9c71bb763f12d61be83"},
    {"amctrl.1CD4", "dbe5dd77c4a5f3b930b30f520ae71064a4b48161ba43d79252351effc0ecb403"},
    {"amctrl.1CE4", "4aae0892755232421ac3077d844110b0c0334eeadc9cef6093e55a3fb8e56ee4"},
    {"amctrl.1CF4", "e0942366cf4a2142ac1369df3d404198ac8d9bf9df73ccde6e540f2554dbcf4c"},
    {"amctrl.dnas.1A90", "3f238f8d5e24ce9402f00be5bb869155d83bcf05457948faa36822a20af04057"},
    {"amctrl.dnas.1AA0", "47d6a491c1dc0424e79d60b28fd7f9fc9fb6f7be0552cf4e77dfdcbea3260869"},
};

// The keys the PGD format needs (host/crypto/pgd.hpp), with fingerprints
// above. The DNAS ones are taken but not used yet: no PGD file of that kind
// has been checked.
int amctrl_index(const std::string &name) {
    if (name == "amctrl.1cd4") return 1;
    if (name == "amctrl.1ce4") return 2;
    if (name == "amctrl.1cf4") return 3;
    if (name == "amctrl.dnas.1a90") return 4;
    if (name == "amctrl.dnas.1aa0") return 5;
    return 0;
}

bool pgd_key_name(const std::string &name) {
    return name == "kirk.aes.38" || name == "kirk.aes.39" || name == "kirk.aes.63" ||
           amctrl_index(name) != 0;
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

// What the reader says of a name it does not use, and what that becomes once
// HLE extension modules had their say: nothing for a name one declares, a
// warning for the rest (another program's keys may share the file).
constexpr std::string_view kNotUsed = " is not a key this program uses.";

void sort_out_unused_names(KeysReport &report, const std::set<std::string> &declared_found) {
    std::vector<std::string> kept;
    for (std::string &problem : report.problems) {
        if (!problem.ends_with(kNotUsed)) {
            kept.push_back(std::move(problem));
            continue;
        }
        const std::string written = problem.substr(0, problem.size() - kNotUsed.size());
        if (declared_found.contains(lower(written))) continue;
        report.warnings.push_back(written + " is not a key this program or its extension modules use; it is ignored.");
    }
    report.problems = std::move(kept);
}

// The keys HLE extension modules declare, and whether the file gave them.
void list_declared(KeysReport &report) {
    for (const DeclaredKey &key : declared_extension_keys())
        report.declared.push_back({key.name, key.module, key_value(&report.keys, key.name).has_value()});
}

// Copies a checked keys file to <home>/keys.txt.
bool copy_to_home(const std::filesystem::path &source, KeysReport &report) {
    std::error_code ec;
    std::filesystem::create_directories(keys_file_path().parent_path(), ec);
    std::filesystem::copy_file(source, keys_file_path(), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        report.problems.push_back("Cannot copy it to " + path_text(keys_file_path()) + ": " + ec.message());
        return false;
    }
    return true;
}

} // namespace

KeysReport read_keys_file(const std::filesystem::path &path) {
    KeysReport report;
    report.path = path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        list_declared(report);
        return report;
    }
    report.file_found = true;
    std::set<std::string> declared_found;  // names a module declares, kept as its keys
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
        // A key an HLE extension module declares (extension_keys.hpp), unless
        // it is one this program checks itself: that check stays.
        if (const DeclaredKey *declared = find_declared_key(declared_extension_keys(), name);
            declared != nullptr && expected_fingerprint(name) == nullptr) {
            if (std::string problem = check_declared_key(*declared, *bytes, raw_name); !problem.empty()) {
                report.problems.push_back(std::move(problem));
                continue;
            }
            store_named_key(report.keys, name, *bytes);
            declared_found.insert(name);
            // A 16-byte one goes on, so a name this program also reads in its
            // own way is kept that way as well.
            if (bytes->size() != 16u) continue;
        } else if (bytes->size() != 16u && expected_fingerprint(name) == nullptr && !name.starts_with("kirk.") &&
                   !name.starts_with("savedata.")) {
            // Not a name anything here reads: a warning, whatever its length.
            report.warnings.push_back(raw_name + " is not a key this program or its extension modules use; it is ignored.");
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
        } else if (const int index = amctrl_index(name); index != 0) {
            report.keys.amctrl[index] = to_key(*bytes);
        }
    }
    sort_out_unused_names(report, declared_found);
    list_declared(report);
    report.tag_count = report.keys.tags.size();
    report.can_decrypt_executables = report.keys.kirk(0x5Du) != nullptr && report.keys.kirk_cmd1.has_value();
    report.can_encrypt_saves = true;
    for (const std::uint8_t slot : {0x03, 0x04, 0x0C, 0x0E, 0x10, 0x12, 0x53, 0x57, 0x64})
        if (report.keys.kirk(slot) == nullptr) report.can_encrypt_saves = false;
    for (int index = 2; index <= 7; ++index)
        if (report.keys.savedata_key(index) == nullptr) report.can_encrypt_saves = false;
    report.can_decrypt_pgd = true;
    for (const std::uint8_t slot : {0x38, 0x39, 0x63})
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
    if (!report.keys.named.empty()) return copy_to_home(source, report);  // keys only modules use
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
    if (!report.keys.named.empty()) return &report.keys;
    if (report.keys.kirk_aes.empty() && !report.keys.kirk_cmd1 && report.keys.savedata.empty() &&
        report.keys.tags.empty() && report.keys.amctrl.empty())
        return nullptr;
    return &report.keys;
}

bool keys_come_from_keys_file() { return true; }

} // namespace portablekit
