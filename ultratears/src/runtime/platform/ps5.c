// (TSword PS5) The console's host layer (runtime/platform.h) for UltraTears, as WSword PS5 does it for WSword
// (SheikahSlate pc/runtime/platform/ps5.c): the payload SDK's ps5platform, prospero-clang, RADV on the console's GPU.
// Modelled on linux.c, with the differences the console makes:
//  - guest memory is direct memory: a title's anonymous mappings come from its small flexible budget, so the guest
//    window is a reserved virtual range (ps5platform/shm.h) backed in 64 KiB units on first touch (the crash handler)
//    and before the kernel itself writes there (plat_commit: file reads)
//  - the write watch (Windows' GetWriteWatch, Linux's userfaultfd: the console has neither) by page protection: a page
//    the graphics code has looked at is made read-only, keeping its GPU access, and its first write faults, marks it
//    written and makes it writable again (below, "write watch")
//  - thread stacks in direct memory too (ut_pthread_create below; the runtime is compiled with pthread_create named so)
//  - no child processes, no re-exec, no terminal: stderr goes to ultratears.log in the title folder
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include "../platform.h"
#include "../rt.h"

#define TITLE_DIR "/app0"
#define UNIT 0x10000ull

// ---- guest address space
// The window is reserved whole where the console allows it, else as the parts the runtime uses (host/svc.c: the stack
// and heap block, the module block, the alias region). A bit per 64 KiB unit says which are backed: a fault on one that
// isn't is a first touch, to back read/write; a fault on one that is was a protection the game set, or a real crash.
static uint64_t g_lo, g_hi;
static _Atomic uint64_t *g_backed;
static struct { uint64_t lo, hi; } g_parts[8];
static int g_nparts;

static bool in_window(uint64_t a) {
    if (!g_backed || a < g_lo || a >= g_hi) return false;
    for (int i = 0; i < g_nparts; i++)
        if (a >= g_parts[i].lo && a < g_parts[i].hi) return true;
    return false;
}

static bool unit_backed(uint64_t a) {
    uint64_t u = (a - g_lo) / UNIT;
    return atomic_load_explicit(&g_backed[u >> 6], memory_order_acquire) >> (u & 63) & 1;
}

static void mark(uint64_t a, uint64_t e, bool on) {
    for (uint64_t u = (a - g_lo) / UNIT, end = (e - g_lo + UNIT - 1) / UNIT; u < end; u++) {
        if (on) atomic_fetch_or_explicit(&g_backed[u >> 6], 1ull << (u & 63), memory_order_release);
        else atomic_fetch_and_explicit(&g_backed[u >> 6], ~(1ull << (u & 63)), memory_order_release);
    }
}

static pthread_mutex_t g_commit_lock = PTHREAD_MUTEX_INITIALIZER;

static bool backing(uint64_t a, uint64_t size) {
    int r = ps5_vrange_commit((void *)(uintptr_t)a, size, PS5_SHM_READ | PS5_SHM_WRITE);
    if (r) {
        fprintf(stderr, "[ps5] guest memory %llx+%llx: no direct memory for it (%x)\n", (unsigned long long)a,
                (unsigned long long)size, r);
        return false;
    }
    mark(a & ~(UNIT - 1), a + size, true);
    return true;
}

// A first touch of guest memory (the crash handler): its unit read/write, as the PC's lazily backed window gives it.
static bool lazy_commit(uint64_t fa) {
    if (!in_window(fa) || unit_backed(fa)) return false;
    pthread_mutex_lock(&g_commit_lock);
    bool ok = unit_backed(fa) || backing(fa & ~(UNIT - 1), UNIT);
    pthread_mutex_unlock(&g_commit_lock);
    return ok;
}

static void wp_forget(uint64_t lo, uint64_t hi);
static void wp_unprotect(uint64_t lo, uint64_t hi);
static void wp_game(uint64_t lo, uint64_t hi, bool writable);
static bool wp_fault(uint64_t fa, bool write);

