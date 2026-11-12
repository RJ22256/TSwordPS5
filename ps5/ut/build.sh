#!/usr/bin/env bash
# ps5/ut/build.sh - TSword on UltraTears as the title's eboot.bin, into dist/<TITLE_ID>/ (as WSword PS5's
# ps5/wsword/build.sh): UltraTears' runtime (ultratears/, tools/sync-ultratears.sh; ps5/ut/configure.py: its Linux
# runtime with the console's platform layer, src/runtime/platform/ps5.c) and the title's side (ps5/ut: the SDL3
# subset, the game code loader, the process end, the working directory) linked whole with the dependencies
# (ps5/ut/deps.sh) and RADV, by the template's link (ps5/tools/link-title.sh). The game code itself is the player's:
# the PC's PS5 export (ultratears/tools/build_ps5.py) builds tsgame.so from their dump with the kit written here.
#
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan): RADV, its link recipe and tools
#   UT_DEPS          the dependencies' prefix (default build/ut-deps; built when missing)
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
deps=${UT_DEPS:-$root/build/ut-deps}
out=$root/build/ut
work=$root/build/ut-title
u=$root/ultratears
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

[[ -f $u/src/runtime/platform/ps5.c ]] || bash "$root/tools/sync-ultratears.sh"
[[ -f $deps/lib/libopus.a && -f $deps/lib/libvpx.a && -f $deps/lib/libzstd.a ]] || bash "$ps5/ut/deps.sh" "$sdk" "$deps"
# What the PS5 export's game code imports beyond the runtime, and the console headers it reads (the kit's include/)
dry=$root/build/ut-dryrun
lld=$(command -v ld.lld || command -v ld.lld-18 || echo "$sdk/bin/ld.lld")
python3 "$u/tools/build_ps5.py" --dry-run --sdk "$sdk" --lld "$lld" --out "$dry"
python3 "$ps5/ut/configure.py" --sdk "$sdk" --deps "$deps" --out "$out" --need "$dry/need.txt"
"$ninja" -C "$out"
[[ $(cat "$out/gamecode.id") == $(cat "$dry/gamecode.id") ]] || { echo "build.sh: game code ids differ"; exit 1; }

mkdir -p "$work"
# Names the runtime and RADV both define (Mesa's common Vulkan runtime is vk_*): the runtime's renamed, in its own
# archive's copy, to ut_<name> - its calls follow, RADV keeps its own
radv_archive=$(find "$vulkan/.deps/native/radv-release/lib" -name 'libvulkan_radeon*.a' | head -1)
lib=$work/libut-linked.a
cp "$out/libut.a" "$lib"
comm -12 <("$sdk/bin/llvm-nm" --defined-only -g "$out/libut.a" 2>/dev/null | awk 'NF == 3 && $2 ~ /^[TDBR]$/ { print $3 }' | sort -u) \
         <("$sdk/bin/llvm-nm" --defined-only -g "$radv_archive" 2>/dev/null | awk 'NF == 3 && $2 ~ /^[TDBR]$/ { print $3 }' | sort -u) \
    | awk '{ print $1 " ut_" $1 }' > "$work/renamed.txt"
if [[ -s $work/renamed.txt ]]; then
    echo "build.sh: renamed in the runtime (RADV defines them too): $(cut -d' ' -f1 "$work/renamed.txt" | tr '\n' ' ')"
    objcopy_tool=$("$sdk/bin/llvm-objcopy" --version > /dev/null 2>&1 && echo "$sdk/bin/llvm-objcopy" || command -v llvm-objcopy)
    "$objcopy_tool" --redefine-syms="$work/renamed.txt" "$lib"
