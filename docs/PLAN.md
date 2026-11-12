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

1. **The console check** - done (v0.1.0-m1): 58 of 58 on the player's console. RADV has everything the runtime uses,
   `VK_EXT_external_memory_host` included (imports at 16 KiB alignment); the 6 GB block reserved at 0x440000000 and the
   alias region at 0x1000000000, 3.5 GB backed and imported into Vulkan; write-protection faults and signals to busy
   threads work. No ASTC on the GPU: the runtime's compute decoder (vk_astc.c) does it, as on most PC GPUs.
2. **The runtime cross-built** - done: `tsword/` (TotK Recomp 0.4.0 at 7a37fc3, `tools/sync-tsword.sh`, with
   `ps5/tsword-patches` and `ps5/tsword-overlay`) compiled by `ps5/tsword/configure.py` into `libtsword.a` (114 objects),
   with opus 1.5.2, libvpx 1.15.2 (VP9 decoder, nasm) and zstd 1.5.7 for the console (`ps5/tsword/deps.sh`). The
   console's platform layer, `runtime/platform/ps5.c`, is linux.c with: _umtx_op for the futex; guest memory, thread
   and fiber stacks on direct memory (ps5platform's ranges, stacks committed as they grow); 16 KiB protection pages;
   FreeBSD's signal contexts; no write watch (the back end runs in its unified-memory mode: RADV calls the PS5's GPU
   discrete, `ps5/tsword-patches/0001` says otherwise); no child processes. What's still needed from outside
   (`ps5/tsword/missing.py`, in each CI run's summary): 154 Vulkan entry points (RADV), 61 SDL3 functions (43 in WSword
   PS5's subset), `__cpu_model`, and the install side - romfs, the patch sets, the layout and version tables - which
   comes with the game code.
3. **First link**: the title is TotK Recomp's runtime, up to where it looks for the game code.
4. **The game code loader and the PS5 export** (the PC side: a script beside TotK Recomp's setup that lifts, builds the
   game code for the console, translates and packs the shaders, and writes the PPSA folder's files).
5. **First boot of the game**, then the renderer on RADV, audio, saves, movies (libvpx), performance - and WSword's
   console features (the PS5 menu look, DualSense, HDR10, FSR 3, resolution modes).

Each milestone ends with a build on the player's console and its log.