static bool reserve_part(uint64_t lo, uint64_t hi) {
    int r = ps5_vrange_reserve_at((void *)(uintptr_t)lo, hi - lo);
    if (r) {
        fprintf(stderr, "[ps5] guest window part %llx-%llx: not reserved (%x)\n", (unsigned long long)lo,
                (unsigned long long)hi, r);
        return false;
    }
    g_parts[g_nparts].lo = lo, g_parts[g_nparts].hi = hi, g_nparts++;
    return true;
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    g_backed = calloc((hi - lo) / UNIT / 64 + 1, sizeof *g_backed);
    if (!g_backed) return false;
    g_lo = lo, g_hi = hi;
    if (reserve_part(lo, hi)) return true;
    // the parts (rt.h, host/svc.c): stack and heap block (6 GiB and its texture heap extra), modules, alias region
    g_nparts = 0;
    bool ok = reserve_part(lo, lo + (12ull << 30)) && reserve_part(MAIN_BASE, HLE_HEAP + HLE_HEAP_SIZE) &&
              reserve_part(0x7000000000ull, 0x7000000000ull + (64ull << 30));
    if (!ok) fprintf(stderr, "[ps5] the guest window couldn't be reserved\n");
    return ok;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_nparts; i++) {
        uint64_t a = lo > g_parts[i].lo ? lo : g_parts[i].lo, e = hi < g_parts[i].hi ? hi : g_parts[i].hi;
        if (a < e && ps5_vrange_decommit((void *)(uintptr_t)a, e - a)) return false;
        if (a < e) mark(a, e, false), wp_forget(a, e);
    }
    return true;
}

// Backs [addr, addr+size) before the kernel writes there (a file read into a game buffer fails with EFAULT on memory
// that isn't, instead of faulting), and before the GPU is given it (an import: RADV gives the GPU access to mapped
// memory only). Only units not backed yet: committing a backed one would reset a protection the game set on it.
void plat_commit(uint64_t addr, uint64_t size) {
    if (!size || !in_window(addr)) return;
    uint64_t a = addr & ~(UNIT - 1), e = (addr + size + UNIT - 1) & ~(UNIT - 1);
    pthread_mutex_lock(&g_commit_lock);
    while (a < e) {
        if (!in_window(a) || unit_backed(a)) { a += UNIT; continue; }
        uint64_t run = a;
        while (run < e && in_window(run) && !unit_backed(run)) run += UNIT;
        backing(a, run - a);
        a = run;
    }
    pthread_mutex_unlock(&g_commit_lock);
    wp_unprotect(addr, addr + size);   // (the kernel's writes don't fault, and RADV's import makes the pages writable)
}

void plat_memory_map(void) {}

// ---- write watch
// What GetWriteWatch is on Windows (gfx/nvn/nvn_dirty_win.c reads it through plat_written_pages): the pages of a range
// written since its last look, reset by the look. The console has no such thing, so it is made of page protection, as
// emulators track guest writes: a page of a range looked at is made read-only (sceKernelMprotect, the CPU's write taken
// away and everything else kept: RADV's import gave it GPU access, radv_ps5_memory_grant_gpu), its first write faults
// (crash_handler -> wp_fault), which marks it written and makes it writable again. Without a write watch every texture
// was compared by content every other submission (vkb_tex.c tex_content_changed, 16 MiB a submission) and the draws'
// index buffers and the GPU's write-backs were taken as changed every time.
//   - pages are the kernel's, 16 KiB (reported as such: *page)
//   - a page's state and its protection change together, under the page's own lock (a byte; the fault handler takes it
//     too, so it spins and yields, never sleeps): clean <=> read-only, written <=> writable. A look reads a clean page's
//     state without the lock, so a page is marked written before it is made writable, and made read-only before it is
//     marked clean. Without it a fault handler
//     that made a page writable just after a look protected it left it writable and clean for a moment, and a write
//     then was reported a look late (a test with 16 writing threads, 2026-11-12)
//   - a write that lands while a look is protecting the page it reported is that look's: the caller reads the memory
//     after the look (an upload, a copy), so it has it
//   - the kernel's own writes don't fault but fail (EFAULT): before them the runtime calls plat_commit (host/fs.c,
//     guest_prepare_write), which makes the range writable and written for a few looks; so does an import of game
//     memory (backend_vk.c import_host), which RADV makes writable for the CPU and the GPU
//   - a page found written HOT_AFTER looks in a row (a buffer the game writes every frame) stays writable and is reported
//     written at every look (as with no watch) for a while, then tried again, waiting longer each time: a fault per page
//     per frame would cost more than it saves
//   - the game's own protection (plat_protect) is left alone: such a page is reported written at every look
//   - a protection change that fails turns the watch off (all writable again, -1 from then on: the content checks)
// TOTK_WRITE_WATCH=0 in ultratears.cfg (or the environment): off.
#define WPAGE 0x4000ull
#define WCHUNK_SHIFT 30   // the states in 1 GiB chunks, made when a range there is first looked at
enum { W_OFF, W_DIRTY, W_CLEAN, W_HOT, W_GAME };   // OFF: not watched (writable); GAME: the game's protection
enum { HOT_AFTER = 3, HOT_WAIT = 4, HOT_WAIT_MAX = 240, HOT_STRIKES_MAX = 6 };
typedef struct { _Atomic uint8_t lock, state; uint8_t streak, wait, strikes; } WState;
#define WCHUNK_PAGES ((1ull << WCHUNK_SHIFT) / WPAGE)
#define CPU_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
static _Atomic(WState *) *g_wchunks;
static uint64_t g_nwchunks;
static _Atomic int g_wp = -1;   // -1: not decided yet, 0: off, 1: on
static pthread_mutex_t g_wp_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_wp_faults, g_wp_protects, g_wp_looks, g_wp_clean, g_wp_hot, g_wp_log_ns;

