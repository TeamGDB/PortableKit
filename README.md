# PortableKit

Everything needed to turn a PlayStation Portable game into a native application: a static recompiler for the game's code, and a full reimplementation of the system it ran on.

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

- [Yakumo](https://github.com/TeamGDB/Yakumo) — *Monster Hunter Portable 3rd HD Ver.*
- [Tenkawa](https://github.com/TeamGDB/Tenkawa) — *Dragon Ball Z: Tenkaichi Tag Team*

A port is its own repository: a file of constants, a four-line `CMakeLists.txt` and this one as a submodule. If a port needs more than that, the framework is missing a seam.

## Start here

- [Bringing up a new game](docs/BRINGING_UP_A_GAME.md) — and why you run the game under the interpreter before recompiling anything
- [Writing a profile](docs/PROFILE_GUIDE.md)
- [Building](docs/BUILDING.md)
- [Working notes](AGENTS.md)

## Status

Early, and honest about it. The recompiler, the runtime and the system layer are here, moved out of Yakumo with their history. The framework builds and its tests pass; a game builds against it as a subdirectory and a profile of constants.

What is not done: Yakumo itself still carries its own copy and has not been switched over ([#2](https://github.com/TeamGDB/PortableKit/issues/2)). The system layer was written against one game, so a second game finds gaps in it — the open issues are that list.

## Legal

PortableKit contains no game data and no code from the console's system software. It is not affiliated with or endorsed by Sony Interactive Entertainment or any game publisher. Players supply their own legally obtained copy of a game.

MIT licensed; see [LICENSE](LICENSE).
