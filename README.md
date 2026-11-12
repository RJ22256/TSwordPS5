# TSword PS5

**TSword - the Tears of the Kingdom recompilation ([TotK Recomp](https://github.com/valentderah/totk-pc-recomp), MIT) - as a native title for a jailbroken PS5:** a raw title folder, `PPSA99778`, with its `eboot.bin`, that ShadowMountPlus puts on the home screen. It sits beside WSword (`PPSA99777`); the two don't share anything on the console.

> **Status: milestone 3 - the first link.** The eboot is TotK Recomp's runtime built for the PS5; it starts and stops where it needs the game code, which the PS5 export (milestone 4) will bring from your PC. See [the plan](docs/PLAN.md).

No game files, keys or game code come with TSword PS5. The game code is built on your PC from your own dump (the base game and the 1.2.1 or 1.4.3 update), as TotK Recomp does on PC.

## What you need

- A PS5 running homebrew: an enabler (etaHEN / kstuff), [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) loaded at boot, and an FTP server (ftpsrv on port 2121, or etaHEN's).
- (Later milestones) your own *Tears of the Kingdom* dump and your console's keys, for the PS5 export on your PC.

## Milestone 1: the console check

1. Download `TSwordPS5-PPSA99778.zip` from the [releases](../../releases) and unzip it.
2. Copy the `PPSA99778` folder to the console over FTP: `/data/homebrew/PPSA99778/`.
3. Reboot or let ShadowMountPlus pick it up: **TSword** appears on the home screen (green, beside WSword's blue). Start it.
4. It shows a lantern (the GPU working) and, in its window (OPTIONS / the touch pad shows and hides it), **TSword PS5 - console check**: how many checks passed and what's missing.
5. Send back `/data/homebrew/PPSA99778/tsword-check.txt` (over FTP). Quit from the window, or with the PS button.

The check looks at what TotK Recomp's runtime needs:

- **The GPU:** Vulkan 1.3 and the extensions its renderer uses - above all `VK_EXT_external_memory_host`, as it gives the game's memory to the GPU in place - on the PS5's RADV driver.
- **Memory:** the game's address space as TotK Recomp lays it out (guest address = host address, below 2^39): a 6 GB stack and heap block and a 64 GB alias region reserved, 3.5 GB of it backed, and some of it imported into Vulkan.
- **Write watch:** a read-only page written and the fault caught - how the runtime learns which pages the CPU changed.
- **Preemption:** a signal handled on a busy thread - how the runtime interrupts a game thread.
- **The CPU and the title folder.**

## Credits

- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh (MIT) - the recompilation this ports, and what it builds on (LAS Recomp, MacNativeGames, SavageWindRecomp, Ryujinx, deko3d...).
- The PS5 side is WSword PS5's: mihawk-99's [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (RADV for the console, its native tool and the PS5 Vulkan Template this started from) and the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk); the HDR10 scanout patch follows bizkut's SavageWindRecomp PS5 findings.
- The icon and backgrounds are WSword's artwork turned green.

TSword is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo or Sony. *The Legend of Zelda* and *Tears of the Kingdom* are trademarks of Nintendo.
