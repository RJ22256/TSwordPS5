#!/usr/bin/env bash
# tools/sync-ultratears.sh [<ultratears checkout>] [<commit>] - ultratears/ from UltraTears (kuraeow/ultratears,
# GPL-3.0): the whole tree at the pinned commit (ultratears/SOURCE) but its GitHub files - the runtime the PS5 title
# builds and the PC builder its PS5 export runs - with TSword PS5's changes kept in ps5/ut-patches/*.patch and applied
# on top, and ps5/ut-overlay/ (the console's own files: src/runtime/platform/ps5.c...) copied over it.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=${1:-}
rev=${2:-$(sed -n 's/^commit: //p' "$root/ultratears/SOURCE" 2>/dev/null || true)}
if [[ -z $src ]]; then
    src=$(mktemp -d)
    trap 'rm -rf "$src"' EXIT
    git clone -q --filter=blob:none --no-checkout https://github.com/kuraeow/ultratears "$src"
fi
[[ -n $rev ]] || rev=$(git -C "$src" rev-parse HEAD)
rm -rf "$root/ultratears"
mkdir -p "$root/ultratears"
git -C "$src" archive "$rev" | tar -x -C "$root/ultratears"
rm -rf "$root/ultratears/.github" "$root/ultratears/.githooks"
for p in "$root"/ps5/ut-patches/*.patch; do
    [[ -f $p ]] || continue
    git -C "$root" apply --directory=ultratears "$p"
done
[[ -d $root/ps5/ut-overlay ]] && cp -r "$root/ps5/ut-overlay/." "$root/ultratears/"
{
    echo "UltraTears (https://github.com/kuraeow/ultratears, GPL-3.0)"
    echo "commit: $(git -C "$src" rev-parse "$rev")"
    echo "date: $(git -C "$src" log -1 --format=%cI "$rev")"
    echo "subject: $(git -C "$src" log -1 --format=%s "$rev")"
} > "$root/ultratears/SOURCE"
echo "ultratears/: $(head -2 "$root/ultratears/SOURCE" | tail -1) ($(find "$root/ultratears" -type f | wc -l) files)"
