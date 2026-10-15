// The PSP's save list, drawn over the running game (ui/save_chooser.hpp).
#include "ui/save_chooser.hpp"

#include "ui/i18n.hpp"
#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"

#include "imgui.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace portablekit::ui {
namespace {

float font() { return Layer::get().font_size(); }
float px(float value) { return std::round(value * Layer::get().scale()); }

struct Icon {
    std::uint64_t id{};  // for ImGui; zero: none
};

// A save's ICON0.PNG as an interface image. The renderer keeps every image it
// makes until it is destroyed, so each version of a file is made once.
Icon icon_for(const std::filesystem::path &path) {
    static std::map<std::string, Icon> made;
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    // libc++ counts file times in a 128-bit integer, which to_string has no
    // overload for.
    const auto stamp = static_cast<long long>(written.time_since_epoch().count());
    const std::string key = path.string() + "|" + std::to_string(stamp);
    if (const auto found = made.find(key); found != made.end()) return found->second;
    Icon icon;
    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, channels = 0;
    if (!bytes.empty()) {
        if (stbi_uc *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                                    &channels, 4)) {
            if (width > 0 && height > 0 && width <= 512 && height <= 512)
                icon.id = Layer::get().renderer().create_ui_image(static_cast<std::uint32_t>(width),
                                                                  static_cast<std::uint32_t>(height),
                                                                  reinterpret_cast<const std::uint32_t *>(pixels));
            stbi_image_free(pixels);
        }
    }
    made[key] = icon;  // a file that cannot be read is not tried again
    return icon;
}

using Clock = std::chrono::steady_clock;

struct Chooser {
    bool open{};
    SaveChooserRequest request;
    SaveChooserDone on_done;
    std::vector<Icon> icons;
    // Focus request.focus on the next frame.
    bool refocus{};
    // The save the question (save over it, delete it) is about.
    std::optional<std::size_t> asking;
    bool ask_focus{};
    // After the choice the answer is given at once, and the panel says how it
    // went until close_at; the frame stays held all the while, so the game
    // has drawn its scene again by the time it shows. Without that, the
    // first frames after the dialog showed the half-drawn screen the game
    // leaves behind the PSP's dialog.
    bool closing{};
    std::string closing_text;  // empty: nothing to say (a cancel)
    Clock::time_point close_at;
};

Chooser &chooser() {
    static Chooser value;
    return value;
}

// Gives the game back its pad and its frames.
void close_now() {
    Chooser &c = chooser();
    if (!c.open) return;
    c.open = false;
    c.closing = false;
    Layer &layer = Layer::get();
    layer.set_interactive(false);
    layer.renderer().set_game_input(true);
    layer.renderer().hold_frame(false);
}

// Answers the game, then shows how it went for a moment before closing.
void finish(std::optional<std::string> chosen) {
    Chooser &c = chooser();
    if (!c.open || c.closing) return;
    c.closing = true;
    const Clock::time_point now = Clock::now();
    if (!chosen) {
        c.close_at = now + std::chrono::milliseconds(300);
    } else if (c.request.mode == SaveChooserMode::Load) {
        c.closing_text = tr("Loading…");
        c.close_at = now + std::chrono::milliseconds(600);
    } else {
        // Until save_chooser_report() says how it went; a few seconds at
        // most if it never does.
        c.closing_text = tr(c.request.mode == SaveChooserMode::Save ? "Saving…" : "Deleting…");
        c.close_at = now + std::chrono::seconds(5);
    }
    SaveChooserDone done = std::move(c.on_done);
    c.on_done = nullptr;
    if (done) done(std::move(chosen));
}

