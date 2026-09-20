#pragma once

// What the framework has to ask a game rather than assume about it.
//
// Everything under host/ is meant to work for any PSP title. Wherever it used
// to name Monster Hunter Portable 3rd, its disc, its memory layout or its save
// folders, it now reads one of these fields instead. A port supplies exactly
// one definition of portablekit::game() and the framework links against it.
//
// The rule for what belongs here: a value the framework needs but cannot
// derive from the executable it was given. Anything the ELF already says (its
// entry point, its segments, its imports) is read from the ELF, not declared.

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace psprecomp {
class Elf32Image;
class Runtime;
} // namespace psprecomp

namespace portablekit {

class HleRegistrar;

// One save folder on the memory stick, as the game names it.
struct SaveFolder {
    const char *name;        // SAVEDATA_DIRECTORY, e.g. "ULJM05800QST"
    const char *description; // shown in the save screen, e.g. "Downloaded quests"
    bool exported;           // included when the player exports or backs up
};

struct GameProfile {
    // --- What the port is called -------------------------------------------
    const char *app_name;          // the executable: "MHP3rdNative"
    const char *project_name;      // what the player sees: "Yakumo"
    // Environment variables are read as "<env_prefix>_NAME", so every port has
    // its own set and two of them can run side by side without sharing state.
    const char *env_prefix;        // "MHP3RD"
    const char *data_organization; // SDL_GetPrefPath organization
    const char *data_application;  // SDL_GetPrefPath application

    // --- The one release the installer accepts -----------------------------
    const char *disc_id;                 // DISC_ID in PSP_GAME/PARAM.SFO
    const char *disc_id_display;         // the same, as it is printed
    const char *game_title;
    const char *executable_path_on_disc; // "PSP_GAME/SYSDIR/EBOOT.BIN"
    const char *param_sfo_path_on_disc;  // "PSP_GAME/PARAM.SFO"
    // SHA-256 of the encrypted executable on the disc, and of the executable
    // the recompiled code was generated from. The installer checks both.
    const char *encrypted_executable_sha256;
    const char *executable_sha256;
    // The "~PSP" header's tag at offset 0xD0 and the key it selects. The
    // framework knows the header layout; only the key is per-release.
    std::uint32_t decryption_tag;
    std::array<std::uint8_t, 16> decryption_key;

    // --- How the game sits in guest memory ---------------------------------
    std::uint32_t load_base;
    std::uint32_t guest_ram_bytes;
    // argv[0] the game is started with, e.g. "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN".
    const char *boot_path;
    // Addresses of the code overlay slots inside the load image's BSS, from
    // the executable's section table. The end of each slot is the start of the
    // next, so the last entry is the end of the load image. Empty for a game
    // that loads no overlays.
    std::span<const std::uint32_t> overlay_slots;

    // --- Save data ---------------------------------------------------------
    const char *save_game_name;             // the main folder, e.g. "ULJM05800"
    std::span<const SaveFolder> save_folders;

    // --- Ad hoc multiplayer ------------------------------------------------
    // The product code the game announces to other players. Two players must
    // agree on it, so it is the original release's code even when the port's
    // disc id is a different one.
    const char *adhoc_product_code;

    // --- Hooks -------------------------------------------------------------
    // Called after the framework has registered its own HLE, so a game can add
    // calls no other game makes or replace one the framework gets wrong for it.
    // Null means there are none.
    void (*register_extra_hle)(HleRegistrar &) = nullptr;
    // Called once the executable is in memory and before it runs, for the
    // per-game patches that have no better home. Null means there are none.
    void (*patch_loaded_image)(psprecomp::Runtime &, const psprecomp::Elf32Image &) = nullptr;
};

// Defined by the port, exactly once. Everything under host/ reads the game
// through this and nothing else.
[[nodiscard]] const GameProfile &game();

// getenv("<env_prefix>_" + name), so host code names the variable without the
// game's prefix: env("TRACE_GE") reads <prefix>_TRACE_GE in Yakumo.
[[nodiscard]] const char *env(const char *name);
// True when the variable is set to anything at all, the convention every
// switch in the port already used.
[[nodiscard]] bool env_set(const char *name);
// The full name of that variable, for messages and for the menu's list of
// settings the environment has overridden.
[[nodiscard]] std::string env_name(const char *name);

} // namespace portablekit
