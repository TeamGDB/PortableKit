// The launcher (ui/launcher.hpp). It draws its own frame (the side bar, the
// home page, the bottom bar with Play) and has the in-game menu draw its pages
// inside it, so every setting lives in one place whether the game runs or not.
//
// The art is the player's: images they put in the launcher folder, else
// PSP_GAME/PIC1.PNG and PSP_GAME/ICON0.PNG read from their disc image.
// Nothing of the game is carried by the port or the framework.

#include "../profile.hpp"
#include "ui/launcher.hpp"

#include "ui/i18n.hpp"
#include "ui/layer.hpp"
#include "ui/text_input.hpp"
#include "ui/widgets.hpp"

#include "app_paths.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "install/installer.hpp"
#include "install/user_data.hpp"
#include "kernel/iso_image.hpp"
#include "save_data/save_transfer.hpp"
#include "settings/settings.hpp"
#include "portablekit_version.hpp"

#include "imgui.h"
#include "imgui_internal.h"

// texture_pack.cpp keeps its decoder to itself; the launcher has its own, for
// the images it shows.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3/dr_mp3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace portablekit::ui {
namespace {

namespace fs = std::filesystem;

// Backups the launcher keeps in <data>/backups/ before it deletes the oldest.
constexpr std::size_t kKeptBackups = 10u;
// Images larger than this on a side are not shown.
constexpr int kMaxImageSide = 8192;

// ---------------------------------------------------------------------------
// Pages. The side bar has the home page, then the menu's pages this game has.

struct Page {
    const char *name;
    int tab;  // the menu's page (launcher_page), or -1 for the home page
};

const std::vector<Page> &pages() {
    static const std::vector<Page> list = [] {
        std::vector<Page> p{{"Home", -1}, {"Video", 0}, {"Audio", 1}, {"Controls", 2}};
        if (portablekit::game().adhoc_play) p.push_back({"Network", 3});
        p.push_back({"Mods", 5});
        p.push_back({"System & saves", 4});
        return p;
    }();
    return list;
}

// ---------------------------------------------------------------------------
// Images.

struct Image {
    std::uint64_t id{};  // for ImGui; zero: none
    float width{};
    float height{};
};

struct Pixels {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

std::optional<Pixels> decode(const std::vector<std::uint8_t> &bytes, const std::string &name) {
    int width = 0, height = 0, channels = 0;
    stbi_uc *data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (data == nullptr || width <= 0 || height <= 0 || width > kMaxImageSide || height > kMaxImageSide) {
        std::cout << "[launcher] cannot use " << name << ": "
                  << (data == nullptr ? stbi_failure_reason() : "too large") << "\n";
        if (data != nullptr) stbi_image_free(data);
        return std::nullopt;
    }
    Pixels pixels{width, height,
                  std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(width) * height * 4u)};
    stbi_image_free(data);
    return pixels;
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path &path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return std::nullopt;
    return bytes;
}

std::optional<std::vector<std::uint8_t>> read_from_disc(IsoImage &iso, const char *path) {
    const std::optional<IsoImage::Entry> entry = iso.find(path);
    if (!entry || entry->directory || entry->size == 0u || entry->size > 32u * 1024u * 1024u) return std::nullopt;
    std::vector<std::uint8_t> bytes(entry->size);
    if (iso.read(static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize, bytes) != bytes.size())
        return std::nullopt;
    return bytes;
}

// The folders a launcher image may be in, first match wins: the data
// folder's launcher/, then the one an app ships.
std::vector<fs::path> launcher_folders() {
    std::vector<fs::path> folders;
    try {
        folders.push_back(install::user_data_directory() / "launcher");
    } catch (const std::exception &) {
    }
    if (fs::path shipped = bundled_launcher_directory(); !shipped.empty()) folders.push_back(std::move(shipped));
    return folders;
}

// A logo drawn on black: black becomes transparent (each pixel taken as the
// logo's colour laid over black at some opacity) and the black around it is
// cropped away.
void lift_from_black(Pixels &p) {
    int left = p.width, top = p.height, right = -1, bottom = -1;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            std::uint8_t *px = &p.rgba[(static_cast<std::size_t>(y) * p.width + x) * 4u];
            const int alpha = std::max({px[0], px[1], px[2]});
            if (alpha > 0)
                for (int c = 0; c < 3; ++c) px[c] = static_cast<std::uint8_t>(std::min(255, px[c] * 255 / alpha));
            px[3] = static_cast<std::uint8_t>(alpha);
            if (alpha > 24) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    if (right < left || bottom < top) return;
    const int pad = std::max(2, (right - left) / 100);
    left = std::max(0, left - pad);
    top = std::max(0, top - pad);
    right = std::min(p.width - 1, right + pad);
    bottom = std::min(p.height - 1, bottom + pad);
    const int width = right - left + 1, height = bottom - top + 1;
    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(width) * height * 4u);
    for (int y = 0; y < height; ++y)
        std::copy_n(&p.rgba[(static_cast<std::size_t>(top + y) * p.width + left) * 4u],
                    static_cast<std::size_t>(width) * 4u, &cropped[static_cast<std::size_t>(y) * width * 4u]);
    p = Pixels{width, height, std::move(cropped)};
}

