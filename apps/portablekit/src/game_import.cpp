#include "game_import.hpp"

#include "app_home.hpp"
#include "keys_file.hpp"
#include "text_format.hpp"

#include "install/executable_preparation.hpp"
#include "install/user_data.hpp"
#include "kernel/iso_image.hpp"
#include "save_data/param_sfo.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>

namespace portablekit::app {
namespace {

constexpr const char *kRecordFile = "game.txt";
constexpr const char *kEbootPath = "PSP_GAME/SYSDIR/EBOOT.BIN";
constexpr const char *kBootPath = "PSP_GAME/SYSDIR/BOOT.BIN";
constexpr const char *kParamSfoPath = "PSP_GAME/PARAM.SFO";

// Exit codes a script can tell apart (app_main.cpp lists them all).
constexpr int kNotADisc = 10;
constexpr int kNeedKeys = 11;
constexpr int kBadExecutable = 12;
constexpr int kIoError = 13;

std::vector<std::uint8_t> read_from_iso(IsoImage &iso, const char *path) {
    const auto entry = iso.find(path);
    if (!entry || entry->directory) return {};
    std::vector<std::uint8_t> bytes(entry->size);
    const std::size_t got = iso.read(static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize, bytes);
    bytes.resize(got);
    return bytes;
}

bool is_elf(const std::vector<std::uint8_t> &bytes) {
    return bytes.size() > 0x34u && bytes[0] == 0x7Fu && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
}

bool is_encrypted(const std::vector<std::uint8_t> &bytes) {
    return bytes.size() > 0x150u && bytes[0] == '~' && bytes[1] == 'P' && bytes[2] == 'S' && bytes[3] == 'P';
}

// A PSP executable the runtime can load: parsing it is the check.
void check_executable(const std::vector<std::uint8_t> &bytes, const std::string &what) {
    if (!is_elf(bytes)) throw ImportError(kBadExecutable, what + " is not a PSP executable (no ELF header).");
    try {
        (void)psprecomp::Elf32Image::from_bytes(bytes, what);
    } catch (const std::exception &e) {
        throw ImportError(kBadExecutable, what + " is not a PSP executable this program can load: " + e.what());
    }
}

std::vector<std::uint8_t> read_whole_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ImportError(kIoError, "Cannot read " + path_text(path) + ".");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> decrypt_with_keys(const std::vector<std::uint8_t> &eboot, const std::string &what) {
    const auto tag = install::executable_tag(eboot);
    if (!tag) throw ImportError(kBadExecutable, what + " is neither encrypted nor an executable.");
    const KeysReport &keys = active_keys();
    if (!keys.can_decrypt_executables)
        throw ImportError(kNeedKeys, what + " is encrypted. Decrypting it needs the keys kirk.aes.5D and kirk.cmd1 "
                                            "in a keys file (portablekit keys import <file>), or give an executable "
                                            "you decrypted yourself with --executable.");
    const auto found = keys.keys.tags.find(*tag);
    if (found == keys.keys.tags.end())
        throw ImportError(kNeedKeys, what + " is encrypted with tag " + hex32_text(*tag) +
                                         ", and the keys file has no key for it (tag." + hex32_text(*tag) + ").");
    install::TagKeyMaterial material{*tag, {}, {}};
    if (found->second.key) material.key = *found->second.key;
    material.table = found->second.table;
    // The optional slot line, as written in any case.
    for (const auto &[name, value] : read_key_values(keys.path))
        if (lower(name) == lower("tag." + hex32_text(*tag) + ".slot"))
            material.kirk_slot = static_cast<std::uint8_t>(std::strtoul(value.c_str(), nullptr, 16));
    try {
        std::vector<std::uint8_t> elf = install::decrypt_executable(eboot, material);
        check_executable(elf, what + " decrypted");
        return elf;
    } catch (const ImportError &) {
        throw;
    } catch (const std::exception &e) {
        throw ImportError(kBadExecutable, what + " did not decrypt: " + e.what());
    }
}

std::string now_text() {
    const std::time_t now = std::time(nullptr);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", std::localtime(&now));
    return text;
}

bool write_file(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::filesystem::path temporary = path;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    return !ec;
}

} // namespace

std::string display_disc_id(const std::string &disc_id) {
    if (disc_id.size() == 9u) return disc_id.substr(0, 4) + "-" + disc_id.substr(4);
    return disc_id;
}

DiscInfo identify_disc(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) throw ImportError(kIoError, "There is no file at " + path_text(path) + ".");
    DiscInfo info;
    try {
        IsoImage iso(path);
        const std::vector<std::uint8_t> sfo_bytes = read_from_iso(iso, kParamSfoPath);
        const auto sfo = savedata::ParamSfo::parse(sfo_bytes);
        if (!sfo) throw ImportError(kNotADisc, path_text(path) + " is not a PSP game disc image: it has no PSP_GAME/PARAM.SFO.");
        info.disc_id = sfo->string("DISC_ID").value_or("");
        info.title = sfo->string("TITLE").value_or("");
        info.disc_version = sfo->string("DISC_VERSION").value_or("");
        info.memsize = sfo->integer("MEMSIZE").value_or(0u);
        if (sfo->string("CATEGORY").value_or("") != "UG")
            throw ImportError(kNotADisc, path_text(path) + " is not a UMD game (its PARAM.SFO category is \"" +
                                             sfo->string("CATEGORY").value_or("") + "\").");
        const std::vector<std::uint8_t> eboot = read_from_iso(iso, kEbootPath);
        const std::vector<std::uint8_t> boot = read_from_iso(iso, kBootPath);
        if (eboot.empty()) throw ImportError(kNotADisc, path_text(path) + " has no PSP_GAME/SYSDIR/EBOOT.BIN.");
        info.eboot_size = static_cast<std::uint32_t>(eboot.size());
        info.boot_size = static_cast<std::uint32_t>(boot.size());
        info.eboot_sha256 = psprecomp::sha256_bytes(eboot);
        info.eboot_tag = install::executable_tag(eboot);
        info.eboot_is_elf = is_elf(eboot);
        info.boot_is_elf = is_elf(boot);
    } catch (const ImportError &) {
        throw;
    } catch (const std::exception &e) {
        throw ImportError(kNotADisc, path_text(path) + " is not a readable disc image: " + e.what());
    }
    if (info.disc_id.empty()) throw ImportError(kNotADisc, path_text(path) + " has no disc id in PARAM.SFO.");
    return info;
}

