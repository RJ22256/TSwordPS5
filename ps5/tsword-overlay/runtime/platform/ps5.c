// (TSword) The PS5 implementation of platform.h, made from linux.c for the console's FreeBSD-based libc: pthreads,
// CLOCK_MONOTONIC, _umtx_op in place of the futex, POSIX files. Memory is direct memory in reserved ranges
// (ps5platform/shm.h: a title's anonymous mappings are charged to its ~1 GB flexible budget), committed in 64 KiB units
// and protected in 16 KiB pages. No child processes, no /proc or sysfs: what needs them reports nothing.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/umtx.h>
#include <pthread_np.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ps5platform/shm.h>

#include "platform.h"

// the console's own: thread affinity, the random number service, the timed join its libkernel has
int scePthreadGetaffinity(pthread_t, uint64_t *);
int scePthreadSetaffinity(pthread_t, uint64_t);
int sceRandomGetRandomNumber(void *buf, size_t n);
int pthread_timedjoin_np(pthread_t, void **, const struct timespec *);

extern char **environ;

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t tid;   // the kernel's id once the thread runs, for plat_thread_set_priority
    char name[16];          // the kernel keeps 15 characters
};

// A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted.
// Never freed: threads that end are few, and a stack freed while a handler might run on it would be worse.
static void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;   // a signal frame with the AVX-512 state takes several KiB
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size};
    sigaltstack(&ss, NULL);
}

// Thread stacks (plat_thread_new_ex): reserved ranges, the top STACK_FIRST committed; stack_fault commits the 64 KiB
// unit a thread touches below it. The ranges stay (threads that end are few).
#define STACK_FIRST (256u << 10)
#define MAX_STACKS 512
static struct { uint64_t lo, hi; } stacks[MAX_STACKS];
static int nstacks;
static PlatMutex stack_lock = PLAT_MUTEX_INIT;

static void *stack_alloc(size_t size) {
    void *b = NULL;
    if (size < STACK_FIRST * 2 || ps5_vrange_reserve(size, NULL, 0x10000, &b)) return NULL;
    if (ps5_vrange_commit((char *)b + size - STACK_FIRST, STACK_FIRST, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(b, size);
        return NULL;
    }
    plat_mutex_lock(&stack_lock);
    if (nstacks < MAX_STACKS) stacks[nstacks].lo = (uint64_t)(uintptr_t)b, stacks[nstacks++].hi = (uint64_t)(uintptr_t)b + size;
    plat_mutex_unlock(&stack_lock);
    return b;
}

// A fault in a thread stack's reserved part: its 64 KiB unit committed (the lowest 16 KiB of the range stays a guard).
static int stack_fault(uint64_t a) {
    for (int i = 0; i < __atomic_load_n(&nstacks, __ATOMIC_ACQUIRE); i++)
        if (a >= stacks[i].lo + 0x4000 && a < stacks[i].hi) {
            uint64_t u = a & ~0xFFFFull;
            if (u < stacks[i].lo + 0x4000) u = stacks[i].lo + 0x4000;
            return ps5_vrange_commit((void *)(uintptr_t)u, ((a & ~0xFFFFull) + 0x10000) - u, PS5_SHM_READ | PS5_SHM_WRITE) == 0;
        }
    return 0;
}

static void fault_install(void);
static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    __atomic_store_n(&t->tid, (int32_t)pthread_getthreadid_np(), __ATOMIC_RELEASE);
    if (t->name[0]) pthread_set_name_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // (the console) The stack is reserved direct memory, its top committed and the rest at a thread's first touch
    // (stack_fault), as Windows reserves it: a pthread's own stack would be charged to the title's ~1 GB flexible budget.
    size_t size = ((stack ? stack : PLAT_STACK_DEFAULT) + 0xFFFF) & ~(size_t)0xFFFF;
    fault_install();   // (stack_fault: the stack's first touches below its top)
    void *base = stack_alloc(size);
    if (base) pthread_attr_setstack(&a, base, size);
    else pthread_attr_setstacksize(&a, size);
    int e = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

static int read_u64_file(const char *path, uint64_t *v);

// The console's affinity mask: logical processors 2k and 2k + 1 are one physical core's two threads (Zen 2).
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    uint64_t m = 0;
    *all = *first = 0;
    if (scePthreadGetaffinity(pthread_self(), &m) || !m) return 0;
    int cores = 0;
    for (int i = 0; i < 64; i += 2)
        if (m >> i & 3) *first |= 1ull << (m >> i & 1 ? i : i + 1), cores++;
    *all = m;
    return cores;
}

// No process-wide affinity on the console: only the calling thread's (threads it starts inherit it).
int plat_process_affinity(uint64_t mask) { return scePthreadSetaffinity(pthread_self(), mask) == 0; }

