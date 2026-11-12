#include <stdio.h>
#include <stdint.h>
#include "platform.h"
#include "rt.h"
#include "ps5platform/kernel.h"
#define POOL (GUEST_LO + 0x40000000ull + 0x3000)
static int look(uint64_t a, uint64_t n, const char *what) {
    uint64_t pages[512], pg; int k = plat_written_pages(a, n, pages, 512, &pg);
    printf("%s: %d:", what, k); for (int i = 0; i < k && i < 8; i++) printf(" %llx", (unsigned long long)(pages[i] - (POOL & ~0x3fffull))); puts("");
    return k;
}
int main(void) {
    plat_reserve_guest(GUEST_LO, GUEST_HI);
    plat_crash_handler_install();
    plat_commit(POOL, 1 << 20);
    look(POOL, 1 << 20, "arm");
    look(POOL, 1 << 20, "again (none)");
    *(volatile int *)(POOL + 0x5000) = 1;
    look(POOL, 1 << 20, "after write at +0x5000");
    look(POOL, 1 << 20, "again (none)");
    *(volatile int *)(POOL + 0x5000) = 2;
    *(volatile int *)(POOL + 0x9000) = 2;
    look(POOL, 1 << 20, "two writes");
    for (int i = 0; i < 5; i++) { *(volatile int *)(POOL + 0x5000) = 3; look(POOL, 1 << 20, "hot?"); }
    look(POOL, 1 << 20, "no write");
}
