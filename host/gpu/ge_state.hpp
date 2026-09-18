#pragma once

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// Graphics Engine state machine: turns a PSP display list into draw calls with
// decoded vertices, independent of any rendering backend.
namespace mhp3rd::gpu {

using psprecomp::GuestMemory;

enum class PrimitiveType : std::uint8_t {
    Points = 0,
    Lines = 1,
    LineStrip = 2,
    Triangles = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprites = 6,
};

enum class TextureFormat : std::uint8_t {
    Rgba5650 = 0,
    Rgba5551 = 1,
    Rgba4444 = 2,
    Rgba8888 = 3,
    Clut4 = 4,
    Clut8 = 5,
    Clut16 = 6,
    Clut32 = 7,
    Dxt1 = 8,
    Dxt3 = 9,
    Dxt5 = 10,
};

// One decoded vertex in the format the backend consumes.
struct Vertex {
    std::array<float, 4> position{0.0f, 0.0f, 0.0f, 1.0f};  // object or screen space
    std::array<float, 3> normal{};
    std::array<float, 2> texcoord{};
    std::uint32_t color{0xFFFFFFFFu};
};

struct TextureState {
    bool enabled{};
    std::uint32_t address{};
    std::uint32_t buffer_width{};
    std::uint16_t width{};
    std::uint16_t height{};
    TextureFormat format{TextureFormat::Rgba5650};
    bool swizzled{};
    std::uint32_t clut_address{};
    std::uint32_t clut_format{};
    std::uint32_t clut_shift{};
    std::uint32_t clut_mask{};
    std::uint32_t clut_offset{};
    std::uint32_t function{};        // TFX: modulate/decal/blend/replace/add
    bool alpha_from_texture{};       // TCC
    std::uint32_t min_filter{};
    std::uint32_t mag_filter{};
    std::uint32_t wrap_s{};
    std::uint32_t wrap_t{};
    // Texture coordinate scale and offset (UV transform).
    float scale_u{1.0f};
    float scale_v{1.0f};
    float offset_u{};
    float offset_v{};
};

struct RenderTarget {
    std::uint32_t color_address{};
    std::uint32_t color_stride{512u};
    std::uint32_t color_format{};  // 0:5650 1:5551 2:4444 3:8888
    std::uint32_t depth_address{};
    std::uint32_t depth_stride{512u};
};

struct BlendState {
    bool enabled{};
    std::uint32_t source_factor{};
    std::uint32_t destination_factor{};
    std::uint32_t equation{};
    std::uint32_t fixed_source{};
    std::uint32_t fixed_destination{};
};

struct DepthState {
    bool test_enabled{};
    bool write_enabled{true};
    std::uint32_t function{};
    std::uint16_t range_near{};
    std::uint16_t range_far{0xFFFFu};
};

struct AlphaTestState {
    bool enabled{};
    std::uint32_t function{};
    std::uint32_t reference{};
    std::uint32_t mask{0xFFu};
};

struct ViewportState {
    float x_scale{}, y_scale{}, z_scale{};
    float x_offset{}, y_offset{}, z_offset{};
    std::uint32_t scissor_x1{}, scissor_y1{}, scissor_x2{479u}, scissor_y2{271u};
    float offset_x{}, offset_y{};  // screen-space origin, 16 bits with 4 fractional
};

// A draw call: decoded vertices plus the state they are drawn with.
struct DrawCall {
    PrimitiveType primitive{};
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;  // empty when the draw is not indexed
    bool through{};                      // vertices are already in screen space
    TextureState texture;
    RenderTarget target;
    BlendState blend;
    DepthState depth;
    AlphaTestState alpha_test;
    ViewportState viewport;
    bool culling_enabled{};
    bool cull_clockwise{};
    bool clear_mode{};                   // CLEARMODE is active for this draw
    std::uint32_t clear_flags{};         // CLEARMODE bits 8..10: color, alpha/stencil, depth
    std::uint32_t vertex_type{};
    std::uint32_t material_color{0xFFFFFFFFu};
    bool lighting_enabled{};
    std::array<float, 16> world{};
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> texture_matrix{};
};

// Executes display lists and reports the draw calls they produce. The backend
// installs a sink; with no sink the lists are still parsed (for callbacks).
class GeState {
public:
    using DrawSink = std::function<void(const DrawCall &)>;
    using SignalSink = std::function<void(std::uint32_t signal, std::uint32_t pc)>;