// The first field of /proc/self/task/<tid>/schedstat is the time the thread has run, in ns.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    (void)tid;
    return 0;   // (no per-thread times on the console)
}

// ---- thread interruption: a signal handshake in place of Windows' SuspendThread / Get/SetThreadContext.
// The caller moves the handle from IDLE to REQ and signals the thread. The thread's handler stores where it was and
// moves REQ to STOPPED, then spins until the caller moves it on to GO (with the redirect in to), changes its own
// return context, and goes back to IDLE. A caller that sees no STOPPED within INTR_WAIT_NS (the thread is blocked in
// the kernel and runs no handler) takes REQ back to IDLE; a handler that then finds IDLE returns at once.
enum { INTR_IDLE, INTR_REQ, INTR_STOPPED, INTR_GO };
#define INTR_WAIT_NS 200000u

struct PlatIntr {
    pthread_t pt;
    volatile int32_t state;
    uint64_t rip, rsp, to;
};

static __thread PlatIntr *intr_me;
static int intr_sig;

static void intr_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    PlatIntr *t = intr_me;
    ucontext_t *u = uc;
    if (!t) return;
    t->rip = (uint64_t)u->uc_mcontext.mc_rip;
    t->rsp = (uint64_t)u->uc_mcontext.mc_rsp;
    int32_t req = INTR_REQ;
    if (!__atomic_compare_exchange_n(&t->state, &req, INTR_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_GO) plat_pause();
    if (t->to) u->uc_mcontext.mc_rip = (__register_t)t->to;
    __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
}

