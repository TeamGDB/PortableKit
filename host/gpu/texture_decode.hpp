#pragma once

#include "ge_state.hpp"

#include <cstdint>
#include <vector>

namespace portablekit::gpu {

// Decodes a PSP texture into RGBA8888 (one std::uint32_t per texel, red in the
// low byte). Handles the direct colour formats, 4/8/16/32-bit palettes, DXT1/3/5
// and the swizzled layout. Returns false when the texture cannot be read.
bool decode_texture(const GuestMemory &memory, const TextureState &texture, std::vector<std::uint32_t> &out);

// Key that identifies the decoded contents of a texture for caching.
[[nodiscard]] std::uint64_t texture_key(const GuestMemory &memory, const TextureState &texture);

} // namespace portablekit::gpu
