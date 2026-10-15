#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The save list of the PSP's save-data dialog: which save to load, to save
// over or to delete, drawn over the running game when the game asks for a
// list (sceUtilitySavedata LISTLOAD, LISTSAVE, LISTDELETE).
//
// Gamepad and keyboard: up and down choose a save, confirm takes it, back
// cancels. Saving over a save or deleting one asks first. The mouse can click
// the rows and the answers.
namespace portablekit::ui {

enum class SaveChooserMode { Load, Save, Delete };

struct SaveSlot {
    std::string name;  // the save name the game listed, e.g. "DATA03"
    bool exists{};     // false: the free slot a new save goes to
    std::string title;   // PARAM.SFO SAVEDATA_TITLE
    std::string detail;  // PARAM.SFO SAVEDATA_DETAIL
    std::string date;    // when it was last saved; empty for a new save
    std::filesystem::path icon;  // ICON0.PNG; empty: none
};

struct SaveChooserRequest {
    SaveChooserMode mode{SaveChooserMode::Load};
    std::string game_title;       // shown beside the panel's title
    std::vector<SaveSlot> slots;  // in the order shown; for saving, a new save first
    std::size_t focus{};          // the row focused when the list opens
};

// Called once, as soon as the player has chosen: the chosen save's name, or
// nullopt when the player cancelled.
using SaveChooserDone = std::function<void(std::optional<std::string>)>;

// Opens the list over the running game. The game keeps running, as it does
// behind the PSP's dialog, but reads a neutral pad, and the window keeps the
// frame that was on screen. False when there is no interface to draw it in.
bool open_game_save_chooser(SaveChooserRequest request, SaveChooserDone on_done);
// Open from the moment it is shown until the result of the choice has been
// shown too.
[[nodiscard]] bool save_chooser_open();
// How the save or delete the player chose went, to show before the list
// closes.
void save_chooser_report(bool ok);
// Builds this frame of the list and handles its input. Call inside an
// interface frame.
void save_chooser_frame();

} // namespace portablekit::ui
