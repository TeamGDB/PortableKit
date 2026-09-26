#!/usr/bin/env bash
# Packs an APK of a PortableKit game for arm64-v8a without Gradle, from a
# build configured with the NDK toolchain and -DPORTABLEKIT_ANDROID_APP=ON. It
# carries no game data: the player's disc image is installed on the device.
#
#   build_apk.sh <build dir> <SDL3 source dir> <libSDL3.so> <output.apk> [overlay limit]
#
# What makes the APK the game's own comes from the game's repository:
#   APP_ID    the application id, such as io.github.teamgdb.yakumo (required)
#   GAME_RES  a resource directory laid over packaging/android/res: at least
#             values/strings.xml with app_name, and the launcher icons
#             (mipmap-*/ic_launcher*.png and values/ic_launcher_background.xml;
#             make_icons.sh makes them from an SVG). Required.
#   NOTICES   the third-party notices to carry in the APK's assets (optional)
#   GAME_REPO the checkout `git describe` names the version after (default:
#             the one GAME_RES is in)
#
# FONT_DIR may name a directory holding NotoSansCJKjp-Regular.otf and
# NotoSansCJK-LICENSE.txt, the fallback font and licence a Linux release
# fetches (packaging/linux/sources.sh pins both); they go into the APK's
# assets, and the app unpacks the font on its first start. Without it the
# game's text is blank unless the device has a font stb_truetype reads.
#
# ANDROID_HOME must name the SDK (build-tools and a platform are needed).
# DEBUGGABLE=1 marks the app debuggable, so `adb shell run-as` reaches its
# files (its log, its data) on a test device; leave it out for players.
# KEYSTORE (with KEYSTORE_PASS and KEY_ALIAS) signs with a key of your own, so
# that later builds install over earlier ones and keep the player's data;
# without it a throwaway key is made in the build directory.
#
# The app needs Android 10 (API 29), where every 64-bit device has Vulkan
# 1.1: build the native code for android-29 (ANDROID_PLATFORM), or it may
# import libc functions Android 10 lacks (libc++ waits with
# pthread_cond_clockwait from android-30 on). Newer Android features are
# looked up at run time. After packing, every function a packed library
# imports is checked against Android 10's system libraries.
# The overlay limit packs only the first N overlay libraries, to keep a test
# APK small; without it every one is packed.
set -euo pipefail

build_dir="$(cd "$1" && pwd)"
sdl_dir="$(cd "$2" && pwd)"
sdl_lib="$3"
output="$4"
overlay_limit="${5:-}"
here="$(cd "$(dirname "$0")" && pwd)"
app_id="${APP_ID:?set APP_ID to the application id of the game}"
game_res="$(cd "${GAME_RES:?set GAME_RES to the Android resources of the game}" && pwd)"

sdk="${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
build_tools="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
platform="$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)"
android_jar="$platform/android.jar"
work="$build_dir/apk"
rm -rf "$work"
mkdir -p "$work/classes" "$work/dex" "$work/lib/arm64-v8a"

echo "compiling SDL's Java activity and the app's own"
javac -nowarn --release 11 -classpath "$android_jar" -d "$work/classes" \
    $(find "$sdl_dir/android-project/app/src/main/java" "$here/java" -name '*.java') 2> "$work/javac.log"
min_sdk=29
"$build_tools/d8" --release --min-api "$min_sdk" --lib "$android_jar" --output "$work/dex" \
    $(find "$work/classes" -name '*.class')

echo "linking resources"
# The licences of what the APK carries: the third-party notices, SDL's and
# FFmpeg's (with where its source is), and the font's when it is packed.
mkdir -p "$work/assets/licenses"
if [[ -n "${NOTICES:-}" ]]; then cp "$NOTICES" "$work/assets/licenses/"; fi
cp "$sdl_dir/LICENSE.txt" "$work/assets/licenses/SDL3-LICENSE.txt"
cp "$build_dir"/bin/lib/FFmpeg-COPYING.LGPLv2.1.txt "$build_dir"/bin/lib/FFmpeg-SOURCE.txt "$work/assets/licenses/"
if [[ -n "${FONT_DIR:-}" ]]; then
    mkdir -p "$work/assets/fonts"
    cp "$FONT_DIR/NotoSansCJKjp-Regular.otf" "$work/assets/fonts/"
    cp "$FONT_DIR/NotoSansCJK-LICENSE.txt" "$work/assets/licenses/NotoSansCJK-OFL.txt"
