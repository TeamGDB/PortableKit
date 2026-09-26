#!/usr/bin/env bash
# Build a PortableKit game's Linux release: the Flatpak bundle and the
# portable tarball.
#
#   release_linux.sh [--game DIR] [--version VERSION] [--jobs N]
#                    [--skip-build] [--no-flatpak] [--no-tarball]
#
# The game's checkout (--game, by default the one around the current
# directory) describes the release in packaging/release.env and brings
# packaging/THIRD_PARTY_NOTICES.md, packaging/icon.png (256 by 256 pixels
# exactly), packaging/linux/README.txt, packaging/linux/<app id>.desktop and
# packaging/linux/<app id>.metainfo.xml (with @VERSION@ and @DATE@), and its
# generated code in generated/ (its generate step makes it; the code is the
# same on every platform). This builds the executable, its overlay libraries
# and the bundled SDL3 and FFmpeg inside the
# Steam Runtime 3 "sniper" SDK container (packaging/linux/build_in_sdk.sh),
# packs both artifacts, checks that neither contains any game data, and
# prints their SHA-256 checksums. Everything lands in out/release-linux of the
# game's checkout; the artifacts in its dist/.
#
# Needs podman (or docker), flatpak with the Flathub remote, curl, ostree and
# free space for the build (about 10 GB without overlays). The first run
# takes an hour or more; later runs reuse the compiler cache and the
# dependencies.
#
#   --game DIR         the game's checkout
#   --version VERSION  name the artifacts after VERSION instead of git describe
#   --jobs N           parallel compile jobs (default 4)
#   --skip-build       package the staged build from a previous run as it is
#   --no-flatpak       do not build the Flatpak bundle
#   --no-tarball       do not build the tarball
set -euo pipefail

kit_dir="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../packaging/sources.sh
source "$kit_dir/packaging/sources.sh"

repo_dir=""
version=""
jobs=4
skip_build=0
make_flatpak=1
make_tarball=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game) repo_dir="${2:?--game needs a value}"; shift 2 ;;
        --version) version="${2:?--version needs a value}"; shift 2 ;;
        --jobs) jobs="${2:?--jobs needs a value}"; shift 2 ;;
        --skip-build) skip_build=1; shift ;;
        --no-flatpak) make_flatpak=0; shift ;;
        --no-tarball) make_tarball=0; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[[ -n "$repo_dir" ]] || repo_dir="$(git rev-parse --show-toplevel)"
repo_dir="$(cd "$repo_dir" && pwd)"
# shellcheck disable=SC1091
source "$repo_dir/packaging/release.env"
: "${RELEASE_NAME:?}" "${RELEASE_SLUG:?}" "${RELEASE_TARGET:?}" "${RELEASE_APP_ID:?}"
RELEASE_OVERLAYS="${RELEASE_OVERLAYS:-0}"
app_id="$RELEASE_APP_ID"
packaging="$repo_dir/packaging/linux"
work="${PORTABLEKIT_RELEASE_WORK:-$repo_dir/out/release-linux}"
dist="$work/dist"
stage="$work/stage/$RELEASE_SLUG"
if [[ -z "$version" ]]; then
    version="$(git -C "$repo_dir" describe --tags --always --dirty)"
    version="${version#v}"
fi
name="$RELEASE_SLUG-$version-linux-x86_64"
export SOURCE_DATE_EPOCH="$(git -C "$repo_dir" log -1 --format=%ct)"

step() { printf '\n=== %s\n' "$*"; }
fail() { echo "error: $*" >&2; exit 1; }

sha256() { sha256sum "$1" | cut -d' ' -f1; }

# The FFmpeg the build bundles, as pinned in cmake/FFmpeg.cmake.
ffmpeg_cmake="$kit_dir/cmake/FFmpeg.cmake"
cmake_value() { sed -n "s/^set($1 \(.*\))\$/\1/p" "$ffmpeg_cmake" | head -1; }
FFMPEG_VERSION="$(cmake_value PORTABLEKIT_FFMPEG_VERSION)"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_FLAGS="$(sed -n '/^set(PORTABLEKIT_FFMPEG_CONFIGURE_FLAGS/,/)/p' "$ffmpeg_cmake" |
    sed 's/^set(PORTABLEKIT_FFMPEG_CONFIGURE_FLAGS//; s/)$//' | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')"
[[ -n "$FFMPEG_VERSION" && -n "$FFMPEG_FLAGS" ]] || fail "cannot read the FFmpeg pins from $ffmpeg_cmake"
grep -qF "\"$FFMPEG_URL\"" "$ffmpeg_cmake" ||
    grep -qF 'https://ffmpeg.org/releases/ffmpeg-${PORTABLEKIT_FFMPEG_VERSION}.tar.xz' "$ffmpeg_cmake" ||
    fail "unexpected FFmpeg source URL in $ffmpeg_cmake"

