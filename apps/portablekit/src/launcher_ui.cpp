#include "launcher_ui.hpp"

#if defined(PORTABLEKIT_HAS_RENDERER)

#include "app_home.hpp"
#include "auto_profile.hpp"
#include "compile_control.hpp"
#include "corpus.hpp"
#include "game_import.hpp"
#include "keys_file.hpp"
#include "text_format.hpp"

#include "hle/hle_common.hpp"
#include "portablekit_build_label.hpp"
#include "ui/file_browser.hpp"
#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace portablekit::app {
namespace {

namespace fs = std::filesystem;
using namespace portablekit::ui;

enum class Screen { Library, Game, BrowseImage, BrowseKeys, BrowseExecutable, BrowseExport, Working, Message, ConfirmRecompile };

std::string state_text(const CompileActivity &activity) {
    if (activity.running) return activity_text(activity);
    if (activity.failure) return "Compile failed";
    if (activity.ready_level) return "Compiled (-O" + std::to_string(*activity.ready_level) + ")";
    return "Not compiled yet: plays slowly";
}

std::string keys_text() {
    const KeysReport &keys = active_keys();
    if (!keys.file_found) return "None: saves stay unencrypted";
    std::string text;
    if (keys.can_decrypt_executables) text += "executables";
    if (keys.can_encrypt_saves) text += std::string(text.empty() ? "" : ", ") + "saves";
    if (keys.tag_count != 0u) text += ", " + std::to_string(keys.tag_count) + " tag(s)";
    if (!keys.problems.empty()) text += std::string(text.empty() ? "" : "; ") + std::to_string(keys.problems.size()) + " problem(s)";
    return text.empty() ? "File found, nothing usable" : text;
}

class Launcher {
public:
    std::optional<LauncherChoice> run();

private:
    bool frame();
    void library();
    void game();
    void browse(const char *title, const char *hint, FileBrowser::Options options, Screen screen);
    void working();
    void message();
    void start_import(const fs::path &iso, std::optional<fs::path> executable);
    void start_export(const fs::path &folder);
    void show_message(std::string title, std::string text, bool offer_keys);
    void confirm_recompile();
    // The game's compile state, read from its status files at most twice a
    // second rather than every frame.
    const CompileActivity &activity(const GameRecord &game);
    void refresh_activity(const GameRecord &game) { activities_.erase(game.id); }

    std::map<std::string, std::pair<double, CompileActivity>> activities_;

    Screen screen_{Screen::Library};
    std::vector<GameRecord> games_{list_games()};
    std::size_t selected_{};
    std::optional<LauncherChoice> choice_;
    std::optional<FileBrowser> browser_;
    fs::path pending_iso_;

    std::mutex lock_;
    std::string stage_;
    std::atomic<bool> busy_{false};
    std::optional<std::string> import_error_;
    int import_error_code_{};
    std::optional<GameRecord> imported_;
    std::optional<std::string> exported_;
    std::thread worker_;

