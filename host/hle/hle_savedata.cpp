// sceUtilitySavedata*: the PSP's save-data dialog, backed by PSP-layout
// folders on the host (see save_data/savedata_store.hpp).
//
// What the game asks for:
//   - at boot, AUTOLOAD of its own save; when there is none, AUTOLOAD of the
//     saves of two other games (ULJM05500, ULJM05710) and SIZES for its own,
//     and "no data" answers lead to a new game;
//   - to save, AUTOLOAD of its own save followed by AUTOSAVE;
//   - after the title screen, AUTOLOAD again to read the characters.
// No save-data UI is drawn: modes that would show a list act as if the player
// confirmed the first entry. Every mode, the failures included, reports QUIT
// after a few polls; see utility_dialog.hpp.
#include "../profile.hpp"
#include "hle_common.hpp"
#include "utility_dialog.hpp"

#include "save_data/savedata_crypto.hpp"
#include "save_data/savedata_store.hpp"
#include "save_data/save_transfer.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace portablekit {
namespace {

// SceUtilitySavedataParam, after the common dialog header.
namespace param {
constexpr std::uint32_t kMode = 0x30u;
constexpr std::uint32_t kOverwrite = 0x38u;
constexpr std::uint32_t kGameName = 0x3Cu;      // char[13]
constexpr std::uint32_t kSaveName = 0x4Cu;      // char[20]
constexpr std::uint32_t kSaveNameList = 0x60u;  // pointer to char[20] entries, "" terminated
constexpr std::uint32_t kFileName = 0x64u;      // char[13]
constexpr std::uint32_t kDataBuf = 0x74u;
constexpr std::uint32_t kDataBufSize = 0x78u;
constexpr std::uint32_t kDataSize = 0x7Cu;
constexpr std::uint32_t kTitle = 0x80u;          // char[128]
constexpr std::uint32_t kSavedataTitle = 0x100u; // char[128]
constexpr std::uint32_t kDetail = 0x180u;        // char[1024]
constexpr std::uint32_t kParentalLevel = 0x580u;
constexpr std::uint32_t kIcon0 = 0x584u;  // {buf, bufSize, size, unknown}
constexpr std::uint32_t kIcon1 = 0x594u;
constexpr std::uint32_t kPic1 = 0x5A4u;
constexpr std::uint32_t kSnd0 = 0x5B4u;
constexpr std::uint32_t kFocus = 0x5C8u;
constexpr std::uint32_t kMsFree = 0x5D0u;
constexpr std::uint32_t kMsData = 0x5D4u;
constexpr std::uint32_t kUtilityData = 0x5D8u;
constexpr std::uint32_t kKey = 0x5DCu;  // char[16], firmware 2.00 and later
constexpr std::uint32_t kSecureVersion = 0x5ECu;
// LIST: a pointer to {maxCount, resultCount, entries}. Read off a game's own
// request (the only pointer in the block past the names), not recalled.
constexpr std::uint32_t kIdList = 0x5F4u;
constexpr std::uint32_t kMinimumSizeWithKey = 0x5ECu;
} // namespace param

enum Mode : std::uint32_t {
    kAutoLoad = 0,
    kAutoSave = 1,
    kLoad = 2,
    kSave = 3,
    kListLoad = 4,
    kListSave = 5,
    kListDelete = 6,
    kDelete = 7,
    kSizes = 8,
    kAutoDelete = 9,
    kSingleDelete = 10,
    kList = 11,
    kFiles = 12,
    kMakeDataSecure = 13,
    kMakeData = 14,
    kReadDataSecure = 15,
    kReadData = 16,
    kWriteDataSecure = 17,
    kWriteData = 18,
    kEraseSecure = 19,
    kErase = 20,
    kDeleteData = 21,
    kGetSize = 22,
};

const char *mode_name(std::uint32_t mode) {
    static const char *const names[] = {"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE",
                                        "LISTDELETE", "DELETE", "SIZES", "AUTODELETE", "SINGLEDELETE", "LIST",
                                        "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
                                        "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA",
                                        "GETSIZE"};
    return mode < std::size(names) ? names[mode] : "UNKNOWN";
}

// Result codes written to the parameter block's result field.
namespace result {
constexpr std::uint32_t kOk = 0u;
constexpr std::uint32_t kLoadDataBroken = 0x80110306u;
constexpr std::uint32_t kLoadNoData = 0x80110307u;
constexpr std::uint32_t kLoadParam = 0x80110308u;
constexpr std::uint32_t kDeleteNoData = 0x80110347u;
constexpr std::uint32_t kSaveAccessError = 0x80110385u;
constexpr std::uint32_t kSaveParam = 0x80110388u;
constexpr std::uint32_t kSizesNoData = 0x801103C7u;
} // namespace result

constexpr std::uint32_t kClusterSize = 0x8000u;
constexpr std::uint32_t kFreeClusters = 0x8000u;  // 1 GiB

struct SavedataState {
    std::filesystem::path memory_stick;
    DialogLifecycle dialog;
};

SavedataState &state() {
    static SavedataState s;
    return s;
}

bool trace_savedata() {
    static const bool enabled = portablekit::env("TRACE_SAVEDATA") != nullptr;
    return enabled;
}

std::string hex_bytes(const savedata::Block &block) {
    std::string text;
    char digits[3];
    for (const std::uint8_t b : block) {
        std::snprintf(digits, sizeof digits, "%02x", b);
        text += digits;
    }
    return text;
}

// "<>" stands for "no save name": the folder is the game name alone.
std::string save_name_at(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    std::string name = read_cstring(memory, address, 20u);
    return name == "<>" ? std::string{} : name;
}

std::vector<std::string> save_name_list(const psprecomp::GuestMemory &memory, std::uint32_t list) {
    std::vector<std::string> names;
    if (list == 0u) return names;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const std::uint32_t entry = list + i * 20u;
        if (memory.load8(entry) == 0u) break;
        names.push_back(save_name_at(memory, entry));
    }
    return names;
}

