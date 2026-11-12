# WSword's patches to RADV (PS5_Mesa)

Applied by `.github/workflows/build.yml` to PS5_Mesa at PS5_Vulkan's pinned revision, committed with fixed names and
dates (so the revision `build-radv.sh` builds and records is the same every run), before RADV is built.

- `0001-videoout-hdr10.patch`: HDR10 scanout in VideoOut's VK_KHR_display (`src/vulkan/wsi/wsi_common_videoout.c`).
  For a title whose `param.json` `"attribute"` has `0x20000000`, the surface also offers `A2B10G10R10` /
  `A2R10G10B10` with `VK_COLOR_SPACE_HDR10_ST2084_EXT`; such a swapchain's buffers are registered with VideoOut's
  BT.2020 PQ pixel formats (`0x8100070422000000` with R in bits 0-9, `0x8100070400000000` with B there), and the
  console sends HDR to the TV. The attribute bit and the pixel formats are as bizkut's SavageWindRecomp PS5 port
  found and measured them (its `ps5/FINDINGS.md`, 2026-10-28). MIT, as the file it changes.