GameRecord import_game(const ImportOptions &options, const ImportProgress &progress) {
    const auto say = [&](const std::string &stage) {
        if (progress) progress(stage);
    };
    say("Reading the disc image");
    const DiscInfo disc = identify_disc(options.iso);

    std::vector<std::uint8_t> executable;
    std::string source;
    if (options.executable) {
        say("Checking the executable you gave");
        std::vector<std::uint8_t> given = read_whole_file(*options.executable);
        if (is_encrypted(given)) {
            executable = decrypt_with_keys(given, path_text(*options.executable));
            source = "provided, decrypted with keys";
        } else {
            check_executable(given, path_text(*options.executable));
            executable = std::move(given);
            source = "provided";
        }
    } else {
        IsoImage iso(options.iso);
        const std::vector<std::uint8_t> eboot = read_from_iso(iso, kEbootPath);
        const std::vector<std::uint8_t> boot = read_from_iso(iso, kBootPath);
        const bool boot_usable = is_elf(boot);
        if (is_elf(eboot)) {
            check_executable(eboot, "EBOOT.BIN");
            executable = eboot;
            source = "EBOOT.BIN (unencrypted)";
        } else if (active_keys().can_decrypt_executables && !options.prefer_boot_bin) {
            say("Decrypting EBOOT.BIN with your keys");
            try {
                executable = decrypt_with_keys(eboot, "EBOOT.BIN");
                source = "EBOOT.BIN, decrypted with keys";
            } catch (const ImportError &e) {
                if (!boot_usable) throw;
                say(std::string("EBOOT.BIN: ") + e.what() + " Using BOOT.BIN.");
            }
        }
        if (executable.empty()) {
            if (!boot_usable)
                throw ImportError(kNeedKeys,
                                  "The game's executable on this disc is encrypted, and the disc carries no "
                                  "unencrypted copy (BOOT.BIN). Add a keys file (portablekit keys import <file>), "
                                  "or give an executable you decrypted yourself with --executable <EBOOT>.");
            check_executable(boot, "BOOT.BIN");
            executable = boot;
            source = "BOOT.BIN";
        }
    }

    GameRecord record;
    record.disc_id = disc.disc_id;
    record.title = disc.title;
    record.iso = std::filesystem::absolute(options.iso);
    record.executable_sha256 = psprecomp::sha256_bytes(executable);
    record.eboot_sha256 = disc.eboot_sha256;
    record.executable_source = source;
    record.memsize = disc.memsize;
    record.id = disc.disc_id + "-" + record.executable_sha256.substr(0, 8);
    record.data_dir = games_directory() / record.id;
    record.added = now_text();

    say("Writing " + path_text(record.data_dir));
    std::error_code ec;
    std::filesystem::create_directories(record.data_dir, ec);
    if (ec) throw ImportError(kIoError, "Cannot create " + path_text(record.data_dir) + ": " + ec.message());
    if (!write_file(record.data_dir / install::kExecutableFile, executable))
        throw ImportError(kIoError, "Cannot write the executable into " + path_text(record.data_dir) + ".");
    // The port's own record of where the disc image is: an absolute path is
    // "used in place", so the app never copies the player's image.
    install::UserSettings settings = install::load_settings(record.data_dir);
    settings.disc_image = record.iso;
    install::save_settings(record.data_dir, settings);

    KeyValues values{
        {"id", record.id},
        {"disc_id", record.disc_id},
        {"title", record.title},
        {"iso", path_text(record.iso)},
        {"executable_sha256", record.executable_sha256},
        {"eboot_sha256", record.eboot_sha256},
        {"executable_source", record.executable_source},
        {"memsize", std::to_string(record.memsize)},
        {"added", record.added},
    };
    if (!write_key_values(record.data_dir / kRecordFile, values,
                          "# What PortableKit found out about this game when it was added.\n"))
        throw ImportError(kIoError, "Cannot write " + path_text(record.data_dir / kRecordFile) + ".");
    return record;
}