std::vector<std::uint8_t> read_guest(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t i = 0; i < size; ++i) bytes[i] = memory.load8(address + i);
    return bytes;
}

// {buf, bufSize, size, unknown}: the bytes the game supplies for an icon file.
std::vector<std::uint8_t> read_file_data(const psprecomp::GuestMemory &memory, std::uint32_t field) {
    const std::uint32_t buffer = memory.load32(field);
    const std::uint32_t size = std::min(memory.load32(field + 8u), memory.load32(field + 4u));
    if (buffer == 0u || size == 0u) return {};
    return read_guest(memory, buffer, size);
}

savedata::SaveFiles files_for(const psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    savedata::SaveFiles files;
    files.game_name = read_cstring(memory, params + param::kGameName, 13u);
    files.save_name = save_name;
    files.file_name = read_cstring(memory, params + param::kFileName, 13u);
    if (memory.load32(params + dialog_common::kSizeOffset) > param::kMinimumSizeWithKey) {
        savedata::Block key{};
        for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
        if (!savedata::is_zero(key)) {
            files.key = key;
            // The menu's Import checks saves with it.
            savedata::remember_game_key(files.game_name, key);
        }
    }
    return files;
}

void log_request(const psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    std::cerr << "[savedata] " << mode_name(mode) << " (" << mode << ")"
              << " game=\"" << read_cstring(memory, params + param::kGameName, 13u) << "\""
              << " save=\"" << read_cstring(memory, params + param::kSaveName, 20u) << "\""
              << " file=\"" << read_cstring(memory, params + param::kFileName, 13u) << "\"";
    if (trace_savedata()) {
        const std::uint32_t size = memory.load32(params + dialog_common::kSizeOffset);
        std::cerr << " size=" << psprecomp::hex32(size)
                  << " buf=" << psprecomp::hex32(memory.load32(params + param::kDataBuf))
                  << " bufSize=" << psprecomp::hex32(memory.load32(params + param::kDataBufSize))
                  << " dataSize=" << psprecomp::hex32(memory.load32(params + param::kDataSize))
                  << " overwrite=" << memory.load32(params + param::kOverwrite)
                  << " focus=" << memory.load32(params + param::kFocus)
                  << " icon0=" << memory.load32(params + param::kIcon0 + 8u)
                  << " icon1=" << memory.load32(params + param::kIcon1 + 8u)
                  << " pic1=" << memory.load32(params + param::kPic1 + 8u)
                  << " snd0=" << memory.load32(params + param::kSnd0 + 8u);
        if (size > param::kMinimumSizeWithKey) {
            savedata::Block key{};
            for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
            std::cerr << " key=" << hex_bytes(key)
                      << " secureVersion=" << memory.load32(params + param::kSecureVersion);
        }
        const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
        if (!names.empty()) {
            std::cerr << " list=[";
            for (std::size_t i = 0; i < names.size(); ++i) std::cerr << (i ? "," : "") << "\"" << names[i] << "\"";
            std::cerr << "]";
        }
    }
    std::cerr << "\n";
}

