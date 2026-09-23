# PortableKit

Everything needed to turn a PlayStation Portable game into a native application: a static recompiler for the game's code, and a full reimplementation of the system it ran on.

> **Work in progress.** PortableKit is under active development and incomplete. Expect bugs, missing system calls, and breaking changes to its interfaces — including the profile structure ports are written against — without notice or deprecation period. Nothing here is a stable release.

> **No game data is included, and none is accepted.** PortableKit is a framework: it contains no game, no disc image, no executable, no code generated from a game and no assets. A port built on it needs your own legally obtained copy of the game. This project is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment or any game publisher.

The game's Allegrex (MIPS) code is translated to C++ ahead of time and compiled for the target machine. There is no JIT, and no interpreter in the hot path: the interpreter exists to run what the recompiler missed, and to run a game before any of it has been recompiled. Around that code, PortableKit supplies what the console used to: the kernel, the system modules, a Vulkan renderer driven by the console's graphics command lists, audio, save data, ad hoc networking, a settings interface and a first-run installer.

## Parts

| Part | What it does |
| --- | --- |
| Recompiler | Analyses the game's executable and its runtime code overlays, and emits C++ |
| Interpreter | Runs what the recompiler missed, and runs a game that has not been recompiled yet |
| Runtime | Kernel, threads, timing, memory, and the system modules the game calls |
| Renderer | Graphics command lists translated to Vulkan, through SDL3 |
| Audio | Mixing, ATRAC3 and movie playback |
| Save data | The console's own save format, with import, export and backups |
| Networking | Ad hoc multiplayer: matchmaking, a built-in host server, and discovery on a LAN |
| Interface | Settings menu, on-screen keyboard, and the installer that reads the player's own disc image |

A game is a **profile** on top of all of this: its identity, what it needs from the system, and anything specific to it.

## Ports built on PortableKit

| Port | Game it targets | Where it is |
| --- | --- | --- |
| [Yakumo](https://github.com/TeamGDB/Yakumo) | *Monster Hunter Portable 3rd HD Ver.* | Playable, with releases. The framework was extracted from it; Yakumo itself still carries its own copy and has not been switched over yet ([#2](https://github.com/TeamGDB/PortableKit/issues/2)) |
| [Purun](https://github.com/TeamGDB/Purun) | *LocoRoco 2* | The first level plays through, saves and continues; tested on macOS only |
| [Tenkawa](https://github.com/TeamGDB/Tenkawa) | *Dragon Ball Z: Tenkaichi Tag Team* | Boots and runs its frame loop; draws nothing yet |

A port is its own repository: a file of constants, a short `CMakeLists.txt` and this one as a submodule. If a port needs more than that, the framework is missing a seam.

## Start here

- [Bringing up a new game](docs/BRINGING_UP_A_GAME.md) — and why you run the game under the interpreter before recompiling anything
- [Writing a profile](docs/PROFILE_GUIDE.md)
- [Building](docs/BUILDING.md), and [why the build keeps its incremental state the way it does](docs/BUILD_SYSTEM.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Where the code comes from, and third-party licences](docs/SOURCE_PROVENANCE.md)
- [Contributing](CONTRIBUTING.md), and the [working notes](AGENTS.md) every change follows

## Status

Early, and honest about it. The recompiler, the runtime and the system layer are here, moved out of Yakumo with their history. The framework builds on its own and its tests pass on Linux, macOS and Windows in [continuous integration](.github/workflows/ci.yml); two further games, Purun and Tenkawa, build against it as a submodule and a profile of constants.

The system layer was first written against one game, so every new game finds gaps in it: unimplemented system calls, graphics commands one game never used, formats one game never read. The [open issues](https://github.com/TeamGDB/PortableKit/issues) are that list.

What has been run with a game, and where, is each port's own business: see its README. Only Yakumo has been run on Linux, Windows and the Steam Deck.

## Building the framework on its own

This builds the recompiler, the runtime and the tests. It needs no game, no GPU, no SDL and no FFmpeg, and takes minutes.

```bash
git clone --recursive https://github.com/TeamGDB/PortableKit.git
cd PortableKit
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out -j2
ctest --test-dir out --output-on-failure
```

| Platform | What you need |
| --- | --- |
| Linux | A C++20 compiler, CMake 3.20+, Ninja, Python 3. CI uses GCC 13 on Ubuntu 24.04 |
| macOS | Xcode command line tools, CMake 3.20+, Ninja, Python 3 (`brew install cmake ninja python`). CI uses Apple Clang 15 on macOS 14 |
| Windows | Visual Studio 2022 with the C++ workload, CMake 3.20+, Python 3. Use the Visual Studio generator (then `--config Release` and `ctest -C Release`), or Ninja from a developer command prompt. CI uses MSVC 19.44 |

A **port** additionally needs SDL3, a Vulkan loader and headers, and `glslangValidator` for the renderer, plus `make` and a C compiler to build the bundled FFmpeg (or an installed FFmpeg with `-DPORTABLEKIT_FFMPEG=system`). On macOS, Vulkan runs through MoltenVK: `brew install sdl3 molten-vk vulkan-loader vulkan-headers glslang`. `ccache` is used automatically when it is installed and saves hours. [docs/BUILDING.md](docs/BUILDING.md) has the details.

## Reporting bugs

Framework bugs go to [PortableKit's issues](https://github.com/TeamGDB/PortableKit/issues); a problem you see while playing a port goes to that port's issues first. Say which platform and GPU, which commit, and attach the log. **Never attach game files, disc images, executables, saves or generated code.**

## Legal

PortableKit contains no game data and no code from the console's system software. It is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment or any game publisher. PlayStation and PSP are trademarks of Sony Interactive Entertainment; game titles named here are trademarks of their owners and are used only to say which game a port targets. Players supply their own legally obtained copy of a game.

PortableKit's own code is MIT licensed; see [LICENSE](LICENSE). Third-party code in `third_party/` keeps its own licence, and a build that bundles FFmpeg links it dynamically under the LGPL; [docs/SOURCE_PROVENANCE.md](docs/SOURCE_PROVENANCE.md) lists every component and what a binary release has to ship with it.