PlatIntr *plat_intr_self(void) {
    static PlatMutex install = PLAT_MUTEX_INIT;
    if (!__atomic_load_n(&intr_sig, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&install);
        if (!intr_sig) {
            int sig = SIGUSR2;   // (the console: handled on a busy thread - the TSword console check)
            struct sigaction sa = {.sa_sigaction = intr_handler, .sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK};
            sigemptyset(&sa.sa_mask);
            if (!sigaction(sig, &sa, NULL)) __atomic_store_n(&intr_sig, sig, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&install);
    }
    if (!intr_sig) return NULL;
    PlatIntr *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pt = pthread_self();
    intr_me = t;
    return t;
}

// Called by the thread itself as it ends, or by another once the thread no longer runs.
void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    if (intr_me == t) intr_me = NULL;
    free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    int32_t idle = INTR_IDLE;
    if (!__atomic_compare_exchange_n(&t->state, &idle, INTR_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (pthread_kill(t->pt, intr_sig)) {
        __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t end = plat_now_ns() + INTR_WAIT_NS;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_STOPPED) {
        if (plat_now_ns() < end) {
            plat_pause();
            continue;
        }
        int32_t req = INTR_REQ;
        if (__atomic_compare_exchange_n(&t->state, &req, INTR_IDLE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    *rip = t->rip;
    *rsp = t->rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    t->to = to;
    __atomic_store_n(&t->state, INTR_GO, __ATOMIC_RELEASE);
    return 1;
}

// ---- fibers: a stack switch of our own. glibc's swapcontext saves and restores the signal mask with a system call
// on every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which realigns the stack and calls fn(arg)
// (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        // fiber_swap's ret lands here with rsp = top - 8, as at the entry of a called function. The call below must
        // be made with rsp 16-byte aligned so that fn starts with rsp = 8 mod 16, as the System V ABI demands; without
        // the realignment every 16-byte local of fn is off by 8, which the VEX instructions of a v3 build tolerate
        // but the SSE ones of a v2 or k10 build (movdqa, pcmpeqb with a memory operand) fault on.
        "    andq $-16, %rsp\n"
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    // Reserved, not committed; the lowest page stays inaccessible, so an overflow faults instead of corrupting.
    // (the console: direct memory in a reserved range, rounded to 64 KiB units, the lowest 16 KiB page left reserved)
    stack = (stack + 0xFFFF) & ~(size_t)0xFFFF;
    void *m = NULL;
    if (ps5_vrange_reserve(stack, NULL, 0x10000, &m)) {
        free(f);
        return NULL;
    }
    if (ps5_vrange_commit((char *)m + 0x4000, stack - 0x4000, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(m, stack);
        free(f);
        return NULL;
    }
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
    uint64_t *sp = top - 10;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) ps5_vrange_release(f->stack, f->size);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);   // pthread_timedjoin_np takes a CLOCK_REALTIME deadline
    uint64_t end = (uint64_t)at.tv_sec * 1000000000u + (uint64_t)at.tv_nsec + ns;
    at.tv_sec = (time_t)(end / 1000000000u), at.tv_nsec = (long)(end % 1000000000u);
    if (pthread_timedjoin_np(t->t, NULL, &at)) return 0;
    free(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t th = t ? t->t : pthread_self();
    uint64_t old = 0;
    if (scePthreadGetaffinity(th, &old)) return 0;
    if (scePthreadSetaffinity(th, mask)) return 0;
    return old;
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// The processors of the process's affinity mask (taskset, a launcher's CPU set), every online one when it is unknown.
uint32_t plat_cpu_usable(void) {
    uint64_t m = 0;
    if (!scePthreadGetaffinity(pthread_self(), &m) && m) return (uint32_t)__builtin_popcountll(m);
    return plat_cpu_count();
}

// Windows' levels map to nice values: IDLE 19, LOWEST 4, BELOW_NORMAL 2, NORMAL 0, ABOVE_NORMAL -2, HIGHEST -5,
// TIME_CRITICAL -10. Applied only when the process may lower a nice value again (platform.h says why).
static __attribute__((unused)) int nice_of(int prio) {
    return prio <= PLAT_PRIO_IDLE ? 19 : prio == PLAT_PRIO_LOWEST ? 4 : prio == PLAT_PRIO_BELOW_NORMAL ? 2
         : prio == PLAT_PRIO_NORMAL ? 0 : prio == PLAT_PRIO_ABOVE_NORMAL ? -2 : prio == PLAT_PRIO_HIGHEST ? -5 : -10;
}

void plat_thread_priority(int prio) { (void)prio; }   // (the console: every thread at the title's priority)

void plat_thread_set_priority(PlatThread *t, int prio) { (void)t, (void)prio; }

int plat_thread_priority_get(void) { return PLAT_PRIO_NORMAL; }

void plat_yield(void) { sched_yield(); }

// ---- time

uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

// The timer slack (50 us by default) is how late the kernel may wake a sleeping thread to batch wakeups. New threads
// inherit it from the thread that creates them.
void plat_timer_1ms(void) {}

int plat_power_full(void) { return -1; }

int plat_perf_core_setup(char *why, size_t n) {
    snprintf(why, n, "not done on Linux");
    return 0;
}
int plat_thread_perf_core(void) { return -1; }

// clock_nanosleep wakes within the timer slack (50 us by default) of its deadline; the last PLAT_SPIN_NS are spent
// yielding so the deadline is met closely, as on Windows.
#define PLAT_SPIN_NS 100000u

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        uint64_t at = end - PLAT_SPIN_NS;
        struct timespec ts = {(time_t)(at / 1000000000u), (long)(at % 1000000000u)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    while (plat_now_ns() < end) sched_yield();
}

void plat_wait_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

// ---- locks: a reader-writer lock in one 32-bit word, zero when free.
// bit 31: a writer holds it; bit 30: some thread sleeps on the word; bits 0-29: readers holding it.
// A thread that cannot take the lock sets the sleeper bit and waits on the futex; every release that finds the bit
// clears it and wakes all sleepers, which retry (and set it again if they still cannot get in). Readers do not enter
// while the sleeper bit is set, so a waiting writer is not starved by a stream of readers.

#define LK_W 0x80000000u
#define LK_S 0x40000000u
#define LK_R 0x3fffffffu
#define LK_SPINS 64

// (the console) FreeBSD's _umtx_op in place of Linux's futex: wait while the word equals val (a relative timeout),
// or wake that many waiters. -1 with errno ETIMEDOUT on a timeout, as the futex.
enum { FUTEX_WAIT, FUTEX_WAKE };
static long futex(volatile uint32_t *w, int op, uint32_t val, const struct timespec *ts) {
    if (op == FUTEX_WAKE) return _umtx_op((void *)w, UMTX_OP_WAKE_PRIVATE, val, NULL, NULL);
    struct timespec t;
    if (ts) t = *ts;
    return _umtx_op((void *)w, UMTX_OP_WAIT_UINT_PRIVATE, val, ts ? (void *)(uintptr_t)sizeof t : NULL, ts ? &t : NULL);
}

static inline volatile uint32_t *lk(PlatMutex *m) { return (volatile uint32_t *)&m->p; }

// Sets the sleeper bit on s and sleeps while the word still equals the result.
static void lk_sleep(volatile uint32_t *w, uint32_t s) {
    if (!(s & LK_S) && !__atomic_compare_exchange_n(w, &s, s | LK_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    futex(w, FUTEX_WAIT, s | LK_S, NULL);
}

static void lk_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~LK_S, __ATOMIC_RELAXED);
    futex(w, FUTEX_WAKE, INT_MAX, NULL);
}

void plat_mutex_init(PlatMutex *m) { m->p = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_R)))
        if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_R))) {
            if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_fetch_and(w, ~LK_W, __ATOMIC_RELEASE);
    if (s & LK_S) lk_wake(w);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_S)))
        if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_S))) {
            if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_sub_fetch(w, 1, __ATOMIC_RELEASE);
    if (!(s & LK_R) && (s & LK_S)) lk_wake(w);
}

