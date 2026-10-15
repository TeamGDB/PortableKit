#include "ui/texture_pack_screen.hpp"

#include "ui/file_browser.hpp"
#include "ui/i18n.hpp"
#include "ui/layer.hpp"
#include "ui/save_screen.hpp"
#include "ui/widgets.hpp"

#include "gpu/texture_pack_import.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "profile.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#if defined(PORTABLEKIT_ANDROID_APP)
#include "platform/android_documents.hpp"
#include "platform/android_jni.hpp"
#endif

#include "imgui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace portablekit::ui {
namespace {

namespace fs = std::filesystem;
using gpu::TexturePackCheck;
using gpu::TexturePackCopy;
using gpu::TexturePackLocation;

enum class Stage { Closed, Choose, Checking, Review, Copying, Result };

// What the review screen shows: the pack found, and the one in use now.
struct Review {
    TexturePackCheck check;
    TexturePackLocation current;
    gpu::InstalledTexturePack current_pack;
    std::optional<std::uint64_t> free_space;
    bool is_current{};  // the pack chosen is the one in use
};

struct Outcome {
    bool ok{};
    bool in_place{};
    bool cancelled{};
    std::string error;
    fs::path folder;  // where the pack is read from now
    fs::path backup;  // where the pack it replaced went
    std::size_t keys{};
};

#if defined(PORTABLEKIT_ANDROID_APP)
// Android: a pack comes from the system's document picker as a tree of
// content:// documents, so it is copied first and checked in the copy, then
// put in place like any other. The copy goes to
// textures/.incomplete-<time>/<the pack folder's name>, so the check sees the
// name the pack was made under.
class DocumentPackCopy {
public:
    using State = TexturePackCopy::State;
    ~DocumentPackCopy() {
        cancel();
        join();
    }
    void start(std::string tree, fs::path textures_root) {
        join();
        cancel_ = false;
        {
            std::lock_guard lock(mutex_);
            progress_ = {};
            error_.clear();
            made_for_.clear();
            keys_ = 0u;
        }
        root_ = gpu::new_texture_pack_staging(textures_root);
        staging_.clear();
        state_ = State::Copying;
        thread_ = std::thread([this, tree = std::move(tree), textures_root = std::move(textures_root)] {
            run(tree, textures_root);
        });
    }
    void cancel() noexcept { cancel_ = true; }
    void join() {
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] State state() const noexcept { return state_.load(); }
    [[nodiscard]] TexturePackCopy::Progress progress() const {
        std::lock_guard lock(mutex_);
        return progress_;
    }
    [[nodiscard]] std::string error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }
    // Once Done: the checked copy, and the folder holding it.
    [[nodiscard]] const fs::path &staging() const noexcept { return staging_; }
    [[nodiscard]] const fs::path &root() const noexcept { return root_; }
    [[nodiscard]] std::size_t keys() const noexcept { return keys_; }
    [[nodiscard]] std::string made_for() const {
        std::lock_guard lock(mutex_);
        return made_for_;
    }

private:
    void fail(const std::string &error) {
        std::cout << "[texpack] import failed: " << error << std::endl;
        gpu::discard_staged_texture_pack(root_);
        std::lock_guard lock(mutex_);
        error_ = error;
        state_ = State::Failed;
    }

