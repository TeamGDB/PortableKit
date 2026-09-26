#pragma once

// Loads the game's compiled corpus into the runtime: at start when one is
// ready, and while the game runs when a compile finishes (docs/DESKTOP_APP.md,
// "Switching to compiled code").

#include "game_import.hpp"

#include <optional>
#include <string>

namespace portablekit::app {

struct CorpusChoice {
    bool interpreter_only{};   // never load compiled code (portablekit run --interpreter)
    bool watch{true};          // switch to a better corpus when a compile finishes
};

// Called before the port starts; register_generated_functions() then reads it.
void prepare_corpus_loading(const GameRecord &game, const CorpusChoice &choice);

struct LoadedCorpus {
    int opt_level{-1};         // -1: none, the game runs under the interpreter
    double register_seconds{};
    unsigned switches{};       // corpora loaded while the game ran
};
[[nodiscard]] LoadedCorpus loaded_corpus();

// Stops the running game at the next dispatch boundary, the way the game's
// own end would: the port then reports its threads before returning.
void request_stop();

// A line for the status overlay and the log, e.g. "Compiling 45% - running
// under the interpreter".
[[nodiscard]] std::string corpus_status_line();

} // namespace portablekit::app