// ---- condition variables: a sequence number in the first 32 bits. A waiter reads it before releasing the lock and
// sleeps only while it is unchanged, so a signal between the release and the sleep is not lost.

static inline volatile uint32_t *cv(PlatCond *c) { return (volatile uint32_t *)&c->p; }

void plat_cond_init(PlatCond *c) { c->p = 0; }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    futex(cv(c), FUTEX_WAIT, seq, NULL);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    plat_mutex_unlock(m);
    long r = futex(cv(c), FUTEX_WAIT, seq, &ts);
    int timed_out = r == -1 && errno == ETIMEDOUT;
    plat_mutex_lock(m);
    return !timed_out;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, 1, NULL);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, INT_MAX, NULL);
}

// ---- files

int plat_file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    return snprintf(out, cap, "%s/%s", cwd, path) < (int)cap;
}

void plat_file_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    mkdir(tmp, 0755);
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (buf && got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

// O_APPEND makes each write land whole at the end of the file, from any thread.
PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t k;
    do k = write(fd, data, n);
    while (k < 0 && errno == EINTR);
    return k == (ssize_t)n;
}

// Flushes the directory holding path, so a rename into it survives a crash.
static void sync_dir_of(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_file_replace(const char *from, const char *to) {
    if (rename(from, to)) return 0;
    sync_dir_of(to);
    return 1;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    // A rename replaces a read-only file here (only the folder's permission counts) but not on Windows: a file the
    // player made read-only stays as it is on both.
    if (access(path, W_OK) && errno != ENOENT) return 0;
    char tmp[PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size) == 0; }

int64_t plat_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}

