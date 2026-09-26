#pragma once

// A game's main corpus built as a shared library instead of linked into the
// executable: an edition's corpus (ProfileVariant), or the one the desktop app
// compiles on the player's machine. The library holds nothing but generated
// code and host/corpus_module.cpp.in's three entry points, and reaches the
// runtime only through the corpus ABI (psprecomp/corpus_abi.hpp).

#include "profile.hpp"

#include "psprecomp/corpus_abi.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace portablekit {

struct CorpusLibrary {
    std::filesystem::path path;
    void (*register_all)(psprecomp::CorpusRuntime &){};
    void *handle{};
};

// Opens the library and checks that it speaks this build's corpus ABI and was
// generated from the executable with this SHA-256 (empty: not checked).
// Touches no runtime, so it may run on any thread. Libraries are never
// closed: code of a replaced corpus may still be on a return path.
[[nodiscard]] std::optional<CorpusLibrary> open_corpus_library(const std::filesystem::path &path,
                                                               std::string_view executable_sha256,
                                                               std::string &error);

// Where a port's build puts an edition's corpus: corpora/<key>.<ext> next to
// the executable.
[[nodiscard]] std::filesystem::path variant_corpus_path(const ProfileVariant &variant);

} // namespace portablekit
