# Building

How a port is built, where the time goes, and how to work on the code without paying for a full build each time.

You need your own copy of the game as a disc image. Nothing else from it is needed, and nothing from it is ever committed.

## The framework on its own

```bash
cmake -S . -B out -G Ninja
cmake --build out -j2
ctest --test-dir out
```

Minutes, and no game data. It builds `psp_analyze`, `psp_recomp`, `dump_function`, the runtime, and the tests that need no game. It does **not** compile `host/main.cpp` or most of `host/`: those are a game's target, so build a port too before calling a system-layer change done.

## A port

A port's repository adds this one as a subdirectory:

```bash
git clone --recursive https://github.com/TeamGDB/<Port>.git
cd <Port>
cmake -S . -B out -G Ninja
cmake --build out -j2
```

Five stages, and only the last two are slow:

| Stage | What happens | Command |
| --- | --- | --- |
| 1. Bootstrap | Build the executable with no game code in it yet | `cmake --build out -j2` |
| 2. Prepare | The bootstrap checks your disc image and decrypts the game's executable from it | `<Game>Native --install image.iso` |
| 3. Run it | Under the interpreter, before anything is recompiled — see [BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md) | `<Game>Native` |
| 4. Generate | Analyse the executable and write it out as C++ units | `psp_recomp <EBOOT.ELF> --auto generated` |
| 5. Compile | Compile those units with the host | `cmake --build out -j2` |

A game with code overlays has a sixth stage: extract each overlay, recompile it, and build a shared library per overlay that the host loads at run time. How the overlays are extracted is the game's business, because the container they sit in is the game's own; the recompiling and the CMake target are the framework's (`tools/add_overlay.py`, and `portablekit_add_game()`).

### Where the time goes

Measured on an Apple M1 with 8 GB, from a fresh clone with an empty compiler cache:

| Stage | Time |
| --- | --- |
| Framework, bootstrap, prepare, first interpreted run | minutes |
| Generating the corpus (a 2.6 MB executable, 153 units) | about 2.5 minutes |
| Compiling the corpus | hours |
| Each code overlay | seconds to a minute |

**The generated units are very large, so compiling them needs a lot of memory.** The build limits how many compile at once to one per 4 GiB, whatever `-j` you pass (`PSPRECOMP_GENERATED_JOBS`). That is 2 on an 8 GB machine, and passing `-j8` does not make it faster, only more likely to swap.

**It is slow only once.** With `ccache` installed, which the build uses automatically, a later rebuild of unchanged generated code takes seconds, across checkouts.

## Working on the code

- **Host-only changes rebuild in seconds.** Changes under `include/psprecomp/` rebuild everything, including every overlay, so avoid them unless they are the point.
- **Don't rebuild the corpus per checkout.** Copy `generated/` from a checkout that has it with a plain `cp -R`, not a copy that keeps old timestamps. Point at an existing overlay build with `<PREFIX>_OVERLAY_DIR`.
- **Don't re-prepare the game per checkout.** `<PREFIX>_GAME_DIR` points at a directory that already holds `EBOOT.ELF`, `disc.iso` and `ms0/`.
- **Build with `cmake --build`, never `ninja` directly.** `cmake --build` holds the per-directory lock and repairs the Ninja log first; see [BUILD_SYSTEM.md](BUILD_SYSTEM.md). Run one build per build directory, and never delete `.ninja_deps` or `.ninja_log`.

## Options

| Option | Default | What it does |
| --- | --- | --- |
| `PSPRECOMP_GENERATED_OPT_LEVEL` | `2` | Optimisation level for generated units. `0` compiles much faster and runs much slower |
| `PSPRECOMP_GENERATED_JOBS` | memory / 4 GiB | How many generated units compile at once |
| `PORTABLEKIT_RENDERER` | `ON` | Build the Vulkan renderer. Off gives a headless build |
| `PORTABLEKIT_FFMPEG` | `bundled` | `bundled` builds FFmpeg with the project, `system` uses one found through pkg-config, `OFF` drops music and movies |
| `PORTABLEKIT_RELEASE` | `OFF` | Build for distribution: no paths into the checkout, libraries from `lib/` next to the executable |
| `PSPRECOMP_LTO` | `OFF` | Whole-program optimisation |
| `PORTABLEKIT_BUILD_LABEL` | empty | A name for the build, shown beside the version: in the window title, the menu's About section, the desktop app's library header and `portablekit --version`. For telling a build made with other components (see below) from the plain one without patching sources |
| `PORTABLEKIT_HLE_EXTENSION_DIRS` | empty | Directories of [HLE extension modules](HLE_EXTENSIONS.md) to link into every program |

## Dependencies

SDL3, Vulkan and `glslangValidator` for the renderer; without them the build says so and produces a headless port. FFmpeg for music and movies, built by the project unless you ask otherwise. `ccache` if you have it. Python 3 for the build's own scripts.
