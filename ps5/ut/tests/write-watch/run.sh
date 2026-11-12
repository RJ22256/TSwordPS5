#!/usr/bin/env bash
# ps5/ut/tests/write-watch/run.sh [threads] [looks] - the PS5 write watch (src/runtime/platform/ps5.c) on Linux: the
# console's protection calls stood in for by mprotect with a shadow of the GPU bits (shim.c), many threads writing a
# pool while one thread looks at it as the graphics code does. Fails if a look misses a write done before it began
# (one reported covers the writes done by the time it returned), if a file read into the pool fails, or if a
# protection change loses the GPU bits. t2: a single-threaded walk through the states.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../../.." && pwd)
u=$root/ultratears/src/runtime
deps=${UT_DEPS:-$root/build/ut-deps}
out=$(mktemp -d)
cp "$u/platform/ps5.c" "$out/ps5.c"
sed -i 's#"../platform.h"#"platform.h"#; s#"../rt.h"#"rt.h"#' "$out/ps5.c"
cat >> "$out/ps5.c" <<'X'
int wp_debug_state(uint64_t a) { WState *w = wstate(a, false); return w ? atomic_load(&w->state) * 1000 + w->streak * 100 + w->wait : -1; }
void wp_debug_dump(uint64_t a) { (void)a; }
X
cc=${CC:-clang}
$cc -O2 -g -w -D_GNU_SOURCE -include "$here/mc.h" -I"$here" -I"$u" -I"$deps/include" -c "$out/ps5.c" -o "$out/ps5.o"
$cc -O2 -g -w -I"$here" -I"$u" -c "$here/shim.c" -o "$out/shim.o"
$cc -O2 -g -w -I"$here" -I"$u" -c "$here/test.c" -o "$out/test.o"
$cc -O2 -g -w -I"$here" -I"$u" -c "$here/t2.c" -o "$out/t2.o"
$cc "$out/ps5.o" "$out/shim.o" "$out/t2.o" -o "$out/t2" -lpthread
$cc "$out/ps5.o" "$out/shim.o" "$out/test.o" -o "$out/wptest" -lpthread
"$out/t2"
"$out/wptest" "${1:-8}" "${2:-1000}"
