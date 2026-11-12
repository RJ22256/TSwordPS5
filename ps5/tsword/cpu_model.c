// (TSword PS5) __builtin_cpu_init / __builtin_cpu_supports' data, which clang's runtime library (compiler-rt's
// cpu_model) provides elsewhere and the payload SDK doesn't: __cpu_model's feature word, filled from CPUID at start.
// The bits are compiler-rt's ProcessorFeatures (llvm X86TargetParser.def) below 32; the runtime asks for sse4.1,
// sse4.2, popcnt, avx2, fma and bmi2 (runtime/main.c's self-test, sha256.c), and the console's Zen 2 has them all.
#include <stdint.h>

struct __processor_model {
    unsigned int __cpu_vendor, __cpu_type, __cpu_subtype;
    unsigned int __cpu_features[1];
} __cpu_model;
unsigned int __cpu_features2[3];

enum { F_CMOV = 0, F_MMX, F_POPCNT, F_SSE, F_SSE2, F_SSE3, F_SSSE3, F_SSE4_1, F_SSE4_2, F_AVX, F_AVX2, F_SSE4_A,
       F_FMA4, F_XOP, F_FMA, F_AVX512F, F_BMI, F_BMI2, F_AES, F_PCLMUL };

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
    __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

__attribute__((constructor(101))) int __cpu_indicator_init(void) {
    if (__cpu_model.__cpu_vendor) return 0;
    unsigned r[4], f = 0;
    cpuid(0, 0, r);
    unsigned max = r[0];
    __cpu_model.__cpu_vendor = r[1] == 0x68747541 ? 2 : 3;   // (AMD: "Auth"; compiler-rt's VENDOR_AMD, else OTHER)
    cpuid(1, 0, r);
    unsigned c = r[2], d = r[3];
    if (d >> 15 & 1) f |= 1u << F_CMOV;
    if (d >> 23 & 1) f |= 1u << F_MMX;
    if (d >> 25 & 1) f |= 1u << F_SSE;
    if (d >> 26 & 1) f |= 1u << F_SSE2;
    if (c & 1) f |= 1u << F_SSE3;
    if (c >> 1 & 1) f |= 1u << F_PCLMUL;
    if (c >> 9 & 1) f |= 1u << F_SSSE3;
    if (c >> 12 & 1) f |= 1u << F_FMA;
    if (c >> 19 & 1) f |= 1u << F_SSE4_1;
    if (c >> 20 & 1) f |= 1u << F_SSE4_2;
    if (c >> 23 & 1) f |= 1u << F_POPCNT;
    if (c >> 25 & 1) f |= 1u << F_AES;
    int avx_os = (c >> 27 & 1) && (c >> 28 & 1);   // (OSXSAVE and AVX; the console's kernel saves the YMM state)
    if (avx_os) f |= 1u << F_AVX;
    if (max >= 7) {
        cpuid(7, 0, r);
        if (avx_os && (r[1] >> 5 & 1)) f |= 1u << F_AVX2;
        if (r[1] >> 3 & 1) f |= 1u << F_BMI;
        if (r[1] >> 8 & 1) f |= 1u << F_BMI2;
    }
    cpuid(0x80000001u, 0, r);
    if (r[2] >> 6 & 1) f |= 1u << F_SSE4_A;
    __cpu_model.__cpu_features[0] = f;
    return 0;
}
