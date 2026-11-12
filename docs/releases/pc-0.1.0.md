**TSword Setup 0.1.0 (Windows)** - *Tears of the Kingdom* for PC ([TotK Recomp](https://github.com/valentderah/totk-pc-recomp) 0.4.0, MIT) with **Export for PS5**: it builds the game for TSword on a jailbroken PS5 too. Made for **TSword PS5 v0.3.0-m4** (the game code it builds is what that release's `eboot.bin` loads).

No game files are included. You need your own dump of *Tears of the Kingdom* (base game, XCI or NSP) and the **1.4.3** (or 1.2.1) update, plus your console's `prod.keys`.

### Install

1. Unzip `TSword-Setup-pc-0.1.0-windows.zip` anywhere and start `TSword-Setup.exe` (SmartScreen: More info, Run anyway).
2. Pick the base game, the update, the keys, the language and an install folder. Leave **Export for PS5** ticked.
3. Start. The PC game is built first (30+ minutes), then the PS5 export (about as long again).
4. Your install folder now has `totk.exe` (the PC game, as TotK Recomp) and a **PS5** folder.

### To the PS5

Copy everything inside the **PS5** folder into `/data/homebrew/PPSA99778/` on the console (FTP), beside the `eboot.bin` of TSword PS5 v0.3.0-m4. See that release's notes.

### Requirements

As TotK Recomp: Windows 10 or later, a CPU with AVX2, a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, 16 GB RAM, about 40 GB free during setup.

### Licences

TotK Recomp's (MIT) and its third-party notices; `ps5/include`: FreeBSD's C library headers as the PS5 payload SDK installs them and zstd's headers (BSD-style licences, each file's own notice).