std::uint32_t do_load(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t capacity = memory.load32(params + param::kDataBufSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kLoadParam;

    const auto loaded = savedata::load_save(state().memory_stick, files);
    if (loaded.status == savedata::LoadStatus::NoData) {
        std::cerr << "[savedata] no save data in " << savedata::save_folder(state().memory_stick, files).string() << "\n";
        return result::kLoadNoData;
    }
    if (loaded.status == savedata::LoadStatus::Broken) {
        std::cerr << "[savedata] broken save in " << savedata::save_folder(state().memory_stick, files).string()
                  << ": " << loaded.reason << "\n";
        return result::kLoadDataBroken;
    }
    const auto &data = loaded.contents.data;
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(data.size(), capacity));
    memory.copy_in(buffer, std::span<const std::uint8_t>(data.data(), count));
    memory.store32(params + param::kDataSize, count);
    write_cstring(memory, params + param::kTitle, loaded.contents.title, 128u);
    write_cstring(memory, params + param::kSavedataTitle, loaded.contents.savedata_title, 128u);
    write_cstring(memory, params + param::kDetail, loaded.contents.detail, 1024u);
    std::cerr << "[savedata] loaded " << count << " bytes from "
              << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (decrypted)" : "") << "\n";
    if (data.size() > capacity)
        std::cerr << "[savedata] " << files.file_name << " holds " << data.size() << " bytes; the buffer takes "
                  << capacity << "\n";
    return result::kOk;
}

