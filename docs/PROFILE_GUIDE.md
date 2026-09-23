# Writing a profile

A profile is everything specific to one PSP title. It lives in the game's own repository, not here, and it should be a file of constants and a four-line `CMakeLists.txt`. If it has to be more than that, something belongs in the framework that is not there yet.

## The layout

```text
<game>/
  CMakeLists.txt        Four lines: add PortableKit, call portablekit_add_game()
  portablekit/          The framework, as a submodule
  host/<game>_profile.cpp   The profile: one definition of portablekit::game()
  config/nids.csv       Optional: NIDs this game imports that the framework does not name
  host/                 Optional: per-game HLE and patches, reached through the profile's hooks
  generated/            The AOT corpus, generated from the player's executable; never committed
  overlays/             One directory per recompiled code overlay; never committed
  game/                 The player's own files; never committed
  docs/                 Compatibility, testing and release pages for this port
```

Nothing derived from the game belongs in the repository: no disc image, no executable, no generated code, no captures of the game's own art, no saves.

## The build

```cmake
cmake_minimum_required(VERSION 3.20)
project(Tenkawa VERSION 0.1.0 LANGUAGES CXX)

add_subdirectory(portablekit)

portablekit_add_game(TenkawaNative
    PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES host/tenkawa_profile.cpp)
```

`portablekit_add_game()` does the rest: the corpus, the renderer and its shaders, the version header, FFmpeg, the NID table, the overlay modules, the stack size and the output directory. `SOURCES` are the profile's own host sources, relative to `PROFILE_DIR`; one of them must define `portablekit::game()`.

## The profile

`portablekit::GameProfile` in [`host/profile.hpp`](../host/profile.hpp) is the whole interface, and its comments say what each field is for. The rule for what belongs in it: **a value the framework needs but cannot derive from the executable it was given.** Anything the ELF already says — its entry point, its segments, its imports — is read from the ELF, not declared.

Read the values off the player's own disc rather than recalling them:

| Field | Where it comes from |
| --- | --- |
| `disc_id`, `game_title` | `PSP_GAME/PARAM.SFO` |
| `encrypted_executable_sha256` | `sha256` of `PSP_GAME/SYSDIR/EBOOT.BIN` |
| `executable_sha256` | `sha256` of what the installer decrypts it to |
| `decryption_tag` | The word at offset 0xD0 of `EBOOT.BIN` |
| `decryption_key` | The published key table's entry for that tag |
| `load_base`, `guest_ram_bytes` | The ELF's program headers |
| `overlay_slots` | The executable's section table, inside the load image's BSS |
| `save_game_name`, `save_folders` | The paths the executable itself contains |

Two fields are hooks rather than values. `register_extra_hle` adds calls this game makes that the framework does not implement, or replaces one it gets wrong for this game. `patch_loaded_image` is for per-game fixes with no better home. Both may be null, and a profile that needs neither is the goal.

## Generated code stays out of the repository

Recompiled code is a translation of the game's own executable, so it is derived from copyrighted material. Every player generates it from their own copy. Make that reproducible by documenting the executable identity and hash the profile expects, the exact generator command, and any deterministic pass that follows it.

## Where to put a fix

When the port needs a change, ask which repository it belongs in.

- It names the game, its addresses, its folders or its behaviour — the profile.
- It is true of the PSP, of a file format, or of any game that makes the same call — the framework.

A profile that grows its own kernel, its own renderer changes or its own copies of framework code has found a missing seam. Add the seam instead.
