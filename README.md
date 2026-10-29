# Balatro360

Play [Balatro](https://www.playbalatro.com/) on your Xbox 360.

Balatro360 is an unofficial, fan-made port for consoles that can run homebrew.
**The game isn't included:** you need your own copy of Balatro for PC, and the
converter in the download copies your game's files across for you.

## What you need

* An Xbox 360 that can run homebrew (RGH, JTAG or ABadAvatar), with a way to
  launch `.xex` files, such as Aurora, Freestyle Dash or XeXMenu
* Balatro for Windows from Steam
* A Windows 10 or 11 PC (on a Mac or Linux, see [below](#on-a-mac-or-linux))
* A USB drive or the console's hard drive, with about 100 MB free

## Install

1. Download the latest `Balatro360-v*.zip` from
   [Releases](https://github.com/dougieprishpreed/balatro360/releases) and
   extract it.
2. Double-click **Convert Balatro.bat** and choose `Balatro.exe`.
   To find it, right-click Balatro in Steam and choose
   **Manage > Browse local files**.
3. Copy the **Balatro360** folder to your USB drive or the console's hard
   drive, for example to `Games\Balatro360`.
4. On the console, launch `default.xex` from that folder.

The first launch takes a little longer while the console prepares the game's
graphics. Later launches are quicker.

### On a Mac or Linux

The zip's `converter` folder also has a Python 3 script that does the same
job:

```
python3 converter/prepare_game.py /path/to/Balatro.exe Balatro360
```

Or open `Balatro.exe` with 7-Zip and extract everything except the `.dll`
files into `Balatro360/game`, so that `Balatro360/game/main.lua` exists.

## Playing

* The game uses Balatro's own controller support: **A** to select, **B** to go
  back, and so on.
* Hold **BACK + START** for a second to quit to the dashboard.
* Menus run at 60 fps. Busy scenes and most of a run play at a steady 30 fps.
* Seeded runs play out the same as on PC.

## Saves

Your saves are in `Balatro360\save` on the console. They use the same format
as the PC version, so you can copy your profiles (the `1`, `2` and `3`
folders) between the two. On Windows, the PC saves are in `%AppData%\Balatro`.

## Updating

* **New Balatro360 version:** copy the new `Balatro360` folder over the old
  one on the console. Your game files and saves stay where they are.
* **New Balatro version:** run the converter again, replace the `game` folder
  on the console with the new one, and delete the `cache` folder.

## Settings

Most settings are in Balatro's own options menu. For a few extras, create a
text file called `b360_config.lua` in `Balatro360\game` on the console:

```lua
return {
    B360_SAFE_AREA = 0.92,  -- shrink the picture if your TV cuts off the edges
    F_RUMBLE = 1.0,         -- controller rumble strength; 0 turns it off
    B360_FPS = 30,          -- always 30 fps, instead of switching between 60 and 30
}
```

Keep only the lines you want.

## Problems?

* **The first launch is slow.** That's normal: the console is preparing the
  graphics, and it only does this once.
* **The edges of the picture are cut off.** Set `B360_SAFE_AREA` (see
  [Settings](#settings)).
* **It doesn't run in Xenia.** Balatro360 only runs on a real console.
* **Anything else:** please
  [open an issue](https://github.com/dougieprishpreed/balatro360/issues) and
  attach `b360.log` and `default.log` from the `Balatro360` folder.

## Credits

Balatro is made by LocalThunk and published by Playstack. Balatro360 isn't
affiliated with or endorsed by LocalThunk, Playstack or Microsoft, and this
repository contains none of the game's files.

Balatro360 is built with the open [xSDK](https://github.com/xenosworks/xsdk)
toolchain and [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon).
The other open-source code it uses is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

**Developers:** building, how the port works and all the testing options are
in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
