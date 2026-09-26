#pragma once

// The library window: the games added so far and what state each is in, adding
// a game from its disc image, the keys file, and starting a game. It is a
// front end over the same functions the command line calls (app_main.cpp).

#include <optional>
#include <string>

namespace portablekit::app {

struct LauncherChoice {
    std::string game_id;
    bool interpreter_only{};
};

// Runs the window until the player starts a game (returned) or quits
// (nothing). Also nothing when no window can be opened.
std::optional<LauncherChoice> run_launcher();

} // namespace portablekit::app