    void set_draw_sink(DrawSink sink) { draw_sink_ = std::move(sink); }
    void set_signal_sink(SignalSink sink) { signal_sink_ = std::move(sink); }

    // Runs commands from `pc` until `stall` (0 = no stall) or END. Returns the
    // address execution stopped at; `finished` reports whether the list ended.
    std::uint32_t execute(const GuestMemory &memory, std::uint32_t pc, std::uint32_t stall, bool &finished);

    [[nodiscard]] const RenderTarget &target() const noexcept { return target_; }
    [[nodiscard]] std::uint64_t draw_count() const noexcept { return draw_count_; }
    [[nodiscard]] std::uint64_t vertex_count() const noexcept { return vertex_count_; }
    [[nodiscard]] std::uint64_t unhandled_command_count() const noexcept { return unhandled_commands_; }

private:
    // Resolves a display-list address operand against BASE and OFFSET_ADDR.
    [[nodiscard]] std::uint32_t relative_address(std::uint32_t data) const noexcept {
        return (offset_address_ + (base_extended_ | (data & 0x00FFFFFFu))) & 0x0FFFFFFFu;
    }

    void handle_command(const GuestMemory &memory, std::uint32_t command, std::uint32_t data);
    void draw_primitive(const GuestMemory &memory, std::uint32_t data);
    void draw_bezier_or_spline(std::uint32_t command);

    std::array<std::uint32_t, 256> registers_{};
    RenderTarget target_{};
    TextureState texture_{};
    BlendState blend_{};
    DepthState depth_{};
    AlphaTestState alpha_test_{};
    ViewportState viewport_{};
    bool culling_enabled_{};
    bool cull_clockwise_{};
    bool clear_mode_{};
    std::uint32_t clear_flags_{};
    std::uint32_t material_color_{0xFFFFFFFFu};
    bool lighting_enabled_{};
    std::uint32_t vertex_type_{};
    std::uint32_t vertex_address_{};
    std::uint32_t index_address_{};
    std::uint32_t base_extended_{};   // BASE: bits 16..19 become address bits 24..27
    std::uint32_t offset_address_{};  // OFFSET_ADDR: added to every relative address
    std::array<float, 16> world_{};
    std::array<float, 16> view_{};
    std::array<float, 16> projection_{};
    std::array<float, 16> texture_matrix_{};
    std::array<float, 96> bone_matrices_{};
    // The GE keeps one auto-incrementing write index per matrix; sharing a
    // single counter lets interleaved uploads scribble over each other.
    std::uint32_t world_write_index_{};
    std::uint32_t view_write_index_{};
    std::uint32_t projection_write_index_{};
    std::uint32_t texture_write_index_{};
    std::uint32_t bone_write_index_{};

    std::vector<std::uint32_t> call_stack_;
    DrawSink draw_sink_;
    SignalSink signal_sink_;
    std::uint64_t draw_count_{};
    std::uint64_t vertex_count_{};
    std::uint64_t unhandled_commands_{};
};

// Decodes `count` vertices of the given vertex type starting at `address`.
// Returns the number of bytes each vertex occupies.
// `bone_matrices`, when given, points at 8 consecutive 3x4 matrices (96 floats)
// and skinned vertices are blended into their bones' space by their weights.
std::uint32_t decode_vertices(const GuestMemory &memory, std::uint32_t address, std::uint32_t vertex_type,
                              std::uint32_t count, std::vector<Vertex> &out,
                              const float *bone_matrices = nullptr);

} // namespace mhp3rd::gpu
