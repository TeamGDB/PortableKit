// sceLibFont: the PSP system fonts live in the console's internal flash, which
// a dumped disc does not contain, so glyphs are rasterized from a TrueType font
// on the host instead. Metrics follow the PSP ABI so guest text layout still
// matches; the shapes are the substitute font's.
#include "hle_common.hpp"

#include "psprecomp/common.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace mhp3rd {
namespace {

// FontPixelFormat values used by SceFontGlyphImage.
constexpr std::uint32_t kPixelFormat4 = 0u;
constexpr std::uint32_t kPixelFormat4Reversed = 1u;
constexpr std::uint32_t kPixelFormat8 = 2u;
constexpr std::uint32_t kPixelFormat24 = 3u;
constexpr std::uint32_t kPixelFormat32 = 4u;

constexpr std::uint32_t kLibraryHandle = 0x00F0F000u;
constexpr std::uint32_t kFontHandle = 0x00F0F100u;
constexpr float kDefaultPixelHeight = 16.0f;

// Candidates that ship with the common desktop systems; a Japanese face is
// needed for the game's text.
const char *const kFontCandidates[] = {
    // macOS ships Hiragino Kaku Gothic W4 under its Japanese file name.
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/AquaKana.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
};

struct FontState {
    std::vector<std::uint8_t> file;
    stbtt_fontinfo info{};
    bool loaded{};
    float pixel_height{kDefaultPixelHeight};
    float scale{};
    int ascent{};
    int descent{};
    int line_gap{};
    std::uint64_t glyphs_rendered{};
};

FontState &font() {
    static FontState state;
    return state;
}

void set_pixel_height(float height) {
    FontState &state = font();
    state.pixel_height = height > 1.0f ? height : kDefaultPixelHeight;
    if (state.loaded) state.scale = stbtt_ScaleForPixelHeight(&state.info, state.pixel_height);
}

bool load_font() {
    FontState &state = font();
    if (state.loaded) return true;
    std::vector<std::string> paths;
    if (const char *configured = std::getenv("MHP3RD_FONT")) paths.emplace_back(configured);
    for (const char *candidate : kFontCandidates) paths.emplace_back(candidate);

    for (const std::string &path : paths) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) continue;
        const auto size = static_cast<std::streamsize>(file.tellg());
        file.seekg(0);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char *>(data.data()), size)) continue;
        const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
        if (offset < 0) continue;
        state.file = std::move(data);
        if (stbtt_InitFont(&state.info, state.file.data(), offset) == 0) {
            state.file.clear();
            continue;
        }
        stbtt_GetFontVMetrics(&state.info, &state.ascent, &state.descent, &state.line_gap);
        state.loaded = true;
        set_pixel_height(state.pixel_height);
        std::cout << "Fonts: rasterizing sceLibFont glyphs from " << path << "\n";
        return true;
    }
    log_once("font-missing",
             "[font] no TrueType font found; guest text stays blank. Set MHP3RD_FONT=<path to a .ttf/.ttc>");
    return false;
}

std::int32_t to_fixed26(float value) { return static_cast<std::int32_t>(value * 64.0f); }

// SceFontCharInfo, 60 bytes.
void write_char_info(psprecomp::GuestMemory &memory, std::uint32_t address, int width, int height, int left, int top,
                     float advance) {
    const auto put = [&](std::uint32_t offset, std::uint32_t value) { memory.store32(address + offset, value); };
    put(0u, static_cast<std::uint32_t>(width));
    put(4u, static_cast<std::uint32_t>(height));
    put(8u, static_cast<std::uint32_t>(left));
    put(12u, static_cast<std::uint32_t>(top));
    put(16u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(width))));
    put(20u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(height))));
    const FontState &state = font();
    put(24u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(state.ascent) * state.scale)));
    put(28u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(state.descent) * state.scale)));
    put(32u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(left))));       // bearing HX
    put(36u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(top))));        // bearing HY
    put(40u, 0u);                                                                    // bearing VX
    put(44u, static_cast<std::uint32_t>(to_fixed26(static_cast<float>(top))));        // bearing VY
    put(48u, static_cast<std::uint32_t>(to_fixed26(advance)));                        // advance H
    put(52u, static_cast<std::uint32_t>(to_fixed26(state.pixel_height)));             // advance V
    memory.store16(address + 56u, 0u);                                               // shadow flags
    memory.store16(address + 58u, 0u);                                               // shadow id
}

