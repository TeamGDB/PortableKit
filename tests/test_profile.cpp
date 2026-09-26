// A stand-in game for the framework's own tests.
//
// Host code asks portablekit::game() for the names a real port would supply.
// The tests here exercise that code without a game, so they link this instead:
// invented identifiers, no hashes worth checking, and the save folders the
// save-data tests expect. Nothing here describes a real release.

#include "profile.hpp"

namespace portablekit {
namespace {

constexpr SaveFolder kSaveFolders[] = {
    {"TEST00001", "Game data", true},
    {"TEST00001QST", "Extra data", true},
    {"TEST00001DAT", "Install data", false},
};

constexpr std::uint32_t kPatchedSlots[] = {0x09000000u, 0x09100000u};
void patched_image(psprecomp::Runtime &, const psprecomp::Elf32Image &) {}

// Two editions beside the release: a patched plain executable, and an
// encrypted one. The hashes are invented.
constexpr ProfileVariant kVariants[] = {
    {.key = "patched",
     .name = "Test patch 1.0",
     .executable_sha256 = "1111111111111111111111111111111111111111111111111111111111111111",
     .patch_loaded_image = &patched_image,
     .overlay_slots = kPatchedSlots},
    {.key = "other",
     .name = "Other release",
     .encrypted_executable_sha256 = "2222222222222222222222222222222222222222222222222222222222222222",
     .executable_sha256 = "3333333333333333333333333333333333333333333333333333333333333333"},
};

} // namespace

const GameProfile &game() {
    static const GameProfile profile{
        .app_name = "portablekit_tests",
        .project_name = "PortableKit",
        .env_prefix = "PORTABLEKIT",
        .data_organization = "PortableKit",
        .data_application = "tests",
        .disc_id = "TEST00001",
        .disc_id_display = "TEST-00001",
        .game_title = "PortableKit test game",
        .executable_path_on_disc = "PSP_GAME/SYSDIR/EBOOT.BIN",
        .param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO",
        .encrypted_executable_sha256 = "",
        .executable_sha256 = "",
        .decryption_tag = 0u,
        .decryption_key = {},
        .variants = kVariants,
        .load_base = 0x08804000u,
        .guest_ram_bytes = 32u * 1024u * 1024u,
        .boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN",
        .overlay_slots = {},
        .save_game_name = "TEST00001",
        .save_folders = kSaveFolders,
        .adhoc_product_code = "TEST00001",
    };
    return profile;
}

} // namespace portablekit
