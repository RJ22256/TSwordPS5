#!/usr/bin/env bash
# ps5/tsword/build.sh - TSword itself as the title's eboot.bin, into dist/<TITLE_ID>/ (as WSword PS5's
# ps5/wsword/build.sh): TotK Recomp's runtime (ps5/tsword/configure.py: tsword/runtime with the console's platform
# layer, SDL3 subset, Vulkan commands, process end and - until the PS5 export - the game code's stand-ins) linked whole
# with its dependencies (ps5/tsword/deps.sh) and RADV, by the template's link (ps5/tools/link-title.sh: PS5_Vulkan's
# recipe, its CRT, the native tool that signs the eboot).
#
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan): RADV, its link recipe and tools
#   TSWORD_DEPS      the dependencies' prefix (default build/tsword-deps; built when missing)
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
sdk=$(bash "$ps5/tools/setup-sdk.sh")
deps=${TSWORD_DEPS:-$root/build/tsword-deps}
out=$root/build/tsword
work=$root/build/tsword-title
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

[[ -f $deps/lib/libopus.a && -f $deps/lib/libvpx.a && -f $deps/lib/libzstd.a ]] || bash "$ps5/tsword/deps.sh" "$sdk" "$deps"
# The PS5 export's game code as the PC will build it, tried here with stand-ins (tsword/tools/build_ps5.py --dry-run):
# what it imports goes into the title's export table, the headers it reads into the PS5 kit TSword Setup carries
dry=$root/build/tsword-dryrun
lld=$(command -v ld.lld || command -v ld.lld-18 || echo "$sdk/bin/ld.lld")
python3 "$root/tsword/tools/build_ps5.py" --dry-run --sdk "$sdk" --runtime "$root/tsword/runtime" --deps "$deps" \
    --out "$dry" --lld "$lld" > "$root/build/tsword-dryrun.log" 2>&1 || { tail -60 "$root/build/tsword-dryrun.log"; exit 1; }
grep '^dry run' "$root/build/tsword-dryrun.log"
python3 "$ps5/tsword/configure.py" --sdk "$sdk" --deps "$deps" --out "$out" --dry-run "$dry"
"$ninja" -C "$out"

mkdir -p "$work"
# Names the runtime and RADV both define (Mesa's common Vulkan runtime is vk_*: vk_sync_finish, as the runtime's
# vk_sync.c): the runtime's renamed, in its own archive's copy, to tsword_<name> - its calls follow, RADV keeps its own
radv_archive=$(find "$vulkan/.deps/native/radv-release/lib" -name 'libvulkan_radeon*.a' | head -1)
lib=$work/libtsword-linked.a
cp "$out/libtsword.a" "$lib"
comm -12 <("$sdk/bin/llvm-nm" --defined-only -g "$out/libtsword.a" 2>/dev/null | awk 'NF == 3 && $2 ~ /^[TDBR]$/ { print $3 }' | sort -u) \
         <("$sdk/bin/llvm-nm" --defined-only -g "$radv_archive" 2>/dev/null | awk 'NF == 3 && $2 ~ /^[TDBR]$/ { print $3 }' | sort -u) \
    | awk '{ print $1 " tsword_" $1 }' > "$work/renamed.txt"
if [[ -s $work/renamed.txt ]]; then
    echo "build.sh: renamed in the runtime (RADV defines them too): $(cut -d' ' -f1 "$work/renamed.txt" | tr '\n' ' ')"
    objcopy_tool=$("$sdk/bin/llvm-objcopy" --version > /dev/null 2>&1 && echo "$sdk/bin/llvm-objcopy" || command -v llvm-objcopy)
    "$objcopy_tool" --redefine-syms="$work/renamed.txt" "$lib"
