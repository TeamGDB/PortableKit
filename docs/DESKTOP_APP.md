# PortableKit as a desktop program

A design, and a prototype of it in [`apps/portablekit`](../apps/portablekit): one program a player installs, points at the disc image of a PSP game they own, and plays. It recompiles the game on the player's own machine, and until that is done it plays the game under the interpreter.

The game throughout is LocoRoco 2 (`UCES-01059`), and on Windows also Monster Hunter Portable 2nd G (`ULJM-05500`). The prototype was built and run on a MacBook Air M1 with 8 GB of memory, macOS 27 and Apple clang 21, and on the maintainer's Windows 11 PC (Ryzen 5 5600, RX 5600 XT), built there with llvm-mingw and delivered as a portable folder. Linux is designed only; see [What was not verified](#what-was-not-verified).

The maintainer's decisions on the first version of this document are recorded under [Decisions](#decisions); this version implements them.

**Contents:** [The product](#the-product) · [What the program ships, what the player brings](#what-the-program-ships-what-the-player-brings) · [Architecture](#architecture) · [Any game: the automatic profile](#any-game-the-automatic-profile) · [Keys](#keys) · [Saves](#saves) · [Compiling on the player's machine](#compiling-on-the-players-machine) · [Switching to compiled code](#switching-to-compiled-code) · [Growing the corpus](#growing-the-corpus) · [The interface](#the-interface) · [The command line](#the-command-line) · [What changed in the framework](#what-changed-in-the-framework) · [What the prototype does](#what-the-prototype-does) · [Decisions](#decisions)

## The product

1. The player installs one program. It contains the runtime, the recompiler and, optionally, hand-written profiles that improve particular games. It contains **no game code and no keys**.
2. The player adds a game: the `.iso` of their own disc. Optionally they give a **keys file** they made themselves, or an **executable they decrypted themselves**.
3. The program identifies the game from the disc and the executable's SHA-256. **No per-game profile is required**: what the framework needs to know is read from the disc and the executable, and the rest has defaults. A hand-written profile that matches the executable's hash is used instead when the program carries one.
4. The player can **play at once**, under the interpreter, while the program recompiles the game to C++ and compiles it with a compiler on the player's machine, in the background. When the compiled code is ready the running game **switches to it without restarting**. The result is cached per game and per build of the program, and not compiled again until the program changes in a way that affects it.
5. Without keys, saves are kept unencrypted; the game cannot tell. With keys, saves are the PSP's own format and move to and from a PSP or PPSSPP.
6. Everything is available from a window and from a command line; the window is a front end over the same functions.
7. Developers keep using the ports' repositories exactly as today.

## What the program ships, what the player brings

*Notes for the maintainer, not legal advice.*

| | Shipped by the program | Brought by the player |
| --- | --- | --- |
| Runtime, HLE, renderer, recompiler | yes: the project's own code (MIT) and third-party code under its own licences (see [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)) | |
| Hand-written profiles | yes: the ports' own code (addresses and constants read off the game, no game code) | |
| Compiler | macOS: no (Apple's, installed by the player). Windows and Linux: a redistributable LLVM toolchain, see [below](#compiling-on-the-players-machine) | |
| Keys | **no**; not even in an encoded form. SHA-256 fingerprints of the fixed keys are compiled in, to tell the player which line of their file is wrong; a fingerprint does not reveal a key | the keys file, optional |
| Game | **no** | the disc image, and optionally the executable they decrypted |
| Generated C++, compiled game code | **no**: made on the player's machine from the player's disc, kept in its data folder, never uploaded. `export` hands it to the player, marked as theirs for their own use only | |

The program never downloads anything from a game or about a game. What the player's machine makes from their disc stays on it; the compatibility report the program can write ([Growing the corpus](#growing-the-corpus)) holds addresses and counts, and the interface should say so before a player attaches it anywhere. A release should keep today's wording from the ports' READMEs: the player supplies their own legally obtained copy.

What changes compared with today's releases (Yakumo ships with keys compiled in, as `executable_preparation.cpp` and `savedata_crypto.cpp` do in this framework): the program no longer contains the console's keys, and a game whose disc carries no unencrypted executable needs either the player's keys or the player's own decrypted executable.

## Architecture

```
  window (launcher_ui)        command line (app_main)
            \                    /
             the app's core: apps/portablekit/src
   game_import   keys_file   auto_profile   corpus (compile)   corpus_loader
        |            |            |               |                  |
        |     crypto_keys()   game()         psp_recomp + the     register_generated_functions()
        |            \        /              system compiler          |
        |          host/ (the framework, unchanged but for seams)     |
        +--------> host/main.cpp as portablekit_host_main() <---------+
```

- A **game** is a directory `<data>/games/<disc id>-<first 8 hex of the executable's SHA-256>/` holding what the port's data directory always held (`EBOOT.ELF`, `settings.ini` naming the disc image in place, `ms0/`), plus `game.txt`, what the app found out.
- **Running** a game means: make `game()` describe it, point `PORTABLEKIT_DATA_DIR` at its directory, and call the framework's own `main()` under the name `portablekit_host_main`. From there it is exactly a port.
- The framework asks `psprecomp::register_generated_functions()` for the game's code, as it always did. A port links its generated registry there; the app defines it itself, and loads the player's compiled library ([Switching to compiled code](#switching-to-compiled-code)).
- The window runs in one process and each game in another (`portablekit run <game>`), so a game's settings, its window and a crash stay its own.

**The program is portable.** Everything it writes lives in one folder beside it, and nothing anywhere else: no AppData, no Application Support, no `~/.config` or `~/.cache`, no registry. Moving or deleting that folder moves or deletes everything, the compiled games and the saves included.

| Program | Data folder |
| --- | --- |
| `portablekit.exe`, `portablekit` (Windows, Linux, a macOS command-line build) | `data/` next to the executable |
| `PortableKit.app` (macOS) | `PortableKit Data/` next to the `.app`: a signed bundle must not be written into |

- `--data-dir <folder>` (or `PORTABLEKIT_HOME`) chooses another folder, for the command line and the window alike. `PORTABLEKIT_CACHE` moves only the compiled games.
- The folder holds `keys.txt`, `games/<id>/` (the prepared executable, `settings.ini`, `ms0/` with the saves, the port's logs), `cache/<id>/` (generated C++ while compiling, the compiled libraries, the interpreter's profile) and `launcher/` (the window's own settings). On Windows the compiler is in `toolchain/` beside the program, inside the same installation.
- **No fallback.** If the folder cannot be written (a read-only disk, `Program Files`), the program says so and what to do, and stops; it never quietly writes to a system folder instead.
- **macOS App Translocation**: an app opened where it was downloaded runs from a random read-only copy, and a folder beside that copy would be lost. The program recognises the path (`/AppTranslocation/`) and asks the player to move `PortableKit.app` into a folder of its own first.
- **What still touches the system**, none of it the program's own data: on macOS, Apple's Command Line Tools (installed by the player, used through `xcrun`); the GPU driver's shader caches; the fonts the game's text is drawn with, read from the system's font folders. Compilers write their temporary files beside their outputs, in the data folder.

## Any game: the automatic profile

Today a port is a `GameProfile` written in C++ ([`host/profile.hpp`](../host/profile.hpp)). The app builds one at run time instead ([`auto_profile.cpp`](../apps/portablekit/src/auto_profile.cpp)), field by field:

| `GameProfile` field | Where the automatic profile gets it | Known statically? |
| --- | --- | --- |
| `disc_id`, `game_title` | `PSP_GAME/PARAM.SFO`: `DISC_ID`, `TITLE` | yes |
| `executable_path_on_disc`, `param_sfo_path_on_disc`, `boot_path` | the UMD layout every game disc has | yes |
| `encrypted_executable_sha256`, `executable_sha256` | computed when the game is added | yes |
| `decryption_tag`, `decryption_key(_table)` | the tag is in `EBOOT.BIN` at `0xD0`; the key for it comes from the player's keys file (`tag.<hex>`), not from a profile | yes, with keys |
| `load_base` | the usual user base, `0x08804000`; a fixed-address executable carries its own addresses | yes |
| `guest_ram_bytes` | 32 MiB, or 64 MiB when `PARAM.SFO` has `MEMSIZE = 1` or the image does not fit | yes |
| `overlay_slots` | none. Code a game copies into memory at run time is not known statically; the interpreter runs it (see [Growing the corpus](#growing-the-corpus)) | no, only by running |
| `save_game_name`, `save_folders` | the disc id. The game names its folders itself when it saves; the save screen's labels are generic ("Game data") | the main one; the others only by running |
| `adhoc_product_code` | the disc id | a guess that holds for most games |
| `app_name`, `project_name`, `env_prefix`, data names | the app's own (`portablekit`, `PORTABLEKIT_*`) | — |
| `register_extra_hle`, `patch_loaded_image`, `camera`, `view_aspect_frame`, `interpolation_thresholds`, `glyph_cache`, `trigger_profiles`, `other_releases` | none: the framework's defaults | these are what hand profiles are for |

The entry point, the code ranges, the imports and relocations are read from the ELF by the framework already; the recompiler finds the code by its own analysis (`psp_recomp --auto`). Modules the game loads from its disc that the framework implements as HLE (`libfont.prx`, `psmf.prx` and the like) are recognised by name at run time.

`portablekit info <game>` prints each decision and why.

### Hand-written profiles as extras

A port's profile source can be compiled into the app: `-DPORTABLEKIT_APP_PROFILES=/path/to/Purun/host/purun_profile.cpp`. Each is compiled with its `portablekit::game()` renamed (`-Dgame=portablekit_app_profile_N`), so the port's file is used unchanged, and is chosen when its `executable_sha256` matches. The app then replaces only the names that decide where it keeps things. The rename is a prototype's shortcut; the lasting form is a registration macro in `profile.hpp` (`PORTABLEKIT_PROFILE(purun) { ... }`) that a port uses instead of defining `game()`, and a port's own build keeps working through a one-line `game()` the macro also defines.

**What Purun's hand profile adds over the automatic one, for LocoRoco 2**, measured: the app built with `PORTABLEKIT_APP_PROFILES` pointing at Purun's profile as it is on Purun's `main` picks it by hash ("profile: purun_profile (hand-written, matched by executable hash)"), and the same scripted run (title, Continue from a save, into level 2) reaches the same frame at the same speed with either profile: 1.8 ms of guest time a frame at -O2 in both. Field by field, that version adds only the release's name and hash check (the app checks hashes itself), the key table for the executable's tag (unneeded: the disc carries the same executable unencrypted as `BOOT.BIN`, and with keys it comes from the keys file), and a label for the save folder. So for LocoRoco 2 as the framework's `main` stands, the automatic profile is complete.

### Editions: ProfileVariant

One profile can cover several executables of the same disc id: the release, and the release with a fan patch applied (FUComplete on Monster Hunter Portable 2nd G, in several versions), or a translation. `GameProfile::variants` lists them ([`host/profile.hpp`](../host/profile.hpp)):

```cpp
constexpr ProfileVariant kEditions[] = {
    {.key = "fuc-1.4", .name = "FUComplete 1.4",
     .executable_sha256 = "…",               // the patched ELF; its EBOOT.BIN is plain
     .patch_loaded_image = &fuc_patches,     // what this edition does differently
     .overlay_slots = kFucSlots},            // empty: the profile's own
};
GameProfile{ …, .variants = kEditions, .run_unknown_executables = true, … };
```

- Each edition is known by the SHA-256 of its executable and, when its `EBOOT.BIN` is encrypted, of that. Setup accepts any of them; an encrypted one is decrypted with the profile's key and checked against its own hash, a plain one is taken as it is.
- `host/main.cpp` picks the edition from the executable's hash. The profile's hooks and overlay slots are read through `active_register_extra_hle()`, `active_patch_loaded_image()` and `active_overlay_slots()`, so an edition's own take effect.
- **Each edition has its own corpus.** The corpus linked into a port's executable runs only the profile's own executable. An edition's is generated into `<profile>/generated-variants/<key>/`; `portablekit_add_game()` builds it into `bin/corpora/<key>.<ext>`, a library that needs nothing from the executable ([the corpus ABI](#the-corpus-abi-and-the-cache)), and `host/corpus_library.cpp` loads it after checking the corpus ABI version and that it was generated from this very executable (`psp_recomp` now writes `generated_corpus_source_sha256()` into every registry).
- Overlays need nothing new: an overlay corpus is identified by its header and code hash, so a patch's replaced overlays simply have libraries of their own beside the release's.
- Saves are shared: the save folders are the profile's.
- **An executable nobody listed** runs under the interpreter with a warning instead of running code generated from another one (which it did before, after a warning). `run_unknown_executables` decides whether setup accepts such a plain executable at all.
- In the app, a hand profile matches by any of its editions' executables; the app applies that edition to its copy of the profile and compiles its own corpus for it, as for any game.

Verified on the Mac with a throwaway port whose profile had one edition, LocoRoco 2's executable, with its corpus in `generated-variants/smoke/`: the build made `bin/corpora/smoke.dylib`, the port printed "Edition: Edition-library test" and ran 789 048 functions from the library without the interpreter; the same executable changed by one byte ran under the interpreter with the warning. `tests/profile_tests.cpp` covers the lookups. This is the mechanism the Fubuki port (MHP2G and FUComplete, #29) builds on; code FUComplete loads from plain files outside `DATA.BIN` is left to `set_code_miss_hook()` and the interpreter.

That changes with the next framework: Purun's working tree already sets `frame_vblanks = 1` (the game's own rate is 60 frames a second, not 20) and interpolation thresholds for a 2D game, fields that the pull requests now open add to `GameProfile` (#25, #27). Those are exactly what a hand profile is for: measured facts about one game that no default gets right. It also shows the cost of compiling profiles in: a profile follows the framework's version, and the app has to be built against a framework that has the fields its profiles use (Purun's current file does not compile against `main`).

### Honest expectations

What decides whether an unknown game works is not the profile but the framework's coverage, which is the same for every game:

- **Likely to work out of the box**: games that keep all their code in `EBOOT.BIN`, use the system modules the framework implements as HLE, and draw with GE features one of the three games already exercised. The interpreter makes code coverage a non-issue: whatever the recompiler missed still runs, only slower.
- **Will need work in the framework, not a profile**: unimplemented system calls (the log names them; LocoRoco 2 imports 53 that nothing implements and still plays), GE features no game has used yet, a movie or audio format no game has used yet.
- **Will need a hand profile**: code overlays that must be recompiled to run at full speed (Yakumo's 355 overlays in 12 slots: the interpreter runs them, but too slowly to play), camera and widescreen drivers, per-game patches, glyph caches, trigger layouts.
- **Do not work yet**: executables compressed inside `~PSP` (the preparation code refuses them), games whose own `.prx` modules are encrypted and loaded at run time (the loader recognises stock modules only), and releases whose `~PSP` tag uses a header layout the preparation code does not know.

The app's "compatibility report" (`interpreted.txt` today; see [Growing the corpus](#growing-the-corpus)) and the port's own `LIST_STUBS` output are what a player can send to say what a game needs.

## Keys

### Where the framework uses keys today

| Where | Keys | Used for |
| --- | --- | --- |
| `host/install/executable_preparation.cpp` (before this branch) | KIRK slot `0x5D` (header key), KIRK command 1 AES key | decrypting `EBOOT.BIN` |
| each profile: `GameProfile::decryption_key` / `decryption_key_table` | the key or the 0x90-byte table the executable's tag selects | decrypting `EBOOT.BIN` |
| `host/save_data/savedata_crypto.cpp` (before this branch) | KIRK command 4/7 keys `0x03 0x04 0x0C 0x0E 0x10 0x12 0x53 0x57 0x64`, save-data keys 2–7 | reading and writing encrypted saves, `PARAM.SFO` hashes, import and export |
| Yakumo `profiles/mhp3rd/host/install/executable_preparation.cpp` | its own copy: tag key, header key, command 1 key | decrypting its `EBOOT.BIN` |
| Yakumo `profiles/mhp3rd/host/save_data/savedata_crypto.cpp` | its own copy of the save keys | saves |

This branch moves every framework key behind one function, `portablekit::crypto_keys()` ([`host/crypto_keys.hpp`](../host/crypto_keys.hpp)). Ports link `crypto_keys_builtin.cpp`, which holds the same values, so nothing changes for them. The app links its own definition, which reads the player's file. The seam is the whole change; taking the keys out of the ports is then a decision, not work:

- **Purun**: nothing to change to keep working. To stop carrying keys: delete `kKeyTableWords` from `purun_profile.cpp` (the disc's `BOOT.BIN` is the same executable), and build with `EXTERNAL_KEYS` and a keys-file loader; the framework could offer that loader as `crypto_keys_file.cpp` beside the built-in one.
- **Tenkawa**: the same; its `decryption_key` would move to the keys file as `tag.D91613F0`. Its disc has no usable `BOOT.BIN` as far as its profile says, so without keys it needs the player's decrypted executable.
- **Yakumo**: its releases keep their keys (the maintainer's decision). When it moves onto the framework ([#2](https://github.com/TeamGDB/PortableKit/issues/2)) its two copies go and it links `crypto_keys_builtin.cpp`, as Purun and Tenkawa do.

### The keys file

Text, one key per line, hex, `#` for comments; `<data>/keys.txt` (in the program's data folder), or `PORTABLEKIT_KEYS`:

```
kirk.aes.5D  = <16 bytes>      # KIRK command 4/7 key slots, by slot number in hex
kirk.cmd1    = <16 bytes>      # the KIRK command 1 AES key
savedata.2   = <16 bytes>      # save-data keys 2 to 7
tag.C0CB167C = <16 bytes, or the 0x90-byte table of an old header layout>
tag.C0CB167C.slot = 5D         # optional: the KIRK slot the tag's header uses
```

- `portablekit keys import <file>` checks the file and copies it into the data folder. Every fixed key is checked against a SHA-256 fingerprint compiled into the program, so the player is told "`savedata.4` is not the right key" instead of getting garbage later. Tag keys have no fingerprint: an executable that decrypts to an ELF the runtime can load is the check.
- What each group enables: `kirk.aes.5D` + `kirk.cmd1` + the game's `tag.*` decrypt that game's `EBOOT.BIN`; the nine save slots and `savedata.2`–`7` encrypt and decrypt saves. `portablekit keys status` says which of these the file provides.
- Without keys, a game can still be added when its disc carries an unencrypted `BOOT.BIN` (LocoRoco 2 does), or when the player gives an executable they decrypted (`--executable`). The program never names a source of keys.

Messages when something is missing, as the prototype prints them:

- *"The game's executable on this disc is encrypted, and the disc carries no unencrypted copy (BOOT.BIN). Add a keys file (portablekit keys import <file>), or give an executable you decrypted yourself with --executable <EBOOT>."* (exit code 11)
- *"EBOOT.BIN is encrypted with tag D91613F0, and the keys file has no key for it (tag.D91613F0)."* (exit code 11)
- *"The save is encrypted, as a PSP writes it. Importing it needs the console's keys: add a keys file."* (the save menu's import)
- In the log, for an encrypted save found in `ms0`: *"the save is encrypted, and reading it needs the console's keys (add a keys file)"*.

## Saves

- **Without keys** the game's key is still read from its save request and remembered, but the save is written unencrypted (`SAVEDATA_PARAMS` flags 0), as early PPSSPP versions wrote them. The game sees exactly the same data. Such saves load in PPSSPP; a PSP does not accept them.
- **With keys**, saves are written exactly as a PSP writes them (mode 5), which is what the framework has always done. Import checks hashes and decrypts; export copies.
- **Keys added later**: saves written without keys stay readable (flags 0 needs no key), and the menu's Export now writes such a save the way a PSP does, with the game's key remembered from its save request, so the exported copy loads on a PSP; the menu says which saves it encrypted. Tested with a save written without a key (`portablekit_savedata_tests`).
- An encrypted save (from a PSP, or from a port built with keys) in a no-keys installation is refused with the message above rather than handed to the game broken.

## Compiling on the player's machine

### The numbers

LocoRoco 2's corpus: 19 201 functions, 193 C++ units plus the registry and the app's entry file, 193 MB of C++. "Largest compiler" is the peak resident size of one compiler process.

**Mac** (M1, 8 GB, Apple clang 21, 2 jobs, `portablekit compile`, nothing else running; the machine was already using 3.4 GB of swap for other programs):

| Step | -O0 | -O2 |
| --- | --- | --- |
| Recompile to C++ (`psp_recomp`, one thread, under 1 GB) | 178 s | 181 s |
| Compile 195 units | **173 s** | **2144 s** (36 min) |
| Largest compiler process | 287 MB | 491 MB |
| Link | 1.6 s | < 1 s |
| Library | 335 MB | 135 MB |
| Objects (deleted after linking) | 565 MB | |
| **From nothing to ready** | **6 min** | **42 min** |
| The same -O0 compile while the game runs | 282 s + 282 s: **9.5 min** | |
| Tiered (`--opt tiered`, 4 jobs): -O0, then -O2 from the same C++ | 201 s + 153 s: ready in **6 min** | + 1946 s: **38 min** in all |

**Windows** (the maintainer's PC: Ryzen 5 5600, 12 threads, 16 GB, Windows 11; the trimmed llvm-mingw 20260922, clang 23, 6 jobs; the units only, recompiled on the Mac):

| Step | -O0 | -O2 |
| --- | --- | --- |
| Compile 195 units | **74 s** | **1064 s** (18 min) |
| Largest compiler process | 218 MB | 318 MB |
| Objects | 388 MB | 198 MB |
| Link the DLL against the program's import library | 1.2 s, 281 MB | |
| Load it and register 789 044 addresses | 141 ms | |

The -O0 corpus compiles **12× faster** than -O2 on the Mac, and 14× on Windows. Memory is not the limit at either level with clang: 0.3 GB per job at -O0, 0.5–0.9 GB at -O2 (the largest unit's compiler reached 0.9 GB in the tiered run). Four jobs instead of two on the M1 gained only 9% at -O2 (it has four fast cores, and the machine was swapping); the Ryzen with 6 jobs was twice as fast. The framework's own build allows one generated unit per 4 GiB, which is far too cautious for clang (it was set for MSVC, where units took over a gigabyte); the app uses one job per GiB left after 3 GiB, and at most half the cores.

**Speed** of each, in the same scripted run (LocoRoco 2 runs at 20 frames a second, so a frame's budget is 50 ms). Guest time per frame, and the lowest one-second speed seen, which is where the game loads a level:

| | Interpreter | -O0 | -O2 |
| --- | --- | --- | --- |
| Title sequence | 13–24 ms | 11–18 ms | 3–8 ms |
| Level 2, playing | **22.5 ms** (21.4–23.5) | **11.8 ms** (10.4–12.8) | **1.8 ms** (1.1–2.0) |
| Lowest speed, loading a level | 6% (frames of 0.5–0.8 s) | 26% | 86% |
| Average speed | 100% | 100% | 100% |

So for this game on this machine:

- **The interpreter is playable.** It is 12× slower than -O2 in play, but LocoRoco 2 needs a fraction of a PSP, so even interpreted it stays within its frame; the price is long hitches while a level loads. A heavier game (Monster Hunter's 3D, Tenkaichi's fights) will not be playable interpreted, and the design must not promise it: "plays slowly" is the honest wording.
- **-O0 is the right first tier**: ready in 6 minutes (under 10 with the game running), twice as fast as the interpreter, and without the interpreter's worst hitches.
- **-O2 is 6.5× faster than -O0** and worth the 36 minutes in the background: the first tier is what the player waits for, the second is free.

### macOS: Apple's Command Line Tools

- **Use Apple's compiler; do not ship one.** It is free, it is what the program was built with (the corpus shares the program's C++ ABI and libc++ by construction), it is signed by Apple, and it is kept up to date by the system. Bundling a clang would add ~400 MB to the download and a second libc++ to reason about.
- **Detection**: `xcode-select -p` answers without side effects; `xcrun --find clang++` then gives the compiler. Calling `xcrun` or `/usr/bin/clang++` without the tools opens the system's install prompt, so the app asks `xcode-select` first and says what to do: *"Compiling needs Apple's Command Line Tools, which are free. Install them with "xcode-select --install" ..."*. The window can run `xcode-select --install` itself, which opens Apple's dialog; the download is roughly 0.7 GB and the tools take **1.7 GB** installed (measured here, version 27.0).
- The app calls `xcrun clang++`, which picks the SDK itself. It compiles each unit with `-std=c++20 -O<n> -g0 -fPIC -fvisibility=hidden` and links a bundle with `-bundle -bundle_loader <the program>`, so the corpus's references to the runtime bind to the running executable. Tested with Xcode's toolchain; with the Command Line Tools alone it is the same `clang++` through `xcrun`, but that was not tested on a machine without Xcode.
- Without the tools the game still plays under the interpreter; nothing else is lost.
- **Signing**: the program is signed and notarized; the corpus library is made on the player's machine and loaded with `dlopen`. Hardened-runtime programs refuse unsigned libraries unless they have the `com.apple.security.cs.disable-library-validation` entitlement, so the release needs it (or the app ad-hoc signs each library it makes with `codesign -s -`, which the linker already does on Apple Silicon). Not tested with a notarized build.

### Windows: ship an LLVM toolchain (done)

Three ways were weighed:

| | A. Build the app with llvm-mingw, ship the same toolchain | B. Build with MSVC, require MSVC Build Tools | C. Build with MSVC, ship clang-cl + lld-link |
| --- | --- | --- | --- |
| Player installs | nothing | Visual Studio Build Tools, ~2–7 GB, with Microsoft's licence to accept | nothing, but the MSVC headers and libraries are still needed |
| Redistributable | yes: LLVM (Apache-2.0 with LLVM exception), mingw-w64 headers and CRT (public domain, ZPL and permissive), libc++ | nothing to ship | clang-cl and lld-link yes; the MSVC STL is Apache-2.0 with LLVM exception, but the UCRT and Windows SDK headers and import libraries are **not** redistributable |
| Size shipped | x86_64-only subset of llvm-mingw 20260922: **334 MB unpacked, 83 MB zip, 54 MB tar.xz** (clang, lld, libLLVM, libclang-cpp, headers, x86_64 sysroot) | 0 | ~150 MB plus a downloader for the SDK parts (the way `xwin` fetches them from Microsoft) |

**A was chosen, and it works.** On the maintainer's PC, with llvm-mingw 20260922 (clang 23), CMake, Ninja, the Vulkan SDK and SDL3's MinGW package, the whole app configures and builds; the only change the framework needed was one `#undef interface` in `host/adhoc/discovery.cpp` (MinGW's COM headers define it). FFmpeg is the framework's pinned prebuilt LGPL build, whose MSVC import libraries lld links as they are. The result, delivered as a portable folder:

```
PortableKit\                          403 MB in all
  portablekit.exe  psp_recomp.exe     the program and the recompiler
  SDL3.dll  avcodec-61.dll  avutil-59.dll  swresample-5.dll  libc++.dll  libunwind.dll
  share\portablekit\include\          the two headers generated code includes
  toolchain\                          the trimmed llvm-mingw the app compiles with
  data\                               empty until the player adds a game
  README.txt  LICENSE.txt  FFmpeg-LICENSE.txt  FFmpeg-SOURCE.txt
```

With the corpus ABI, a game's library links on its own (`-shared -static`) and imports nothing from the program, so it could as well be built by another compiler than the program was. Measured there with Monster Hunter Portable 2nd G ([numbers](#the-numbers) for LocoRoco 2): added without keys from `BOOT.BIN`; under the interpreter it reaches the Dolby logo and, with Start pressed through the intro, the game menu; `portablekit compile --opt 0` recompiles it in 475 s and compiles it in 16 s on 6 jobs (a 62 MB `corpus.dll`), and the game then runs from the library. The library window was driven by a script there too.

Two things found on the way are fixed on this branch: `include/psprecomp/allegrex_context.hpp` used `std::max` without `<algorithm>` (libc++ 23 does not bring it in through other headers, and every generated unit failed to compile), and the shader step called `python3`, which on Windows reaches the Microsoft Store's placeholder (taken from #29).

### Linux (x86_64, Steam Deck)

- **The system compiler** (`c++`, `g++`, `clang++` on `PATH`) works on a desktop distribution and is what the prototype looks for. The corpus is linked `-shared -Wl,--no-undefined`: with the corpus ABI it needs nothing from the executable.
- **But the Steam Deck has none**: SteamOS's root is read-only and ships no compiler. So a Linux release needs a bundled toolchain too: an LLVM release for x86_64 (clang + lld, about 100–150 MB compressed) and a sysroot with the glibc headers of the release's glibc floor (the same floor the program is built against, e.g. glibc 2.31), and libstdc++'s headers matching the program's statically linked libstdc++. Linking the corpus against the running program only, with `-nostdlib`, keeps it off the system's libstdc++ entirely.
- **A portable folder** (a `.tar.xz` with the program, its libraries in `lib/`, the toolchain and `data/`) fits the portable rule directly and is the recommended Linux form, the Steam Deck included.
- **AppImage**: the program runs from a read-only mount, so the data folder goes next to the `.AppImage` file (`$APPIMAGE` names it), and `dlopen` from there works.
- **Flatpak** does not fit the portable rule: the sandbox's writable place is `~/.var/app/<id>/`. If a Flatpak is wanted, its data folder would be that, as the one exception, and the toolchain would come from the SDK extension `org.freedesktop.Sdk.Extension.llvm*`.
- The release build links libstdc++ statically with hidden symbols (see `PORTABLEKIT_RELEASE` in `cmake/PortableKit.cmake`); the corpus, which shares no C++ type with the program any more, links its own statically too.

### Cutting the time

| Idea | What it saves | Cost |
| --- | --- | --- |
| **Tiers**: compile -O0 first (6 min here), switch to it, then -O2 in the background and switch again | time to compiled code: 42 min → 6 min | 8% more compile work; the switch already exists |
| **Pipelining**: `psp_recomp` writes units one by one (1 per second); start compiling each as it is written | the 3 minutes of recompiling overlap with compiling | `psp_recomp` reporting each finished unit, which it already prints |
| **Share the C++ between levels**: the generated code does not depend on the level | the second 3-minute recompile | done in the prototype's tiered compile: -O2 takes -O0's `generated/` |
| **Hot units at -O2, the rest at -O1/-O0**, from the interpreter's profile (`interpreted.txt`) or a first -O0 run's dispatch counts | most of the -O2 time: in level 2 the hot set is 14 of 193 units | a profile to collect before the fast build |
| **Parallelism by memory**: jobs from memory left and the per-unit peak (0.3–0.5 GB with clang) instead of one per 4 GiB | on 8 GB: 4 jobs instead of 2 | the app records the peak per compile, so it can adjust |
| **Split large functions**: the recompiler emits one function per 16 KiB unit (up to ~20 000 lines); -O2's time grows faster than the function's size | not measured | recompiler work |
| **Keep the objects** and relink only what changed when a profile-guided pass adds code | minutes per pass | 565 MB of disk per game |
| Background priority | nothing, but the game never stutters: compiles run at `nice 10` (Windows: below normal) | — |

For Yakumo, whose corpus plus 355 overlays take about two hours on an M1 at `-j2 -O2`, the tiers matter most: at the 12× measured here, an -O0 first tier would be ready in about ten minutes.

### The corpus ABI and the cache

Generated code used to include `runtime.hpp` and reach into `Runtime`: its private chain state through an inline template, `GuestMemory`'s slow paths, a dozen out-of-line members taking `std::string`, and three process globals. So every compiled corpus was tied to one build of the runtime, to the compiler that built it, and, as a library, to the program's exported symbols. That is gone ([`include/psprecomp/corpus_abi.hpp`](../include/psprecomp/corpus_abi.hpp)):

- **`CorpusRuntime`**, the base class of `Runtime`: plain data (the chain state, a memory view, pointers to the process state and to the host's table) and inline members, the direct chain among them, doing exactly what it did. Generated functions take `CorpusRuntime &` and never see more.
- **`AotFastView`**, moved out of `GuestMemory`, with its slow paths reached through a table of function pointers.
- **`CorpusHostApi`**, one table per process of everything else: chained calls, the scheduler's safe point, stopping, imports, context tokens, extra instructions, unaligned words, and registration (with `const char *` names).
- A corpus therefore references **no symbol of the program**: `nm -u` on LocoRoco 2's library lists only libc and the C++ runtime's unwinder. It links as a plain shared library, can be built by another compiler than the program was, and survives every runtime change that leaves this header, `AllegrexContext` and the recompiler alone.
- Measured on LocoRoco 2 at -O0: the same speed (11.8 ms of guest time a frame in level 2 before and after), registration in 0.07 s instead of 0.14, and a library of 234 MB instead of 335 (no `std::string` built for each of 789 044 registrations).
- Host functions registered as guest code (the kernel's stubs, the import stub, the tests' fixtures) take `CorpusRuntime &` and cast back. Overlay libraries take `CorpusRuntime &`; `kOverlayAbiVersion` is 2, so older ones are refused. Every port regenerates and recompiles its corpus once (accepted by the maintainer).

The app's cache key follows from it. The library is `<data>/cache/<game id>/<abi>-O<level>/corpus.<dylib|so|dll>`, and `<abi>` is a SHA-256 over `corpus_abi.hpp`, `allegrex_context.hpp`, the recompiler's sources (`codegen_main.cpp`, the decoder, the analysis, the ELF reader) and the compile definitions ([`cmake/corpus_abi.cmake`](../apps/portablekit/cmake/corpus_abi.cmake)); the runtime's `.cpp` files and `runtime.hpp` no longer invalidate it. The library exports the corpus ABI version, the build's ABI hash and the executable it was generated from, and the program refuses one whose answers differ. When a game is compiled for a new ABI, its corpora of older ones are removed.

## Switching to compiled code

Hot swap is feasible and the prototype does it:

- The framework dispatches guest code in an outer loop; between two dispatches no generated code is on the host stack and `ctx.pc` is simply the next guest address. The interpreter stops only at instruction boundaries outside a branch's delay slot.
- A watcher thread looks at the cache once a second. When a better corpus is ready, it `dlopen`s it and checks its ABI and executable there, off the game's thread. A heartbeat hook (`set_runtime_heartbeat_hook`, every 4096 dispatches) then registers its functions at the next dispatch boundary. The next dispatch finds compiled code at `ctx.pc`: every instruction address of the corpus is registered, so the switch can happen anywhere in the game's code.
- Measured in level 2 of LocoRoco 2, the game running under the interpreter while the -O0 corpus compiled beside it: guest time went from 20–22 ms a frame to 10–11 ms at the switch. Registering 789 048 addresses takes 0.14 s. The first version opened the library on the game's thread too, and that frame took **4.4 s**; with the library opened by the watcher, the switch costs one frame of **190 ms**.
- Import stubs the interpreter bound are replaced by the corpus's wrappers, which call the same HLE.
- Libraries are never unloaded. Swapping -O0 for -O2 while the game runs is the same operation; the replaced library's code is simply never called again.
- The window shows a line over the game while a compile runs ("Compiling the game: 45% - playing under the interpreter (slow) until it is done") and "Switched to compiled code" for five seconds after, through the framework's new `ui::set_status_overlay()`.
- Restarting is the fallback if a game ever misbehaves across the switch: `portablekit run` loads the best ready corpus at start.

## Growing the corpus

Static analysis finds what the executable shows; code a game builds or copies at run time only appears when it runs. The loop:

1. Recompile what is known and compile it.
2. Play. Whatever reaches the interpreter is counted: `psprecomp::interpreter_entry_profile()` gives entry addresses and instructions run. The app writes it to `<data>/cache/<game id>/interpreted.txt` when the game stops.
3. The next compile passes those addresses to the recompiler as extra seeds, and the new code is compiled and switched to like any other corpus.

Step 3 needs the recompiler to take code its analysis did not find. `psp_recomp --code START-END` (from #29, taken into this branch) does that for ranges the section table does not call code; the loop would pass the interpreted entries' ranges to it. Wiring `interpreted.txt` to `--code` is the next step and is not in the prototype. For code at addresses whose contents change (overlays), seeds are not enough: the corpus must be keyed by the bytes, which is exactly what the framework's overlay libraries do. An automatic version of that — detect a slot by `sceKernelIcacheInvalidate*` over a range the interpreter then runs, dump it, recompile it as an overlay library — is the path to Yakumo-like games without a hand profile.

For LocoRoco 2 the question does not arise: with the compiled corpus loaded, the interpreter was never entered (no interpreter line in any compiled run's log), as Purun's README reports for its own build. From an interpreter-only run the same file is the other thing the design wants, the hot set: from the title into level 2, 78 entry addresses account for 90% of the instructions interpreted, and they lie in **14 of the 193 units**. Those are what "hot units at -O2 first" would compile first: a few minutes instead of 36 for most of -O2's benefit, if the hot set holds across a game (not measured past level 2).

## The interface

The window reuses the ports' ImGui layer, widgets and file browser, and works with a gamepad alone (Steam Deck Game Mode).

First run:

1. **Library** — empty: "Add a game..." and "Keys: None: saves stay unencrypted". The compiler found (or what to install), and where data and compiled games are kept.
2. **Choose a disc image** — the ports' file browser; dropping the `.iso` on the window does the same.
3. **Adding the game** — the disc is read and the executable taken, in a second or two. If the executable is encrypted and no keys are there: the message above, with "Choose a keys file..." and "Choose an executable I decrypted...".
4. **The game's page** — Play; Play under the interpreter only; state ("Not compiled yet: plays slowly" / "Compiling 45%" / "Compiled (-O2)"); profile (automatic or the hand-written one); where the executable came from; the disc image; the space compiled code takes, and "Remove compiled code".
5. **Play** — the game starts in its own process. With nothing compiled, a compile starts in the background at once and the game runs under the interpreter with the progress line over it, and switches when the compile is done. Esc opens the port's own menu, as today.

The prototype's window, captured with `PORTABLEKIT_INPUT_SCRIPT` (kept locally, not committed: the game's pictures are the game's): the library with one game "Compiled (-O2)", keys "None: saves stay unencrypted", the compiler found and the two folders; the game's page with Play, the state, "Profile: automatic", "Executable: BOOT.BIN" and the space its compiled code takes; the file browser for adding a game; and, over the running game, "Compiling the game: 52% - playing under the interpreter (slow) until it is done".

Errors are sentences for the player (see [Keys](#keys)); a failed compile keeps its whole log in `<data>/cache/.../build.log`, and the game keeps playing under the interpreter.

## The command line

The same functions as the window, for scripts, headless machines and agents. Every command takes `--json` and then prints exactly one JSON object on stdout.

```
portablekit                          the library window
portablekit add <image.iso> [--executable <EBOOT>] [--boot-bin]
portablekit list
portablekit info <game>              what was found, the profile's decisions, compiled state
portablekit status <game>            compile progress and cache, per level
portablekit compile <game> [--opt 0|1|2|tiered] [--jobs N] [--background] [--keep]
portablekit run <game> [--interpreter] [--no-compile] [--opt 0|1|2|tiered] [--headless] [--seconds N]
portablekit export <game> <folder> [--source-only | --library-only]
portablekit keys import <file>
portablekit keys status
portablekit cache clear <game>
portablekit toolchain
portablekit version                  also --version: the build, its label, the corpus ABI, HLE extension modules
```

`<game>` is the id (`UCES01059-e1075b96`), the disc id with or without its dash, or a unique prefix. `run` starts a background compile when -O2 is not ready yet, `tiered` by default: -O0 first, then -O2 from the same C++ (the recompiler runs once). `--seconds` bounds a run for tests; `PORTABLEKIT_MAX_OPT` caps the level loaded, for comparing levels. `--data-dir <folder>` works with every command.

`export` writes the game's recompiled code to a folder without running the game: the C++ the recompiler makes (with the two headers it includes, so the folder compiles on its own) and the best compiled library, beside a `NOTICE.txt`. The window has the same action on a game's page ("Export recompiled code..."). The command, the window and the notice all say the same thing: **this output is the player's game code in another form, made from their own copy, for their own use only; it must not be shared, uploaded or attached to bug reports.**

| Exit code | Meaning |
| --- | --- |
| 0 | done |
| 1 | an unexpected error |
| 2 | the command line is wrong |
| 3 | no such game in the library |
| 10 | not a PSP game disc image |
| 11 | the executable is encrypted and the keys are missing |
| 12 | the executable is not one this program can load |
| 13 | a file could not be read or written |
| 20 | no compiler was found |
| 21 / 22 / 23 | the recompiler / a unit / the link failed |
| 24 | a compile of this game is already running |
| 25 | the export failed |
| 30 | the keys file was not accepted |
| 40 + n | the game stopped with the port's own exit code n |

Example (`--json` output, abridged):

```
$ portablekit add ~/Downloads/LocoRoco2.iso --json
{"id": "UCES01059-e1075b96", "disc_id": "UCES01059", "title": "LocoRoco™ 2", ..., "executable_source": "BOOT.BIN", "profile": "auto", "compiled_opt_level": null, "ok": true}
$ portablekit status UCES01059 --json
{"id": "UCES01059-e1075b96", "corpora": [{"opt_level": 0, "state": "ready", "units_done": 195, "units_total": 195, "jobs": 2, "generate_seconds": 178.1, "compile_seconds": 173, ...}], "cache_bytes": ..., "ready_opt_level": 0, "ok": true}
```

## What changed in the framework

Each is a self-contained commit, so other branches can take them:

| Change | Files |
| --- | --- |
| Every key behind `crypto_keys()`; ports link `crypto_keys_builtin.cpp` with the same values | `host/crypto_keys.hpp`, `host/crypto_keys_builtin.cpp`, `host/install/executable_preparation.*`, `host/save_data/savedata_crypto.cpp` |
| No keys: saves written unencrypted, encrypted saves and imports refused with a sentence; with keys, such saves encrypted on export | `host/hle/hle_savedata.cpp`, `host/save_data/*`, `host/ui/save_screen.cpp` |
| `decrypt_executable()`: any tag's key material, no hash check; `executable_tag()` | `host/install/executable_preparation.*` |
| `portablekit_add_game(... EXTERNAL_KEYS NO_CORPUS HOST_MAIN_NAME <name>)`, absolute `SOURCES` | `cmake/PortableKit.cmake` |
| `main()` under another name | `host/main.cpp` |
| `set_code_miss_hook()`, `ui::set_status_overlay()` | `host/overlays.*`, `host/ui/*` |
| **The corpus ABI** | `include/psprecomp/corpus_abi.hpp`, `runtime.*`, `guest_memory.*`, `tools/codegen_main.cpp`, `host/kernel/kernel.cpp`, `host/overlay_module.*`, `tests/test_main.cpp` |
| `<algorithm>` in `allegrex_context.hpp` | `include/psprecomp/allegrex_context.hpp` |
| **Editions** (`ProfileVariant`), edition corpora as libraries, the corpus library loader | `host/profile.*`, `host/corpus_library.*`, `host/corpus_module.cpp.in`, `host/main.cpp`, `host/system.cpp`, `host/overlays.cpp`, `host/install/*`, `cmake/PortableKit.cmake`, `tests/profile_tests.cpp` |
| MinGW build of the ad hoc discovery | `host/adhoc/discovery.cpp` |
| SIGNAL's list flow (jump, call, return, sync), `<prefix>_TRACE_GE_LIST` | `host/gpu/ge_state.cpp` |
| ATRAC streaming through `sceAtracSetData` | `host/hle/hle_atrac.cpp` |

The PortableKit stack #23, #24, #25 and #27 (`purun-first-levels`) is merged into this branch, and b24fc69 (only the vertices an indexed draw uses) cherry-picked; neither changes the corpus ABI, so compiled caches stay valid.

Taken from other branches (cherry-picked with `-x`, or by hand where it would not apply): from #29 (`fubuki-prep`) the recompiler fix for imports called from the same unit, VCRS.T, `psp_recomp --code`, the remaining user-mode Allegrex instructions and the test fix they need, SDL3.dll beside the executable on Windows, the shader step's Python, SYNC as a plain fence, `sceMpegAvcDecode`, and SAS noise voices and envelopes (with its prerequisite from #27). Worth taking when they land: #28's release packaging for macOS and Linux (the app needs an `.app` bundle with the recompiler and headers inside), and #23's commit that finds overlays and fonts inside a macOS bundle.

## What the prototype does

Built with `cmake -S apps/portablekit -B out-app -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out-app -j2` on the Mac (warning-free), and exercised there with LocoRoco 2:

- `portablekit add <iso>` with **no keys**: identifies `UCES01059`, takes `BOOT.BIN` (SHA-256 `e1075b96…`, the same executable Purun's profile names), writes the game's directory. 0.07 s.
- `portablekit keys import` of a test keys file made from the framework's own published values (kept outside the repository and deleted afterwards), then `add` in a separate home: `EBOOT.BIN` decrypted with `tag.C0CB167C` from the file, to the same SHA-256. A key changed by one digit is refused by name ("savedata.4 is not the right key: its fingerprint does not match", exit 30). A save written by Purun's build (encrypted, mode 5) loads in that home and the game continues from it.
- `portablekit run` with the automatic profile under the interpreter: boots, loads the disc's HLE modules, the intro, the language screen and the title sequence at full speed.
- `portablekit compile` at -O0 and -O2 into the cache, and `run` loading them: 789 044 addresses registered in 0.16 s, the game runs from compiled code.
- Hot swap: a fresh cache, `portablekit run` started the -O0 compile beside the game; the game played through the title into level 2 under the interpreter with the progress line over it, and switched to the -O0 corpus in the level after 6 minutes, without a restart (see [Switching to compiled code](#switching-to-compiled-code)). The compile, left running when the bounded game run ended, finished on its own and was picked up at the next start.
- Hand profile: Purun's profile compiled in as an extra, matched by hash; same behaviour and speed as the automatic profile ([above](#hand-written-profiles-as-extras)).
- The library window: the library, a game's page and the file browser, driven by a script; the game's page starts `portablekit run` in a new process.
- Rebuilding the program with a recompiler fix taken from #29 changed the corpus ABI (`f68962b4…` → `826f0774…`): the -O0 and -O2 libraries compiled before were no longer offered ("not compiled"), as intended, and the next compile removes them.
- Windows, on the maintainer's PC (Ryzen 5 5600, 16 GB, RX 5600 XT, Windows 11): the app built with llvm-mingw, delivered as a portable folder, and checked with Monster Hunter Portable 2nd G as described under [Windows](#windows-ship-an-llvm-toolchain-done). Test data was removed from the delivered folder.
- Portable data: with no `--data-dir`, the Mac build made `data/` beside itself; `export --library-only` wrote the library and `NOTICE.txt`.

### More games, on Windows, with the automatic profile

The maintainer's own discs, each in a data folder of its own beside the delivered build, run bounded. What each needed was framework, never a profile:

| Game | Reached | What it needed |
| --- | --- | --- |
| Grand Theft Auto: Chinatown Wars (`ULUS-10490`, EBOOT decrypted with the player's keys) | the Rockstar logo, the story intro with subtitles, the 3D airport scene at full speed, under the interpreter (guest ~21 ms a frame) and from -O0 (~12 ms) | fixed-size memory pools (`sceKernel*Fpl`), asynchronous file I/O (`sceIo*Async`), the scratchpad (taken from `tenkawa-bringup`), `sceKernelVolatileMemTryLock`, `sceKernelTryLockMutex`, a 64 MiB stack for the MinGW build (compiled code overflowed the default at once) |
| Grand Theft Auto: Vice City Stories (`ULES-00502`) | the Rockstar logo movie and the next one, then waits for its world streaming (`WorldStreamEventFlag` bits 1 and 2 are never set) | directory reads (`sceIoDread`; the disc must not list "." and ".."), `sceUmdCheckMedium`, callbacks in `*CB` waits, thread suspend and resume. The stream wait is the open blocker |
| Monster Hunter Portable 2nd G (`ULJM-05500`) | the game menu | (the Fubuki port's fixes) |
| Patapon (`UCES-00995`) | the warning that system data will not be saved, then (after "Yes") the developer logo and the title screen, at full speed under the interpreter. The title does not react to Start, Cross, Circle or Square yet | the notify callback of an asynchronous open that completed before the callback was set, the system language and confirm button from the disc's region, `sceIoIoctl` first sector, size and seek, SIGNAL's own list flow (behaviour 0x11: its frame list jumps into a list inside `DATA_CMN.BND`; without it, a black screen with one fade sprite), and ATRAC tracks streamed through `sceAtracSetData` with a buffer smaller than the file |

Known in Chinatown Wars: grey untextured areas in 3D, unchanged by the vertex-space fixes from #24 and b24fc69 (the same frame of the same run looks the same before and after). Control past the intro was not tried.

Known in Patapon: the title screen waits for something not yet found; the pad is read every frame (`sceCtrlReadBufferPositive`) and every thread but the main one is idle in an ordinary wait. Its looping menu music goes silent after the first pass, because a streamed track that loops is not refilled from the loop start yet (`sceAtracGetStreamDataInfo` reports no room once the whole file was added).

### What was not verified

- **Windows**: the app was built and smoke-tested there, but not played: no gameplay past the menu, no -O2 compile of a whole game in the app, no hot switch while a game runs, no keys.
- **A `.app` bundle and App Translocation**: the rule is implemented, but no `.app` was built to try it.
- **Linux and the Steam Deck**: designed only. The code has Linux paths (`posix_spawn`, `dlopen`, `c++` on `PATH`) that were not compiled.
- **macOS without Xcode**: compiled with Xcode's toolchain through `xcrun`; a machine with only the Command Line Tools was not tried, nor the install prompt, nor a signed and notarized build loading an unsigned corpus.
- **Gameplay** in the app went as far as the start of level 2, reached by continuing from a save made by Purun's build; nothing was played by hand, and nothing past that point was tried.
- **Unknown games**: LocoRoco 2 on the Mac and Monster Hunter Portable 2nd G on Windows were added with the automatic profile; no other game.
- **The encrypted-executable message without keys** (exit 11) was checked by reading the code only: this disc always has `BOOT.BIN` to fall back to.
- Import and export of saves in the no-keys mode were not exercised through the menu.
- The framework's own build (`cmake -S . -B out`) builds warning-free apart from an existing warning in `common.hpp`, `portablekit_host_check` links, and all 7 `ctest` suites pass on the Mac with this branch. No real port (Purun, Tenkawa, Yakumo) was rebuilt against this branch, only a throwaway one for editions; with the corpus ABI each needs its corpus regenerated. The ad hoc discovery test fails on this Mac only while a VPN interface is up.

## Decisions

Taken by the maintainer on the first version of this document, and done:

1. **Keys**: Yakumo's releases keep their keys; the app ships none.
2. **Two-stage compile** (-O0, then -O2) is the default.
3. **Windows: option A**, llvm-mingw for the program and the shipped toolchain.
4. **The thin corpus ABI**, with the one-time recompile of the ports, and the `<algorithm>` fix.
5. **The app stays in `apps/`** of this repository: it needs the framework's sources at the same commit to compute its ABI, and it is the framework's first consumer that runs any game.
6. **Hand profiles** stay compiled in (`PORTABLEKIT_APP_PROFILES`) and gain editions (`ProfileVariant`), shared with the Fubuki port.
7. **Saves** are encrypted on export once keys exist; **`export`** gives the recompiled code without running the game, marked as the player's own.
8. **Window and game in separate processes.**
9. **The compatibility report** is the app's to design (below).
10. **Portable**: everything in one folder beside the program.

What a compatibility report would hold, written only when the player asks and never sent by the program: the program's version and corpus ABI; the game's disc id, title and executable hash (no bytes of it); the profile and its decisions (`portablekit info`); the imports nothing implements (`LIST_STUBS`); the interpreter's entry addresses and counts (`interpreted.txt`); the last run's log with the player's user name removed from paths. Addresses and counts describe the game but contain none of it.

Still open:

- A signed and notarized macOS `.app` (the `disable-library-validation` entitlement, or ad-hoc signing each corpus), and a Linux portable build.
- Growing the corpus from the interpreter's profile: feeding `interpreted.txt` to `psp_recomp --code`.
- Whether the framework's overlay tooling should be used by the app for games like Monster Hunter, whose overlays now stay interpreted.
