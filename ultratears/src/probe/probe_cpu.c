// Phase 2 probe: the recompiled TOTK modules load, relocate and run. Calls exported SDK functions (libc, math) through
// the recompiled code and compares the results with the host's libc; times a few of them.
//   probe_cpu <exefs dir>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle.h"
#include "../runtime/rt.h"

static Cpu *g_cpu;
static uint64_t g_stack_top;
static int g_fail, g_pass;

static void reset_cpu(void) {
    memset(g_cpu, 0, sizeof *g_cpu);
    g_cpu->sp = g_stack_top;
    g_cpu->tpidrro = (uint64_t)(uintptr_t)hle_alloc(0x200);
}

static uint64_t call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t f = loader_find_symbol(name);
    if (!f) { fprintf(stderr, "missing %s\n", name); g_fail++; return 0; }
    g_cpu->x[0] = a0; g_cpu->x[1] = a1; g_cpu->x[2] = a2; g_cpu->x[3] = a3;
    g_cpu->sp = g_stack_top;
    guest_call(g_cpu, f);
    return g_cpu->x[0];
}

static float callf(const char *name, float a, float b) {
    uint64_t f = loader_find_symbol(name);
    if (!f) { fprintf(stderr, "missing %s\n", name); g_fail++; return 0; }
    memset(g_cpu->v, 0, 2 * sizeof g_cpu->v[0]);
    memcpy(&g_cpu->v[0], &a, 4); memcpy(&g_cpu->v[1], &b, 4);
    g_cpu->sp = g_stack_top;
    guest_call(g_cpu, f);
    float r; memcpy(&r, &g_cpu->v[0], 4);
    return r;
}

static double calld(const char *name, double a, double b) {
    uint64_t f = loader_find_symbol(name);
    if (!f) { fprintf(stderr, "missing %s\n", name); g_fail++; return 0; }
    memset(g_cpu->v, 0, 2 * sizeof g_cpu->v[0]);
    memcpy(&g_cpu->v[0], &a, 8); memcpy(&g_cpu->v[1], &b, 8);
    g_cpu->sp = g_stack_top;
    guest_call(g_cpu, f);
    double r; memcpy(&r, &g_cpu->v[0], 8);
    return r;
}

static void check(const char *what, int ok) {
    if (ok) g_pass++; else g_fail++;
    printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
}

// A host function the guest calls back (qsort comparator): int cmp(const int *a, const int *b).
static void host_cmp(Cpu *c) {
    int a = *(int *)(uintptr_t)c->x[0], b = *(int *)(uintptr_t)c->x[1];
    c->x[0] = (uint64_t)(int64_t)(a < b ? -1 : a > b);
}

