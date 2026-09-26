# Bringing up a new game

How to get from a disc image to a first list of what is missing, without waiting a day for it.

The short version: **run the game under the interpreter before any of its code has been recompiled.** The recompile takes hours. Almost everything you need to learn in the first day — which system calls the game makes, what it asks of the graphics hardware, which instructions the recompiler does not cover — you can learn in the first ten minutes, because the interpreter runs the game's code as it is.

## The interpreter is part of the framework, not a fallback

`src/interpreter.cpp` is an Allegrex interpreter with the same memory, the same kernel and the same HLE as the recompiled code. It has two jobs, and both matter:

- **It runs what the recompiler missed.** Code reached through a computed jump, code the analyser never saw as a function, code in an overlay with no corpus. When the dispatcher finds no recompiled function at an address, the interpreter executes from there and hands control back.
- **It runs a game that has no recompiled code at all.** This is the one that saves a day.

It is roughly twenty times slower than recompiled code. It is a development tool, not how anyone plays. A finished port should never fall back to it during normal play, and you check that with `PSPRECOMP_NO_INTERPRETER=1`, which turns the fallback off: the runtime then stops at the first address with no recompiled function, and says which. Run a port that way through its opening and its first level before you call it finished.

## The order to do things in

Assume you have a disc image and nothing else.

### 1. Build the framework and a profile that is only constants

Write the profile — identity, hashes, the `~PSP` tag and its key, the load address and memory size, the save folders — and a four-line `CMakeLists.txt`. See the [profile guide](PROFILE_GUIDE.md). This builds in minutes, because there is no generated code yet.

### 2. Prepare the executable

```bash
<Game>Native --install /path/to/image.iso --in-place
```

The installer checks the disc id in `PARAM.SFO` and the SHA-256 of `PSP_GAME/SYSDIR/EBOOT.BIN`, decrypts the executable with the key the profile supplies, and checks the result. If your `decryption_tag` or `decryption_key` is wrong you find out here, in seconds.

### 3. Run it, now

```bash
<PREFIX>_NO_RENDER=1 <PREFIX>_NO_AUDIO=1 timeout 90 <Game>Native
```

With no corpus, the program says so and runs the whole game under the interpreter. Bound every run with `timeout`; never leave a game running.

What you get out of this first run:

- **`HLE imports: N total, M implemented, S logging stubs`** — the size of the gap before a single instruction has run. Add `<PREFIX>_LIST_STUBS=1` and it names every one of them, grouped by library. That is the executable's own answer; do not try to work it out by grepping the framework for `hle.add`, because calls registered in a loop do not appear as literals and you will overstate the gap.
- **`[hle-stub] Library::name a0=… a1=… ra=…`** — printed the first time the game calls something that is not implemented, with its arguments. This is the list you are after, in the order the game needs it.
- **`[interpreter] instructions=… entries=… unique_addresses=…`** and the addresses with the most instructions — where the game actually spent its time before it stopped.
- **`Runtime stopped: …`** — why it stopped. `PSP scheduler deadlock: no runnable thread` usually means an unimplemented call returned a lie and the game gave up; the last `[hle-stub]` line before it names the culprit.

Then implement the call the game stopped on, run again, and repeat. Each round costs a build of the host only — seconds — because there is still no generated code to recompile.

### 4. Only then, generate and compile

```bash
psp_recomp <EBOOT.ELF> --auto generated
cmake --build <build> -j2
```

Generating is a couple of minutes. Compiling the result is the hours. Start it when you have a boot that gets somewhere, and do something else while it runs; the corpus does not change what is missing, only how fast the game reaches it.

`psp_recomp` tells you what it could not lower, as `unsupported(...)` calls in the generated code, and each one carries the address and the instruction word. Count them before you compile:

```bash
grep -rho 'unsupported(0x[0-9A-Fa-f]*u, 0x[0-9A-Fa-f]*u, "[^"]*"' generated/*.cpp \
  | sed 's/.*"\(.*\)"/\1/' | sort | uniq -c | sort -rn
```

A handful of sites is normal; they are reached, if ever, through the interpreter. A category with thousands of sites is a real gap in the recompiler and worth knowing about before you spend three hours compiling.