std::uint32_t do_save(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t size = memory.load32(params + param::kDataSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kSaveParam;

    savedata::SaveContents contents;
    contents.data = read_guest(memory, buffer, size);
    contents.title = read_cstring(memory, params + param::kTitle, 128u);
    contents.savedata_title = read_cstring(memory, params + param::kSavedataTitle, 128u);
    contents.detail = read_cstring(memory, params + param::kDetail, 1024u);
    contents.parental_level = memory.load8(params + param::kParentalLevel);
    contents.icon0 = read_file_data(memory, params + param::kIcon0);
    contents.icon1 = read_file_data(memory, params + param::kIcon1);
    contents.pic1 = read_file_data(memory, params + param::kPic1);
    contents.snd0 = read_file_data(memory, params + param::kSnd0);

    std::string error;
    if (!savedata::write_save(state().memory_stick, files, contents, error)) {
        std::cerr << "[savedata] save failed: " << error << "\n";
        return result::kSaveAccessError;
    }
    std::cerr << "[savedata] saved " << size << " bytes to " << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (encrypted)" : "") << "\n";
    return result::kOk;
}

std::uint32_t do_delete(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    if (!savedata::delete_save(state().memory_stick, files)) return result::kDeleteNoData;
    std::cerr << "[savedata] deleted " << savedata::save_folder(state().memory_stick, files).string() << "\n";
    return result::kOk;
}

void write_size_string(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t kilobytes) {
    const std::string text = kilobytes >= 1024u * 1024u ? std::to_string(kilobytes / (1024u * 1024u)) + " GB"
                             : kilobytes >= 1024u     ? std::to_string(kilobytes / 1024u) + " MB"
                                                      : std::to_string(kilobytes) + " KB";
    write_cstring(memory, address, text, 8u);
}

// SIZES: free space on the stick, the space an existing save takes, and the
// space the requested save would need.
std::uint32_t do_sizes(psprecomp::GuestMemory &memory, std::uint32_t params) {
    std::uint32_t outcome = result::kOk;
    if (const std::uint32_t free = memory.load32(params + param::kMsFree); free != 0u) {
        memory.store32(free, kClusterSize);
        memory.store32(free + 4u, kFreeClusters);
        const std::uint64_t free_kb = static_cast<std::uint64_t>(kClusterSize) * kFreeClusters / 1024u;
        memory.store32(free + 8u, static_cast<std::uint32_t>(free_kb));
        write_size_string(memory, free + 12u, free_kb);
    }
    if (const std::uint32_t data = memory.load32(params + param::kMsData); data != 0u) {
        savedata::SaveFiles files;
        files.game_name = read_cstring(memory, data, 13u);
        files.save_name = save_name_at(memory, data + 16u);
        const std::uint64_t bytes = savedata::save_size(state().memory_stick, files);
        const std::uint32_t info = data + 36u;
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        memory.store32(info, clusters);
        memory.store32(info + 4u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 8u, (bytes + 1023u) / 1024u);
        memory.store32(info + 16u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 20u, (bytes + 1023u) / 1024u);
        if (bytes == 0u) outcome = result::kSizesNoData;
    }
    if (const std::uint32_t needed = memory.load32(params + param::kUtilityData); needed != 0u) {
        std::uint64_t bytes = memory.load32(params + param::kDataSize) + 16u;
        for (const std::uint32_t field : {param::kIcon0, param::kIcon1, param::kPic1, param::kSnd0})
            bytes += memory.load32(params + field + 8u);
        bytes += 0x2000u;  // PARAM.SFO and directory entries
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        const std::uint64_t kb = static_cast<std::uint64_t>(clusters) * kClusterSize / 1024u;
        memory.store32(needed, clusters);
        memory.store32(needed + 4u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 8u, kb);
        memory.store32(needed + 16u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 20u, kb);
    }
    return outcome;
}

// Picks the save a list dialog would land on when the player just confirms:
// for loading and deleting the first listed save that exists, for saving the
// first listed name.
std::optional<std::string> pick_from_list(psprecomp::GuestMemory &memory, std::uint32_t params, bool must_exist) {
    const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
    for (const auto &name : names) {
        if (!must_exist || savedata::save_exists(state().memory_stick, files_for(memory, params, name))) return name;
    }
    if (!must_exist) return save_name_at(memory, params + param::kSaveName);
    return std::nullopt;
}

// A save name pattern as LIST takes it: '?' stands for any one character.
bool name_matches(std::string_view pattern, std::string_view name) {
    if (pattern.size() != name.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    return true;
}

// LIST: which saves of this game match the save name, up to the list's
// capacity. Each entry is a folder mode word, three 16-byte date records
// (created, accessed, modified) and the 20-byte save name. Only the count has
// been seen used, by a game with no saves yet; the entry layout is the
// documented one, not traced.
std::uint32_t do_list(psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t list = memory.load32(params + param::kIdList);
    if (list == 0u || !memory.contains(list, 12u)) return result::kLoadParam;
    const std::uint32_t capacity = memory.load32(list);
    const std::uint32_t entries = memory.load32(list + 8u);
    const std::string game_name = read_cstring(memory, params + param::kGameName, 13u);
    const std::string pattern = save_name_at(memory, params + param::kSaveName);
    std::vector<std::pair<std::string, std::filesystem::file_time_type>> found;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(savedata::savedata_root(state().memory_stick), error)) {
        if (!entry.is_directory()) continue;
        const std::string folder = entry.path().filename().string();
        if (!folder.starts_with(game_name)) continue;
        const std::string save = folder.substr(game_name.size());
        if (!name_matches(pattern, save)) continue;
        found.emplace_back(save, entry.last_write_time(error));
    }
    std::sort(found.begin(), found.end());
    constexpr std::uint32_t kEntrySize = 4u + 3u * 16u + 20u;
    std::uint32_t count = 0u;
    for (const auto &[save, written] : found) {
        if (count >= capacity || entries == 0u) break;
        const std::uint32_t at = entries + count * kEntrySize;
        if (!memory.contains(at, kEntrySize)) break;
        memory.store32(at, 0x11FFu);  // a folder, readable and writable
        // file_time_type's clock is not the system clock everywhere, and not
        // every standard library converts between them; offset by now.
        const auto system_time = std::chrono::system_clock::now() +
                                 std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                     written - std::filesystem::file_time_type::clock::now());
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch());
        const std::time_t time = static_cast<std::time_t>(seconds.count());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        for (std::uint32_t record = 0; record < 3u; ++record) {
            const std::uint32_t date = at + 4u + record * 16u;
            memory.store16(date, static_cast<std::uint16_t>(tm.tm_year + 1900));
            memory.store16(date + 2u, static_cast<std::uint16_t>(tm.tm_mon + 1));
            memory.store16(date + 4u, static_cast<std::uint16_t>(tm.tm_mday));
            memory.store16(date + 6u, static_cast<std::uint16_t>(tm.tm_hour));
            memory.store16(date + 8u, static_cast<std::uint16_t>(tm.tm_min));
            memory.store16(date + 10u, static_cast<std::uint16_t>(tm.tm_sec));
            memory.store32(date + 12u, 0u);
        }
        write_cstring(memory, at + 52u, save, 20u);
        ++count;
    }
    memory.store32(list + 4u, count);
    if (trace_savedata())
        std::cerr << "[savedata] LIST " << game_name << pattern << ": " << count << " of " << found.size()
                  << " matching saves\n";
    return result::kOk;
}

