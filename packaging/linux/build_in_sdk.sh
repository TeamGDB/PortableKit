#!/usr/bin/env bash
# Builds everything a Linux release contains, inside the Steam Runtime 3
# "sniper" SDK container that scripts/release_linux.sh starts:
#
#   1. SDL3 from the source pinned in sources.sh
#   2. the game's recompiled executable, from the generated code already in
#      its checkout (the game's generate step makes it; the code is the same
#      on every platform), with the LGPL-only FFmpeg the build bundles
#      (cmake/FFmpeg.cmake)
#   3. its overlay libraries, if it has any
#   4. a staging tree with the executable, overlays/, lib/, fonts/ and
#      licenses/, the part both the tarball and the Flatpak ship
#
# Every step is resumable: finished dependencies are kept, ccache holds the
# compiled code.
#
# Environment: PORTABLEKIT_RELEASE_WORK (required) is the work directory;
# PORTABLEKIT_GAME (required) the game's checkout; PORTABLEKIT_JOBS the
# number of parallel jobs (default 4).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
kit_dir="$(cd "$here/../.." && pwd)"
repo_dir="${PORTABLEKIT_GAME:?set PORTABLEKIT_GAME to the checkout of the game}"
work="${PORTABLEKIT_RELEASE_WORK:?set PORTABLEKIT_RELEASE_WORK to the work directory}"
jobs="${PORTABLEKIT_JOBS:-4}"
# shellcheck source=../sources.sh
source "$kit_dir/packaging/sources.sh"
# shellcheck disable=SC1091
source "$repo_dir/packaging/release.env"
RELEASE_OVERLAYS="${RELEASE_OVERLAYS:-0}"

sources="$work/sources"
deps="$work/deps"
deps_build="$work/deps-build"
build="$work/build"
stage="$work/stage/$RELEASE_SLUG"

export CC=gcc-14 CXX=g++-14
export CCACHE_DIR="$work/ccache" CCACHE_MAXSIZE=20G
export PKG_CONFIG_PATH="$deps/lib/pkgconfig"
mkdir -p "$sources" "$deps" "$deps_build"

step() { printf '\n=== %s\n' "$*"; }

# fetch <file name> <url> <sha256>
fetch() {
    local target="$sources/$1"
    if [[ -f "$target" ]] && echo "$3  $target" | sha256sum -c --status; then return; fi
    curl -fsSL -o "$target.part" "$2"
    if ! echo "$3  $target.part" | sha256sum -c --status; then
        echo "error: $2 does not match its pinned SHA-256" >&2
        exit 1
    fi
    mv "$target.part" "$target"
}

# A dependency is rebuilt when its version or configuration changes.
stamp_matches() { [[ -f "$deps/.$1.stamp" && "$(cat "$deps/.$1.stamp")" == "$2" ]]; }

step "Fetching pinned sources"
fetch "SDL3-$SDL3_VERSION.tar.gz" "$SDL3_URL" "$SDL3_SHA256"
fetch NotoSansCJKjp-Regular.otf "$NOTO_CJK_URL" "$NOTO_CJK_SHA256"
fetch NotoSansCJK-LICENSE.txt "$NOTO_CJK_LICENSE_URL" "$NOTO_CJK_LICENSE_SHA256"

sdl_stamp="$SDL3_VERSION $SDL3_SHA256"
if ! stamp_matches sdl3 "$sdl_stamp"; then
    step "Building SDL3 $SDL3_VERSION"
    rm -rf "$deps_build/SDL3-$SDL3_VERSION"
    tar -xzf "$sources/SDL3-$SDL3_VERSION.tar.gz" -C "$deps_build"
    cmake -S "$deps_build/SDL3-$SDL3_VERSION" -B "$deps_build/sdl3-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$deps_build/sdl3-build" -j "$jobs"
    cmake --install "$deps_build/sdl3-build"
    echo "$sdl_stamp" > "$deps/.sdl3.stamp"
fi

