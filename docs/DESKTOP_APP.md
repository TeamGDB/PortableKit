# PortableKit as a desktop program

A design, and a prototype of it in [`apps/portablekit`](../apps/portablekit): one program a player installs, points at the disc image of a PSP game they own, and plays. It recompiles the game on the player's own machine, and until that is done it plays the game under the interpreter.

The game throughout is LocoRoco 2 (`UCES-01059`). The prototype was built and run on a MacBook Air M1 with 8 GB of memory, macOS 27 and Apple clang 21; the corpus half of the Windows design (compiling, linking and loading the game's code with a shipped toolchain) was measured on the maintainer's Windows 11 PC. The app itself was not built on Windows, and Linux is designed only; see [What was not verified](#what-was-not-verified).

**Contents:** [The product](#the-product) · [What the program ships, what the player brings](#what-the-program-ships-what-the-player-brings) · [Architecture](#architecture) · [Any game: the automatic profile](#any-game-the-automatic-profile) · [Keys](#keys) · [Saves](#saves) · [Compiling on the player's machine](#compiling-on-the-players-machine) · [Switching to compiled code](#switching-to-compiled-code) · [Growing the corpus](#growing-the-corpus) · [The interface](#the-interface) · [The command line](#the-command-line) · [What changed in the framework](#what-changed-in-the-framework) · [What the prototype does](#what-the-prototype-does) · [Decisions for the maintainer](#decisions-for-the-maintainer)

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
| Generated C++, compiled game code | **no**: made on the player's machine from the player's disc, kept in the player's cache, never uploaded | |

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

- A **game** is a directory `<home>/games/<disc id>-<first 8 hex of the executable's SHA-256>/` holding what the port's data directory always held (`EBOOT.ELF`, `settings.ini` naming the disc image in place, `ms0/`), plus `game.txt`, what the app found out.
- **Running** a game means: make `game()` describe it, point `PORTABLEKIT_DATA_DIR` at its directory, and call the framework's own `main()` under the name `portablekit_host_main`. From there it is exactly a port.
- The framework asks `psprecomp::register_generated_functions()` for the game's code, as it always did. A port links its generated registry there; the app defines it itself, and loads the player's compiled library ([Switching to compiled code](#switching-to-compiled-code)).
- The window runs in one process and each game in another (`portablekit run <game>`), so a game's settings, its window and a crash stay its own.

| Where | macOS | Windows | Linux |
| --- | --- | --- | --- |
| `<home>`: games, saves, keys | `~/Library/Application Support/PortableKit` | `%LOCALAPPDATA%\PortableKit` | `$XDG_DATA_HOME/PortableKit` (`~/.local/share/PortableKit`) |
| `<cache>`: compiled games | `~/Library/Caches/PortableKit` | `%LOCALAPPDATA%\PortableKit\cache` | `$XDG_CACHE_HOME/PortableKit` (`~/.cache/PortableKit`) |

`PORTABLEKIT_HOME` and `PORTABLEKIT_CACHE` move them (tests, portable installs). The cache can be deleted at any time; it costs only the time to compile again.

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
- **Yakumo**: when it moves onto the framework ([#2](https://github.com/TeamGDB/PortableKit/issues/2)), its two copies go and it gets the seam for free. Until then its releases carry keys, as they do today.

### The keys file

Text, one key per line, hex, `#` for comments; `<home>/keys.txt`, or `PORTABLEKIT_KEYS`:

```
kirk.aes.5D  = <16 bytes>      # KIRK command 4/7 key slots, by slot number in hex
kirk.cmd1    = <16 bytes>      # the KIRK command 1 AES key
savedata.2   = <16 bytes>      # save-data keys 2 to 7
tag.C0CB167C = <16 bytes, or the 0x90-byte table of an old header layout>
tag.C0CB167C.slot = 5D         # optional: the KIRK slot the tag's header uses
```

- `portablekit keys import <file>` checks the file and copies it into `<home>`. Every fixed key is checked against a SHA-256 fingerprint compiled into the program, so the player is told "`savedata.4` is not the right key" instead of getting garbage later. Tag keys have no fingerprint: an executable that decrypts to an ELF the runtime can load is the check.
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
- **Keys added later**: saves written without keys stay readable (flags 0 needs no key). Encrypting them for a PSP at export time is the missing piece: export would re-encrypt with the remembered game key. Not in the prototype.
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

### Windows: ship an LLVM toolchain

The program is built with some compiler, and the corpus must match its C++ ABI: the generated code calls `Runtime` members, passes `std::string`, and inlines `GuestMemory` and `AllegrexContext` from the runtime headers. Three ways:

| | A. Build the app with llvm-mingw, ship the same toolchain | B. Build with MSVC, require MSVC Build Tools | C. Build with MSVC, ship clang-cl + lld-link |
| --- | --- | --- | --- |
| Player installs | nothing | Visual Studio Build Tools, ~2–7 GB, with Microsoft's licence to accept | nothing, but the MSVC headers and libraries are still needed |
| Redistributable | yes: LLVM (Apache-2.0 with LLVM exception), mingw-w64 headers and CRT (public domain, ZPL and permissive), libc++ | nothing to ship | clang-cl and lld-link yes; the MSVC STL is Apache-2.0 with LLVM exception, but the UCRT and Windows SDK headers and import libraries are **not** redistributable |
| Size shipped | x86_64-only subset of llvm-mingw 20260922: **334 MB unpacked, 83 MB zip, 54 MB tar.xz** (measured: clang, lld, libLLVM, libclang-cpp, headers, x86_64 sysroot) | 0 | ~150 MB plus a downloader for the SDK parts (the way `xwin` fetches them from Microsoft) |
| Risk | the framework has only been built with MSVC on Windows; building it with mingw is untried (FFmpeg, SDL3 and Vulkan are C and fine) | a 2–7 GB prerequisite for a game | licensing of the downloaded parts; two compilers' ABIs to keep in step |

**Recommendation: A**, and in the longer run the thin corpus ABI below, which makes the choice of compiler for the corpus independent of the program's. The prototype's code already looks for `toolchain\bin\clang++.exe` next to `portablekit.exe`, compiles with it, and links against the program's import library (`libportablekit.dll.a`).

What was tried on the maintainer's PC (Windows 11, the trimmed toolchain unpacked in `C:\Dev\llvm-mingw`, nothing else installed): the whole LocoRoco 2 corpus compiles at -O0 and -O2 ([numbers](#the-numbers)); the runtime's own sources (`src/*.cpp`) compile; a stand-in program built from them with `-Wl,--export-all-symbols -Wl,--out-implib` exports the runtime; the -O0 corpus links into a DLL against that import library in 1.2 s; the program loads it with `LoadLibrary`, the ABI string matches, and it registers 789 044 addresses in 141 ms. That is the app's whole link model, minus the app.

One portability bug found on the way: `include/psprecomp/allegrex_context.hpp` uses `std::max` without including `<algorithm>`. Apple's libc++ and MSVC include it through other headers; libc++ 23 in llvm-mingw does not, and every generated unit fails to compile. The Windows measurements used `-include algorithm`. The fix is one line, but it is in `include/psprecomp/`, so it changes the corpus ABI and rebuilds every port's generated code; it is left out of this branch for that reason and should go in with the next change that touches that directory anyway.

### Linux (x86_64, Steam Deck)

- **The system compiler** (`c++`, `g++`, `clang++` on `PATH`) works on a desktop distribution and is what the prototype looks for. The corpus is linked `-shared` and resolves the runtime against the executable, which already exports its symbols (`ENABLE_EXPORTS`).
- **But the Steam Deck has none**: SteamOS's root is read-only and ships no compiler. So a Linux release needs a bundled toolchain too: an LLVM release for x86_64 (clang + lld, about 100–150 MB compressed) and a sysroot with the glibc headers of the release's glibc floor (the same floor the program is built against, e.g. glibc 2.31), and libstdc++'s headers matching the program's statically linked libstdc++. Linking the corpus against the running program only, with `-nostdlib`, keeps it off the system's libstdc++ entirely.
- **AppImage**: the program runs from a read-only mount; the toolchain can sit inside it, the cache goes to `$XDG_CACHE_HOME`, and `dlopen` from there works.
- **Flatpak**: the sandbox allows writing and `dlopen`ing in `~/.var/app/<id>/cache`. The Freedesktop SDK runtime has a compiler, but the Platform runtime the app runs in does not; either bundle the toolchain in the app, or depend on the SDK extension `org.freedesktop.Sdk.Extension.llvm*` and add it to the app's `PATH`.
- The release build links libstdc++ statically with hidden symbols (see `PORTABLEKIT_RELEASE` in `cmake/PortableKit.cmake`). The corpus then must not bring its own: another reason for the thin corpus ABI.

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

- The library is `<cache>/<game id>/<abi>-O<level>/corpus.<dylib|so|dll>`. `<abi>` is a SHA-256 over every runtime header the generated code includes, every source of the recompiler and the runtime, and the compile definitions ([`cmake/corpus_abi.cmake`](../apps/portablekit/cmake/corpus_abi.cmake)); the prototype's is `5e9ff759e9e4a7db…` (it was `f68962b4…` before the commits taken from #29, and every such change invalidated the caches as intended).
- The library exports `portablekit_corpus_abi()`, `portablekit_corpus_executable()` (the executable's SHA-256) and `portablekit_corpus_register(Runtime &)`; the program refuses a library whose answers differ from its own. Its own `register_generated_functions` is renamed at compile time so it cannot collide with the program's loader.
- So a program update that touches none of those files keeps every cache; one that does compiles again, as it must. Today that is almost every update, because `runtime.hpp` changes often.
- **Thin corpus ABI (proposed)**: generated code today includes `runtime.hpp`, which pulls in `std::string`, `std::vector` and the whole `Runtime` class. It needs far less: 11 runtime functions and 2 globals (listed from an object file of this corpus: `invoke_chained_call`, `invoke_chained_unit`, `register_function`, `register_generated_unit`, `run_starvation_boundary`, `unsupported`, five `GuestMemory::aot_*_slow`, the two chain globals) plus the layouts of `AllegrexContext` and `GuestMemory::AotFastView`. A C header with those layouts and a table of function pointers the program passes in would make the cache survive every runtime change that does not touch them, let any C++ compiler build the corpus for a program built by any other (MSVC program, clang corpus), and remove the C++ standard library from the corpus's link. It is the change that most improves this design; it touches the recompiler's output and `include/psprecomp/`, so it is its own piece of work.

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
2. Play. Whatever reaches the interpreter is counted: `psprecomp::interpreter_entry_profile()` gives entry addresses and instructions run. The app writes it to `<cache>/<game id>/interpreted.txt` when the game stops.
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

Errors are sentences for the player (see [Keys](#keys)); a failed compile keeps its whole log in `<cache>/.../build.log`, and the game keeps playing under the interpreter.

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
portablekit keys import <file>
portablekit keys status
portablekit cache clear <game>
portablekit toolchain
```

`<game>` is the id (`UCES01059-e1075b96`), the disc id with or without its dash, or a unique prefix. `run` starts a background compile when -O2 is not ready yet, `tiered` by default: -O0 first, then -O2 from the same C++ (the recompiler runs once). `--seconds` bounds a run for tests; `PORTABLEKIT_MAX_OPT` caps the level loaded, for comparing levels.

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

Small seams, each off unless a program asks for it; ports build and behave as before:

| Change | Files |
| --- | --- |
| Every key behind `crypto_keys()`; ports link `crypto_keys_builtin.cpp` with the same values | `host/crypto_keys.hpp`, `host/crypto_keys_builtin.cpp`, `host/install/executable_preparation.*`, `host/save_data/savedata_crypto.cpp` |
| No keys: saves written unencrypted, encrypted saves and imports refused with a sentence | `host/hle/hle_savedata.cpp`, `host/save_data/savedata_store.cpp`, `host/save_data/save_transfer.cpp` |
| `decrypt_executable()`: any tag's key material, no hash check; `executable_tag()` | `host/install/executable_preparation.*` |
| `portablekit_add_game(... EXTERNAL_KEYS NO_CORPUS HOST_MAIN_NAME <name>)`, absolute `SOURCES` | `cmake/PortableKit.cmake` |
| `main()` under another name | `host/main.cpp` |
| `set_code_miss_hook()`: asked before the overlay corpora (for a program that loads code another way) | `host/overlays.*` |
| `ui::set_status_overlay()`: a line of the program's own over the game | `host/ui/ui.hpp`, `host/ui/menu.cpp` |

The seams change nothing under `include/psprecomp/` or `src/`; the commits taken from #29 below do.

Taken from other branches (cherry-picked, with `-x`), all from #29 (`fubuki-prep`), because a program for any game needs them more than any port does: "Call an import from the same unit through the dispatcher, not its label" (a recompiler fix), "Decode, interpret and recompile VCRS.T", "Let psp_recomp take code the section table does not call code" (`--code`), "Implement the Allegrex user-mode instructions no game has run yet", the test fix they depend on, and "Put SDL3.dll next to the executable on Windows". These do change `include/psprecomp/` and `src/`, so ports rebuild their generated code once when this branch lands, and the app's corpus ABI changed with them (to `5e9ff759…`); LocoRoco 2 was compiled again at -O0 with them and runs. Its shader-step change did not apply without the rest of #27 and was left. Worth taking when they land: #28's release packaging for macOS and Linux (the app needs an `.app` bundle with the recompiler and headers inside), and #23's commit that finds overlays and fonts inside a macOS bundle.

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
- Windows, on the maintainer's PC (Ryzen 5 5600, 16 GB, Windows 11) with the trimmed llvm-mingw in `C:\Dev`: the corpus compiled, linked and loaded as described under [Windows](#windows-ship-an-llvm-toolchain). The test files (the generated C++ and objects) were deleted from that machine afterwards; the toolchain stays in `C:\Dev\llvm-mingw`.

### What was not verified

- **Windows**: the app itself was not built or run there. What was run on the maintainer's PC is the corpus half: the trimmed llvm-mingw compiling this game's generated units, and the link model (a stand-in program exporting the runtime, a corpus DLL linked against its import library and loaded). Whether the framework and the app build with llvm-mingw is the open question of option A.
- **Linux and the Steam Deck**: designed only. The code has Linux paths (`posix_spawn`, `dlopen`, XDG directories, `c++` on `PATH`) that were not compiled.
- **macOS without Xcode**: compiled with Xcode's toolchain through `xcrun`; a machine with only the Command Line Tools was not tried, nor the install prompt, nor a signed and notarized build loading an unsigned corpus.
- **Gameplay** in the app went as far as the start of level 2, reached by continuing from a save made by Purun's build; nothing was played by hand, and nothing past that point was tried.
- **Unknown games**: only LocoRoco 2 was added. The automatic profile's defaults are reasoned, not tested on a second game; Tenkawa's disc was not at hand.
- **The encrypted-executable message without keys** (exit 11) was checked by reading the code only: this disc always has `BOOT.BIN` to fall back to.
- Import and export of saves in the no-keys mode were not exercised through the menu.
- The framework's own build (`cmake -S . -B out`) builds warning-free apart from an existing warning in `common.hpp`, `portablekit_host_check` links, and all 7 `ctest` suites pass on the Mac with this branch. No port (Purun, Tenkawa, Yakumo) was rebuilt against it; the seams are meant to change nothing for them, and `crypto_keys_builtin.cpp` carries the same values as before.

## Decisions for the maintainer

1. **Keys out of the releases.** The seam is here; do the ports keep linking `crypto_keys_builtin.cpp`, or do they move to the keys file too (and Yakumo with its move onto the framework)? The app cannot ship keys either way.
2. **Default optimisation plan.** -O0 first and -O2 after (two switches, fastest to playable), or straight to -O2 (one compile, a longer wait under the interpreter), or hot units at -O2 only. The numbers above favour tiers (what the prototype does by default): playable compiled code in 6 minutes instead of 42, for 8% more compile work (measured tiered: -O0 ready at 6 min, -O2 at 38).
3. **Windows toolchain: A, B or C** (see the table). A means building the program itself with llvm-mingw, which nobody has tried with this framework; it needs a Windows machine for a day. B is zero work and a 2–7 GB prerequisite.
4. **The thin corpus ABI.** It is the change that makes caches survive updates and frees the corpus from the program's compiler. It touches `include/psprecomp/` and the recompiler, so every port rebuilds once. Worth scheduling before a first release of the app?
5. **Where the app lives.** In this repository under `apps/` (as now), or its own repository like a port. It needs the framework's source at the same commit to compute the ABI, which argues for here.
6. **Hand profiles in the app.** Compiled in (as now, via `PORTABLEKIT_APP_PROFILES`), or loaded as plug-in libraries, or data-only profiles (a text file per game for the fields that are data: overlay slots, save labels, trigger layouts), with hooks staying compiled.
7. **Encrypting saves at export** when keys are added after playing without them: wanted?
8. **One process or two.** The prototype runs the window and the game in separate processes and re-executes itself to start a game. Fine for macOS and Linux; on Windows it spawns and exits. Keep it?
9. **Telemetry-free compatibility reports.** A button that writes a report (profile decisions, missing imports, interpreted addresses, the log) for the player to attach to an issue, never sent by the program. Agree on what it may contain.