// The owner's write permission stands for Windows' read-only attribute.
int plat_file_set_readonly(const char *path, int on) {
    struct stat st;
    if (stat(path, &st)) return 0;
    mode_t m = on ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(path, m & 07777) == 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

// ---- virtual memory (the console). Reservations are reserved ranges (ps5_vrange_reserve_at); commit backs them with
// direct memory in 64 KiB units and sets the protection in 16 KiB pages (ps5_vrange_commit), decommit gives whole
// units back and zeroes the rest (ps5_vrange_decommit) - so decommitted pages read zero, as the runtime expects. A byte
// per 4 KiB guest page records the state Horizon has (pstate), as on Linux.
//
// The protection is the hardware's 16 KiB page: a protection that would take away access from part of one (a 4 KiB
// module data page made read-only beside a writable one) is applied to the whole pages inside the range only, so the
// rest keeps its access; one that gives access is applied to every page the range touches.
//
// Write watch: none. The PS5's memory is one pool, which the back end imports in place (vk_memory.c: uma), so the
// uploads it would drive don't happen; plat_mem_written reports the ranges untracked, as Linux without userfaultfd.

#define PG 4096ull
#define HPG 0x4000ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
static uint8_t *pstate;
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

static int shm_prot(int prot) {
    return prot == PLAT_PROT_RW ? PS5_SHM_READ | PS5_SHM_WRITE : prot == PLAT_PROT_READ ? PS5_SHM_READ : 0;
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    (void)watch;
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        // 128 MiB of state for the 2^39 window: reserved, committed as it is written (mem_state_fault)
        void *p = NULL;
        if (!ps5_vrange_reserve(PS_LIMIT / PG, NULL, 0x10000, &p)) pstate = p;
    }
    int ok = pstate && addr + n <= PS_LIMIT && !(addr & 0xFFFF) && ps5_vrange_reserve_at((void *)(uintptr_t)addr, n) == 0;
    if (ok) {
        uint64_t a = addr / PG, e = (addr + n + PG - 1) / PG;
        // the state bytes of the range, backed
        uint64_t s0 = ((uint64_t)(uintptr_t)pstate + a) & ~0xFFFFull, s1 = ((uint64_t)(uintptr_t)pstate + e + 0xFFFF) & ~0xFFFFull;
        ps5_vrange_commit((void *)(uintptr_t)s0, s1 - s0, PS5_SHM_READ | PS5_SHM_WRITE);
        ps_set(addr, n, PS_RESERVED);
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void *plat_mem_alloc(size_t n) {
    void *p = NULL;
    size_t size = (n + 0xFFFF) & ~(size_t)0xFFFF;
    if (ps5_vrange_reserve(size, NULL, 0x10000, &p)) return NULL;
    if (ps5_vrange_commit(p, size, PS5_SHM_READ | PS5_SHM_WRITE)) {
        ps5_vrange_release(p, size);
        return NULL;
    }
    return p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (ps5_vrange_commit((void *)(uintptr_t)addr, n, PS5_SHM_READ | PS5_SHM_WRITE)) return 0;
    ps_set(addr, n, PS_RW);
    return 1;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    if (ps5_vrange_decommit((void *)(uintptr_t)addr, n)) return 0;
    ps_set(addr, n, PS_RESERVED);
    return 1;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    uint64_t lo = addr, hi = addr + n;
    if (prot != PLAT_PROT_RW) lo = (lo + HPG - 1) & ~(HPG - 1), hi &= ~(HPG - 1);   // (whole 16 KiB pages only)
    else lo &= ~(HPG - 1), hi = (hi + HPG - 1) & ~(HPG - 1);
    if (hi > lo && mprotect((void *)(uintptr_t)lo, hi - lo, shm_prot(prot) == (PS5_SHM_READ | PS5_SHM_WRITE)
                                                                ? PROT_READ | PROT_WRITE
                                                            : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE))
        return 0;
    ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    return 1;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    if (!plat_mem_readable((uint64_t)(uintptr_t)(pstate + i), 1)) {   // (state never backed: nothing reserved there)
        *end = ((uint64_t)(uintptr_t)(pstate + i) | 0xFFFF) + 1 - (uint64_t)(uintptr_t)pstate;
        *end = *end * PG < lim ? *end * PG : lim;
        return PLAT_MEM_FREE;
    }
    uint8_t v = pstate[i];
    while ((i + 1) * PG < lim && ((uintptr_t)(pstate + i + 1) & 0xFFFF) && pstate[i + 1] == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    (void)addr, (void)n, (void)pages, (void)max, (void)reset;
    return UINT64_MAX;   // (untracked: above)
}

// One handler for the synchronous faults. A SIGSEGV whose page-fault error code has the write bit set goes to the
// write-fault hook first (memory.c thaws a frozen region); any other fault, in this executable or in a library (Mesa,
// SDL), then goes to the crash hook (main.c), which reports it and ends the process. Without a crash hook the fault
// gets the default action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static void (*crash_fn)(const PlatCrash *c);

static __thread sigjmp_buf *read_guard;   // (plat_mem_read: a fault while reading jumps back)

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    long err = (long)u->uc_mcontext.mc_err;
    if ((sig == SIGSEGV || sig == SIGBUS) && stack_fault((uint64_t)(uintptr_t)si->si_addr)) return;
    if ((sig == SIGSEGV || sig == SIGBUS) && read_guard) siglongjmp(*read_guard, 1);
    if ((sig == SIGSEGV || sig == SIGBUS) && fault_hook && (err & 2) && fault_hook((uint64_t)(uintptr_t)si->si_addr)) return;
    uintptr_t rip = (uintptr_t)u->uc_mcontext.mc_rip;
    if (crash_fn) {
        // (the console: the title's image is the module; anything else - the game code, a system module - by address)
        char mod[32];
        uintptr_t mbase = 0;
        int found = plat_module_of(rip, mod, sizeof mod, &mbase);
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, found ? rip - mbase : rip, -1, 0};
        if (!found) c.module = "";
        if (sig == SIGSEGV || sig == SIGBUS)
            c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = (uint64_t)(uintptr_t)si->si_addr;
        const mcontext_t *m = &u->uc_mcontext;
        const uint64_t gr[16] = {m->mc_rax, m->mc_rcx, m->mc_rdx, m->mc_rbx, m->mc_rsp, m->mc_rbp, m->mc_rsi, m->mc_rdi,
                                 m->mc_r8,  m->mc_r9,  m->mc_r10, m->mc_r11, m->mc_r12, m->mc_r13, m->mc_r14, m->mc_r15};
        for (int i = 0; i < 16; i++) c.gpr[i] = gr[i];
        c.has_regs = 1;
        crash_fn(&c);
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

void plat_reexec(void) {
    fprintf(stderr, "FATAL: the console can't start the process again\n");   // (no exec for a title)
    fflush(stderr);
    _exit(70);
}

// (the console) A copy that a fault ends: the fault handler jumps back here (read_guard).
int plat_mem_read(uint64_t addr, void *out, size_t n) {
    fault_install();
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) {
        read_guard = NULL;
        return 0;
    }
    read_guard = &jb;
    memcpy(out, (const void *)(uintptr_t)addr, n);
    read_guard = NULL;
    return 1;
}

// Not known before the SDL window exists; the Steam Deck has its own profile (cfg.c)
int plat_screen_size(int *w, int *h) {
    (void)w, (void)h;
    return 0;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// A PlatFile is the descriptor + 1, so descriptor 0 is not NULL.
PlatFile *plat_file_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

static int fd_of(PlatFile *f) { return (int)(intptr_t)f - 1; }

void plat_file_close(PlatFile *f) {
    if (f) close(fd_of(f));
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pread(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k >= 0 ? k : errno == EFAULT ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec,
                        (uint64_t)st.st_ctim.tv_sec * 1000000000u + (uint64_t)st.st_ctim.tv_nsec};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    struct stat st;
    if (fstat(fd_of(f), &st) || !st.st_size) return NULL;
    void *v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd_of(f), 0);
    return v == MAP_FAILED ? NULL : v;
}

int plat_fs_os_error(void) { return errno; }

static int fs_err(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return PLAT_FS_MISSING;
    return S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_rmdir(const char *path) {
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return unlink(path) ? fs_err(errno) : PLAT_FS_OK;
    return rmdir(path) ? fs_err(errno) : PLAT_FS_OK;
}

// Deleting needs write access to the directory, not to the file: a read-only file goes as any other.
int plat_fs_unlink(const char *path) { return unlink(path) ? fs_err(errno) : PLAT_FS_OK; }

// RENAME_NOREPLACE keeps Windows' rule that a rename never replaces; a file system without it (EINVAL) gets a check
// first, which another process could race but the save directories are ours.
int plat_fs_rename(const char *from, const char *to, int durable) {
    // (the console: no renameat2 - the check first, the save directories being ours)
    if (plat_fs_kind(to) != PLAT_FS_MISSING) return PLAT_FS_EXISTS;
    if (rename(from, to)) return fs_err(errno);
    if (durable) {
        sync_dir_of(from);
        sync_dir_of(to);
    }
    return PLAT_FS_OK;
}

int plat_fs_link(const char *existing, const char *path) { return link(existing, path) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_links(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int)st.st_nlink;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

// Open files never stop a rename here.
void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    (void)dir;
    if (cap) *out = 0;
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return fs_err(errno);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
        if (!cb(e->d_name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    struct stat st;
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1, errno = ENOENT;
    }
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pwrite(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k;
}

int plat_file_set_size(PlatFile *f, uint64_t size) { return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : 0; }

int plat_file_fsync(PlatFile *f) { return fsync(fd_of(f)) == 0; }

int plat_temp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    while (n > 1 && t[n - 1] == '/') n--;
    return snprintf(out, cap, "%.*s", (int)n, t) < (int)cap;
}

// (the console) The title's image: from the ELF header the linker places first (__ehdr_start) to its end (_end). Its
// path is /app0/eboot.bin - the title folder, where the runtime keeps its log, its settings and its caches.
extern const char __ehdr_start[], _end[];
uintptr_t plat_exe_base(void) { return (uintptr_t)__ehdr_start; }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    if (addr < (uintptr_t)__ehdr_start || addr >= (uintptr_t)_end) return 0;
    *base = (uintptr_t)__ehdr_start;
    return snprintf(name, cap, "eboot.bin") < (int)cap;
}

int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, "/app0/eboot.bin") < (int)cap; }

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Cached per thread: plat_rmutex_lock asks on every acquire, hundreds of thousands of times a second in play, and
// gettid is a full syscall on Linux (Windows reads GetCurrentThreadId from the TEB). A thread's id never changes, and
// the runtime starts child processes with posix_spawn only, so no forked copy inherits a stale id. noinline: a fiber
// may resume on another thread (platform.h), and a caller that inlined this could keep the address of tid it computed
// before a plat_fiber_switch; as a call it reads the slot of the thread it runs on.
__attribute__((noinline)) uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) tid = (uint32_t)pthread_getthreadid_np();
    return tid;
}