Image upload(const Pixels &p) {
    Image image;
    image.id = Layer::get().renderer().create_ui_image(static_cast<std::uint32_t>(p.width),
                                                       static_cast<std::uint32_t>(p.height),
                                                       reinterpret_cast<const std::uint32_t *>(p.rgba.data()));
    if (image.id == 0u) return {};
    image.width = static_cast<float>(p.width);
    image.height = static_cast<float>(p.height);
    return image;
}

// ---------------------------------------------------------------------------
// Music.

// launcher/music.mp3 in the launcher's folders (launcher_folders()), looped
// quietly while the launcher shows. It is the player's own file: nothing in
// this repository or its releases ships one. Silent when the audio is muted
// or off (<prefix>_NO_AUDIO), and when there is no such file.
class LauncherMusic {
public:
    LauncherMusic() {
        const settings::Settings &s = settings::current();
        if (s.mute || s.volume == 0u || portablekit::env("NO_AUDIO") != nullptr) return;
        fs::path file;
        std::error_code ec;
        for (const fs::path &folder : launcher_folders())
            if (fs::is_regular_file(folder / "music.mp3", ec)) {
                file = folder / "music.mp3";
                break;
            }
        if (file.empty()) return;
        std::optional<std::vector<std::uint8_t>> bytes = read_file(file);
        if (!bytes) return;
        bytes_ = std::move(*bytes);
        if (!drmp3_init_memory(&mp3_, bytes_.data(), bytes_.size(), nullptr)) {
            std::cout << "[launcher] cannot play " << install::path_to_utf8(file) << ": not an MP3 file\n";
            return;
        }
        decoder_ = true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
        audio_ = true;
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = static_cast<int>(mp3_.channels);
        spec.freq = static_cast<int>(mp3_.sampleRate);
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &feed, this);
        if (stream_ == nullptr) return;
        SDL_SetAudioStreamGain(stream_, kGain * static_cast<float>(std::min(s.volume, 100u)) / 100.0f);
        SDL_ResumeAudioStreamDevice(stream_);
        std::cout << "[launcher] playing " << install::path_to_utf8(file) << "\n";
    }
    ~LauncherMusic() {
        if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
        if (audio_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        if (decoder_) drmp3_uninit(&mp3_);
    }
    LauncherMusic(const LauncherMusic &) = delete;
    LauncherMusic &operator=(const LauncherMusic &) = delete;

private:
    // Quiet under the menu: a share of the game's own volume setting.
    static constexpr float kGain = 0.35f;

    // On SDL's audio thread: decodes what the device asks for, starting over
    // at the end.
    static void SDLCALL feed(void *self, SDL_AudioStream *stream, int additional, int) {
        LauncherMusic &m = *static_cast<LauncherMusic *>(self);
        const drmp3_uint32 channels = m.mp3_.channels;
        std::int16_t buffer[4096];
        auto wanted = static_cast<drmp3_uint64>(additional) / (channels * sizeof(std::int16_t));
        bool restarted = false;
        while (wanted > 0u) {
            const drmp3_uint64 chunk = std::min<drmp3_uint64>(wanted, std::size(buffer) / channels);
            const drmp3_uint64 got = drmp3_read_pcm_frames_s16(&m.mp3_, chunk, buffer);
            if (got == 0u) {
                // The end: from the start again, once per call, so a file that
                // decodes to nothing cannot spin here.
                if (restarted || !drmp3_seek_to_pcm_frame(&m.mp3_, 0u)) return;
                restarted = true;
                continue;
            }
            SDL_PutAudioStreamData(stream, buffer, static_cast<int>(got * channels * sizeof(std::int16_t)));
            wanted -= got;
        }
    }

    std::vector<std::uint8_t> bytes_;
    drmp3 mp3_{};
    bool decoder_{};
    bool audio_{};
    SDL_AudioStream *stream_{};
};

