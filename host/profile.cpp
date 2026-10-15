#include "profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace portablekit {

const char *env(const char *name) {
    // The prefix is fixed for the life of the process, so the name is built
    // once per call and never kept: getenv's result points into the
    // environment, not into this string.
    std::string full = game().env_prefix;
    full += '_';
    full += name;
    return std::getenv(full.c_str());
}

bool env_set(const char *name) { return env(name) != nullptr; }

std::string env_name(const char *name) { return std::string(game().env_prefix) + '_' + name; }

bool texture_pack_fits(std::string_view disc_id) {
    const auto same = [disc_id](std::string_view other) {
        return other.size() == disc_id.size() &&
               std::equal(other.begin(), other.end(), disc_id.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               });
    };
    if (game().disc_id != nullptr && same(game().disc_id)) return true;
    return std::any_of(game().texture_pack_releases.begin(), game().texture_pack_releases.end(),
                       [&](const char *release) { return release != nullptr && same(release); });
}

namespace {
const ProfileVariant *g_active_variant = nullptr;
}

const ProfileVariant *find_variant(std::string_view executable_sha256) {
    for (const ProfileVariant &variant : game().variants)
        if (variant.executable_sha256 != nullptr && executable_sha256 == variant.executable_sha256) return &variant;
    return nullptr;
}

const ProfileVariant *find_variant_by_encrypted(std::string_view eboot_sha256) {
    for (const ProfileVariant &variant : game().variants)
        if (variant.encrypted_executable_sha256 != nullptr && eboot_sha256 == variant.encrypted_executable_sha256)
            return &variant;
    return nullptr;
}

bool is_known_executable(std::string_view executable_sha256) {
    return (game().executable_sha256 != nullptr && executable_sha256 == game().executable_sha256) ||
           find_variant(executable_sha256) != nullptr;
}

void set_active_variant(const ProfileVariant *variant) { g_active_variant = variant; }
const ProfileVariant *active_variant() { return g_active_variant; }

ExtraHleHook active_register_extra_hle() {
    if (g_active_variant != nullptr && g_active_variant->register_extra_hle != nullptr)
        return g_active_variant->register_extra_hle;
    return game().register_extra_hle;
}

PatchImageHook active_patch_loaded_image() {
    if (g_active_variant != nullptr && g_active_variant->patch_loaded_image != nullptr)
        return g_active_variant->patch_loaded_image;
    return game().patch_loaded_image;
}

std::span<const std::uint32_t> active_overlay_slots() {
    if (g_active_variant != nullptr && !g_active_variant->overlay_slots.empty()) return g_active_variant->overlay_slots;
    return game().overlay_slots;
}

} // namespace portablekit