// The first set variable of names ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into tag; "" for C and POSIX.
static void locale_tag(const char *const *names, int n, char *tag, size_t cap) {
    const char *v = NULL;
    for (int i = 0; i < n && !(v && *v); i++) v = getenv(names[i]);
    size_t k = 0;
    if (v && strcmp(v, "C") && strncmp(v, "C.", 2) && strcmp(v, "POSIX"))
        for (; v[k] && v[k] != '.' && v[k] != '@' && k < cap - 1; k++) tag[k] = v[k] == '_' ? '-' : v[k];
    tag[k] = 0;
}

const char *plat_ui_language(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_TIME", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

// Noto CJK as the common distributions and SteamOS install it; one collection covers every CJK language.
int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *files[] = {
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    };
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++)
        if (plat_file_exists(files[i])) return snprintf(out, cap, "%s", files[i]) < (int)cap;
    return 0;
}

// PRETTY_NAME of /etc/os-release and the kernel release: "SteamOS (Linux 6.16.12-valve)".
int plat_os_version(char *out, size_t cap) { return snprintf(out, cap, "PlayStation 5 (TSword PS5)") < (int)cap; }

int plat_is_wine(void) { return 0; }

int plat_stderr_path(char *out, size_t cap) { (void)out, (void)cap; return 0; }   // (the console's stderr: the kernel log)

// ---- process

#ifdef TOTK_PGO_GEN
// compiler-rt's profile runtime (an instrumented build, tools/build_game.py --pgo gen). It writes its counters from an
// atexit handler, which _exit skips; __llvm_profile_dump writes them once and makes later writes no-ops.
int __llvm_profile_dump(void);
#endif