// The panel while closing: one line, centred.
void closing_frame(const Chooser &c) {
    if (c.closing_text.empty()) return;
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, io.DisplaySize, colors::kBackdrop);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {px(36.0f), px(22.0f)});
    ImGui::Begin("##savedata_result", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::PushStyleColor(ImGuiCol_Text, colors::kAccentBright);
    ImGui::TextUnformatted(c.closing_text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}

const char *title_of(SaveChooserMode mode) {
    switch (mode) {
    case SaveChooserMode::Load: return "Load";
    case SaveChooserMode::Save: return "Save";
    case SaveChooserMode::Delete: return "Delete";
    }
    return "";
}

std::string shown_title(const SaveSlot &slot) {
    if (!slot.exists) return tr("New save data");
    return slot.title.empty() ? slot.name : slot.title;
}

// "Slot 4" for DATA03: the number a save name ends in, counted from one.
std::string slot_label(const std::string &name) {
    std::size_t digits = name.size();
    while (digits > 0u && name[digits - 1u] >= '0' && name[digits - 1u] <= '9') --digits;
    if (digits == name.size() || name.size() - digits > 4u) return {};
    return trf("Slot {}", {std::to_string(std::stoi(name.substr(digits)) + 1)});
}

// One save: its icon, title, detail, when it was saved and its slot.
// True when activated.
bool slot_row(std::size_t index, const SaveSlot &slot, const Icon &icon, bool focus_now) {
    // The title over the detail's lines (four at most), and never shorter
    // than the icon wants.
    const int lines = 1 + static_cast<int>(std::count(slot.detail.begin(), slot.detail.end(), '\n'));
    const int detail_lines = slot.detail.empty() ? 0 : std::min(4, lines);
    const float height = std::round(font() * std::max(4.0f, 1.9f + 1.25f * static_cast<float>(detail_lines)));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 max{min.x + width, min.y + height};
    if (focus_now) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(static_cast<int>(index));
    const bool pressed = ImGui::InvisibleButton("##slot", {width, height}, ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    if (focus_now) ImGui::SetScrollHereY(0.5f);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float rounding = px(6.0f);
    if (focused) {
        draw->AddRectFilled(min, max, colors::kRowFocus, rounding);
        draw->AddRectFilled(min, {min.x + px(4.0f), max.y}, colors::kAccent, rounding, ImDrawFlags_RoundCornersLeft);
    } else if (hovered) {
        draw->AddRectFilled(min, max, colors::kRowHover, rounding);
    }

    // The PSP's save icons are 144 by 80 pixels.
    const float pad = px(8.0f);
    const float icon_height = height - 2.0f * pad;
    const ImVec2 icon_min{min.x + px(14.0f), min.y + pad};
    const ImVec2 icon_max{icon_min.x + std::round(icon_height * 144.0f / 80.0f), icon_min.y + icon_height};
    if (icon.id != 0u) {
        draw->AddImage(static_cast<ImTextureID>(icon.id), icon_min, icon_max);
    } else {
        draw->AddRect(icon_min, icon_max, with_alpha(colors::kPanelEdge, 160), px(4.0f), 0, px(1.5f));
        if (!slot.exists) {
            const ImVec2 c{(icon_min.x + icon_max.x) * 0.5f, (icon_min.y + icon_max.y) * 0.5f};
            const float arm = icon_height * 0.25f;
            draw->AddLine({c.x - arm, c.y}, {c.x + arm, c.y}, colors::kAccent, px(3.0f));
            draw->AddLine({c.x, c.y - arm}, {c.x, c.y + arm}, colors::kAccent, px(3.0f));
        }
    }

    const float text_x = icon_max.x + px(16.0f);
    const float right = max.x - px(16.0f);
    const float line = min.y + pad + px(2.0f);
    const float date_width = slot.date.empty() ? 0.0f : ImGui::CalcTextSize(slot.date.c_str()).x;
    const std::string title = shown_title(slot);
    ImGui::PushClipRect({text_x, min.y}, {right - date_width - px(12.0f), max.y}, true);
    draw->AddText({text_x, line}, focused ? colors::kAccentBright : colors::kText, title.c_str());
    ImGui::PopClipRect();
    if (!slot.date.empty()) draw->AddText({right - date_width, line}, colors::kTextDim, slot.date.c_str());
    const std::string label = slot_label(slot.name);
    const float label_width = label.empty() ? 0.0f : ImGui::CalcTextSize(label.c_str()).x;
    if (!label.empty())
        draw->AddText({right - label_width, line + font() * 1.25f}, focused ? colors::kAccent : colors::kTextDim,
                      label.c_str());
    if (!slot.detail.empty()) {
        ImGui::PushClipRect({text_x, min.y}, {right - label_width - px(12.0f), max.y - pad}, true);
        draw->AddText(nullptr, 0.0f, {text_x, line + font() * 1.2f}, colors::kTextDim, slot.detail.c_str(), nullptr,
                      right - text_x);
        ImGui::PopClipRect();
    }
    return pressed;
}

} // namespace

bool open_game_save_chooser(SaveChooserRequest request, SaveChooserDone on_done) {
    Layer &layer = Layer::get();
    if (!layer.attached() || request.slots.empty()) return false;
    if (chooser().open) {
        finish(std::nullopt);
        close_now();
    }
    Chooser &c = chooser();
    c = Chooser{};
    for (const SaveSlot &slot : request.slots) c.icons.push_back(slot.icon.empty() ? Icon{} : icon_for(slot.icon));
    request.focus = std::min(request.focus, request.slots.size() - 1u);
    c.request = std::move(request);
    c.on_done = std::move(on_done);
    c.refocus = true;
    c.open = true;
    // The frame on screen stays behind the list, as the game's own screen
    // stays behind the PSP's dialog.
    layer.renderer().hold_frame(true);
    layer.renderer().set_game_input(false);
    layer.set_interactive(true);
    return true;
}

bool save_chooser_open() { return chooser().open; }

void save_chooser_report(bool ok) {
    Chooser &c = chooser();
    if (!c.open || !c.closing || c.request.mode == SaveChooserMode::Load || c.closing_text.empty()) return;
    const bool save = c.request.mode == SaveChooserMode::Save;
    c.closing_text = ok ? tr(save ? "Saved." : "Deleted.") : tr(save ? "Could not save." : "Could not delete.");
    c.close_at = Clock::now() + std::chrono::milliseconds(ok ? 1200 : 2500);
}

void save_chooser_frame() {
    Chooser &c = chooser();
    if (!c.open) return;
    Layer &layer = Layer::get();
    // The menu does not open over the list.
    (void)layer.take_menu_toggle();
    if (c.closing) {
        (void)layer.take_back();
        if (Clock::now() >= c.close_at) {
            close_now();
            return;
        }
        closing_frame(c);
        return;
    }
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    // The game's own prompt is answered with a face button that may still be
    // down when the list opens: the pad counts only once every face button
    // has been let go, as for navigation.
    const bool back = layer.take_back() || (layer.gamepad_armed() && ImGui::IsKeyPressed(cancel, false));
    const SaveChooserMode mode = c.request.mode;

    begin_panel("##savedata", title_of(mode), c.request.game_title, true);
    begin_content();
    std::optional<std::size_t> chosen;
    bool yes = false;
    bool no = false;
    if (c.asking) {
        const SaveSlot &slot = c.request.slots[*c.asking];
        heading(tr(mode == SaveChooserMode::Save ? "Save over this save?" : "Delete this save?"));
        paragraph(shown_title(slot) + (slot.date.empty() ? "" : "  ·  " + slot.date), colors::kTextDim);
        ImGui::Dummy({0.0f, px(8.0f)});
        if (std::exchange(c.ask_focus, false)) focus_next_row();
        no = button_row("No");
        yes = button_row("Yes");
    } else {
        for (std::size_t i = 0; i < c.request.slots.size(); ++i)
            if (slot_row(i, c.request.slots[i], c.icons[i], c.refocus && i == c.request.focus)) chosen = i;
        c.refocus = false;
        const auto saves = std::count_if(c.request.slots.begin(), c.request.slots.end(),
                                         [](const SaveSlot &slot) { return slot.exists; });
        layer.set_description(saves == 1 ? tr("1 save") : trf("{} saves", {std::to_string(saves)}));
    }
    begin_footer();
    hints({{Control::Confirm, "Choose"}, {Control::Back, c.asking ? "No" : "Cancel"}});
    end_panel();

    if (c.asking) {
        if (yes) {
            finish(c.request.slots[*c.asking].name);
        } else if (no || back) {
            c.request.focus = *c.asking;
            c.asking.reset();
            c.refocus = true;
        }
        return;
    }
    if (back) {
        finish(std::nullopt);
        return;
    }
    if (chosen) {
        if (c.request.slots[*chosen].exists && mode != SaveChooserMode::Load) {
            c.asking = *chosen;
            c.ask_focus = true;
        } else {
            finish(c.request.slots[*chosen].name);
        }
    }
}

} // namespace portablekit::ui
