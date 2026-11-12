// BC7 and BC1 encoding of RGBA8 texels (the port's wrapper of bc7enc and rgbcx, tex/bc7enc: MIT). C interface.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bytes of BC7 blocks for w x h x depth texels: 16 per 4x4 block, rows of blocks, slices one after another.
size_t bc7_size(uint32_t w, uint32_t h, uint32_t depth);
// Encodes w x h texels per slice (rows of w * 4 bytes, `depth` slices one after another) into out (bc7_size bytes).
// perceptual: colour weights (sRGB textures); else every channel weighs the same (normals, masks, other data).
// quality: 0 (fastest) .. 4 (best, slowest); blocks past the edge repeat the last row / column.
void bc7_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t depth, int perceptual, int quality, uint8_t *out);
// BC1 (opaque: alpha ignored; 8 bytes per 4x4 block, rows of blocks, slices one after another), rgbcx's level 0..18.
size_t bc1_size(uint32_t w, uint32_t h, uint32_t depth);
void bc1_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t depth, int level, uint8_t *out);

#ifdef __cplusplus
}
#endif