std::uint32_t run_request(psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    const std::string save_name = save_name_at(memory, params + param::kSaveName);
    switch (mode) {
    case kAutoLoad:
    case kLoad:
        return do_load(memory, params, save_name);
    case kAutoSave:
    case kSave:
        return do_save(memory, params, save_name);
    case kListLoad: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kLoadNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_load(memory, params, *chosen);
    }
    case kListSave: {
        const auto chosen = pick_from_list(memory, params, false);
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_save(memory, params, *chosen);
    }
    case kListDelete: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kDeleteNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_delete(memory, params, *chosen);
    }
    case kDelete:
    case kAutoDelete:
    case kSingleDelete:
        return do_delete(memory, params, save_name);
    case kSizes:
    // GETSIZE asks the same question as SIZES and is answered from the same
    // three blocks of the parameter: what the card has free, what the save
    // already on it takes, and what this one would need. What the two modes
    // do differently on a PSP is not established here; what is established
    // is that a game asks GETSIZE while checking the memory stick, and that
    // answering it with a parameter error puts an error dialog on the screen
    // and stops the game there.
    case kGetSize:
        return do_sizes(memory, params);
    case kList:
        return do_list(memory, params);
    default:
        // Not used by this game. Report a parameter error so the guest takes
        // its failure path instead of reading results that were never written.
        std::cerr << "[savedata] mode " << mode << " (" << mode_name(mode) << ") is not implemented\n";
        if (trace_savedata()) {
            // What the game put in the block past the names, so a mode can be
            // implemented from what a game sends rather than from memory.
            const std::uint32_t size = memory.load32(params);
            for (std::uint32_t offset = 0x580u; offset + 4u <= std::min<std::uint32_t>(size, 0x800u); offset += 4u)
                if (const std::uint32_t word = memory.load32(params + offset); word != 0u) {
                    std::cerr << "[savedata]   +" << psprecomp::hex32(offset) << " = " << psprecomp::hex32(word);
                    // A pointer into RAM: what it points at, too.
                    if (memory.contains(word, 16u))
                        for (std::uint32_t i = 0; i < 4u; ++i)
                            std::cerr << (i == 0u ? " -> " : " ") << psprecomp::hex32(memory.load32(word + i * 4u));
                    std::cerr << "\n";
                }
        }
        return result::kLoadParam;
    }
}

} // namespace

void register_savedata(HleRegistrar &hle, const std::filesystem::path &memory_stick) {
    state().memory_stick = memory_stick;
    savedata::set_memory_stick(memory_stick);

    hle.add("sceUtility", "sceUtilitySavedataInitStart", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t params = arg(ctx, 0);
        // A new request while one is still active replaces it rather than
        // failing: the earlier request's work is already done.
        if (state().dialog.active())
            std::cerr << "[savedata] InitStart while a dialog is active (status " << state().dialog.status() << ")\n";
        log_request(memory, params);
        const std::uint32_t outcome = run_request(memory, params);
        memory.store32(params + dialog_common::kResultOffset, outcome);
        if (outcome != result::kOk || trace_savedata())
            std::cerr << "[savedata] result " << psprecomp::hex32(outcome) << "\n";
        state().dialog.start();
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataUpdate", [](Runtime &, AllegrexContext &ctx) {
        (void)state().dialog.poll();
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataGetStatus", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t status = state().dialog.poll();
        if (trace_savedata()) std::cerr << "[savedata] status " << status << "\n";
        kernel().finish(ctx, status);
    });

    hle.add("sceUtility", "sceUtilitySavedataShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        if (!state().dialog.active()) {
            kernel().finish(ctx, kErrorUtilityInvalidStatus);
            return;
        }
        if (!state().dialog.shutdown())
            std::cerr << "[savedata] ShutdownStart before the dialog finished\n";
        kernel().finish(ctx, 0u);
    });
}

} // namespace portablekit