fi
printf 'tsword' > "$work/samples.txt"  # (link-title.sh's list of samples: no shaders of its own to copy)
# - the runtime whole (its constructors, HLE tables and sections are reached only through themselves)
# - main, exit, _exit and atexit wrapped: the title ends through the shell (ps5/ut/ps5_glue.c)
# - the log's writes and the environment wrapped as UltraTears' own Linux link does (logcost.c, envcache.c)
# - every libc function the runtime and its dependencies call that the platform layer replaces (ps5platform/libc.h),
#   bound to the platform's and kept local, as WSword PS5 does
platform_names=$("$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^ps5_/ { print substr($3, 5) }' | sort -u)
used=$("$sdk/bin/llvm-nm" -u "$out/libut.a" "$deps"/lib/*.a 2>/dev/null | awk '{ print $NF }' | sort -u)
# - the working directory kept by the title (ps5/ut/ps5_cwd.c: a title may not chdir)
cwd_wraps="chdir getcwd fopen freopen open stat lstat mkdir rmdir remove unlink rename utime access opendir realpath mkstemp"
ut_wraps="fprintf vfprintf fputs fputc getenv setenv unsetenv putenv"
platform_calls=$("$sdk/bin/llvm-nm" -u "$sdk/target/lib/libps5platform.a" 2>/dev/null | awk '{ print $NF }' | sort -u)
bound=$(comm -12 <(echo "$platform_names") <(echo "$used") | comm -23 - <(echo "$platform_calls") |
    grep -vxF -f <(tr ' ' '\n' <<<"$cwd_wraps $ut_wraps") || true)
bind_flags=""
{
    printf '{\n    local:\n'
    for name in $bound; do printf '        %s;\n' "$name"; bind_flags+=" --defsym=$name=ps5_$name"; done
    printf '};\n'
} > "$work/platform-local.map"
echo "build.sh: bound to the platform layer: $(echo $bound)"
wrap_flags=""
for name in $cwd_wraps $ut_wraps; do wrap_flags+=" --wrap=$name"; done
export LINK_EXTRA_FLAGS="--wrap=main --wrap=exit --wrap=_exit --wrap=_Exit --wrap=atexit$wrap_flags -z nostart-stop-gc -T $ps5/ut/sections.ld$bind_flags --version-script $work/platform-local.map"
trap '[[ -f $work/link/llvm-pie.elf ]] && "$(command -v llvm-readelf-18 || command -v llvm-readelf || echo readelf)" -lSW "$work/link/llvm-pie.elf" | grep -vE "^\s*$" | head -120' ERR
bash "$ps5/tools/link-title.sh" "$work" "$vulkan" "$sdk" \
    --whole-archive "$lib" --no-whole-archive \
    "$deps/lib/libvpx.a" "$deps/lib/libopus.a" "$deps/lib/libzstd.a"

app="$root/dist/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")"
# the notices of what the title carries (UltraTears' LICENSE, its lifter's, its third parties')
cp "$u/LICENSE" "$app/UltraTears-LICENSE.txt"
cp "$u/tools/lifter/LICENSE-LASRecomp" "$app/LASRecomp-LICENSE.txt"
rm -rf "$app/third-party-licenses" && cp -R "$u/third_party/licenses" "$app/third-party-licenses"
cp "$ps5/ut/ultratears.cfg" "$app/ultratears.cfg.example"
[[ -f $app/ultratears.cfg ]] || cp "$ps5/ut/ultratears.cfg" "$app/ultratears.cfg"
rm -rf "$app/assets" "$app/shaders"  # (the template's samples' assets, not TSword's)
# The PC's PS5 export (dist/TSword-PS5-export): build_ps5.py with the lifter and runtime headers of this UltraTears
# commit, and the kit it compiles the console's game code with - the headers, what this eboot exports and the game code
# id it expects. It runs against the player's own UltraTears game folder (its Python, clang and data).
exp=$root/dist/TSword-PS5-export
rm -rf "$exp" "$root/dist/TSword-PS5-kit"
mkdir -p "$exp/tools" "$exp/src/runtime" "$exp/kit"
cp "$u/tools/build_ps5.py" "$u/tools/gen_game.py" "$u/tools/progress.py" "$exp/tools/"
cp -R "$u/tools/lifter" "$exp/tools/" && rm -rf "$exp/tools/lifter/__pycache__"
cp "$u"/src/runtime/*.h "$exp/src/runtime/"
cp "$u/SOURCE" "$u/LICENSE" "$exp/"
cp "$ps5/ut/export/README.txt" "$ps5/ut/export/Export for PS5.bat" "$exp/"
cp -R "$dry/include" "$exp/kit/include"
cp "$out/exports.txt" "$exp/kit/exports.txt"
cp "$out/gamecode.id" "$exp/kit/gamecode.id"
[[ $(python3 "$exp/tools/build_ps5.py" --id-only) == $(cat "$exp/kit/gamecode.id") ]] ||
    { echo "build.sh: the export folder makes another game code id than the title expects"; exit 1; }
echo "==> the PC's PS5 export: $exp (game code id $(cat "$exp/kit/gamecode.id"), $(wc -l < "$exp/kit/exports.txt") exports)"
echo "==> TSword (UltraTears $(grep '^commit: ' "$u/SOURCE" | cut -c9-20)) for the PS5: $app"
# The eboot's symbols beside it (TSword.sym), for the console's crash reports
elf=$work/link/llvm-pie.elf
"$sdk/bin/llvm-nm" -n -C --defined-only "$elf" | grep -E ' [tTwW] ' > "$app/TSword.sym"
readelf_tool=$(command -v llvm-readelf-18 || command -v llvm-readelf || echo readelf)
"$readelf_tool" -lW "$elf" | grep -E "LOAD|Entry" || true