static WState *wstate(uint64_t a, bool make) {
    if (!g_wchunks || a < g_lo || a >= g_hi) return NULL;
    uint64_t c = (a - g_lo) >> WCHUNK_SHIFT;
    WState *w = atomic_load_explicit(&g_wchunks[c], memory_order_acquire);
    if (!w && make) {
        pthread_mutex_lock(&g_wp_lock);
        w = atomic_load(&g_wchunks[c]);
        if (!w && (w = calloc(WCHUNK_PAGES, sizeof *w))) atomic_store_explicit(&g_wchunks[c], w, memory_order_release);
        pthread_mutex_unlock(&g_wp_lock);
    }
    return w ? &w[((a - g_lo) & ((1ull << WCHUNK_SHIFT) - 1)) / WPAGE] : NULL;
}

// The page's lock: held only around its state and protection change (no fault can come while it is held: nothing
// writes the page then but other threads, which wait)
static void wlock(WState *w) {
    for (int n = 0; atomic_exchange_explicit(&w->lock, 1, memory_order_acquire); n++)
        if (n > 64) sched_yield();
}
static void wunlock(WState *w) { atomic_store_explicit(&w->lock, 0, memory_order_release); }

// A state change, with the counts of read-only and left-writable pages
static void wset(WState *w, uint8_t st) {
    uint8_t was = atomic_exchange(&w->state, st);
    if (was == st) return;
    if (was == W_CLEAN) atomic_fetch_sub(&g_wp_clean, 1);
    if (was == W_HOT) atomic_fetch_sub(&g_wp_hot, 1);
    if (st == W_CLEAN) atomic_fetch_add(&g_wp_clean, 1);
    if (st == W_HOT) atomic_fetch_add(&g_wp_hot, 1);
}

static bool wp_on(void) {
    if (g_wp < 0) {
        pthread_mutex_lock(&g_wp_lock);
        if (g_wp < 0) {
            const char *e = getenv("TOTK_WRITE_WATCH");
            g_nwchunks = g_backed ? ((g_hi - g_lo) >> WCHUNK_SHIFT) + 1 : 0;
            // (0.4.5) off unless TOTK_WRITE_WATCH=1: every run with it froze the console (the game stopped after its first
            // frame, and the console when the title was closed - protection changes on memory the GPU has imported are
            // the likeliest cause); without it textures are compared by content, as up to 0.4.0
            bool want = e && !strcmp(e, "1");
            g_wchunks = g_nwchunks && want ? calloc(g_nwchunks, sizeof *g_wchunks) : NULL;
            fprintf(stderr, "[ps5] write watch: %s\n", g_wchunks ? "on (page protection, TOTK_WRITE_WATCH=1)" :
                    !want ? "off: textures are compared by content (TOTK_WRITE_WATCH=1: on)" : "off (no memory)");
            atomic_store(&g_wp, g_wchunks != NULL);
        }
        pthread_mutex_unlock(&g_wp_lock);
    }
    return atomic_load(&g_wp) > 0;
}

// The protection of the page at a, as the kernel has it (0: unknown)
static uint32_t prot_of(uint64_t a) {
    void *s, *e;
    uint32_t prot = 0;
    return sceKernelQueryMemoryProtection((void *)(uintptr_t)a, &s, &e, &prot) ? 0 : prot;
}

// The page at p (its lock held) writable, or read-only; its other protection (GPU access) kept. 0: done
static int set_writable(uint64_t p, bool on) {
    uint32_t prot = prot_of(p);
    if (!prot) return -1;
    uint32_t want = on ? prot | CPU_RW : (prot | PS5_KERNEL_PROT_CPU_READ) & ~(uint32_t)PS5_KERNEL_PROT_CPU_WRITE;
    return want == prot ? 0 : sceKernelMprotect((void *)(uintptr_t)p, WPAGE, (int)want);
}