    void run(const std::string &tree, const fs::path &textures_root) {
        std::string error;
        const std::string game_id = portablekit::game().disc_id;
        {
            std::lock_guard lock(mutex_);
            progress_.current = "Looking for the pack…";
        }
        const std::optional<android::FoundPack> found = android::find_texture_pack(
            tree, gpu::texture_pack_folder_names(game_id), &gpu::texture_pack_file_skipped, error);
        if (!found) return fail(error);
        std::uint64_t total = 0u;
        for (const android::PackFile &file : found->files) total += file.size;
        std::cout << "[texpack] import from " << found->where << ": " << found->name << ", " << found->files.size()
                  << " files, " << (total >> 20u) << " MB" << std::endl;
        if (const auto free = gpu::texture_pack_free_space(textures_root);
            free && *free < total + gpu::kTexturePackSpaceMargin)
            return fail("Not enough free space: the pack needs " + human_size(total + gpu::kTexturePackSpaceMargin) +
                        " and " + human_size(*free) + " is free.");
        {
            std::lock_guard lock(mutex_);
            progress_.total_bytes = total;
            progress_.total_files = found->files.size();
        }
        const fs::path staging = root_ / fs::path(std::u8string(found->name.begin(), found->name.end()));
        std::uint64_t done = 0u;
        for (std::size_t i = 0; i < found->files.size(); ++i) {
            if (cancel_) {
                gpu::discard_staged_texture_pack(root_);
                std::cout << "[texpack] copy cancelled" << std::endl;
                state_ = State::Cancelled;
                return;
            }
            const android::PackFile &file = found->files[i];
            {
                std::lock_guard lock(mutex_);
                progress_.current = file.relative;
            }
            const fs::path target = staging / fs::path(std::u8string(file.relative.begin(), file.relative.end()));
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            if (ec) return fail("cannot create " + install::path_to_utf8(target.parent_path()) + ": " + ec.message());
            if (!android::copy_document(file.uri, target, error)) return fail(error);
            done += file.size;
            std::lock_guard lock(mutex_);
            progress_.bytes = done;
            progress_.files = i + 1u;
        }
        // Checked in the copy, as the desktop checks the folder chosen.
        const TexturePackCheck check = gpu::check_texture_pack(staging, game_id);
        if (!check.ok()) return fail(check.problem.empty() ? "the copied pack does not load" : check.problem);
        staging_ = check.folder;
        keys_ = check.keys;
        {
            std::lock_guard lock(mutex_);
            made_for_ = check.made_for;
        }
        std::cout << "[texpack] copied " << found->files.size() << " files (" << (done >> 20u) << " MB) to "
                  << install::path_to_utf8(staging_) << ", " << keys_ << " keys" << std::endl;
        state_ = State::Done;
    }

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    TexturePackCopy::Progress progress_;
    std::string error_;
    std::string made_for_;
    fs::path root_;
    fs::path staging_;
    std::size_t keys_{};
};
#endif

struct State {
    Stage stage{Stage::Closed};
    std::unique_ptr<FileBrowser> browser;
    fs::path last_folder;
    std::future<Review> checking;
    fs::path checking_folder;
    bool cancel_requested{};
    Review review;
    TexturePackCopy copy;
#if defined(PORTABLEKIT_ANDROID_APP)
    DocumentPackCopy document_copy;
    bool from_document{};  // the copy running is document_copy
#endif
    bool installing{};  // the copy is done; waiting for the renderer to let go of the old pack
    Outcome outcome;
    bool focus{};
    bool focus_row{};
    std::chrono::steady_clock::time_point copy_started;
};

State &state() {
    static State s;
    return s;
}

// The copy running, whichever kind it is.
TexturePackCopy::State copy_state() {
#if defined(PORTABLEKIT_ANDROID_APP)
    if (state().from_document) return state().document_copy.state();
#endif
    return state().copy.state();
}

TexturePackCopy::Progress copy_progress() {
#if defined(PORTABLEKIT_ANDROID_APP)
    if (state().from_document) return state().document_copy.progress();
#endif
    return state().copy.progress();
}

void copy_cancel() {
#if defined(PORTABLEKIT_ANDROID_APP)
    if (state().from_document) return state().document_copy.cancel();
#endif
    state().copy.cancel();
}

void copy_join() {
#if defined(PORTABLEKIT_ANDROID_APP)
    if (state().from_document) return state().document_copy.join();
#endif
    state().copy.join();
}

std::string copy_error() {
#if defined(PORTABLEKIT_ANDROID_APP)
    if (state().from_document) return state().document_copy.error();
#endif
    return state().copy.error();
}