struct GlyphBitmap {
    std::vector<std::uint8_t> pixels;
    int width{};
    int height{};
    int left{};
    int top{};
    float advance{};
};

bool rasterize(std::uint32_t code, GlyphBitmap &out) {
    FontState &state = font();
    if (!state.loaded) return false;
    const int glyph = stbtt_FindGlyphIndex(&state.info, static_cast<int>(code));
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&state.info, glyph, &advance, &bearing);
    out.advance = static_cast<float>(advance) * state.scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&state.info, glyph, state.scale, state.scale, &x0, &y0, &x1, &y1);
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.left = x0;
    out.top = -y0;
    if (out.width <= 0 || out.height <= 0) {
        out.pixels.clear();
        out.width = std::max(out.width, 0);
        out.height = std::max(out.height, 0);
        return true;
    }
    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height, 0u);
    stbtt_MakeGlyphBitmap(&state.info, out.pixels.data(), out.width, out.height, out.width, state.scale, state.scale,
                          glyph);
    return true;
}

// Writes one glyph into the guest buffer described by SceFontGlyphImage.
void blit_glyph(psprecomp::GuestMemory &memory, std::uint32_t image_address, const GlyphBitmap &glyph) {
    const std::uint32_t pixel_format = memory.load32(image_address);
    const auto x_position = static_cast<std::int32_t>(memory.load32(image_address + 4u)) / 64;
    const auto y_position = static_cast<std::int32_t>(memory.load32(image_address + 8u)) / 64;
    const std::uint32_t buffer_width = memory.load16(image_address + 12u);
    const std::uint32_t buffer_height = memory.load16(image_address + 14u);
    const std::uint32_t bytes_per_line = memory.load16(image_address + 16u);
    const std::uint32_t buffer = memory.load32(image_address + 20u);
    if (buffer == 0u || glyph.pixels.empty()) return;

    for (int row = 0; row < glyph.height; ++row) {
        const std::int32_t y = y_position + row;
        if (y < 0 || static_cast<std::uint32_t>(y) >= buffer_height) continue;
        for (int column = 0; column < glyph.width; ++column) {
            const std::int32_t x = x_position + column;
            if (x < 0 || static_cast<std::uint32_t>(x) >= buffer_width) continue;
            const std::uint8_t value = glyph.pixels[static_cast<std::size_t>(row) * glyph.width + column];
            const std::uint32_t line = buffer + static_cast<std::uint32_t>(y) * bytes_per_line;
            switch (pixel_format) {
            case kPixelFormat4:
            case kPixelFormat4Reversed: {
                const std::uint32_t at = line + static_cast<std::uint32_t>(x) / 2u;
                const std::uint8_t existing = memory.load8(at);
                const std::uint8_t nibble = static_cast<std::uint8_t>(value >> 4u);
                const bool high = pixel_format == kPixelFormat4 ? ((x & 1) != 0) : ((x & 1) == 0);
                const std::uint8_t merged = high ? static_cast<std::uint8_t>((existing & 0x0Fu) | (nibble << 4u))
                                                 : static_cast<std::uint8_t>((existing & 0xF0u) | nibble);
                memory.store8(at, merged);
                break;
            }
            case kPixelFormat8:
                memory.store8(line + static_cast<std::uint32_t>(x), value);
                break;
            case kPixelFormat24: {
                const std::uint32_t at = line + static_cast<std::uint32_t>(x) * 3u;
                memory.store8(at, value);
                memory.store8(at + 1u, value);
                memory.store8(at + 2u, value);
                break;
            }
            case kPixelFormat32:
            default:
                memory.store32(line + static_cast<std::uint32_t>(x) * 4u,
                               (static_cast<std::uint32_t>(value) << 24u) | 0x00FFFFFFu);
                break;
            }
        }
    }
}

} // namespace