static void wp_off(const char *why, uint64_t a, int r) {
    int on = 1;
    if (!atomic_compare_exchange_strong(&g_wp, &on, 0)) return;
    fprintf(stderr, "[ps5] write watch: off (%s at %llx: %x); textures are compared by content from now on\n", why,
            (unsigned long long)a, r);
    for (uint64_t c = 0; c < g_nwchunks; c++) {   // every page it protected writable again
        WState *w = atomic_load(&g_wchunks[c]);
        for (uint64_t i = 0; w && i < WCHUNK_PAGES; i++) {
            if (atomic_load(&w[i].state) != W_CLEAN) continue;
            wlock(&w[i]);
            if (atomic_load(&w[i].state) == W_CLEAN) set_writable(g_lo + (c << WCHUNK_SHIFT) + i * WPAGE, true), wset(&w[i], W_OFF);
            wunlock(&w[i]);
        }
    }
}

static void wp_log(void) {
    uint64_t now = plat_now_ns(), last = atomic_load_explicit(&g_wp_log_ns, memory_order_relaxed);
    if (now - last < 10000000000ull || !atomic_compare_exchange_strong(&g_wp_log_ns, &last, now)) return;
    static uint64_t pf, pp, pl;
    uint64_t f = atomic_load(&g_wp_faults), pr = atomic_load(&g_wp_protects), l = atomic_load(&g_wp_looks);
    if (last) {
        double s = (now - last) / 1e9;
        fprintf(stderr, "[ps5] write watch: %.0f writes caught/s, %.0f pages protected/s, %.0f looks/s; %llu MiB read-only, "
                        "%llu MiB left writable (written at every look)\n", (f - pf) / s, (pr - pp) / s, (l - pl) / s,
                (unsigned long long)(atomic_load(&g_wp_clean) * WPAGE >> 20), (unsigned long long)(atomic_load(&g_wp_hot) * WPAGE >> 20));
    }
    pf = f, pp = pr, pl = l;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!wp_on() || !size || max <= 0 || addr < g_lo || addr + size > g_hi) return -1;
    *page = WPAGE;
    atomic_fetch_add_explicit(&g_wp_looks, 1, memory_order_relaxed);
    wp_log();
    int n = 0;
    for (uint64_t p = addr & ~(WPAGE - 1), end = addr + size; p < end; p += WPAGE) {
        if (!in_window(p) || !unit_backed(p)) continue;   // (not backed: nothing written there)
        WState *w = wstate(p, true);
        if (!w) { wp_off("no memory for the page states", p, 0); return -1; }
        uint8_t st = atomic_load(&w->state);
        if (st == W_CLEAN) { w->streak = 0; continue; }   // (unlocked: a write makes it written under the lock)
        if (n == max) break;   // (the caller looks again from here)
        pages[n++] = p;
        if (st == W_GAME) continue;
        wlock(w);
        st = atomic_load(&w->state);
        bool protect = false;
        if (st == W_DIRTY && ++w->streak >= HOT_AFTER) {   // written at every look: left writable for a while
            int k = w->strikes < HOT_STRIKES_MAX ? w->strikes : HOT_STRIKES_MAX;
            w->wait = (uint8_t)(HOT_WAIT << k < HOT_WAIT_MAX ? HOT_WAIT << k : HOT_WAIT_MAX);
            if (w->strikes < 255) w->strikes++;
            w->streak = 0;
            wset(w, W_HOT);
        } else if (st == W_DIRTY || st == W_OFF) {
            protect = true;
        } else if (st == W_HOT && (!w->wait || !--w->wait)) {   // tried again
            w->streak = 0;
            protect = true;
        }
        int r = protect ? set_writable(p, false) : 0;
        if (protect && !r) wset(w, W_CLEAN), atomic_fetch_add_explicit(&g_wp_protects, 1, memory_order_relaxed);
        wunlock(w);
        if (r) { wp_off("a protection change", p, r); return -1; }
    }
    return atomic_load(&g_wp) > 0 ? n : -1;
}

