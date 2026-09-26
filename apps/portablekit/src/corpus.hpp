#pragma once

// A game's recompiled code, compiled on the player's machine and cached.
//
//   <cache>/<game id>/<abi>-O<level>/
//       generated/       what psp_recomp wrote (removed once compiled)
//       objects/         one object per unit (removed once linked)
//       corpus.<ext>     the library the runtime loads: .dylib, .so or .dll
//       status.txt       state, progress, timings; read by the running game
//       build.log        everything the recompiler and compiler printed
//
// <abi> is kCorpusAbi (corpus_abi.hpp): a hash of the runtime headers, the
// recompiler and the compile definitions. A new build of the program that
// changes any of them has a new <abi>, so it never loads a library made for
// another; the old directory is left for `portablekit cache clear`.

#include "game_import.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::app {

// The compiler the app found, and how it calls it.
struct Toolchain {
    bool found{};
    std::string kind;          // "apple-clang", "clang", "gcc", "clang-cl", "llvm-mingw"
    std::string compiler;      // path or name
    std::string version;       // first line of --version
    std::string problem;       // what to do when nothing was found, for the player
};

[[nodiscard]] Toolchain find_toolchain();

struct CorpusPaths {
    std::filesystem::path root;
    std::filesystem::path generated;
    std::filesystem::path objects;
    std::filesystem::path library;
    std::filesystem::path status;
    std::filesystem::path log;
};

[[nodiscard]] CorpusPaths corpus_paths(const GameRecord &game, int opt_level);
// Every level's directory for this game and this build, best first.
[[nodiscard]] std::vector<int> corpus_levels();

enum class CorpusState { None, Generating, Compiling, Linking, Ready, Failed };
[[nodiscard]] const char *state_name(CorpusState state);

struct CorpusStatus {
    CorpusState state{CorpusState::None};
    int opt_level{};
    unsigned jobs{};
    unsigned units_total{};
    unsigned units_done{};
    long pid{};                  // the compiling process, while it runs
    double generate_seconds{};
    double compile_seconds{};
    double link_seconds{};
    std::uint64_t library_bytes{};
    std::uint64_t peak_unit_rss{};  // bytes, when the platform reports it
    std::string message;
    std::string toolchain;
    std::string updated;
};

[[nodiscard]] CorpusStatus read_status(const CorpusPaths &paths);
// A compile that says it is running but whose process is gone reads as Failed.
[[nodiscard]] CorpusStatus current_status(const GameRecord &game, int opt_level);

// The best ready corpus for this build, if any: its level and library.
struct ReadyCorpus {
    int opt_level{};
    std::filesystem::path library;
};
[[nodiscard]] std::optional<ReadyCorpus> ready_corpus(const GameRecord &game);

struct CompileOptions {
    int opt_level{1};
    unsigned jobs{};              // 0: from memory and cores
    bool keep_intermediates{};    // keep generated/ and objects/
    std::function<void(const CorpusStatus &)> progress;
};

[[nodiscard]] unsigned default_jobs();

// Recompiles and compiles the game into its cache directory, updating
// status.txt as it goes. Returns 0 on success, or an exit code (app_main.cpp).
int compile_corpus(const GameRecord &game, const CompileOptions &options);

// Bytes the game's caches use, all levels and builds.
[[nodiscard]] std::uint64_t cache_bytes(const GameRecord &game);
// Removes the game's caches. Returns false with `error` set on failure.
bool clear_cache(const GameRecord &game, std::string &error);

[[nodiscard]] std::string library_extension();

} // namespace portablekit::app