// ---------------------------------------------------------------------------
// The look.

ImU32 rgb(std::uint32_t color, int alpha = 255) {
    return IM_COL32((color >> 16) & 0xFFu, (color >> 8) & 0xFFu, color & 0xFFu, alpha);
}

// A bold serif face of the system's for headings and the Play button.
const char *const kSerifFonts[] = {
    "/System/Library/Fonts/Supplemental/Georgia Bold.ttf",
    "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf",
    "C:/Windows/Fonts/georgiab.ttf",
    "C:/Windows/Fonts/timesbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSerif-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
    "/system/fonts/NotoSerif-Bold.ttf",
};

ImFont *load_font(std::span<const std::uint8_t> data) {
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    if (!data.empty()) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        return atlas->AddFontFromMemoryTTF(const_cast<std::uint8_t *>(data.data()), static_cast<int>(data.size()),
                                           0.0f, &config);
    }
    for (const char *candidate : kSerifFonts) {
        std::error_code ec;
        if (fs::is_regular_file(install::path_from_utf8(candidate), ec)) return atlas->AddFontFromFileTTF(candidate);
    }
    return nullptr;
}

// Draws `image` over `min`-`max`, cropped to that shape; `focus_y` is the
// part of its height kept in view.
void draw_cover(ImDrawList *draw, const Image &image, ImVec2 min, ImVec2 max, float focus_y) {
    const float image_aspect = image.width / image.height;
    const float area_aspect = (max.x - min.x) / std::max(max.y - min.y, 1.0f);
    ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
    if (area_aspect > image_aspect) {
        const float shown = image_aspect / area_aspect;
        uv0.y = (1.0f - shown) * std::clamp(focus_y, 0.0f, 1.0f);
        uv1.y = uv0.y + shown;
    } else {
        const float shown = area_aspect / image_aspect;
        uv0.x = (1.0f - shown) * 0.5f;
        uv1.x = uv0.x + shown;
    }
    draw->AddImage(static_cast<ImTextureID>(image.id), min, max, uv0, uv1);
}

class Launcher {
public:
    Launcher(const LauncherTheme &theme, const fs::path &disc_image);
    // One frame; false once the player chose.
    bool frame();
    [[nodiscard]] LauncherChoice choice() const noexcept { return choice_; }

private:
    void sidebar(float width, float height);
    void home(ImVec2 min, ImVec2 max);
    void settings_page(ImVec2 min, ImVec2 max, bool back);
    void bottom_bar(ImVec2 min, ImVec2 max);
    void heading(const char *text, ImVec2 at, float width, float size);
    void link(const char *label, const char *url);

    const LauncherTheme &theme_;
    Image backdrop_;
    Image logo_;
    ImFont *serif_{};
    bool disc_unreadable_{};
    bool first_frame_{true};
    int page_{0};
    bool focus_nav_{};  // focus the selected page's entry in the side bar
    LauncherChoice choice_{LauncherChoice::Play};
    bool done_{};
};

