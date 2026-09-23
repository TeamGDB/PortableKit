# Architecture

PortableKit has two layers: the framework, in this repository, and a profile per game, each in the game's own repository.

## Framework

`psprecomp_core` provides:

- Allegrex/MIPS instruction decoding and architectural state;
- PSP ELF/PRX loading and relocation support;
- guest memory and EDRAM address handling;
- NID/import registration;
- generated-function registration and dispatch;
- an Allegrex interpreter, which runs guest addresses the corpus does not cover and runs a whole game that has not been recompiled yet (see [BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md));
- bounded generated-unit chaining;
- scheduler visibility boundaries;
- AOT hot-register and fast-memory support.

`host/` provides the rest of the console: the kernel and its scheduler, the HLE modules, a Vulkan renderer driven by the GE's display lists, audio, save data, ad hoc networking, the interface and the installer.

The root tools provide executable analysis and generic C++ generation. **Nothing in the framework may require a specific game's address, name or asset.** Where it needs to know something about the game it is running, it asks `portablekit::game()` — a `GameProfile` the port defines, described in [`host/profile.hpp`](../host/profile.hpp) and in [PROFILE_GUIDE.md](PROFILE_GUIDE.md).

## Profiles

A profile supplies what only the game can answer, and as little else as possible: its identity and hashes, the key its executable's tag selects, where it loads and how much memory it wants, its overlay slots, its save folders, and the names the port goes by. Two hooks let it add HLE calls the framework does not implement and patch its own loaded image. Its recompiled code and its release packaging live with it.

The profile boundary is intentional. Optimizations that are valid because of a measured address, ABI or data layout in one game stay in that game's profile even when they use reusable runtime APIs.

## Generated execution

Guest functions are emitted ahead of time as C++ and registered at their guest addresses. Generated units may chain directly when the runtime can prove that the target has not been replaced by an import/HLE/host override. Visibility boundaries materialize cached architectural state before host code or scheduling can inspect or replace the guest context.

## The interpreter

The interpreter has two jobs, and the second is not a fallback at all.

**Running what the corpus missed.** A corpus only contains the code that was visible when it was generated, so a title that
swaps overlays into a guest address window will eventually jump somewhere no generated
function claims. The outer dispatcher then interprets that code instead of stopping, in
bounded slices so scheduling and preemption still happen between them. Reaching any
registered address - an AOT unit, a host override or a PSP import stub - leaves the
interpreter through normal dispatch, so imports still run their HLE wrappers and thread
switching is unaffected. `PSPRECOMP_NO_INTERPRETER=1` restores the strict stop, and the
end-of-run report names the addresses that ran interpreted. A finished port should
never reach the interpreter during normal play, and that switch is how it is checked.

**Running a game that has no corpus yet.** Recompiling a whole game takes hours, and
nothing about what the game needs can be learned until it runs. With no generated code at
all, the runtime binds the executable's import stubs itself and interprets from the entry
point, reaching the same kernel, HLE and renderer as recompiled code would. The first run
of a brand-new port therefore already names the system calls it is missing, in the order
the game asks for them. It is roughly twenty times slower, which does not matter for
that. See [BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md).

## Code that changes at run time

Some titles copy code into memory while they run — overlays swapped into a fixed address window, for example — so the corpus generated from the executable cannot cover it. The framework leaves the policy to the profile and gives it three levers:

- `set_runtime_missing_function_hook` is called when dispatch finds nothing at an address. The profile can register functions for whatever is loaded there and return true, and dispatch retries. Only if it declines does the interpreter take over.
- `set_runtime_unsupported_hook` is called when generated code reaches an instruction it cannot execute, which is what stale code looks like after the memory under it was replaced. Returning true retries at the same address.
- `Runtime::unregister_functions(start, end)` drops every generated function in a range, so code compiled for the previous contents of a window cannot run against the new ones.

A profile typically builds each piece of run-time code as its own shared library, identifies which one is loaded by hashing guest memory, and swaps libraries through these three calls. Yakumo, which loads 355 of them into 12 shared slots, describes its scheme in full in its own README.

## Native fast paths

`Runtime::register_native_fast_path(address, callback)` is the extension point for a profile to replace a measured guest leaf without adding title-specific code to `psprecomp_core`. Profile-generated code enters through `Runtime::invoke_native_fast_path()` and falls back to the generated function if no profile callback is registered.
