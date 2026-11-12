// The key of a surface in the BC7 cache (src/gfx/vkb_bc7.c, tools/bc7build): shared by the game and the build, which
// must compute the same key for the same texture whichever way it was laid out.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t a, b; } bc7_key;
// blocks: the surface's ASTC blocks in rows (n bytes; a 3D level: its slices one after another); w x h texels, depth
// slices, block size bw x bh. The colour space is not part of it: the BC7 bytes serve an sRGB and a UNORM view alike.
bc7_key bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh);
