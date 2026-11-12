// The key of a surface in the BC7 cache: two 64-bit lanes over the bytes, the size and block size mixed in first.
#include <string.h>

#include "bc7key.h"

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v * 0x9E3779B97F4A7C15ull; h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full; return h; }
static uint64_t fin(uint64_t h) { h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33; return h; }

bc7_key bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh) {
    uint64_t a = 0x5432102030405060ull, b = 0x0123456789ABCDEFull;
    uint64_t p0 = (uint64_t)w << 32 | h, p1 = (uint64_t)depth << 32 | bw << 16 | bh << 8;
    a = mix(a, p0); a = mix(a, p1); b = mix(b, p1); b = mix(b, p0);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        memcpy(&x, blocks + i, 8); memcpy(&y, blocks + i + 8, 8);
        a = mix(a, x); b = mix(b, y);
    }
    for (; i < n; i++) { a = mix(a, blocks[i]); b = mix(b, (uint64_t)blocks[i] << 8); }
    bc7_key k = {fin(a ^ n), fin(b ^ (n << 1))};
    if (!k.a && !k.b) k.a = 1;   // 0,0 marks an empty slot in the cache's index
    return k;
}