void plat_exit(int code) {
#ifdef TOTK_PGO_GEN
    __llvm_profile_dump();
#endif
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

int plat_setenv(const char *name, const char *value) {
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

uint64_t plat_process_cpu_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// (the console) Since the first call, made early at start (main.c's log header).
uint64_t plat_process_uptime_ns(void) {
    static uint64_t start;
    if (!start) start = plat_now_ns();
    return plat_now_ns() - start;
}

uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

// The range of the Windows CRT's localtime_s and gmtime_s (1970 to 3000), which glibc exceeds: the game's time
// service reports times outside it as invalid on both hosts.
#define PLAT_TIME_MAX 32535215999ll

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {   // (FreeBSD: no timezone variable - January's offset, as standard time)
    struct tm t;
    time_t at = 15 * 86400;
    if (!localtime_r(&at, &t)) return 0;
    return -(long)t.tm_gmtoff;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    (void)env_name, (void)env_value, (void)timeout_ms;   // (no child processes on the console)
    if (cap) out[0] = 0;
    return -1;
}

int plat_random(void *out, size_t n) {   // (the console's random number service: at most 64 bytes a call)
    uint8_t *p = out;
    while (n) {
        size_t k = n < 64 ? n : 64;
        if (sceRandomGetRandomNumber(p, k) < 0) return 0;
        p += k, n -= k;
    }
    return 1;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) { (void)utf8; return 0; }   // (no clipboard on the console)

int plat_open_folder(const char *path) { (void)path; return 0; }   // (no file manager on the console)

// ---- dynamic libraries

// (the console: a title loads no libraries of its own this way - the game code has the title's loader)
void *plat_lib_open(const char *name) { (void)name; return NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib, (void)name; return NULL; }

void plat_lib_error(char *out, size_t cap) { snprintf(out, cap, "no dynamic libraries on the console"); }

int plat_ngx_core_path(char *out, size_t cap) { return snprintf(out, cap, "libnvidia-ngx.so.1") < (int)cap; }

// UTF-8 to UTF-32 wchar_t, without the C library's locale (the runtime never sets one, so mbstowcs would stop at the
// first non-ASCII byte). Returns 0 on malformed input or when out has no room.
static int utf8_to_wide(const char *in, wchar_t *out, size_t cap) {
    const unsigned char *u = (const unsigned char *)in;
    size_t n = 0;
    while (*u) {
        unsigned c = *u++, more = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 4;
        if (more == 4) return 0;
        if (more) c &= 0x3F >> more;
        for (unsigned k = 0; k < more; k++, u++) {
            if ((*u & 0xC0) != 0x80) return 0;
            c = c << 6 | (*u & 0x3F);
        }
        if (n + 1 >= cap) return 0;
        out[n++] = (wchar_t)c;
    }
    out[n] = 0;
    return 1;
}

int plat_exe_dir_wide(wchar_t *out, size_t cap) {
    char dir[PLAT_PATH_MAX];
    char *cut = plat_exe_path(dir, sizeof dir) ? plat_last_sep(dir) : NULL;
    if (!cut) return 0;
    cut[1] = 0;
    return utf8_to_wide(dir, out, cap);
}

// ---- machine load: /proc/stat and /proc/self/stat for the CPU, hwmon for the temperature, the amdgpu sysfs files of
// the render device for the GPU.

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
    uint64_t proc_ns;
    char gpu[256];                // /sys/class/drm/cardN/device of the GPU in use; empty when unknown
    char temp[256];               // the CPU's hwmon temp1_input; empty when unknown
};

static int read_u64_file(const char *path, uint64_t *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long x;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (ok) *v = x;
    return ok;
}

// The hwmon device of the CPU package: k10temp (AMD), coretemp (Intel), or the first that has a temp1_input.
static void find_cpu_temp(char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[256], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        int cpu = !strcmp(name, "k10temp") || !strcmp(name, "coretemp") || !strcmp(name, "zenpower");
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
        if ((cpu || !out[0]) && plat_file_exists(path)) snprintf(out, cap, "%s", path);
        if (cpu && out[0]) break;
    }
    closedir(d);
}