Launcher::Launcher(const LauncherTheme &theme, const fs::path &disc_image) : theme_(theme) {
    const std::vector<fs::path> folders = launcher_folders();
    std::optional<IsoImage> iso;
    if (!disc_image.empty()) {
        try {
            iso.emplace(disc_image);
        } catch (const std::exception &e) {
            std::cout << "[launcher] " << e.what() << "\n";
            disc_unreadable_ = true;
        }
    }
    // A file in a launcher folder, else the image on the disc.
    const auto load = [&](const char *file, const char *on_disc, bool on_black) -> Image {
        std::optional<Pixels> pixels;
        for (const fs::path &folder : folders) {
            if (file == nullptr || pixels) break;
            const fs::path path = folder / file;
            if (const auto bytes = read_file(path)) pixels = decode(*bytes, install::path_to_utf8(path));
        }
        if (!pixels && on_disc != nullptr && iso) {
            if (const auto bytes = read_from_disc(*iso, on_disc)) pixels = decode(*bytes, on_disc);
            on_black = false;
        }
        if (!pixels) return {};
        if (on_black) lift_from_black(*pixels);
        return upload(*pixels);
    };
    backdrop_ = load(theme.backdrop_file, theme.backdrop_on_disc, false);
    logo_ = load(theme.logo_file, theme.logo_on_disc, theme.logo_on_black);
    serif_ = load_font(!theme.heading_font.empty() ? theme.heading_font : theme.title_font);
}

void Launcher::heading(const char *text, ImVec2 at, float width, float size) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(serif_, size);
    draw->AddText(at, rgb(theme_.accent_bright), text);
    const float height = ImGui::CalcTextSize(text).y;
    ImGui::PopFont();
    const float y = std::round(at.y + height + size * 0.25f);
    draw->AddLine({at.x, y}, {at.x + width, y}, rgb(theme_.text, 40), std::max(1.0f, std::round(size * 0.06f)));
}

void Launcher::link(const char *label, const char *url) {
    if (url == nullptr) return;
    const ImVec2 size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav) && !SDL_OpenURL(url))
        std::cout << "[launcher] cannot open " << url << ": " << SDL_GetError() << "\n";
    const bool lit = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddText(min, rgb(lit ? theme_.accent_bright : theme_.text_dim), label, ImGui::FindRenderedTextEnd(label));
    if (lit) draw->AddLine({min.x, min.y + size.y}, {min.x + size.x, min.y + size.y}, rgb(theme_.accent_bright));
}

void Launcher::sidebar(float width, float height) {
    const float font = Layer::get().font_size();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({0.0f, 0.0f}, {width, height}, rgb(theme_.sidebar));
    draw->AddLine({width - 1.0f, 0.0f}, {width - 1.0f, height}, rgb(theme_.text, 14));
    const float pad = std::round(font * 0.8f);
    float y = pad;

    // The logo, or the title.
    if (logo_.id != 0u) {
        float w = width - pad * 2.0f;
        float h = w * logo_.height / logo_.width;
        if (h > font * 6.0f) {
            h = font * 6.0f;
            w = h * logo_.width / logo_.height;
        }
        const float x = std::round((width - w) * 0.5f);
        draw->AddImage(static_cast<ImTextureID>(logo_.id), {x, y}, {x + w, y + h});
        y += std::round(h + font * 0.8f);
    } else if (theme_.title != nullptr) {
        ImGui::PushFont(serif_, font * 1.25f);
        ImGui::PushTextWrapPos(width - pad);
        ImGui::SetCursorScreenPos({pad, y});
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rgb(theme_.text)), "%s", theme_.title);
        if (theme_.subtitle != nullptr)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rgb(theme_.text)), "%s", theme_.subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        y = std::round(ImGui::GetCursorScreenPos().y + font * 0.8f);
    }

    // The pages.
    const float item = std::round(font * 2.1f);
    for (int i = 0; i < static_cast<int>(pages().size()); ++i) {
        const ImVec2 min{0.0f, y};
        const ImVec2 max{width - 1.0f, y + item};
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("page", {max.x - min.x, item}, ImGuiButtonFlags_EnableNav)) page_ = i;
        if (focus_nav_ && i == page_) {
            ImGui::FocusItem();
            focus_nav_ = false;
        }
        const bool focused = ImGui::IsItemFocused();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool selected = i == page_;
        if (selected) draw->AddRectFilled(min, max, rgb(theme_.text, 14));
        else if (focused || hovered) draw->AddRectFilled(min, max, rgb(theme_.text, 8));
        if (focused)
            draw->AddRectFilled(min, {min.x + std::max(2.0f, std::round(font * 0.15f)), max.y},
                                rgb(theme_.accent_bright));
        draw->AddText({pad, std::round(y + (item - font) * 0.5f)},
                      rgb(selected ? theme_.accent_bright : focused || hovered ? theme_.text : theme_.text_dim),
                      tr(pages()[static_cast<std::size_t>(i)].name));
        y += item;
    }

    // How to open the menu in the game, at the foot.
    ImGui::PushFont(nullptr, font * 0.78f);
    const float wrap = width - pad * 2.0f;
    const char *menu_hint = tr("In the game: Esc, or L3+R3 on a gamepad, opens the port's menu.");
    const ImVec2 hint = ImGui::CalcTextSize(menu_hint, nullptr, false, wrap);
    draw->AddText(nullptr, 0.0f, {pad, std::round(height - pad - hint.y)}, rgb(theme_.text_dim), menu_hint, nullptr,
                  wrap);
    ImGui::PopFont();
}

