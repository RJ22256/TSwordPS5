# lsfg-vk in TSword PS5

TSword PS5 uses this code for frame generation. The frame generation itself comes from the player's own copy of Lossless Scaling's `Lossless.dll`; none is included.

The code was taken from [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk), by PancakeTAS and contributors, through WSword (the same copy). Only two parts of it are here:

- `lsfg-vk-common`: the Vulkan wrappers.
- `lsfg-vk-backend`: reads the shaders out of `Lossless.dll` and runs the frame generation passes.

The Vulkan layer, the user interface and the configuration code were left out.

## Version and licence

The code is from GitHub PR #544, head `f4f7444` (5 August 2026). That PR is built on maintainer commit `8b0da26` (28 June 2026).

Its licence is **GPL-3.0** (`LICENSE.md`). This is the last version under that licence: on 27 August 2026 the project moved to CC BY-NC-ND 4.0, which does not allow modified copies.

**What this means for TSword PS5's builds:** a title built with this code is a combined work under GPL-3.0; anyone given it must be able to get the source (this repository). The files that link it carry the GPL-3.0 notice: `ps5/ut/ui/lsfg.cpp` and `ps5/ut/ui/framegen.c`.

## Changes (made for WSword, kept here)

Every change is marked `(WSword)` in the code. TSword uses the same copy, unchanged.

### `vulkan.hpp` / `vulkan.cpp`
- **A new constructor** that uses an existing device and one of its queues exactly as they are. No second device is made and no memory is shared. The caller chooses the queue family and whether FP16 is used.
- **Optional extension functions.** The file-descriptor functions are no longer required. The timeline-semaphore functions fall back to their Vulkan 1.2 core names.
- **No `vk_layer.h`.** The one declaration used from it, `PFN_vkSetDeviceLoaderData`, is written out in the file instead.

### `lsfgvk.hpp` / `lsfgvk.cpp`
- **`Instance(ExternalDevice, …)`** creates an instance on the application's device.
- **`openLocalContext`** opens a context whose images and timeline semaphore belong to the instance. Its source images can also be copied into, because they are created with `TRANSFER_DST`. All images start in `GENERAL` layout.
- **New accessors:**
  - `images`, `nextSignal` and `frameIndex`
  - `lowPrecision` and `persistCache`
- **`checkShaderDll`** checks a DLL without needing a GPU. It reports whether the full-precision and FP16 shader sets are complete. 
- **Context construction changed.** A context is now built from images and a semaphore that were made beforehand. `openContext`, the original entry point that imports file descriptors, still works the same way.

## How TSword uses it

`ps5/ut/ui/framegen.c`, called by the present thread (`ultratears/src/vk/present.c`). On each new game frame:

1. The game's picture is copied into `source[frame % 2]`; that submission signals the timeline semaphore at `nextSignal()`.
2. `scheduleFrames` submits the backend's passes (under the queue's lock: the game's replayer shares the queue).
3. Each generated frame `i` is copied, once its value `nextSignal() + 1 + i` is reached, into an image the present thread shows as it shows the game's, and presented in turn; the real frame is presented last. The frames are spaced evenly over the time between two of the game's frames.
