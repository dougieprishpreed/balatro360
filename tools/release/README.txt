Balatro360 {VERSION}
===================

An unofficial port of Balatro to the Xbox 360. It is a runtime for the game's
own files: you need your own copy of Balatro for PC (Steam), and the converter
below copies its files over. No game files are included here.

Balatro360 isn't affiliated with or endorsed by LocalThunk, Playstack or
Microsoft. Source code: https://github.com/dougieprishpreed/balatro360


What you need
-------------
* An Xbox 360 that runs unsigned code (RGH or JTAG) and a way to launch
  .xex files on it (Aurora, Freestyle Dash, XeXMenu, ...).
* Balatro for Windows (Steam). The Mac version's Balatro.love works too.
* A USB drive (FAT32) or the console's hard disk, with about 100 MB free.


Install
-------
1. Extract this whole zip file to a folder on your PC.

2. Double-click "Convert Balatro.bat" and choose Balatro.exe in the window that
   opens. To find it in Steam: right-click Balatro, Manage, Browse local files.
   (You can also drop Balatro.exe onto "Convert Balatro.bat".)
   The game's files are copied into the Balatro360\game folder.

3. Copy the whole Balatro360 folder to your USB drive or the console's hard
   disk, for example to Games\Balatro360.

4. On the console, launch default.xex from that folder.

The first start takes longer while the console prepares the game's shaders;
later starts reuse them (the cache folder).

Without the converter (Mac, Linux, or by hand)
* Python 3: python3 converter/prepare_game.py /path/to/Balatro.exe Balatro360
* Or open Balatro.exe with 7-Zip and extract everything except the .dll files
  into Balatro360\game, so that Balatro360\game\main.lua exists.

After a Balatro update, run the converter again: it replaces the game folder.
Then delete Balatro360\cache on the console so the shaders are rebuilt.


Playing
-------
* Balatro's own controller support is used: A select, B back, and so on.
* Hold BACK + START for a second to quit to the dashboard.
* Saves are in Balatro360\save. They use the PC version's format, so you can
  copy PC saves (the 1, 2, 3 profile folders) there and back.
* Balatro runs at 60 frames a second in menus and drops to a steady 30 in
  busy scenes and most of a run.


Settings (optional)
-------------------
Create Balatro360\game\b360_config.lua with a text editor, for example:

    return {
        B360_SAFE_AREA = 0.92,   -- shrink the picture if your TV cuts off the edges
        F_RUMBLE = 1.0,          -- controller rumble strength, 0 to turn it off
    }

More options are listed in the project's README on GitHub.


Problems
--------
Balatro360 writes b360.log next to default.xex. If something goes wrong,
please open an issue on GitHub and attach b360.log (and default.log).


Licences
--------
Balatro360 is open source; the libraries it uses and their licences are
listed in Balatro360\THIRD_PARTY.md and Balatro360\licenses.
Balatro and all of its assets belong to their respective owners.
