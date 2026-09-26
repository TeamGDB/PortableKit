#pragma once

// The GameProfile the framework asks for (host/profile.hpp), made at run time
// for whichever game the player chose, instead of written by hand per game.
//
// What the disc and the executable say is read from them; what nobody can
// know in advance gets a default that holds for most games (docs/DESKTOP_APP.md,
// "The automatic profile"). A hand-written profile, when one carried by this
// build matches the executable's hash, is used instead, with only the names
// that decide where the app keeps things replaced by the app's own.

#include "game_import.hpp"

#include "profile.hpp"

#include <string>
#include <vector>

namespace portablekit::app {

struct HandProfile {
    const char *name;                 // the source file's name, e.g. "purun_profile"
    const GameProfile &(*get)();
};

// Every hand-written profile this build carries (PORTABLEKIT_APP_PROFILES).
[[nodiscard]] std::vector<HandProfile> hand_profiles();
[[nodiscard]] const HandProfile *hand_profile_for(const std::string &executable_sha256);

// Makes game() describe the launcher, before a game is chosen.
void activate_launcher_profile();
// Makes game() describe this game. Returns the name of the hand profile used,
// or "auto".
std::string activate_game_profile(const GameRecord &record);

// What the automatic profile decided and why, for `portablekit info` and the
// compatibility report.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> describe_active_profile();

} // namespace portablekit::app