void Launcher::home(ImVec2 min, ImVec2 max) {
    const float font = Layer::get().font_size();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float width = max.x - min.x;
    const float pad = std::round(font * 1.4f);

    // The banner, fading into the page.
    const float banner = std::round(std::min((max.y - min.y) * 0.44f, width * 0.38f));
    if (backdrop_.id != 0u) {
        draw_cover(draw, backdrop_, min, {max.x, min.y + banner}, theme_.backdrop_focus_y);
        const float fade = std::round(banner * 0.4f);
        draw->AddRectFilledMultiColor({min.x, min.y + banner - fade}, {max.x, min.y + banner + 1.0f},
                                      rgb(theme_.background, 0), rgb(theme_.background, 0),
                                      rgb(theme_.background, 255), rgb(theme_.background, 255));
    }
    float y = std::round(min.y + banner - font * 0.6f);

    // The disc and the version.
    ImGui::PushFont(serif_, font * 0.95f);
    const std::string id = std::string(portablekit::game().disc_id_display) + "  ·  " +
                           portablekit::game().project_name + " " + kBuildVersion;
    draw->AddText({min.x + pad, y}, rgb(theme_.accent_bright), id.c_str());
    ImGui::PopFont();
    if (disc_unreadable_) {
        ImGui::PushFont(nullptr, font * 0.8f);
        draw->AddText({min.x + pad, std::round(y + font * 1.2f)}, rgb(theme_.text_dim),
                      tr("Your disc image could not be read."));
        ImGui::PopFont();
    }
    y += std::round(font * 2.4f);

    // The quick settings, in two columns where there is room: the game's
    // picture, then the launcher's own.
    const float area = std::round(std::min(width - pad * 2.0f, font * 46.0f));
    heading(tr("QUICK SETTINGS"), {min.x + pad, y}, area, font * 1.05f);
    y += std::round(font * 1.9f);
    const float foot = std::round(font * 1.8f);
    const bool two = area >= font * 34.0f;
    const float gap = std::round(font * 1.2f);
    const float column = two ? std::round((area - gap) * 0.5f) : area;
    const float rows_height = std::max(font * 3.0f, max.y - y - foot);
    ImGui::SetCursorScreenPos({min.x + pad, y});
    ImGui::BeginChild("##quick", {column, rows_height}, ImGuiChildFlags_NavFlattened);
    launcher_quick_settings();
    if (two) {
        touch_scroll();
        ImGui::EndChild();
        ImGui::SetCursorScreenPos({min.x + pad + column + gap, y});
        ImGui::BeginChild("##quick_launcher", {column, rows_height}, ImGuiChildFlags_NavFlattened);
    }
    settings::Settings &s = settings::current();
    if (choice_row("Back up saves before playing", s.launcher_backup ? "Yes" : "No",
                   {false, {}, "Copies your saves to the backups folder in the data folder each time you press "
                               "Play; the newest ten are kept."})) {
        s.launcher_backup = !s.launcher_backup;
        settings::save();
    }
    if (choice_row("Show this launcher at start", s.launcher_skip ? "No" : "Yes",
                   {false, {}, "No: the game starts at once. Start with --launcher to see this screen again."})) {
        s.launcher_skip = !s.launcher_skip;
        settings::save();
    }
    if (choice_row("Language", language_name(),
                   {false, {}, "The language of this menu and the launcher. The game's own text is not changed."}))
        next_language();
    touch_scroll();
    ImGui::EndChild();

    // Links and credits.
    ImGui::PushFont(nullptr, font * 0.8f);
    const float line_y = std::round(max.y - foot + font * 0.5f);
    ImGui::SetCursorScreenPos({min.x + pad, line_y});
    link(tr("Project page"), theme_.project_url);
    if (theme_.project_url != nullptr) ImGui::SameLine(0.0f, font * 1.2f);
    link(tr("Report a problem"), theme_.issues_url);
    if (theme_.credits != nullptr) {
        const char *credits = tr(theme_.credits);
        const ImVec2 size = ImGui::CalcTextSize(credits);
        draw->AddText({std::round(max.x - pad - size.x), line_y}, rgb(theme_.text_dim, 200), credits);
    }
    ImGui::PopFont();
}

