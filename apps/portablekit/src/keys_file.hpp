#pragma once

// The player's keys file. The program contains no key: every key the console's
// crypto needs is read from this file, which the player makes from their own
// console or finds themselves. See docs/DESKTOP_APP.md, "Keys".
//
// The format is text, one key per line, hex, '#' comments:
//
//   kirk.aes.5D  = <16 bytes>      KIRK command 4/7 key slots (hex slot number)
//   kirk.cmd1    = <16 bytes>      KIRK command 1 AES key
//   savedata.2   = <16 bytes>      save-data keys 2 to 7
//   tag.C0CB167C = <16 or 0x90 bytes>   what an executable's tag selects
//   tag.C0CB167C.slot = 5D         (optional) the KIRK slot of that tag
//   amctrl.1CD4  = <16 bytes>      the DRM library's fixed keys, which PGD data
//                                  needs with kirk.aes.38, .39 and .63; also
//                                  amctrl.1CE4, .1CF4, amctrl.dnas.1A90 and
//                                  .1AA0 (host/crypto/pgd.hpp)
//
// HLE extension modules may declare more names (Registry::declare_key), each
// with its length and optionally a fingerprint; they are read and checked the
// same way. A name nothing declares is a warning and is left out, but does not
// refuse the file.
//
// The fixed keys are checked against SHA-256 fingerprints compiled into the
// program, so a mistyped key is named instead of producing garbage; a
// fingerprint does not reveal the key. Tag keys have no fingerprint: an
// executable that decrypts to a valid ELF is the check.

#include "crypto_keys.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace portablekit::app {

struct KeysReport {
    bool file_found{};
    std::filesystem::path path;
    CryptoKeys keys;
    std::vector<std::string> problems;  // one sentence each
    // Names nothing reads (not this program, not an extension module): kept
    // out, but they do not refuse the file.
    std::vector<std::string> warnings;
    // The keys HLE extension modules declare, and whether the file has them.
    struct DeclaredStatus {
        std::string name;
        std::string module;
        bool present{};
    };
    std::vector<DeclaredStatus> declared;
    // What the keys that passed allow.
    bool can_decrypt_executables{};     // kirk.aes.5D and kirk.cmd1
    bool can_encrypt_saves{};           // every save-data key
    bool can_decrypt_pgd{};             // kirk.aes.38/39/63 and amctrl.1CD4/1CE4/1CF4
    std::size_t tag_count{};
};

[[nodiscard]] KeysReport read_keys_file(const std::filesystem::path &path);

// PORTABLEKIT_KEYS when set, otherwise <home>/keys.txt.
[[nodiscard]] std::filesystem::path active_keys_file();

// The report for the active keys file, read once.
[[nodiscard]] const KeysReport &active_keys();

// Checks `source` and copies it to <home>/keys.txt. The report says what was
// wrong when it returns false.
bool import_keys_file(const std::filesystem::path &source, KeysReport &report);

} // namespace portablekit::app