// The DRM card with gpu_busy_percent (amdgpu) on PCI bus bus, the game's device. With bus -1 (no VK_EXT_pci_bus_info):
// of several, the one with the most VRAM in use. None when the game's GPU has no such file (NVIDIA): the overlay then
// takes its load and VRAM from NVML, not from an idle integrated GPU.
static void find_gpu(char *out, size_t cap, int bus) {
    out[0] = 0;
    uint64_t best = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
        char dev[256], path[300];
        snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", e->d_name);
        snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
        uint64_t v, used = 0;
        if (!read_u64_file(path, &v)) continue;
        if (bus >= 0) {   // device links to the PCI function, "0000:03:00.0": domain, bus, device, function
            char real[PATH_MAX];
            unsigned dom, b;
            const char *base = realpath(dev, real) ? strrchr(real, '/') : NULL;
            if (!base || sscanf(base + 1, "%x:%x:", &dom, &b) != 2 || (int)b != bus) continue;
        }
        snprintf(path, sizeof path, "%s/mem_info_vram_used", dev);
        read_u64_file(path, &used);
        if (!out[0] || used > best) snprintf(out, cap, "%s", dev), best = used;
    }
    closedir(d);
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)gpu_luid;   // Linux has no adapter LUIDs; the PCI bus names the card
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    find_cpu_temp(l->temp, sizeof l->temp);
    find_gpu(l->gpu, sizeof l->gpu, pci_bus);
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[10] = {0};
    int n = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7], &v[8], &v[9])
              : 0;
    if (f) fclose(f);
    uint64_t pns = plat_process_cpu_ns();
    if (n >= 8) {
        uint64_t idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   // guest time is already inside user time
        uint64_t busy = total - idle;
        if (l->total && total > l->total) {
            uint64_t dt = total - l->total;
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)dt;
            // the process time is in ns, the machine's in jiffies summed over every CPU
            double dt_ns = (double)dt * 1e9 / (double)sysconf(_SC_CLK_TCK);
            *proc_pct = (float)(100.0 * (double)(pns - l->proc_ns) / dt_ns);
        }
        l->busy = busy, l->total = total, l->proc_ns = pns;
    }
    uint64_t mc;
    if (l->temp[0] && read_u64_file(l->temp, &mc)) *temp_c = (float)mc / 1000.0f;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu[0]) return;
    char path[300];
    uint64_t v;
    snprintf(path, sizeof path, "%s/gpu_busy_percent", l->gpu);
    if (read_u64_file(path, &v)) *load_pct = (float)v;
    snprintf(path, sizeof path, "%s/mem_info_vram_used", l->gpu);
    if (read_u64_file(path, &v)) *vram_used_mb = (float)(v >> 20);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long total = 0, avail = 0, k;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
        }
        fclose(f);
        if (total) *total_mb = (float)(total >> 10), *used_mb = (float)((total - avail) >> 10);
    }
    f = fopen("/proc/self/statm", "r");
    unsigned long long size, resident;
    if (f && fscanf(f, "%llu %llu", &size, &resident) == 2) *proc_mb = (float)(resident * (uint64_t)sysconf(_SC_PAGESIZE) >> 20);
    if (f) fclose(f);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = *private_bytes = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[128];
    unsigned long long k;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %llu kB", &k) == 1) *resident = (uint64_t)k << 10;
        else if (sscanf(line, "RssAnon: %llu kB", &k) == 1) *private_bytes = (uint64_t)k << 10;
    }
    fclose(f);
}

// Linux overcommits by default, so CommitLimit binds only with vm.overcommit_memory = 2; Committed_AS is what the
// system has promised. The process's share is its resident anonymous memory, the nearest to Windows' private bytes.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    char line[128];
    unsigned long long k;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "CommitLimit: %llu kB", &k) == 1) *limit_mb = (double)(k >> 10);
            else if (sscanf(line, "Committed_AS: %llu kB", &k) == 1) *used_mb = (double)(k >> 10);
        }
        fclose(f);
    }
    uint64_t resident, private_bytes;
    plat_process_memory(&resident, &private_bytes);
    if (private_bytes) *proc_mb = (double)(private_bytes >> 20);
}

void plat_mem_room(uint64_t *commit_room, uint64_t *phys_room) {
    *commit_room = *phys_room = UINT64_MAX;
    char line[128];
    unsigned long long k;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) *phys_room = (uint64_t)k << 10;
    fclose(f);
}

// ---- child processes, local IPC, restarts: none on the console (no processes but the title's; no Discord)

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    (void)exe, (void)arg;
    if (err) *err = ENOSYS;
    return NULL;
}
void plat_child_stop(PlatChild *c) { (void)c; }
int plat_child_write(PlatChild *c, const void *data, size_t n) { (void)c, (void)data, (void)n; return 0; }
long plat_child_read(PlatChild *c, void *buf, size_t cap) { (void)c, (void)buf, (void)cap; return -1; }

PlatIpc *plat_ipc_connect(const char *name) { (void)name; return NULL; }
int plat_ipc_read(PlatIpc *p, void *buf, size_t n, uint32_t ms) { (void)p, (void)buf, (void)n, (void)ms; return 0; }
int plat_ipc_write(PlatIpc *p, const void *buf, size_t n) { (void)p, (void)buf, (void)n; return 0; }
void plat_ipc_close(PlatIpc *p) { (void)p; }

int plat_restart(void) { return 0; }
int plat_spawn(const char *const *argv, const char *cwd) { (void)argv, (void)cwd; return 0; }
int plat_restart_wait(uint32_t timeout_ms) { (void)timeout_ms; return 0; }