    std::string message_title_;
    std::string message_text_;
    bool message_offers_keys_{};
    bool first_{true};
    bool quit_{};
};

float font() { return Layer::get().font_size(); }

std::optional<LauncherChoice> Launcher::run() {
    Layer &layer = Layer::get();
    layer.set_interactive(true);
    layer.run([this] { return frame(); }, false);
    if (worker_.joinable()) worker_.join();
    layer.set_interactive(false);
    return choice_;
}

bool Launcher::frame() {
    Layer &layer = Layer::get();
    if (auto dropped = layer.take_dropped_file(); dropped && !busy_) {
        if (screen_ == Screen::BrowseKeys) {
            KeysReport report;
            const bool ok = import_keys_file(*dropped, report);
            show_message(ok ? "Keys added" : "Keys not added",
                         ok ? "Restart PortableKit for the keys to take effect." :
                              (report.problems.empty() ? std::string("The file was not accepted.") : report.problems.front()),
                         false);
        } else {
            start_import(*dropped, std::nullopt);
        }
    }
    switch (screen_) {
    case Screen::Library: library(); break;
    case Screen::Game: game(); break;
    case Screen::BrowseImage: {
        FileBrowser::Options options;
        browse("Choose a disc image", "Look for the .iso image of a PSP game disc.", options, screen_);
        break;
    }
    case Screen::BrowseKeys: {
        FileBrowser::Options options;
        options.extensions = {".txt", ".keys", ".ini"};
        options.filter_name = "keys files";
        options.listed_name = "keys files (.txt)";
        options.empty_note = "No folders or keys files here.";
        browse("Choose your keys file", "A text file of keys: see docs/DESKTOP_APP.md for its format.", options, screen_);
        break;
    }
    case Screen::BrowseExecutable: {
        FileBrowser::Options options;
        options.extensions = {".bin", ".elf", ".prx"};
        options.filter_name = "executables";
        options.listed_name = "executables (.bin, .elf)";
        options.empty_note = "No folders or executables here.";
        browse("Choose the decrypted executable", "EBOOT.BIN as you decrypted it from this disc.", options, screen_);
        break;
    }
    case Screen::BrowseExport: {
        FileBrowser::Options options;
        options.extensions = {};
        options.filter_name = "folders";
        options.listed_name = "folders";
        options.empty_note = "No folders here.";
        options.choose_folder = "Export into this folder";
        browse("Where to export the recompiled code", kExportNotice, options, screen_);
        break;
    }
    case Screen::Working: working(); break;
    case Screen::Message: message(); break;
    case Screen::ConfirmRecompile: confirm_recompile(); break;
    }
    return !choice_ && !quit_;
}

void Launcher::library() {
    Layer &layer = Layer::get();
    if (layer.take_back()) quit_ = true;
    begin_panel("##library", *kBuildLabel != '\0' ? std::string("PortableKit (") + kBuildLabel + ")" : "PortableKit",
                "Your games", false);
    begin_content();
    if (first_) focus_next_row();
    first_ = false;
    if (games_.empty()) {
        ImGui::Dummy({0.0f, font() * 0.3f});
        paragraph("No games yet. Add one from the disc image of your own PSP game disc (an .iso file); you can also "
                  "drop the file onto this window.",
                  colors::kTextDim);
    }
    for (std::size_t i = 0; i < games_.size(); ++i) {
        const GameRecord &record = games_[i];
        const std::string id = "##game" + std::to_string(i);
        if (list_row(id.c_str(), record.title + "  (" + display_disc_id(record.disc_id) + ")",
                     state_text(activity(record)), ListIcon::Disc)) {
            selected_ = i;
            screen_ = Screen::Game;
            first_ = true;
        }
    }
    section("");
    if (button_row("Add a game...", {false, "", "Choose the .iso disc image of a game you own."})) {
        screen_ = Screen::BrowseImage;
        browser_.reset();
    }
    if (value_row("Keys", keys_text(),
                  {false, "", "Optional. Without keys, only discs that carry an unencrypted executable can be added, "
                              "and saves are kept unencrypted."})) {
        screen_ = Screen::BrowseKeys;
        browser_.reset();
    }
    const Toolchain toolchain = find_toolchain();
    info_row("Compiler", toolchain.found ? toolchain.kind : "not found");
    info_row("Data", path_text(home_directory()));
    info_row("Compiled games", path_text(cache_root()));
    if (button_row("Quit")) quit_ = true;
    if (!toolchain.found) {
        ImGui::Dummy({0.0f, font() * 0.3f});
        paragraph(toolchain.problem, colors::kDanger);
    }
    begin_footer();
    hints({{Control::Confirm, "Select"}, {Control::Back, "Quit"}});
    end_panel();
}

void Launcher::game() {
    Layer &layer = Layer::get();
    if (layer.take_back() || selected_ >= games_.size()) {
        screen_ = Screen::Library;
        first_ = true;
        return;
    }
    const GameRecord &record = games_[selected_];
    begin_panel("##game", record.title, display_disc_id(record.disc_id), false);
    begin_content();
    if (first_) focus_next_row();
    first_ = false;
    const CompileActivity &compiling = activity(record);
    const std::optional<ReadyCorpus> ready = ready_corpus(record);
    const std::string play_note =
        ready ? "Plays compiled code." :
        compiling.running ? "Plays under the interpreter at once, slowly, while the compile goes on; the game "
                            "switches to compiled code when it is ready." :
                            "Plays under the interpreter at once, slowly, and compiles the game in the "
                            "background; the game switches to compiled code when it is ready.";
    if (button_row("Play", {false, "", play_note}, colors::kAccentBright)) choice_ = LauncherChoice{record.id, false};
    if (button_row("Play under the interpreter only",
                   {false, "", "For comparing: never loads compiled code."}))
        choice_ = LauncherChoice{record.id, true};
    section("Compiled code");
    if (compiling.running) {
        const CorpusStatus &status = compiling.status;
        info_row("Compiling", activity_text(compiling));
        const float fraction = status.state == CorpusState::Compiling && status.units_total != 0u
                                   ? static_cast<float>(status.units_done) / static_cast<float>(status.units_total)
                               : status.state == CorpusState::Linking ? 1.0f
                                                                        : static_cast<float>(std::fmod(ImGui::GetTime(), 2.0) / 2.0);
        progress_bar(fraction, "");
        if (button_row("Stop compiling",
                       {false, "", "Stops the compile. What is finished is kept: compiling again continues from it."})) {
            std::string error;
            if (!stop_compile(record, error)) show_message("Not stopped", error, false);
            refresh_activity(record);
        }
    } else if (compiling.failure) {
        info_row("State", "Compile failed");
        paragraph(*compiling.failure, colors::kDanger);
        if (button_row("Try again", {false, "", "Continues where the compile stopped: finished work is kept."})) {
            if (!start_background_compile(record, "tiered")) show_message("Not started", "The compile could not be started.", false);
            refresh_activity(record);
        }
    } else if (!compiling.ready_best) {
        info_row("State", state_text(compiling));
        if (button_row("Compile",
                       {false, "", "Compiles the game without starting it: first quickly (-O0), then fully (-O2). "
                                   "Stopped or failed, it continues where it stopped."})) {
            if (!start_background_compile(record, "tiered")) show_message("Not started", "The compile could not be started.", false);
            refresh_activity(record);
        }
    } else {
        info_row("State", state_text(compiling));
    }
    const HandProfile *hand = hand_profile_for(record.executable_sha256);
    info_row("Profile", hand != nullptr ? std::string(hand->name) + " (hand-written)" : "automatic");
    info_row("Executable", record.executable_source);
    info_row("Disc image", path_text(record.iso));
    info_row("Compiled code", human_bytes(cache_bytes(record)));
    if (button_row("Export recompiled code...", {false, "", kExportNotice})) {
        screen_ = Screen::BrowseExport;
        browser_.reset();
    }
    if (!compiling.running) {
        if (button_row("Remove compiled code", {false, "", "Frees the space; the game compiles again when played."})) {
            std::string error;
            if (!clear_cache(record, error)) show_message("Not removed", error, false);
            refresh_activity(record);
        }
        if (button_row("Recompile from scratch...",
                       {false, "", "Discards this game's compiled code and everything the compiler kept, then "
                                   "compiles it again from the beginning."})) {
            screen_ = Screen::ConfirmRecompile;
            first_ = true;
        }
    }
    if (button_row("Back")) {
        screen_ = Screen::Library;
        first_ = true;
    }
    begin_footer();
    hints({{Control::Confirm, "Select"}, {Control::Back, "Back"}});
    end_panel();
}

void Launcher::browse(const char *title, const char *hint, FileBrowser::Options options, Screen screen) {
    Layer &layer = Layer::get();
    if (!browser_) browser_.emplace(FileBrowser::home(), std::move(options));
    const bool back = layer.take_back();
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    begin_panel("##browse", title, "", false);
    begin_content();
    const FileBrowser::Result result = browser_->frame(back || ImGui::IsKeyPressed(cancel, false));
    layer.set_description(hint);
    begin_footer();
    hints({{Control::Confirm, "Open"}, {Control::Back, "Up a folder"}});
    end_panel();
    if (result == FileBrowser::Result::Cancelled) {
        screen_ = Screen::Library;
        first_ = true;
    } else if (result == FileBrowser::Result::Chosen) {
        const fs::path chosen = browser_->chosen();
        browser_.reset();
        if (screen == Screen::BrowseImage) {
            start_import(chosen, std::nullopt);
        } else if (screen == Screen::BrowseExecutable) {
            start_import(pending_iso_, chosen);
        } else if (screen == Screen::BrowseExport) {
            start_export(chosen);
        } else {
            KeysReport report;
            const bool ok = import_keys_file(chosen, report);
            std::string text = ok ? "The keys are in " + path_text(keys_file_path()) +
                                        ". Restart PortableKit for them to take effect."
                                  : std::string();
            for (const std::string &problem : report.problems) text += problem + "\n";
            show_message(ok ? "Keys added" : "Keys not added", text, false);
        }
    }
}

void Launcher::start_import(const fs::path &iso, std::optional<fs::path> executable) {
    if (worker_.joinable()) worker_.join();
    pending_iso_ = iso;
    busy_ = true;
    import_error_.reset();
    imported_.reset();
    screen_ = Screen::Working;
    worker_ = std::thread([this, iso, executable] {
        try {
            ImportOptions options;
            options.iso = iso;
            options.executable = executable;
            GameRecord record = import_game(options, [this](const std::string &stage) {
                const std::lock_guard<std::mutex> guard(lock_);
                stage_ = stage;
            });
            const std::lock_guard<std::mutex> guard(lock_);
            imported_ = std::move(record);
        } catch (const ImportError &e) {
            const std::lock_guard<std::mutex> guard(lock_);
            import_error_ = e.what();
            import_error_code_ = e.code();
        } catch (const std::exception &e) {
            const std::lock_guard<std::mutex> guard(lock_);
            import_error_ = e.what();
            import_error_code_ = 1;
        }
        busy_ = false;
    });
}

void Launcher::start_export(const fs::path &folder) {
    if (worker_.joinable()) worker_.join();
    if (selected_ >= games_.size()) return;
    const GameRecord game = games_[selected_];
    busy_ = true;
    import_error_.reset();
    imported_.reset();
    exported_.reset();
    {
        const std::lock_guard<std::mutex> guard(lock_);
        stage_ = "Recompiling the game's code into " + path_text(folder);
    }
    screen_ = Screen::Working;
    worker_ = std::thread([this, game, folder] {
        std::string message;
        const fs::path target = folder / (game.id + "-recompiled");
        const int code = export_corpus(game, target, ExportOptions{}, message);
        const std::lock_guard<std::mutex> guard(lock_);
        if (code == 0) exported_ = message;
        else {
            import_error_ = message;
            import_error_code_ = code;
        }
        busy_ = false;
    });
}

void Launcher::working() {
    std::string stage;
    {
        const std::lock_guard<std::mutex> guard(lock_);
        stage = stage_;
    }
    begin_panel("##working", "Working", "", false);
    begin_content();
    ImGui::Dummy({0.0f, font() * 0.5f});
    paragraph(stage.empty() ? "Reading the disc image" : stage);
    const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0) / 2.0);
    progress_bar(t, "");
    begin_footer();
    end_panel();
    if (busy_) return;
    const std::lock_guard<std::mutex> guard(lock_);
    if (exported_) {
        show_message("Recompiled code exported", *exported_ + ".\n\n" + kExportNotice, false);
        exported_.reset();
        return;
    }
    if (imported_) {
        games_ = list_games();
        for (std::size_t i = 0; i < games_.size(); ++i)
            if (games_[i].id == imported_->id) selected_ = i;
        screen_ = Screen::Game;
        first_ = true;
    } else {
        show_message("The game was not added", import_error_.value_or("Unknown error."), import_error_code_ == 11);
    }
}

