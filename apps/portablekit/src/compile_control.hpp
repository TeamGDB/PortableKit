#pragma once

// Compiling a game from the library window: starting the same background
// compile as `portablekit compile <game> --opt tiered`, following it through
// status.txt, and stopping it.

#include "corpus.hpp"

#include <optional>
#include <string>

namespace portablekit::app {

struct CompileActivity {
    bool running{};
    bool ready_best{};                 // compiled at the best level there is
    std::optional<int> ready_level;     // the best compiled level, if any
    CorpusStatus status;               // the running compile's, or the failed one's
    std::optional<std::string> failure;  // why the last compile stopped, when it did
};

// What is happening to the game's compiled code now.
[[nodiscard]] CompileActivity compile_activity(const GameRecord &game);

// "Recompiling to C++", "Compiling 120/385", "Linking", with the elapsed time.
[[nodiscard]] std::string activity_text(const CompileActivity &activity);

// Starts this program's `compile <game> --opt <level>` detached, logging to
// the game's cache directory. False if it could not start.
bool start_background_compile(const GameRecord &game, const std::string &level, unsigned jobs = 0u);

// Stops the game's running compile and everything it started (the recompiler,
// the compilers). What was finished stays: the next compile continues from it.
// False with `error` set when there was nothing to stop or it could not be
// stopped.
bool stop_compile(const GameRecord &game, std::string &error);

} // namespace portablekit::app
