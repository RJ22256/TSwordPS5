// (TSword PS5) The console's host layer (runtime/platform.h) for UltraTears, as WSword PS5 does it for WSword
// (SheikahSlate pc/runtime/platform/ps5.c): the payload SDK's ps5platform, prospero-clang, RADV on the console's GPU.
// Modelled on linux.c, with the differences the console makes:
//  - guest memory is direct memory: a title's anonymous mappings come from its small flexible budget, so the guest
//    window is a reserved virtual range (ps5platform/shm.h) backed in 64 KiB units on first touch (the crash handler)
//    and before the kernel itself writes there (plat_commit: file reads)
//  - no write watch (the console has neither userfaultfd nor GetWriteWatch): plat_written_pages answers -1, and the
//    graphics code imports game memory and compares texture contents instead (gfx/nvn/nvn_dirty_win.c, vkb_tex.c), as
//    on Windows with imports
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
        if (a < e) mark(a, e, false);
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
}

void plat_memory_map(void) {}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr, (void)size, (void)pages, (void)max, (void)page;
    return -1;   // (no write watch on the console: above)
}

void plat_watch_freeze_stats(uint64_t *frozen_mib, uint64_t *thaws, uint64_t *skipped_mib) {
    *frozen_mib = *thaws = *skipped_mib = 0;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? 0 : prot == PLAT_PROT_READ ? PS5_SHM_READ : PS5_SHM_READ | PS5_SHM_WRITE;
    if (in_window(addr)) {   // backed first (whole units read/write), then the pages' protection
        plat_commit(addr, size);
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

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
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
    dup2(STDERR_FILENO, STDOUT_FILENO);
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

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) return;
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