float px(float value) { return std::round(value * Layer::get().scale()); }

std::string utf8(const fs::path &path) { return install::path_to_utf8(path); }

gpu::VulkanRenderer &renderer() { return Layer::get().renderer(); }

fs::path textures_root() { return gpu::VulkanRenderer::textures_root(); }

fs::path installed_folder() { return textures_root() / portablekit::game().disc_id; }

void go(Stage stage) {
    State &s = state();
    s.stage = stage;
    s.focus = true;
    ImGui::SetScrollY(0.0f);
}

void close() {
    State &s = state();
    s.stage = Stage::Closed;
    s.browser.reset();
    s.focus_row = true;
}

void focus_first() {
    State &s = state();
    if (s.focus) {
        focus_next_row();
        s.focus = false;
    }
}

void indented(const std::string &text, ImU32 color = colors::kTextDim) {
    ImGui::Indent(px(16.0f));
    paragraph(text, color);
    ImGui::Unindent(px(16.0f));
}

std::string count(std::size_t n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1u ? one : many);
}

void start_document_copy();

void open_browser() {
#if defined(PORTABLEKIT_ANDROID_APP)
    // The system's picker reaches Downloads, SD cards and other apps'
    // folders, which the game cannot list by itself.
    start_document_copy();
#else
    State &s = state();
    FileBrowser::Options options;
    options.extensions = {};
    options.filter_name = "folders";
    options.listed_name = "folders";
    options.empty_note = "No folders here.";
    options.choose_folder = "Import from this folder";
    // A pack folder is chosen as soon as it is opened.
    options.choose_on_open = [](const fs::path &folder) {
        std::error_code ec;
        return fs::is_regular_file(folder / "textures.ini", ec);
    };
    options.system_first = true;
    fs::path start = s.last_folder;
    if (start.empty()) {
        start = FileBrowser::home() / "Downloads";
        std::error_code ec;
        if (!fs::is_directory(start, ec)) start = FileBrowser::home();
    }
    s.browser = std::make_unique<FileBrowser>(start, std::move(options));
    go(Stage::Choose);
#endif
}

void start_check(const fs::path &chosen) {
    State &s = state();
    const std::string in_place = settings::current().texture_pack_folder;
    // Reading a large pack's folder and textures.ini takes a moment on a slow
    // card; the menu keeps drawing meanwhile.
    s.checking_folder = chosen;
    s.checking = std::async(std::launch::async, [chosen, in_place, root = textures_root()] {
        Review review;
        review.check = gpu::check_texture_pack(chosen, portablekit::game().disc_id);
        review.current = gpu::texture_pack_location(root, portablekit::game().disc_id, in_place);
        review.current_pack = gpu::summarize_texture_pack(review.current.folder, portablekit::game().disc_id);
        review.free_space = gpu::texture_pack_free_space(root);
        std::error_code ec;
        review.is_current = review.check.found() && fs::exists(review.current.folder, ec) &&
                            fs::equivalent(review.check.folder, review.current.folder, ec);
        return review;
    });
    go(Stage::Checking);
}

void log_review(const Review &r) {
    const TexturePackCheck &c = r.check;
    std::cout << "[texpack] import from " << utf8(c.chosen) << ": ";
    if (!c.found()) {
        std::cout << "no pack found\n";
        return;
    }
    std::cout << utf8(c.folder) << ", " << c.keys << " keys, " << c.images << " images, " << c.files << " files, "
              << (c.bytes >> 20u) << " MB, " << c.missing << " missing"
              << (c.made_for.empty() ? "" : ", made for " + c.made_for)
              << (c.ok() ? "" : "; refused: " + c.problem) << std::endl;
}