// A write fault (crash_handler) on a page the watch protected: written, and writable again. A fault on a page that
// is writable by now is another thread's fault having come first: the write is only tried again (the same address
// faulting over and over on a writable page is a real fault). Async-signal-safe: atomics, the page's lock (spinning)
// and system calls; no thread-local state (the title's TLS is emulated, which isn't safe in a signal handler).
static bool wp_fault(uint64_t fa, bool write) {
    if (atomic_load(&g_wp) <= 0 || !write) return false;
    WState *w = wstate(fa, false);
    if (!w) return false;
    uint64_t p = fa & ~(WPAGE - 1);
    wlock(w);
    uint8_t st = atomic_load(&w->state);
    int r = -1;
    if (st == W_CLEAN) {   // written first: a look reads the state without the lock, and a write may follow the moment
                           // the page is writable
        wset(w, W_DIRTY);
        if ((r = set_writable(p, true))) wset(w, W_CLEAN);
        else atomic_fetch_add_explicit(&g_wp_faults, 1, memory_order_relaxed);
    }
    uint32_t prot = st == W_CLEAN ? 0 : prot_of(p);
    wunlock(w);
    if (st == W_CLEAN) return r == 0;
    if (st == W_GAME || !(prot & PS5_KERNEL_PROT_CPU_WRITE)) return false;   // read-only, not by the watch: not ours
    static _Atomic uint64_t last;
    static _Atomic int again;
    if (atomic_exchange(&last, fa) == fa) {
        if (atomic_fetch_add(&again, 1) > 100000) { atomic_store(&again, 0); return false; }
    } else {
        atomic_store(&again, 0);
    }
    return true;
}

// [lo, hi) writable and written (plat_commit: before the kernel or the GPU driver writes it), and kept so for a few
// looks (W_HOT), so that a look on another thread doesn't protect it again before the kernel's write (that would fail,
// EFAULT, and host/fs.c tries again: rarely needed, then)
static void wp_unprotect(uint64_t lo, uint64_t hi) {
    if (atomic_load(&g_wp) <= 0) return;
    for (uint64_t p = lo & ~(WPAGE - 1); p < hi; p += WPAGE) {
        WState *w = wstate(p, false);
        if (!w) { p = (((p >> WCHUNK_SHIFT) + 1) << WCHUNK_SHIFT) - WPAGE; continue; }   // (no chunk: nothing protected)
        uint8_t st = atomic_load(&w->state);
        if (st == W_GAME || st == W_OFF) continue;
        wlock(w);
        st = atomic_load(&w->state);
        if (st == W_CLEAN || st == W_DIRTY || st == W_HOT) {
            w->wait = HOT_WAIT;
            wset(w, W_HOT);   // (before it is writable: as in wp_fault)
            if (st == W_CLEAN) set_writable(p, true);
        }
        wunlock(w);
    }
}

// The game sets the protection of [lo, hi) itself (plat_protect): left to it (writable: watched again from the next look)
static void wp_game(uint64_t lo, uint64_t hi, bool writable) {
    if (atomic_load(&g_wp) <= 0) return;
    for (uint64_t p = lo & ~(WPAGE - 1); p < hi; p += WPAGE) {
        WState *w = wstate(p, false);
        if (!w) { p = (((p >> WCHUNK_SHIFT) + 1) << WCHUNK_SHIFT) - WPAGE; continue; }
        wlock(w);
        wset(w, writable ? W_OFF : W_GAME);
        wunlock(w);
    }
}

// Decommitted (plat_reset_guest): nothing there
static void wp_forget(uint64_t lo, uint64_t hi) {
    if (atomic_load(&g_wp) <= 0) return;
    for (uint64_t p = lo & ~(WPAGE - 1); p < hi; p += WPAGE) {
        WState *w = wstate(p, false);
        if (!w) { p = (((p >> WCHUNK_SHIFT) + 1) << WCHUNK_SHIFT) - WPAGE; continue; }
        wlock(w);
        wset(w, W_OFF);
        w->streak = w->wait = w->strikes = 0;
        wunlock(w);
    }
}

void plat_watch_freeze_stats(uint64_t *frozen_mib, uint64_t *thaws, uint64_t *skipped_mib) {
    *frozen_mib = *thaws = *skipped_mib = 0;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? PS5_SHM_READ : PS5_SHM_READ | PS5_SHM_WRITE;
    if (in_window(addr)) {   // backed first (whole units read/write), then the pages' protection
        plat_commit(addr, size);
        wp_game(addr, addr + size, prot == PLAT_PROT_RW);
        return ps5_vrange_commit((void *)(uintptr_t)addr, size, p) ? -1 : 0;
    }
    return mprotect((void *)(uintptr_t)addr, size,
                    prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE);
}

