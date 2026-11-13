// The time the log costs: every write to stderr / stdout (the log: ultratears.log, unbuffered so that a crash loses
// nothing) goes through these wrappers (the link's --wrap, tools/build_game.py), which count the writes and the time
// they take. The long-frame line (gfx/nvn/hle_nvn.c) says how much of a long frame went to the log, so a stutter caused
// by writing it shows as such instead of as an unexplained frame. Writes to other files are passed through untimed.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "platform.h"

static _Atomic uint64_t g_writes, g_ns;

void runtime_log_cost(uint64_t *writes, uint64_t *ns) {
    *writes = atomic_load(&g_writes);
    *ns = atomic_load(&g_ns);
}

static int is_log(FILE *f) { return f == stderr || f == stdout; }

static void account(uint64_t t0) {
    atomic_fetch_add(&g_writes, 1);
    atomic_fetch_add(&g_ns, plat_now_ns() - t0);
}

int __real_vfprintf(FILE *f, const char *fmt, va_list ap);
int __real_fputs(const char *s, FILE *f);
int __real_fputc(int c, FILE *f);

#ifdef __PROSPERO__
// (TSword PS5 0.4.10) The log written by a thread of its own. Each line was a write to the file from the thread that
// logged it, and on the console a write took up to seconds (the long-frame lines: "log 4 writes / 3794 ms"): the game's
// threads waited for the log, frame after frame, until the game made no frames at all. Lines now go into a ring in
// memory and the writer thread empties it into the file; a full ring drops lines (counted) rather than wait.
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#define LOG_RING (4u << 20)
static char g_ring[LOG_RING];
static uint64_t g_head, g_tail, g_dropped;   // bytes in and out, ever
static pthread_mutex_t g_rm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rc = PTHREAD_COND_INITIALIZER;
static int g_writer;   // 0 not started, 1 running, -1 none (written directly)
static int g_fd = -1;   // the log file's own descriptor: fileno(stderr) (on the console freopen does not keep it at 2)
static void *log_writer(void *unused) {
    (void)unused;
    static char buf[65536];
    for (;;) {
        pthread_mutex_lock(&g_rm);
        while (g_head == g_tail) pthread_cond_wait(&g_rc, &g_rm);
        uint64_t n = g_head - g_tail, dropped = g_dropped;
        if (n > sizeof buf) n = sizeof buf;
        for (uint64_t i = 0; i < n; i++) buf[i] = g_ring[(g_tail + i) % LOG_RING];
        g_dropped = 0;
        pthread_mutex_unlock(&g_rm);
        int fd = fileno(stderr);   // (each time: the log may be reopened after the writer started)
        g_fd = fd;
        for (size_t done = 0; done < n;) {
            ssize_t w = write(g_fd, buf + done, (size_t)n - done);
            if (w <= 0) break;
            done += (size_t)w;
        }
        if (dropped) {
            char note[96];
            int l = snprintf(note, sizeof note, "\n[log] %llu bytes of log lines dropped (the file could not keep up)\n",
                             (unsigned long long)dropped);
            if (write(g_fd, note, (size_t)l) < 0) {}
        }
        pthread_mutex_lock(&g_rm);
        g_tail += n;
        pthread_cond_broadcast(&g_rc);
        pthread_mutex_unlock(&g_rm);
    }
    return NULL;
}
static int log_push(const char *s, size_t n) {   // 1: taken (or dropped), 0: write it directly
    if (g_writer < 0) return 0;
    static __thread int t_holding;   // (a crash handler on the thread that holds the lock: its line goes straight out)
    if (t_holding) return 0;
    pthread_mutex_lock(&g_rm);
    t_holding = 1;
    if (!g_writer) {
        // (0.4.11) the writer only once the log is the file: 0.4.10 wrote to descriptor 2, which on the console was
        // not the log (freopen gave it another number) - its lines were lost, or went into whatever file 2 was
        g_fd = fileno(stderr);
        if (g_fd < 0) { g_writer = -1; t_holding = 0; pthread_mutex_unlock(&g_rm); return 0; }
        pthread_t t;
        g_writer = pthread_create(&t, NULL, log_writer, NULL) ? -1 : 1;
        if (g_writer > 0) pthread_detach(t);
        else { t_holding = 0; pthread_mutex_unlock(&g_rm); return 0; }
    }
    if (g_head - g_tail + n > LOG_RING) g_dropped += n;
    else for (size_t i = 0; i < n; i++) g_ring[(g_head + i) % LOG_RING] = s[i];
    g_head += g_head - g_tail + n > LOG_RING ? 0 : n;
    pthread_cond_signal(&g_rc);
    t_holding = 0;
    pthread_mutex_unlock(&g_rm);
    return 1;
}
// Before the title ends (plat_fatal_notice): the lines still in the ring onto the file, for at most ms.
void runtime_log_drain(unsigned ms) {
    if (g_writer <= 0) return;
    for (unsigned i = 0; i < ms; i += 10) {
        pthread_mutex_lock(&g_rm);
        int empty = g_head == g_tail;
        pthread_mutex_unlock(&g_rm);
        if (empty) return;
        usleep(10000);
    }
}
static __thread char t_line[16384];   // (not on the stack: some game threads have only a few KiB of it)
#else
void runtime_log_drain(unsigned ms) { (void)ms; }
#endif

int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!is_log(f)) return __real_vfprintf(f, fmt, ap);
#ifdef __PROSPERO__
    {
        va_list aq;
        va_copy(aq, ap);
        int n = vsnprintf(t_line, sizeof t_line, fmt, aq);
        va_end(aq);
        if (n >= 0 && (size_t)n < sizeof t_line && log_push(t_line, (size_t)n)) { atomic_fetch_add(&g_writes, 1); return n; }
    }
#endif
    uint64_t t0 = plat_now_ns();
    int r = __real_vfprintf(f, fmt, ap);
    account(t0);
    return r;
}

int __wrap_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __wrap_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int __wrap_fputs(const char *s, FILE *f) {
    if (!is_log(f)) return __real_fputs(s, f);
#ifdef __PROSPERO__
    if (log_push(s, strlen(s))) { atomic_fetch_add(&g_writes, 1); return 1; }
#endif
    uint64_t t0 = plat_now_ns();
    int r = __real_fputs(s, f);
    account(t0);
    return r;
}

int __wrap_fputc(int c, FILE *f) {
    if (!is_log(f)) return __real_fputc(c, f);
#ifdef __PROSPERO__
    { char ch = (char)c; if (log_push(&ch, 1)) { atomic_fetch_add(&g_writes, 1); return (unsigned char)c; } }
#endif
    uint64_t t0 = plat_now_ns();
    int r = __real_fputc(c, f);
    account(t0);
    return r;
}
