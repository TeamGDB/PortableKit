#pragma once

// Saves through Android's document picker (#17). A folder the player picks
// is a tree of content:// documents, not a path, so the save screen works on
// local copies: an import copies the save folders it can find in the picked
// folder into a staging folder and checks them there as on any platform; an
// export or a backup is written to a staging folder and then copied into the
// picked one. Only in the Android app (PORTABLEKIT_ANDROID_APP).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::android {

struct PickedImport {
    std::filesystem::path staged;  // the local copy to look for saves in
    std::string error;             // empty: the copy worked
};
// Asks for a folder, then copies the save folders (those holding a PARAM.SFO)
// found in it into <staging>/<its name>/SAVEDATA, where find_saves() looks:
// the folder itself, its SAVEDATA and PSP folders and two levels of other
// folders, so a memory stick's root, PPSSPP's PSP folder or an export all
// work. `staging` is emptied first. Nothing when the player cancels.
[[nodiscard]] std::optional<PickedImport> pick_saves_to_import(const std::filesystem::path &staging);

struct PickedExport {
    std::string where;  // the picked folder, for the player
    std::string error;  // empty: the copy worked
};
// Asks for a folder and copies `local` into it as a folder of the same name,
// with everything in it. Nothing when the player cancels.
[[nodiscard]] std::optional<PickedExport> pick_folder_and_copy(const std::filesystem::path &local);

// --- Texture packs ---------------------------------------------------------
// A texture pack in a picked tree, and every file under its folder.
struct PackFile {
    std::string uri;
    std::string relative;  // "textures.ini", "ui/title.png"
    std::uint64_t size{};
};
struct FoundPack {
    std::string name;    // its folder's name, "UCUS98737"
    std::string where;   // the picked folder, for the player
    std::string layout;  // how it was found
    std::vector<PackFile> files;
};
// Finds the pack in a picked tree (`tree` as pick_folder() gives it) in the
// layouts the desktop import knows (gpu/texture_pack_import.hpp): the folder
// itself, <name>, textures/<name> or PSP/TEXTURES/<name> for each of `names`,
// else the first folder within three levels that holds textures.ini. Lists its
// files, leaving out those `skipped` names. Sets `error` when there is none.
[[nodiscard]] std::optional<FoundPack> find_texture_pack(const std::string &tree, const std::vector<std::string> &names,
                                                         bool (*skipped)(const std::string &), std::string &error);
// Copies one document into a local file.
bool copy_document(const std::string &uri, const std::filesystem::path &target, std::string &error);

} // namespace portablekit::android