const CompileActivity &Launcher::activity(const GameRecord &game) {
    const double now = ImGui::GetTime();
    auto found = activities_.find(game.id);
    if (found == activities_.end() || now - found->second.first > 0.5)
        found = activities_.insert_or_assign(game.id, std::make_pair(now, compile_activity(game))).first;
    return found->second.second;
}

void Launcher::confirm_recompile() {
    Layer &layer = Layer::get();
    if (selected_ >= games_.size()) {
        screen_ = Screen::Library;
        return;
    }
    const GameRecord &record = games_[selected_];
    begin_panel("##confirm", "Recompile from scratch?", record.title, false);
    begin_content();
    ImGui::Dummy({0.0f, font() * 0.4f});
    paragraph("This discards " + human_bytes(cache_bytes(record)) + " of compiled code for " + record.title +
              ", including the C++ the recompiler wrote, and compiles everything again. That can take a long time "
              "for a large game.");
    ImGui::Dummy({0.0f, font() * 0.6f});
    if (first_) focus_next_row();
    first_ = false;
    bool back = layer.take_back();
    if (button_row("Cancel")) back = true;
    if (button_row("Discard and compile", {false, "", "Removes the compiled code, then starts the compile."},
                   colors::kDanger)) {
        std::string error;
        if (!clear_cache(record, error)) show_message("Not removed", error, false);
        else if (!start_background_compile(record, "tiered"))
            show_message("Not started", "The compile could not be started.", false);
        else screen_ = Screen::Game;
        refresh_activity(record);
        first_ = true;
    }
    begin_footer();
    hints({{Control::Confirm, "Select"}, {Control::Back, "Back"}});
    end_panel();
    if (back) {
        screen_ = Screen::Game;
        first_ = true;
    }
}