bool browse(bool back) {
    State &s = state();
    indented("Import: open the pack's folder, the one that holds textures.ini, or choose a folder that holds it, "
             "such as PPSSPP's PSP/TEXTURES. You can also drop the folder on the window.");
    // A folder dropped on the window is chosen at once.
    if (auto dropped = Layer::get().take_dropped_file()) {
        s.last_folder = s.browser->folder();
        s.browser.reset();
        std::error_code ec;
        start_check(fs::is_directory(*dropped, ec) ? *dropped : dropped->parent_path());
        return true;
    }
    const FileBrowser::Result result = s.browser->frame(back);
    if (result == FileBrowser::Result::Browsing) return true;
    s.last_folder = s.browser->folder();
    const fs::path chosen = s.browser->chosen();
    s.browser.reset();
    if (result == FileBrowser::Result::Cancelled) {
        close();
        return false;
    }
    start_check(chosen);
    return true;
}

void checking_screen(bool back) {
    State &s = state();
    if (back) {
        // The check reads only; let it finish on its own.
        open_browser();
        return;
    }
    section("Import texture pack");
    focus_first();
    indented("Checking " + utf8(s.checking_folder) + "…");
    const double t = ImGui::GetTime();
    progress_bar(static_cast<float>(0.5 + 0.5 * std::sin(t * 3.0)), "");
    if (s.checking.valid() &&
        s.checking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        s.review = s.checking.get();
        log_review(s.review);
        go(Stage::Review);
    }
}

std::string describe_current(const Review &r) {
    if (!r.current_pack.exists) {
        if (r.current.source == TexturePackLocation::Source::Installed) return "None";
        return "Folder missing: " + utf8(r.current.folder);
    }
    if (!r.current_pack.problem.empty()) return trf("Does not load, {}", {human_size(r.current_pack.bytes)});
    const std::string text =
        trf("{} textures, {}", {std::to_string(r.current_pack.keys), human_size(r.current_pack.bytes)});
    return text;
}

// Turns the Texture pack setting on, unless <prefix>_TEXTURE_PACK decides it.
void enable_pack(settings::Settings &settings) {
    if (!settings::overridden_by("video.texture_pack").empty()) return;
    settings.texture_pack = true;
    renderer().set_texture_pack(true);
}

void finish(Outcome outcome) {
    State &s = state();
    s.outcome = std::move(outcome);
    go(Stage::Result);
}

void use_in_place() {
    State &s = state();
    settings::Settings &settings = settings::current();
    std::error_code ec;
    const bool is_installed = fs::equivalent(s.review.check.folder, installed_folder(), ec);
    settings.texture_pack_folder = is_installed ? std::string{} : utf8(s.review.check.folder);
    enable_pack(settings);
    settings::save();
    renderer().reload_texture_pack();
    std::cout << "[texpack] using the pack in place: " << utf8(s.review.check.folder) << std::endl;
    Outcome outcome;
    outcome.ok = true;
    outcome.in_place = true;
    outcome.folder = s.review.check.folder;
    outcome.keys = s.review.check.keys;
    finish(std::move(outcome));
}

void start_document_copy() {
#if defined(PORTABLEKIT_ANDROID_APP)
    State &s = state();
    const std::optional<std::string> tree = android::pick_folder();
    if (!tree) {
        close();
        return;
    }
    std::error_code ec;
    fs::create_directories(textures_root(), ec);
    s.from_document = true;
    s.installing = false;
    s.cancel_requested = false;
    s.copy_started = std::chrono::steady_clock::now();
    s.document_copy.start(*tree, textures_root());
    go(Stage::Copying);
#endif
}

void start_copy() {
    State &s = state();
#if defined(PORTABLEKIT_ANDROID_APP)
    s.from_document = false;
#endif
    std::error_code ec;
    fs::create_directories(textures_root(), ec);
    s.installing = false;
    s.cancel_requested = false;
    s.copy_started = std::chrono::steady_clock::now();
    s.copy.start(s.review.check, textures_root());
    go(Stage::Copying);
}

