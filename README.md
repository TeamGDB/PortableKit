# PortableKit

Everything needed to turn a PlayStation Portable game into a native application: a static recompiler for the game's code, and a full reimplementation of the system it ran on.

The game's Allegrex (MIPS) code is translated to C++ ahead of time and compiled for the target machine. There is no interpreter or JIT in the hot path. Around that code, PortableKit supplies what the console used to: the kernel, the system modules, a Vulkan renderer driven by the console's graphics command lists, audio, save data, ad hoc networking, a settings interface and a first-run installer.

## Parts

| Part | What it does |
| --- | --- |
| Recompiler | Analyses the game's executable and its runtime code overlays, and emits C++ |
| Runtime | Kernel, threads, timing, memory, and the system modules the game calls |
| Renderer | Graphics command lists translated to Vulkan, through SDL3 |
| Audio | Mixing, ATRAC3 and movie playback |
| Save data | The console's own save format, with import, export and backups |
| Networking | Ad hoc multiplayer: matchmaking, a built-in host server, and discovery on a LAN |
| Interface | Settings menu, on-screen keyboard, and the installer that reads the player's own disc image |

A game is a **profile** on top of all of this: its identity, what it needs from the system, and anything specific to it.

## Ports built on PortableKit

- [Yakumo](https://github.com/TeamGDB/Yakumo) — *Monster Hunter Portable 3rd HD Ver.*

## Status

Early. The code lives in the Yakumo repository today and is being separated out; this repository is where it will land.

## Legal

PortableKit contains no game data and no code from the console's system software. It is not affiliated with or endorsed by Sony Interactive Entertainment or any game publisher. Players supply their own legally obtained copy of a game.

MIT licensed; see [LICENSE](LICENSE).