// A file mapped read-only (the shader pack, the BC7 cache: gfx/vkb_shpack.c, vkb_bc7.c).
const void *plat_map_file(const char *path, uint64_t len, uint64_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (!len) len = fstat(fd, &st) ? 0 : (uint64_t)st.st_size;
    void *p = len ? mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = len;
    return p;
}
void plat_unmap_file(const void *p, uint64_t size) { munmap((void *)p, size); }

// ---- thread stacks
// Every thread the runtime starts gets its stack in direct memory (the runtime is compiled with pthread_create named
// ut_pthread_create: ps5/ut/configure.py): asked-for sizes (8 MiB for each guest thread, host/threads.c) from the
// flexible budget ran out after a few dozen threads (TSword PS5, v0.3.7). Placed from 0xA0_0000_0000, above the guest's
// address space and the game code, where the kernel reserves at the hint or moves upward (ps5platform docs/PROBE.md).
// The stack of a thread that ended is used again for a new one once it is surely off it (a second later).
#define STACK_AREA ((void *)0xA000000000ull)
#define STACK_DEFAULT (8ull << 20)   // Linux's default, which the runtime's threads without a size were made with
#define STACK_GUARD 0x10000ull
typedef struct StackRec { struct StackRec *next; void *base; size_t size; uint64_t free_ns; } StackRec;
static StackRec *g_free_stacks;
static pthread_mutex_t g_stack_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned g_stacks_live, g_stacks_made;

static void *stack_take(size_t size) {
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_stack_lock);
    for (StackRec **pp = &g_free_stacks; *pp; pp = &(*pp)->next)
        if ((*pp)->size == size && now - (*pp)->free_ns > 1000000000ull) {
            StackRec *r = *pp;
            *pp = r->next;
            pthread_mutex_unlock(&g_stack_lock);
            void *b = r->base;
            free(r);
            return b;
        }
    pthread_mutex_unlock(&g_stack_lock);
    void *b = NULL;
    for (int attempt = 0; attempt < 2 && !b; attempt++) {
        if (ps5_vrange_reserve(size + STACK_GUARD, attempt ? NULL : STACK_AREA, 0x10000, &b)) { b = NULL; continue; }
        if (ps5_vrange_commit((char *)b + STACK_GUARD, size, PS5_SHM_READ | PS5_SHM_WRITE)) {
            ps5_vrange_release(b, size + STACK_GUARD);
            b = NULL;
        }
    }
    if (b) atomic_fetch_add(&g_stacks_made, 1);
    return b ? (char *)b + STACK_GUARD : NULL;
}

static void stack_give(void *base, size_t size) {
    StackRec *r = malloc(sizeof *r);
    if (!r) return;   // (kept: a leak is better than a stack in use given out)
    *r = (StackRec){NULL, base, size, plat_now_ns()};
    pthread_mutex_lock(&g_stack_lock);
    r->next = g_free_stacks, g_free_stacks = r;
    pthread_mutex_unlock(&g_stack_lock);
}

typedef struct { void *(*fn)(void *); void *arg; void *stack; size_t size; } ThreadStart;
static void *thread_start(void *p) {
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    void *r = s.fn(s.arg);
    atomic_fetch_sub(&g_stacks_live, 1);
    stack_give(s.stack, s.size);   // (its reuse waits a second: this thread is still on it until it has ended)
    return r;
}

int ut_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    size_t size = 0;
    int detached = PTHREAD_CREATE_JOINABLE;
    if (attr) pthread_attr_getstacksize(attr, &size), pthread_attr_getdetachstate(attr, &detached);
    if (size < STACK_DEFAULT) size = STACK_DEFAULT;
    size = (size + UNIT - 1) & ~(UNIT - 1);
    ThreadStart *s = malloc(sizeof *s);
    void *stack = s ? stack_take(size) : NULL;
    if (!stack) {
        free(s);
        fprintf(stderr, "[ps5] a thread's stack (%zu MiB) has no direct memory: the system's\n", size >> 20);
        return pthread_create(th, attr, fn, arg);
    }
    *s = (ThreadStart){fn, arg, stack, size};
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, detached);
    pthread_attr_setstack(&a, stack, size);
    int r = pthread_create(th, &a, thread_start, s);
    pthread_attr_destroy(&a);
    if (r) {
        fprintf(stderr, "[ps5] pthread_create: error %d (%u thread stacks made, %u live)\n", r,
                atomic_load(&g_stacks_made), atomic_load(&g_stacks_live));
        stack_give(stack, size);
        free(s);
        return r;
    }
    atomic_fetch_add(&g_stacks_live, 1);
    return 0;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {   // (relative sleeps, measured again after an interruption)
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t d = t - now;
        struct timespec ts = {.tv_sec = (time_t)(d / 1000000000u), .tv_nsec = (long)(d % 1000000000u)};
        nanosleep(&ts, NULL);
    }
}

