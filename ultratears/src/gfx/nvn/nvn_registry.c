// The registry of NVN implementations (MacNativeGames' Mach-O sections swnvn / totknvnx) as PE or ELF sections,
// and the guest addresses the game receives for NVN names.
#include <string.h>

#include "../host/hle.h"
#include "nvn_impl.h"

#if defined(__linux__) || defined(__PROSPERO__)
// ELF: the linker gives every section named as a C identifier __start_<name> / __stop_<name> (nvn_impl.h). A PE grouped
// section's "$" order means nothing to it: with the Windows markers the registry came out empty and the game stopped at
// its first NVN call ("NVN registry"; the first start on a Steam Deck, 2026-11-01).
extern const nvn_impl __start_nvni[], __stop_nvni[], __start_nvnx[], __stop_nvnx[];
#define impls_start __start_nvni
#define impls_stop __stop_nvni
#define extras_start __start_nvnx
#define extras_stop __stop_nvnx
#define FIRST 0
#else
// PE/COFF: the linker sorts .nvni$a < .nvni$m (NVN / NVN_NOPROTO entries) < .nvni$z by the text after '$'; likewise .nvnx.
__attribute__((section(".nvni$a"), used)) static const nvn_impl impls_start[1];
__attribute__((section(".nvni$z"), used)) static const nvn_impl impls_stop[1];
__attribute__((section(".nvnx$a"), used)) static const nvn_impl extras_start[1];
__attribute__((section(".nvnx$z"), used)) static const nvn_impl extras_stop[1];
#define FIRST 1   // past the start marker
#endif

static const nvn_impl *range(const nvn_impl *a, const nvn_impl *b, unsigned *n) {
    __asm__("" : "+r"(a), "+r"(b));   // separate objects: keep the compiler from reasoning about them
    const nvn_impl *first = a + FIRST;
    while (first < b && !first->name) first++;   // (section padding)
    *n = (unsigned)(b - first);
    return first;
}
const nvn_impl *nvn_registry_impls(unsigned *n) { return range(impls_start, impls_stop, n); }
const nvn_impl *nvn_registry_extras(unsigned *n) { return range(extras_start, extras_stop, n); }

// The native implementation of an NVN name (the thunks call it).
void *nvn_impl_fn(const char *name, int extra) {
    unsigned n;
    const nvn_impl *t = extra ? nvn_registry_extras(&n) : nvn_registry_impls(&n);
    for (unsigned i = 0; i < n; i++) if (t[i].name && !strcmp(t[i].name, name)) return t[i].fn;
    LOG("NVN implementation %s missing from the registry", name);
    runtime_fail("NVN registry");
}

extern const struct { const char *name; void (*fn)(Cpu *); } nvn_thunks[];   // nvn_thunks.gen.c
extern const unsigned nvn_thunk_count;

int nvn_nop_name(const char *name);   // hle_nvn.c g_nops
static void nvn_nop_thunk(Cpu *c) { (void)c; }

// The guest address for an NVN name: its thunk's, an empty function for the GPU-only commands the null device ignores
// (hle_nvn.c g_nops), or a host stub that stops the game naming it when it is called.
/* TSword: the graphics-call profile (TOTK_NVN_PROF; =0 off). Each NVN thunk the game is given goes through a wrapper that times
   the call with the CPU's time-stamp counter (a clock read per call would cost more than many of the calls), on the
   game's main thread apart from its other threads. nvn_prof_report (from the [frametime] line, nvn/hle_nvn.c) logs the
   functions that took the most time per frame. (MacNativeGames' DM_NVN_PROF wraps the implementations in assembly, not
   ported; this wraps the thunks instead.) */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
