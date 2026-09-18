#!/usr/bin/env bash
# Extract every code overlay from DATA.BIN and build a recompiled library for each.
#
#   build_overlays.sh [build_dir]
#
# The game loads its overlays into shared slots at run time, so each one needs
# its own corpus. This builds all of them up front instead of waiting for the
# game to reach them. Overlays that already have a library are skipped, so an
# interrupted run picks up where it stopped. Expect roughly 40 minutes for the
# whole set; small map overlays take seconds, weapon and mode tasks up to a
# minute each.
set -euo pipefail

profile_dir="$(cd "$(dirname "$0")/.." && pwd)"
repo_dir="$(cd "$profile_dir/../.." && pwd)"
build_dir="${1:-$repo_dir/out/mhp3rd}"
iso="$profile_dir/game/disc.iso"
extract_dir="$profile_dir/analysis/overlays"
library_dir="$build_dir/bin/overlays"

if [[ ! -e "$iso" ]]; then
    echo "error: $iso not found; run scripts/prepare_game.sh first" >&2
    exit 1
fi
if [[ ! -x "$build_dir/bin/MHP3rdNative" ]]; then
    echo "error: $build_dir/bin/MHP3rdNative not found; build the profile first" >&2
    exit 1
fi

mkdir -p "$extract_dir"
echo "extracting overlays from DATA.BIN into $extract_dir"
python3 "$profile_dir/tools/databin.py" "$iso" extract-overlays "$extract_dir" > /dev/null

images=("$extract_dir"/overlay_*.bin)
total=${#images[@]}
built=0
skipped=0
failed=()

for image in "${images[@]}"; do
    stem="$(basename "$image" .bin)"   # overlay_<BASE>_<name>
    rest="${stem#overlay_}"
    base="${rest%%_*}"
    name="${rest#*_}"
    if compgen -G "$library_dir/ovl${base}_${name}_*" > /dev/null; then
        skipped=$((skipped + 1))
        continue
    fi
    echo "[$((built + skipped + ${#failed[@]} + 1))/$total] $name at 0x$base"
    if python3 "$profile_dir/tools/add_overlay.py" "$build_dir" "$image" "0x$base" > /dev/null 2>&1; then
        built=$((built + 1))
    else
        failed+=("$name")
    fi
done

echo "built $built, already present $skipped, failed ${#failed[@]}, of $total"
if [[ ${#failed[@]} -gt 0 ]]; then
    printf '  failed: %s\n' "${failed[@]}"
    exit 1
fi
