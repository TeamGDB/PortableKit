#pragma once

#include <filesystem>

// The launcher: shown before the game starts, in the game's own look
// (GameProfile::launcher). A side bar with the game's logo and the menu's
// pages, a home page with a banner and quick settings, and a Play button.
// Everything the in-game menu sets can be set here before anything of the
// game has run.
namespace portablekit::ui {

enum class LauncherChoice {
    Play,   // start the game
    Quit,   // the player closed the window
    SetUp,  // the player asked to set up game data again
};

// Shows the launcher until the player chooses. Returns Play at once when the
// profile has no launcher, the player chose to skip it (unless `always`),
// <prefix>_NO_LAUNCHER is set, or there is no interface to draw it with.
LauncherChoice run_launcher(const std::filesystem::path &disc_image, bool always);

// Defined in menu.cpp, for the launcher.
struct LauncherPage {
    bool back_used{};  // the back button closed something inside the page
    bool set_up{};     // the player asked to set up game data again
};
// The in-game menu's page `tab` (Video, Audio, Controls, Network, System)
// drawn into the current window; `back`: the back button was pressed.
LauncherPage launcher_page(int tab, bool back);
// The rows of the Video page that matter before the game starts.
void launcher_quick_settings();

} // namespace portablekit::ui
