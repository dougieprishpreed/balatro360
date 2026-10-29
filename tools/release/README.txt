Balatro360 {VERSION}
===================
https://github.com/dougieprishpreed/balatro360

An unofficial port of Balatro to the Xbox 360. It runs the game from your own
copy of Balatro for PC (Steam); no game files are included here.

Balatro360 isn't affiliated with or endorsed by LocalThunk, Playstack or
Microsoft. Report its problems at the address above.

You need an Xbox 360 that runs unsigned code (RGH, JTAG or ABadAvatar).


Install
-------
1. Copy the apps folder in this zip to the root of the console's hard drive,
   the drive FTP shows as Hdd1. If an apps folder is already there, copy
   balatro360 into it instead. A USB drive works too: copy the balatro360
   folder anywhere on it.

2. Copy Balatro.exe from your Balatro installation into apps\balatro360\,
   beside default.xex. To find it, right-click Balatro in Steam and choose
   Manage > Browse local files. Balatro.love from the Mac version (inside
   Balatro.app) works too.

3. Start default.xex from your dashboard or the Homebrew App Store.

The first start takes longer: Balatro360 takes the game's files out of
Balatro.exe, deletes Balatro.exe, and prepares the game's shaders. Later
starts are quicker.

The Homebrew App Store installs Balatro360 in the same folder,
hdd:\apps\balatro360\, so with the store, only step 2 is needed.

After a Balatro update, copy the new Balatro.exe beside default.xex and start
Balatro360 again.


Playing
-------
* Balatro's own controller support is used: A select, B back, and so on.
* Hold BACK + START for a second to quit to the dashboard.
* Saves are in apps\balatro360\save. They use the PC version's format, so
  you can copy your PC profiles (the 1, 2 and 3 folders in %AppData%\Balatro)
  there and back.
* Balatro runs at 60 frames a second in menus and drops to a steady 30 in
  busy scenes and most of a run.


Settings (optional)
-------------------
Create apps\balatro360\game\b360_config.lua with a text editor, for example:

    return {
        B360_SAFE_AREA = 0.92,   -- shrink the picture if your TV cuts off the edges
        F_RUMBLE = 1.0,          -- controller rumble strength, 0 to turn it off
        B360_FPS = 30,           -- always 30 fps, instead of switching between 60 and 30
    }


Problems
--------
Balatro360 writes b360.log next to default.xex. If something goes wrong,
please open an issue on GitHub and attach b360.log and default.log.


Licences
--------
Balatro360 is open source under the MIT licence (LICENSE.txt). The libraries
it uses and their licences are listed in THIRD_PARTY.md and licenses\.
Balatro and all of its assets belong to their respective owners.