// What was found in the chosen folder, in a few lines, and what can be done
// with it: the actions come first, the pack in use now after them.
void review_screen(bool back) {
    State &s = state();
    if (back) {
        open_browser();
        return;
    }
    const Review &r = s.review;
    const TexturePackCheck &c = r.check;
    const std::string id = portablekit::game().disc_id;
    ImGui::Indent(px(16.0f));
    ImGui::Dummy({0.0f, px(4.0f)});
    heading(!c.found() ? "No texture pack in this folder" : c.ok() ? "Ready to install" : "This pack cannot be used");
    ImGui::Dummy({0.0f, px(2.0f)});
    paragraph(utf8(c.found() ? c.folder : c.chosen), colors::kTextDim);
    ImGui::Unindent(px(16.0f));
    ImGui::Dummy({0.0f, px(6.0f)});
    if (!c.found() || !c.ok()) indented(c.problem, colors::kDanger);
    if (c.found()) {
        // One line: how big, and the release it was made for.
        std::string summary = trf("{} textures, {}", {std::to_string(c.keys), human_size(c.bytes)});
        if (!c.made_for.empty()) summary += "  ·  " + trf("made for {}, works on {}", {c.made_for, id});
        indented(summary, colors::kText);
        if (c.missing > 0u)
            indented(trf("{} images missing: those textures keep the game's own look.", {std::to_string(c.missing)}),
                     colors::kDanger);
    }

    if (c.ok()) {
        section("Install");
        const std::uint64_t needed = c.bytes + gpu::kTexturePackSpaceMargin;
        const bool room = !r.free_space || *r.free_space >= needed;
        std::error_code ec;
        const bool installed_exists = fs::exists(installed_folder(), ec);
        const bool is_installed = r.is_current && r.current.source == TexturePackLocation::Source::Installed;
        // Kept to two lines: the footer has no room for a path.
        std::string copy_note =
            r.free_space ? trf("Copies {} to mods/textures/{} in the data folder ({} free).",
                               {human_size(c.bytes), id, human_size(*r.free_space)})
                         : trf("Copies {} to mods/textures/{} in the data folder.", {human_size(c.bytes), id});
        if (installed_exists) copy_note += " " + std::string(tr("The pack there now is kept as a backup."));
        if (!room)
            copy_note = trf("Not enough free space: the copy needs {} and {} is free. Use it where it is instead.",
                            {human_size(needed), human_size(*r.free_space)});
        if (is_installed) copy_note = "This is the installed pack already.";
        const char *label = installed_exists ? "Copy and replace" : "Copy into the data folder";
        // The focus starts on the first thing the player can do.
        if (room && !is_installed) focus_first();
        if (button_row(label, {!room || is_installed, {}, copy_note}, colors::kAccentBright)) start_copy();
        const bool in_use = r.is_current && r.current.source != TexturePackLocation::Source::Variable;
        const std::string place_note =
            in_use ? std::string("This pack is the one in use already.")
                   : trf("Reads the pack from its folder and copies nothing, saving {}. The folder must stay where "
                         "it is.",
                         {human_size(c.bytes)});
        focus_first();
        if (button_row("Use it where it is", {in_use, {}, place_note})) use_in_place();
    }
    ImGui::Dummy({0.0f, px(6.0f)});
    focus_first();
    if (button_row("Choose another folder", {false, {}, "Back to the folders."})) open_browser();
    if (button_row("Cancel", {false, {}, "Import nothing."})) close();

    // The pack in use now, if any: what an install replaces.
    if (r.current_pack.exists || r.current.source != TexturePackLocation::Source::Installed) {
        section("In use now");
        info_row(r.current.source == TexturePackLocation::Source::Installed ? "Installed pack" : "Pack folder",
                 describe_current(r));
        if (r.current.source != TexturePackLocation::Source::Installed)
            info_row("Used from", utf8(r.current.folder));
        if (r.current.source == TexturePackLocation::Source::Variable)
            indented(portablekit::env_name("TEXTURE_PACK") +
                         " names this folder, so it stays in use until the variable is unset.",
                     colors::kDanger);
    }
}

