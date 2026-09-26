# Android packaging

What a game's Android app is made of besides the native code. A game builds `libmain.so` with the NDK toolchain and `-DPORTABLEKIT_ANDROID_APP=ON`, then packs it with `build_apk.sh`, giving its own application id and resources.

| File | What it is |
| --- | --- |
| `AndroidManifest.xml` | The app: Android 11 (API 30) and Vulkan 1.1 required, landscape, the libraries extracted so the overlay loader can list them, the backup rules. `build_apk.sh` renames its package to the game's `APP_ID` |
| `java/…/GameActivity.java` | SDL's activity plus what the host needs from Android: the display cutout, the document picker for folders and files, restarting the app |
| `res/` | The theme without a title bar, the adaptive icon's layout and the backup rules (saves and settings only). The game's own resources (`GAME_RES`) are laid over these: its name (`values/strings.xml`), its launcher icons, and its own backup rules if it needs to leave a folder out |
| `build_apk.sh` | Packs and signs an APK without Gradle, with the SDK's own tools |
| `make_icons.sh` | Makes a game's launcher icons from an SVG emblem; needs `rsvg-convert` and ImageMagick |

SDL's own Java classes come from the SDL3 source the build uses, not from this directory.

Yakumo's Android release (TeamGDB/Yakumo) is where this came from; its release script shows the whole path from a build directory to a signed APK.
