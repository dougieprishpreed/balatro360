# Balatro360

Play [Balatro](https://www.playbalatro.com/) on your Xbox 360.

Balatro360 is an unofficial, fan-made port.
**The game isn't included:** you need your own copy of Balatro for PC.

Balatro360 is built with the open [xSDK](https://github.com/xenosworks/xsdk)
toolchain and [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon).

## What you need

* An Xbox 360 that can run homebrew (RGH or ABadAvatar)
* Balatro for PC from Steam (Windows, or Mac)
* A way to copy files to the console: FTP, or a USB drive
* About 150 MB free on the console's hard drive or the USB drive

## Install

1. Download the latest `Balatro360-Xbox360-*.zip` from
   [Releases](https://github.com/dougieprishpreed/balatro360/releases) and
   extract it.
2. Copy its `apps` folder to the root of the console's hard drive (`Hdd1`
   over FTP). If there's an `apps` folder there already, copy `balatro360`
   into it instead. A USB drive works too: copy the `balatro360` folder
   anywhere on it.
3. Copy `Balatro.exe` into that `balatro360` folder, beside `default.xex`.
   To find it, right-click Balatro in Steam and choose
   **Manage > Browse local files**. On a Mac, `Balatro.love` from inside
   `Balatro.app` works instead.
4. On the console, start `default.xex`.

The first start takes longer: Balatro360 takes the game's files out of
`Balatro.exe`, deletes it, and prepares the game's graphics. Later starts are
quicker.

## Playing

* The game uses Balatro's own controller support: **A** to select, **B** to go
  back, and so on.
* Hold **BACK + START** for a second to quit to the dashboard.
* Menus run at 60 fps. Busy scenes and most of a run play at a steady 30 fps.
* Seeded runs play out the same as on PC.

## Saves

Your saves are in `balatro360\save` on the console. They use the same format
as the PC version, so you can copy your profiles (the `1`, `2` and `3`
folders) between the two. On Windows, the PC saves are in `%AppData%\Balatro`.

## Updating

* **New Balatro360 version:** copy the new `balatro360` folder over the old
  one on the console. Your game files and saves stay where they are.
* **New Balatro version:** copy the new `Balatro.exe` beside `default.xex`
  and start Balatro360 again.

## Settings

Most settings are in Balatro's own options menu. For a few extras, create a
text file called `b360_config.lua` in `balatro360\game` on the console:

```lua
return {
    B360_SAFE_AREA = 0.92,  -- shrink the picture if your TV cuts off the edges
    F_RUMBLE = 1.0,         -- controller rumble strength; 0 turns it off
    B360_FPS = 30,          -- always 30 fps, instead of switching between 60 and 30
}
```

Keep only the lines you want.

## Problems?

* **"Balatro360 needs your own copy of Balatro."** `Balatro.exe` isn't beside
  `default.xex`, or it isn't Balatro's. Copy it there and start again.
* **The first start is slow.** That's normal: the console is preparing the
  game, and it only does this once.
* **The edges of the picture are cut off.** Set `B360_SAFE_AREA` (see
  [Settings](#settings)).
* **It doesn't run in Xenia.** Balatro360 only runs on a real console.
* **Anything else:** please
  [open an issue](https://github.com/dougieprishpreed/balatro360/issues) and
  attach `b360.log` and `default.log` from the `balatro360` folder.

## Credits

Balatro is made by LocalThunk and published by Playstack. Balatro360 isn't
affiliated with or endorsed by LocalThunk, Playstack or Microsoft, and this
repository contains none of the game's files.

Balatro360 is built with the open [xSDK](https://github.com/xenosworks/xsdk)
toolchain and [vulkan4xenon](https://github.com/xenosworks/vulkan4xenon).
Its own code is under the [MIT licence](LICENSE); the other open-source code
it uses is listed in [THIRD_PARTY.md](THIRD_PARTY.md).

**Developers:** building, how the port works and all the testing options are
in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
