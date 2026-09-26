#include "auto_profile.hpp"

#include "hand_profiles.inc"

#include "psprecomp/elf32.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace portablekit::app {
namespace {

constexpr std::uint32_t kPsp1000Ram = 32u * 1024u * 1024u;
constexpr std::uint32_t kPsp2000Ram = 64u * 1024u * 1024u;

// The strings the active profile points into live here.
struct ProfileStrings {
    std::string disc_id;
    std::string disc_id_display;
    std::string title;
    std::string save_description;
    std::string data_application;
};

ProfileStrings &strings() {
    static ProfileStrings value;
    return value;
}

GameProfile &active() {
    static GameProfile profile{};
    return profile;
}

SaveFolder &main_save_folder() {
    static SaveFolder folder{};
    return folder;
}

std::string &active_profile_name() {
    static std::string name = "launcher";
    return name;
}

std::vector<std::pair<std::string, std::string>> &decisions() {
    static std::vector<std::pair<std::string, std::string>> value;
    return value;
}

// The fields every profile gets from the app, whichever game it describes:
// the names that decide where settings and switches live.
void apply_app_identity(GameProfile &profile) {
    profile.app_name = "portablekit";
    profile.project_name = "PortableKit";
    profile.env_prefix = "PORTABLEKIT";
    profile.data_organization = "PortableKit";
}

} // namespace

std::vector<HandProfile> hand_profiles() {
    return std::vector<HandProfile>{PORTABLEKIT_APP_HAND_PROFILES};
}

const HandProfile *hand_profile_for(const std::string &executable_sha256) {
    static const std::vector<HandProfile> profiles = hand_profiles();
    for (const HandProfile &profile : profiles) {
        const GameProfile &game = profile.get();
        if (game.executable_sha256 != nullptr && executable_sha256 == game.executable_sha256) return &profile;
        for (const ProfileVariant &variant : game.variants)
            if (variant.executable_sha256 != nullptr && executable_sha256 == variant.executable_sha256)
                return &profile;
    }
    return nullptr;
}

namespace {
// A hand profile's edition, applied to the app's copy of the profile: the app
// runs its own corpus for whichever edition it is, so to the framework the
// edition is simply the profile's own executable.
std::string apply_edition(GameProfile &profile, const std::string &executable_sha256) {
    for (const ProfileVariant &variant : profile.variants) {
        if (variant.executable_sha256 == nullptr || executable_sha256 != variant.executable_sha256) continue;
        profile.executable_sha256 = variant.executable_sha256;
        if (variant.encrypted_executable_sha256 != nullptr)
            profile.encrypted_executable_sha256 = variant.encrypted_executable_sha256;
        if (variant.register_extra_hle != nullptr) profile.register_extra_hle = variant.register_extra_hle;
        if (variant.patch_loaded_image != nullptr) profile.patch_loaded_image = variant.patch_loaded_image;
        if (!variant.overlay_slots.empty()) profile.overlay_slots = variant.overlay_slots;
        profile.variants = {};
        return variant.name;
    }
    profile.variants = {};
    return {};
}
} // namespace

void activate_launcher_profile() {
    GameProfile &profile = active();
    profile = GameProfile{};
    apply_app_identity(profile);
    profile.data_application = "launcher";
    profile.disc_id = "";
    profile.disc_id_display = "";
    profile.game_title = "PortableKit";
    profile.executable_path_on_disc = "PSP_GAME/SYSDIR/EBOOT.BIN";
    profile.param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO";
    profile.encrypted_executable_sha256 = "";
    profile.executable_sha256 = "";
    profile.load_base = psprecomp::kDefaultPspUserLoadBase;
    profile.guest_ram_bytes = kPsp1000Ram;
    profile.boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    profile.save_game_name = "";
    profile.adhoc_product_code = "";
    active_profile_name() = "launcher";
}

