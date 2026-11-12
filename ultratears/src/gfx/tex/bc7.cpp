// BC7 and BC1 encoding of RGBA8 texels: bc7enc and rgbcx (tex/bc7enc, Rich Geldreich, MIT) block by block, with the
// port's C interface.
#include <pthread.h>
#include <string.h>

#include "bc7.h"
#include "bc7enc/bc7enc.h"
#include "bc7enc/rgbcx.h"

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void init(void) { bc7enc_compress_block_init(); rgbcx::init(); }   // their tables, once (read-only afterwards)

// The 4x4 texels of block (x, y) of a slice; past the edge, the last row / column repeats.
static void block_px(const uint8_t *slice, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t px[64]) {
    for (uint32_t j = 0; j < 4; j++) {
        uint32_t sy = y * 4 + j < h ? y * 4 + j : h - 1;
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t sx = x * 4 + i < w ? x * 4 + i : w - 1;
            memcpy(px + (j * 4 + i) * 4, slice + ((size_t)sy * w + sx) * 4, 4);
        }
    }
}

extern "C" size_t bc1_size(uint32_t w, uint32_t h, uint32_t depth) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (depth ? depth : 1) * 8;
}
extern "C" void bc1_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t depth, int level, uint8_t *out) {
    pthread_once(&g_once, init);
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    for (uint32_t z = 0; z < (depth ? depth : 1); z++)
        for (uint32_t y = 0; y < by; y++)
            for (uint32_t x = 0; x < bx; x++) {
                uint8_t px[64];
                block_px(rgba + (size_t)w * h * 4 * z, w, h, x, y, px);
                rgbcx::encode_bc1((uint32_t)level, out, px, false, false);   // 4-colour blocks: opaque, no punch-through
                out += 8;
            }
}

extern "C" size_t bc7_size(uint32_t w, uint32_t h, uint32_t depth) {
    return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (depth ? depth : 1) * 16;
}

extern "C" void bc7_encode(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t depth, int perceptual, int quality, uint8_t *out) {
    pthread_once(&g_once, init);
    bc7enc_compress_block_params p;
    bc7enc_compress_block_params_init(&p);
    if (!perceptual) bc7enc_compress_block_params_init_linear_weights(&p);
    p.m_uber_level = (uint32_t)(quality < 0 ? 0 : quality > BC7ENC_MAX_UBER_LEVEL ? BC7ENC_MAX_UBER_LEVEL : quality);
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    for (uint32_t z = 0; z < (depth ? depth : 1); z++) {
        const uint8_t *slice = rgba + (size_t)w * h * 4 * z;
        for (uint32_t y = 0; y < by; y++)
            for (uint32_t x = 0; x < bx; x++) {
                uint8_t px[64];
                block_px(slice, w, h, x, y, px);
                bc7enc_compress_block(out, px, &p);
                out += 16;
            }
    }
}