Two things to know about these. First, an unsupported site is not always survivable: the interpreter refuses the same instruction the recompiler could not lower, so a site the game actually executes stops the run rather than slowing it. Second, the count collapses once you look: 435 sites in one game turned out to be six instructions.

**Decode them, do not recall them.** The check that tells you your reference uses the same convention as this decoder is that it reproduces the entries `src/decoder.cpp` already has. If it does not, you are reading a different encoding and everything else you take from it is wrong. Then confirm against the corpus itself — the instructions around a site usually say what it must be doing.

## Useful switches while bringing a game up

Every one of these takes the profile's own prefix, so `TENKAWA_TRACE_KERNEL` for one port and `MHP3RD_TRACE_KERNEL` for another, and two ports can run side by side without sharing them.

| Variable | What it does |
| --- | --- |
| `<PREFIX>_NO_RENDER`, `<PREFIX>_NO_AUDIO` | No window, no sound: a boot check that runs anywhere |
| `<PREFIX>_LIST_STUBS` | Name every import nothing implements, at start-up |
| `<PREFIX>_NO_HLE_EXTENSIONS` | Ignore the [HLE extension modules](HLE_EXTENSIONS.md) the build links, for comparing with the framework's own HLE |
| `<PREFIX>_STRICT_HLE` | Do not bind logging stubs for unimplemented imports, so the game stops at the first one instead of carrying on with a wrong answer |
| `<PREFIX>_TRACE_KERNEL`, `_TRACE_IO`, `_TRACE_GE`, `_TRACE_SAVEDATA`, … | One subsystem each |
| `<PREFIX>_TRACE_SYNC` | Every kernel object a thread waits on, which is how a deadlock is read |
| `<PREFIX>_TRACE_GE_LIST=N[:M]` | Every command word of display-list runs N to N+M-1: what a screen that draws nothing is built from |
| `<PREFIX>_TRACE_SAS` | SAS voices set, keyed on and off, and each change of the end flags a game polls |
| `<PREFIX>_NO_BUSY_CLOCK` | Keep emulated time still for a thread that never waits, as before the busy clock (`Kernel::charge_busy_time`) |
| `PSPRECOMP_NO_INTERPRETER=1` | Turn the interpreter off. Nothing runs without a corpus; with one, the runtime stops at the first address the recompiler missed and names it |
| `PSPRECOMP_MAX_DISPATCHES` | Stop after this many dispatches, for a bounded run |
| `PSPRECOMP_INTERPRETER_WATCH` | Guest addresses, comma separated: report the registers each time interpreted code reaches one. `PSPRECOMP_INTERPRETER_WATCH_WORDS=N` also dumps N words at `a0`, `PSPRECOMP_INTERPRETER_WATCH_MEMORY` (addresses, comma separated) prints those guest words with every report, and `PSPRECOMP_INTERPRETER_WATCH_LIMIT` bounds how many times each address is reported (40 by default, 0 for no limit) |

## Reading what the game itself decides

A game that boots, draws and then will not go on has usually taken one
branch of its own code where it should have taken the other, and the last
step of finding out which is not something a trace of the system calls can
tell you. Disassemble outwards from what it last did, narrow it to the
compare, then watch that address with `PSPRECOMP_INTERPRETER_WATCH` and read
the registers.

This found the whole of one game's opening: an object waiting for a field to
become `-1`, the scene that would have set it sitting on a message screen,
and that screen's state 1 reading the pad and doing nothing at all until the
confirm button is pressed. None of that is visible in an I/O trace, and all
of it took three runs once the addresses were known.

It only sees interpreted code. For a game with no corpus that is everything;
for a game with one it is whatever the recompiler did not cover, which
includes any module the game loads and runs at run time. Run without a
corpus when you need to watch an address in the executable itself.

## A warning about a run with a window

A run with the renderer up does not always die on `SIGTERM`, so `timeout 90` can leave the game running. Use `timeout -s KILL`. Never leave a game running.

## What this does not tell you

The interpreter reaches the same HLE and the same renderer as recompiled code, so what it shows about system calls and graphics is real. What it cannot show you is anything about speed, and anything that depends on a game running at its own pace: timing-sensitive code, audio underruns, or a frame rate. Those wait for the corpus.