static inline uint64_t prof_tick(void) { return __rdtsc(); }
#else
static inline uint64_t prof_tick(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#endif
__attribute__((weak)) int runtime_on_main_thread(void) { return 0; }   // (host/threads.c)
static HostFn g_prof_real[448];
static const char *g_prof_name[448];
static uint64_t g_prof_main_t[448], g_prof_main_n[448];   // (written by the main thread alone)
static _Atomic uint64_t g_prof_other_t[448], g_prof_other_n[448];
static unsigned g_prof_used;
static void prof_call(unsigned i, Cpu *c) {
    uint64_t t0 = prof_tick();
    g_prof_real[i](c);
    uint64_t d = prof_tick() - t0;
    if (runtime_on_main_thread()) { g_prof_main_t[i] += d; g_prof_main_n[i]++; }
    else { atomic_fetch_add_explicit(&g_prof_other_t[i], d, memory_order_relaxed); atomic_fetch_add_explicit(&g_prof_other_n[i], 1, memory_order_relaxed); }
}
#include "nvn_prof_wrappers.h"
// (on in TSword 0.5.8, the test build: TOTK_NVN_PROF=0 turns it off)
static int prof_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_NVN_PROF"); v = !(e && *e == '0'); } return v; }
static uint64_t prof_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static HostFn prof_wrap(const char *name, HostFn fn) {
    if (!prof_on() || g_prof_used >= NVN_PROF_SLOTS) return fn;
    unsigned i = g_prof_used++;
    g_prof_real[i] = fn; g_prof_name[i] = name;
    if (i == 0) LOG("[nvnprof] the graphics calls are timed (TOTK_NVN_PROF=0: not): a [nvnprof] line with each [frametime] line");
    return g_prof_wrap[i];
}
// the window since the last report, over `frames` presents: the calls' time on the main thread and the others, and the
// functions that took the most (ms a frame, calls a frame, the main thread's share)
void nvn_prof_report(unsigned frames) {
    static uint64_t tick0, ns0;
    static double tsc_ms;   // ms a tick: measured against the clock between reports (the first one assumes 1.6 GHz)
    if (!prof_on() || !g_prof_used || !frames) return;
    uint64_t tk = prof_tick(), ns = prof_now_ns();
    if (tick0 && tk > tick0 && ns > ns0 + 100000000ull) tsc_ms = (double)(ns - ns0) / 1e6 / (double)(tk - tick0);
    if (tsc_ms <= 0) tsc_ms = 1.0 / 1.6e6;
    tick0 = tk; ns0 = ns;
    static double mt[448], ot[448], mn[448], on[448];   // this window's snapshot, the counters cleared
    double main_all = 0, other_all = 0, calls_m = 0, calls_o = 0;
    for (unsigned i = 0; i < g_prof_used; i++) {
        mt[i] = (double)g_prof_main_t[i]; mn[i] = (double)g_prof_main_n[i]; g_prof_main_t[i] = g_prof_main_n[i] = 0;
        ot[i] = (double)atomic_exchange(&g_prof_other_t[i], 0); on[i] = (double)atomic_exchange(&g_prof_other_n[i], 0);
        main_all += mt[i]; other_all += ot[i]; calls_m += mn[i]; calls_o += on[i];
    }
    char line[1400]; size_t o = 0; line[0] = 0;
    for (int k = 0; k < 12; k++) {
        unsigned best = 0; double bt = -1;
        for (unsigned i = 0; i < g_prof_used; i++) if (mt[i] + ot[i] > bt) bt = mt[i] + ot[i], best = i;
        if (bt <= 0) break;
        int w = snprintf(line + o, sizeof line - o, "%s %s %.2f ms (%.0f calls, %.0f%% on main)", k ? ";" : "", g_prof_name[best] + 3,
                         bt * tsc_ms / frames, (mn[best] + on[best]) / frames, 100.0 * mt[best] / bt);
        if (w < 0 || (size_t)w >= sizeof line - o) break;
        o += (size_t)w; mt[best] = ot[best] = 0;
    }
    LOG("[nvnprof] graphics calls a frame: main thread %.2f ms (%.0f calls), other game threads %.2f ms (%.0f calls); the most:%s",
        main_all * tsc_ms / frames, calls_m / frames, other_all * tsc_ms / frames, calls_o / frames, line);
}

uint64_t nvn_registry_guest_address(const char *name) {
    for (unsigned i = 0; i < nvn_thunk_count; i++)
        if (!strcmp(nvn_thunks[i].name, name)) return hle_register(nvn_thunks[i].name, prof_wrap(nvn_thunks[i].name, nvn_thunks[i].fn));
    if (nvn_nop_name(name)) return hle_register(name, nvn_nop_thunk);
    return hle_register(name, NULL);
}
