# Texture helpers (copied as is)

| File | Origin | License |
|---|---|---|
| `nvn_deswizzle.c/.h` | MacNativeGames `games/totk/third_party/d2r_gfx/gfx/tex` @b75be8c | MIT (`LICENSE-MacNativeGames`) |
| `astc.c/.h` | LASRecomp `runtime/nvn` @da35b83 | MIT (`LICENSE-LASRecomp`) |
| `bc7enc/bc7enc.cpp/.h` | richgel999/bc7enc_rdo @b943862 (2026-07-30) | MIT (alternative A of `bc7enc/LICENSE`; Rich Geldreich) |
| `bc7enc/rgbcx.cpp/.h`, `rgbcx_table4*.h` | the same repository and commit (BC1 encoder) | MIT (the same `bc7enc/LICENSE`) |

`nvn_deswizzle` uses `../nvn/nvn_tex.h` (also from MacNativeGames). ASTC decoding on the CPU is temporary: ASTC is
transcoded to BC7 (`bc7.cpp`, the port's own wrapper; cache: `../vkb_bc7.c`), filled at build time from the RomFS in the
next step.
