#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

// File mods: a file in mods/files of the data folder is read in place of the
// disc's file at the same path (mods/files/PSP_GAME/USRDIR/DATA/GAME.BIN for
// disc0:/PSP_GAME/USRDIR/DATA/GAME.BIN). It is the base of most mods: a
// translation of the game's text, models, sounds.
//
// Paths are matched without regard to case: the disc names its files in
// capitals, and a mod made on Windows or macOS may not, while Android and
// Linux tell the two apart. The setting mods.files (<prefix>_FILE_MODS) turns
// replacement off; it takes effect the next time the game opens a file.
namespace portablekit::file_mods {

// mods/files in the data folder; empty when there is no data folder.
[[nodiscard]] std::filesystem::path folder();

// The mod's file that stands in for the disc's file at `disc_path` ('/'
// separated, no device, as IsoImage names it), when file mods are on and the
// folder has one.
[[nodiscard]] std::optional<std::filesystem::path> find(const std::string &disc_path);

// How many files mods/files holds, whether or not file mods are on. Looks at
// the folder again at most every couple of seconds, so a mod copied in while
// the game runs is found by its next open.
[[nodiscard]] std::size_t count();

} // namespace portablekit::file_mods