void Launcher::settings_page(ImVec2 min, ImVec2 max, bool back) {
    const float font = Layer::get().font_size();
    const float pad = std::round(font * 1.4f);
    const float width = max.x - min.x - pad * 2.0f;
    const Page &page = pages()[static_cast<std::size_t>(page_)];
    heading(tr(page.name), {min.x + pad, min.y + pad}, width, font * 1.4f);
    const float top = std::round(min.y + pad + font * 2.6f);
    ImGui::SetCursorScreenPos({min.x + pad, top});
    ImGui::BeginChild("##page", {width, max.y - top - font * 0.6f}, ImGuiChildFlags_NavFlattened);
    const LauncherPage result = launcher_page(page.tab, back);
    touch_scroll();
    ImGui::EndChild();
    if (result.set_up) {
        choice_ = LauncherChoice::SetUp;
        done_ = true;
    } else if (back && !result.back_used) {
        page_ = 0;
        focus_nav_ = true;
    }
}

void Launcher::bottom_bar(ImVec2 min, ImVec2 max) {
    const float font = Layer::get().font_size();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, rgb(theme_.sidebar));
    draw->AddLine(min, {max.x, min.y}, rgb(theme_.text, 14));
    const float pad = std::round(font * 1.4f);
    const float gap = std::round(font * 0.45f);

    // Play.
    const float button_width = std::round(font * 9.5f);
    const ImVec2 bmin{max.x - gap - button_width, min.y + gap};
    const ImVec2 bmax{max.x - gap, max.y - gap};
    ImGui::SetCursorScreenPos(bmin);
    if (ImGui::InvisibleButton("##play", {bmax.x - bmin.x, bmax.y - bmin.y}, ImGuiButtonFlags_EnableNav)) {
        choice_ = LauncherChoice::Play;
        done_ = true;
    }
    if (first_frame_) {
        ImGui::FocusItem();
        first_frame_ = false;
    }
    const bool focused = ImGui::IsItemFocused();
    const bool lit = focused || ImGui::IsItemHovered();
    draw->AddRectFilled(bmin, bmax, rgb(lit ? theme_.accent_bright : theme_.accent));
    if (focused) draw->AddRect(bmin, bmax, rgb(theme_.text, 200), 0.0f, 0, std::max(1.0f, std::round(font * 0.08f)));
    ImGui::PushFont(serif_, font * 1.3f);
    const char *play = tr("PLAY");
    const ImVec2 text = ImGui::CalcTextSize(play);
    draw->AddText({std::round(bmin.x + (bmax.x - bmin.x - text.x) * 0.5f),
                   std::round(bmin.y + (bmax.y - bmin.y - text.y) * 0.5f)},
                  rgb(theme_.text), play);
    ImGui::PopFont();

    // What the focused row does.
    const std::string &description = Layer::get().description();
    if (!description.empty()) {
        ImGui::PushFont(nullptr, font * 0.8f);
        const float wrap = bmin.x - min.x - pad * 2.0f;
        const ImVec2 size = ImGui::CalcTextSize(description.c_str(), nullptr, false, wrap);
        draw->PushClipRect(min, {bmin.x - pad, max.y}, true);
        draw->AddText(nullptr, 0.0f, {min.x + pad, std::round(min.y + std::max(gap, (max.y - min.y - size.y) * 0.5f))},
                      rgb(theme_.text_dim), description.c_str(), nullptr, wrap);
        draw->PopClipRect();
        ImGui::PopFont();
    }
}