# The notices must describe exactly what is bundled.
notices="$repo_dir/packaging/THIRD_PARTY_NOTICES.md"
for pinned in "SDL3 $SDL3_VERSION" "FFmpeg $FFMPEG_VERSION" "$FFMPEG_URL" "$SDL3_URL" \
              "./configure --prefix=<prefix> $FFMPEG_FLAGS"; do
    grep -qF -- "$pinned" "$notices" || fail "THIRD_PARTY_NOTICES.md does not mention: $pinned"
done

grep -qF "runtime-version: '$FLATPAK_RUNTIME_VERSION'" "$kit_dir/packaging/linux/flatpak.yml.in" ||
    fail "the Flatpak manifest does not use runtime $FLATPAK_RUNTIME_VERSION from sources.sh"

# ---------------------------------------------------------------------------
# The game data never leaves this machine. Every artifact is checked by file
# name and by content before it is published.
# ---------------------------------------------------------------------------
check_no_game_data() {
    local root="$1" what="$2" bad=()
    while IFS= read -r -d '' file; do
        local base lower
        base="$(basename "$file")"
        lower="${base,,}"
        case "$lower" in
            eboot*|*.iso|*.cso|*.pbp|*.prx|*.elf|*.ovl|*.bin|data.bin|param.sfo|umd_data*|ms0|savedata)
                bad+=("$file (name)"); continue ;;
        esac
        local pattern named=0
        for pattern in ${RELEASE_GAME_NAMES:-}; do
            # shellcheck disable=SC2254
            case "$lower" in $pattern) named=1 ;; esac
        done
        [[ $named -eq 0 ]] || { bad+=("$file (name)"); continue; }
        [[ -f "$file" && ! -L "$file" ]] || continue
        local head
        head="$(head -c 20 "$file" | od -An -tx1 | tr -d ' \n')"
        case "$head" in
            7f454c46*)
                # ELF: e_machine at offset 18, little endian. 8 is MIPS, the PSP.
                [[ "${head:36:4}" == "0800" ]] && bad+=("$file (PSP executable)") ;;
            00504250*) bad+=("$file (PBP)") ;;
            7e505350*) bad+=("$file (encrypted PSP module)") ;;
            00505346*) bad+=("$file (PARAM.SFO)") ;;
        esac
        if [[ "$(stat -c %s "$file")" -gt 32774 ]] &&
           [[ "$(dd if="$file" bs=1 skip=32769 count=5 2>/dev/null | tr -d '\0')" == "CD001" ]]; then
            bad+=("$file (disc image)")
        fi
    done < <(find "$root" -print0)
    if [[ ${#bad[@]} -gt 0 ]]; then
        printf 'error: %s contains game data:\n' "$what" >&2
        printf '  %s\n' "${bad[@]}" >&2
        exit 1
    fi
    echo "no game data in $what"
}

# ---------------------------------------------------------------------------
step "$RELEASE_NAME $version for Linux"
if [[ -n "$(git -C "$repo_dir" status --porcelain --untracked-files=no)" ]]; then
    echo "warning: the checkout has uncommitted changes; the version says -dirty" >&2
fi

if [[ $skip_build -eq 0 ]]; then
    ls "$repo_dir"/generated/*.cpp > /dev/null 2>&1 ||
        fail "no generated code in $repo_dir/generated; run the game's generate step first"

    if command -v podman > /dev/null; then container=podman
    elif command -v docker > /dev/null; then container=docker
    else fail "podman or docker is needed to run the build SDK"; fi

    # The checkouts and the work directory appear at the same paths inside
    # the container.
    mounts=(-v "$repo_dir:$repo_dir")
    [[ "$kit_dir" == "$repo_dir"/* ]] || mounts+=(-v "$kit_dir:$kit_dir")
    [[ "$work" == "$repo_dir"/* ]] || mounts+=(-v "$work:$work")
    user_args=()
    [[ "$container" == podman ]] && user_args=(--userns=keep-id) || user_args=(--user "$(id -u):$(id -g)")

    step "Building in $SDK_IMAGE"
    mkdir -p "$work/home"
    "$container" run --rm "${user_args[@]}" --security-opt label=disable \
        "${mounts[@]}" -w "$repo_dir" \
        -e HOME="$work/home" -e PORTABLEKIT_RELEASE_WORK="$work" -e PORTABLEKIT_JOBS="$jobs" \
        -e PORTABLEKIT_GAME="$repo_dir" \
        "$SDK_IMAGE" bash "$kit_dir/packaging/linux/build_in_sdk.sh"
fi

[[ -x "$stage/$RELEASE_NAME" ]] || fail "nothing staged in $stage; run without --skip-build"
overlay_count="$(find "$stage/overlays" -name '*.so' | wc -l)"
[[ "$overlay_count" -eq "$RELEASE_OVERLAYS" ]] || fail "expected $RELEASE_OVERLAYS overlay libraries, found $overlay_count"
fill() { sed -e "s/@NAME@/$RELEASE_NAME/g" -e "s/@SLUG@/$RELEASE_SLUG/g" -e "s/@APP_ID@/$app_id/g" "$1"; }
check_no_game_data "$stage" "the staged build"
rm -rf "$dist"
mkdir -p "$dist"

# ---------------------------------------------------------------------------
if [[ $make_tarball -eq 1 ]]; then
    step "Tarball"
    tree="$work/tarball/$name"
    rm -rf "$work/tarball"
    mkdir -p "$tree"
    cp -a "$stage/." "$tree/"
    fill "$kit_dir/packaging/linux/launcher.sh.in" > "$tree/$RELEASE_SLUG"
    chmod 755 "$tree/$RELEASE_SLUG"
    install -m 644 "$packaging/README.txt" "$tree/README.txt"
    tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" \
        -C "$work/tarball" -cf - "$name" | gzip -n -9 > "$dist/$name.tar.gz"
    rm -rf "$work/tarball"

    # Check what was actually packed.
    check_dir="$work/check-tarball"
    rm -rf "$check_dir"
    mkdir -p "$check_dir"
    tar -xzf "$dist/$name.tar.gz" -C "$check_dir"
    check_no_game_data "$check_dir" "$name.tar.gz"
    rm -rf "$check_dir"
fi

# ---------------------------------------------------------------------------
if [[ $make_flatpak -eq 1 ]]; then
    step "Flatpak"
    flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak --user install --noninteractive --or-update flathub org.flatpak.Builder \
        "org.freedesktop.Platform//$FLATPAK_RUNTIME_VERSION" "org.freedesktop.Sdk//$FLATPAK_RUNTIME_VERSION"

    source_dir="$work/flatpak/source"
    rm -rf "$source_dir"
    mkdir -p "$source_dir/files"
    cp -al "$stage" "$source_dir/$RELEASE_SLUG"
    fill "$kit_dir/packaging/linux/launcher.sh.in" > "$source_dir/files/$RELEASE_SLUG"
    chmod 755 "$source_dir/files/$RELEASE_SLUG"
    install -m 644 "$packaging/$app_id.desktop" "$source_dir/files/"
    install -m 644 "$repo_dir/packaging/icon.png" "$source_dir/files/$app_id.png"
    sed -e "s/@VERSION@/$version/" -e "s/@DATE@/$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%d)/" \
        "$packaging/$app_id.metainfo.xml" > "$source_dir/files/$app_id.metainfo.xml"
    fill "$kit_dir/packaging/linux/flatpak.yml.in" > "$source_dir/$app_id.yml"

    rm -rf "$work/flatpak/repo"
    flatpak run --filesystem="$work" org.flatpak.Builder \
        --user --force-clean --disable-rofiles-fuse --default-branch=stable \
        --state-dir="$work/flatpak/state" --repo="$work/flatpak/repo" \
        "$work/flatpak/build" "$source_dir/$app_id.yml"
    flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
        "$work/flatpak/repo" "$dist/$name.flatpak" "$app_id" stable

    # Check the committed tree, which is what the bundle carries.
    check_dir="$work/check-flatpak"
    rm -rf "$check_dir"
    ostree --repo="$work/flatpak/repo" checkout --user-mode "app/$app_id/x86_64/stable" "$check_dir"
    check_no_game_data "$check_dir" "$name.flatpak"
    rm -rf "$check_dir" "$work/flatpak/build"
fi

# ---------------------------------------------------------------------------
step "Checksums"
# The LGPL source of the FFmpeg the artifacts contain goes on the same release
# page (see THIRD_PARTY_NOTICES.md).
cp "$work/sources/ffmpeg-$FFMPEG_VERSION.tar.xz" "$dist/" ||
    fail "the FFmpeg source archive is not in $work/sources"
(
    cd "$dist"
    shopt -s nullglob
    sha256sum ./*.tar.gz ./*.flatpak ./*.tar.xz | sed 's# \./# #' > SHA256SUMS
    cat SHA256SUMS
)
{
    echo "$RELEASE_NAME $version for Linux (x86-64)"
    echo "Built from: $(git -C "$repo_dir" rev-parse HEAD), PortableKit $(git -C "$kit_dir" rev-parse HEAD)"
    echo "Build environment: $SDK_IMAGE"
    echo "Flatpak runtime: org.freedesktop.Platform//$FLATPAK_RUNTIME_VERSION"
    echo "Needs glibc: $(sed 's/GLIBC_//' "$work/stage/glibc-floor.txt") or newer"
    echo "SDL3 $SDL3_VERSION, FFmpeg $FFMPEG_VERSION"
    echo "FFmpeg configured with: ./configure --prefix=<prefix> $FFMPEG_FLAGS"
} > "$dist/BUILDINFO.txt"
ls -l "$dist"