void plat_thread_boost(void) {}
void plat_thread_background(void) {}
void plat_thread_nice(int n) { (void)n; }

// The time-constraint threads of the PC (macOS's real-time policy: the game's main thread at its acquire, the
// translation thread). Off on the console unless TOTK_PS5_REALTIME=1: as SCHED_FIFO the game's main thread froze the
// whole console twice right after it was set (2026-11-13, 0.4.2 and 0.4.3: the line just before both freezes) - a
// FIFO thread spinning on a lock never lets the lock's owner on its core run. WSword runs at normal priority.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    static _Atomic int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_PS5_REALTIME");
        int v = e && *e == '1';
        int was = -1;
        if (atomic_compare_exchange_strong(&on, &was, v))
            fprintf(stderr, "[ps5] real-time threads: %s\n", v ? "on (TOTK_PS5_REALTIME=1)" : "off, normal priority (TOTK_PS5_REALTIME=1: on)");
    }
    if (!on) return;
    struct sched_param sp = {.sched_priority = 1};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   // (refused quietly where a title may not)
}

// Guest memory that may have been released: through the mapping state, never by faulting (the console ends a title
// that reads execute-only memory; a read of unbacked guest memory would back it instead of failing).
int plat_safe_read(void *dst, const void *src, size_t n) {
    uint64_t a = (uint64_t)(uintptr_t)src;
    if (in_window(a)) {
        for (uint64_t u = a & ~(UNIT - 1); u < a + n; u += UNIT)
            if (!in_window(u) || !unit_backed(u)) return 0;
        memcpy(dst, src, n);
        return 1;
    }
    return 0;
}

// ---- process
static char g_log_path[1024];

// From the start (a constructor): what the title says before the runtime's main - where the game files are, the game
// code's loading - goes into the log too (the kernel log only, else, which a player can't read).
__attribute__((constructor(101))) static void log_to_title_folder(void) {
    snprintf(g_log_path, sizeof g_log_path, TITLE_DIR "/ultratears.log");
    rename(g_log_path, TITLE_DIR "/ultratears-previous.log");
    if (!freopen(g_log_path, "w", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fileno(stderr), STDOUT_FILENO);   // (0.4.11: the log's own descriptor; freopen need not keep it at 2)
}

void plat_debug_console(int debug) { (void)debug; }

void plat_init(void) {
    fprintf(stderr, "[ps5] UltraTears for the PS5 (TSword PS5): log %s\n", g_log_path);
}

bool plat_exe_path(char *buf, size_t size) {
    snprintf(buf, size, TITLE_DIR "/eboot.bin");
    return true;
}

void plat_exec(char *const argv[]) { (void)argv; }   // (no re-exec on the console: the caller carries on)

// ---- diagnostics
// A title carries no symbol table to answer dladdr from: TSword.sym beside eboot.bin does ("address kind name" per
// line, as linked: ps5/ut/build.sh), read once and slid by where the title was loaded (plat_symbol's own address).
static struct { uint64_t addr; char *name; } *g_syms;
static size_t g_nsyms;

static void syms_load(void) {
    static bool tried;
    if (tried) return;
    tried = true;
    FILE *f = fopen(TITLE_DIR "/TSword.sym", "r");
    if (!f) return;
    size_t cap = 0;
    char line[600];
    int64_t slide = 0;
    bool slid = false;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a;
        char kind, name[512];
        if (sscanf(line, "%llx %c %511[^\n]", &a, &kind, name) != 3) continue;
        if (g_nsyms == cap) {
            cap = cap ? cap * 2 : 16384;
            void *n = realloc(g_syms, cap * sizeof *g_syms);
            if (!n) break;
            g_syms = n;
        }
        if (!slid && !strcmp(name, "plat_symbol")) slide = (int64_t)(uintptr_t)plat_symbol - (int64_t)a, slid = true;
        g_syms[g_nsyms].addr = a;
        g_syms[g_nsyms++].name = strdup(name);
    }
    fclose(f);
    for (size_t i = 0; i < g_nsyms; i++) g_syms[i].addr += (uint64_t)slide;   // (sorted already: nm -n)
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    syms_load();
    size_t lo = 0, hi = g_nsyms;
    while (lo < hi) {   // the last symbol at or below pc
        size_t mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - g_syms[lo - 1].addr > 0x100000) return NULL;
    if (offset) *offset = pc - g_syms[lo - 1].addr;
    return g_syms[lo - 1].name;
}