void register_font(HleRegistrar &hle) {
    load_font();

    hle.add("sceLibFont", "sceFontNewLib", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        kernel().finish(ctx, kLibraryHandle);
    });
    hle.add("sceLibFont", "sceFontDoneLib", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceLibFont", "sceFontGetNumFontList", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        kernel().finish(ctx, 1u);
    });
    hle.add("sceLibFont", "sceFontFindOptimumFont", [](Runtime &rt, AllegrexContext &ctx) {
        // The requested style carries the pixel size the game wants.
        const std::uint32_t style = arg(ctx, 1);
        if (style != 0u) {
            const std::uint32_t bits = rt.memory().load32(style);
            float height{};
            std::memcpy(&height, &bits, sizeof(height));
            if (height > 1.0f && height < 128.0f) set_pixel_height(height);
        }
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), 0u);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceLibFont", "sceFontOpen", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
        kernel().finish(ctx, font().loaded ? kFontHandle : 0u);
    });
    hle.add("sceLibFont", "sceFontClose", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });

    hle.add("sceLibFont", "sceFontGetFontInfo", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 1);
        if (address == 0u || !font().loaded) {
            kernel().finish(ctx, 0u);
            return;
        }
        auto &memory = rt.memory();
        const FontState &state = font();
        const float ascender = static_cast<float>(state.ascent) * state.scale;
        const float descender = static_cast<float>(state.descent) * state.scale;
        const float height = state.pixel_height;
        const std::int32_t fixed[10] = {
            to_fixed26(height), to_fixed26(height), to_fixed26(ascender), to_fixed26(descender),
            0,                  to_fixed26(ascender), to_fixed26(height / 2.0f), to_fixed26(ascender),
            to_fixed26(height), to_fixed26(height),
        };
        for (std::uint32_t i = 0; i < 10u; ++i) memory.store32(address + i * 4u, static_cast<std::uint32_t>(fixed[i]));
        const float floats[10] = {height, height, ascender, descender, 0.0f, ascender, height / 2.0f, ascender,
                                  height, height};
        for (std::uint32_t i = 0; i < 10u; ++i) {
            std::uint32_t bits{};
            std::memcpy(&bits, &floats[i], sizeof(bits));
            memory.store32(address + 40u + i * 4u, bits);
        }
        memory.store16(address + 80u, static_cast<std::uint16_t>(height));
        memory.store16(address + 82u, static_cast<std::uint16_t>(height));
        memory.store32(address + 84u, 0x10000u);  // glyph count
        memory.store32(address + 88u, 0u);        // shadow map length
        kernel().finish(ctx, 0u);
    });

    hle.add("sceLibFont", "sceFontGetCharInfo", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 2);
        if (address == 0u) {
            kernel().finish(ctx, 0u);
            return;
        }
        GlyphBitmap glyph{};
        if (!rasterize(arg(ctx, 1), glyph)) {
            for (std::uint32_t i = 0; i < 60u; i += 4u) rt.memory().store32(address + i, 0u);
            kernel().finish(ctx, 0u);
            return;
        }
        write_char_info(rt.memory(), address, glyph.width, glyph.height, glyph.left, glyph.top, glyph.advance);
        kernel().finish(ctx, 0u);
    });

    hle.add("sceLibFont", "sceFontGetCharGlyphImage", [](Runtime &rt, AllegrexContext &ctx) {
        GlyphBitmap glyph{};
        if (rasterize(arg(ctx, 1), glyph)) {
            blit_glyph(rt.memory(), arg(ctx, 2), glyph);
            ++font().glyphs_rendered;
        }
        kernel().finish(ctx, 0u);
    });
}

} // namespace mhp3rd
