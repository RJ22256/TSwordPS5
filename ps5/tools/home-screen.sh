#!/usr/bin/env bash
# ps5/tools/home-screen.sh <PS5_Vulkan checkout> - the home screen's assets from ps5/art (ps5/art/README.md) into
# ps5/sce_sys: icon0.png (512x512), pic0.dds / pic1.dds (3840x2160 BC7, PS5_Vulkan's prepare-assets.sh and its pinned
# encoder) and snd0.at9 (as it is). What ps5/art doesn't have stays as committed in ps5/sce_sys.
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vulkan=$(cd -- "${1:?usage: home-screen.sh <PS5_Vulkan>}" && pwd)
art=$ps5/art
pick() { for e in png jpg jpeg webp bmp tga; do [[ -f $art/$1.$e ]] && { echo "$art/$1.$e"; return 0; }; done; return 0; }
icon=$(pick icon) bg=$(pick background) launch=$(pick launch-background)
args=()
[[ -n $icon ]] && args+=(--icon "$icon")
if [[ -n $bg && -n $launch ]]; then args+=(--selection-background "$bg" --launch-background "$launch")
elif [[ -n $bg ]]; then args+=(--background "$bg"); fi
if (( ${#args[@]} )); then
    command -v ffmpeg > /dev/null || { echo "home-screen.sh: needs ffmpeg" >&2; exit 1; }
    bash "$vulkan/tools/setup-asset-dependencies.sh" > /dev/null
    bash "$vulkan/tools/prepare-assets.sh" "${args[@]}" --output-directory "$ps5/sce_sys"
    rm -f "$ps5"/sce_sys/*-source.png  # (prepare-assets' copies of the sources: ps5/art has them)
fi
if [[ -f $art/music.at9 ]]; then
    head -c 4 "$art/music.at9" | grep -q RIFF || { echo "home-screen.sh: music.at9 isn't an ATRAC9 (RIFF) file" >&2; exit 1; }
    cp "$art/music.at9" "$ps5/sce_sys/snd0.at9"
fi
echo "home screen: ${icon:+icon }${bg:+background }${launch:+launch-background }$( [[ -f $art/music.at9 ]] && echo music)"