fi
printf 'tsword' > "$work/samples.txt"  # (link-title.sh's list of samples: no shaders of its own to copy)
# - the runtime whole (its constructors and tables are reached only through themselves)
# - main, exit, _exit and atexit wrapped: the title ends through the shell (ps5/tsword/ps5_glue.c)
# - every libc function the runtime and its dependencies call that the platform layer replaces (ps5platform/libc.h),
#   bound to the platform's and kept local, as WSword PS5 does
platform_names=$("$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^ps5_/ { print substr($3, 5) }' | sort -u)
used=$("$sdk/bin/llvm-nm" -u "$out/libtsword.a" "$deps"/lib/*.a 2>/dev/null | awk '{ print $NF }' | sort -u)
# - the working directory kept by the title (ps5/tsword/ps5_cwd.c: a title may not chdir)
cwd_wraps="chdir getcwd fopen freopen open stat lstat mkdir rmdir remove unlink rename utime access opendir realpath mkstemp"
platform_calls=$("$sdk/bin/llvm-nm" -u "$sdk/target/lib/libps5platform.a" 2>/dev/null | awk '{ print $NF }' | sort -u)
bound=$(comm -12 <(echo "$platform_names") <(echo "$used") | comm -23 - <(echo "$platform_calls") |
    grep -vxF -f <(tr ' ' '\n' <<<"$cwd_wraps") || true)
bind_flags=""
{
    printf '{\n    local:\n'
    for name in $bound; do printf '        %s;\n' "$name"; bind_flags+=" --defsym=$name=ps5_$name"; done
    printf '};\n'
} > "$work/platform-local.map"
echo "build.sh: bound to the platform layer: $(echo $bound)"
wrap_flags=""
for name in $cwd_wraps; do wrap_flags+=" --wrap=$name"; done
export LINK_EXTRA_FLAGS="--wrap=main --wrap=exit --wrap=_exit --wrap=_Exit --wrap=atexit$wrap_flags -z nostart-stop-gc -T $ps5/tsword/sections.ld$bind_flags --version-script $work/platform-local.map"
trap '[[ -f $work/link/llvm-pie.elf ]] && "$(command -v llvm-readelf-18 || command -v llvm-readelf || echo readelf)" -lSW "$work/link/llvm-pie.elf" | grep -vE "^\s*$" | head -120' ERR
bash "$ps5/tools/link-title.sh" "$work" "$vulkan" "$sdk" \
    --whole-archive "$lib" --no-whole-archive \
    "$deps/lib/libvpx.a" "$deps/lib/libopus.a" "$deps/lib/libzstd.a"

app="$root/dist/$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")"
t=$root/tsword
# the notices of what the title carries (TotK Recomp's LICENSE and THIRD_PARTY_NOTICES)
cp "$t/LICENSE" "$app/TotK-Recomp-LICENSE.txt"
cp "$t/THIRD_PARTY_NOTICES.md" "$app/TotK-Recomp-THIRD_PARTY_NOTICES.md"
cp "$t/totk.ini.example" "$app/totk.ini.example"
rm -rf "$app/assets" "$app/shaders"  # (the template's samples' assets, not TSword's)
# The PS5 kit TSword Setup (the PC release) builds the console's game code with: the headers, what this eboot
# exports and the game code id it expects (dist/TSword-PS5-kit)
kit=$root/dist/TSword-PS5-kit
rm -rf "$kit"
mkdir -p "$kit"
cp -R "$dry/include" "$kit/include"
cp "$out/exports.txt" "$kit/exports.txt"
cp "$dry/gamecode.id" "$kit/gamecode.id"
echo "==> the PS5 kit for TSword Setup: $kit (game code id $(cat "$kit/gamecode.id"), $(wc -l < "$kit/exports.txt") exports)"
echo "==> TSword (TotK Recomp $(cat "$t/VERSION")) for the PS5: $app"
# The eboot's symbols beside it (TSword.sym), for the console's crash reports
elf=$work/link/llvm-pie.elf
"$sdk/bin/llvm-nm" -n -C --defined-only "$elf" | grep -E ' [tTwW] ' > "$app/TSword.sym"
readelf_tool=$(command -v llvm-readelf-18 || command -v llvm-readelf || echo readelf)
"$readelf_tool" -lW "$elf" | grep -E "LOAD|Entry" || true