step "Configuring $RELEASE_NAME (release)"
ls "$repo_dir"/generated/*.cpp > /dev/null 2>&1 ||
    { echo "error: no generated code in $repo_dir/generated; run the game's generate step first" >&2; exit 1; }
cmake -S "$repo_dir" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORTABLEKIT_RELEASE=ON \
    -DPORTABLEKIT_FFMPEG=bundled \
    -DPORTABLEKIT_FFMPEG_DOWNLOAD_DIR="$sources" \
    -DCMAKE_PREFIX_PATH="$deps" \
    -DPSPRECOMP_GENERATED_JOBS="$jobs" | tee "$work/configure.log"
# A release without the renderer or without music would configure fine; refuse it.
for feature in "$RELEASE_TARGET: Vulkan renderer enabled" "bundled FFmpeg"; do
    if ! grep -q "$feature" "$work/configure.log"; then
        echo "error: configure did not report '$feature'" >&2
        exit 1
    fi
done

step "Building $RELEASE_TARGET"
cmake --build "$build" -j "$jobs"

step "Staging"
rm -rf "$stage"
mkdir -p "$stage/lib" "$stage/overlays" "$stage/fonts" "$stage/licenses"
install -m 755 "$build/bin/$RELEASE_TARGET" "$stage/$RELEASE_NAME"
strip --strip-unneeded "$stage/$RELEASE_NAME"
if [[ "$RELEASE_OVERLAYS" -gt 0 ]]; then
    cp "$build/bin/overlays/"*.so "$stage/overlays/"
    strip --strip-unneeded "$stage/overlays/"*.so
fi

# The libraries built above that the executable needs, directly or through
# each other, under the names the loader looks for.
# SDL3 comes from the dependency prefix, FFmpeg from the build's bin/lib.
needed() { objdump -p "$1" | awk '$1 == "NEEDED" { print $2 }'; }
pending=("$stage/$RELEASE_NAME")
while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"
    pending=("${pending[@]:1}")
    for soname in $(needed "$current"); do
        [[ -e "$stage/lib/$soname" ]] && continue
        for dir in "$build/bin/lib" "$deps/lib"; do
            [[ -e "$dir/$soname" ]] || continue
            cp -L "$dir/$soname" "$stage/lib/$soname"
            chmod 644 "$stage/lib/$soname"
            strip --strip-unneeded "$stage/lib/$soname"
            pending+=("$stage/lib/$soname")
            break
        done
    done
done

cp "$sources/NotoSansCJKjp-Regular.otf" "$stage/fonts/"
cp "$repo_dir/LICENSE" "$stage/licenses/$RELEASE_NAME-LICENSE.txt"
cp "$kit_dir/LICENSE" "$stage/licenses/PortableKit-LICENSE.txt"
cp "$repo_dir/packaging/THIRD_PARTY_NOTICES.md" "$stage/licenses/THIRD_PARTY_NOTICES.md"
cp "$deps_build/SDL3-$SDL3_VERSION/LICENSE.txt" "$stage/licenses/SDL3-LICENSE.txt"
# The FFmpeg build leaves its licence and a note of its source and configure
# line next to the libraries.
cp "$build/bin/lib/FFmpeg-COPYING.LGPLv2.1.txt" "$build/bin/lib/FFmpeg-SOURCE.txt" "$stage/licenses/"
cp "$kit_dir/third_party/imgui/LICENSE.txt" "$stage/licenses/DearImGui-LICENSE.txt"
cp "$kit_dir/third_party/tiny_aes/UNLICENSE" "$stage/licenses/tiny-AES-c-UNLICENSE.txt"
cp "$kit_dir/third_party/xxhash/LICENSE" "$stage/licenses/xxHash-LICENSE.txt"
cp "$sources/NotoSansCJK-LICENSE.txt" "$stage/licenses/NotoSansCJK-OFL.txt"

step "Checking the staged program"
# Everything must resolve from lib/ or from libraries every desktop has.
missing="$(LD_LIBRARY_PATH='' ldd "$stage/$RELEASE_NAME" | grep 'not found' || true)"
if [[ -n "$missing" ]]; then
    echo "error: unresolved libraries:" >&2
    echo "$missing" >&2
    exit 1
fi
ldd "$stage/$RELEASE_NAME" | grep "$stage/lib" || { echo "error: bundled libraries not used" >&2; exit 1; }
# No libstdc++ from the build machine is needed or exported.
if objdump -p "$stage/$RELEASE_NAME" $(find "$stage/overlays" -name '*.so') | grep -q 'NEEDED.*libstdc++'; then
    echo "error: a staged binary needs libstdc++.so" >&2
    exit 1
fi
glibc_floor="$(objdump -T "$stage/$RELEASE_NAME" "$stage/lib/"*.so* $(find "$stage/overlays" -name '*.so') |
    grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "Newest glibc symbol version needed: $glibc_floor"
echo "$glibc_floor" > "$work/stage/glibc-floor.txt"
echo "Staged $(find "$stage/overlays" -name '*.so' | wc -l) overlay libraries in $stage"
