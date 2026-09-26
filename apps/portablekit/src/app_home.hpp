#pragma once

// Where the app keeps what it is given and what it makes.
//
//   <home>/keys.txt                   the player's keys file, if any
//   <home>/games/<game id>/           one game: the port's data directory
//       game.txt                      what the app found out about it
//       EBOOT.ELF                     the executable, decrypted
//       settings.ini                  where the disc image is, and settings
//       ms0/                          the memory stick: saves
//   <cache>/<game id>/<build>/        one compiled corpus (see corpus.hpp)
//
// <home> is PORTABLEKIT_HOME when set, otherwise the platform's place for an
// application's data; <cache> is PORTABLEKIT_CACHE, otherwise the platform's
// place for caches, which may be cleared without losing anything but time.

#include <filesystem>
#include <string>

namespace portablekit::app {

[[nodiscard]] std::filesystem::path home_directory();
[[nodiscard]] std::filesystem::path cache_root();
[[nodiscard]] std::filesystem::path games_directory();
[[nodiscard]] std::filesystem::path keys_file_path();

// The recompiler and the headers the generated code includes, shipped with
// the program.
[[nodiscard]] std::filesystem::path recompiler_path();
[[nodiscard]] std::filesystem::path corpus_include_directory();

[[nodiscard]] std::string path_text(const std::filesystem::path &path);

} // namespace portablekit::app
