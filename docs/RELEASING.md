# Releasing a game

The framework packs a game's release; the game says what it is called and brings what is its own. The scripts came from Yakumo's release scripts and do the same checks for every game: pinned third-party sources checked by SHA-256, notices that must name what is bundled, no game data and no path of the build machine in any artifact.

## What the game brings

In its own repository:

| File | What it is |
| --- | --- |
| `packaging/release.env` | `RELEASE_NAME` (what players see, the app bundle's name), `RELEASE_SLUG` (artifact names), `RELEASE_TARGET` (the CMake target), `RELEASE_APP_ID` (reverse-DNS id: Flatpak app id; lower-cased, the macOS bundle id), `RELEASE_OVERLAYS` (how many overlay libraries the build makes), `RELEASE_GAME_NAMES` (lower-case globs of the game's own file names that must never be packed, such as its disc id), and optionally `RELEASE_MACOS_CATEGORY` |
| `packaging/THIRD_PARTY_NOTICES.md` | The notices for what the release bundles; the scripts refuse to package when it does not name the pinned versions in `packaging/sources.sh` and `cmake/FFmpeg.cmake` |
| `LICENSE` | The game's licence; the framework's own goes in beside it |
| `packaging/macos/<RELEASE_NAME>.icns`, `packaging/macos/README.txt` | The macOS icon and the read-me in the disk image (`@MINIMUM_SYSTEM_VERSION@` is filled in) |
| `packaging/icon.png` (256×256), `packaging/linux/README.txt`, `packaging/linux/<app id>.desktop`, `packaging/linux/<app id>.metainfo.xml` | The Linux icon, read-me, desktop entry and AppStream data (`@VERSION@`, `@DATE@` are filled in) |
| `packaging/android/res/` | For Android, the name and launcher icons; see `packaging/android/README.md` |

`release.env` may also set, for a program with more than one executable file: `RELEASE_EXTRA_EXECUTABLES` (programs of the build's `bin/` to put beside the executable), `RELEASE_RESOURCES` (folders of `bin/` to ship in `Contents/Resources`, as `<from>=<to>`) and `RELEASE_PROFILE_DIR` (where the profile is in the checkout, `.` by default). The desktop app uses all three.

## macOS

On an Apple Silicon Mac with the macOS 13 SDK installed (the deployment target in `packaging/sources.sh`):

```sh
cmake -S <game> -B out-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTABLEKIT_RELEASE=ON -DPORTABLEKIT_FFMPEG=bundled
cmake --build out-release --target <RELEASE_TARGET>
<game>/portablekit/scripts/release_macos.sh --game <game> out-release
```

It builds SDL3 and the Vulkan loader from pinned sources for the deployment target, takes MoltenVK's pinned release, assembles `<Name>.app`, links everything inside the bundle, checks the system imports against the macOS 13 SDK, signs ad hoc (no notarization) and packs a disk image into `out/release-macos/dist`, with the FFmpeg source archive and `SHA256SUMS`.

`--packaging DIR` reads `release.env`, the notices and `macos/` from another directory than `<game>/packaging`. `--license NAME=FILE`, `--notices FILE` and `--extra PATH` (each repeatable) add what a build brings beyond the game and the framework: a licence as `licenses/NAME-LICENSE.txt`, a notices file in `licenses/`, and files or folders beside the app in the disk image and the zip.

## Linux

`scripts/release_linux.sh --game <game>` builds inside the Steam Runtime 3 "sniper" SDK container (podman or docker, x86-64) from the game's generated code, and packs a tarball and a Flatpak bundle. It needs flatpak with Flathub, ostree and curl.