std::string activate_game_profile(const GameRecord &record) {
    ProfileStrings &text = strings();
    text.disc_id = record.disc_id;
    text.disc_id_display = display_disc_id(record.disc_id);
    text.title = record.title;
    text.data_application = record.id;
    decisions().clear();

    GameProfile &profile = active();
    if (const HandProfile *hand = hand_profile_for(record.executable_sha256)) {
        profile = hand->get();
        const std::string edition = apply_edition(profile, record.executable_sha256);
        apply_app_identity(profile);
        profile.data_application = text.data_application.c_str();
        active_profile_name() = edition.empty() ? std::string(hand->name) : std::string(hand->name) + " (" + edition + ")";
        decisions().emplace_back("profile", active_profile_name() + " (hand-written, matched by executable hash)");
        return active_profile_name();
    }

    profile = GameProfile{};
    apply_app_identity(profile);
    profile.data_application = text.data_application.c_str();
    profile.disc_id = text.disc_id.c_str();
    profile.disc_id_display = text.disc_id_display.c_str();
    profile.game_title = text.title.c_str();
    profile.executable_path_on_disc = "PSP_GAME/SYSDIR/EBOOT.BIN";
    profile.param_sfo_path_on_disc = "PSP_GAME/PARAM.SFO";
    // The app checked the executable when it was added; these are what it found.
    profile.encrypted_executable_sha256 = record.eboot_sha256.c_str();
    profile.executable_sha256 = record.executable_sha256.c_str();
    profile.decryption_tag = 0u;
    profile.decryption_key = {};

    // The usual user base: relocatable executables are placed there, and a
    // fixed-address one says its own addresses in its program headers.
    profile.load_base = psprecomp::kDefaultPspUserLoadBase;
    // A PSP-1000's 32 MiB unless the disc asks for more (MEMSIZE) or the image
    // does not fit.
    profile.guest_ram_bytes = kPsp1000Ram;
    std::string ram_reason = "32 MiB, as on a PSP-1000";
    if (record.memsize == 1u) {
        profile.guest_ram_bytes = kPsp2000Ram;
        ram_reason = "64 MiB: PARAM.SFO MEMSIZE=1 asks for the PSP-2000's memory";
    }
    try {
        const auto elf = psprecomp::Elf32Image::from_file(record.data_dir / "EBOOT.ELF");
        if (elf.required_ram_size(profile.load_base) > profile.guest_ram_bytes) {
            profile.guest_ram_bytes = kPsp2000Ram;
            ram_reason = "64 MiB: the executable's image does not fit in 32 MiB";
        }
    } catch (const std::exception &) {
        // Reported when the game starts.
    }
    profile.boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    // Nothing is known about code the game loads at run time; the interpreter
    // runs whatever it is, and the next compile can include it.
    profile.overlay_slots = {};

    // The game names its save folders itself when it saves; the disc id is
    // the main one on every disc seen so far.
    profile.save_game_name = text.disc_id.c_str();
    text.save_description = "Game data";
    main_save_folder() = SaveFolder{text.disc_id.c_str(), text.save_description.c_str(), true};
    profile.save_folders = std::span<const SaveFolder>(&main_save_folder(), 1u);
    profile.adhoc_product_code = text.disc_id.c_str();
    // A console of the disc's region: the third letter of the disc id is the
    // region (J Japan; U, E, A, K elsewhere). Japan: Japanese, confirm with
    // circle; elsewhere English, confirm with cross.
    const bool japanese = text.disc_id.size() > 2u && text.disc_id[2] == 'J';
    profile.system_language = japanese ? 0u : 1u;
    profile.confirm_button = japanese ? 0u : 1u;
    active_profile_name() = "auto";

    decisions().emplace_back("profile", "automatic");
    decisions().emplace_back("guest_ram", ram_reason);
    decisions().emplace_back("load_base", "0x08804000, the usual user base");
    decisions().emplace_back("overlays", "none declared; code loaded at run time is interpreted");
    decisions().emplace_back("save_folder", text.disc_id + " (the disc id)");
    decisions().emplace_back("adhoc_product", text.disc_id + " (the disc id)");
    decisions().emplace_back("console", japanese ? "Japanese, confirm with circle (a Japanese disc)"
                                                 : "English, confirm with cross (a disc from outside Japan)");
    decisions().emplace_back("camera, widescreen, glyph cache, extra HLE, patches", "none (hand profiles only)");
    return "auto";
}

std::vector<std::pair<std::string, std::string>> describe_active_profile() { return decisions(); }

} // namespace portablekit::app

namespace portablekit {

// The one definition the framework links against.
const GameProfile &game() {
    static const bool initialised = [] {
        app::activate_launcher_profile();
        return true;
    }();
    (void)initialised;
    return app::active();
}

} // namespace portablekit