void copying_screen(bool back) {
    State &s = state();
    if (back) {
        copy_cancel();
        s.cancel_requested = true;
    }
    const TexturePackCopy::Progress p = copy_progress();
    section(s.installing ? "Putting the pack in place" : "Copying texture pack");
    ImGui::Dummy({0.0f, px(6.0f)});
    const float fraction =
        p.total_bytes > 0u ? static_cast<float>(static_cast<double>(p.bytes) / static_cast<double>(p.total_bytes))
                           : 0.0f;
    char text[128];
    std::snprintf(text, sizeof(text), "%d%%   %s of %s", static_cast<int>(fraction * 100.0f),
                  human_size(p.bytes).c_str(), human_size(p.total_bytes).c_str());
    ImGui::Indent(px(16.0f));
    progress_bar(fraction, text);
    ImGui::Unindent(px(16.0f));
    if (p.total_files == 0u && !p.current.empty())
        indented(tr(p.current));  // an Android import still looking through the picked folder
    else
        indented(std::to_string(p.files) + " of " + std::to_string(p.total_files) + " files" +
                 (p.current.empty() ? "" : ": " + p.current));
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s.copy_started).count();
    if (fraction > 0.02f && seconds > 2.0) {
        char eta[64];
        std::snprintf(eta, sizeof(eta), "About %d s left.",
                      static_cast<int>(std::ceil(seconds * (1.0 - fraction) / fraction)));
        indented(eta);
    }
    indented("The pack in use now stays as it is until the copy is complete. The menu stays open until then.");
    ImGui::Dummy({0.0f, px(12.0f)});
    focus_first();
    if (button_row(s.cancel_requested ? "Cancelling…" : "Cancel",
                   {s.installing || s.cancel_requested, {},
                    "Stops the copy and removes what it has copied so far. Nothing else changes."},
                   colors::kDanger)) {
        copy_cancel();
        s.cancel_requested = true;
    }
}

void result_screen(bool back) {
    State &s = state();
    if (back) {
        close();
        return;
    }
    const Outcome &o = s.outcome;
    section("Import texture pack");
    if (o.ok) {
        info_row("Installed", count(o.keys, "texture", "textures") + (o.in_place ? ", used where it is" : ""));
        info_row(o.in_place ? "Used from" : "Installed in", utf8(o.folder));
        if (!o.backup.empty()) info_row("Replaced pack", "Kept in " + utf8(o.backup));
        const std::string status = renderer().texture_pack_status();
        info_row("Texture pack", status);
        indented("The pack is on and applies at once: textures already on screen change within a frame or two.");
    } else if (o.cancelled) {
        info_row("Not imported", "Cancelled");
        indented("Nothing changed: the pack in use before is still in place.");
    } else {
        info_row("Not imported", o.error);
        indented("Nothing changed: the pack in use before is still in place.");
    }
    ImGui::Dummy({0.0f, px(12.0f)});
#if !defined(PORTABLEKIT_ANDROID_APP)
    if (button_row("Open the textures folder", {false, {}, "Show the folder the pack is in."}))
        open_folder(o.ok && o.in_place ? o.folder : textures_root());
#endif
    focus_first();
    if (button_row("Done", {false, {}, "Back to the menu."})) close();
}

