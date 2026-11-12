TSword PS5 export
=================

This folder turns your UltraTears game folder on your PC into the TSword folder the PS5 plays from. You need:

  - UltraTears set up on your PC with its tools\setup.py (your own Tears of the Kingdom 1.4.3 dump): the folder that
    has UltraTears.exe, data\ (exefs, romfs, shaders, textures, fonts) and deps\. The PS5 title was built from the
    UltraTears commit in SOURCE: a game folder set up with the same UltraTears is the safest.
  - a USB drive formatted exFAT (or the PS5's extended storage), about 30 GB free.

1. Double-click "Export for PS5.bat". It asks for your UltraTears folder and where to write (e.g. E:\TSword on the
   USB drive). It translates the game's code again for the PS5's addresses (data\generated-ps5 in your UltraTears
   folder, 10-30 minutes the first time), compiles it into tsgame.so (with UltraTears' own clang), and copies the game
   data beside it. Your UltraTears install is not changed apart from data\generated-ps5 and build-ps5\ being added.
2. Plug the drive into the PS5 and start TSword. It finds TSword\ on the drive by itself (or put the folder's path
   on the first line of game-files.txt in the title folder).

The game code (tsgame.so) belongs to this TSword release: after updating TSword on the PS5, run the export of the new
release again (the data is not copied twice). Settings are in ultratears.cfg in the title folder; the log is
ultratears.log there.