fi
assets=(-A "$work/assets")
"$build_tools/aapt2" compile --dir "$here/res" -o "$work/res.zip"
"$build_tools/aapt2" compile --dir "$game_res" -o "$work/game-res.zip"
# The version: `git describe` of the checkout, and the number of commits as
# the code Android compares, so a later build is always an update
# (VERSION_NAME and VERSION_CODE override them).
repo="${GAME_REPO:-$(git -C "$game_res" rev-parse --show-toplevel 2>/dev/null || echo "$game_res")}"
version_name="${VERSION_NAME:-$(git -C "$repo" describe --tags --always --dirty 2>/dev/null || echo 0.0)}"
version_code="${VERSION_CODE:-$(git -C "$repo" rev-list --count HEAD 2>/dev/null || echo 1)}"
"$build_tools/aapt2" link -I "$android_jar" --manifest "$here/AndroidManifest.xml" \
    --min-sdk-version "$min_sdk" --target-sdk-version 35 --version-name "$version_name" \
    --version-code "$version_code" ${DEBUGGABLE:+--debug-mode} "${assets[@]}" -o "$work/unsigned.apk" \
    --rename-manifest-package "$app_id" "$work/res.zip" -R "$work/game-res.zip" --auto-add-overlay

echo "adding native libraries"
lib="$work/lib/arm64-v8a"
cp "$sdl_lib" "$lib/libSDL3.so"
cp "$build_dir/bin/libmain.so" "$lib/"
cp "$build_dir"/bin/lib/libavcodec.so "$build_dir"/bin/lib/libavutil.so "$lib/"
overlays=("$build_dir"/bin/overlays/libovl*.so)
if [[ -n "$overlay_limit" ]]; then overlays=("${overlays[@]:0:$overlay_limit}"); fi
cp "${overlays[@]}" "$lib/"
strip="$(ls -d "$ANDROID_NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)"
"$strip" --strip-unneeded "$lib"/*.so
# Every function a packed library imports must be in Android $min_sdk's system
# libraries or in another packed one; a missing one stops the app loading
# there. Weak imports may be missing: their callers check them first.
llvm="$(dirname "$strip")"
stubs="$llvm/../sysroot/usr/lib/aarch64-linux-android/$min_sdk"
{
    for system in libc libm libdl liblog libandroid libvulkan libOpenSLES libGLESv1_CM libGLESv2 libEGL; do
        "$llvm/llvm-nm" -D --defined-only "$stubs/$system.so"
    done
    "$llvm/llvm-nm" -D --defined-only "$lib"/*.so
} 2> /dev/null | awk 'NF >= 3 { sub(/@.*/, "", $3); print $3 }' | sort -u > "$work/exports.txt"
missing="$("$llvm/llvm-nm" -D --undefined-only "$lib"/*.so | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' |
    sort -u | comm -23 - "$work/exports.txt")"
if [[ -n "$missing" ]]; then
    echo "error: the libraries import what Android $min_sdk lacks:" $missing >&2
    exit 1
fi
echo "every import is in Android $min_sdk"
# extractNativeLibs is on, so the libraries may be compressed: the package
# manager unpacks them at install time.
(cd "$work" && cp dex/classes.dex . && zip -q unsigned.apk classes.dex && zip -q -r -9 unsigned.apk lib)

echo "aligning and signing"
keystore="${KEYSTORE:-$build_dir/apk-research.keystore}"
password="${KEYSTORE_PASS:-research}"
alias="${KEY_ALIAS:-research}"
if [[ ! -f "$keystore" ]]; then
    [[ -n "${KEYSTORE:-}" ]] && { echo "error: $keystore not found" >&2; exit 1; }
    keytool -genkeypair -keystore "$keystore" -storepass research -keypass research -alias research \
        -keyalg RSA -keysize 2048 -validity 3650 -dname "CN=$app_id research build" > /dev/null 2>&1
fi
"$build_tools/zipalign" -f -P 16 4 "$work/unsigned.apk" "$work/aligned.apk"
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass "pass:$password" --key-pass "pass:$password" \
    --ks-key-alias "$alias" --out "$output" "$work/aligned.apk"
echo "wrote $output ($(du -h "$output" | cut -f1), ${#overlays[@]} overlay libraries, version $version_name ($version_code))"