bool Launcher::frame() {
    Layer &layer = Layer::get();
    // A text field's keyboard takes the whole screen until it closes.
    if (text_input_open()) {
        text_input_frame();
        return true;
    }
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    const bool back = layer.take_back() || ImGui::IsKeyPressed(cancel, false);
    (void)layer.take_menu_toggle();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
        choice_ = LauncherChoice::Play;
        return false;
    }
    // The shoulder buttons go through the pages.
    const int count = static_cast<int>(pages().size());
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) page_ = (page_ + count - 1) % count;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) page_ = (page_ + 1) % count;

    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float font = layer.font_size();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::GetWindowDrawList()->AddRectFilled({0.0f, 0.0f}, size, rgb(theme_.background));

    const float side = std::round(std::clamp(size.x * 0.2f, font * 10.0f, font * 14.0f));
    const float bar = std::round(font * 3.3f);
    sidebar(side, size.y);
    const ImVec2 main_min{side, 0.0f};
    const ImVec2 main_max{size.x, size.y - bar};
    if (pages()[static_cast<std::size_t>(page_)].tab < 0) home(main_min, main_max);
    else settings_page(main_min, main_max, back);
    bottom_bar({side, size.y - bar}, size);

    ImGui::End();
    return !done_;
}

// Copies the saves to <data>/backups/<time>/ and keeps the newest few.
void back_up_saves() {
    const fs::path memory_stick = savedata::memory_stick();
    if (memory_stick.empty() || savedata::saves_to_back_up(memory_stick).empty()) return;
    fs::path root;
    try {
        root = install::user_data_directory() / "backups";
    } catch (const std::exception &e) {
        std::cout << "[launcher] no backup: " << e.what() << "\n";
        return;
    }
    const savedata::BackupResult result =
        savedata::back_up_saves(memory_stick, savedata::backup_folder(root, std::chrono::system_clock::now()), false);
    if (!result.ok) {
        std::cout << "[launcher] backup failed: " << result.error << "\n";
        return;
    }
    std::cout << "[launcher] saves backed up to " << install::path_to_utf8(result.folder) << "\n";
    std::error_code ec;
    std::vector<fs::path> backups;
    for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
        if (entry.is_directory(ec)) backups.push_back(entry.path());
    // Named by their time, so the oldest sort first.
    std::sort(backups.begin(), backups.end());
    for (std::size_t i = 0; i + kKeptBackups < backups.size(); ++i) fs::remove_all(backups[i], ec);
}

} // namespace

LauncherChoice run_launcher(const fs::path &disc_image, bool always) {
    const LauncherTheme *theme = portablekit::game().launcher;
    Layer &layer = Layer::get();
    if (theme == nullptr || !layer.attached() || portablekit::env("NO_LAUNCHER") != nullptr)
        return LauncherChoice::Play;
    if (settings::current().launcher_skip && !always) return LauncherChoice::Play;

    Launcher launcher(*theme, disc_image);
    const LauncherMusic music;
    layer.renderer().set_game_input(false);
    layer.set_interactive(true);
    const bool window_open = layer.run([&] { return launcher.frame(); }, false);
    layer.set_interactive(false);
    layer.renderer().set_game_input(true);
    if (!window_open) return LauncherChoice::Quit;
    if (launcher.choice() == LauncherChoice::SetUp || install::setup_requested_on_exit()) return LauncherChoice::SetUp;
    if (launcher.choice() == LauncherChoice::Play && settings::current().launcher_backup) back_up_saves();
    return launcher.choice();
}

} // namespace portablekit::ui