static int near_f(float a, float b) { return fabsf(a - b) <= 1e-6f * fmaxf(1.0f, fabsf(b)); }
static int near_d(double a, double b) { return fabs(a - b) <= 1e-14 * fmax(1.0, fabs(b)); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: probe_cpu <exefs dir>\n"); return 2; }
    plat_init();
    plat_crash_handler_install();
    if (!plat_reserve_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reserve the guest window %llx-%llx\n", GUEST_LO, GUEST_HI);
    uint64_t t0 = plat_now_ns();
    loader_load(argv[1]);
    dispatch_init();
    printf("load+relocate: %.1f ms\n", (plat_now_ns() - t0) / 1e6);

    g_cpu = _aligned_malloc(sizeof(Cpu), 64);
    g_stack_top = (uint64_t)(uintptr_t)hle_alloc(1 << 20) + (1 << 20);
    reset_cpu();

    printf("libc through recompiled sdk:\n");
    char *s = hle_alloc(64);
    strcpy(s, "Tears of the Kingdom");
    check("strlen", call("strlen", (uintptr_t)s, 0, 0, 0) == strlen(s));
    char *t = hle_alloc(64);
    strcpy(t, "Tears of the Kingdon");
    check("memcmp <", (int)call("memcmp", (uintptr_t)s, (uintptr_t)t, 20, 0) < 0);
    check("strcmp ==", (int)call("strcmp", (uintptr_t)s, (uintptr_t)s, 0, 0) == 0);
    char *num = hle_alloc(32);
    strcpy(num, "-123456789");
    check("strtol", (long)call("strtol", (uintptr_t)num, 0, 10, 0) == -123456789L);
    uint8_t *src = hle_alloc(4096), *dst = hle_alloc(4096);
    for (int i = 0; i < 4096; i++) src[i] = (uint8_t)(i * 7 + 3);
    call("memcpy", (uintptr_t)dst + 3, (uintptr_t)src + 1, 3001, 0);
    check("memcpy (unaligned 3001 bytes)", !memcmp(dst + 3, src + 1, 3001));
    call("memset", (uintptr_t)dst, 0xab, 4096, 0);
    int all = 1;
    for (int i = 0; i < 4096; i++) all &= dst[i] == 0xab;
    check("memset", all);

    printf("math (float/double, NEON/FP registers):\n");
    float fx[] = {0.5f, 1.25f, -2.0f, 3.14159f, 10.0f};
    int fok = 1;
    for (int i = 0; i < 5; i++) {
        fok &= near_f(callf("sinf", fx[i], 0), sinf(fx[i]));
        fok &= near_f(callf("cosf", fx[i], 0), cosf(fx[i]));
        fok &= near_f(callf("expf", fx[i], 0), expf(fx[i]));
        fok &= near_f(callf("atan2f", fx[i], 0.75f), atan2f(fx[i], 0.75f));
        fok &= near_f(callf("powf", fabsf(fx[i]), 1.7f), powf(fabsf(fx[i]), 1.7f));
        fok &= callf("sqrtf", fabsf(fx[i]), 0) == sqrtf(fabsf(fx[i]));
    }
    check("sinf cosf expf atan2f powf sqrtf (5 inputs)", fok);
    int dok = 1;
    for (int i = 0; i < 5; i++) {
        dok &= near_d(calld("sin", fx[i], 0), sin(fx[i]));
        dok &= near_d(calld("exp", fx[i], 0), exp(fx[i]));
        dok &= near_d(calld("pow", fabs(fx[i]), 2.5), pow(fabs(fx[i]), 2.5));
    }
    check("sin exp pow (double, 5 inputs)", dok);

    printf("formatting and callbacks:\n");
    char *out = hle_alloc(128), *fmt = hle_alloc(64);
    strcpy(fmt, "%d|%x|%s");
    {  // varargs (AAPCS64: in registers x3..x5 here)
        uint64_t f = loader_find_symbol("snprintf");
        g_cpu->x[4] = 0xbeef; g_cpu->x[5] = (uintptr_t)s;
        g_cpu->x[0] = (uintptr_t)out; g_cpu->x[1] = 128; g_cpu->x[2] = (uintptr_t)fmt; g_cpu->x[3] = 42;
        g_cpu->sp = g_stack_top;
        guest_call(g_cpu, f);
    }
    printf("    snprintf -> \"%s\"\n", out);
    check("snprintf %d|%x|%s", !strcmp(out, "42|beef|Tears of the Kingdom"));
    int *arr = hle_alloc(1000 * sizeof(int));
    for (int i = 0; i < 1000; i++) arr[i] = (i * 7919) % 1000 - 500;
    uint64_t cmp = hle_register("probe_host_cmp", host_cmp);
    call("qsort", (uintptr_t)arr, 1000, sizeof(int), cmp);
    int sorted = 1;
    for (int i = 1; i < 1000; i++) sorted &= arr[i - 1] <= arr[i];
    check("qsort with a host comparator (guest -> host)", sorted);

    printf("timing (recompiled vs host):\n");
    uint8_t *big_src = hle_alloc(32 << 20), *big_dst = hle_alloc(32 << 20);
    memset(big_src, 1, 32 << 20);
    t0 = plat_now_ns();
    for (int i = 0; i < 20; i++) call("memcpy", (uintptr_t)big_dst, (uintptr_t)big_src, 32 << 20, 0);
    double g_ms = (plat_now_ns() - t0) / 1e6;
    t0 = plat_now_ns();
    for (int i = 0; i < 20; i++) memcpy(big_dst, big_src, 32 << 20);
    double h_ms = (plat_now_ns() - t0) / 1e6;
    printf("  memcpy 20 x 32 MiB: recompiled %.1f ms (%.1f GB/s), host %.1f ms\n", g_ms, 20 * 32 / 1024.0 / (g_ms / 1e3), h_ms);
    uint64_t fsin = loader_find_symbol("sinf");
    volatile float acc = 0;
    t0 = plat_now_ns();
    for (int i = 0; i < 1000000; i++) {
        float a = i * 1e-5f;
        memcpy(&g_cpu->v[0], &a, 4);
        g_cpu->sp = g_stack_top;
        guest_call(g_cpu, fsin);
        float r; memcpy(&r, &g_cpu->v[0], 4);
        acc += r;
    }
    g_ms = (plat_now_ns() - t0) / 1e6;
    t0 = plat_now_ns();
    for (int i = 0; i < 1000000; i++) acc += sinf(i * 1e-5f);
    h_ms = (plat_now_ns() - t0) / 1e6;
    printf("  sinf x 1e6: recompiled %.1f ms (%.1f ns/call), host %.1f ms\n", g_ms, g_ms, h_ms);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}

// No Horizon host in this probe: a system call means the tested code reached the OS.
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("svc 0x%x at %llx (no Horizon host in probe_cpu)\n", imm, (unsigned long long)pc);
}
