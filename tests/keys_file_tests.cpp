// The desktop app's keys file as HLE extension modules see it: every key the
// file gives can be looked up by its name (hle_extension::key goes through
// key_value), PortableKit's own included, whether or not a module also
// declares it. The values of PortableKit's own keys come from
// host/crypto_keys_builtin.cpp, compiled here under another name, so no key
// is written in this file.

#include "extension_keys.hpp"
#include "keys_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace portablekit {
const CryptoKeys *builtin_crypto_keys();  // crypto_keys_builtin.cpp, renamed

// The keys the modules of this test declare: one PortableKit checks itself,
// and one of the module's own.
const std::vector<DeclaredKey> &declared_extension_keys() {
    static const std::vector<DeclaredKey> keys{{"kirk.aes.5d", "", 16u, "Test module"},
                                               {"vendor.key", "", 16u, "Test module"}};
    return keys;
}
} // namespace portablekit

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

std::string hex(const portablekit::Key16 &key) {
    std::string text;
    char digits[3];
    for (const std::uint8_t byte : key) {
        std::snprintf(digits, sizeof digits, "%02X", byte);
        text += digits;
    }
    return text;
}

std::vector<std::uint8_t> bytes(const portablekit::Key16 &key) { return {key.begin(), key.end()}; }

} // namespace

int main() {
    const portablekit::CryptoKeys &builtin = *portablekit::builtin_crypto_keys();
    const portablekit::Key16 kirk5d = *builtin.kirk(0x5Du);
    const portablekit::Key16 save2 = *builtin.savedata_key(2);
    const portablekit::Key16 cmd1 = *builtin.kirk_cmd1;

    const auto path = std::filesystem::temp_directory_path() / "portablekit_keys_file_test.txt";
    {
        std::ofstream out(path);
        out << "# checked by PortableKit, declared by the module too\n"
            << "kirk.aes.5D = " << hex(kirk5d) << "\n"
            << "# checked by PortableKit, declared by nobody\n"
            << "savedata.2 = " << hex(save2) << "\n"
            << "KIRK.CMD1 = " << hex(cmd1) << "\n"
            << "vendor.key = 000102030405060708090A0B0C0D0E0F\n"
            << "unknown.name = 00\n";
    }
    const portablekit::app::KeysReport report = portablekit::app::read_keys_file(path);
    std::filesystem::remove(path);

    for (const std::string &problem : report.problems) std::printf("     problem: %s\n", problem.c_str());
    check(report.file_found && report.problems.empty(), "the file is read without problems");
    check(report.warnings.size() == 1u, "a name nothing reads is a warning");
    check(report.can_decrypt_executables, "PortableKit's own keys still do their work");

    // What hle_extension::key(name) answers from (hle_extension_host.cpp).
    const portablekit::CryptoKeys *keys = &report.keys;
    check(portablekit::key_value(keys, "kirk.aes.5D") == bytes(kirk5d) && report.keys.named.contains("kirk.aes.5d"),
          "a name PortableKit checks and a module declares is found by name");
    check(portablekit::key_value(keys, "savedata.2") == bytes(save2) && report.keys.named.contains("savedata.2"),
          "a name PortableKit checks and no module declares is found by name");
    check(report.keys.named.contains("kirk.cmd1"), "every key PortableKit accepts is kept by its name");
    check(portablekit::key_value(keys, "vendor.key").value_or(std::vector<std::uint8_t>{}).size() == 16u,
          "a module's own key is found");
    bool listed = report.declared.size() == 2u;
    for (const auto &key : report.declared) listed = listed && key.present;
    check(listed, "keys status lists both declared keys as present");

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