std::optional<GameRecord> read_game(const std::filesystem::path &data_dir) {
    const KeyValues values = read_key_values(data_dir / kRecordFile);
    if (values.empty() || !values.contains("id")) return std::nullopt;
    const auto get = [&](const char *key) {
        const auto it = values.find(key);
        return it != values.end() ? it->second : std::string();
    };
    GameRecord record;
    record.id = get("id");
    record.disc_id = get("disc_id");
    record.title = get("title");
    record.iso = install::path_from_utf8(get("iso"));
    record.data_dir = data_dir;
    record.executable_sha256 = get("executable_sha256");
    record.eboot_sha256 = get("eboot_sha256");
    record.executable_source = get("executable_source");
    record.memsize = static_cast<std::uint32_t>(std::strtoul(get("memsize").c_str(), nullptr, 10));
    record.added = get("added");
    return record;
}

std::vector<GameRecord> list_games() {
    std::vector<GameRecord> games;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(games_directory(), ec)) {
        if (!entry.is_directory()) continue;
        if (auto record = read_game(entry.path())) games.push_back(std::move(*record));
    }
    std::sort(games.begin(), games.end(), [](const GameRecord &a, const GameRecord &b) { return a.title < b.title; });
    return games;
}

std::optional<GameRecord> find_game(const std::string &name) {
    const std::string wanted = lower(name);
    std::optional<GameRecord> match;
    int matches = 0;
    for (GameRecord &record : list_games()) {
        const std::string id = lower(record.id);
        const std::string disc = lower(record.disc_id);
        if (id == wanted) return record;
        if (disc == wanted || id.starts_with(wanted) || lower(display_disc_id(record.disc_id)) == wanted) {
            match = record;
            ++matches;
        }
    }
    return matches == 1 ? match : std::nullopt;
}

} // namespace portablekit::app
