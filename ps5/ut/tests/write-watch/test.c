#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform.h"
#include "rt.h"
#include "ps5platform/kernel.h"
extern _Atomic long n_mprotect, n_query;
#define POOL (GUEST_LO + 0x40000000ull + 0x3000)   // a pool not 16 KiB aligned
#define POOL_SIZE (48ull << 20)
#define NP4 (POOL_SIZE / 4096)
int wp_debug_state(uint64_t a);
void wp_debug_dump(uint64_t a);
static unsigned dbgprot(uint64_t a) { void *s, *e; uint32_t p = 0; sceKernelQueryMemoryProtection((void *)a, &s, &e, &p); return p; }
static _Atomic uint64_t g_seq = 1, done_[NP4];
static _Atomic int stop;
static _Atomic long writes;
static void *writer(void *arg) {
    unsigned r = (unsigned)(uintptr_t)arg * 2654435761u;
    plat_thread_start();
    while (!stop) {
        r = r * 1103515245u + 12345u;
        uint64_t pg;
        // a few hot pages (written all the time), the rest now and then
        if ((r >> 8) % 4 == 0) pg = (r >> 12) % 64; else pg = (r >> 12) % NP4;
        uint64_t s = atomic_fetch_add(&g_seq, 1);
        volatile uint64_t *p = (uint64_t *)(POOL + pg * 4096 + ((r >> 3) % 512) * 8);
        *p = s;
        uint64_t d = atomic_load(&done_[pg]);
        while (d < s && !atomic_compare_exchange_weak(&done_[pg], &d, s)) {}
        writes++;
        if ((r & 0xff) == 0) usleep(50);
    }
    return NULL;
}
int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 6, looks = argc > 2 ? atoi(argv[2]) : 3000;
    if (!plat_reserve_guest(GUEST_LO, GUEST_HI)) { puts("reserve failed"); return 1; }
    plat_crash_handler_install();
    plat_commit(POOL, POOL_SIZE);
    // the "import": GPU bits added, as RADV's grant
    sceKernelMprotect((void *)(POOL & ~0x3fffull), (POOL + POOL_SIZE - (POOL & ~0x3fffull) + 0x3fff) & ~0x3fffull, 0x33);
    static uint64_t covered[NP4], snap[NP4], after[NP4], pages[512];
    static uint8_t rep[NP4];
    uint64_t pg;
    int n = plat_written_pages(POOL, POOL_SIZE, pages, 512, &pg);   // arm (everything reported)
    while (n == 512) { uint64_t lo = pages[511] + pg; n = plat_written_pages(lo, POOL + POOL_SIZE - lo, pages, 512, &pg); }
    pthread_t t[32];
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, writer, (void *)(uintptr_t)(i + 1));
    long bad = 0, reported = 0, kernel_ok = 0;
    for (int k = 0; k < looks; k++) {
        for (uint64_t i = 0; i < NP4; i++) snap[i] = atomic_load(&done_[i]);
        memset(rep, 0, sizeof rep);
        uint64_t lo = POOL, hi = POOL + POOL_SIZE;
        for (;;) {
            n = plat_written_pages(lo, hi - lo, pages, 512, &pg);
            if (n < 0) { printf("watch off at look %d\n", k); return 2; }
            for (int i = 0; i < n; i++)
                for (uint64_t q = pages[i] < POOL ? POOL : pages[i]; q < pages[i] + pg && q < hi; q += 4096) rep[(q - POOL) / 4096] = 1;
            reported += n;
            if (n < 512) break;
            lo = pages[n - 1] + pg;
        }
        // a page reported covers the writes done by the time the look returned (the caller reads after it)
        for (uint64_t i = 0; i < NP4; i++) after[i] = atomic_load(&done_[i]);
        for (uint64_t i = 0; i < NP4; i++) {
            if (rep[i]) covered[i] = after[i];
            else if (snap[i] > covered[i]) { if (bad++ < 10) printf("MISSED: look %d page %llu state %d prot %x write %llu covered %llu\n", k, (unsigned long long)i, wp_debug_state(POOL + i * 4096), dbgprot(POOL + i * 4096), (unsigned long long)snap[i], (unsigned long long)covered[i]); }
        }
        if (k % 10 == 0) {   // a "file read" into the pool: the kernel writes it (fails with EFAULT on a read-only page)
            int fd[2];
            if (pipe(fd)) return 1;
            char buf[20000];
            memset(buf, 7, sizeof buf);
            write(fd[1], buf, sizeof buf);
            uint64_t at = POOL + (uint64_t)(k * 7919 % (NP4 - 8)) * 4096;
            plat_commit(at, sizeof buf);   // (guest_prepare_write)
            uint64_t s = atomic_fetch_add(&g_seq, 1);
            ssize_t r = read(fd[0], (void *)at, sizeof buf);
            if (r != (ssize_t)sizeof buf) { printf("kernel write failed: %zd\n", r); return 4; }
            for (uint64_t q = at; q < at + sizeof buf; q += 4096) { uint64_t i = (q - POOL) / 4096, d = done_[i]; while (d < s && !atomic_compare_exchange_weak(&done_[i], &d, s)) {} }
            close(fd[0]), close(fd[1]);
            kernel_ok++;
        }
        // the GPU bits must survive every protection change
        uint32_t prot; void *s_, *e_;
        sceKernelQueryMemoryProtection((void *)(POOL + 0x100000), &s_, &e_, &prot);
        if ((prot & 0x30) != 0x30) { printf("GPU bits lost: %x\n", prot); return 5; }
        usleep(2000);
    }
    stop = 1;
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    printf("%d looks, %ld writes by %d threads, %ld pages reported, %ld kernel reads; missed %ld; mprotect %ld query %ld\n",
           looks, (long)writes, threads, reported, kernel_ok, bad, (long)n_mprotect, (long)n_query);
    return bad != 0;
}
