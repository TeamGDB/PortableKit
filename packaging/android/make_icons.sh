#!/usr/bin/env bash
# Makes a game's Android launcher icons from its emblem: the adaptive icon's
# foreground, background colour and monochrome layer, and legacy square and
# round icons, at every density.
#
#   make_icons.sh <emblem.svg> <game res dir> [background colour, default #130e0b]
#
# Needs rsvg-convert and ImageMagick; the game commits the PNGs, so a build
# does not. Run it again after the emblem changes.
set -euo pipefail
emblem="${1:?usage: make_icons.sh <emblem.svg> <res dir> [background]}"
res="${2:?usage: make_icons.sh <emblem.svg> <res dir> [background]}"
background="${3:-#130e0b}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

rsvg-convert -w 1024 -h 1024 "$emblem" -o "$work/emblem.png"

for pair in mdpi:1 hdpi:1.5 xhdpi:2 xxhdpi:3 xxxhdpi:4; do
    density="${pair%%:*}"
    scale="${pair##*:}"
    dir="$res/mipmap-$density"
    mkdir -p "$dir"
    layer=$(printf '%.0f' "$(echo "108 * $scale" | bc -l)")
    inner=$(printf '%.0f' "$(echo "72 * $scale" | bc -l)")
    legacy=$(printf '%.0f' "$(echo "48 * $scale" | bc -l)")
    # Adaptive foreground: the emblem inside the 72 dp the launcher's mask keeps.
    magick "$work/emblem.png" -resize "${inner}x${inner}" -background none -gravity center \
        -extent "${layer}x${layer}" "$dir/ic_launcher_foreground.png"
    # Monochrome: the emblem's shape, lighter where it is lighter, for themed icons.
    magick "$dir/ic_launcher_foreground.png" -alpha extract "$work/alpha.png"
    magick "$dir/ic_launcher_foreground.png" -alpha off -colorspace gray -level 20%,90% "$work/light.png"
    magick "$work/alpha.png" "$work/light.png" -compose multiply -composite "$work/mask.png"
    magick -size "${layer}x${layer}" xc:white "$work/mask.png" -alpha off -compose copy_opacity -composite \
        "$dir/ic_launcher_monochrome.png"
    # Legacy icons for launchers without adaptive icons.
    magick "$work/emblem.png" -resize "${legacy}x${legacy}" "$dir/ic_launcher.png"
    cp "$dir/ic_launcher.png" "$dir/ic_launcher_round.png"
done
mkdir -p "$res/values"
cat > "$res/values/ic_launcher_background.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="ic_launcher_background">$background</color>
</resources>
XML
echo "icons written to $res"
