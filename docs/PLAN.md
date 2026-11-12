# TSword PS5: the plan

TotK Recomp for PC is three parts, as WSword was: the **runtime** (`runtime/`: the host services, the NVN front end
and its Vulkan back end, the menu), the **game code** (the game's ARM64 code lifted to C by `recomp/gen.py` and
compiled on the player's PC from their own dump), and the **game files** (the dump's RomFS, with the patch overlays
`tools/make_romfs_patches.py` writes). On the PS5 they become:

| Part | On the PS5 |
| --- | --- |
| the runtime | `eboot.bin`, built here by CI from `tsword/` (TotK Recomp's runtime, synced, with the console's platform layer), released ready to copy |
| the game code | built **on the PC** by TSword's PS5 export (the same lift, compiled with clang for the console's ABI and its FreeBSD libc headers, linked for the title's in-process loader) - it can't be shipped |
| the shaders | translated **on the PC** too: TotK Recomp translates shaders with Ryujinx's translator in a separate .NET process, which a PS5 title can't start. The export runs `prepare_shaders.py` there and packs the SPIR-V (`shader-cache.pack`); on the console a shader missing from the pack is logged and its draws skipped |
| the game files | the dump's RomFS and the overlays, copied by the same export |

## What's different from WSword

- **Memory.** Guest address = host address, below 2^39, laid out by `runtime/memory.c` (a 6 GB stack and heap
  block from 4 GB up, a 64 GB alias region): the console's platform layer reserves these as direct-memory ranges and
  commits them page by page (as WSword's does at 0x7000000000).
- **The GPU gets the game's memory in place** (`VK_EXT_external_memory_host`): RADV on the PS5 has it; the check
  imports guest memory to make sure.
- **Write watch** (which pages the CPU wrote, for the uploads): Windows' `MEM_WRITE_WATCH`; on the console,
  write-protection and its faults.
- **Asynchronous preemption** suspends a game thread (Win32) or signals it (Linux): on the console a signal, if the
  check shows one handled on a busy thread, else the runtime's own safe points (`TOTK_ASYNC_PREEMPT=2`).
- **The 2^39 limit and the alias region's 64 GB** are reservations only, not memory: the check reserves them to be sure.

## Milestones

1. **The console check** (v0.1.0-m1): the title builds, signs and boots from `PPSA99778`; its report says what the
   runtime will find there (above).
2. **The runtime cross-built**: `tsword/` (TotK Recomp's `runtime/`, `tools/sync-tsword.sh`) compiled with the payload
   SDK into `libtsword.a`, with its dependencies for the console (SDL3 subset - WSword's, libopus, libvpx, zstd, VMA);
   `runtime/platform/ps5.c` from `linux.c`; the list of what's still missing.
3. **First link**: the title is TotK Recomp's runtime, up to where it looks for the game code.
4. **The game code loader and the PS5 export** (the PC side: a script beside TotK Recomp's setup that lifts, builds the
   game code for the console, translates and packs the shaders, and writes the PPSA folder's files).
5. **First boot of the game**, then the renderer on RADV, audio, saves, movies (libvpx), performance - and WSword's
   console features (the PS5 menu look, DualSense, HDR10, FSR 3, resolution modes).

Each milestone ends with a build on the player's console and its log.
