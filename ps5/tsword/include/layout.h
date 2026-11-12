// (TSword PS5) The runtime's layout.h on the console. On PC tools/build_game.py compiles the runtime with the
// generated/<version>/layout.h of the game it builds; the console's runtime is one eboot for every game version, so
// it has what all of them share: the main module at 0x7000000000 (recomp/layout.py FIRST_BASE: main comes first) and
// room for every module recomp/layout.py places (ORDER: main, subsdk0-9, sdk). The real bases and sizes come with the
// game code from the PC's export (its modules.c: rt_layout, rt_linked_modules), as on PC.
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_COUNT 12
