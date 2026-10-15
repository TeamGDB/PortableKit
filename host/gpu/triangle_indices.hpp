#pragma once

#include "ge_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace portablekit::gpu {

// The vertex of a draw at position `index` of its primitive: through the
// index list when there is one, clamped to the vertices decoded.
inline std::uint16_t primitive_vertex(const std::vector<std::uint16_t> &indices, std::size_t vertex_count,
                                      std::size_t index) {
    const std::size_t last = vertex_count - 1u;
    const std::size_t mapped = indices.empty() ? index : indices[index];
    return static_cast<std::uint16_t>(std::min(mapped, last));
}

namespace detail {

// The triangles of one primitive, written to `out` with `base` added to each
// index. `at` maps a position in the primitive to a decoded vertex.
template <typename At>
inline std::uint16_t *write_triangles(PrimitiveType primitive, std::size_t count, std::uint16_t base, At at,
                                      std::uint16_t *out) {
    const auto put = [&](std::size_t index) { *out++ = static_cast<std::uint16_t>(at(index) + base); };
    switch (primitive) {
    case PrimitiveType::Triangles:
        for (std::size_t i = 0; i + 2u < count; i += 3u) {
            put(i);
            put(i + 1u);
            put(i + 2u);
        }
        break;
    case PrimitiveType::TriangleStrip:
        for (std::size_t i = 0; i + 2u < count; ++i) {
            const bool odd = (i & 1u) != 0u;
            put(i);
            put(odd ? i + 2u : i + 1u);
            put(odd ? i + 1u : i + 2u);
        }
        break;
    case PrimitiveType::TriangleFan:
        for (std::size_t i = 1u; i + 1u < count; ++i) {
            put(0);
            put(i);
            put(i + 1u);
        }
        break;
    default: break;
    }
    return out;
}

inline std::size_t triangle_index_count(PrimitiveType primitive, std::size_t count) {
    switch (primitive) {
    case PrimitiveType::Triangles: return count / 3u * 3u;
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleFan: return count >= 3u ? (count - 2u) * 3u : 0u;
    default: return 0u;
    }
}

} // namespace detail

// Appends to `out` the triangle list a triangle, strip or fan primitive
// becomes, as indices into its decoded vertices plus `base`. The order is the
// one the renderer expands vertices in: every triangle in the same place and
// with the same first (provoking) vertex, so an indexed draw of the decoded
// vertices rasterises exactly what a draw of the expanded ones does.
//
// `count` is the number of vertices the primitive consumes (the index count
// when it is indexed), `vertex_count` the number decoded, at most 65536.
// Returns false, appending nothing, for any other primitive.
inline bool append_triangle_indices(PrimitiveType primitive, std::size_t count,
                                    const std::vector<std::uint16_t> &indices, std::size_t vertex_count,
                                    std::uint16_t base, std::vector<std::uint16_t> &out) {
    const bool triangles = primitive == PrimitiveType::Triangles || primitive == PrimitiveType::TriangleStrip ||
                           primitive == PrimitiveType::TriangleFan;
    if (!triangles || vertex_count == 0u) return triangles;
    const std::size_t start = out.size();
    out.resize(start + detail::triangle_index_count(primitive, count));
    std::uint16_t *const first = out.data() + start;
    std::uint16_t *last;
    if (indices.empty() && count <= vertex_count) {
        // Every position is a vertex: nothing to look up or clamp.
        last = detail::write_triangles(primitive, count, base,
                                       [](std::size_t index) { return static_cast<std::uint16_t>(index); }, first);
    } else {
        last = detail::write_triangles(
            primitive, count, base,
            [&](std::size_t index) { return primitive_vertex(indices, vertex_count, index); }, first);
    }
    out.resize(static_cast<std::size_t>(last - out.data()));
    return true;
}

// The same into an emptied `out`, with no base.
inline bool triangle_indices(PrimitiveType primitive, std::size_t count, const std::vector<std::uint16_t> &indices,
                             std::size_t vertex_count, std::vector<std::uint16_t> &out) {
    out.clear();
    return append_triangle_indices(primitive, count, indices, vertex_count, 0u, out);
}

} // namespace portablekit::gpu