// Advances a copy whatever page the menu shows: once it is done, the old
// pack is closed, the folders swap, and the new pack opens.
void tick() {
    State &s = state();
    if (s.stage != Stage::Copying) return;
    switch (copy_state()) {
    case TexturePackCopy::State::Copying:
    case TexturePackCopy::State::Idle: return;
    case TexturePackCopy::State::Cancelled: {
        copy_join();
        Outcome outcome;
        outcome.cancelled = true;
        finish(std::move(outcome));
        return;
    }
    case TexturePackCopy::State::Failed: {
        copy_join();
        Outcome outcome;
        outcome.error = copy_error();
        finish(std::move(outcome));
        return;
    }
    case TexturePackCopy::State::Done: break;
    }
    copy_join();
    // The renderer closes the pack at its next frame; the folder moves after.
    if (!s.installing) {
        s.installing = true;
        renderer().hold_texture_pack(true);
    }
    if (!renderer().texture_pack_held()) return;
    Outcome outcome;
    std::string error;
    const fs::path backup_dir = gpu::texture_pack_backup_directory(textures_root(), std::chrono::system_clock::now());
    fs::path staging = s.copy.staging();
    fs::path staging_root = staging;  // what a failure removes
    std::size_t keys = s.review.check.keys;
#if defined(PORTABLEKIT_ANDROID_APP)
    if (s.from_document) {
        staging = s.document_copy.staging();
        staging_root = s.document_copy.root();
        keys = s.document_copy.keys();
    }
#endif
    if (gpu::install_staged_texture_pack(staging, textures_root(), portablekit::game().disc_id, backup_dir,
                                         outcome.backup, error)) {
        // A document copy sat one folder down; that folder is empty now.
        std::error_code ec;
        if (staging_root != staging) fs::remove(staging_root, ec);
        settings::Settings &settings = settings::current();
        settings.texture_pack_folder.clear();
        enable_pack(settings);
        settings::save();
        outcome.ok = true;
        outcome.folder = installed_folder();
        outcome.keys = keys;
        std::cout << "[texpack] installed " << utf8(outcome.folder)
                  << (outcome.backup.empty() ? "" : "; the replaced pack is in " + utf8(outcome.backup)) << std::endl;
    } else {
        gpu::discard_staged_texture_pack(staging_root);
        outcome.error = error;
        std::cout << "[texpack] import failed: " << error << std::endl;
    }
    renderer().hold_texture_pack(false);
    renderer().reload_texture_pack();
    s.installing = false;
    finish(std::move(outcome));
}

} // namespace

void texture_pack_rows() {
    State &s = state();
    if (s.focus_row) {
        focus_next_row();
        s.focus_row = false;
    }
    settings::Settings &settings = settings::current();
#if defined(PORTABLEKIT_ANDROID_APP)
    const std::string import_note =
        trf("Pick the pack's folder (the one that holds textures.ini, or one that holds it in PSP/TEXTURES/{}) in "
            "Android's file picker. It is copied into the game's data folder and checked; the pack it replaces is "
            "kept.",
            {portablekit::game().disc_id});
#else
    const std::string import_note =
        trf("Install an HD texture pack from a folder: the one that holds textures.ini, or one that holds it in "
            "textures/{} or PSP/TEXTURES. It is checked first; the pack it replaces is kept.",
            {portablekit::game().disc_id});
#endif
    if (button_row("Import texture pack…", {false, {}, import_note})) open_browser();
#if !defined(PORTABLEKIT_ANDROID_APP)
    if (button_row("Open the textures folder",
                   {false, {}, "Show mods/textures in the data folder, where imported packs go."}))
        open_folder(textures_root());
#endif
    if (!settings.texture_pack_folder.empty()) info_row("Pack used from", settings.texture_pack_folder);
    if (!settings.texture_pack_folder.empty() &&
        button_row("Stop using the pack folder",
                   {false, {},
                    "Go back to the pack installed in the data folder, if any. The pack's own folder is left "
                    "alone."})) {
        settings.texture_pack_folder.clear();
        settings::save();
        renderer().reload_texture_pack();
    }
}

bool texture_pack_screen_open() { return state().stage != Stage::Closed; }

bool texture_pack_import_busy() { return state().stage == Stage::Copying; }

bool texture_pack_screen(bool back) {
    State &s = state();
    switch (s.stage) {
    case Stage::Closed: return false;
    case Stage::Choose: return browse(back);
    case Stage::Checking: checking_screen(back); break;
    case Stage::Review: review_screen(back); break;
    case Stage::Copying: copying_screen(back); break;
    case Stage::Result: result_screen(back); break;
    }
    return true;
}

void texture_pack_import_tick() { tick(); }

} // namespace portablekit::ui
