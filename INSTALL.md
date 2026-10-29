# Terraria 3D mod v0.5.3: install guide

Terraria 3D is a Luma3DS plugin (a `.3gx` file) that adds real stereoscopic 3D to
Terraria on the Nintendo 3DS. This guide takes about five minutes.

The mod contains no game files. You need your own legally owned copy of Terraria.

## What you get

- Real per-layer stereo 3D: sky far back, walls behind tiles, particles and HUD in front.
- Depth presets, default "Terrarium Window", plus Diorama, Pop-out, Tight walls,
  Cinematic, Caves, Builder and Combat.
- 2x zoom (hotkey L + R).
- C-stick aiming for weapons, tools, the grapple and blocks.
- A cheat console (L + R + Y) and a Creative mode menu: Godmode, Fullbright,
  Instant mining, Long reach, Flight (experimental) and more.
- "Colour booster (fake OLED)": Normal, OLED (deep blacks) or OLED vivid.

## Requirements

- A New 3DS / New 2DS XL running custom firmware with **Luma3DS v13.0 or newer**.
  The plugin loader is built into Luma3DS, so nothing else is needed.
  - New 3DS only. Old 3DS is not supported.
- **Terraria, USA version** (title ID `000400000016A900`), updated to **v5.6.0**
  (the latest update).

Other regions or versions are refused by the plugin's version check. It shows a red
notice and applies no patches, so nothing is broken, but nothing works either.

## Install

1. Copy `Terraria3D.3gx` to this exact path on your SD card, creating any missing
   folders:

   ```
   SD:/luma/plugins/000400000016A900/Terraria3D.3gx
   ```

2. Enable the plugin loader. Open the Rosalina menu with
   **L + D-pad Down + SELECT**, choose "Plugin loader: [Disabled]" and press **A**
   so it reads Enabled. Luma remembers this setting.
3. Launch Terraria. A green "Terraria 3D mod loaded" notice appears.
4. Press **SELECT** to open the mod menu.

## Controls

- **ZR**: use / mine / place at the C-stick aim point
- **ZL**: grappling hook
- **Y**: stock behaviour (smart-cursor stock/break when the C-stick is centred)
- **C-stick**: aim; with a pickaxe, axe or hammer, pushing it auto-tunnels in that direction
- **Y + ZR**: toggle the C-stick style (Spring / Cursor)
- **Inventory**: tap to move items (tap a stack to pick it up, tap a slot to place, swap or merge)
- **L + R**: 2x zoom; **SELECT**: mod menu; **L + R + Y**: cheat console

## Using the menu

- **Stereo 3D**: enable/disable, per-layer depth, Depth preset, Depth strength,
  Pop-out effects, Invert depth, Diagnostics.
- **2x zoom hotkey**: L + R by default.
- **Controls**: C-stick mode, C-stick style (Spring or Cursor; in play, hold Y and press ZR to switch), Cursor speed, Circle pad: interact only, C-stick debug.
- **Colour booster (fake OLED)**: Normal / OLED / OLED vivid. It uses the display
  hardware, so it costs no FPS.
- **Cheats**: cheat console hotkey (L + R + Y) or open it from the menu.
- **Creative mode**: toggles for godmode, no spawns, fullbright, infinite items, and more.
- **Performance**: FPS display, 3D slider value, and the performance log.

## Updating

Replace `Terraria3D.3gx` with the new file. Nothing else to do.

## Uninstalling

Delete `Terraria3D.3gx`, or turn off "Plugin loader" in the Rosalina menu.

## Using a console from another region

Luma3DS lets out-of-region games run, and you can install your own USA dump on, for
example, a Japanese console. For English text and correct behaviour:

1. Hold **SELECT** while booting to open the Luma config, and enable
   **"Enable game patching"**.
2. Create a text file at `SD:/luma/titles/000400000016A900/locale.txt` containing:

   ```
   USA EN
   ```

Do **not** region-change the console's firmware. It is not needed and it is risky.

## Troubleshooting

- **No green notice at startup**: the plugin loader is off, or the file is in the
  wrong folder. Re-check the path above, including the title ID folder.
- **Red "unsupported version" notice**: update Terraria to v5.6.0 (USA version).
- **Eye strain or double vision**: lower the 3D slider, or choose the **Builder**
  depth preset (shallow and precise).
- **Crash**: please send us `SD:/luma/plugins/000400000016A900/t3d_log.txt` and any
  `SD:/luma/dumps/arm11/*.dmp` files, along with what you were doing.

## Log file

The plugin writes a performance log, one line per second, to
`SD:/luma/plugins/000400000016A900/t3d_log.txt`. Turn it off in the mod menu under
Performance ("Log performance to SD").

## Credits

- [CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework)
- [Luma3DS](https://github.com/LumaTeam/Luma3DS)
- [devkitPro](https://devkitpro.org/)

## Legal

Terraria 3D is a fan mod. It is not affiliated with Re-Logic, 505 Games or Nintendo.
Terraria is a trademark of its respective owners. No game files are included or
distributed; you must own the game.