// Fatal error: a notification on the console (the SDL subset shows it); the details are in the log.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    void runtime_log_drain(unsigned ms);   // (0.4.10) the log's writer thread: its last lines onto the file first
    runtime_log_drain(1000);
    fflush(stderr);
    void tsword_fatal_notice(void);   // ps5/tsword/gamecode.c: the reason as the TV's notification
    tsword_fatal_notice();
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);   // runtime/dispatch.c

void plat_print_backtrace(void) {
    // Frame pointers: the runtime and the game code keep them (-fno-omit-frame-pointer in the PS5 builds)
    uint64_t *fp = __builtin_frame_address(0);
    for (int i = 0; i < 64 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uint64_t pc = fp[1], off = 0;
        if (!pc) break;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s ? s : "?", (unsigned long long)off);
        uint64_t *next = (uint64_t *)(uintptr_t)fp[0];
        if (next <= fp) break;
        fp = next;
    }
}

// (0.4.14) The stall report: another thread's stack. One signal; its handler walks its own frame pointers from the
// interrupted registers and prints them, then the asker goes on (it waits at most half a second).
static _Atomic int g_stack_done;
static void stack_handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)si;
    ucontext_t *uc = ctx;
    uint64_t pc = (uint64_t)uc->uc_mcontext.mc_rip, off = 0;
    const char *s = plat_symbol(pc, &off);
    if (!s) s = rt_host_fn_name(pc, &off);
    fprintf(stderr, "[stall]     at %016llx %s+%llx\n", (unsigned long long)pc, s ? s : "?", (unsigned long long)off);
    uint64_t *fp = (uint64_t *)(uintptr_t)uc->uc_mcontext.mc_rbp;
    for (int i = 0; i < 24 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uint64_t ret = fp[1];
        if (!ret) break;
        off = 0;
        s = plat_symbol(ret, &off);
        if (!s) s = rt_host_fn_name(ret, &off);
        fprintf(stderr, "[stall]     from %016llx %s+%llx\n", (unsigned long long)ret, s ? s : "?", (unsigned long long)off);
        uint64_t *next = (uint64_t *)(uintptr_t)fp[0];
        if (next <= fp) break;
        fp = next;
    }
    atomic_store(&g_stack_done, 1);
}
void plat_thread_stack(pthread_t t) {
    static int installed;
    if (!installed) {
        uint64_t off;
        (void)plat_symbol(0, &off);   // the symbols loaded here, not in the handler
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = stack_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR2, &sa, NULL)) return;
        installed = 1;
    }
    atomic_store(&g_stack_done, 0);
    if (pthread_kill(t, SIGUSR2)) { fprintf(stderr, "[stall]     (its stack could not be read)\n"); return; }
    for (int i = 0; i < 50 && !atomic_load(&g_stack_done); i++) usleep(10000);
    if (!atomic_load(&g_stack_done)) fprintf(stderr, "[stall]     (it did not answer in half a second)\n");
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) return;
    if ((sig == SIGSEGV || sig == SIGBUS) && wp_fault(fa, uc->uc_mcontext.mc_err & 2)) return;   // (x86 PF_W: a write)
    uint64_t pc = (uint64_t)uc->uc_mcontext.mc_rip, sp = (uint64_t)uc->uc_mcontext.mc_rsp, lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

// A signal stack per thread (in direct memory: the flexible budget is small), so the fault handler runs on a thread
// whose own stack is exhausted too.
void plat_thread_start(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    void *b = stack_take(1 << 16);
    stack_t ss = {.ss_sp = b, .ss_size = 1 << 16};
    if (b) sigaltstack(&ss, NULL);
}

void plat_crash_handler_install(void) {
    void *b = stack_take(1 << 20);
    stack_t ss = {.ss_sp = b ? b : malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes: none on the console (the lifter and the compiler run on the PC)
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv, (void)log_path, (void)append_log;
    return 0;
}
void plat_proc_kill(PlatProc p) { (void)p; }
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }

// rm -rf (the title's libc has no nftw)
bool plat_remove_tree(const char *dir) {
    struct stat st;
    if (lstat(dir, &st) && stat(dir, &st)) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return remove(dir) == 0;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool ok = true;
    char path[1024];
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        ok &= plat_remove_tree(path);
    }
    closedir(d);
    return rmdir(dir) == 0 && ok;
}

void plat_terminate(void) { _exit(0); }
