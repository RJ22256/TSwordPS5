#!/usr/bin/env bash
# tools/sync-tsword.sh [<totk-pc-recomp checkout>] [<commit>] - tsword/ from TotK Recomp (valentderah/totk-pc-recomp,
# MIT): the whole tree at the pinned commit (tsword/SOURCE) but its GitHub files - the runtime the PS5 title builds,
# and everything TSword Setup (the PC release, .github/workflows/pc.yml) is made of - with TSword's own changes kept
# in ps5/tsword-patches/*.patch and applied on top, and ps5/tsword-overlay/ copied over it.
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
git -C "$src" archive "$rev" | tar -x -C "$root/tsword"
rm -rf "$root/tsword/.github" "$root/tsword/.githooks"
for p in "$root"/ps5/tsword-patches/*.patch; do
    [[ -f $p ]] || continue
    git -C "$root" apply --directory=tsword "$p"
done
# the console's own files (runtime/platform/ps5.c...), over the tree as they are
cp -r "$root/ps5/tsword-overlay/." "$root/tsword/"
{
    echo "TotK Recomp (https://github.com/valentderah/totk-pc-recomp, MIT)"
    echo "commit: $(git -C "$src" rev-parse "$rev")"
    echo "date: $(git -C "$src" log -1 --format=%cI "$rev")"
    echo "subject: $(git -C "$src" log -1 --format=%s "$rev")"
} > "$root/tsword/SOURCE"
echo "tsword/: $(head -2 "$root/tsword/SOURCE" | tail -1) ($(find "$root/tsword" -type f | wc -l) files)"
