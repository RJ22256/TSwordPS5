#!/usr/bin/env bash
# tools/sync-tsword.sh [<totk-pc-recomp checkout>] [<commit>] - tsword/ from TotK Recomp (valentderah/totk-pc-recomp,
# MIT): its runtime/, recomp/ (the lifter the PS5 export runs on the PC) and the tools the export uses, at the pinned
# commit (tsword/SOURCE), with TSword's own changes kept in ps5/tsword-patches/*.patch and applied on top.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=${1:-}
rev=${2:-$(sed -n 's/^commit: //p' "$root/tsword/SOURCE" 2>/dev/null || true)}
if [[ -z $src ]]; then
    src=$(mktemp -d)
    trap 'rm -rf "$src"' EXIT
    git clone -q --filter=blob:none --no-checkout https://github.com/valentderah/totk-pc-recomp "$src"
fi
[[ -n $rev ]] || rev=$(git -C "$src" rev-parse HEAD)
rm -rf "$root/tsword"
mkdir -p "$root/tsword"
git -C "$src" archive "$rev" runtime recomp LICENSE THIRD_PARTY_NOTICES.md VERSION totk.ini.example \
    tools/build_game.py tools/prepare_shaders.py tools/pack_shader_cache.py tools/make_romfs_patches.py \
    tools/romfs_index.py tools/requirements.txt 2>/dev/null | tar -x -C "$root/tsword"
for p in "$root"/ps5/tsword-patches/*.patch; do
    [[ -f $p ]] || continue
    git -C "$root" apply --directory=tsword "$p"
done
{
    echo "TotK Recomp (https://github.com/valentderah/totk-pc-recomp, MIT)"
    echo "commit: $(git -C "$src" rev-parse "$rev")"
    echo "date: $(git -C "$src" log -1 --format=%cI "$rev")"
    echo "subject: $(git -C "$src" log -1 --format=%s "$rev")"
} > "$root/tsword/SOURCE"
echo "tsword/: $(head -2 "$root/tsword/SOURCE" | tail -1) ($(find "$root/tsword" -type f | wc -l) files)"
