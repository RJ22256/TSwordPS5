#include <unistd.h>
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include "ps5platform/kernel.h"
#include "ps5platform/shm.h"
// shadow protection per 16 KiB page of [SH_LO, SH_LO + SH_N pages)
#define SH_LO 0x0800000000ull
#define SH_N (1u << 22)   // 64 GiB
static _Atomic uint8_t sh[SH_N];
_Atomic long n_mprotect, n_query;
static void sh_set(uint64_t a, uint64_t n, int prot) {
    for (uint64_t p = a & ~0x3fffull; p < a + n; p += 0x4000) if (p >= SH_LO && (p - SH_LO) / 0x4000 < SH_N) sh[(p - SH_LO) / 0x4000] = (uint8_t)prot;
}
static int lin(int prot) { return (prot & 1 ? PROT_READ : 0) | (prot & 2 ? PROT_WRITE : 0); }
static atomic_flag g_sl = ATOMIC_FLAG_INIT;   // (the kernel changes a mapping and answers queries atomically)
static void sl_lock(void) { while (atomic_flag_test_and_set(&g_sl)) {} }
static void sl_unlock(void) { atomic_flag_clear(&g_sl); }
int32_t sceKernelMprotect(const void *a, size_t n, int prot) {
    n_mprotect++;
    sl_lock();
    int r = mprotect((void *)a, n, lin(prot));
    if (!r) sh_set((uint64_t)a, n, prot);
    sl_unlock();
    return r ? -1 : 0;
}
int32_t sceKernelQueryMemoryProtection(void *a, void **s, void **e, uint32_t *prot) {
    n_query++;
    uint64_t p = (uint64_t)a & ~0x3fffull;
    if (p < SH_LO || (p - SH_LO) / 0x4000 >= SH_N) return -1;
    sl_lock();
    uint8_t v = sh[(p - SH_LO) / 0x4000];
    uint64_t q = p + 0x4000;   // the run of the same protection, as the kernel reports a map entry
    while ((q - SH_LO) / 0x4000 < SH_N && sh[(q - SH_LO) / 0x4000] == v && q - p < (1ull << 24)) q += 0x4000;
    sl_unlock();
    *s = (void *)p, *e = (void *)q, *prot = v;
    return 0;
}
int ps5_vrange_reserve_at(void *a, size_t n) { return mmap(a, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) == a ? 0 : -1; }
int ps5_vrange_reserve(size_t n, void *hint, size_t al, void **b) { (void)hint, (void)al; void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); if (p == MAP_FAILED) return -1; *b = p; return 0; }
int ps5_vrange_release(void *b, size_t n) { return munmap(b, n); }
// commit: as the console, the protection asked for (CPU only); the GPU bits come with an "import" (the test's)
int ps5_vrange_commit(void *a, size_t n, int prot) { sl_lock(); int r = mprotect(a, n, lin(prot)); if (!r) sh_set((uint64_t)a, n, prot); sl_unlock(); return r ? -1 : 0; }
int ps5_vrange_decommit(void *a, size_t n) { if (mmap(a, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != a) return -1; sh_set((uint64_t)a, n, 0); return 0; }
// runtime stand-ins
#include <stdbool.h>
bool rt_fault_watch(uint64_t a, uint64_t pc) { (void)a, (void)pc; return false; }
void rt_fault_report(const char *w, uint64_t fa, uint64_t pc, uint64_t lr) { fprintf(stderr, "REAL FAULT %s at %llx pc %llx\n", w, (unsigned long long)fa, (unsigned long long)pc); _exit(3); }
const char *rt_host_fn_name(uint64_t pc, uint64_t *o) { (void)pc, (void)o; return NULL; }
void tsword_fatal_notice(void) {}

// (0.4.10) ps5.c drains the log writer (runtime/logcost.c) before a fatal notice; not linked into the test
void runtime_log_drain(unsigned ms) { (void)ms; }
// (0.5.3) ps5.c's CPU placement: the console's affinity calls (the test has nothing to place)
int scePthreadSetaffinity(pthread_t t, uint64_t mask) { (void)t; (void)mask; return 0; }
int scePthreadGetaffinity(pthread_t t, uint64_t *mask) { (void)t; *mask = 0; return -1; }
