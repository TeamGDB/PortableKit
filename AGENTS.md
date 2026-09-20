# AGENTS.md

Working notes for anyone changing this repository: people and coding agents alike. Read this first. It is short on purpose and links to the longer documents instead of repeating them.

PortableKit turns a PlayStation Portable game into a native application. It recompiles the game's PSP (MIPS) code to C++ ahead of time and supplies the PSP system around it: the kernel, HLE modules, a Vulkan GE renderer, audio, input, save data, ad hoc networking, an ImGui interface and a first-run installer. The recompiler and the core runtime are in `src/` and `include/psprecomp/`; the system layer is in `host/`.

**A game is not in this repository.** Each port is its own repository with its own profile: [Yakumo](https://github.com/TeamGDB/Yakumo) for *Monster Hunter Portable 3rd HD Ver.*, [Tenkawa](https://github.com/TeamGDB/Tenkawa) for *Dragon Ball Z: Tenkaichi Tag Team*. A profile is a file of constants and a four-line `CMakeLists.txt`; if it ever needs to be more, that is a gap in the framework, not in the profile.

## Rules

- **English only** in everything committed: code, comments, docs, commit messages, pull requests.
- **Branch and pull request.** Never push to `main`.
- **No game data, ever.** Disc images, executables, generated code, overlay corpora and saves stay local. The same goes for anything personal: home paths, user names, machine names, addresses.
- **Nothing about one game in `host/`, `src/` or `include/`.** A game's disc id, its addresses, its save folders, its quirks go in that game's profile. If the framework needs to know something, it is a field of `portablekit::GameProfile` in [`host/profile.hpp`](../host/profile.hpp) or a hook on it, and the framework asks.
- **Write it yourself.** Read public documentation and other projects to understand the PSP, file formats and protocols. Never copy, paste or line-by-line translate code from a project whose licence is incompatible with this repository's MIT licence. Constants, offsets and format facts are fine. Record where intentionally included third-party code comes from; see [SOURCE_PROVENANCE.md](docs/SOURCE_PROVENANCE.md).
- **Say what you did not verify.** A pull request lists what was tested, on which platform and with which game, and what was not.

## The framework's own build

```bash
cmake -S . -B out -G Ninja
cmake --build out -j2
ctest --test-dir out
```

That builds the recompiler (`psp_analyze`, `psp_recomp`, `dump_function`), the runtime, and the tests that need no game: the codegen tests, the save-data crypto and round trips, and the ad hoc client, server and discovery over loopback. `tests/test_profile.cpp` is a stand-in game, so host code that asks the profile for names can be tested without one.

**This build does not compile `host/main.cpp` or most of `host/`.** Those belong to a game's target. A change to the system layer is not built until a port builds it, so build at least one port before calling a change done.

## Building a port

A port's repository adds this one as a subdirectory and calls `portablekit_add_game()`. [docs/BUILDING.md](docs/BUILDING.md) covers the whole build and where the time goes; the short version:

- **Keep parallelism low** (`-j2`): each generated unit needs well over a gigabyte to compile. The build caps them at one per 4 GiB of memory whatever `-j` says.
- **Build with `cmake --build`, never `ninja` directly**: `cmake --build` holds the per-directory lock. One build per build directory. Never delete `.ninja_deps` or `.ninja_log`.
- **Install `ccache`**; the build uses it automatically, across checkouts.
- **A full recompile takes hours.** Do not wait for it before finding out what a new game needs.

## Bringing up a new game

[docs/BRINGING_UP_A_GAME.md](docs/BRINGING_UP_A_GAME.md) is the guide, and the order it gives matters. In short: build the framework and a profile of constants, prepare the executable, and **run the game under the interpreter before recompiling anything**. The interpreter uses the same memory, kernel, HLE and renderer as recompiled code, so the first run already tells you which system calls are missing, in the order the game needs them. Waiting for the recompile first costs a day and tells you nothing extra.

The interpreter is roughly twenty times slower than recompiled code. It is a development tool, not how anyone plays; `PSPRECOMP_NO_INTERPRETER=1` turns it off, which is how a finished port is checked for never falling back to it.

## Running and testing

- **Bound every run.** `timeout 60 out/bin/<Game>Native`. Never leave a game running, and never drive it with an open-ended input loop: it does not converge, and someone may be watching the screen.
- **Quick boot checks.** `<PREFIX>_NO_RENDER=1 <PREFIX>_NO_AUDIO=1 timeout 40 …`, then look at the function count, the import counts and `[overlay] installed`.
- **Scripted input and captures.** `<PREFIX>_INPUT_SCRIPT` sends keys, virtual gamepad input and dropped files, and captures the window; the syntax is in `host/ui/input_script.hpp`. `<PREFIX>_SCREENSHOT_DIR` captures the game's own frames. Look at the captures; don't assume.
- **Several instances.** For multiplayer or before/after comparisons, give each instance its own `<PREFIX>_DATA_DIR`, its own saves and a `<PREFIX>_WINDOW_TITLE`.
- **Numbers.** `<PREFIX>_PERF=log` prints one line per second: fps, the game's own frame rate, emulation speed, and guest/render/wait time.
- **Tracing.** `<PREFIX>_TRACE_*` logs one subsystem each: GE, material and lighting registers, save data, fonts, pad, audio, ATRAC, MPEG, ad hoc, I/O, kernel.

Every one of those variables takes the profile's own prefix, so two ports run side by side without sharing switches.

## Lessons that cost real time

- **Trace the hardware; don't recall it.** GE register numbers, PSP struct layouts and HLE semantics taken from memory have been wrong, and each wrong guess cost a debugging session. Add or use a `<PREFIX>_TRACE_*` switch and read what the game actually does.
- **Measure before changing.** Find the cause with a trace, a profile or a number, then change code. Successive guesses at a rendering bug from screenshots failed four times in a row.
- **Compare like with like.** Compare a known-good build and yours at the same frame of the same scripted run. A model that isn't on screen yet looks the same in a broken and a working build. The guest clock is seeded from the wall clock and a connected gamepad changes input, so pin both when you need pixel-identical runs.
- **Give new behaviour an off switch.** An environment variable that restores the old behaviour lets anyone compare both on the same screen.
- **Logs.** stdout redirected to a file is block-buffered. A truncated last line means the buffer hasn't been flushed, not that the program has stopped.
- **The game's view differs from the host's.** A value the game reads every frame must mean exactly what it means on a PSP. A wrong one breaks things far from where it is read.
- **Performance problems in the renderer are usually memory reads.** Resolve guest memory once per draw, not per element. Profile before optimising.
- **A second game finds what one game hides.** The framework's own build does not compile most of `host/`, and one game's build exercises only the paths that game takes. Several of the bugs in this repository's history were found the first time a different game was built against it.

## Pull requests

- Open a draft pull request early and push after every commit that builds. Work that exists only locally is lost when a session ends.
- The description says what changed and why, how it was verified (platform, game, what you looked at), what was not verified, and which issues it closes or references.
- Update the docs the change makes wrong.
