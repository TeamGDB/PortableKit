#pragma once

// Adding a game: reading the disc image the player points at, finding out
// which game it is, and getting its executable out of it.
//
// Nothing here knows a game in advance. The disc says what it is (PARAM.SFO);
// the executable comes, in this order, from:
//   1. an executable the player decrypted themselves and gives explicitly;
//   2. EBOOT.BIN, decrypted with the player's keys when they have them;
//   3. BOOT.BIN, which many discs carry unencrypted beside EBOOT.BIN.
// Whatever it came from, the result must be a PSP ELF, and its SHA-256 is what
// the game is known by from then on: the cache, and a hand-written profile if
// one matches.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace portablekit::app {

struct DiscInfo {
    std::string disc_id;        // "UCES01059"
    std::string title;          // "LocoRoco 2"
    std::string disc_version;   // DISC_VERSION
    std::uint32_t memsize{};    // PARAM.SFO MEMSIZE: 1 asks for the PSP-2000's extra memory
    std::uint32_t eboot_size{};
    std::uint32_t boot_size{};
    std::string eboot_sha256;
    std::optional<std::uint32_t> eboot_tag;  // the "~PSP" tag, when EBOOT.BIN is encrypted
    bool eboot_is_elf{};
    bool boot_is_elf{};
};

// Throws ImportError when the file is not a PSP disc image.
[[nodiscard]] DiscInfo identify_disc(const std::filesystem::path &iso);

class ImportError final : public std::runtime_error {
public:
    // `code` is for scripts: see app_main.cpp's exit codes.
    ImportError(int code, const std::string &message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

struct GameRecord {
    std::string id;              // "UCES01059-e1075b96": disc id and executable hash
    std::string disc_id;
    std::string title;
    std::filesystem::path iso;   // the player's image, used where it is
    std::filesystem::path data_dir;
    std::string executable_sha256;
    std::string eboot_sha256;
    std::string executable_source;  // "BOOT.BIN", "EBOOT.BIN (keys)", "provided"
    std::uint32_t memsize{};
    std::string added;           // when, for the library
};

struct ImportOptions {
    std::filesystem::path iso;
    std::optional<std::filesystem::path> executable;  // a decrypted EBOOT the player gives
    bool prefer_boot_bin{};                           // take BOOT.BIN even with keys
};

using ImportProgress = std::function<void(const std::string &stage)>;

// Identifies the disc, gets its executable, and records the game in
// <home>/games/<id>. Adding the same game again refreshes the record.
GameRecord import_game(const ImportOptions &options, const ImportProgress &progress = {});

[[nodiscard]] std::vector<GameRecord> list_games();
// By id, by disc id, or by a unique prefix of either (case-insensitive).
[[nodiscard]] std::optional<GameRecord> find_game(const std::string &name);
[[nodiscard]] std::optional<GameRecord> read_game(const std::filesystem::path &data_dir);

// "UCES-01059".
[[nodiscard]] std::string display_disc_id(const std::string &disc_id);

} // namespace portablekit::app
