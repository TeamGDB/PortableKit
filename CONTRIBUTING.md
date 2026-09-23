# Contributing to PortableKit

Thank you for wanting to help. PortableKit is a work in progress, so there is plenty to do; the [open issues](https://github.com/TeamGDB/PortableKit/issues) are the list. These rules are the short form of [AGENTS.md](AGENTS.md), which every change follows, whoever or whatever writes it.

## The rules

- **English only** in everything committed: code, comments, documentation, commit messages, issues and pull requests.
- **No game data, ever.** No disc images, executables (`EBOOT.BIN`, decrypted ELFs, PRX modules), code overlays, code generated from a game, saves, audio or video dumps, or screenshots and captures of a game's own art. Not in commits, not in issues, not in pull requests, not as attachments. Describe what you saw in words and numbers instead: addresses, register values, log lines.
- **Nothing personal.** No home paths (`/Users/<name>`, `C:\Users\<name>`), user names, machine names, e-mail addresses other than a GitHub no-reply address, or IP addresses in anything committed. Check your logs before pasting them.
- **Write it yourself.** Public documentation, observed behaviour and other projects are fine to *learn* from. Code from a project whose licence is incompatible with MIT — PPSSPP and other GPL emulators included — must never be copied, pasted, adapted or translated line by line. Constants, offsets and format facts are fine. Third-party code that is intentionally included must have a compatible licence and be recorded in [docs/SOURCE_PROVENANCE.md](docs/SOURCE_PROVENANCE.md).
- **Trace the game; don't recall the hardware.** GE register numbers, PSP struct layouts and system call semantics written from memory have been wrong in this codebase before, and each one cost a debugging session. Turn on the relevant `<PREFIX>_TRACE_*` switch and read what the game actually does, then write the code.
- **Nothing about one game in the framework.** A game's disc id, addresses, save folders and quirks belong in that game's profile, in its own repository. If the framework needs to know something about a game, add a field or a hook to `portablekit::GameProfile` (`host/profile.hpp`) and let the profile answer.
- **Say what you did not verify.**

## How to send a change

1. Fork the repository and create a branch. Nobody pushes to `main`, maintainers included.
2. Build the framework and run its tests:

   ```bash
   cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build out -j2
   ctest --test-dir out --output-on-failure
   ```

   A change to `host/` also needs at least one port built against it, because the framework's own build does not compile the renderer or the interface. See [docs/BUILDING.md](docs/BUILDING.md).
3. Open a pull request. Say what changed and why, how you verified it (platform, GPU, which game, what you looked at), what you did not verify, and which issues it closes. Continuous integration builds and tests it on Linux, macOS and Windows.

Small, focused pull requests are reviewed faster than large ones. If you are planning something large, open an issue first so the approach can be agreed before you spend the time.

## Reporting a bug

Use the bug report form. It asks for your platform, the commit you built, and the log; please give all three. If the problem shows up while playing a port, report it to that port first — it will be moved here if the cause is in the framework.

## Licence

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE), like the rest of the project.