void Launcher::show_message(std::string title, std::string text, bool offer_keys) {
    message_title_ = std::move(title);
    message_text_ = std::move(text);
    message_offers_keys_ = offer_keys;
    screen_ = Screen::Message;
    first_ = true;
}

void Launcher::message() {
    Layer &layer = Layer::get();
    begin_panel("##message", message_title_, "", false);
    begin_content();
    ImGui::Dummy({0.0f, font() * 0.4f});
    paragraph(message_text_);
    ImGui::Dummy({0.0f, font() * 0.6f});
    if (first_) focus_next_row();
    first_ = false;
    bool back = layer.take_back();
    if (message_offers_keys_) {
        if (button_row("Choose a keys file...")) {
            screen_ = Screen::BrowseKeys;
            browser_.reset();
        }
        if (button_row("Choose an executable I decrypted...")) {
            screen_ = Screen::BrowseExecutable;
            browser_.reset();
        }
    }
    if (button_row("Back")) back = true;
    begin_footer();
    hints({{Control::Confirm, "Select"}, {Control::Back, "Back"}});
    end_panel();
    if (back) {
        games_ = list_games();
        screen_ = Screen::Library;
        first_ = true;
    }
}

} // namespace

std::optional<LauncherChoice> run_launcher() {
    if (ensure_renderer() == nullptr || !Layer::get().attached()) return std::nullopt;
    Launcher launcher;
    return launcher.run();
}

} // namespace portablekit::app

#else

namespace portablekit::app {
std::optional<LauncherChoice> run_launcher() { return std::nullopt; }
} // namespace portablekit::app

#endif
