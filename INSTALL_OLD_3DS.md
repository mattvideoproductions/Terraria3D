# Terraria 3D mod v0.7.0 for Old 3DS: install guide

This is the **Old 3DS** build of Terraria 3D. It is for the Old 3DS, Old 3DS XL and 2DS.
On a New 3DS or New 2DS XL, use the [regular build](README.md#which-file-do-i-need) instead.

Terraria 3D is a Luma3DS plugin (a `.3gx` file). It contains no game files; you need
your own copy of Terraria.

## What's new in v0.7.0

**Real layered 3D on Old 3DS.** Before, the whole picture sat at one depth (flat 3D). Now
the right eye is drawn a second time, with each layer moved by its own amount. The sky and
mountains are far back, the cave backdrop and walls sit behind you, the ground, you and
the NPCs are just behind the screen, and the HUD is on the screen ("Lite" preset, 4 planes).

It costs **no extra game memory**. The second picture uses a smaller 240x400 buffer that
fits in the video memory v0.6.1 already set aside. The Old 3DS has to draw the
top screen twice, but on a real Old 3DS the frame-rate cost was barely noticeable. If a
busy scene ever feels slow, turn off **Stereo 3D > Per-layer depth** to get the cheaper
flat 3D back.
If the console's video memory ever has no room for the second picture, the plugin falls
back to flat 3D by itself. The menu then shows "Per-layer depth (unavailable: no memory)".

### From v0.6.1 (still true)
v0.6.0 crashed about 13 seconds after boot because Luma's plugin memory counts against the
game's own limit on Old 3DS. v0.6.1 and later use the smallest plugin memory (2 MiB) and
move 1 MiB inside the game's memory to where the 3D buffers push a texture.

## What's different from the New 3DS build

- **30 fps lock** (Performance menu, default "Auto"). The frame rate is an even 30 while
  3D is on or while you're underground. On the surface in 2D it is uncapped, as in the
  stock game. You can also choose "Always 30" or "Off". The game speed doesn't change.
- **Per-layer depth** is on by default, with the "Lite" depth preset (4 planes). Other
  presets work too. Turning it off gives flat 3D: one depth, almost no cost.
- **Controls** are the game's own: D-pad cursor and touch, plus tap-to-move items. There
  is no C-stick, ZL/ZR or Circle Pad Pro support in this build.
- All the speed-ups from the New 3DS build are on: cave backdrop culling, off-screen skip,
  strip merging and fast backdrop lighting.
- CTRPF's built-in cheat **search** tool is turned off, because it needs memory this build
  doesn't have. The mod's own cheats and creative mode work.

## Requirements

- An Old 3DS / Old 3DS XL / 2DS with custom firmware: **Luma3DS v13.0 or newer**.
- **Terraria, USA version** (title ID `000400000016A900`), updated to **v5.6.0**.

## Install

1. Download `Terraria3D-Old3DS-v0.7.0.zip` from the
   [Old 3DS release](https://github.com/mattvideoproductions/Terraria3D/releases/tag/o3ds-v0.7.0)
   and extract it to the root of your SD card. That puts `Terraria3D.3gx` at this path
   (replace the file if one is already there):

   ```
   SD:/luma/plugins/000400000016A900/Terraria3D.3gx
   ```

   Keep **only one** `.3gx` file in that folder. Luma loads one plugin per game.
2. Turn the plugin loader on: open the Rosalina menu with **L + D-pad Down + SELECT**,
   choose "Plugin loader" and set it to Enabled.
3. Start Terraria. A green "Terraria 3D mod v0.7.0 (Old 3DS) loaded" notice appears.
4. Press **SELECT** for the mod menu.

Don't put CTRPF menu backgrounds (`TopBackground.bmp`, `BottomBackground.bmp`) or menu
sound files in the plugin folder. This build has only about 0.5 MiB of plugin memory for
them.

## Playing without 3D (more memory for the game)

If you never use 3D, or the game still runs out of memory, create an empty file named

```
SD:/luma/plugins/000400000016A900/t3d_no3d.txt
```

With that file there, the plugin reserves no 3D memory and leaves the game's memory as
the stock game has it. Everything else still works. Delete the file to get 3D back. On a
2DS this happens automatically.

## Region-free consoles

A console from another region (for example a Japanese Old 3DS running the USA game)
works with Luma's game patching:

1. Hold SELECT at boot and enable **"Enable game patching"**.
2. Put a file `SD:/luma/titles/000400000016A900/locale.txt` containing `USA EN`.

Don't region-change the console itself.

## Log file

The plugin writes `SD:/luma/plugins/000400000016A900/t3d_log.txt`. Lines go to the card
straight away for the first 60 seconds after boot, then every few seconds. If the game
crashes, the plugin writes a last "PANIC" line with the game's memory state. Above 2 MiB
the file moves to `t3d_log_old.txt`. Both builds use the same file name.

## Uninstall

Delete `Terraria3D.3gx`, or turn the plugin loader off in Rosalina.

## Troubleshooting

- **No green notice**: the plugin loader is off, or the file is in the wrong folder.
- **Red "unsupported version" notice**: update Terraria to v5.6.0 (USA).
- **Luma says plugin memory couldn't be allocated / reboot needed**: reboot the console
  and start Terraria first, before other apps.
- **Crash**: send `t3d_log.txt`, `t3d_log_old.txt` and any `SD:/luma/dumps/arm11/*.dmp`.
  Then try again with `t3d_no3d.txt` (see above).

## Legal

Terraria 3D is a fan mod. It is not affiliated with Re-Logic, 505 Games or Nintendo. No
game files are included.
